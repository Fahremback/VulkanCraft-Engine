#include "EditorApplication.hpp"
// Frontend port from the Wicked Engine Editor (MIT, commit 2aa9fdf…): Font
// Awesome 6 icon font + codepoint macros, and the Liberation Sans UI font
// (zstd-compressed, same font Wicked ships). See frontend/PORTS.md.
#include "frontend/FontAwesomeV6.h"
#include "frontend/IconsFontAwesome6.h"
#include "frontend/liberation_sans.h"
#include "frontend/ForgeTheme.hpp"
#include "frontend/ForgeWidgets.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include "../engine/physics/VoxelBoxMerger.hpp"
#include "../engine/animation/AnimationAssets.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "../engine/audio/OggDecoder.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include <array>
#include <map>
#include <random>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>
#include <chrono>
#include <ctime>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>

// Local copies of the file-scope render helpers (the originals live in a
// nested anonymous namespace inside `namespace Engine`, later in this TU, so
// they are not visible to the runtime-wired Wicked-port code below). These
// global-scope versions share the same implementations.
namespace {
glm::mat4 model_from_transform(const Engine::TransformComponent& t) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!finite(t.position.x) || !finite(t.position.y) || !finite(t.position.z) ||
        !finite(t.rotation.x) || !finite(t.rotation.y) || !finite(t.rotation.z) ||
        !finite(t.scale.x) || !finite(t.scale.y) || !finite(t.scale.z)) {
        // NaN/inf guard: a non-finite transform would poison the MVP matrix and
        // black out the viewport. Draw this entity at the origin instead.
        return glm::mat4(1.0f);
    }
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}
// Current fog state (set per-frame from WeatherComponent)
static glm::vec4 g_fogParams{ 0.001f, 100.0f, 0.0f, 0.0f };
static glm::vec4 g_fogColor{ 0.5f, 0.6f, 0.7f, 1.0f };
void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp,
                    const glm::vec4& color, const glm::mat4& model = glm::mat4(1.0f)) {
    const Engine::ScenePushConstants pc{ mvp, color, g_fogParams, g_fogColor, model };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}
void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color,
                       const glm::mat4& model = glm::mat4(1.0f)) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color, model);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}
void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color, const glm::mat4& model = glm::mat4(1.0f)) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color, model);
}

} // namespace

#include <imgui_impl_vulkan.h>
#include <VkBootstrap.h>
#include <miniaudio.h>
#include <thread>

// ---------------------------------------------------------------------------
// Safe Vulkan helpers (avoid null-pointer dereferences on mapping failure)
// ---------------------------------------------------------------------------
static bool safe_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                             VkDeviceSize size, VkFlags flags, void** ppData) {
    *ppData = nullptr;
    const VkResult result = vkMapMemory(device, memory, offset, size, flags, ppData);
    if (result != VK_SUCCESS) {
        std::cerr << "[Vulkan] vkMapMemory failed (" << result << ")" << std::endl;
        return false;
    }
    return true;
}
static bool safe_map_and_copy(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                              VkDeviceSize size, const void* source) {
    void* data = nullptr;
    if (!safe_vkMapMemory(device, memory, offset, size, 0, &data)) return false;
    std::memcpy(data, source, static_cast<size_t>(size));
    vkUnmapMemory(device, memory);
    return true;
}

// PlayNavAgent — the public provider's path follower (mirrors the legacy
// NavigationAgent stepping so the Fase 8 behavior is preserved).
void PlayNavAgent::set_path(std::vector<glm::vec3> points) {
    path = std::move(points);
    waypoint = 0;
    reached = path.empty();
}

void PlayNavAgent::update(float deltaTime) {
    if (reached) return;
    if (waypoint >= path.size()) {
        reached = true;
        return;
    }
    float remaining = speed * deltaTime;
    while (remaining > 0.0f && waypoint < path.size()) {
        const glm::vec3 target = path[waypoint];
        const glm::vec3 dir = target - position;
        const float dist = glm::length(dir);
        if (dist <= stoppingDistance) {
            position = target;
            ++waypoint;
            continue;
        }
        const float step = std::min(remaining, dist);
        position += (dir / dist) * step;
        remaining -= step;
        if (step >= dist - 1e-4f) {
            position = target;
            ++waypoint;
        }
    }
    if (waypoint >= path.size()) reached = true;
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wincodec.h>
#include <propsys.h> // IPropertyBag2 (WIC frame encode)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Engine {

EditorApplication::EditorApplication() {
    // Loopback HTTP control API: drive the editor from a terminal or an agent
    // via curl http://127.0.0.1:8321/{play,pause,resume,stop,step,state}.
    m_controlApi.start(8321);

    // Panel plugin registry: the shell derives menus/palette/layout from this
    // (ezEngine "tools as replaceable plugins" pillar). Populated here so the
    // control API can expose it from the very first frame.
    register_editor_panels();
    register_project_templates();
    m_uiDocJson = build_ui_doc_json();
    apply_layout_defaults();
    build_message_catalog();
    build_shortcut_doc();
    build_command_index();
    refresh_play_mode();
    m_frameProfiler = engine::profiling::create_frame_profiler(600, 33.3);
    refresh_profiler();
    refresh_undo();
    build_content_browser();
    m_windowMode = engine::editor::create_window_mode();
    refresh_window_mode();
    {
        engine::editor::EditorCameraState camState;
        camState.yaw = m_editorCamera.yaw;
        camState.pitch = m_editorCamera.pitch;
        camState.distance = m_editorCamera.orbitDistance;
        camState.target = engine::editor::CamVec3(
            m_editorCamera.orbitTarget.x, m_editorCamera.orbitTarget.y,
            m_editorCamera.orbitTarget.z);
        camState.fov = m_editorCamera.fov;
        camState.near_plane = m_editorCamera.nearPlane;
        camState.far_plane = m_editorCamera.farPlane;
        m_cameraContract = engine::editor::create_editor_camera(camState);
    }
    m_gizmoContract = engine::editor::create_gizmo_controller();
    m_publishPipeline = engine::editor::create_publish_pipeline();
    m_inspectorDoc = engine::editor::create_inspector_doc();
    m_sceneHierarchy = engine::editor::create_scene_hierarchy();
    m_onboardingTour = engine::editor::create_onboarding_tour();
    refresh_camera();
    refresh_gizmo();
    refresh_publish();
    refresh_inspector();
    refresh_hierarchy();
    refresh_onboarding();

    // Playback sink for the play-in-editor mixer (audio previews + play-mode
    // audio components). Before this the Mixer rendered into a buffer that was
    // never sent to a device, so nothing produced sound.
    init_audio_output();

    const std::filesystem::path registryPath =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (std::filesystem::exists(registryPath) && !m_assetRegistry.load(registryPath)) {
        std::cerr << "[AssetRegistry] Ignoring invalid database: " << registryPath << std::endl;
    }
    m_assetPipeline = std::make_unique<AssetPipeline>(m_assetRegistry);
    m_assetPipeline->add_importer(std::make_unique<TextureImporter>());
    m_assetPipeline->add_importer(std::make_unique<BinaryCopyImporter>(
        AssetType::Texture, std::vector<std::string>{".jpg", ".jpeg", ".exr"}, ".texturebin"));
    m_assetPipeline->add_importer(std::make_unique<MeshImporter>());
    m_assetPipeline->add_importer(std::make_unique<SkeletonImporter>());
    m_assetPipeline->add_importer(std::make_unique<AnimationClipImporter>());
    m_assetPipeline->add_importer(std::make_unique<BinaryCopyImporter>(
        AssetType::Unknown, std::vector<std::string>{".bin"}, ".blobbin"));
    m_assetPipeline->add_importer(std::make_unique<AudioImporter>());
    m_assetPipeline->add_importer(std::make_unique<BinaryCopyImporter>(
        AssetType::Audio, std::vector<std::string>{".ogg"}, ".audiobin"));
    m_assetPipeline->add_importer(std::make_unique<TextMaterialImporter>());
    m_assetHotReload = std::make_unique<AssetHotReloadService>(
        *m_assetPipeline, m_assetRegistry,
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache");
}

EditorApplication::~EditorApplication() {
    // Stop the loopback control API first so its thread never outlives the
    // main loop / teardown (it only talks to 127.0.0.1).
    m_controlApi.stop();
    cleanup();
}

void EditorApplication::register_project_templates() {
    // Built-in project templates for the wizard (ezEngine pillar). The wizard
    // lists these; create_project_from_template materializes the scaffold.
    Engine::Editor::register_builtin_templates(m_templateRegistry);
}

void EditorApplication::apply_layout_defaults() {
    // Safe reset: the shell layout is derived from the panel registry defaults
    // (a fresh layout never depends on prior state). The real windows consume
    // this model; GET /layout exposes it for observability.
    m_layoutModel.reset_from_registry(m_panelRegistry.panels());
}

void EditorApplication::build_message_catalog() {
    // Curated user-facing messages (plano agente 2 §C): the shell's status
    // bar/toasts/dialogs consume these ids instead of ad-hoc strings. Built
    // once and exposed via GET /messages.
    using engine::editor::CatalogMessage;
    using engine::editor::MessageCatalogDoc;
    MessageCatalogDoc doc;
    const auto add = [&](const char* id, engine::editor::MessageSeverity sev,
                         const char* text, const char* action = "") {
        CatalogMessage m;
        m.id = id;
        m.severity = sev;
        m.text = text;
        m.action = action;
        doc.messages.push_back(m);
    };
    add("editor.startup", engine::editor::MessageSeverity::Info,
        "VulkanCraft Editor ready — {0}", "");
    add("editor.save.ok", engine::editor::MessageSeverity::Info,
        "Saved {0}", "");
    add("editor.save.failed", engine::editor::MessageSeverity::Error,
        "Could not save {0}: {1}", "asset:open_save_settings");
    add("editor.import.failed", engine::editor::MessageSeverity::Error,
        "Failed to import {0}: {1}", "asset:open_import_settings");
    add("editor.command.unknown", engine::editor::MessageSeverity::Warning,
        "Unknown command {0}", "palette:open");
    add("editor.scene.unsaved", engine::editor::MessageSeverity::Warning,
        "Scene has unsaved changes", "editor:save");
    add("editor.gpu.memory", engine::editor::MessageSeverity::Warning,
        "GPU memory at {0}%", "dev:open_profiler");
    m_messageCatalogJson = doc.to_json();
}

void EditorApplication::build_shortcut_doc() {
    // The shell's CURRENT shortcuts, rendered as markdown from the IActionMap
    // contract (plano agente 2 §C). Today the shell wires these directly; the
    // doc is the single data-driven reference for the Help menu / README, and
    // reflects any future IActionMap-backed rebinding.
    using engine::editor::ShortcutDocSpec;
    using engine::editor::ShortcutEntry;
    using engine::input::ActionBinding;
    using engine::input::ActionMapSpec;
    using engine::input::InputBinding;
    using engine::input::InputSource;

    ShortcutDocSpec spec;
    spec.title = "VulkanCraft Editor Shortcuts";
    const auto entry = [&](const char* action, const char* label,
                           const char* desc) {
        ShortcutEntry e;
        e.action = action;
        e.label = label;
        e.description = desc;
        spec.entries.push_back(e);
    };
    entry("gizmo.select", "Select", "Select entities in the viewport.");
    entry("gizmo.move", "Move Gizmo", "Translate the selection.");
    entry("gizmo.rotate", "Rotate Gizmo", "Rotate the selection.");
    entry("gizmo.scale", "Scale Gizmo", "Scale the selection.");
    entry("palette", "Command Palette", "Open the global command palette.");
    entry("play.toggle", "Play / Stop", "Enter or leave Play mode.");

    ActionMapSpec map;
    const auto action = [&](const char* name, const char* input) {
        ActionBinding ab;
        ab.action = name;
        InputBinding b;
        b.source = InputSource::Keyboard;
        b.input = input;
        ab.bindings.push_back(b);
        map.actions.push_back(ab);
    };
    action("gizmo.select", "Q");
    action("gizmo.move", "W");
    action("gizmo.rotate", "E");
    action("gizmo.scale", "R");
    action("palette", "Ctrl+K");
    action("play.toggle", "Space");

    std::string err;
    auto doc = engine::editor::create_shortcut_doc(spec, err);
    if (doc != nullptr) {
        m_shortcutDocMarkdown = doc->document(map, err);
    } else {
        m_shortcutDocMarkdown = std::string();
    }
}

void EditorApplication::build_command_index() {
    // The palette's data-driven command catalog (plano agente 2 §B): the
    // shell's Ctrl+K global search consumes this index (id/label/category/
    // keywords/action) — exposed via GET /commands/search. Built once from
    // the REAL commands the shell offers.
    using engine::editor::CommandEntry;
    using engine::editor::CommandIndexDoc;
    CommandIndexDoc doc;
    const auto add = [&](const char* id, const char* label,
                         const char* category, const char* action,
                         const char* keywords) {
        CommandEntry e;
        e.id = id;
        e.label = label;
        e.category = category;
        e.action = action;
        std::string kw = keywords;
        std::size_t pos = 0;
        while ((pos = kw.find(',')) != std::string::npos) {
            e.keywords.push_back(kw.substr(0, pos));
            kw.erase(0, pos + 1);
        }
        if (!kw.empty()) e.keywords.push_back(kw);
        doc.entries.push_back(std::move(e));
    };
    add("scene.new", "New Scene", "Scene", "scene.new", "create");
    add("scene.open", "Open Scene", "Scene", "scene.open", "load");
    add("scene.save", "Save Scene", "Scene", "scene.save", "save,write");
    add("scene.save_as", "Save Scene As", "Scene", "scene.save_as", "save,export");
    add("build.game", "Build / Export Game", "Build", "build.game", "exe,package");
    add("asset.import", "Import Asset", "Asset", "asset.import", "import");
    add("asset.refresh", "Refresh Assets", "Asset", "asset.refresh", "reload");
    add("entity.cube", "Add Cube", "Entity", "entity.cube", "mesh");
    add("entity.empty", "Add Empty Object", "Entity", "entity.empty", "create");
    add("entity.light", "Add Directional Light", "Entity", "entity.light", "sun,light");
    add("play.toggle", "Play / Stop", "Play", "play.toggle", "play,run");
    add("palette", "Command Palette", "Tools", "palette.open", "search,ctrl+k");
    m_commandIndexJson = doc.to_json();
}

void EditorApplication::refresh_play_mode() {
    // The unambiguous play-state machine (plano agente 2 §B) serialized for
    // observability (GET /play-mode): state, runtime, paused, simulating.
    // The editor's PlayModeManager is the runtime owner (it clones the scene
    // on start_play); the PUBLIC IPlayMode contract is the spec — we mirror
    // the current state into it so the emitted JSON is exactly the contract's
    // deterministic format (PlayMode.cpp is linked into the editor).
    std::string err;
    auto pm = engine::editor::create_play_mode();
    switch (m_playMode.get_state()) {
        case PlayState::Edit: break;
        case PlayState::Play: pm->play(err); break;
        case PlayState::Simulate: pm->simulate(err); break;
        case PlayState::Pause:
            pm->play(err);
            pm->pause(err);
            break;
    }
    m_playModeJson = pm->to_json();
}

void EditorApplication::refresh_profiler() {
    // Deterministic frame/memory stats (plano agente 2 §B) serialized for
    // observability (GET /profiler): samples, min/max/avg, p95/p99, spikes,
    // fps, heap. The IFrameProfiler contract is fed every frame in the main
    // loop (FrameProfiler.cpp is linked into the editor).
    if (m_frameProfiler) {
        m_profilerJson = m_frameProfiler->to_json();
    } else {
        m_profilerJson = std::string();
    }
}

void EditorApplication::refresh_undo() {
    // The UndoSystem's generic undo/redo stack (plano agente 2 §B), exposed
    // via GET /undo: depths, can_undo/can_redo, top command. The editor's
    // UndoSystem delegates its stacks to the IUndoHistory contract (cap 256
    // evicts the oldest entry — no more unbounded growth).
    m_undoJson = m_undo.history()->to_json();
}

void EditorApplication::refresh_window_mode() {
    // The unambiguous window-mode model (plano agente 2 §B): Windowed /
    // Borderless / Fullscreen with only valid transitions and geometry
    // preservation — exposed via GET /window-mode. The GLFW window the shell
    // drives will drive this contract (WindowMode.cpp is linked in).
    if (m_windowMode) {
        m_windowModeJson = m_windowMode->to_json();
    } else {
        m_windowModeJson = std::string();
    }
}

void EditorApplication::refresh_camera() {
    // The orbit/pan/zoom/fly camera MODEL (plano agente 2 §B), which drives
    // the real m_editorCamera — exposed via GET /camera.
    if (m_cameraContract) {
        m_cameraJson = m_cameraContract->to_json();
    } else {
        m_cameraJson = std::string();
    }
}

void EditorApplication::refresh_publish() {
    // The build pipeline stage machine (plano agente 2 §C), driven by the
    // real build_game() flow — exposed via GET /publish.
    if (m_publishPipeline) {
        m_publishJson = m_publishPipeline->to_json();
    } else {
        m_publishJson = std::string();
    }
}

void EditorApplication::refresh_onboarding() {
    // The onboarding tour step machine (plano agente 2 §C) — a default first-
    // run tour over the real editor surfaces (panels + publish + scene). The
    // shell drives start/next/skip/complete/dismiss; this publishes the state.
    if (m_onboardingTour && m_onboardingTour->state() == engine::editor::TourState::Idle) {
        std::vector<engine::editor::TourStepDef> steps;
        engine::editor::TourStepDef s1;
        s1.id = "welcome";
        s1.title = "Welcome";
        s1.copy = "Open or create a project, then play the scene.";
        s1.target = "panels/ProjectLauncher";
        steps.push_back(s1);
        engine::editor::TourStepDef s2;
        s2.id = "scene";
        s2.title = "Scene";
        s2.copy = "Entities live in the Hierarchy; select one to inspect it.";
        s2.target = "panels/Hierarchy";
        steps.push_back(s2);
        engine::editor::TourStepDef s3;
        s3.id = "inspector";
        s3.title = "Inspector";
        s3.copy = "Components and properties appear here for the selection.";
        s3.target = "panels/Inspector";
        steps.push_back(s3);
        engine::editor::TourStepDef s4;
        s4.id = "publish";
        s4.title = "Build & Publish";
        s4.copy = "Package the game from the Build menu when ready.";
        s4.target = "command/publish";
        steps.push_back(s4);
        m_onboardingTour->start("first-run", steps);
    }
    if (m_onboardingTour) {
        m_onboardingJson = m_onboardingTour->to_json();
    } else {
        m_onboardingJson = std::string();
    }
}

void EditorApplication::refresh_hierarchy() {
    // The deterministic flat scene tree (plano agente 2 §B) built from the
    // REAL entities and parent links — exposed via GET /hierarchy.
    if (!m_sceneHierarchy) {
        m_hierarchyJson = std::string();
        return;
    }
    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    std::vector<engine::editor::HierarchyEntity> entities;
    std::vector<engine::editor::HierarchyLink> links;
    if (scene) {
        for (const auto& [id, ent] : scene->get_entities()) {
            entities.push_back({id.to_string(), ent.get_name()});
        }
        // ordem estável: id sort para determinismo entre publicações
        std::sort(entities.begin(), entities.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        for (const auto& [childId, comp] : scene->hierarchyComponents) {
            links.push_back({childId.to_string(), comp.parentID.to_string()});
        }
    }
    const std::vector<engine::editor::HierarchyRow> rows =
        m_sceneHierarchy->build(entities, links, m_hierarchySearch);
    m_hierarchyJson = m_sceneHierarchy->to_json(rows);
}

void EditorApplication::refresh_inspector() {
    // The semantic inspector model (plano agente 2 §C) built from the REAL
    // selected entity's components — exposed via GET /inspector.
    if (!m_inspectorDoc) {
        m_inspectorJson = std::string();
        return;
    }
    const bool has = m_selectedEntity.is_valid();
    std::string name;
    if (has) name = m_selectedEntity.get_name();
    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    const UUID id = m_selectedEntity.get_id();
    const auto contains = [&](const auto& map) { return map.contains(id); };
    const engine::editor::InspectorDoc doc = m_inspectorDoc->build(
        name, has,
        scene && contains(scene->transformComponents),
        scene && contains(scene->meshRendererComponents),
        scene && contains(scene->rigidbodyComponents),
        scene && contains(scene->destructionComponents),
        scene && contains(scene->weaponComponents),
        scene && contains(scene->vehicleComponents),
        scene && contains(scene->ragdollComponents),
        scene && contains(scene->animationComponents),
        scene && contains(scene->timelineComponents),
        scene && contains(scene->ikComponents),
        scene && contains(scene->retargetComponents),
        scene && contains(scene->missionComponents),
        scene && contains(scene->dialogueComponents),
        scene && contains(scene->navigationComponents));
    m_inspectorJson = m_inspectorDoc->to_json(doc);
}

void EditorApplication::refresh_gizmo() {
    // The gizmo math contract state (plano agente 2 §B): the live gizmo mode
    // and snap values the drag math uses — exposed via GET /gizmo.
    std::ostringstream out;
    out << "{\"mode\":\"";
    switch (m_gizmoMode) {
        case GizmoMode::Translate: out << "translate"; break;
        case GizmoMode::Rotate: out << "rotate"; break;
        case GizmoMode::Scale: out << "scale"; break;
        default: out << "select"; break;
    }
    out << "\",\"local\":" << (m_gizmoLocal ? "true" : "false")
        << ",\"snap_translate\":" << m_snapTranslate
        << ",\"snap_rotate\":" << m_snapRotate
        << ",\"snap_scale\":" << m_snapScale << "}";
    m_gizmoJson = out.str();
}

void EditorApplication::build_content_browser() {
    // The Content Browser navigation model (plano agente 2 §B), fed by the
    // REAL AssetRegistry snapshot — exposed via GET /content-browser. The
    // visual panel can consume this index instead of ad-hoc tree building.
    using engine::editor::ContentAsset;
    using engine::editor::ContentBrowserDoc;
    ContentBrowserDoc doc;
    for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
        ContentAsset asset;
        asset.id = meta.id.to_string();
        asset.name = meta.sourcePath.stem().string();
        if (asset.name.empty()) asset.name = "asset";
        switch (meta.type) {
            case AssetType::Texture: asset.type = "texture"; break;
            case AssetType::Mesh: asset.type = "model"; break;
            case AssetType::Material: asset.type = "material"; break;
            case AssetType::Audio: asset.type = "audio"; break;
            case AssetType::Skeleton: asset.type = "skeleton"; break;
            case AssetType::Animation: asset.type = "animation"; break;
            case AssetType::Scene: asset.type = "scene"; break;
            case AssetType::VoxelStructure: asset.type = "voxel"; break;
            case AssetType::Block: asset.type = "block"; break;
            default: asset.type = "other"; break;
        }
        asset.folder = meta.sourcePath.parent_path().filename().string();
        doc.assets.push_back(std::move(asset));
    }
    std::string err;
    auto browser = engine::editor::create_content_browser(doc, err);
    m_contentBrowserJson =
        browser != nullptr ? browser->to_json() : std::string();
}

std::string EditorApplication::build_ui_doc_json() {
    // Composes the editor's UI as ONE versioned JSON document (engine/ui
    // IUiDoc): layout + widgets + viewport + confirmations. Exposed via
    // GET /ui-doc as the data surface for reflection/scripting/MCP tooling.
    using namespace engine::ui;

    UiDoc doc;

    // Layout: screen -> header + viewport + footer.
    doc.layout.version = 1;
    doc.layout.root = "screen";
    doc.layout.tree.id = "screen";
    LayoutNode header;
    header.id = "header";
    header.weight = 0.0;
    header.min_h = 40.0;
    header.text_binding = "\"VulkanCraft Editor\"";
    doc.layout.tree.children.push_back(header);
    LayoutNode viewport;
    viewport.id = "viewport";
    viewport.weight = 1.0;
    doc.layout.tree.children.push_back(viewport);
    LayoutNode footer;
    footer.id = "footer";
    footer.weight = 0.0;
    footer.min_h = 32.0;
    doc.layout.tree.children.push_back(footer);

    // Widgets: fps bar, a confirmation modal, a 2-column focus grid.
    UiBarSpec fps;
    fps.id = "fps";
    fps.value_binding = "$fps";
    fps.min = 0.0;
    fps.max = 120.0;
    doc.widgets.bars.push_back(fps);

    UiModalSpec modal;
    modal.id = "exit_confirm";
    modal.title_binding = "\"Exit editor?\"";
    modal.visible_binding = "$exit_open";
    modal.confirm_label = "Exit";
    modal.cancel_label = "Cancel";
    modal.on_confirm = "editor:exit";
    modal.on_cancel = "editor:dismiss";
    doc.widgets.modals.push_back(modal);

    UiFocusSpec focus;
    focus.id = "shell";
    focus.ids = { "hierarchy", "inspector", "viewport", "console" };
    focus.cols = 2;
    doc.widgets.focus.push_back(focus);

    // Viewport: 16:9 reference, Fit, small safe-area (editor margins).
    doc.viewport.version = 1;
    doc.viewport.reference_width = 1920.0;
    doc.viewport.reference_height = 1080.0;
    doc.viewport.scale_mode = UiScaleMode::Fit;
    doc.viewport.safe_area.left = 20.0;
    doc.viewport.safe_area.top = 40.0;
    doc.viewport.text_scale = 1.25;
    doc.viewport.high_contrast = true;

    // Confirmation: destructive actions go through the authority gate.
    ConfirmActionSpec exitAction;
    exitAction.id = "exit";
    exitAction.title = "Exit editor";
    exitAction.severity = ConfirmSeverity::Danger;
    exitAction.on_confirm = "editor:exit";
    exitAction.on_cancel = "editor:dismiss";
    doc.confirmations.push_back(exitAction);

    return doc.to_json();
}

void EditorApplication::register_editor_panels() {
    // Shell + Wicked-port panels, declared once so menus/command palette/layout
    // can be derived from the registry instead of hardcoded flags. Categories
    // follow the PORTS.md grouping; titles are the localized strings the menu
    // already shows. toggleable = appears in the View menu with a show/hide.
    using Engine::Editor::EditorPanelSpec;
    const auto add = [this](const char* id, const char* title, const char* category,
                            bool toggleable = true, bool default_open = false) {
        EditorPanelSpec spec;
        spec.id = id;
        spec.title = title;
        spec.category = category;
        spec.toggleable = toggleable;
        spec.default_open = default_open;
        m_panelRegistry.register_panel(std::move(spec));
    };

    // Core editor shell.
    add("hierarchy", "Hierarchy", "scene", true, true);
    add("inspector", "Inspector", "scene", true, true);
    add("viewport", "Viewport", "scene", false, true);
    add("content_browser", "Content Browser", "assets", true, true);
    add("console", "Console", "debug", true, false);

    // Scene / object (Wicked hierarchy & object toolset).
    add("object_name", "Object Name", "scene");
    add("layers", "Layers", "scene");
    add("object", "Object", "scene");

    // Components (Wicked component windows).
    add("light", "Light", "components");
    add("camera", "Camera", "components");
    add("material", "Material", "components");
    add("sound", "Sound", "components");
    add("rigid_body", "Rigid Body", "components");
    add("collider", "Collider", "components");
    add("constraint", "Constraint", "components");
    add("soft_body", "Soft Body", "components");
    add("spring", "Spring", "components");
    add("decal", "Decal", "components");
    add("emitter", "Emitter", "components");
    add("hair_particle", "Hair Particle", "components");
    add("spline", "Spline", "components");
    add("force_field", "Force Field", "components");
    add("env_probe", "Env Probe", "components");
    add("weather", "Weather", "components");

    // Animation (Wicked animation toolset).
    add("animation", "Animation", "animation");
    add("armature", "Armature", "animation");
    add("humanoid", "Humanoid", "animation");
    add("ik", "IK", "animation");
    add("expression", "Expression", "animation");

    // Terrain / painting.
    add("terrain", "Terrain", "terrain");
    add("paint_tool", "Paint Tool", "terrain");

    // Import / content.
    add("mesh", "Mesh", "import");
    add("model_importer", "Model Importer", "import");
    add("video", "Video", "import");
    add("gaussian_splat", "Gaussian Splat", "import");

    // Editor tools / project.
    add("project_creator", "Project Creator", "editor");
    add("general", "General", "editor");
    add("graphics", "Graphics", "editor");
    add("profiler", "Profiler", "editor");
    add("theme_editor", "Theme Editor", "editor");
    add("developer", "Developer", "editor");
    add("guide", "Guide", "editor");
}

// ---------------------------------------------------------------------------
// Editor camera (orbit around a target; free-fly via WASD while orbiting)
// ---------------------------------------------------------------------------
namespace {

glm::vec3 euler_direction(float yawDeg, float pitchDeg) {
    const float yawRad = glm::radians(yawDeg);
    const float pitchRad = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(
        std::cos(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::sin(yawRad) * std::cos(pitchRad)));
}

// Distance from a point to a 2D segment (screen space).
float dist_point_segment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-8f) return glm::length(p - a);
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

// Strict float-token parsing for control-API commands: every token must be a
// complete number. std::stof / operator>> happily parse a UUID pasted into a
// numeric field as "2e7e5bd6…" = 2e7 = 20000000 and teleport the camera with
// no error — reject any leftover garbage after the last number instead.
bool parse_all_floats(const std::string& text, std::vector<float>& out) {
    out.clear();
    std::istringstream ss(text);
    float v;
    while (ss >> v) out.push_back(v);
    return ss.eof();  // failbit with unread text = garbage token
}

constexpr glm::vec3 kAxisDirs[3] = { {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} };
constexpr glm::vec3 kAxisColors[3] = { {1.0f, 0.25f, 0.25f}, {0.30f, 1.0f, 0.45f}, {0.35f, 0.62f, 1.0f} };

// Bridge glm → contract vectors (engine/editor IGizmoController).
engine::editor::GizVec3 glm_to_giz(const glm::vec3& v) {
    return engine::editor::GizVec3(v.x, v.y, v.z);
}

} // namespace

glm::vec3 EditorCamera::get_front() const {
    return euler_direction(yaw, pitch);
}

glm::vec3 EditorCamera::get_right() const {
    const glm::vec3 front = get_front();
    const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
    return right;
}

glm::vec3 EditorCamera::get_up() const {
    return glm::normalize(glm::cross(get_right(), get_front()));
}

glm::mat4 EditorCamera::get_view_matrix() const {
    return glm::lookAt(position, orbitTarget, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 EditorCamera::get_projection_matrix(float aspectRatio) const {
    return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
}

int EditorApplication::run() {
    try {
        init_window();
        init_vulkan();
        init_imgui();
        init_offscreen_target();
        init_scene_pipeline();
        init_geometry_buffers();
        init_default_scene();
        // Autosave recovery: a session that closed without an explicit save
        // leaves assets/scenes/autosave.scene — load it so nothing is lost.
        {
            const std::filesystem::path recovery =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes" / "autosave.scene";
            if (std::filesystem::is_regular_file(recovery)) {
                load_scene_file(recovery.string());
                std::cout << "[Editor] Autosave recuperado: " << recovery.string() << std::endl;
            }
        }
        // Persisted editor preferences (language, VSync, shadows, theme).
        load_settings();

        // VC_EDITOR_TEST_RENDERGRAPH=1: exercise the render graph executor on
        // the real device — a two-pass graph (Scene → Composite) is recorded
        // and submitted headlessly before the main loop, asserting the compiled
        // pass order and barriers drive the frame.
        if (std::getenv("VC_EDITOR_TEST_RENDERGRAPH") != nullptr) {
            std::exit(run_render_graph_self_test());
        }

        // VC_EDITOR_TEST_HDR=1: cook a tiny Radiance HDR, load it through the
        // material-graph texture path (must produce a real R16G16B16A16_SFLOAT
        // image, not the old solid fallback), read the pixel back and bind it
        // in a material-graph pipeline.
        if (std::getenv("VC_EDITOR_TEST_HDR") != nullptr) {
            std::exit(run_hdr_texture_self_test());
        }

        main_loop();
        cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Editor Fatal Error] " << e.what() << std::endl;
        return -1;
    }
}

void EditorApplication::init_window() {
    if (!glfwInit()) throw std::runtime_error("Failed to initialize GLFW");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, tr("VulkanCraft Engine - Gerenciador de Jogos", "VulkanCraft Engine - Game Launcher"), nullptr, nullptr);
    if (!m_window) throw std::runtime_error("Failed to create GLFW window");
    // Own scroll callback (chained by the ImGui backend, which stores it as its
    // previous callback and forwards events). The camera consumes the delta
    // when the 3D view is hovered; io.MouseWheel alone is useless there because
    // ImGui clears it at the end of NewFrame, after the camera update.
    glfwSetWindowUserPointer(m_window, this);
    glfwSetScrollCallback(m_window, [](GLFWwindow* win, double /*xoff*/, double yoff) {
        if (auto* app = static_cast<EditorApplication*>(glfwGetWindowUserPointer(win))) {
            app->m_scrollAccum += yoff;
        }
    });
    // Seed the mouse position so the first camera frame has no fake jump.
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    m_lastMousePos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
}

void EditorApplication::init_vulkan() {
    // VC_EDITOR_VALIDATION=1 enables the Vulkan validation layers + default
    // debug messenger (messages go to stderr). VC_EDITOR_SKIP_LAUNCHER=1 skips
    // the launcher hub so the 3D viewport is exercised immediately.
    const bool validate = std::getenv("VC_EDITOR_VALIDATION") != nullptr;
    vkb::InstanceBuilder builder;
    builder.set_app_name("VulkanCraft Engine")
           .require_api_version(1, 3, 0);
    if (validate) {
        builder.request_validation_layers(true);
        builder.use_default_debug_messenger();
    } else {
        builder.request_validation_layers(false);
    }
    auto inst_ret = builder.build();
    if (!inst_ret) throw std::runtime_error("Failed to create Vulkan instance");
    vkb::Instance vkb_inst = inst_ret.value();
    m_instance = vkb_inst.instance;

    glfwCreateWindowSurface(m_instance, m_window, nullptr, &m_surface);

    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    auto phys_ret = selector.set_surface(m_surface)
                            .set_minimum_version(1, 3)
                            .select();
    if (!phys_ret) throw std::runtime_error("Failed to select physical GPU");
    vkb::PhysicalDevice vkb_gpu = phys_ret.value();
    m_physicalDevice = vkb_gpu.physical_device;
    VkPhysicalDeviceProperties deviceProps{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &deviceProps);
    m_gpuName = deviceProps.deviceName ? deviceProps.deviceName : "Unknown GPU";

    vkb::DeviceBuilder device_builder{ vkb_gpu };
    auto dev_ret = device_builder.build();
    if (!dev_ret) throw std::runtime_error("Failed to create logical device");
    vkb::Device vkb_dev = dev_ret.value();
    m_device = vkb_dev.device;
    m_graphicsQueue = vkb_dev.get_queue(vkb::QueueType::graphics).value();
    m_graphicsQueueFamily = vkb_dev.get_queue_index(vkb::QueueType::graphics).value();

    // Swapchain creation with UNORM format (prevents washed-out sRGB gamma colors)
    vkb::SwapchainBuilder swapchain_builder{ vkb_dev };
    auto swap_ret = swapchain_builder
        .set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_extent(m_windowWidth, m_windowHeight)
        .build();
    if (!swap_ret) throw std::runtime_error("Failed to build swapchain");
    vkb::Swapchain vkb_swap = swap_ret.value();
    m_swapchain = vkb_swap.swapchain;
    m_swapchainFormat = vkb_swap.image_format;
    m_swapchainExtent = vkb_swap.extent;
    m_swapchainImages = vkb_swap.get_images().value();
    m_swapchainViews = vkb_swap.get_image_views().value();

    // Render pass creation
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }

    // Framebuffers
    m_framebuffers.resize(m_swapchainViews.size());
    for (size_t i = 0; i < m_swapchainViews.size(); i++) {
        VkImageView attachments[] = { m_swapchainViews[i] };
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = m_swapchainExtent.width;
        framebufferInfo.height = m_swapchainExtent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }

    // Command pool and buffers
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }

    m_commandBuffers.resize(2);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 2;
    if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }

    // Synchronization primitives
    m_imageAvailableSemaphores.resize(2);
    m_renderFinishedSemaphores.resize(2);
    m_inFlightFences.resize(2);

    VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };

    for (size_t i = 0; i < 2; i++) {
        vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]);
        vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]);
        vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]);
    }
}

void EditorApplication::init_imgui() {
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * 11;
    pool_info.poolSizeCount = static_cast<uint32_t>(sizeof(pool_sizes)/sizeof(pool_sizes[0]));
    pool_info.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_imguiDescriptorPool);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.FontGlobalScale = 1.0f; // Forge design system: roomy, not oversized

    // Frontend port (Wicked Editor, MIT): the base UI font is Liberation Sans
    // (the same font the Wicked editor ships, embedded zstd-compressed here),
    // decompressed through the public compression provider. Static storage
    // keeps the TTF alive for the atlas; the atlas must NOT take ownership.
    {
        auto provider = ::engine::compression::create_zstd_compression_provider();
        std::string uiFontData = provider->decompress(std::string(
            reinterpret_cast<const char*>(liberation_sans_zstd), sizeof(liberation_sans_zstd)));
        if (!uiFontData.empty()) {
            static std::string s_uiFont = std::move(uiFontData);
            ImFontConfig baseConfig{};
            baseConfig.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(s_uiFont.data()),
                                           static_cast<int>(s_uiFont.size()), 15.0f,
                                           &baseConfig, io.Fonts->GetGlyphRangesDefault());
        } else {
            io.Fonts->AddFontDefault();
        }
    }
    // Merge the Font Awesome 6 Solid icon font into the base font so ICON_FA_*
    // strings render inline. Glyph range from IconsFontAwesome6.h (0xe005–0xf8ff).
    static const ImWchar s_iconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconConfig{};
    iconConfig.MergeMode = true;
    iconConfig.GlyphMinAdvanceX = 16.0f;
    iconConfig.GlyphOffset = ImVec2(0.0f, 1.0f);
    iconConfig.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(font_awesome_v6),
                                   static_cast<int>(sizeof(font_awesome_v6)), 15.0f,
                                   &iconConfig, s_iconRanges);

    // Forge design system (light, product-grade). WindowMinSize (no panel can
    // shrink below this) is set inside applyForgeTheme.
    UI::applyForgeTheme();

    ImGui_ImplGlfw_InitForVulkan(m_window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = m_instance;
    init_info.PhysicalDevice = m_physicalDevice;
    init_info.Device = m_device;
    init_info.QueueFamily = m_graphicsQueueFamily;
    init_info.Queue = m_graphicsQueue;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = m_imguiDescriptorPool;
    init_info.MinImageCount = 2;
    init_info.ImageCount = 2;
    init_info.PipelineInfoMain.RenderPass = m_renderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init_info);

    // Diagnostic (post-atlas-build): which console glyphs are missing?
    {
        const char* probe = "Português (Brasil) | Memória RAM | Placa de Vídeo | çãõéêáíóúâôàü";
        std::string missing;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(probe); *p; ++p) {
            if (*p < 0x20) continue;
            ImFont* font = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
            if (!font || !font->IsGlyphInFont(*p)) {
                char b[16];
                snprintf(b, sizeof(b), "U+%04X ", *p);
                missing += b;
            }
        }
        std::cout << "[Font] " << (missing.empty() ? "All console glyphs covered" : ("Missing glyphs: " + missing)) << std::endl;
    }
}

void EditorApplication::init_default_scene() {
    m_editorScene = std::make_unique<Scene>("Untitled Scene");
    m_playMode.set_editor_scene(m_editorScene.get());
    m_activeScenePath.clear();
    m_autosavePath.clear();
    m_sceneDirty = false;
    // A fresh scene has no authored terrain (the previous scene's heightmap
    // must not leak into the new one).
    clear_terrain_mesh();

    Entity camera = m_editorScene->create_entity(tr("Câmera Principal", "Main Camera"));
    m_editorScene->transformComponents[camera.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
    m_editorScene->cameraComponents[camera.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };

    Entity sun = m_editorScene->create_entity(tr("Luz Direcional", "Directional Light"));
    m_editorScene->lightComponents[sun.get_id()] = LightComponent{ glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
    m_editorScene->transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);

    m_selectedEntity = camera;
    m_editorGui.init(m_editorScene.get(), &m_undo);
    m_editorGui.set_asset_registry(&m_assetRegistry);
    m_editorGui.select_entity(m_selectedEntity);
    // Disable the EditorGUI's own (English, undocked) panels: the real panels
    // are the Portuguese ones drawn by EditorApplication. Keeping the flags
    // false prevents duplicate floating windows ("World Outliner", "Inspector"...).
    m_editorGui.showOutliner = false;
    m_editorGui.showInspector = false;
    m_editorGui.showContentBrowser = false;
    m_editorGui.showConsole = false;
    m_editorGui.showVoxelTools = false;
    m_editorGui.showProfiler = false;

    if (std::getenv("VC_EDITOR_SKIP_LAUNCHER") != nullptr) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
    }

    // VC_EDITOR_TEST_MATERIAL=1: exercise the material-graph viewport path
    // (graph → GLSL → glslc → pipeline → per-entity UBO → draw) headlessly.
    if (std::getenv("VC_EDITOR_TEST_MATERIAL") != nullptr) {
        // The material graph pipeline is built by the viewport pass, which
        // only runs outside the launcher hub — leave the hub for this test.
        m_inLauncherMode = false;
        m_materialTestMatId = UUID();
        m_materialTestMeshId = UUID();
        Entity matCube = m_editorScene->create_entity("Material Test Cube");
        m_editorScene->transformComponents[matCube.get_id()].position = glm::vec3(2.0f, 1.0f, 0.0f);
        m_editorScene->materialComponents[matCube.get_id()] = MaterialComponent{
            glm::vec3(0.9f, 0.25f, 0.15f), 0.35f, 0.1f, glm::vec3(0.0f), 0.0f };
        m_editorScene->meshRendererComponents[matCube.get_id()] =
            MeshRendererComponent{ m_materialTestMeshId, m_materialTestMatId, true, true };
        // GPU mesh resource built from cube geometry (no asset round-trip).
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        generate_cube_geometry(verts, indices);
        EditorMeshResource cubeRes;
        cubeRes.vertexCount = static_cast<uint32_t>(verts.size());
        cubeRes.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
        const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
        create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      cubeRes.vb.buffer, cubeRes.vb.memory);
        create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      cubeRes.ib.buffer, cubeRes.ib.memory);
        safe_map_and_copy(m_device, cubeRes.vb.memory, 0, vbSize, verts.data());
        safe_map_and_copy(m_device, cubeRes.ib.memory, 0, ibSize, indices.data());
        cubeRes.valid = true;
        m_meshResources[m_materialTestMeshId] = std::move(cubeRes);
        // Material asset values become the graph's parameter defaults.
        MaterialAsset testMat;
        testMat.albedo = glm::vec3(0.2f, 0.6f, 0.9f);
        testMat.roughness = 0.7f;
        testMat.metallic = 0.0f;
        m_materialAssets[m_materialTestMatId] = testMat;
        m_materialTestFramesLeft = 150;
    }

    // VC_EDITOR_TEST_PLAY=1: a cube with a rigidbody is dropped into the play
    // world; the test asserts gravity moved it down while the viewport renders
    // the play scene.
    if (std::getenv("VC_EDITOR_TEST_PLAY") != nullptr) {
        // The play world only ticks outside the launcher hub (main_loop gates
        // tick_play_runtime on !m_inLauncherMode), so leave the hub for this
        // headless verification of the in-engine game.
        m_inLauncherMode = false;
        Entity fallingCube = m_editorScene->create_entity("Falling Cube");
        m_editorScene->transformComponents[fallingCube.get_id()].position = glm::vec3(0.0f, 5.0f, 0.0f);
        m_editorScene->rigidbodyComponents[fallingCube.get_id()] =
            RigidbodyComponent{ 1.0f, 0.5f, 0.1f, false, true };
        m_playTestEntityId = fallingCube.get_id();
        m_playMode.start_play(m_editorScene.get());
        setup_play_runtime();
        m_playTestFramesLeft = 120;
    }

    // VC_EDITOR_TEST_BUILD=1: run the full Build Game pipeline headlessly and
    // exit with the build result. A visible mesh entity is added first so the
    // packaged initial scene actually renders something.
    if (std::getenv("VC_EDITOR_TEST_BUILD") != nullptr) {
        Entity buildCube = m_editorScene->create_entity("Build Cube");
        m_editorScene->transformComponents[buildCube.get_id()].position = glm::vec3(0.0f, 0.5f, 0.0f);
        m_editorScene->materialComponents[buildCube.get_id()] = MaterialComponent{
            glm::vec3(0.25f, 0.65f, 0.90f), 0.35f, 0.1f, glm::vec3(0.0f), 0.0f };
        m_editorScene->meshRendererComponents[buildCube.get_id()] = MeshRendererComponent{};
        m_editorScene->rigidbodyComponents[buildCube.get_id()] = RigidbodyComponent{};
        run_game_build();
        const bool buildOk = std::none_of(m_buildLog.begin(), m_buildLog.end(),
                                          [](const std::string& line) { return line.rfind("Build failed", 0) == 0; });
        std::cout << "[Editor] BUILD_TEST " << (buildOk ? "PASS" : "FAIL") << std::endl;
        std::exit(buildOk ? 0 : 1);
    }
}


// ===========================================================================
// Core (constructor, init, main loop, render frame). Monolithic: a split
// attempt (EditorApplication_{Panels,Vulkan,PlayMode,Assets}.cpp, 18/ago) was
// never wired into CMake — those orphan duplicates were removed (2026-08-26,
// AGENT-2 §B dedup). All editor code lives here.
// ===========================================================================

int EditorApplication::run_render_graph_self_test() {
    using namespace Engine::Rendering;

    // Match the offscreen viewport target to the swapchain so a single render
    // area extent serves both passes.
    recreate_offscreen_if_needed(m_swapchainExtent.width, m_swapchainExtent.height);
    if (m_offscreen.framebuffer == VK_NULL_HANDLE) {
        std::cerr << "[Editor] RENDERGRAPH_TEST FAIL (no offscreen framebuffer)" << std::endl;
        return 1;
    }

    // Two-pass graph: Scene writes the offscreen color+depth; Composite reads
    // the color and writes the swapchain.
    RenderGraph graph;
    const auto sceneColor = graph.add_resource({ "Scene Color", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto sceneDepth = graph.add_resource({ "Scene Depth", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto swap = graph.add_resource({ "Swapchain", RenderResourceKind::Image, 0,
        m_swapchainExtent.width, m_swapchainExtent.height, 1, false, true, RenderResourceState::Present });
    const auto scenePass = graph.add_pass({ "Scene", RenderQueue::Graphics,
        { { sceneColor, RenderAccess::Write, RenderResourceState::ColorAttachment },
          { sceneDepth, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
    const auto compositePass = graph.add_pass({ "Composite", RenderQueue::Graphics,
        { { sceneColor, RenderAccess::Read, RenderResourceState::ShaderRead },
          { swap, RenderAccess::Write, RenderResourceState::Present } }, true });
    (void)graph.add_dependency(scenePass, compositePass);

    VulkanRenderGraphExecutor executor;
    std::string error;
    if (!executor.initialize(m_device, graph, &error)) {
        std::cerr << "[Editor] RENDERGRAPH_TEST FAIL (init: " << error << ")" << std::endl;
        return 1;
    }
    const RenderGraphCompileResult& compiled = executor.compile_result();
    if (compiled.order.size() != 2 || compiled.barriers.empty()) {
        std::cerr << "[Editor] RENDERGRAPH_TEST FAIL (order=" << compiled.order.size()
                  << ", barriers=" << compiled.barriers.size() << ")" << std::endl;
        return 1;
    }

    // The executor begins each pass itself, so the draw callbacks must record
    // content only (no render-pass begin) — here both passes are clear-only.
    // The game runtime (main_game.cpp) drives real content through these
    // callbacks (drawScene/drawComposite).
    VulkanRenderGraphExecutor::PassFrame sceneFrame;
    sceneFrame.renderPass = m_offscreen.renderPass;
    sceneFrame.framebuffers = { m_offscreen.framebuffer };
    sceneFrame.clearValues.resize(2);
    sceneFrame.clearValues[0].color = { { 0.2f, 0.3f, 0.4f, 1.0f } };
    sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
    sceneFrame.draw = [](VkCommandBuffer /*cb*/) {};
    executor.register_pass(scenePass, std::move(sceneFrame));

    VulkanRenderGraphExecutor::PassFrame compositeFrame;
    compositeFrame.renderPass = m_renderPass;
    compositeFrame.framebuffers = m_framebuffers;
    compositeFrame.clearValues.resize(1);
    compositeFrame.clearValues[0].color = { { 0.08f, 0.09f, 0.12f, 1.0f } };
    compositeFrame.draw = [](VkCommandBuffer /*cb*/) {};
    executor.register_pass(compositePass, std::move(compositeFrame));

    // Record one full frame through the executor and submit it.
    VkCommandBuffer cb = m_commandBuffers[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &begin);
    executor.record(cb, 0, m_swapchainExtent);
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    const VkResult result = vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkDeviceWaitIdle(m_device);
    vkQueueWaitIdle(m_graphicsQueue);

    const bool ok = result == VK_SUCCESS && executor.executed_pass_count() == 2 &&
                    executor.total_barriers() >= 1;
    std::cout << "[Editor] RENDERGRAPH_TEST " << (ok ? "PASS" : "FAIL")
              << " (passes=" << executor.executed_pass_count()
              << ", barriers=" << executor.total_barriers() << ")" << std::endl;
    return ok ? 0 : 1;
}

// VC_EDITOR_TEST_HDR=1: cooks a tiny Radiance HDR (2x1, left pixel red = 4.0),
// loads it through load_viewport_texture — which must produce a real
// R16G16B16A16_SFLOAT image instead of the old flat-shading fallback — then
// reads the first pixel back (half 0x4800 = 4.0) and builds a material-graph
// pipeline bound to the HDR texture.
int EditorApplication::run_hdr_texture_self_test() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path src = dir / "vc_hdr_selftest.hdr";
    const std::filesystem::path cookedDir = dir / "vc_hdr_selftest_cooked";
    std::ofstream out(src, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[Editor] HDR_TEST FAIL (cannot write " << src << ")" << std::endl;
        return 1;
    }
    // Radiance RGBE header; orientation line "-Y 1 +X 2" => 2x1, top-left first.
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    // Pixel 0: RGBE {1,0,0,138} => r = 2^(138-136) = 4.0 (half 0x4400).
    // Pixel 1: black.
    const uint8_t pixels[8] = { 1, 0, 0, 138, 0, 0, 0, 0 };
    out.write(reinterpret_cast<const char*>(pixels), 8);
    out.close();

    ImportRequest request;
    request.source = src;
    request.cookedDirectory = cookedDir;
    const ImportResult cooked = m_assetPipeline->import(request);
    if (!cooked) {
        std::cerr << "[Editor] HDR_TEST FAIL (cook: " << cooked.error << ")" << std::endl;
        return 1;
    }

    GraphTexture tex;
    std::string error;
    if (!load_viewport_texture(cooked.asset.id, tex, error)) {
        std::cerr << "[Editor] HDR_TEST FAIL (load: " << error << ")" << std::endl;
        return 1;
    }
    if (tex.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        std::cerr << "[Editor] HDR_TEST FAIL (format " << static_cast<int>(tex.format)
                  << " != R16G16B16A16_SFLOAT)" << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }

    // Read pixel 0 back and verify the half-float value survived (r = 4.0).
    const VkDeviceSize size = static_cast<VkDeviceSize>(2) * 1 * 8;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        std::cerr << "[Editor] HDR_TEST FAIL (staging alloc)" << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, tex.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { 2, 1, 1 };
    vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
    transition_image_layout(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, size, 0, &data);
    if (!data) {
        std::cerr << "[Editor] HDR_TEST FAIL (staging map)" << std::endl;
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        destroy_graph_texture(tex);
        return 1;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const uint16_t halfR = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
    vkUnmapMemory(m_device, stagingMemory);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    if (halfR != 0x4400) {
        std::cerr << "[Editor] HDR_TEST FAIL (pixel r half = 0x" << std::hex << halfR
                  << ", expected 0x4400 = 4.0)" << std::dec << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }

    // Build a material-graph pipeline with the HDR texture bound to BaseColor
    // (reconnect the BaseColor output of the standard PBR graph).
    Rendering::MaterialGraph graph = Rendering::material_graph_from_pbr(MaterialAsset{});
    const auto texNode = graph.add_texture_sample("HDR Texture");
    if (auto* node = graph.find_node(texNode)) node->value = cooked.asset.id.to_string();
    bool connected = false;
    for (const auto& candidate : graph.nodes()) {
        if (candidate.kind != Rendering::MaterialNodeKind::Output ||
            candidate.parameter != "BaseColor")
            continue;
        connected = graph.connect(texNode, candidate.id, 0);
        break;
    }
    GraphMaterialPipeline pipeline;
    if (!connected || !build_graph_pipeline(graph, pipeline)) {
        std::cerr << "[Editor] HDR_TEST FAIL (pipeline: "
                  << (pipeline.lastError.empty() ? "texture not connected" : pipeline.lastError) << ")" << std::endl;
        destroy_graph_texture(tex);
        return 1;
    }
    destroy_graph_pipeline(pipeline);
    destroy_graph_texture(tex);
    std::cout << "[Editor] HDR_TEST PASS (RGBA16F, pixel r=4.0 half=0x4400, texture pipeline bound)" << std::endl;
    return 0;
}

void EditorApplication::main_loop() {
    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        double currentTime = glfwGetTime();
        float deltaTime = static_cast<float>(currentTime - lastTime);
        lastTime = currentTime;
        m_fps = 1.0f / std::max(deltaTime, 0.001f);
        m_frameTimeMs = deltaTime * 1000.0f;
        if (m_frameProfiler) {
            m_frameProfiler->record(m_frameTimeMs,
                                    static_cast<double>(m_ramUsageMb));
        }

        // Graphics changes (Opções Gráficas) are deferred to here — before
        // acquire, with nothing in flight — so the swapchain and shadow map
        // can be recreated safely.
        if (m_recreateSwapchain) {
            m_recreateSwapchain = false;
            recreate_swapchain();
        }
        if (m_recreateShadowMap) {
            m_recreateShadowMap = false;
            const uint32_t newSize = shadow_size_from_quality(m_shadowQuality);
            if (m_shadowMap.size != newSize) {
                vkDeviceWaitIdle(m_device);
                m_shadowMap.size = newSize;
                create_shadow_map();
            }
        }

        // Control API: execute queued commands (play/pause/resume/stop/step)
        // from the loopback HTTP server. The drain runs even while the
        // launcher hub is open so API-driven actions (open-scene, new-scene,
        // create-project) can leave the hub; then publish live state for
        // /state.
        {
            for (const auto& pc : m_controlApi.drain_commands()) {
                std::cout << "[API-DEBUG] " << pc.cmd << std::endl;
                m_controlResult.clear();
                m_controlData.clear();
                handle_control_command(pc.cmd);
                m_controlApi.complete_command(pc.id, m_controlResult.empty(), m_controlResult, m_controlData);
            }
        }

        if (!m_inLauncherMode) {
            {
                EditorApiState api;
                switch (m_playMode.get_state()) {
                    case PlayState::Play: api.state = "play"; break;
                    case PlayState::Pause: api.state = "pause"; break;
                    case PlayState::Simulate: api.state = "simulate"; break;
                    default: break;
                }
                Scene* s = m_playMode.get_active_scene();
                // get_active_scene() can return null in Edit mode before the
                // first Play (the cached editor-scene pointer is only set by
                // start_play / set_editor_scene); fall back to the editor's
                // own scene, matching every render/tick path.
                if (!s) s = m_editorScene.get();
                api.fps = m_fps;
                api.entities = s ? s->get_entities().size() : 0u;
                api.orbitDistance = m_editorCamera.orbitDistance;
                api.viewportHovered = m_viewportHovered;
                api.imageHovered = m_viewportImageHovered;
                ImGuiIO& io = ImGui::GetIO();
                api.keyboardCapture = io.WantCaptureKeyboard;
                api.typing = io.WantTextInput;
                api.camX = m_editorCamera.position.x;
                api.camY = m_editorCamera.position.y;
                api.camZ = m_editorCamera.position.z;
                api.yaw = m_editorCamera.yaw;
                api.pitch = m_editorCamera.pitch;
                api.vsync = m_vsyncEnabled;
                api.shadowQuality = m_shadowQuality;
                api.terrainValid = m_terrainValid;
                if (m_terrainValid) {
                    const int seg = m_terrainParams.segments;
                    api.terrainVertices = static_cast<std::size_t>(seg + 1) * (seg + 1);
                    api.terrainTriangles = m_terrainIndexCount / 3;
                }
                api.meshEdited = m_meshEdited;
                api.settingsPath = m_settingsPath;
                api.selectedEntity = m_selectedEntity.is_valid()
                    ? m_selectedEntity.get_id().to_string() : std::string();
                switch (m_gizmoMode) {
                    case GizmoMode::Select: api.gizmoMode = "select"; break;
                    case GizmoMode::Translate: api.gizmoMode = "translate"; break;
                    case GizmoMode::Rotate: api.gizmoMode = "rotate"; break;
                    case GizmoMode::Scale: api.gizmoMode = "scale"; break;
                }
                if (m_gizmoLocal) api.gizmoMode += ":local";
                api.snap = m_snapTranslate;
                api.camTargetX = m_editorCamera.orbitTarget.x;
                api.camTargetY = m_editorCamera.orbitTarget.y;
                api.camTargetZ = m_editorCamera.orbitTarget.z;
                api.lastSelfTest = m_lastSelfTestResult;
                api.panels = m_panelRegistry.panel_ids();
                api.templates = m_templateRegistry.template_ids();
                api.ui_doc = m_uiDocJson;
                api.layout = m_layoutModel.snapshot().to_json();
                api.messages = m_messageCatalogJson;
                api.shortcuts = m_shortcutDocMarkdown;
                refresh_play_mode();
                api.play_mode = m_playModeJson;
                api.command_index = m_commandIndexJson;
                refresh_profiler();
                api.profiler = m_profilerJson;
                refresh_undo();
                api.undo = m_undoJson;
                api.content_browser = m_contentBrowserJson;
                refresh_window_mode();
                api.window_mode = m_windowModeJson;
                refresh_camera();
                api.camera = m_cameraJson;
                refresh_gizmo();
                api.gizmo = m_gizmoJson;
                refresh_publish();
                api.publish = m_publishJson;
                refresh_inspector();
                api.inspector = m_inspectorJson;
                refresh_hierarchy();
                api.hierarchy = m_hierarchyJson;
                refresh_onboarding();
                api.onboarding = m_onboardingJson;
                switch (m_playScript.status()) {
                    case VMStatus::Idle: api.scriptState = "idle"; break;
                    case VMStatus::Running: api.scriptState = "running"; break;
                    case VMStatus::Waiting: api.scriptState = "waiting"; break;
                    case VMStatus::Paused: api.scriptState = "paused"; break;
                    case VMStatus::Completed: api.scriptState = "completed"; break;
                    case VMStatus::Error: api.scriptState = "error"; break;
                }
                m_controlApi.publish_state(api);
            }
            update_editor_camera(deltaTime);
            process_viewport_input();
            if (m_stepRequested && m_playMode.get_state() == PlayState::Pause) {
                // Advance the play world a single frame (Pause → Play → tick →
                // Pause) so the PASSO button works.
                m_stepRequested = false;
                m_playMode.set_state(PlayState::Play);
                tick_play_runtime(deltaTime);
                m_playMode.set_state(PlayState::Pause);
            } else {
                m_stepRequested = false;
            }
            tick_play_runtime(deltaTime);
        }

        // Audio preview: pick up background decodes and start the requested
        // voice. Must run every frame in EVERY mode (the asset browser is used
        // in edit mode, where tick_play_runtime early-returns). The miniaudio
        // device drives the mixer in real time when available; without a
        // device we still advance it so voice state (▶/⏸) stays truthful.
        pump_audio_preview_decodes();
        pump_asset_thumbnail_decodes();
        if (!m_audioDeviceStarted) m_playAudio.render(1024);

        // Scene autosave: debounced persist of every change (see
        // mark_scene_dirty / autosave_scene). Runs in every mode so API
        // mutations made before the launcher hub is left still get saved.
        autosave_scene();

        // 3D asset thumbnails (mesh + block cubes): a few renders per frame.
        pump_asset_thumbnails(4);

        // Voxel block pipelines are built here, outside the render pass:
        // creating pipelines/uploading atlases while record_viewport_scene
        // content is recording the viewport pass hung the GPU (device lost).
        ensure_voxel_pipelines();

        // Runtime-wired Wicked-port features (hair strands, soft-body cloth,
        // video flipbooks, gaussian splats, env-probe captures): preview in
        // Edit and keep simulating in Play (the same scene the viewport draws).
        Scene* simScene = m_playMode.get_active_scene();
        if (!simScene) simScene = m_editorScene.get();
        tick_special_runtimes(simScene, deltaTime);

        render_frame();

        if (m_playTestFramesLeft > 0 && --m_playTestFramesLeft == 0) {
            Scene* playScene = m_playMode.get_active_scene();
            bool fell = false;
            float y = -1.0f;
            if (playScene) {
                const auto tit = playScene->transformComponents.find(m_playTestEntityId);
                if (tit != playScene->transformComponents.end()) {
                    y = tit->second.position.y;
                    fell = y < 4.0f;
                }
            }
            std::cout << "[Editor] PLAY_TEST " << (fell ? "PASS" : "FAIL")
                      << " (cube y=" << y << ")" << std::endl;
            vkDeviceWaitIdle(m_device);
            std::exit(fell ? 0 : 1);
        }

        if (m_materialTestFramesLeft > 0 && --m_materialTestFramesLeft == 0) {
            const auto it = m_graphMaterialPipelines.find(m_materialTestMatId);
            bool ok = false;
            std::string error;
            if (it != m_graphMaterialPipelines.end()) {
                ok = it->second.valid;
                error = it->second.lastError;
            }
            // Texture path: rebuild the live preview graph with a TextureSample
            // feeding BaseColor and verify a texture pipeline builds and binds.
            UUID textureId;
            std::string textureName;
            for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
                if (meta.type == AssetType::Texture && meta.isCooked) {
                    textureId = meta.id;
                    textureName = meta.sourcePath.filename().string();
                    break;
                }
            }
            if (ok && textureId.is_valid()) {
                auto& live = m_specializedEditors.live_material_graph_mutable();
                const auto texNode = live.add_texture_sample("Test Texture");
                if (auto* node = live.find_node(texNode)) node->value = textureId.to_string();
                for (const auto& candidate : live.nodes()) {
                    if (candidate.kind != Rendering::MaterialNodeKind::Output ||
                        candidate.parameter != "BaseColor")
                        continue;
                    auto* outNode = live.find_node(candidate.id);
                    if (!outNode || outNode->inputs.empty()) continue;
                    const auto* src = outNode->inputs[0].source != Rendering::InvalidMaterialNode
                        ? live.find_node(outNode->inputs[0].source) : nullptr;
                    if (src && src->kind == Rendering::MaterialNodeKind::Constant) {
                        (void)live.connect(texNode, outNode->id, 0);
                        break;
                    }
                }
                m_liveGraphHash = 0;
                destroy_graph_pipeline(m_liveGraphPipeline);
                if (!build_graph_pipeline(live, m_liveGraphPipeline)) {
                    ok = false;
                    error = m_liveGraphPipeline.lastError;
                } else if (m_liveGraphPipeline.textures.empty()) {
                    ok = false;
                    error = "texture pipeline has no bound textures";
                } else {
                    error = "texture: " + textureName;
                }
            } else if (ok) {
                ok = false;
                error = (error.empty() ? "" : error + "; ") + "no cooked texture asset found";
            }
            std::cout << "[Editor] MATERIAL_TEST " << (ok ? "PASS" : "FAIL")
                      << (error.empty() ? "" : " (" + error + ")") << std::endl;
            vkDeviceWaitIdle(m_device);
            std::exit(ok ? 0 : 1);
        }
    }

    // Final autosave flush: the window is closing — persist any change that
    // arrived inside the debounce window so nothing is lost on exit.
    autosave_scene(true);

    vkDeviceWaitIdle(m_device);
}

void EditorApplication::render_frame() {
    // Bounded wait: a GPU stall must degrade the frame, not hang the editor
    // forever (the previous UINT64_MAX wait froze input and left the window
    // black when a presentation fence never signalled).
    constexpr std::uint64_t kFenceTimeoutNs = 2'000'000'000ull; // 2s
    const VkResult fenceResult =
        vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, kFenceTimeoutNs);
    if (fenceResult == VK_TIMEOUT) {
        std::cerr << "[Vulkan] fence wait timed out; skipping frame\n";
        return;
    }
    if (fenceResult != VK_SUCCESS) {
        std::cerr << "[Vulkan] fence wait failed: " << static_cast<int>(fenceResult) << "\n";
        return;
    }

    // A pick requested from the previous frame is resolved before this frame's
    // scene pass so the freshly selected entity is highlighted immediately.
    // Hover pick uses the same pass (one extra pixel read, zero extra GPU cost).
    if ((m_pickRequested || m_hoverPickPending) && !m_inLauncherMode) {
        perform_pick_readback();
        m_pickRequested = false;
        m_hoverPickPending = false;
    }


    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
        }
        return;
    }
    if (result != VK_SUCCESS) {
        // VK_ERROR_DEVICE_LOST / VK_TIMEOUT / others: don't submit on a broken
        // swapchain (it would never present). Log and let the next frame retry.
        std::cerr << "[Vulkan] acquire next image failed: " << static_cast<int>(result) << "\n";
        return;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

    VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &beginInfo);

    if (!m_inLauncherMode) {
        // Size the offscreen to the panel (not the fitted image) so its aspect
        // ratio tracks the panel instead of locking onto its own previous size.
        recreate_offscreen_if_needed(
            static_cast<uint32_t>(std::max(1.0f, m_viewportPanelSize.x)),
            static_cast<uint32_t>(std::max(1.0f, m_viewportPanelSize.y)));
        if (m_offscreen.framebuffer != VK_NULL_HANDLE) {
            render_scene_to_offscreen(cmd);
        }
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = m_swapchainExtent;

    VkClearValue clearColor = { {{0.08f, 0.09f, 0.12f, 1.0f}} };
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (m_inLauncherMode) {
        draw_project_launcher();
    } else {
        // Ctrl+K: focus the global search box (the command palette).
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
            m_focusGlobalSearch = true;
        }

        // Draw order matters for the shell: main menu bar on top, then the
        // app bar, then the dockspace fills the remaining area (each positioned
        // from viewport->Pos/Size, so nothing is double-offset).
        draw_menu_bar();
        draw_app_bar();
        draw_dockspace();
        if (m_showHierarchy) draw_hierarchy_panel();
        if (m_showInspector) draw_inspector_panel();
        if (m_showViewport) draw_viewport_panel();
        if (m_showContentBrowser) draw_content_browser_panel();
#if VC_ENABLE_VOXEL_PLUGIN
        if (m_showVoxelTools) draw_voxel_tool_panel();
#endif
        if (m_showConsole) draw_console_panel();
        if (m_showScriptDebugger) draw_script_debugger_panel();
        if (m_showScriptCanvas) { if (!m_scriptCanvasLoaded) load_script_canvas(); draw_script_canvas_panel(); }
        {
            // Feed cooked texture assets to the Material Editor texture pickers.
            std::vector<std::pair<std::string, UUID>> textureAssets;
            for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
                if (meta.type == AssetType::Texture && meta.isCooked)
                    textureAssets.emplace_back(meta.sourcePath.filename().string(), meta.id);
            }
            m_specializedEditors.set_texture_assets(std::move(textureAssets));
        }
        Scene* activeScene = m_playMode.get_active_scene();
        if (!activeScene) activeScene = m_editorScene.get();
        m_specializedEditors.set_scene_context(activeScene, m_selectedEntity.get_id());
        m_specializedEditors.draw();
        {
            // Wicked-port tool windows: refresh the live context every frame.
            m_wickedTools.set_context(activeScene, m_selectedEntity.get_id(), &m_currentLanguage);
            // Paint tool state mirrors the selected entity's paintMode so the
            // viewport click handler paints without an extra callback.
            if (m_selectedEntity.is_valid()) {
                const auto paintIt = activeScene->paintComponents.find(m_selectedEntity.get_id());
                m_paintToolActive = paintIt != activeScene->paintComponents.end() &&
                                    paintIt->second.enabled && paintIt->second.paintMode;
            } else {
                m_paintToolActive = false;
            }
            m_wickedTools.set_asset_registry(&m_assetRegistry);
            m_wickedTools.set_open_specialized_editors(&m_specializedEditors.open);
            m_wickedTools.set_on_entity_deleted([this](UUID doomed) {
                // A tool panel deleted the entity: clear the editor selection so
                // the Inspector/hierarchy don't keep a dangling reference.
                if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == doomed) {
                    m_selectedEntity = Entity();
                    m_editorGui.select_entity(m_selectedEntity);
                }
            });
            m_wickedTools.set_create_project_callback([this](const std::string& name,
                                                             const std::string& folder) -> std::string {
                return create_project(name, folder);
            });
            m_wickedTools.set_terrain_callback([this](float scale, int octaves, float amount, float falloff) {
                generate_terrain_mesh(TerrainParams{ scale, octaves, amount, falloff });
            });
            m_wickedTools.set_graphics_callback([this](bool vsync, int quality) {
                apply_graphics_settings(vsync, quality);
            });
            m_wickedTools.set_save_settings_callback([this]() { save_settings(); });
            m_wickedTools.set_mesh_callback([this](int mode) -> std::string { return apply_mesh_normals(mode); });
            // Dev panel: route Control-API commands through the same handler the
            // HTTP API uses, and run headless self-tests on demand.
            m_wickedTools.set_control_command_callback([this](const std::string& cmd) {
                handle_control_command(cmd);
            });
            m_wickedTools.set_self_test_callback([this](int which) -> std::string {
                return run_editor_self_test(which);
            });
            m_wickedTools.set_package_assets_callback([this]() -> std::string {
                return package_assets_only();
            });
            m_wickedTools.set_hot_reload_status_callback([this]() -> std::string {
                if (!m_assetHotReload) return tr("inativo", "inactive");
                const size_t watched = m_assetRegistry.snapshot().size();
                return tr("ativo — vigia ", "active — watches ") + std::to_string(watched) +
                       tr(" asset(s) e reimporta mudanças nos arquivos de origem",
                          " asset(s) and reimports source-file changes");
            });
            m_wickedTools.set_play_state(static_cast<int>(m_playMode.get_state()));
            // Profiler: feed frame stats every frame for the graph window.
            m_wickedTools.set_frame_stats(m_fps, m_frameTimeMs);
            m_wickedTools.set_import_asset_callback([this](const std::string& requested) -> std::string {
                // "" = ask for a file via the editor's Windows dialog.
                std::string path = requested;
                if (path.empty()) {
                    if (!pick_file_dialog(path, L"Modelos (*.gltf;*.fbx;*.obj;*.ply)\0*.gltf;*.fbx;*.obj;*.ply\0Todos (*.*)\0*.*\0",
                                           L"Importar Modelo", nullptr)) {
                        return std::string();
                    }
                }
                if (!m_assetPipeline) return "Sem pipeline de assets.";
                const std::filesystem::path cookedRoot =
                    std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                const ImportResult result = m_assetPipeline->import({ path, cookedRoot, 1 });
                if (!result) return "Falha: " + result.error;
                std::cout << "[Editor] Modelo importado: " << path << " (" << result.asset.id.to_string() << ")" << std::endl;
                return "OK: " + result.asset.sourcePath.filename().string();
            });
            m_wickedTools.draw();
        }
    }

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkSemaphore waitSemaphores[] = { m_imageAvailableSemaphores[m_currentFrame] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[m_currentFrame] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]);

    VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }
    m_currentFrame = (m_currentFrame + 1) % 2;
}

void EditorApplication::recreate_swapchain() {
    vkDeviceWaitIdle(m_device);
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    for (auto view : m_swapchainViews) vkDestroyImageView(m_device, view, nullptr);
    m_framebuffers.clear();
    m_swapchainViews.clear();
    m_swapchainImages.clear();

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(m_window, &width, &height);
        glfwWaitEvents();
    }
    vkDeviceWaitIdle(m_device);

    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities) != VK_SUCCESS) {
        return;
    }
    VkExtent2D extent = capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()
        ? capabilities.currentExtent
        : VkExtent2D{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    extent.width = std::clamp(extent.width, std::max(1u, capabilities.minImageExtent.width), capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, std::max(1u, capabilities.minImageExtent.height), capabilities.maxImageExtent.height);

    // Prefer UNORM (matches the initial swapchain: no sRGB gamma washout).
    VkFormat imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
            imageFormat = candidate.format;
            colorSpace = candidate.colorSpace;
            break;
        }
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainKHR oldSwapchain = m_swapchain;
    VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = imageFormat;
    createInfo.imageColorSpace = colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // VSync (Opções Gráficas): FIFO when on; MAILBOX (preferred) or IMMEDIATE
    // when off — queried from the surface so unsupported modes never break.
    createInfo.presentMode = m_vsyncEnabled ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (!m_vsyncEnabled) {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, modes.data());
        for (const VkPresentModeKHR mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { createInfo.presentMode = mode; break; }
        }
    }
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;
    if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        m_swapchain = oldSwapchain;
        return;
    }
    vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);

    m_swapchainFormat = imageFormat;
    m_swapchainExtent = extent;
    uint32_t swapImageCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &swapImageCount, nullptr);
    m_swapchainImages.resize(swapImageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &swapImageCount, m_swapchainImages.data());
    m_swapchainViews.resize(swapImageCount);
    m_framebuffers.resize(swapImageCount);
    for (uint32_t i = 0; i < swapImageCount; ++i) {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = m_swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to recreate swapchain image view");
        }
        VkImageView attachments[] = { m_swapchainViews[i] };
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to recreate framebuffer");
        }
    }
}


size_t EditorApplication::import_texture_pack(const std::filesystem::path& folder) {
    if (!m_assetPipeline || !std::filesystem::is_directory(folder)) return 0;
    const std::filesystem::path cookedRoot =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
    size_t imported = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg" &&
            ext != ".bmp" && ext != ".hdr" && ext != ".dds" &&
            ext != ".glb" && ext != ".gltf" && ext != ".fbx" &&
            ext != ".obj" && ext != ".ply" &&
            ext != ".wav" && ext != ".ogg" && ext != ".mp3" &&
            ext != ".vmat") continue;
        const ImportResult result = m_assetPipeline->import({it->path(), cookedRoot, 1});
        if (result) {
            ++imported;
            if ((ext == ".png" || ext == ".tga" || ext == ".bmp" || ext == ".dds") &&
                result.asset.width > 0 && result.asset.height > 0 &&
                result.asset.width == result.asset.height) {
                const uint32_t s = result.asset.width;
                if (s >= 8 && s <= 256 && (s & (s - 1)) == 0) {
                    if (!is_character_texture(result.asset) && !is_aux_map_texture(result.asset)) {
                        create_block_asset(result.asset);
                    }
                }
            }
        } else if (result.error.rfind("No importer supports", 0) != 0) {
            std::cerr << "[PackImport] " << it->path().filename().string()
                      << ": " << result.error << std::endl;
        }
    }
    if (imported > 0) {
        m_assetHotReload->watch_registered_assets();
        const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
            "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[PackImport] Could not persist registry" << std::endl;
        std::cout << "[PackImport] Imported " << imported << " assets from "
                  << folder.string() << std::endl;
    }
    return imported;
}




// ===========================================================================
// Editor Panels (split from EditorApplication.cpp for compilation-unit separation)
// ===========================================================================
void EditorApplication::draw_project_launcher() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Project Launcher Hub", nullptr, flags);

    // Modern Header Banner
    ImGui::SetCursorPosY(35.0f);
    ImGui::SetCursorPosX((viewport->WorkSize.x - 550.0f) * 0.5f);
    ImGui::TextColored(ImVec4(0.39f, 0.40f, 0.95f, 1.00f), "%s", tr("GERENCIADOR DE JOGOS VULKAN ENGINE", "VULKAN ENGINE GAME LAUNCHER"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.4f, 1.0f), "[v1.5.0]");

    ImGui::SetCursorPosX((viewport->WorkSize.x - 550.0f) * 0.5f);
    ImGui::TextDisabled("%s", tr("Escolha um jogo para editar ou crie um novo projeto", "Select a game to edit or create a new project"));
    ImGui::Separator();
    ImGui::Spacing();

    // Centered Projects Card
    ImGui::SetCursorPosX((viewport->WorkSize.x - 720.0f) * 0.5f);
    ImGui::BeginChild("ProjectsListContainer", ImVec2(720, 480), true, ImGuiWindowFlags_None);

    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.95f, 1.0f), "%s", tr("Seus Jogos e Projetos:", "Your Games & Projects:"));
    ImGui::Separator();
    ImGui::Spacing();

    // Scan Projects/ for real project folders (no hardcoded list).
    std::vector<LauncherProject> projects;
    scan_projects(projects);

    if (projects.empty()) {
        ImGui::TextDisabled("%s", tr("Nenhum projeto encontrado em Projects/ — crie um novo acima.",
                                      "No projects found in Projects/ — create one above."));
        ImGui::Spacing();
    }

    for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
        const auto& proj = projects[i];
        bool isSelected = (m_selectedProjectIndex == i);

        ImGui::PushID(i);
        if (ImGui::Selectable("##ProjectSelectable", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 80))) {
            m_selectedProjectIndex = i;
            m_currentProjectName = proj.name;
            if (ImGui::IsMouseDoubleClicked(0)) {
                m_inLauncherMode = false; // Launch Engine Studio
                glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
            }
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::TextColored(isSelected ? ImVec4(0.4f, 0.7f, 1.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[JOGO]  %s", proj.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(proj.hasScene ? ImVec4(0.20f, 0.82f, 0.60f, 1.0f) : ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                           proj.hasScene ? tr("[TEM CENA]", "[HAS SCENE]") : tr("[VAZIO]", "[EMPTY]"));

        ImGui::TextDisabled("Pasta: %s", proj.path.c_str());
        ImGui::TextDisabled("%s: %s", tr("Modificado", "Last modified"), proj.lastModified.c_str());
        ImGui::EndGroup();

        ImGui::PopID();
        ImGui::Separator();
    }

    ImGui::EndChild();

    // Launcher Action Buttons
    ImGui::SetCursorPosY(viewport->WorkSize.y - 75.0f);
    ImGui::SetCursorPosX((viewport->WorkSize.x - 720.0f) * 0.5f);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.39f, 0.40f, 0.95f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.49f, 0.50f, 1.00f, 1.00f));
    if (ImGui::Button(tr("ABRIR NO EDITOR", "LAUNCH ENGINE STUDIO"), ImVec2(250, 44))) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    if (ImGui::Button(tr("+ Criar Novo Jogo", "+ Create New Game"), ImVec2(220, 44))) {
        m_inLauncherMode = false;
        glfwSetWindowTitle(m_window, "VulkanCraft Engine - [Novo Jogo]");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Procurar Pasta...", "Browse Folder..."), ImVec2(220, 44))) {
        std::string folder;
        if (pick_folder_dialog(folder, L"Escolher pasta do projeto")) {
            // Enter the editor scoped to the chosen project folder.
            m_currentProjectName = std::filesystem::path(folder).filename().string();
            if (m_currentProjectName.empty()) m_currentProjectName = "Projeto";
            m_inLauncherMode = false;
            glfwSetWindowTitle(m_window, ("VulkanCraft Engine - [" + m_currentProjectName + "]").c_str());
        }
    }

    ImGui::End();
}

void EditorApplication::draw_dockspace() {
    static bool firstTime = true;
    ImGuiID dockspace_id = ImGui::GetID("VulkanEngineStudioDockspace");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Dock area starts below the main menu bar + the 56 px app bar. Base the
    // math on viewport->Pos/Size (not WorkPos/WorkSize, which already accounts
    // for the menu bar) so the offset is applied exactly once.
    const float menuBarHeight = ImGui::GetFrameHeight();
    const float appBarHeight = 56.0f;
    ImVec2 dockPos = ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight + appBarHeight);
    ImVec2 dockSize = ImVec2(viewport->Size.x, viewport->Size.y - menuBarHeight - appBarHeight);

    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // No global WindowMinSize may inflate the shell (it fills the remaining area).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));

    ImGui::Begin("VulkanCraft Engine Shell", nullptr, host_window_flags);
    ImGui::PopStyleVar(4);

    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    if (firstTime) {
        firstTime = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dockSize);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.20f, nullptr, &dock_main_id);
        // The left column is split vertically: Scene (always visible) on top,
        // the voxel sculpting tools below it — never a competing tab that hides
        // the scene hierarchy.
        ImGuiID dock_left_bottom = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.30f, nullptr, &dock_left);
        ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, 0.25f, nullptr, &dock_main_id);
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.25f, nullptr, &dock_main_id);

        ImGui::DockBuilderDockWindow(tr("Cena", "Scene"), dock_left);
        ImGui::DockBuilderDockWindow(tr("Inspector", "Inspector"), dock_right);
        ImGui::DockBuilderDockWindow(tr("Viewport", "Viewport"), dock_main_id);
        ImGui::DockBuilderDockWindow(tr("Assets", "Assets"), dock_bottom);
        ImGui::DockBuilderDockWindow(tr("Console", "Console"), dock_bottom);
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::DockBuilderDockWindow(tr("Escultura de Blocos", "Voxel Sculpting Tools"), dock_left_bottom);
#endif

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::End();
}

void EditorApplication::draw_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(tr("Arquivo", "File"))) {
            if (ImGui::MenuItem(tr("Gerenciador de Jogos", "Game Launcher Hub"))) {
                m_inLauncherMode = true;
                glfwSetWindowTitle(m_window, tr("VulkanCraft Engine - Gerenciador de Jogos", "VulkanCraft Engine - Game Launcher"));
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Novo Jogo", "New Scene"), "Ctrl+N")) {
                // Ask whether to save the current scene before discarding it.
                m_pendingNewSceneConfirm = true;
            }
            if (ImGui::MenuItem(tr("Abrir Jogo...", "Open Scene..."), "Ctrl+O")) {
                std::string scenePath;
                if (pick_file_dialog(scenePath, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0",
                                     L"Abrir Cena", L"scene")) {
                    load_scene_file(scenePath);
                }
            }
            if (ImGui::MenuItem(tr("Salvar Jogo", "Save Scene"), "Ctrl+S")) {
                save_current_scene();
            }
            if (ImGui::MenuItem(tr("Salvar Como...", "Save Scene As..."))) {
                save_scene_as();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Exportar Jogo Pronto (.exe)", "Export Executable Game Build..."))) {
                run_game_build();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Sair", "Exit Studio"), "Alt+F4")) {
                glfwSetWindowShouldClose(m_window, true);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Editar", "Edit"))) {
            if (ImGui::MenuItem(tr("Desfazer", "Undo"), "Ctrl+Z", false, m_undo.can_undo())) {
                m_undo.undo();
                mark_scene_dirty();
            }
            if (ImGui::MenuItem(tr("Refazer", "Redo"), "Ctrl+Y", false, m_undo.can_redo())) {
                m_undo.redo();
                mark_scene_dirty();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(tr("Configurações", "Settings"))) {
                if (ImGui::BeginMenu(tr("Idioma / Language", "Language"))) {
                    bool isPt = (m_currentLanguage == EngineLanguage::PT_BR);
                    bool isEn = (m_currentLanguage == EngineLanguage::EN_US);
                    if (ImGui::MenuItem("Português (Brasil)", nullptr, isPt)) {
                        m_currentLanguage = EngineLanguage::PT_BR;
                    }
                    if (ImGui::MenuItem("English (US)", nullptr, isEn)) {
                        m_currentLanguage = EngineLanguage::EN_US;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Adicionar Objeto", "GameObject"))) {
            if (ImGui::MenuItem(tr("Objeto Vazio", "Create Empty Entity"))) {
                if (m_editorScene) {
                    Entity ent = m_editorScene->create_entity(tr("Novo Objeto", "New Entity"));
                    m_selectedEntity = ent;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Objeto 3D > Cubo", "3D Object > Cube"))) {
                if (m_editorScene) {
                    Entity cube = m_editorScene->create_entity(tr("Cubo 3D", "Cube"));
                    m_editorScene->meshRendererComponents[cube.get_id()] = MeshRendererComponent{};
                    m_selectedEntity = cube;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz do Sol", "Light > Directional Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz do Sol", "Directional Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{};
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz de Lâmpada", "Light > Point Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz de Lâmpada", "Point Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true };
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz Spot", "Light > Spot Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz Spot", "Spot Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
            if (ImGui::MenuItem(tr("Iluminação > Luz de Área", "Light > Area Light"))) {
                if (m_editorScene) {
                    Entity light = m_editorScene->create_entity(tr("Luz de Área", "Area Light"));
                    m_editorScene->lightComponents[light.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
                    m_selectedEntity = light;
                    mark_scene_dirty();
                }
            }
#if VC_ENABLE_VOXEL_PLUGIN
            if (ImGui::MenuItem(tr("Blocos > Mundo de Blocos", "Voxel > Voxel Terrain Volume"))) {
                if (m_editorScene) {
                    Entity voxel = m_editorScene->create_entity(tr("Mundo de Blocos", "Voxel Volume"));
                    m_editorScene->voxelVolumeComponents[voxel.get_id()] = VoxelVolumeComponent{};
                    m_selectedEntity = voxel;
                    mark_scene_dirty();
                }
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Janelas", "Window"))) {
            ImGui::MenuItem(tr("Viewport", "Viewport"), nullptr, &m_showViewport);
            ImGui::MenuItem(tr("Cena", "Scene"), nullptr, &m_showHierarchy);
            ImGui::MenuItem(tr("Inspector", "Inspector"), nullptr, &m_showInspector);
            ImGui::MenuItem(tr("Assets", "Assets"), nullptr, &m_showContentBrowser);
#if VC_ENABLE_VOXEL_PLUGIN
            ImGui::MenuItem(tr("Escultura de Blocos", "Voxel Sculpting Tools"), nullptr, &m_showVoxelTools);
#endif
            ImGui::MenuItem(tr("Console", "Console"), nullptr, &m_showConsole);
            ImGui::MenuItem(tr("Debugger de Scripts", "Script Debugger"), nullptr, &m_showScriptDebugger);
            ImGui::MenuItem(tr("Canvas de Scripts", "Script Canvas"), nullptr, &m_showScriptCanvas);
            ImGui::Separator();
            ImGui::MenuItem(tr("Editores Especializados", "Specialized Editors"), nullptr, &m_specializedEditors.open);
            m_wickedTools.draw_tools_menu();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Ajuda", "Help"))) {
            if (ImGui::MenuItem(tr("Como Usar (Guia)", "How to Use (Guide)"))) {
                m_wickedTools.showGuideWindow = !m_wickedTools.showGuideWindow;
            }
            if (ImGui::MenuItem(tr("Painel de Desenvolvimento", "Developer Panel"))) {
                m_wickedTools.showDevWindow = !m_wickedTools.showDevWindow;
            }
            if (ImGui::MenuItem(tr("Manual da Engine", "VulkanCraft Documentation"))) {
                // Open the docs folder in Explorer (best effort).
                std::string docsDir = "docs";
                if (std::filesystem::exists("docs")) {
                    ShellExecuteW(nullptr, L"open", L"docs", nullptr, nullptr, SW_SHOWNORMAL);
                } else if (std::filesystem::exists("../docs")) {
                    ShellExecuteW(nullptr, L"open", L"..\\docs", nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            if (ImGui::MenuItem(tr("Sobre a Engine", "About VulkanCraft Engine"))) {
                m_showAboutDialog = true;
            }
            ImGui::EndMenu();
        }

        // Sobre — modal simples.
        if (m_showAboutDialog) {
            ImGui::OpenPopup(tr("Sobre a Engine", "About"));
            m_showAboutDialog = false;
        }
        if (ImGui::BeginPopupModal(tr("Sobre a Engine", "About"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("VulkanCraft Engine 1.5.0");
            ImGui::Separator();
            ImGui::TextWrapped("%s", tr(
                "Engine de jogos em Vulkan 1.3 (renderer, cena, ECS, física, "
                "assets, voxel, navegação, áudio, scripting). Frontend do editor "
                "inspirado/portado do Wicked Engine (MIT) — ver "
                "src/editor/frontend/PORTS.md.",
                "Vulkan game engine on Vulkan 1.3 (renderer, scene, ECS, physics, "
                "assets, voxel, navigation, audio, scripting). Editor frontend "
                "ported/inspired by Wicked Engine (MIT) — see "
                "src/editor/frontend/PORTS.md."));
            if (ImGui::Button(tr("Fechar", "Close"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Novo Jogo: confirm before discarding the current scene + name it.
        if (m_pendingNewSceneConfirm) {
            ImGui::OpenPopup(tr("Novo Jogo", "New Scene"));
            m_pendingNewSceneConfirm = false;
        }
        if (ImGui::BeginPopupModal(tr("Novo Jogo", "New Scene"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", tr("Criar uma nova cena? A cena atual será descartada.",
                                          "Create a new scene? The current scene will be discarded."));
            ImGui::Separator();
            ImGui::InputText(tr("Nome da Cena", "Scene Name"), m_newSceneName, sizeof(m_newSceneName));
            ImGui::Spacing();
            if (ImGui::Button(tr("Salvar e Criar", "Save & Create"), ImVec2(150, 0))) {
                save_current_scene();
                create_new_scene();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Criar Sem Salvar", "Create Without Saving"), ImVec2(180, 0))) {
                create_new_scene();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancelar", "Cancel"), ImVec2(110, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorApplication::draw_app_bar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float kBarHeight = 56.0f;

    // Position from viewport->Pos/Size: the menu bar occupies the first
    // frame height, the app bar sits right below it (no WorkPos double-offset).
    const float menuBarHeight = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, kBarHeight), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 10.0f));
    // No global WindowMinSize may inflate this fixed 56 px bar.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UI::Colors::Surface);

    ImGui::Begin("##AppBar", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);

    // Search state is shared with the command palette drawn below the table.
    static char search[128]{};
    ImVec2 searchMin{ 0.0f, 0.0f };
    ImVec2 searchMax{ 0.0f, 0.0f };

    // 3-column responsive shell: Left (logo + actions) | Center (PLAY) |
    // Right (search + config). The stretch columns absorb the window width so
    // nothing is hard-positioned by fixed viewport math, and the table clips
    // cell content instead of letting any element overflow the bar.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 0.0f));
    if (ImGui::BeginTable("##AppBarLayout", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Center", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        // Left: logo + product name + New Scene / Import / Save.
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(UI::Colors::Accent, "%s", ICON_FA_CUBES);
        ImGui::SameLine();
        ImGui::TextUnformatted("VulkanCraft");
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_PLUS "  Nova Cena", ICON_FA_PLUS "  New Scene"), ImVec2(122, 36))) {
            m_pendingNewSceneConfirm = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_FILE_IMPORT "  Importar", ICON_FA_FILE_IMPORT "  Import"), ImVec2(108, 36))) {
            std::string path;
            if (pick_file_dialog(path, L"Assets (*.*)\0*.*\0", L"Importar Asset", nullptr)) {
                if (m_assetPipeline) {
                    const std::filesystem::path cookedRoot =
                        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                    const ImportResult result = m_assetPipeline->import({ path, cookedRoot, 1 });
                    if (result) {
                        std::cout << "[Editor] Asset importado: " << result.asset.sourcePath.filename().string() << std::endl;
                    } else {
                        std::cout << "[Editor] Falha ao importar: " << result.error << std::endl;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_FLOPPY_DISK "  Salvar", ICON_FA_FLOPPY_DISK "  Save"), ImVec2(96, 36))) {
            save_current_scene();
        }
        ImGui::SameLine();
        // [Build] — the fundamental action lives in the app bar, not buried in
        // Arquivo > Exportar.
        if (ImGui::Button(tr(ICON_FA_HAMMER "  Build", ICON_FA_HAMMER "  Build"), ImVec2(96, 36))) {
            run_game_build();
        }

        // Center: the single play button (green to start/resume, amber to
        // pause; right-click while playing = PARAR), centered in its column.
        ImGui::TableSetColumnIndex(1);
        const float btnWidth = 150.0f;
        const float btnHeight = 36.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImMax(0.0f, (ImGui::GetContentRegionAvail().x - btnWidth) * 0.5f));

        const PlayState state = m_playMode.get_state();
        const bool inPlay = state != PlayState::Edit;
        const bool paused = state == PlayState::Pause;
        const std::string playLabel = std::string(" " ICON_FA_PLAY "  ") + tr("TESTAR JOGO", "PLAY");
        const std::string resumeLabel = std::string(" " ICON_FA_PLAY "  ") + tr("CONTINUAR", "RESUME");
        const std::string pauseLabel = std::string(" " ICON_FA_PAUSE "  ") + tr("PAUSAR", "PAUSE");

        if (inPlay && !paused) {
            ImGui::PushStyleColor(ImGuiCol_Button, UI::Colors::Warning);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.72f, 0.25f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, UI::Colors::Success);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.78f, 0.45f, 1.0f));
        }
        const char* playBtnLabel = paused ? resumeLabel.c_str() : (inPlay ? pauseLabel.c_str() : playLabel.c_str());
        if (ImGui::Button(playBtnLabel, ImVec2(btnWidth, btnHeight))) {
            if (!inPlay) {
                m_playMode.start_play(m_editorScene.get());
                setup_play_runtime();
            } else {
                m_playMode.pause_play();
            }
        }
        // IsItemHovered() is false while the tooltip popup is open, so use the
        // raw rect test for the right-click stop.
        if (inPlay && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
            teardown_play_runtime();
            m_playMode.stop_play();
            m_playMode.set_editor_scene(m_editorScene.get());
            m_selectedEntity = Entity();
            m_editorGui.select_entity(m_selectedEntity);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", inPlay
                ? tr("Clique: PAUSAR/CONTINUAR • Clique direito: PARAR o jogo",
                     "Click: PAUSE/RESUME • Right-click: STOP the game")
                : tr("Inicia o jogo interno (Play In Editor) — física, scripts, partículas e armas rodam no viewport",
                     "Starts the in-engine game (Play In Editor) — physics, scripts, particles and weapons run in the viewport"));
        }
        // PASSO: single-frame step while paused (was Control-API-only).
        if (paused) {
            ImGui::SameLine();
            if (ImGui::Button(tr(" PASSO ", " STEP "), ImVec2(0, btnHeight))) {
                m_stepRequested = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("Avança um único frame do jogo pausado (equivalente ao comando 'step' da Control API)", "Advances the paused game a single frame (same as the 'step' Control API command)"));
            }
        }

        // Right: search (stretches) + help + settings. The search box doubles
        // as a real command palette (Ctrl+K focuses it; see below).
        ImGui::TableSetColumnIndex(2);
        const float iconArea = 2.0f * 26.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImMax(80.0f, ImGui::GetContentRegionAvail().x - iconArea));
        if (m_focusGlobalSearch) {
            ImGui::SetKeyboardFocusHere();
            m_focusGlobalSearch = false;
        }
        ImGui::InputTextWithHint("##GlobalSearch", ICON_FA_MAGNIFYING_GLASS "  Buscar (Ctrl+K)", search, sizeof(search));
        const ImVec2 searchMin = ImGui::GetItemRectMin();
        const ImVec2 searchMax = ImGui::GetItemRectMax();
        ImGui::SameLine();
        if (UI::iconButton(ICON_FA_CIRCLE_QUESTION, tr("Ajuda", "Help"))) {
            m_showAboutDialog = true;
        }
        ImGui::SameLine();
        if (UI::iconButton(ICON_FA_GEAR, tr("Configurações", "Settings"))) {
            m_wickedTools.showGeneralWindow = !m_wickedTools.showGeneralWindow;
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // Command palette: typing in the search box filters real commands, Enter
    // runs the first match, Esc closes. Drawn outside the table (a floating
    // window anchored below the search field).
    if (search[0] != '\0') {
        const float paletteW = ImMax(260.0f, searchMax.x - searchMin.x);
        ImGui::SetNextWindowPos(ImVec2(searchMin.x, searchMax.y + 6.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(paletteW, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::Begin("##CommandPalette", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PopStyleVar();

        std::string query = search;
        std::transform(query.begin(), query.end(), query.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        struct PaletteCmd { const char* label; std::function<void()> run; };
        const std::vector<PaletteCmd> cmds = {
            { tr("Novo Jogo...", "New Scene..."), [this]() { m_pendingNewSceneConfirm = true; } },
            { tr("Abrir Cena...", "Open Scene..."), [this]() {
                std::string p;
                if (pick_file_dialog(p, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0", L"Abrir Cena", L"scene")) load_scene_file(p);
            } },
            { tr("Salvar Cena", "Save Scene"), [this]() { save_current_scene(); } },
            { tr("Salvar Como...", "Save Scene As..."), [this]() { save_scene_as(); } },
            { tr("Build / Exportar Jogo (.exe)", "Build / Export Executable"), [this]() { run_game_build(); } },
            { tr("Importar Asset...", "Import Asset..."), [this]() {
                std::string path;
                if (pick_file_dialog(path, L"Assets (*.*)\0*.*\0", L"Importar Asset", nullptr)) {
                    if (m_assetPipeline) {
                        const std::filesystem::path cookedRoot =
                            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                        const ImportResult result = m_assetPipeline->import({ path, cookedRoot, 1 });
                        if (!result) std::cerr << "[Editor] " << result.error << std::endl;
                    }
                }
            } },
            { tr("Adicionar Cubo", "Add Cube"), [this]() {
                if (m_editorScene) {
                    Entity e = m_editorScene->create_entity(tr("Cubo 3D", "Cube"));
                    m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{};
                    m_selectedEntity = e;
                }
            } },
            { tr("Adicionar Objeto Vazio", "Add Empty Object"), [this]() {
                if (m_editorScene) m_selectedEntity = m_editorScene->create_entity(tr("Novo Objeto", "New Entity"));
            } },
            { tr("Adicionar Luz do Sol", "Add Directional Light"), [this]() {
                if (m_editorScene) {
                    Entity e = m_editorScene->create_entity(tr("Luz do Sol", "Directional Light"));
                    m_editorScene->lightComponents[e.get_id()] = LightComponent{};
                    m_selectedEntity = e;
                }
            } },
            { tr("Testar Jogo / Parar", "Play / Stop"), [this]() {
                if (m_playMode.get_state() == PlayState::Edit) {
                    m_playMode.start_play(m_editorScene.get());
                    setup_play_runtime();
                } else {
                    teardown_play_runtime();
                    m_playMode.stop_play();
                    m_playMode.set_editor_scene(m_editorScene.get());
                    m_selectedEntity = Entity();
                    m_editorGui.select_entity(m_selectedEntity);
                }
            } },
            { tr("Abrir Guia de Uso", "Open How-to-Use Guide"), [this]() { m_wickedTools.showGuideWindow = true; } },
            { tr("Abrir Painel de Desenvolvimento", "Open Developer Panel"), [this]() { m_wickedTools.showDevWindow = true; } },
            { tr("Desfazer", "Undo"), [this]() { m_undo.undo(); mark_scene_dirty(); } },
            { tr("Refazer", "Redo"), [this]() { m_undo.redo(); mark_scene_dirty(); } },
            { tr("Alternar Viewport", "Toggle Viewport"), [this]() { m_showViewport = !m_showViewport; } },
            { tr("Alternar Cena", "Toggle Scene"), [this]() { m_showHierarchy = !m_showHierarchy; } },
            { tr("Alternar Inspector", "Toggle Inspector"), [this]() { m_showInspector = !m_showInspector; } },
            { tr("Alternar Assets", "Toggle Assets"), [this]() { m_showContentBrowser = !m_showContentBrowser; } },
            { tr("Alternar Console", "Toggle Console"), [this]() { m_showConsole = !m_showConsole; } },
            { tr("Alternar Grid", "Toggle Grid"), [this]() { m_showGrid = !m_showGrid; } },
            { tr("Alternar Gizmos", "Toggle Gizmos"), [this]() { m_showGizmos = !m_showGizmos; } },
            { tr("Alternar Colliders", "Toggle Colliders"), [this]() { m_showColliders = !m_showColliders; } },
            { tr("Idioma: PT / EN", "Language: PT / EN"), [this]() {
                m_currentLanguage = (m_currentLanguage == EngineLanguage::PT_BR) ? EngineLanguage::EN_US : EngineLanguage::PT_BR;
            } },
            { tr("Configurações", "Settings"), [this]() { m_wickedTools.showGeneralWindow = !m_wickedTools.showGeneralWindow; } },
            { tr("Sobre a Engine", "About"), [this]() { m_showAboutDialog = true; } },
        };

        int shown = 0;
        PaletteCmd firstMatch{ nullptr, nullptr };
        for (const auto& c : cmds) {
            std::string label = c.label;
            std::transform(label.begin(), label.end(), label.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (label.find(query) == std::string::npos) continue;
            if (!firstMatch.run) firstMatch = c;
            if (shown < 8) {
                if (ImGui::Selectable(c.label)) {
                    c.run();
                    search[0] = '\0';
                }
                ++shown;
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("%s", tr("Nenhum comando encontrado", "No matching commands"));
        }
        if (firstMatch.run && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
            firstMatch.run();
            search[0] = '\0';
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            search[0] = '\0';
        }

        ImGui::End();
    }

    ImGui::End();
}

void EditorApplication::draw_hierarchy_panel() {
    // Local minimum only (see draw_app_bar note about the global style).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(240.0f, 180.0f));
    ImGui::Begin(tr("Cena", "Scene"));
    ImGui::PopStyleVar();

    // Search + Add row: a real-time filter and the full entity creation menu.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 52.0f);
    ImGui::InputTextWithHint("##SceneSearch", ICON_FA_MAGNIFYING_GLASS "  Buscar na cena...", m_hierarchySearch, sizeof(m_hierarchySearch));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS, ImVec2(40, 0))) ImGui::OpenPopup("##AddEntityMenu");

    const auto createSel = [this](const std::string& name) -> Entity {
        if (!m_editorScene) return Entity();
        Entity e = m_editorScene->create_entity(name);
        m_selectedEntity = e;
        mark_scene_dirty();
        return e;
    };
    if (ImGui::BeginPopup("##AddEntityMenu")) {
        ImGui::TextDisabled("%s", tr("BÁSICO", "BASIC"));
        if (ImGui::MenuItem(tr("Objeto Vazio", "Empty Object"))) createSel(tr("Novo Objeto", "New Entity"));
        if (ImGui::MenuItem(tr("Cubo 3D", "Cube"))) {
            Entity e = createSel(tr("Cubo 3D", "Cube"));
            if (e.is_valid()) m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{};
        }
        if (ImGui::MenuItem(tr("Câmera", "Camera"))) {
            Entity e = createSel(tr("Câmera", "Camera"));
            if (e.is_valid()) m_editorScene->cameraComponents[e.get_id()] = CameraComponent{};
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("ILUMINAÇÃO", "LIGHTING"));
        if (ImGui::MenuItem(tr("Luz do Sol", "Directional Light"))) {
            Entity e = createSel(tr("Luz do Sol", "Directional Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{};
        }
        if (ImGui::MenuItem(tr("Luz de Lâmpada", "Point Light"))) {
            Entity e = createSel(tr("Luz de Lâmpada", "Point Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true };
        }
        if (ImGui::MenuItem(tr("Luz Spot", "Spot Light"))) {
            Entity e = createSel(tr("Luz Spot", "Spot Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
        }
        if (ImGui::MenuItem(tr("Luz de Área", "Area Light"))) {
            Entity e = createSel(tr("Luz de Área", "Area Light"));
            if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("EFEITOS", "EFFECTS"));
        if (ImGui::MenuItem(tr("Emissor de Partículas", "Particle Emitter"))) {
            Entity e = createSel(tr("Emissor de Partículas", "Particle Emitter"));
            if (e.is_valid()) m_editorScene->particleEmitterComponents[e.get_id()] = ParticleEmitterComponent{};
        }
        if (ImGui::MenuItem(tr("Fonte de Áudio", "Audio Source"))) {
            Entity e = createSel(tr("Fonte de Áudio", "Audio Source"));
            if (e.is_valid()) m_editorScene->audioComponents[e.get_id()] = AudioComponent{};
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("FÍSICA / GAMEPLAY", "PHYSICS / GAMEPLAY"));
        if (ImGui::MenuItem(tr("Corpo Rígido", "Rigidbody Object"))) {
            Entity e = createSel(tr("Corpo Rígido", "Rigidbody Object"));
            if (e.is_valid()) m_editorScene->rigidbodyComponents[e.get_id()] = RigidbodyComponent{};
        }
        if (ImGui::MenuItem(tr("Veículo", "Vehicle"))) {
            Entity e = createSel(tr("Veículo", "Vehicle"));
            if (e.is_valid()) m_editorScene->vehicleComponents[e.get_id()] = VehicleComponent{};
        }
        if (ImGui::MenuItem(tr("Destrutível", "Destructible"))) {
            Entity e = createSel(tr("Destrutível", "Destructible"));
            if (e.is_valid()) m_editorScene->destructionComponents[e.get_id()] = DestructionComponent{};
        }
        if (ImGui::MenuItem(tr("Agente de Navegação", "Navigation Agent"))) {
            Entity e = createSel(tr("Agente de Navegação", "Navigation Agent"));
            if (e.is_valid()) m_editorScene->navigationComponents[e.get_id()] = NavigationComponent{};
        }
        if (ImGui::MenuItem(tr("Missão", "Mission"))) {
            Entity e = createSel(tr("Missão", "Mission"));
            if (e.is_valid()) m_editorScene->missionComponents[e.get_id()] = MissionComponent{};
        }
        if (ImGui::MenuItem(tr("Diálogo", "Dialogue"))) {
            Entity e = createSel(tr("Diálogo", "Dialogue"));
            if (e.is_valid()) m_editorScene->dialogueComponents[e.get_id()] = DialogueComponent{};
        }
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("MUNDO", "WORLD"));
        if (ImGui::MenuItem(tr("Mundo de Blocos", "Voxel Volume"))) {
            Entity e = createSel(tr("Mundo de Blocos", "Voxel Volume"));
            if (e.is_valid()) m_editorScene->voxelVolumeComponents[e.get_id()] = VoxelVolumeComponent{};
        }
#endif
        ImGui::EndPopup();
    }
    ImGui::Separator();

    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    if (!scene) {
        ImGui::TextDisabled("%s", tr("Nenhuma cena aberta", "No scene open"));
        ImGui::End();
        return;
    }

    const std::string filter = m_hierarchySearch;
    static const char* kEntityDrag = "VC_ENTITY";

    // Recursive node renderer: real parent/child tree (roots first), with
    // drag-to-reparent (cycle-safe) and a delete context menu.
    std::function<void(UUID)> drawNode = [&](UUID id) {
        const Entity* ent = scene->find_entity_by_id_const(id);
        if (!ent) return;
        const std::vector<UUID> children = scene->get_children(id);
        const bool hasChildren = !children.empty();

        ImGuiTreeNodeFlags flags = ((m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) ? ImGuiTreeNodeFlags_Selected : 0) |
                                   ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   (hasChildren ? 0 : (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen));

        ImVec4 iconColor = ImVec4(0.60f, 0.60f, 0.68f, 1.0f);
        if (scene->cameraComponents.contains(id)) iconColor = ImVec4(0.37f, 0.64f, 0.98f, 1.0f);
        else if (scene->lightComponents.contains(id)) iconColor = ImVec4(0.98f, 0.75f, 0.14f, 1.0f);
        else if (scene->voxelVolumeComponents.contains(id)) iconColor = ImVec4(0.20f, 0.82f, 0.60f, 1.0f);
        else if (scene->meshRendererComponents.contains(id)) iconColor = ImVec4(0.45f, 0.55f, 0.85f, 1.0f);
        else if (scene->particleEmitterComponents.contains(id)) iconColor = ImVec4(0.95f, 0.45f, 0.25f, 1.0f);

        ImGui::TextColored(iconColor, "%s", UI::entityIcon(scene, id));
        ImGui::SameLine();

        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(id.get_high() ^ id.get_low()),
                                            flags, "%s", ent->get_name().c_str());
        if (ImGui::IsItemClicked()) {
            m_selectedEntity = *ent;
        }

        // Drag source: pick this entity up to reparent it.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            ImGui::SetDragDropPayload(kEntityDrag, &id, sizeof(UUID));
            ImGui::TextUnformatted(ent->get_name().c_str());
            ImGui::EndDragDropSource();
        }
        // Drop target: drop another entity here to make it a child.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDrag)) {
                UUID dragged;
                std::memcpy(&dragged, payload->Data, sizeof(UUID));
                if (dragged != id) {
                    scene->set_parent(dragged, id); // cycle-safe
                    mark_scene_dirty();
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(tr("Deletar Objeto", "Delete Entity"))) {
                scene->destroy_entity(id);
                if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) m_selectedEntity = Entity();
                mark_scene_dirty();
            }
            ImGui::EndPopup();
        }

        if (hasChildren && open) {
            for (const UUID& child : children) drawNode(child);
            ImGui::TreePop();
        }
    };

    if (filter.empty()) {
        // Real hierarchy: roots first, then their children recursively.
        for (const auto& [id, entity] : scene->get_entities()) {
            (void)entity;
            if (!scene->get_parent(id).is_valid()) drawNode(id);
        }
    } else {
        // Search mode: flat list of matches (a parent may not match the query).
        for (const auto& [id, entity] : scene->get_entities()) {
            if (entity.get_name().find(filter) != std::string::npos) drawNode(id);
        }
    }

    ImGui::End();
}

void EditorApplication::draw_inspector_panel() {
    // Local minimum only (see draw_app_bar note about the global style).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(300.0f, 200.0f));
    ImGui::Begin(tr("Inspector", "Inspector"));
    ImGui::PopStyleVar();

    if (!m_selectedEntity.is_valid()) {
        ImGui::TextDisabled("%s", tr("Nenhum objeto selecionado", "No Object Selected"));
        ImGui::End();
        return;
    }

    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    if (!scene) {
        ImGui::End();
        return;
    }

    UUID id = m_selectedEntity.get_id();

    // Entity header: name + advanced-mode toggle (Forge design). The UUID and
    // technical fields are hidden unless advanced mode is on.
    char nameBuf[256];
    strncpy(nameBuf, m_selectedEntity.get_name().c_str(), sizeof(nameBuf));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
    if (ImGui::InputText("##EntityName", nameBuf, sizeof(nameBuf))) {
        m_selectedEntity.set_name(nameBuf);
    }
    ImGui::SameLine();
    UI::toggle("##AdvancedToggle", &m_advancedInspector,
               tr("Mostrar propriedades avançadas", "Show advanced properties"));
    if (m_advancedInspector) {
        ImGui::TextDisabled("Código Único: %s", id.to_string().c_str());
    }
    ImGui::Separator();

    // Transform Component — card with collapse + vec3 rows.
    if (scene->transformComponents.contains(id)) {
        if (UI::sectionHeader(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, tr("Transform", "Transform"))) {
            auto& t = scene->transformComponents[id];
            UI::vec3Property(tr("Posição", "Position"), &t.position.x, 0.1f);
            UI::vec3Property(tr("Rotação", "Rotation"), &t.rotation.x, 1.0f);
            UI::vec3Property(tr("Escala", "Scale"), &t.scale.x, 0.1f);
            ImGui::Spacing();
        }
    }

    // Semantic sections (Inspector): components are grouped by role —
    // Appearance / Physics / Gameplay / Effects & World. Each group header is
    // emitted once, before the first component of that group that exists.
    bool inspectorGroupEmitted[5] = { false, false, false, false, false };
    const auto beginInspectorGroup = [&](int group, const char* title) {
        if (inspectorGroupEmitted[group]) return;
        inspectorGroupEmitted[group] = true;
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Colors::Accent);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
    };

    // Mesh Renderer Component
    if (scene->meshRendererComponents.contains(id)) {
        beginInspectorGroup(0, tr("APARÊNCIA", "APPEARANCE"));
        UI::sectionHeader(ICON_FA_CUBE, tr("Malha", "Mesh Renderer"));
        auto& mr = scene->meshRendererComponents[id];
        // Mesh asset picker (from the project asset registry).
        std::vector<std::pair<UUID, std::string>> meshAssets;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type == AssetType::Mesh) {
                meshAssets.emplace_back(asset.id, asset.sourcePath.filename().string());
            }
        }
        const std::string noneLabel = tr("(Nenhuma malha)", "(None)");
        const char* currentName = noneLabel.c_str();
        int currentIndex = -1;
        for (size_t i = 0; i < meshAssets.size(); ++i) {
            if (meshAssets[i].first == mr.meshAssetID) {
                currentIndex = static_cast<int>(i);
                currentName = meshAssets[i].second.c_str();
                break;
            }
        }
        if (ImGui::BeginCombo(tr("Malha 3D", "Mesh"), currentName)) {
            if (ImGui::Selectable(noneLabel.c_str(), currentIndex < 0)) {
                mr.meshAssetID = UUID();
                m_meshLoadFailed.erase(UUID());
            }
            for (size_t i = 0; i < meshAssets.size(); ++i) {
                if (ImGui::Selectable(meshAssets[i].second.c_str(), currentIndex == static_cast<int>(i))) {
                    mr.meshAssetID = meshAssets[i].first;
                }
            }
            ImGui::EndCombo();
        }
        // Material asset picker: rendered on the mesh via a material-graph pipeline.
        std::vector<std::pair<UUID, std::string>> materialAssets;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type == AssetType::Material) {
                materialAssets.emplace_back(asset.id, asset.sourcePath.filename().string());
            }
        }
        const std::string matNoneLabel = tr("(Padrão)", "(Default)");
        const char* matCurrentName = matNoneLabel.c_str();
        int matCurrentIndex = -1;
        for (size_t i = 0; i < materialAssets.size(); ++i) {
            if (materialAssets[i].first == mr.materialAssetID) {
                matCurrentIndex = static_cast<int>(i);
                matCurrentName = materialAssets[i].second.c_str();
                break;
            }
        }
        if (ImGui::BeginCombo(tr("Material", "Material"), matCurrentName)) {
            if (ImGui::Selectable(matNoneLabel.c_str(), matCurrentIndex < 0)) {
                mr.materialAssetID = UUID();
                m_materialLoadFailed.erase(UUID());
            }
            for (size_t i = 0; i < materialAssets.size(); ++i) {
                if (ImGui::Selectable(materialAssets[i].second.c_str(), matCurrentIndex == static_cast<int>(i))) {
                    mr.materialAssetID = materialAssets[i].first;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox(tr("Visível", "Visible"), &mr.isVisible);
        ImGui::Checkbox(tr("Projetar Sombras", "Cast Shadows"), &mr.castShadows);
        ImGui::Spacing();
    }

    // Rigidbody Component
    if (scene->rigidbodyComponents.contains(id)) {
        beginInspectorGroup(1, tr("FÍSICA", "PHYSICS"));
        UI::sectionHeader(ICON_FA_WEIGHT_HANGING, tr("Física", "Rigidbody"));
        auto& r = scene->rigidbodyComponents[id];
        ImGui::DragFloat(tr("Peso (kg)", "Mass (kg)"), &r.mass, 0.5f, 0.01f, 10000.0f);
        ImGui::SliderFloat(tr("Deslize (Fricção)", "Friction"), &r.friction, 0.0f, 1.0f);
        ImGui::SliderFloat(tr("Quique (Elasticidade)", "Restitution"), &r.restitution, 0.0f, 1.0f);
        ImGui::Checkbox(tr("Física Fixa (Sem Mover)", "Is Kinematic"), &r.isKinematic);
        ImGui::Checkbox(tr("Ativar Gravidade", "Use Gravity"), &r.useGravity);
        ImGui::Spacing();
    }

    // Destruction Component (a destructible of chunkCount boxes; weapon hits
    // within damageRadius detach chunks in play).
    if (scene->destructionComponents.contains(id)) {
        beginInspectorGroup(1, tr("FÍSICA", "PHYSICS"));
        UI::sectionHeader(ICON_FA_EXPLOSION, tr("Destrutível", "Destruction"));
        auto& ds = scene->destructionComponents[id];
        ImGui::DragFloat3(tr("Tamanho do pedaço", "Chunk Size"), &ds.chunkSize.x, 0.05f, 0.05f, 10.0f);
        ImGui::DragInt(tr("Nº de pedaços", "Chunk Count"), reinterpret_cast<int*>(&ds.chunkCount), 1, 1, 1000);
        ImGui::DragFloat(tr("Vida do pedaço", "Chunk Health"), &ds.chunkHealth, 1.0f, 1.0f, 100000.0f);
        ImGui::DragFloat(tr("Raio de dano", "Damage Radius"), &ds.damageRadius, 0.1f, 0.1f, 100.0f);
        ImGui::DragFloat(tr("Impulso do dano", "Damage Impulse"), &ds.damageImpulse, 0.5f, 0.0f, 1000.0f);
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &ds.enabled);
        if (ImGui::Button(tr("Remover Destrutível", "Remove Destruction"))) scene->destructionComponents.erase(id);
        ImGui::Spacing();
    }

    // Weapon Component (authored in the Weapon panel; the play world fires it).
    if (scene->weaponComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_GUN, tr("Arma", "Weapon"));
        auto& w = scene->weaponComponents[id];
        ImGui::DragFloat(tr("Dano", "Damage"), &w.damage, 0.5f, 0.0f, 10000.0f);
        ImGui::DragFloat(tr("Tiros/min", "Rounds Per Minute"), &w.roundsPerMinute, 5.0f, 1.0f, 5000.0f);
        ImGui::DragInt(tr("Pente", "Magazine Size"), reinterpret_cast<int*>(&w.magazineSize), 1, 1, 1000);
        ImGui::DragInt(tr("Reserva", "Reserve Ammo"), reinterpret_cast<int*>(&w.reserveAmmo), 1, 0, 10000);
        ImGui::Checkbox(tr("Automática", "Automatic"), &w.automatic);
        ImGui::SliderFloat(tr("Espalhamento (graus)", "Spread (degrees)"), &w.spreadDegrees, 0.0f, 20.0f);
        ImGui::Checkbox(tr("Hitscan", "Hitscan"), &w.hitscan);
        if (ImGui::Button(tr("Remover Arma", "Remove Weapon"))) scene->weaponComponents.erase(id);
        ImGui::Spacing();
    }

    // Vehicle Component (authored in the Vehicle panel; the play world builds
    // a chassis body + four wheels and drives it with a VehicleRuntime).
    if (scene->vehicleComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_CAR, tr("Veículo", "Vehicle"));
        auto& v = scene->vehicleComponents[id];
        ImGui::DragFloat(tr("Potência do motor", "Engine Power"), &v.enginePower, 100.0f, 0.0f, 100000.0f);
        ImGui::SliderFloat(tr("Ângulo máx. de direção (rad)", "Max Steer Angle"), &v.maxSteerAngle, 0.0f, 1.2f);
        ImGui::DragFloat(tr("Força de freio", "Brake Force"), &v.brakeForce, 100.0f, 0.0f, 100000.0f);
        ImGui::DragFloat(tr("Raio da roda", "Wheel Radius"), &v.wheelRadius, 0.01f, 0.05f, 2.0f);
        ImGui::DragFloat(tr("Suspensão (descanso)", "Suspension Rest"), &v.suspensionRest, 0.01f, 0.0f, 2.0f);
        ImGui::DragFloat(tr("Distância entre eixos", "Wheel Base"), &v.wheelBase, 0.05f, 0.5f, 20.0f);
        ImGui::DragFloat(tr("Bitola (largura)", "Track Width"), &v.trackWidth, 0.05f, 0.2f, 10.0f);
        ImGui::DragFloat(tr("Massa", "Mass"), &v.mass, 50.0f, 10.0f, 20000.0f);
        ImGui::Checkbox(tr("Tração dianteira", "Front Wheel Drive"), &v.frontWheelDrive);
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &v.enabled);
        if (ImGui::Button(tr("Remover Veículo", "Remove Vehicle"))) scene->vehicleComponents.erase(id);
        ImGui::Spacing();
    }

    // Ragdoll Component (authored in the Ragdoll panel; the play world builds
    // physics bodies per bone from the skin skeleton when fromSkeleton is set).
    if (scene->ragdollComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_USER, tr("Ragdoll", "Ragdoll"));
        auto& rg = scene->ragdollComponents[id];
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &rg.enabled);
        ImGui::SliderFloat(tr("Blend da física", "Physics Blend"), &rg.blendWeight, 0.0f, 1.0f);
        ImGui::Checkbox(tr("Da esqueleto (skin)", "From skeleton (skin)"), &rg.fromSkeleton);
        ImGui::DragFloat(tr("Massa por osso", "Mass per bone"), &rg.massPerBone, 0.1f, 0.1f, 100.0f);
        ImGui::DragFloat3(tr("Deslocamento de spawn", "Spawn Offset"), &rg.spawnOffset.x, 0.1f);
        if (ImGui::Button(tr("Remover Ragdoll", "Remove Ragdoll"))) scene->ragdollComponents.erase(id);
        ImGui::Spacing();
    }

    // Animation Component (authored in the Animation editor; the play world
    // samples the entry state's clip onto the bone entities under this one).
    if (scene->animationComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_FILM, tr("Animação", "Animation"));
        auto& an = scene->animationComponents[id];
        ImGui::Checkbox(tr("Tocando", "Playing"), &an.playing);
        char entryBuf[64];
        std::snprintf(entryBuf, sizeof(entryBuf), "%s", an.entryState.c_str());
        if (ImGui::InputText(tr("Estado de entrada", "Entry State"), entryBuf, sizeof(entryBuf))) an.entryState = entryBuf;
        ImGui::TextDisabled("%zu %s", an.states.size(), tr("estados", "states"));
        for (const auto& s : an.states) {
            ImGui::BulletText("%s (clip %s, x%.2f)", s.id.c_str(), s.clip.to_string().c_str(), s.speed);
        }
        if (ImGui::Button(tr("Remover Animação", "Remove Animation"))) scene->animationComponents.erase(id);
        ImGui::Spacing();
    }

    // Timeline Component (authored in the Timeline editor; the play world
    // animates the entity's transform from Property tracks).
    if (scene->timelineComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_CLOCK, tr("Timeline", "Timeline"));
        auto& tl = scene->timelineComponents[id];
        ImGui::DragFloat(tr("Duração", "Duration"), &tl.duration, 0.1f, 0.01f, 10000.0f);
        ImGui::SliderFloat(tr("Playhead", "Playhead"), &tl.playhead, 0.0f, std::max(tl.duration, 0.01f));
        ImGui::Checkbox(tr("Loop", "Loop"), &tl.loop);
        ImGui::TextDisabled("%zu %s", tl.tracks.size(), tr("trilhas", "tracks"));
        for (const auto& t : tl.tracks) ImGui::BulletText("%s (%zu %s)", t.name.c_str(), t.keys.size(), tr("chaves", "keys"));
        if (ImGui::Button(tr("Remover Timeline", "Remove Timeline"))) scene->timelineComponents.erase(id);
        ImGui::Spacing();
    }

    // IK Component (authored in the IK editor; the play world bends the
    // chain root -> mid -> end so the end entity reaches the target entity).
    if (scene->ikComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_BONE, tr("IK", "IK"));
        auto& ik = scene->ikComponents[id];
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &ik.enabled);
        ImGui::SliderFloat(tr("Peso", "Weight"), &ik.weight, 0.0f, 1.0f);
        ImGui::DragInt(tr("Iterações", "Iterations"), &ik.iterations, 1, 1, 64);
        ImGui::TextDisabled("root=%s mid=%s end=%s target=%s", ik.rootEntity.to_string().c_str(),
                            ik.midEntity.to_string().c_str(), ik.endEntity.to_string().c_str(),
                            ik.targetEntity.to_string().c_str());
        if (ImGui::Button(tr("Remover IK", "Remove IK"))) scene->ikComponents.erase(id);
        ImGui::Spacing();
    }

    // Retarget Component (authored in the Retarget editor; the play world
    // copies mapped source-bone transforms onto the target-bone entities).
    if (scene->retargetComponents.contains(id)) {
        beginInspectorGroup(4, tr("ANIMAÇÃO", "ANIMATION"));
        UI::sectionHeader(ICON_FA_SHUFFLE, tr("Retarget", "Retarget"));
        auto& rt = scene->retargetComponents[id];
        ImGui::Checkbox(tr("Preservar root motion", "Preserve Root Motion"), &rt.preserveRootMotion);
        ImGui::TextDisabled("%zu %s", rt.mapping.size(), tr("mapeamentos", "mappings"));
        for (const auto& m : rt.mapping) ImGui::BulletText("%s -> %s", m.sourceBone.c_str(), m.targetBone.c_str());
        if (ImGui::Button(tr("Remover Retarget", "Remove Retarget"))) scene->retargetComponents.erase(id);
        ImGui::Spacing();
    }

    // Mission Component (the play world registers a Mission that the
    // completeEvent — a script EmitEvent — finishes).
    if (scene->missionComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_FLAG, tr("Missão", "Mission"));
        auto& m = scene->missionComponents[id];
        char missionBuf[128]; std::snprintf(missionBuf, sizeof(missionBuf), "%s", m.missionId.c_str());
        if (ImGui::InputText(tr("ID", "ID"), missionBuf, sizeof(missionBuf))) m.missionId = missionBuf;
        char objBuf[256]; std::snprintf(objBuf, sizeof(objBuf), "%s", m.objectiveText.c_str());
        if (ImGui::InputText(tr("Objetivo", "Objective"), objBuf, sizeof(objBuf))) m.objectiveText = objBuf;
        ImGui::DragInt(tr("Alvo", "Target"), reinterpret_cast<int*>(&m.objectiveTarget), 1, 1, 100000);
        char evBuf[128]; std::snprintf(evBuf, sizeof(evBuf), "%s", m.completeEvent.c_str());
        if (ImGui::InputText(tr("Evento de conclusão", "Complete Event"), evBuf, sizeof(evBuf))) m.completeEvent = evBuf;
        ImGui::Checkbox(tr("Início automático", "Auto Start"), &m.autoStart);
        ImGui::TextDisabled("%s: %s", tr("Estado", "State"), m.active ? tr("ativa", "active") : tr("inativa", "inactive"));
        if (ImGui::Button(tr("Remover Missão", "Remove Mission"))) scene->missionComponents.erase(id);
        ImGui::Spacing();
    }

    // Dialogue Component (a one-node graph with a line and one choice).
    if (scene->dialogueComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_COMMENT, tr("Diálogo", "Dialogue"));
        auto& d = scene->dialogueComponents[id];
        char dgBuf[128]; std::snprintf(dgBuf, sizeof(dgBuf), "%s", d.dialogueId.c_str());
        if (ImGui::InputText("ID", dgBuf, sizeof(dgBuf))) d.dialogueId = dgBuf;
        char chBuf[128]; std::snprintf(chBuf, sizeof(chBuf), "%s", d.character.c_str());
        if (ImGui::InputText(tr("Personagem", "Character"), chBuf, sizeof(chBuf))) d.character = chBuf;
        char lineBuf[256]; std::snprintf(lineBuf, sizeof(lineBuf), "%s", d.line.c_str());
        if (ImGui::InputText(tr("Fala", "Line"), lineBuf, sizeof(lineBuf))) d.line = lineBuf;
        char choiceBuf[128]; std::snprintf(choiceBuf, sizeof(choiceBuf), "%s", d.choiceText.c_str());
        if (ImGui::InputText(tr("Escolha", "Choice"), choiceBuf, sizeof(choiceBuf))) d.choiceText = choiceBuf;
        char nextBuf[128]; std::snprintf(nextBuf, sizeof(nextBuf), "%s", d.nextDialogueId.c_str());
        if (ImGui::InputText(tr("Próximo diálogo", "Next Dialogue"), nextBuf, sizeof(nextBuf))) d.nextDialogueId = nextBuf;
        ImGui::Checkbox(tr("Tocar ao iniciar", "Play On Start"), &d.playOnStart);
        if (ImGui::Button(tr("Remover Diálogo", "Remove Dialogue"))) scene->dialogueComponents.erase(id);
        ImGui::Spacing();
    }

    // Navigation Component (a baked grid + an agent toward the camera).
    if (scene->navigationComponents.contains(id)) {
        beginInspectorGroup(2, tr("JOGABILIDADE", "GAMEPLAY"));
        UI::sectionHeader(ICON_FA_LOCATION_CROSSHAIRS, tr("Navegação", "Navigation"));
        auto& nav = scene->navigationComponents[id];
        ImGui::DragInt(tr("Largura do grid", "Grid Width"), &nav.gridWidth, 1, 4, 512);
        ImGui::DragInt(tr("Altura do grid", "Grid Height"), &nav.gridHeight, 1, 4, 512);
        ImGui::DragFloat(tr("Tamanho da célula", "Cell Size"), &nav.cellSize, 0.1f, 0.1f, 20.0f);
        ImGui::DragFloat(tr("Velocidade do agente", "Agent Speed"), &nav.agentSpeed, 0.1f, 0.1f, 50.0f);
        ImGui::Checkbox(tr("Habilitado", "Enabled"), &nav.enabled);
        if (ImGui::Button(tr("Remover Navegação", "Remove Navigation"))) scene->navigationComponents.erase(id);
        ImGui::Spacing();
    }

    // Audio Component (an OGG source played through the play Mixer).
    if (scene->audioComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_VOLUME_HIGH, tr("Áudio", "Audio"));
        auto& au = scene->audioComponents[id];
        char clipBuf[256]; std::snprintf(clipBuf, sizeof(clipBuf), "%s", au.clipPath.c_str());
        if (ImGui::InputText(".ogg", clipBuf, sizeof(clipBuf))) au.clipPath = clipBuf;
        ImGui::DragFloat(tr("Volume", "Volume"), &au.volume, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat(tr("Pitch", "Pitch"), &au.pitch, 0.01f, 0.1f, 4.0f);
        ImGui::Checkbox(tr("Espacial", "Spatial"), &au.spatial);
        ImGui::SameLine();
        ImGui::Checkbox(tr("Em loop", "Looping"), &au.looping);
        ImGui::Checkbox(tr("Tocar ao iniciar", "Play On Start"), &au.playOnStart);        if (ImGui::Button(tr("Remover Áudio", "Remove Audio"))) scene->audioComponents.erase(id);
        ImGui::Spacing();
    }

    // Light Component
    if (scene->lightComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_SUN, tr("Luz", "Light"));
        auto& l = scene->lightComponents[id];
        ImGui::ColorEdit3(tr("Cor da Luz", "Light Color"), &l.color.r);
        ImGui::DragFloat(tr("Brilho (Intensidade)", "Intensity"), &l.intensity, 100.0f, 0.0f, 100000.0f);
        ImGui::DragFloat(tr("Alcance da Luz", "Range"), &l.range, 0.5f, 0.1f, 1000.0f);
        ImGui::Checkbox(tr("Projetar Sombras", "Cast Shadows"), &l.castShadows);
        ImGui::Spacing();
    }

    // Camera Component — near/far planes are advanced-only fields.
    if (scene->cameraComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_CAMERA, tr("Câmera", "Camera"));
        auto& c = scene->cameraComponents[id];
        ImGui::SliderFloat(tr("Campo de Visão (FOV)", "Field of View (FOV)"), &c.fov, 10.0f, 160.0f);
        if (m_advancedInspector) {
            ImGui::DragFloat(tr("Visão Próxima", "Near Plane"), &c.nearPlane, 0.01f, 0.001f, 10.0f);
            ImGui::DragFloat(tr("Visão Distante", "Far Plane"), &c.farPlane, 100.0f, 10.0f, 100000.0f);
        }
        ImGui::Checkbox(tr("Câmera Principal do Jogo", "Primary Camera"), &c.isPrimary);
        ImGui::Spacing();
    }

    // Particle Emitter Component (authored in the Particle panel; the play
    // world instantiates a ParticleSimulation emitter at the entity origin).
    if (scene->particleEmitterComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        UI::sectionHeader(ICON_FA_FIRE, tr("Partículas", "Particle Emitter"));
        auto& p = scene->particleEmitterComponents[id];
        ImGui::DragFloat3(tr("Posição (local)", "Position (local)"), &p.position.x, 0.05f);
        ImGui::DragFloat3(tr("Direção", "Direction"), &p.direction.x, 0.05f);
        ImGui::SliderFloat(tr("Cone (rad)", "Cone (rad)"), &p.coneAngle, 0.0f, 1.5f);
        ImGui::DragFloat(tr("Taxa (part/s)", "Rate (part/s)"), &p.rate, 1.0f, 0.0f, 10000.0f);
        ImGui::DragFloat(tr("Vel. min", "Speed Min"), &p.speedMin, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat(tr("Vel. máx", "Speed Max"), &p.speedMax, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat(tr("Vida min (s)", "Lifetime Min"), &p.lifetimeMin, 0.05f, 0.01f, 60.0f);
        ImGui::DragFloat(tr("Vida máx (s)", "Lifetime Max"), &p.lifetimeMax, 0.05f, 0.01f, 60.0f);
        ImGui::DragFloat(tr("Tamanho inicial", "Size Start"), &p.sizeStart, 0.01f, 0.0f, 10.0f);
        ImGui::DragFloat(tr("Tamanho final", "Size End"), &p.sizeEnd, 0.01f, 0.0f, 10.0f);
        ImGui::ColorEdit4(tr("Cor inicial", "Start Color"), &p.colorStart.x);
        ImGui::ColorEdit4(tr("Cor final", "End Color"), &p.colorEnd.x);
        ImGui::DragFloat3(tr("Aceleração", "Acceleration"), &p.acceleration.x, 0.1f);
        ImGui::DragFloat(tr("Arrasto", "Drag"), &p.drag, 0.01f, 0.0f, 1.0f);
        ImGui::DragInt(tr("Rajada no início", "Burst on start"), reinterpret_cast<int*>(&p.burstCount), 1, 0, 100000);
        ImGui::Checkbox(tr("Colide com física", "Collides with physics"), &p.collide);
        ImGui::Checkbox(tr("Emitindo", "Emitting"), &p.emitting);
        if (ImGui::Button(tr("Remover Emissor", "Remove Emitter"))) scene->particleEmitterComponents.erase(id);
        ImGui::Spacing();
    }

#if VC_ENABLE_VOXEL_PLUGIN

    // Voxel Volume Component
    if (scene->voxelVolumeComponents.contains(id)) {
        beginInspectorGroup(3, tr("EFEITOS & MUNDO", "EFFECTS & WORLD"));
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("Mundo de Terreno em Blocos", "Voxel Terrain Volume"));
        ImGui::Separator();
        auto& v = scene->voxelVolumeComponents[id];
        ImGui::SliderInt(tr("Distância de Visão (Blocos)", "Chunk Radius"), &v.chunkBudget, 64, 4096);
        ImGui::InputInt(tr("Semente de Geração (Seed)", "Terrain Seed"), &v.seed);
        ImGui::DragFloat(tr("Nível da Água", "Sea Level"), &v.seaLevel, 0.5f, 0.0f, 100.0f);
        ImGui::Checkbox(tr("Carregar Terreno Distante", "Enable Far LOD Clipmap"), &v.enableFarLod);
        ImGui::Spacing();
    }
#endif

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.39f, 0.40f, 0.95f, 1.00f));
    if (ImGui::Button(tr("+ Adicionar Nova Propriedade", "+ Add Component"), ImVec2(240, 32))) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    ImGui::PopStyleColor();

    if (ImGui::BeginPopup("AddComponentPopup")) {
        static char compSearch[64]{};
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##CompSearch", tr("Buscar componentes...", "Search components..."), compSearch, sizeof(compSearch));
        ImGui::Separator();

        const std::string cq = compSearch;
        const auto match = [&](const char* label) {
            if (cq.empty()) return true;
            std::string l = label;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            std::string q = cq;
            std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return l.find(q) != std::string::npos;
        };
        const auto section = [&](const char* title) {
            if (cq.empty()) ImGui::TextDisabled("%s", title);
        };

        section(tr("COMUM", "COMMON"));
        if (match(tr("Iluminação e Luz", "Light"))) { if (ImGui::MenuItem(tr("Iluminação e Luz", "Light Component"))) scene->lightComponents[id] = LightComponent{}; }
        if (match(tr("Câmera de Visão", "Camera"))) { if (ImGui::MenuItem(tr("Câmera de Visão", "Camera Component"))) scene->cameraComponents[id] = CameraComponent{}; }
        if (match(tr("Modelo 3D (Mesh)", "Mesh Renderer"))) { if (ImGui::MenuItem(tr("Modelo 3D (Mesh)", "Mesh Renderer"))) scene->meshRendererComponents[id] = MeshRendererComponent{}; }
        if (match(tr("Material", "Material"))) { if (ImGui::MenuItem(tr("Material", "Material Component"))) scene->materialComponents[id] = MaterialComponent{}; }
        ImGui::Separator();
        section(tr("FÍSICA / GAMEPLAY", "PHYSICS / GAMEPLAY"));
        if (match(tr("Física e Gravidade", "Rigidbody"))) { if (ImGui::MenuItem(tr("Física e Gravidade", "Rigidbody Component"))) scene->rigidbodyComponents[id] = RigidbodyComponent{}; }
        if (match(tr("Arma (Hitscan)", "Weapon"))) { if (ImGui::MenuItem(tr("Arma (Hitscan)", "Weapon Component"))) scene->weaponComponents[id] = WeaponComponent{}; }
        if (match(tr("Veículo", "Vehicle"))) { if (ImGui::MenuItem(tr("Veículo", "Vehicle Component"))) scene->vehicleComponents[id] = VehicleComponent{}; }
        if (match(tr("Ragdoll", "Ragdoll"))) { if (ImGui::MenuItem(tr("Ragdoll", "Ragdoll Component"))) scene->ragdollComponents[id] = RagdollComponent{}; }
        if (match(tr("Destrutível", "Destructible"))) { if (ImGui::MenuItem(tr("Destrutível", "Destruction Component"))) scene->destructionComponents[id] = DestructionComponent{}; }
        if (match(tr("Navegação", "Navigation"))) { if (ImGui::MenuItem(tr("Navegação", "Navigation Component"))) scene->navigationComponents[id] = NavigationComponent{}; }
        ImGui::Separator();
        section(tr("EFEITOS / NARRATIVA", "EFFECTS / NARRATIVE"));
        if (match(tr("Emissor de Partículas", "Particle"))) { if (ImGui::MenuItem(tr("Emissor de Partículas", "Particle Emitter Component"))) scene->particleEmitterComponents[id] = ParticleEmitterComponent{}; }
        if (match(tr("Fonte de Áudio", "Audio"))) { if (ImGui::MenuItem(tr("Fonte de Áudio", "Audio Component"))) scene->audioComponents[id] = AudioComponent{}; }
        if (match(tr("Missão", "Mission"))) { if (ImGui::MenuItem(tr("Missão", "Mission Component"))) scene->missionComponents[id] = MissionComponent{}; }
        if (match(tr("Diálogo", "Dialogue"))) { if (ImGui::MenuItem(tr("Diálogo", "Dialogue Component"))) scene->dialogueComponents[id] = DialogueComponent{}; }
        ImGui::Separator();
        section(tr("ANIMAÇÃO", "ANIMATION"));
        if (match(tr("Máquina de Estados (Animação)", "Animation State Machine"))) { if (ImGui::MenuItem(tr("Máquina de Estados (Animação)", "Animation Component"))) scene->animationComponents[id] = AnimationComponent{}; }
        if (match(tr("Timeline", "Timeline"))) { if (ImGui::MenuItem(tr("Timeline", "Timeline Component"))) scene->timelineComponents[id] = TimelineComponent{}; }
        if (match(tr("IK (Cadeia de Ossos)", "IK Chain"))) { if (ImGui::MenuItem(tr("IK (Cadeia de Ossos)", "IK Component"))) scene->ikComponents[id] = IKComponent{}; }
        if (match(tr("Retargeting de Esqueleto", "Retarget"))) { if (ImGui::MenuItem(tr("Retargeting de Esqueleto", "Retarget Component"))) scene->retargetComponents[id] = RetargetComponent{}; }
#if VC_ENABLE_VOXEL_PLUGIN
        ImGui::Separator();
        section(tr("MUNDO", "WORLD"));
        if (match(tr("Mundo de Blocos", "Voxel"))) { if (ImGui::MenuItem(tr("Mundo de Blocos", "Voxel Terrain Volume"))) scene->voxelVolumeComponents[id] = VoxelVolumeComponent{}; }
#endif
        ImGui::EndPopup();
    }

    // Inspector edits (drag floats, color pickers, combos, the name field,
    // Add-Component popup): any mouse release inside the panel ends an edit —
    // mark the scene dirty so autosave persists it. A spurious save of an
    // unchanged scene is harmless (debounce caps the frequency).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        mark_scene_dirty();
    }

    ImGui::End();
}

void EditorApplication::draw_viewport_panel() {
    // No scrollbar: the viewport must fill the allowed area 1:1. The offscreen
    // target is sized to the image area (content minus the header) so the
    // rendered image exactly matches the panel — nothing overflows.
    ImGui::Begin(tr("Viewport", "Viewport"), nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 panelSize = ImGui::GetContentRegionAvail();

    // Viewport toolbar: gizmo mode (Move/Rotate/Scale), the camera label and
    // the live play-mode badge. Responsive: secondary info drops first as the
    // panel narrows (panel size → camera label); the gizmo buttons and the
    // play badge always remain (badge = status, buttons = actions).
    const float toolbarWidth = panelSize.x;
    // Select / Move / Rotate / Scale (Q/W/E/R). Select hides the gizmo and
    // only picks entities.
    if (UI::iconButton(ICON_FA_HAND_POINTER, tr("Selecionar (Q)", "Select (Q)"),
                       m_gizmoMode == GizmoMode::Select)) m_gizmoMode = GizmoMode::Select;
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, tr("Mover (W)", "Move (W)"),
                       m_gizmoMode == GizmoMode::Translate)) m_gizmoMode = GizmoMode::Translate;
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_ROTATE, tr("Rotar (E)", "Rotate (E)"),
                       m_gizmoMode == GizmoMode::Rotate)) m_gizmoMode = GizmoMode::Rotate;
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, tr("Escalar (R)", "Scale (R)"),
                       m_gizmoMode == GizmoMode::Scale)) m_gizmoMode = GizmoMode::Scale;
    if (toolbarWidth > 420.0f) {
        // World/Local gizmo space: Local rotates the drag axes with the entity.
        ImGui::SameLine();
        if (UI::iconButton(m_gizmoLocal ? ICON_FA_CUBE : ICON_FA_GLOBE,
                           m_gizmoLocal ? tr("Local", "Local") : tr("Mundo", "World"),
                           m_gizmoLocal)) {
            m_gizmoLocal = !m_gizmoLocal;
        }
        // Snap step used by Ctrl-drag (translate); 0 disables snapping.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        const char* snapLabels[] = { "Snap 0", "Snap 0.1", "Snap 0.5", "Snap 1", "Snap 2", "Snap 5" };
        const float snapValues[] = { 0.0f, 0.1f, 0.5f, 1.0f, 2.0f, 5.0f };
        int snapIdx = 2;
        for (int i = 0; i < IM_ARRAYSIZE(snapValues); ++i) {
            if (std::abs(m_snapTranslate - snapValues[i]) < 1e-4f) snapIdx = i;
        }
        if (ImGui::BeginCombo("##SnapStep", snapLabels[snapIdx], ImGuiComboFlags_NoArrowButton)) {
            for (int i = 0; i < IM_ARRAYSIZE(snapValues); ++i) {
                if (ImGui::Selectable(snapLabels[i], i == snapIdx)) m_snapTranslate = snapValues[i];
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Passo de snap ao segurar Ctrl ao arrastar o gizmo", "Snap step when holding Ctrl while dragging the gizmo"));
        }
    }
    if (toolbarWidth > 350.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Perspectiva", "Perspective"));
    }
    // Live play-mode state in the header: the scene looks identical while the
    // in-engine game runs, so the mode must be explicit.
    const PlayState headerState = m_playMode.get_state();
    if (headerState != PlayState::Edit) {
        const bool paused = headerState == PlayState::Pause;
        const std::string stateTag = std::string(paused ? ICON_FA_PAUSE
                                                        : (headerState == PlayState::Simulate ? ICON_FA_ARROWS_ROTATE : ICON_FA_PLAY)) +
            "  " + (paused ? tr("PAUSADO", "PAUSED")
                           : (headerState == PlayState::Simulate ? tr("SIMULANDO", "SIMULATING")
                                                                 : tr("JOGO EM EXECUÇÃO", "PLAYING")));
        ImGui::SameLine();
        ImGui::TextColored(paused ? ImVec4(0.96f, 0.62f, 0.04f, 1.0f) : ImVec4(0.30f, 0.90f, 0.60f, 1.0f),
                           "%s", stateTag.c_str());
    }
    if (toolbarWidth > 500.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %dx%d", static_cast<int>(panelSize.x), static_cast<int>(panelSize.y));
    }
    // Overflow menu: display toggles that used to be hidden in Janelas.
    ImGui::SameLine();
    if (UI::iconButton(ICON_FA_ELLIPSIS_VERTICAL, tr("Opções do Viewport", "Viewport Options"))) {
        ImGui::OpenPopup("##ViewportOptions");
    }
    if (ImGui::BeginPopup("##ViewportOptions")) {
        ImGui::MenuItem(tr("Grid", "Grid"), nullptr, &m_showGrid);
        ImGui::MenuItem(tr("Gizmos", "Gizmos"), nullptr, &m_showGizmos);
        ImGui::MenuItem(tr("Colliders (wireframe)", "Collider Wireframes"), nullptr, &m_showColliders);
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Segure Ctrl ao arrastar o gizmo para usar snap", "Hold Ctrl while dragging the gizmo to snap"));
        ImGui::EndPopup();
    }
    ImGui::Separator();

    // The image area is what remains BELOW the header. Measure it AFTER the
    // header is drawn — the header is two lines + separator, so estimating it
    // from a single frame height overflowed and the mouse wheel scrolled the
    // panel (the NoScrollbar flag hides the bar but does NOT stop the wheel).
    const ImVec2 imageAvail = ImGui::GetContentRegionAvail();
    const float availW = std::max(1.0f, imageAvail.x);
    const float availH = std::max(1.0f, imageAvail.y);
    m_viewportPanelSize = imageAvail;

    m_viewportHovered = ImGui::IsWindowHovered();
    m_viewportFocused = ImGui::IsWindowFocused();

    if (m_offscreen.imguiTextureID == VK_NULL_HANDLE) {
        ImGui::TextDisabled("%s", tr("O viewport 3D será criado ao entrar no editor...", "3D viewport is being prepared..."));
        ImGui::End();
        return;
    }

    // Fit the offscreen texture into the remaining image area, preserving the
    // aspect ratio: fills 100% of the allowed space, centered, no overflow.
    const float texAspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    float dispW = availW;
    float dispH = dispW / texAspect;
    if (dispH > availH) {
        dispH = availH;
        dispW = dispH * texAspect;
    }
    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    const ImVec2 dispPos(cursorPos.x + std::max(0.0f, (availW - dispW) * 0.5f), cursorPos.y);
    m_viewportImagePos = dispPos;
    m_viewportImageSize = ImVec2(dispW, dispH);
    m_viewportImageHovered = ImGui::IsMouseHoveringRect(dispPos, ImVec2(dispPos.x + dispW, dispPos.y + dispH));

    ImGui::SetCursorScreenPos(dispPos);
    // Vulkan images are top-down; flip V so the world appears upright.
    ImGui::Image(m_offscreen.imguiTextureID, ImVec2(dispW, dispH), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    // Play-mode feedback overlay: while the in-engine game runs, draw a colored
    // border + state badge over the viewport so the mode is unmistakable (the
    // rendered scene itself is identical to edit mode). When the play world has
    // nothing that animates, show a hint instead of silence — the most common
    // "o botão não fez nada" confusion is an empty scene.
    const PlayState playState = m_playMode.get_state();
    if (playState != PlayState::Edit) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool paused = playState == PlayState::Pause;
        const ImU32 accent = paused ? IM_COL32(245, 158, 11, 235) : IM_COL32(16, 185, 129, 235);
        dl->AddRect(dispPos, ImVec2(dispPos.x + dispW, dispPos.y + dispH), accent, 0.0f, 0, 3.0f);

        const std::string badge = std::string(paused ? ICON_FA_PAUSE : ICON_FA_PLAY) + "  " +
            (paused ? tr("PAUSADO", "PAUSED") : tr("JOGO EM EXECUÇÃO", "PLAYING"));
        const ImVec2 badgeSize = ImGui::CalcTextSize(badge.c_str());
        const ImVec2 badgePos(dispPos.x + 10.0f, dispPos.y + 10.0f);
        dl->AddRectFilled(badgePos,
                          ImVec2(badgePos.x + badgeSize.x + 16.0f, badgePos.y + badgeSize.y + 8.0f),
                          IM_COL32(0, 0, 0, 175), 4.0f);
        dl->AddText(ImVec2(badgePos.x + 8.0f, badgePos.y + 4.0f), accent, badge.c_str());

        Scene* playScene = m_playMode.get_active_scene();
        if (playScene) {
            const bool hasMotion = !playScene->rigidbodyComponents.empty() ||
                                   !playScene->particleEmitterComponents.empty() ||
                                   !playScene->weaponComponents.empty() || m_playScriptLoaded;
            if (!hasMotion) {
                const char* hint = tr("Nada muda aqui — adicione um Corpo Rígido, Partícula ou Script e veja o Play agir",
                                      "Nothing moves here — add a Rigid Body, Particle or Script to see Play in action");
                const ImVec2 hintSize = ImGui::CalcTextSize(hint);
                const ImVec2 hintPos(dispPos.x + dispW * 0.5f - hintSize.x * 0.5f - 8.0f,
                                     dispPos.y + dispH - hintSize.y - 14.0f);
                dl->AddRectFilled(hintPos,
                                  ImVec2(hintPos.x + hintSize.x + 16.0f, hintPos.y + hintSize.y + 8.0f),
                                  IM_COL32(0, 0, 0, 195), 4.0f);
                dl->AddText(ImVec2(hintPos.x + 8.0f, hintPos.y + 4.0f), IM_COL32(235, 235, 240, 255), hint);
            }
        }
    }

    // Drag & drop de assets (Content Browser → cena): mesh cria uma entidade
    // em frente à câmera; material aplica na entidade selecionada.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_ASSET_UUID")) {
            if (payload->DataSize > 1) {
                const std::string droppedId(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                handle_asset_drop(UUID::from_string(droppedId));
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);

    if (m_viewportImageHovered) {
        // Paint tool (vertex painting): left-drag paints the selected mesh
        // instead of picking. Active while m_paintToolActive is on (the Paint
        // panel toggles it, or the toolbar 'P' button).
        if (m_paintToolActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_paintBrushDown = paint_mesh_stroke(m_editorCamera.position, viewport_mouse_dir(mouse));
            m_activeAxis = GizmoAxis::None;
            m_gizmoDragging = false;
        } else {
            m_paintBrushDown = false;
        }
        // Left click: grab the gizmo axis first, otherwise pick the entity.
        // Select mode has no gizmo — clicks always pick.
        if (!m_paintToolActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_gizmoDragging) {
            if (m_gizmoMode != GizmoMode::Select && gizmo_axis_hit_test(mouse)) {
                m_activeAxis = m_hoveredAxis;
                start_gizmo_drag(mouse);
            } else {
                m_activeAxis = GizmoAxis::None;
                m_pickPixel = (mouse - glm::vec2(dispPos.x, dispPos.y)) *
                    glm::vec2(static_cast<float>(m_offscreen.width) / dispW,
                              static_cast<float>(m_offscreen.height) / dispH);
                m_pickRequested = true;
            }
        }
    }

    if (m_gizmoDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            update_gizmo_drag(mouse);
        } else {
            m_gizmoDragging = false;
            m_activeAxis = GizmoAxis::None;
            // Gizmo drag released: the transform change is committed.
            mark_scene_dirty();
        }
    } else if (m_viewportImageHovered && !ImGui::IsAnyMouseDown() && m_gizmoMode != GizmoMode::Select) {
        gizmo_axis_hit_test(mouse); // hover highlight
    }

    // Entity hover tooltip: update the hover-pick pixel every few frames
    // (throttled to avoid a GPU pick pass every frame).
    if (m_viewportImageHovered && !m_gizmoDragging && !ImGui::IsAnyMouseDown()) {
        static int hoverFrame = 0;
        if (++hoverFrame % 3 == 0) {
            m_hoverPickPixel = (mouse - glm::vec2(dispPos.x, dispPos.y)) *
                glm::vec2(static_cast<float>(m_offscreen.width) / dispW,
                          static_cast<float>(m_offscreen.height) / dispH);
            m_hoverPickPending = true;
        }
    } else {
        m_hoverEntityName.clear();
    }
    // Tooltip: show entity name at cursor position when hovering over an entity
    if (!m_hoverEntityName.empty() && m_viewportImageHovered) {
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.55f, 0.60f, 1.0f, 1.0f), ICON_FA_CUBE " %s", m_hoverEntityName.c_str());
        ImGui::EndTooltip();
    }

    ImGui::End();
}

void EditorApplication::handle_asset_drop(const UUID& assetId) {
    Scene* scene = m_editorScene.get();
    if (!scene) return;
    const auto found = m_assetRegistry.find(assetId);
    if (!found) {
        std::cerr << "[Viewport] Dropped unknown asset " << assetId.to_string() << std::endl;
        return;
    }
    const AssetMetadata& asset = *found;
    if (asset.type == AssetType::Mesh) {
        Entity ent = scene->create_entity(asset.sourcePath.stem().string());
        scene->meshRendererComponents[ent.get_id()] = MeshRendererComponent{ asset.id, {}, true, true };
        scene->transformComponents[ent.get_id()].position =
            m_editorCamera.position + m_editorCamera.get_front() * 2.0f;
        m_selectedEntity = ent;
        std::cout << "[Viewport] Dropped mesh '" << asset.sourcePath.filename().string()
                  << "' -> spawned entity '" << ent.get_name() << "'\n";
    } else if (asset.type == AssetType::Block) {
        // Block model: spawn as a textured cube in front of the camera (the
        // renderer builds the cube mesh + texture pipeline on demand).
        spawn_block_entity(asset.id, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
        std::cout << "[Viewport] Dropped block '" << asset.sourcePath.filename().string()
                  << "' -> spawned block entity\n";
    } else if (asset.type == AssetType::Texture && is_character_texture(asset)) {
        // Minecraft character/mob skin: drop it and the humanoid character
        // spawns in the scene with the skin as its texture — no sidecar, the
        // PNG itself is the character (just like the block flow).
        spawn_character_entity(asset.id, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
        std::cout << "[Viewport] Dropped skin '" << asset.sourcePath.filename().string()
                  << "' -> spawned character entity\n";
    } else if (asset.type == AssetType::Texture && is_block_texture(asset)) {
        // The PNG itself IS a Minecraft-style block (square POT 8-256, not a
        // character/mob skin): drop it straight in the viewport and it becomes
        // the textured cube — no manual "create block model" step needed. The
        // .vblock sidecar is auto-created on first use and reused afterwards.
        const UUID blockId = create_block_asset(asset);
        if (blockId.is_valid()) {
            spawn_block_entity(blockId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[Viewport] Dropped block texture '" << asset.sourcePath.filename().string()
                      << "' -> spawned block entity (vblock " << blockId.to_string() << ")\n";
        }
    } else if (asset.type == AssetType::Material) {
        if (m_selectedEntity.is_valid()) {
            const auto it = scene->meshRendererComponents.find(m_selectedEntity.get_id());
            if (it != scene->meshRendererComponents.end()) {
                it->second.materialAssetID = asset.id;
                std::cout << "[Viewport] Dropped material '" << asset.sourcePath.filename().string()
                          << "' on '" << m_selectedEntity.get_name() << "'\n";
                return;
            }
        }
        std::cout << "[Viewport] Material drop needs a mesh entity selected\n";
    }
}

void EditorApplication::draw_content_browser_panel() {
    // Local minimum only (see draw_app_bar note about the global style).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(260.0f, 180.0f));
    ImGui::Begin(tr("Assets", "Assets"));
    ImGui::PopStyleVar();

    static bool indexed = false;
    static char search[256]{};
    static int typeFilter = 0;
    static std::optional<UUID> selectedAssetId;
    static ImportSettings editedImportSettings;
    const std::filesystem::path projectAssets =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects" / m_currentProjectName / "Assets";
    const std::filesystem::path fallbackAssets = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets";
    const std::filesystem::path sourceRoot = std::filesystem::exists(projectAssets) ? projectAssets : fallbackAssets;
    const std::filesystem::path cookedRoot =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";

    if (!indexed && m_assetPipeline && std::filesystem::exists(sourceRoot)) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(sourceRoot, error), end; it != end && !error; it.increment(error)) {
            if (!it->is_regular_file()) continue;
            const ImportResult result = m_assetPipeline->import({it->path(), cookedRoot, 1});
            if (!result && result.error.rfind("No importer supports", 0) != 0) {
                std::cerr << "[ContentBrowser] " << result.error << std::endl;
            }
        }
        m_assetHotReload->watch_registered_assets();
        // Auxiliary maps (_n/_s/…) are never blocks: heal sidecars created by
        // older builds right after indexing (idempotent, once per texture),
        // then record sibling material maps on base blocks created before
        // grouping existed.
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
            if (candidate.type == AssetType::Texture && is_aux_map_texture(candidate) &&
                !m_auxBlockHealed.contains(candidate.id)) {
                m_auxBlockHealed.insert(candidate.id);
                heal_aux_block_sidecars(candidate);
            }
        }
        enrich_block_material_maps();
        // Sweep orphan .vblock sidecars: files created by older builds but not
        // present in the registry (dead plumbing). They caused the duplicate
        // pile-up (gold_ore_2/3/4/5.vblock) on every drop of the same texture.
        std::error_code sweepEc;
        for (std::filesystem::recursive_directory_iterator it(sourceRoot, sweepEc), end; it != end && !sweepEc; it.increment(sweepEc)) {
            if (!it->is_regular_file()) continue;
            if (it->path().extension().string() != ".vblock") continue;
            if (!m_assetRegistry.find_id(it->path())) {
                std::filesystem::remove(it->path(), sweepEc);
                std::cout << "[ContentBrowser] Removed orphan sidecar '"
                          << it->path().filename().string() << "'" << std::endl;
            }
        }
        const std::filesystem::path registryPath =
            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[AssetRegistry] Could not persist database: " << registryPath << std::endl;
        indexed = true;
    }

    ImGui::TextDisabled("%s: %s", tr("Pasta do Jogo", "Game Directory"), sourceRoot.string().c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 96.0f);
    ImGui::InputTextWithHint("##AssetSearch", tr("Pesquisar assets...", "Search assets..."), search, sizeof(search));
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FILE_IMPORT "  Importar", ICON_FA_FILE_IMPORT "  Import"), ImVec2(88, 0))) {
        std::string importPath;
        if (pick_file_dialog(importPath, L"Assets (*.*)\0*.*\0", L"Importar Asset", nullptr)) {
            if (m_assetPipeline) {
                const std::filesystem::path cookedRoot =
                    std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
                const ImportResult result = m_assetPipeline->import({ importPath, cookedRoot, 1 });
                if (!result) std::cerr << "[ContentBrowser] " << result.error << std::endl;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_ROTATE "  Atualizar", ICON_FA_ROTATE "  Refresh"), ImVec2(0, 0))) {
        indexed = false;
        m_contentBrowserDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_UPLOAD "  Pack", ICON_FA_UPLOAD "  Pack"), ImVec2(0, 0))) {
        std::string packFolder;
        if (pick_folder_dialog(packFolder, L"Selecionar pasta do pack de texturas")) {
            const size_t count = import_texture_pack(std::filesystem::path(packFolder));
            if (count > 0) {
                indexed = false; // force re-index on next frame
                m_contentBrowserDirty = true;
            }
        }
    }
    // Type filter tabs (the old Combo row could not fit when the dock shrank).
    // "Modelos" groups every 3D/model asset: industry meshes (glTF), block
    // models assembled from Minecraft-style PNGs, and voxel structures.
    const char* filtersPt[] = { "Tudo", "Texturas", "Malhas", "Modelos", "Materiais", "Áudio", "Cenas", "Animações", "Não Usados" };
    const char* filtersEn[] = { "All", "Textures", "Meshes", "Models", "Materials", "Audio", "Scenes", "Animations", "Unused" };
    const float tabAvail = ImGui::GetContentRegionAvail().x;
    const float tabW = ImMax(1.0f, tabAvail / IM_ARRAYSIZE(filtersEn));
    for (int i = 0; i < IM_ARRAYSIZE(filtersEn); ++i) {
        const bool selected = (typeFilter == i);
        if (ImGui::Selectable((m_currentLanguage == EngineLanguage::PT_BR) ? filtersPt[i] : filtersEn[i],
                              selected, 0, ImVec2(tabW, 0))) {
            typeFilter = i;
        }
        if (i + 1 < IM_ARRAYSIZE(filtersEn)) ImGui::SameLine();
    }
    ImGui::Separator();

    std::optional<AssetType> selectedType;
    switch (typeFilter) {
        case 1: selectedType = AssetType::Texture; break;
        case 2: selectedType = AssetType::Mesh; break;
        case 4: selectedType = AssetType::Material; break;
        case 5: selectedType = AssetType::Audio; break;
        case 6: selectedType = AssetType::Scene; break;
        case 7: selectedType = AssetType::Animation; break;
        default: break;
    }

    AssetBrowserModel browser(m_assetRegistry);
    std::vector<AssetMetadata> assets = browser.query(search, selectedType);
    // .vblock sidecars are hidden plumbing: the PNG IS the block, so Block
    // assets never appear as cards (that would duplicate the texture).
    assets.erase(std::remove_if(assets.begin(), assets.end(), [](const AssetMetadata& candidate) {
        return candidate.type == AssetType::Block;
    }), assets.end());
    if (typeFilter == 3) {
        // Modelos: industry meshes + voxel structures + block-capable textures
        // (the PNG itself is the Minecraft-style block) + character/mob skins.
        assets.erase(std::remove_if(assets.begin(), assets.end(), [&](const AssetMetadata& candidate) {
            return candidate.type != AssetType::Mesh && candidate.type != AssetType::VoxelStructure &&
                   !(candidate.type == AssetType::Texture &&
                     (is_block_texture(candidate) || is_character_texture(candidate)));
        }), assets.end());
    }
    if (typeFilter == 8) {
        std::vector<UUID> roots;
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot())
            if (candidate.type == AssetType::Scene) roots.push_back(candidate.id);
        const std::vector<UUID> unused = m_assetRegistry.unused_assets(roots);
        const std::unordered_set<UUID> unusedSet(unused.begin(), unused.end());
        assets.erase(std::remove_if(assets.begin(), assets.end(), [&](const AssetMetadata& candidate) {
            return !unusedSet.contains(candidate.id);
        }), assets.end());
    }
    const auto assetIcon = [](AssetType t) -> const char* {
        switch (t) {
            case AssetType::Texture: return ICON_FA_IMAGE;
            case AssetType::Mesh: return ICON_FA_CUBE;
            case AssetType::Material: return ICON_FA_PAINTBRUSH;
            case AssetType::Audio: return ICON_FA_MUSIC;
            case AssetType::Skeleton: return ICON_FA_SITEMAP;
            case AssetType::Animation: return ICON_FA_FILM;
            case AssetType::Scene: return ICON_FA_CLAPPERBOARD;
            case AssetType::VoxelStructure: return ICON_FA_CUBES;
            case AssetType::Block: return ICON_FA_CUBES;
            default: return ICON_FA_FILE;
        }
    };
    const auto assetTypeName = [this](AssetType t) -> const char* {
        switch (t) {
            case AssetType::Texture: return tr("Textura", "Texture");
            case AssetType::Mesh: return tr("Malha", "Mesh");
            case AssetType::Material: return tr("Material", "Material");
            case AssetType::Audio: return tr("Áudio", "Audio");
            case AssetType::Skeleton: return tr("Esqueleto", "Skeleton");
            case AssetType::Animation: return tr("Animação", "Animation");
            case AssetType::Scene: return tr("Cena", "Scene");
            case AssetType::VoxelStructure: return tr("Voxel", "Voxel");
            case AssetType::Block: return tr("Bloco", "Block");
            default: return "?";
        }
    };

    const float cellSize = 150.0f;
    int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellSize));
    // Lazy loading (like IntersectionObserver in JS): only assets whose cards
    // are inside the browser's visible area ever trigger thumbnail work.
    // Off-screen rows never decode/upload/render, so scrolling a huge folder
    // loads just the screenful instead of the whole list. Row height is an
    // estimate (48px button + type line + wrapped name + spacing) with a
    // one-row safety margin; being slightly generous only costs a few extra
    // small async requests, never a stall.
    const float cellHeight = 96.0f;
    const float scrollY = ImGui::GetScrollY();
    const float windowH = ImGui::GetWindowHeight();
    const int firstVisibleRow = std::max(0, static_cast<int>(scrollY / cellHeight) - 1);
    const int lastVisibleRow = static_cast<int>((scrollY + windowH) / cellHeight) + 1;
    ImGui::Columns(columns, "AssetGrid", false);
    size_t gridIndex = 0;
    for (const AssetMetadata& asset : assets) {
        const int row = static_cast<int>(gridIndex / static_cast<size_t>(std::max(columns, 1)));
        ++gridIndex;
        const bool isVisible = row >= firstVisibleRow && row <= lastVisibleRow;
        const std::string filename = asset.sourcePath.filename().string();
        ImGui::PushID(asset.id.to_string().c_str());

        // Real previews: cooked textures show the actual image (async decode on
        // a worker, cached forever); meshes and blocks render a true 3D
        // thumbnail (offscreen, budgeted a few per frame); audio tiles carry a
        // single active ▶/⏸ preview voice. Only visible cards request work.
        const bool isTexture = (asset.type == AssetType::Texture);
        const bool isAudio = (asset.type == AssetType::Audio);
        const bool isBlockTexture = isTexture && is_block_texture(asset);
        const bool isSkinTexture = isTexture && !isBlockTexture && is_character_texture(asset);
        const bool isModel = (asset.type == AssetType::Mesh);
        VkDescriptorSet thumb = VK_NULL_HANDLE;
        if (isTexture && !isBlockTexture && !isSkinTexture) {
            const auto found = m_assetThumbnails.find(asset.id);
            if (found != m_assetThumbnails.end()) {
                thumb = found->second.imguiId;
            } else if (isVisible) {
                request_asset_thumbnail_decode(asset); // async, 1 upload/frame
            }
        } else if ((isModel || isBlockTexture || isSkinTexture) && isVisible) {
            const auto thumbIt = m_asset3dThumbnails.find(asset.id);
            if (thumbIt != m_asset3dThumbnails.end()) {
                thumb = thumbIt->second;
            } else {
                request_3d_thumbnail(asset.id); // rendered by pump_asset_thumbnails
            }
        }
        if (thumb != VK_NULL_HANDLE) {
            if (ImGui::ImageButton("##thumb", thumb, ImVec2(135, 48))) {
                selectedAssetId = asset.id;
                editedImportSettings = asset.importSettings;
            }
        } else {
            if (ImGui::Button(assetIcon(asset.type), ImVec2(135, 48))) {
                selectedAssetId = asset.id;
                editedImportSettings = asset.importSettings;
            }
        }
        if (ImGui::IsItemHovered()) {
            const char* label = isBlockTexture ? tr("Bloco", "Block")
                               : isSkinTexture ? tr("Skin (Personagem/Mob)", "Skin (Character/Mob)")
                               : assetTypeName(asset.type);
            ImGui::SetTooltip("%s\n%s", filename.c_str(), label);
        }
        // Double-click opens the matching editor (industry convention).
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            switch (asset.type) {
                case AssetType::Material: m_specializedEditors.open_editor("Material"); break;
                case AssetType::Animation: m_specializedEditors.open_editor("Animation"); break;
                case AssetType::Audio: m_specializedEditors.open_editor("Audio"); break;
                case AssetType::Mesh: m_wickedTools.showMeshWindow = true; break;
                default: break;
            }
        }
        if (isAudio) {
            const bool playing = m_audioPreviewAsset == asset.id && m_audioPreviewVoice != 0 &&
                                 m_playAudio.is_active(m_audioPreviewVoice);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (135.0f - 42.0f) * 0.5f);
            if (ImGui::Button(playing ? ICON_FA_PAUSE : ICON_FA_PLAY, ImVec2(42, 24))) {
                toggle_audio_preview(asset);
            }
        }
        if (ImGui::BeginPopupContextItem("AssetContext")) {
            if (ImGui::MenuItem(tr("Duplicar", "Duplicate"))) {
                std::filesystem::path duplicatePath = asset.sourcePath.parent_path() /
                    (asset.sourcePath.stem().string() + "_copy" + asset.sourcePath.extension().string());
                unsigned suffix = 2;
                while (std::filesystem::exists(duplicatePath)) {
                    duplicatePath = asset.sourcePath.parent_path() /
                        (asset.sourcePath.stem().string() + "_copy" + std::to_string(suffix++) + asset.sourcePath.extension().string());
                }
                const AssetFileOperationResult duplicated = browser.duplicate_asset(asset.id, duplicatePath);
                if (!duplicated) {
                    std::cerr << "[ContentBrowser] " << duplicated.error << std::endl;
                } else {
                    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
                        "Intermediate" / "AssetRegistry.db";
                    if (!m_assetRegistry.save(registryPath))
                        std::cerr << "[AssetRegistry] Could not persist duplicated asset" << std::endl;
                }
            }
            if (ImGui::MenuItem(tr("Excluir", "Delete"))) {
                const AssetFileOperationResult deleted = browser.delete_asset(asset.id);
                if (!deleted) {
                    std::cerr << "[ContentBrowser] " << deleted.error << std::endl;
                } else {
                    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
                        "Intermediate" / "AssetRegistry.db";
                    if (!m_assetRegistry.save(registryPath))
                        std::cerr << "[AssetRegistry] Could not persist asset deletion" << std::endl;
                }
            }
            // The PNG is the block: spawn it as a textured cube in the scene
            // (same result as dragging it into the viewport).
            if (isBlockTexture) {
                if (ImGui::MenuItem(tr("Criar Entidade de Bloco na Cena", "Spawn Block in Scene"))) {
                    const UUID blockId = create_block_asset(asset);
                    if (blockId.is_valid()) spawn_block_entity(blockId, m_editorCamera.orbitTarget);
                }
                if (ImGui::MenuItem(tr("Criar Modelo de Bloco (Minecraft)", "Create Block Model (Minecraft)"))) {
                    create_block_asset(asset);
                }
            }
            // The PNG is the character/mob: spawn the humanoid in the scene
            // (same result as dragging it into the viewport).
            if (isSkinTexture) {
                if (ImGui::MenuItem(tr("Criar Personagem na Cena", "Spawn Character in Scene"))) {
                    spawn_character_entity(asset.id, m_editorCamera.orbitTarget);
                }
            }
            // Classification override: the heuristic can misfire on skins vs
            // blocks — the .vblock sidecar is the explicit user mark.
            if (isTexture) {
                if (ImGui::MenuItem(isBlockTexture
                                        ? tr("Desmarcar como Bloco (é personagem/mob?)", "Unmark as Block (character/mob?)")
                                        : tr("Marcar como Bloco de Minecraft", "Mark as Minecraft Block"))) {
                    if (isBlockTexture) unmark_block_texture(asset);
                    else create_block_asset(asset);
                }
            }
            const auto referencers = m_assetRegistry.referencers_of(asset.id);
            if (!referencers.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("%zu reference(s)", referencers.size());
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource()) {
            const std::string id = asset.id.to_string();
            ImGui::SetDragDropPayload("CONTENT_ASSET_UUID", id.c_str(), id.size() + 1);
            ImGui::TextUnformatted(filename.c_str());
            ImGui::EndDragDropSource();
        }
        if (isBlockTexture) {
            ImGui::TextColored(ImVec4(0.30f, 0.75f, 0.95f, 1.0f), "%s", tr("Bloco", "Block"));
        } else if (isSkinTexture) {
            ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.95f, 1.0f), "%s", tr("Skin", "Skin"));
        } else {
            ImGui::TextColored(UI::Colors::TextSecondary, "%s", assetTypeName(asset.type));
        }
        ImGui::SameLine();
        if (asset.isCooked) ImGui::TextColored(UI::Colors::Success, "%s", ICON_FA_CIRCLE_CHECK);
        ImGui::TextWrapped("%s", filename.c_str());
        ImGui::PopID();
        ImGui::NextColumn();
    }
    ImGui::Columns(1);

    if (selectedAssetId) {
        const auto selected = m_assetRegistry.find(*selectedAssetId);
        if (!selected) {
            selectedAssetId.reset();
        } else {
            ImGui::SeparatorText(tr("Configurações de Importação", "Import Settings"));
            ImGui::Text("%s", selected->sourcePath.filename().string().c_str());
            ImGui::TextDisabled("UUID: %s", selected->id.to_string().c_str());
            ImGui::TextDisabled("Cooked: %s", selected->cookedPath.string().c_str());
            if (selected->type == AssetType::Texture) {
                ImGui::Checkbox(tr("Gerar mipmaps", "Generate mipmaps"), &editedImportSettings.generateMipmaps);
                ImGui::Checkbox("sRGB", &editedImportSettings.srgb);
                int textureQuality = static_cast<int>(editedImportSettings.textureQuality);
                if (ImGui::SliderInt(tr("Qualidade", "Quality"), &textureQuality, 0, 100))
                    editedImportSettings.textureQuality = static_cast<uint32_t>(textureQuality);
                ImGui::TextDisabled("%u x %u, %u channel(s)", selected->width, selected->height, selected->channels);
            } else if (selected->type == AssetType::Mesh) {
                ImGui::DragFloat(tr("Escala da mesh", "Mesh scale"), &editedImportSettings.meshScale,
                                 0.01f, 0.001f, 1000.0f, "%.3f");
                ImGui::TextDisabled("%u primitive(s), %llu vertices, %llu indices",
                    selected->primitiveCount,
                    static_cast<unsigned long long>(selected->vertexCount),
                    static_cast<unsigned long long>(selected->indexCount));
            } else if (selected->type == AssetType::Audio) {
                ImGui::TextDisabled("%u Hz, %u channel(s), %.2f s", selected->sampleRate,
                                    selected->audioChannels, selected->durationSeconds);
            } else {
                ImGui::TextDisabled("No editable import settings for this asset type");
            }
            const bool editable = selected->type == AssetType::Texture || selected->type == AssetType::Mesh;
            if (!editable) ImGui::BeginDisabled();
            if (ImGui::Button(tr("Aplicar e reimportar", "Apply and reimport"))) {
                const ImportResult reimported = m_assetPipeline->import({
                    .source = selected->sourcePath,
                    .cookedDirectory = cookedRoot,
                    .importerVersion = selected->importerVersion,
                    .settings = editedImportSettings});
                if (!reimported) {
                    std::cerr << "[ContentBrowser] Reimport failed: " << reimported.error << std::endl;
                } else {
                    editedImportSettings = reimported.asset.importSettings;
                    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) /
                        "Intermediate" / "AssetRegistry.db";
                    if (!m_assetRegistry.save(registryPath))
                        std::cerr << "[AssetRegistry] Could not persist import settings" << std::endl;
                    if (m_assetHotReload) m_assetHotReload->watch_registered_assets();
                }
            }
            if (!editable) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Fechar", "Close"))) selectedAssetId.reset();
            const auto dependencies = m_assetRegistry.dependencies_of(selected->id);
            const auto referencers = m_assetRegistry.referencers_of(selected->id);
            ImGui::TextDisabled("%zu dependencies, %zu referencers", dependencies.size(), referencers.size());
        }
    }

    if (m_assetHotReload) {
        const auto reloaded = m_assetHotReload->poll();
        if (!reloaded.empty()) ImGui::Text("%zu asset(s) reimported", reloaded.size());
    }
    ImGui::End();
}

void EditorApplication::load_script_canvas() {
    m_scriptCanvasPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Content" / "Scenes" / "Initial.script";
    m_scriptCanvas = VisualScriptCanvas{};
    m_scriptCanvasLoaded = true;
    if (!std::filesystem::exists(m_scriptCanvasPath)) {
        std::cout << "[Editor] Script Canvas: " << m_scriptCanvasPath.string() << " not found — starting empty\n";
        return;
    }
    ScriptGraphAsset asset;
    if (!asset.load(m_scriptCanvasPath)) {
        std::cerr << "[Editor] Script Canvas: failed to load " << m_scriptCanvasPath.string() << '\n';
        return;
    }
    m_scriptCanvas = VisualScriptCanvas(to_visual_graph(asset));
    // Stagger the layout so nodes never stack on top of each other.
    float x = 40.0f;
    for (const ScriptNode& node : m_scriptCanvas.nodes()) {
        m_scriptCanvas.move_node(node.id, glm::vec2(x, 60.0f));
        x += 200.0f;
    }
    std::cout << "[Editor] Script Canvas loaded: " << m_scriptCanvasPath.string()
              << " (nodes=" << m_scriptCanvas.nodes().size()
              << ", connections=" << m_scriptCanvas.connections().size() << ")\n";
}

void EditorApplication::save_script_canvas() {
    if (m_scriptCanvasPath.empty()) load_script_canvas();
    const ScriptGraphAsset asset = from_visual_graph(m_scriptCanvas.graph());
    if (asset.save(m_scriptCanvasPath)) {
        m_scriptCanvas.mark_saved();
        std::cout << "[Editor] Script Canvas saved: " << m_scriptCanvasPath.string()
                  << " (nodes=" << asset.nodes.size() << ", links=" << asset.links.size()
                  << ") — play mode hot-reloads it\n";
    } else {
        std::cerr << "[Editor] Script Canvas: save failed: " << m_scriptCanvasPath.string() << '\n';
    }
}

void EditorApplication::add_canvas_node(const std::string& kind, glm::vec2 worldPos) {
    ScriptNode node;
    node.id = UUID();
    node.title = kind;
    const auto pin = [](const std::string& name, PinType type, bool isInput) {
        ScriptPin p;
        p.id = UUID();
        p.name = name;
        p.type = type;
        p.isInput = isInput;
        return p;
    };
    if (kind == "Event" || kind == "Return" || kind == "Scope" || kind == "Scope End" ||
        kind == "Function" || kind == "Function Call" || kind == "Emit Event" || kind == "Wait") {
        if (kind != "Return" && kind != "Scope End") node.outputs.push_back(pin("Out", PinType::Execution, false));
        node.inputs.push_back(pin("In", PinType::Execution, true));
    } else if (kind == "Branch") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.inputs.push_back(pin("Condition", PinType::Boolean, true));
        node.outputs.push_back(pin("True", PinType::Execution, false));
        node.outputs.push_back(pin("False", PinType::Execution, false));
    } else if (kind == "Constant Float") {
        node.outputs.push_back(pin("Value", PinType::Float, false));
    } else if (kind == "Constant Integer") {
        node.outputs.push_back(pin("Value", PinType::Integer, false));
    } else if (kind == "Constant Boolean") {
        node.outputs.push_back(pin("Value", PinType::Boolean, false));
    } else if (kind == "Get Variable") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.outputs.push_back(pin("Value", PinType::Float, false));
    } else if (kind == "Set Variable") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.inputs.push_back(pin("Value", PinType::Float, true));
        node.outputs.push_back(pin("Out", PinType::Execution, false));
    } else if (kind == "Log") {
        node.inputs.push_back(pin("In", PinType::Execution, true));
        node.inputs.push_back(pin("Message", PinType::Float, true));
        node.outputs.push_back(pin("Out", PinType::Execution, false));
    } else if (kind == "Add Float" || kind == "Subtract Float" || kind == "Multiply Float") {
        node.inputs.push_back(pin("A", PinType::Float, true));
        node.inputs.push_back(pin("B", PinType::Float, true));
        node.outputs.push_back(pin("Result", PinType::Float, false));
    } else {
        return; // unknown kind
    }
    m_scriptCanvas.add_node(node, worldPos);
    m_scriptCanvas.clear_selection();
    m_scriptCanvas.select(node.id);
}

void EditorApplication::draw_script_canvas_panel() {
    if (!ImGui::Begin(tr("Canvas de Scripts", "Script Canvas"), &m_showScriptCanvas)) {
        ImGui::End();
        return;
    }
    clamp_floating_window_on_screen();

    // Toolbar.
    if (ImGui::Button(tr("Salvar", "Save"))) save_script_canvas();
    ImGui::SameLine();
    if (ImGui::Button(tr("Recarregar", "Reload"))) load_script_canvas();
    ImGui::SameLine();
    if (ImGui::Button(tr("Desfazer", "Undo"))) m_scriptCanvas.undo();
    ImGui::SameLine();
    if (ImGui::Button(tr("Refazer", "Redo"))) m_scriptCanvas.redo();
    ImGui::SameLine();
    if (ImGui::BeginCombo("##add", m_canvasAddKind.c_str())) {
        static const char* kinds[] = {"Event", "Constant Float", "Constant Integer", "Constant Boolean",
                                      "Get Variable", "Set Variable", "Add Float", "Subtract Float",
                                      "Multiply Float", "Branch", "Wait", "Emit Event", "Log",
                                      "Function", "Function Call", "Scope", "Scope End", "Return"};
        for (const char* kind : kinds) {
            if (ImGui::Selectable(kind, m_canvasAddKind == kind)) m_canvasAddKind = kind;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Adicionar", "Add"))) {
        add_canvas_node(m_canvasAddKind, m_scriptCanvas.screen_to_world(glm::vec2(60.0f, 40.0f)));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s: %zu nós, %zu ligações, zoom %.2f%s",
                        m_scriptCanvas.dirty() ? tr("sujo", "dirty") : tr("salvo", "saved"),
                        m_scriptCanvas.nodes().size(), m_scriptCanvas.connections().size(),
                        m_scriptCanvas.zoom(), m_scriptCanvas.can_undo() ? " [U/D disponível]" : "");
    for (const auto& issue : m_scriptCanvas.validate()) {
        if (issue.severity == CanvasIssue::Severity::Info) continue;
        const ImVec4 color = issue.severity == CanvasIssue::Severity::Error
                                 ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                                 : ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        ImGui::TextColored(color, "[%s] %s: %s",
                           issue.severity == CanvasIssue::Severity::Error ? "erro" : "aviso",
                           issue.field.c_str(), issue.message.c_str());
    }

    // Canvas surface.
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 10.0f || canvasSize.y < 10.0f) { ImGui::End(); return; }
    ImGui::InvisibleButton("##scriptcanvas", canvasSize);
    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const glm::vec2 canvasOrigin(canvasPos.x, canvasPos.y);

    // Background + grid.
    draw->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                        IM_COL32(28, 28, 32, 255));
    const float gridStep = 24.0f * m_scriptCanvas.zoom();
    if (gridStep > 8.0f) {
        const glm::vec2 worldTopLeft = m_scriptCanvas.screen_to_world(glm::vec2(0.0f, 0.0f));
        const glm::vec2 worldBottomRight =
            m_scriptCanvas.screen_to_world(glm::vec2(canvasSize.x, canvasSize.y));
        for (float gx = std::floor(worldTopLeft.x) * gridStep; gx < worldBottomRight.x * m_scriptCanvas.zoom() + canvasPos.x; gx += gridStep) {
            draw->AddLine(ImVec2(canvasPos.x + gx, canvasPos.y),
                          ImVec2(canvasPos.x + gx, canvasPos.y + canvasSize.y), IM_COL32(45, 45, 52, 255));
        }
        for (float gy = std::floor(worldTopLeft.y) * gridStep; gy < worldBottomRight.y * m_scriptCanvas.zoom() + canvasPos.y; gy += gridStep) {
            draw->AddLine(ImVec2(canvasPos.x, canvasPos.y + gy),
                          ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + gy), IM_COL32(45, 45, 52, 255));
        }
    }

    // Pan (middle drag) and zoom (wheel around the cursor).
    if (canvasHovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            m_scriptCanvas.pan_by(glm::vec2(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y));
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const glm::vec2 before = m_scriptCanvas.screen_to_world(
                glm::vec2(ImGui::GetIO().MousePos.x - canvasPos.x, ImGui::GetIO().MousePos.y - canvasPos.y));
            m_scriptCanvas.set_zoom(m_scriptCanvas.zoom() * (wheel > 0.0f ? 1.15f : 1.0f / 1.15f));
            const glm::vec2 after = m_scriptCanvas.screen_to_world(
                glm::vec2(ImGui::GetIO().MousePos.x - canvasPos.x, ImGui::GetIO().MousePos.y - canvasPos.y));
            m_scriptCanvas.pan_by(glm::vec2((after.x - before.x) * m_scriptCanvas.zoom(),
                                            (after.y - before.y) * m_scriptCanvas.zoom()));
        }
    }

    // Pin world position: y offset by index within the node's pin list.
    const auto pin_pos = [&](const ScriptNode& node, const ScriptPin& pin, bool isInput) -> glm::vec2 {
        const CanvasRect rect = m_scriptCanvas.node_rect(node.id);
        int index = 0;
        if (isInput) {
            for (std::size_t i = 0; i < node.inputs.size(); ++i) if (node.inputs[i].id == pin.id) { index = static_cast<int>(i); break; }
        } else {
            for (std::size_t i = 0; i < node.outputs.size(); ++i) if (node.outputs[i].id == pin.id) { index = static_cast<int>(i); break; }
        }
        const glm::vec2 world(std::min(rect.min.x, rect.max.x) + 8.0f,
                              std::min(rect.min.y, rect.max.y) + 24.0f +
                                  static_cast<float>(index) * 18.0f);
        return m_scriptCanvas.world_to_screen(world) + canvasOrigin;
    };
    // Wires (behind nodes).
    const auto pinScreen = [&](UUID owner, UUID pin) -> std::optional<glm::vec2> {
        for (const ScriptNode& node : m_scriptCanvas.nodes()) {
            if (node.id != owner) continue;
            for (const ScriptPin& p : node.inputs) if (p.id == pin) return pin_pos(node, p, true);
            for (const ScriptPin& p : node.outputs) if (p.id == pin) return pin_pos(node, p, false);
        }
        return std::nullopt;
    };

    for (const ScriptConnection& connection : m_scriptCanvas.connections()) {
        const auto fromPos = pinScreen(connection.fromPinID, connection.fromPinID);
        const auto toPos = pinScreen(connection.toPinID, connection.toPinID);
        if (!fromPos || !toPos) continue;
        draw->AddBezierCubic(ImVec2(fromPos->x, fromPos->y),
                             ImVec2(fromPos->x + 60.0f, fromPos->y),
                             ImVec2(toPos->x - 60.0f, toPos->y),
                             ImVec2(toPos->x, toPos->y),
                             IM_COL32(140, 160, 220, 255), 2.0f);
    }
    // In-progress drag wire.
    if (m_canvasDragPin.is_valid()) {
        const ImVec2 mouse(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        draw->AddBezierCubic(ImVec2(m_canvasDragPinPos.x, m_canvasDragPinPos.y),
                             ImVec2(m_canvasDragPinPos.x + 60.0f, m_canvasDragPinPos.y),
                             ImVec2(mouse.x - 60.0f, mouse.y), mouse,
                             IM_COL32(220, 180, 80, 255), 2.0f);
    }

    // Nodes.
    for (const ScriptNode& node : m_scriptCanvas.nodes()) {
        const CanvasRect rect = m_scriptCanvas.node_rect(node.id);
        const glm::vec2 topLeft = m_scriptCanvas.world_to_screen(rect.min) + canvasOrigin;
        const glm::vec2 bottomRight = m_scriptCanvas.world_to_screen(rect.max) + canvasOrigin;
        const bool selected = m_scriptCanvas.is_selected(node.id);
        draw->AddRectFilled(ImVec2(topLeft.x, topLeft.y), ImVec2(bottomRight.x, bottomRight.y),
                            selected ? IM_COL32(58, 66, 92, 255) : IM_COL32(48, 50, 62, 255), 6.0f);
        draw->AddRect(ImVec2(topLeft.x, topLeft.y), ImVec2(bottomRight.x, bottomRight.y),
                      selected ? IM_COL32(110, 150, 255, 255) : IM_COL32(90, 95, 115, 255), 6.0f);
        draw->AddText(ImVec2(topLeft.x + 8.0f, topLeft.y + 4.0f), IM_COL32(230, 230, 235, 255), node.title.c_str());
        // Pins.
        const auto drawPin = [&](const ScriptPin& pin, bool isInput) {
            const glm::vec2 p = pin_pos(node, pin, isInput);
            const ImU32 color = pin.type == PinType::Execution ? IM_COL32(190, 120, 220, 255)
                                : (pin.type == PinType::Boolean ? IM_COL32(90, 200, 120, 255)
                                : (pin.type == PinType::Integer ? IM_COL32(120, 170, 240, 255)
                                : IM_COL32(240, 180, 90, 255)));
            draw->AddCircleFilled(ImVec2(p.x, p.y), 5.0f, color);
            draw->AddCircle(ImVec2(p.x, p.y), 5.0f, IM_COL32(20, 20, 25, 255));
            draw->AddText(ImVec2(p.x + (isInput ? 9.0f : -9.0f - ImGui::CalcTextSize(pin.name.c_str()).x), p.y - 7.0f),
                          IM_COL32(200, 200, 210, 255), pin.name.c_str());
        };
        for (const ScriptPin& pin : node.inputs) drawPin(pin, true);
        for (const ScriptPin& pin : node.outputs) drawPin(pin, false);
    }

    // Interaction: pick / drag nodes, drag pins to connect, marquee, delete.
    const glm::vec2 mouseWorld = m_scriptCanvas.screen_to_world(
        glm::vec2(ImGui::GetIO().MousePos.x - canvasPos.x, ImGui::GetIO().MousePos.y - canvasPos.y));
    const bool leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool leftRelease = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (canvasHovered && leftClick) {
        // Pin hit test first (connect start).
        bool pinHit = false;
        for (const ScriptNode& node : m_scriptCanvas.nodes()) {
            for (const ScriptPin& pin : node.inputs) {
                const glm::vec2 p = pin_pos(node, pin, true);
                if (glm::distance(p, glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y)) <= 8.0f) {
                    // Dropping an in-progress wire on an input pin.
                    if (m_canvasDragPin.is_valid()) {
                        std::string reason;
                        if (!m_scriptCanvas.connect(m_canvasDragPin, pin.id, &reason)) {
                            std::cerr << "[Script Canvas] connect: " << reason << '\n';
                        }
                        m_canvasDragPin = UUID{0, 0};
                    }
                    pinHit = true;
                    break;
                }
            }
            if (pinHit) break;
            for (const ScriptPin& pin : node.outputs) {
                const glm::vec2 p = pin_pos(node, pin, false);
                if (glm::distance(p, glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y)) <= 8.0f) {
                    if (m_canvasDragPin.is_valid()) {
                        std::string reason;
                        if (!m_scriptCanvas.connect(m_canvasDragPin, pin.id, &reason)) {
                            std::cerr << "[Script Canvas] connect: " << reason << '\n';
                        }
                        m_canvasDragPin = UUID{0, 0};
                    } else {
                        m_canvasDragPin = pin.id;
                        m_canvasDragPinPos = p;
                    }
                    pinHit = true;
                    break;
                }
            }
            if (pinHit) break;
        }
        if (!pinHit) {
            const UUID hit = m_scriptCanvas.node_at(mouseWorld);
            if (hit.is_valid()) {
                m_scriptCanvas.select(hit, ImGui::GetIO().KeyCtrl);
            } else if (!ImGui::GetIO().KeyCtrl) {
                m_scriptCanvas.clear_selection();
            }
        }
    }
    if (leftRelease && m_canvasDragPin.is_valid()) {
        // Dropped on empty space: keep the pin selected but clear the drag.
        m_canvasDragPin = UUID{0, 0};
    }
    // Drag selected nodes (left held after click on a node).
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        m_scriptCanvas.selection_count() > 0) {
        const UUID under = m_scriptCanvas.node_at(mouseWorld);
        if (!under.is_valid() || m_scriptCanvas.is_selected(under)) {
            m_scriptCanvas.move_selection(glm::vec2(ImGui::GetIO().MouseDelta.x / m_scriptCanvas.zoom(),
                                                    ImGui::GetIO().MouseDelta.y / m_scriptCanvas.zoom()));
        }
    }
    // Delete key removes the selection.
    if (canvasHovered && ImGui::IsKeyPressed(ImGuiKey_Delete) && m_scriptCanvas.selection_count() > 0) {
        m_scriptCanvas.begin_batch("delete");
        const auto selection = m_scriptCanvas.selection();
        for (const UUID& id : selection) m_scriptCanvas.remove_node(id);
        m_scriptCanvas.end_batch();
    }
    // Undo/redo shortcuts.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) m_scriptCanvas.undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) m_scriptCanvas.redo();

    ImGui::End();
}

void EditorApplication::draw_script_debugger_panel() {
    using namespace Engine::Scripting;
    ImGui::Begin(tr("Debugger de Scripts", "Script Debugger"));
    clamp_floating_window_on_screen();
    const bool playing = m_playMode.get_state() == PlayState::Play ||
                         m_playMode.get_state() == PlayState::Simulate;
    if (!m_playScriptLoaded || !playing) {
        ImGui::TextWrapped("%s", tr(
            "Inicie o Play para depurar Initial.script: breakpoints, passo a passo, variáveis e watches ao vivo.",
            "Start Play to debug Initial.script: breakpoints, stepping, live variables and watches."));
        ImGui::TextDisabled("%s", m_playScriptPath.string().c_str());
        ImGui::End();
        return;
    }

    const VMStatus vmStatus = m_playScript.status();
    const size_t ip = m_playScript.instruction_pointer();
    const std::string stateText =
        vmStatus == VMStatus::Paused   ? tr("Em pausa (breakpoint)", "Paused (breakpoint)") :
        vmStatus == VMStatus::Completed ? tr("Concluído", "Completed") :
        vmStatus == VMStatus::Error    ? ("Error: " + m_playScript.error()) :
        m_scriptPauseRequested         ? tr("Segurando", "Held") : tr("Executando", "Running");
    ImGui::Text("%s | ip=%zu", stateText.c_str(), ip);

    const bool pausedOrHeld = m_scriptPauseRequested || vmStatus == VMStatus::Paused;
    if (pausedOrHeld) {
        if (ImGui::Button(tr("Continuar", "Continue"))) {
            m_scriptPauseRequested = false;
            if (vmStatus == VMStatus::Paused) m_scriptDebugger.continue_run(10000, 0.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Passo", "Step"))) m_scriptDebugger.step_into(0.0f);
        ImGui::SameLine();
        if (ImGui::Button(tr("Pular", "Step Over"))) m_scriptDebugger.step_over(0.0f);
        ImGui::SameLine();
        if (ImGui::Button(tr("Sair", "Step Out"))) m_scriptDebugger.step_out(0.0f);
    } else {
        if (ImGui::Button(tr("Pausar", "Pause"))) m_scriptPauseRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Reiniciar", "Restart"))) {
        m_scriptPauseRequested = false;
        if (m_playScript.start_event("OnStart")) m_scriptDebugger.continue_run(10000, 0.0f);
    }

    // Compiled bytecode with click-to-toggle breakpoints; the current
    // instruction is highlighted. The executing node (sourceNode) is shown
    // so the user can correlate bytecode with the graph.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Bytecode (clique para alternar breakpoint)", "Bytecode (click to toggle breakpoint)"));
    const ScriptProgram& prog = m_playScript.program();
    if (ImGui::BeginChild("##scriptInstr", ImVec2(0, 280), true)) {
        for (size_t i = 0; i < prog.instructions.size(); ++i) {
            const Instruction& inst = prog.instructions[i];
            const bool isBp = m_scriptDebugger.has_breakpoint(i);
            const bool isIp = (i == ip);
            std::string label = (isBp ? "[B] " : "    ") + std::to_string(i) + "  " +
                                script_opcode_name(inst.opcode);
            if (!inst.text.empty()) label += " '" + inst.text + "'";
            if (std::holds_alternative<double>(inst.operand))
                label += " " + std::to_string(std::get<double>(inst.operand));
            if (inst.target != 0) label += " ->" + std::to_string(inst.target);
            if (isIp) label += "   <<<";
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), isBp)) {
                if (isBp) m_scriptDebugger.remove_breakpoint(i);
                else m_scriptDebugger.add_breakpoint(i);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Graph view: the authored nodes, with the node owning the current
    // instruction highlighted.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Nós do grafo", "Graph nodes"));
    UUID currentNode{ 0, 0 };
    if (ip < prog.instructions.size()) currentNode = prog.instructions[ip].sourceNode;
    if (ImGui::BeginChild("##scriptNodes", ImVec2(0, 110), true)) {
        for (const TypedScriptNode& node : m_scriptDebugGraph.nodes) {
            std::string label = script_node_kind_name(node.kind);
            if (!node.event.empty()) label += " '" + node.event + "'";
            if (!node.variable.empty()) label += " '" + node.variable + "'";
            const bool active = node.id == currentNode;
            if (active) label += "   <<<";
            ImGui::TextColored(active ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                               "%s", label.c_str());
        }
    }
    ImGui::EndChild();

    // Live variables.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Variáveis", "Variables"));
    if (ImGui::BeginChild("##scriptVars", ImVec2(0, 110), true)) {
        if (m_playScript.variables().empty()) ImGui::TextDisabled("(sem variáveis)");
        for (const auto& [name, value] : m_playScript.variables())
            ImGui::Text("%s = %s", name.c_str(), ScriptDebugger::value_to_string(value).c_str());
    }
    ImGui::EndChild();

    // Call stack.
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Pilha de chamadas", "Call Stack"));
    const auto& frames = m_scriptDebugger.call_stack();
    if (frames.empty()) ImGui::TextDisabled("(frame principal)");
    for (const auto& frame : frames) ImGui::Text("%s @ %zu", frame.name.c_str(), frame.entry);

    // Watch expressions (evaluated against the current scope).
    ImGui::Separator();
    static char watchBuf[128] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##watchExpr", watchBuf, sizeof(watchBuf));
    ImGui::SameLine();
    if (ImGui::Button(tr("Adicionar Watch", "Add Watch"))) {
        if (watchBuf[0]) {
            m_scriptDebugger.add_watch(watchBuf);
            watchBuf[0] = '\0';
        }
    }
    m_scriptDebugger.evaluate_watches();
    for (const auto& watch : m_scriptDebugger.watches())
        ImGui::Text("%s = %s", watch.expression.c_str(), watch.result.c_str());
    ImGui::End();
}

void EditorApplication::draw_voxel_tool_panel() {
#if VC_ENABLE_VOXEL_PLUGIN
    // Local minimum only: this panel never shrinks below a readable width
    // (the global WindowMinSize stays small so the app bar/shell stay exact).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(290.0f, 160.0f));
    ImGui::Begin(tr("Escultura de Blocos", "Voxel Sculpting Tools"));
    ImGui::PopStyleVar();

    // Responsive rows: label | control when wide, stacked when narrow. The
    // brush settings are written straight into m_activeVoxelBrush (the real
    // operation consumed by paint_voxel_ray) on every change.
    static int shapeIdx = 0;
    const char* shapesPt[] = { "Esfera", "Cubo" };
    const char* shapesEn[] = { "Sphere", "Cube" };
    {
        const bool table = UI::beginPropertyRow(tr("Formato do Pincel", "Brush Shape"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##BrushShape", &shapeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? shapesPt : shapesEn, 2)) {
            m_activeVoxelBrush.shape = static_cast<VoxelBrushShape>(shapeIdx);
        }
        UI::endPropertyRow(table);
    }

    static int modeIdx = 0;
    const char* modesPt[] = { "Colocar Blocos", "Destruir Blocos", "Substituir Blocos" };
    const char* modesEn[] = { "Add Voxels", "Remove Voxels", "Replace Voxels" };
    {
        const bool table = UI::beginPropertyRow(tr("Modo de Ação", "Brush Mode"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##BrushMode", &modeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? modesPt : modesEn, 3)) {
            m_activeVoxelBrush.mode = static_cast<VoxelBrushMode>(modeIdx);
        }
        UI::endPropertyRow(table);
    }

    {
        const bool table = UI::beginPropertyRow(tr("Tamanho do Pincel", "Brush Radius"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##BrushRadius", &m_activeVoxelBrush.radius, 0.5f, 25.0f);
        UI::endPropertyRow(table);
    }

    static int voxelTypeIdx = 1;
    const char* materialsPt[] = { "Grama", "Terra", "Pedra", "Areia", "Madeira", "Vidro", "Pedregulho", "Obsidiana", "Basalto", "Neve" };
    const char* materialsEn[] = { "Grass", "Dirt", "Stone", "Sand", "Wood", "Glass", "Cobblestone", "Obsidian", "Basalt", "Snow" };
    {
        const bool table = UI::beginPropertyRow(tr("Tipo de Bloco", "Voxel Material"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##VoxelType", &voxelTypeIdx, (m_currentLanguage == EngineLanguage::PT_BR) ? materialsPt : materialsEn, 10)) {
            m_activeVoxelBrush.voxelType = static_cast<uint16_t>(voxelTypeIdx);
        }
        UI::endPropertyRow(table);
    }

    ImGui::Separator();
    // Paint mode: click/drag in the viewport paints on the selected voxel
    // volume (right button removes). The old "Aplicar Pincel" button used to
    // write settings nobody consumed — painting is now live.
    ImGui::Checkbox(tr("Pintar no viewport (arraste; botão direito remove)", "Paint in viewport (drag; right button removes)"), &m_voxelPaintMode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Com um Mundo de Blocos selecionado, arraste no viewport para esculpir. A ferramenta Select (Q) é usada enquanto o modo de pintura está desligado.", "With a Voxel World selected, drag in the viewport to sculpt. The Select tool (Q) is used while paint mode is off."));
    }

    // Volume actions: generate the terrain from the seed, or clear everything.
    const UUID selectedVolume = (m_selectedEntity.is_valid() && m_editorScene &&
                                 m_editorScene->voxelVolumeComponents.contains(m_selectedEntity.get_id()))
                                    ? m_selectedEntity.get_id()
                                    : UUID{ 0, 0 };
    if (selectedVolume.is_valid()) {
        auto& vol = m_editorScene->voxelVolumeComponents[selectedVolume];
        if (ImGui::Button(tr("Gerar Terreno (semente)", "Generate Terrain (seed)"), ImVec2(-FLT_MIN, 28))) {
            m_voxelStructures.erase(selectedVolume);
            ensure_voxel_volume(selectedVolume, vol.seed, vol.seaLevel);
            m_voxelMeshesDirty.insert(selectedVolume);
        }
        if (ImGui::Button(tr("Limpar Volume", "Clear Volume"), ImVec2(-FLT_MIN, 28))) {
            const auto gridIt = m_voxelStructures.find(selectedVolume);
            if (gridIt != m_voxelStructures.end()) {
                const auto& size = gridIt->second->size();
                for (int x = 0; x < size.x; ++x)
                    for (int y = 0; y < size.y; ++y)
                        for (int z = 0; z < size.z; ++z)
                            gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
                m_voxelMeshesDirty.insert(selectedVolume);
            }
        }
        ImGui::TextDisabled("%s", tr("Selecione o Mundo de Blocos para esculpir", "Select the Voxel World entity to sculpt"));
    } else {
        ImGui::TextDisabled("%s", tr("Selecione uma entidade 'Mundo de Blocos' (ou crie pelo +Add) para esculpir", "Select a Voxel World entity (or create one via +Add) to sculpt"));
    }

    ImGui::End();
#endif
}

void EditorApplication::run_game_build() {
    m_buildLog.clear();
    const auto log = [this](const std::string& line) {
        m_buildLog.push_back(line);
        std::cout << "[Build] " << line << std::endl;
    };
    const std::filesystem::path sourceRoot = std::filesystem::path(VULKANCRAFT_SOURCE_DIR);
    const std::filesystem::path cookedRoot = sourceRoot / "Intermediate" / "DerivedDataCache";
    const std::filesystem::path buildRoot =
        sourceRoot / "Projects" / m_currentProjectName / "Build" / m_currentProjectName;
    const std::filesystem::path binDir = buildRoot / "Bin";
    std::error_code ec;

    if (!m_editorScene || !m_assetPipeline) {
        log("Build failed: no scene open");
        if (m_publishPipeline) m_publishPipeline->fail("no scene open");
        return;
    }
    log("Build started for project '" + m_currentProjectName + "'");
    if (m_publishPipeline) m_publishPipeline->begin(m_currentProjectName);

    // 1. Cook every uncooked asset (same path as the Content Browser).
    size_t imported = 0, failed = 0;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) continue;
        const ImportResult result = m_assetPipeline->import({ asset.sourcePath, cookedRoot, 1 });
        if (result) ++imported;
        else { ++failed; log("  cook failed: " + result.error); }
    }
    log(std::to_string(imported) + " asset(s) cooked, " + std::to_string(failed) + " failed");
    if (m_publishPipeline) m_publishPipeline->cooking_done(imported, failed);

    // 2. Package all cooked assets (Content/<uuid>/<file> + AssetManifest.txt).
    std::vector<UUID> roots;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) roots.push_back(asset.id);
    }
    if (roots.empty()) {
        log("Build failed: no cooked assets to package");
        if (m_publishPipeline) m_publishPipeline->fail("no cooked assets to package");
        return;
    }
    const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, buildRoot);
    if (!packaged) {
        log("Build failed: " + packaged.error);
        if (m_publishPipeline) m_publishPipeline->fail(packaged.error);
        return;
    }
    log(std::to_string(packaged.assets.size()) + " asset(s) packaged");
    if (m_publishPipeline) m_publishPipeline->packaging_done(packaged.assets.size());

    // 3. Save the authored scene as the game's initial scene.
    std::filesystem::create_directories(buildRoot / "Content" / "Scenes", ec);
    if (!m_editorScene->save_to_file((buildRoot / "Content" / "Scenes" / "Initial.scene").string())) {
        log("Build failed: could not save scene");
        if (m_publishPipeline) m_publishPipeline->fail("could not save scene");
        return;
    }
    log("Scene saved to Content/Scenes/Initial.scene");

    // 4. Copy compiled shaders (the game falls back to Content/Shaders).
    const std::filesystem::path shaderSrc = std::filesystem::path(VULKANCRAFT_SHADER_DIR);
    size_t shaders = 0;
    if (std::filesystem::is_directory(shaderSrc)) {
        std::filesystem::create_directories(buildRoot / "Content" / "Shaders", ec);
        for (const auto& entry : std::filesystem::directory_iterator(shaderSrc, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".spv") continue;
            std::filesystem::copy_file(entry.path(), buildRoot / "Content" / "Shaders" / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) ++shaders;
        }
    }
    log(std::to_string(shaders) + " shader(s) copied");

    // 5. Copy the game executable next to the package.
    std::filesystem::create_directories(binDir, ec);
    const std::filesystem::path gameExe = sourceRoot / "build" / "Release" / "VulkanEngineGame.exe";
    if (std::filesystem::is_regular_file(gameExe)) {
        std::filesystem::copy_file(gameExe, binDir / "VulkanEngineGame.exe",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        log("Copied VulkanEngineGame.exe");
    } else {
        log("WARNING: " + gameExe.string() + " not found — build the VulkanEngineGame target first");
    }

    // 6. Package manifest + launcher script.
    std::ofstream manifest(buildRoot / "PackageManifest.txt", std::ios::trunc);
    manifest << "VulkanEngine.Package 1\nproject " << m_currentProjectName
             << "\ninitialScene Content/Scenes/Initial.scene\n";
    std::ofstream launcher(buildRoot / "run_game.bat", std::ios::trunc);
    launcher << "@echo off\ncd /d %~dp0\nBin\\VulkanEngineGame.exe\n";
    if (m_publishPipeline) m_publishPipeline->publishing_done();
    log("Build complete: " + buildRoot.string());
}

void EditorApplication::draw_console_panel() {
    ImGui::Begin(tr("Console", "Console"));

    ImGui::TextColored(ImVec4(0.39f, 0.40f, 0.95f, 1.00f), "%s", tr("VulkanCraft Engine 1.5.0 - Português (Brasil)", "VulkanCraft Engine 1.5.0 - English (US)"));
    ImGui::Text(tr("Velocidade: %.1f FPS  |  Tempo por Quadro: %.2f ms  |  Memória RAM: %zu MB", "Speed: %.1f FPS  |  Frame Time: %.2f ms  |  RAM: %zu MB"), m_fps, m_frameTimeMs, m_ramUsageMb);
    ImGui::Separator();

    if (!m_buildLog.empty()) {
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("Registro do Build:", "Build Log:"));
        for (const std::string& line : m_buildLog) {
            ImGui::TextWrapped("%s", line.c_str());
        }
        ImGui::Separator();
    }

    // Real device status (queried from the physical device at init).
    // NOTE: the translated string carries its own format specifier, so it must
    // be formatted with snprintf BEFORE ImGui::TextColored (passing it as the
    // "%s" arg would leave the inner %s/%zu literals unexpanded).
    {
        char gpuMsg[512];
        snprintf(gpuMsg, sizeof(gpuMsg), tr("[INFO] Placa de Vídeo Vulkan 1.3 Inicializada: %s", "[INFO] Vulkan 1.3 Device Initialized: %s"), m_gpuName.c_str());
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", gpuMsg);
    }

    // Real scene status: the edited scene and the play world (if playing).
    if (m_editorScene) {
        const size_t entityCount = m_editorScene->get_entities().size();
        char sceneMsg[256];
        snprintf(sceneMsg, sizeof(sceneMsg), tr("[INFO] Cena carregada: %zu entidades", "[INFO] Scene loaded: %zu entities"), entityCount);
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", sceneMsg);
    }
    const PlayState state = m_playMode.get_state();
    if (state == PlayState::Play || state == PlayState::Simulate) {
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", tr("[INFO] Jogo interno em execução (Play In Editor)", "[INFO] In-engine game running (Play In Editor)"));
    }

    // Real asset status from the registry.
    {
        size_t cooked = 0;
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.isCooked) ++cooked;
        }
        char assetMsg[256];
        snprintf(assetMsg, sizeof(assetMsg), tr("[INFO] Registro de assets: %zu total, %zu cozidos", "[INFO] Asset registry: %zu total, %zu cooked"), m_assetRegistry.size(), cooked);
        ImGui::TextColored(ImVec4(0.20f, 0.82f, 0.60f, 1.0f), "%s", assetMsg);
    }

    ImGui::End();
}

// ===========================================================================
// Vulkan helpers for the viewport
// ===========================================================================

namespace {

std::vector<uint32_t> read_spv(const char* name) {
    const std::string path = std::string(VULKANCRAFT_SHADER_DIR) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "[Editor] Cannot read shader: " << path << std::endl;
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0 || size % 4 != 0) {
        std::cerr << "[Editor] Invalid SPIR-V size for: " << name << std::endl;
        return {};
    }
    std::vector<uint32_t> spirv(static_cast<size_t>(size) / 4);
    in.read(reinterpret_cast<char*>(spirv.data()), size);
    return spirv;
}

VkShaderModule make_module(VkDevice device, const std::vector<uint32_t>& spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        std::cerr << "[Editor] Failed to create shader module" << std::endl;
        return VK_NULL_HANDLE;
    }
    return module;
}

VkPipeline create_scene_pipeline(VkDevice device, VkRenderPass renderPass, VkPipelineLayout layout,
                                 VkShaderModule vert, VkShaderModule frag,
                                 VkSampleCountFlagBits samples,
                                 bool wireframe, bool depthTest, bool cull, bool withUv = false,
                                 bool noVertexInput = false, bool blend = false,
                                 bool lessOrEqualDepth = false, bool depthBias = false,
                                 bool depthWrite = true,
                                 VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(EditorVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, normal)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, color)) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, uv)) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = noVertexInput ? 0u : 1u;
    vertexInput.pVertexBindingDescriptions = noVertexInput ? nullptr : &binding;
    vertexInput.vertexAttributeDescriptionCount = noVertexInput ? 0u : (withUv ? 4u : 3u);
    vertexInput.pVertexAttributeDescriptions = noVertexInput ? nullptr : attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = wireframe ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = depthBias ? VK_TRUE : VK_FALSE;
    if (depthBias) {
        // Push the grid slightly away so geometry sitting on the plane
        // (y = 0) wins the depth test instead of z-fighting.
        rasterizer.depthBiasConstantFactor = 4.0f;
        rasterizer.depthBiasSlopeFactor = 1.0f;
        rasterizer.depthBiasClamp = 0.0f;
    }

    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = samples;
    multisampling.alphaToCoverageEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = (depthTest && depthWrite) ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = lessOrEqualDepth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        // Premultiplied alpha (the grid is the only blended pipeline; its
        // shader writes vec4(col * alpha, alpha)): ONE / 1-SRC_ALPHA keeps the
        // AA edges clean with no dark fringes and an exact alpha channel.
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlending;
    info.pDynamicState = &dynamicState;
    info.layout = layout;
    info.renderPass = renderPass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        std::cerr << "[Editor] Failed to create scene pipeline" << std::endl;
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// Unit cube (24 vertices with per-face normals, 36 indices). Vertex colors white.
// Cube with per-face UVs: 24 vertices / 36 indices. The UVs matter — the
// material-graph pipeline samples textures with the vertex UVs (location 3),
// so block cubes in the scene would otherwise sample the single (0,0) texel
// and render as a solid color ("sem textura").
void build_cube(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices) {
    const glm::vec3 n[6] = {
        { 0,  0, -1}, { 0,  0,  1}, {-1,  0,  0},
        { 1,  0,  0}, { 0, -1,  0}, { 0,  1,  0}
    };
    struct FaceVert { glm::vec3 pos; glm::vec2 uv; };
    const FaceVert face[6][4] = {
        { FaceVert{{-0.5f, -0.5f, -0.5f}, {0, 0}}, FaceVert{{ 0.5f, -0.5f, -0.5f}, {1, 0}},
          FaceVert{{ 0.5f,  0.5f, -0.5f}, {1, 1}}, FaceVert{{-0.5f,  0.5f, -0.5f}, {0, 1}} }, // -Z
        { FaceVert{{ 0.5f, -0.5f,  0.5f}, {0, 0}}, FaceVert{{-0.5f, -0.5f,  0.5f}, {1, 0}},
          FaceVert{{-0.5f,  0.5f,  0.5f}, {1, 1}}, FaceVert{{ 0.5f,  0.5f,  0.5f}, {0, 1}} }, // +Z
        { FaceVert{{-0.5f, -0.5f,  0.5f}, {0, 0}}, FaceVert{{-0.5f, -0.5f, -0.5f}, {1, 0}},
          FaceVert{{-0.5f,  0.5f, -0.5f}, {1, 1}}, FaceVert{{-0.5f,  0.5f,  0.5f}, {0, 1}} }, // -X
        { FaceVert{{ 0.5f, -0.5f, -0.5f}, {0, 0}}, FaceVert{{ 0.5f, -0.5f,  0.5f}, {1, 0}},
          FaceVert{{ 0.5f,  0.5f,  0.5f}, {1, 1}}, FaceVert{{ 0.5f,  0.5f, -0.5f}, {0, 1}} }, // +X
        { FaceVert{{-0.5f, -0.5f, -0.5f}, {0, 0}}, FaceVert{{ 0.5f, -0.5f, -0.5f}, {1, 0}},
          FaceVert{{ 0.5f, -0.5f,  0.5f}, {1, 1}}, FaceVert{{-0.5f, -0.5f,  0.5f}, {0, 1}} }, // -Y
        { FaceVert{{-0.5f,  0.5f,  0.5f}, {0, 0}}, FaceVert{{ 0.5f,  0.5f,  0.5f}, {1, 0}},
          FaceVert{{ 0.5f,  0.5f, -0.5f}, {1, 1}}, FaceVert{{-0.5f,  0.5f, -0.5f}, {0, 1}} }, // +Y
    };
    verts.clear();
    indices.clear();
    for (int f = 0; f < 6; ++f) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (int c = 0; c < 4; ++c) {
            EditorVertex v;
            v.pos = face[f][c].pos;
            v.normal = n[f];
            v.color = glm::vec3(1.0f);
            v.uv = face[f][c].uv;
            verts.push_back(v);
        }
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

// ---------------------------------------------------------------------------
// Minecraft-style character geometry (player/mob skin → humanoid): head + body
// + arms + legs boxes, each face UV-mapped to the standard 64x64 skin layout
// (legacy 64x32 reuses the right arm/leg regions for the left side; HD skins
// scale the whole layout, so UVs are normalized from the 64-unit grid). One
// unit = 1/16 m → the character is ~2 m tall. Winding matches build_cube
// (CCW + back-cull), so it renders with the material-graph pipeline.
// ---------------------------------------------------------------------------
namespace {
struct CharacterUVRect { float u0, v0, u1, v1; };

void append_character_face(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                           const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           const glm::vec3& d, const glm::vec2& ta, const glm::vec2& tb,
                           const glm::vec2& tc, const glm::vec2& td, const glm::vec3& normal) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const glm::vec3 p[4] = { a, b, c, d };
    const glm::vec2 t[4] = { ta, tb, tc, td };
    for (int i = 0; i < 4; ++i) {
        EditorVertex v;
        v.pos = p[i];
        v.normal = normal;
        v.color = glm::vec3(1.0f);
        v.uv = t[i];
        verts.push_back(v);
    }
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

// Adds the six faces of a box. `right`/`left` map the +X/-X faces,
// `front`/`back` the +Z/-Z (back flipped so the layout reads correctly),
// `top`/`bottom` the +Y/-Y. UVs come from the 64-unit layout grid and are
// normalized by `skinHeight` (64x64 or legacy 64x32).
void append_character_box(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                          float x0, float y0, float z0, float x1, float y1, float z1,
                          float skinHeight, const CharacterUVRect& right,
                          const CharacterUVRect& left, const CharacterUVRect& front,
                          const CharacterUVRect& back, const CharacterUVRect& top,
                          const CharacterUVRect& bottom) {
    const auto uv = [&](const CharacterUVRect& r, float u, float v) {
        return glm::vec2((r.u0 + (r.u1 - r.u0) * u) / 64.0f,
                         (r.v0 + (r.v1 - r.v0) * v) / skinHeight);
    };
    // +Z front
    append_character_face(verts, indices, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 },
                          { x0, y1, z1 }, uv(front, 0, 1), uv(front, 1, 1), uv(front, 1, 0),
                          uv(front, 0, 0), { 0, 0, 1 });
    // -Z back (u flipped)
    append_character_face(verts, indices, { x1, y0, z0 }, { x0, y0, z0 }, { x0, y1, z0 },
                          { x1, y1, z0 }, uv(back, 1, 1), uv(back, 0, 1), uv(back, 0, 0),
                          uv(back, 1, 0), { 0, 0, -1 });
    // +X right
    append_character_face(verts, indices, { x1, y0, z0 }, { x1, y0, z1 }, { x1, y1, z1 },
                          { x1, y1, z0 }, uv(right, 0, 1), uv(right, 1, 1), uv(right, 1, 0),
                          uv(right, 0, 0), { 1, 0, 0 });
    // -X left (u flipped)
    append_character_face(verts, indices, { x0, y0, z1 }, { x0, y0, z0 }, { x0, y1, z0 },
                          { x0, y1, z1 }, uv(left, 1, 1), uv(left, 0, 1), uv(left, 0, 0),
                          uv(left, 1, 0), { -1, 0, 0 });
    // +Y top (z1 -> v0, the front edge of the top rect)
    append_character_face(verts, indices, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 },
                          { x0, y1, z0 }, uv(top, 0, 0), uv(top, 1, 0), uv(top, 1, 1),
                          uv(top, 0, 1), { 0, 1, 0 });
    // -Y bottom
    append_character_face(verts, indices, { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 },
                          { x0, y0, z1 }, uv(bottom, 0, 1), uv(bottom, 1, 1),
                          uv(bottom, 1, 0), uv(bottom, 0, 0), { 0, -1, 0 });
}
} // namespace

void build_character_geometry(float skinHeight, std::vector<EditorVertex>& verts,
                              std::vector<uint32_t>& indices) {
    // Skin layout rects in the 64x64 coordinate grid (authoritative: the
    // reference implementation used by mineatar.io). v is normalized by
    // skinHeight so legacy 64x32 skins work too.
    const CharacterUVRect headTop{ 8, 0, 16, 8 }, headBottom{ 16, 0, 24, 8 },
        headRight{ 0, 8, 8, 16 }, headFront{ 8, 8, 16, 16 },
        headLeft{ 16, 8, 24, 16 }, headBack{ 24, 8, 32, 16 };
    const CharacterUVRect bodyTop{ 20, 16, 28, 20 }, bodyBottom{ 28, 16, 36, 20 },
        bodyRight{ 16, 20, 20, 32 }, bodyFront{ 20, 20, 28, 32 },
        bodyLeft{ 28, 20, 32, 32 }, bodyBack{ 32, 20, 40, 32 };
    const CharacterUVRect rightArmTop{ 44, 16, 48, 20 }, rightArmBottom{ 48, 16, 52, 20 },
        rightArmRight{ 40, 20, 44, 32 }, rightArmFront{ 44, 20, 48, 32 },
        rightArmLeft{ 48, 20, 52, 32 }, rightArmBack{ 52, 20, 56, 32 };
    const CharacterUVRect rightLegTop{ 4, 16, 8, 20 }, rightLegBottom{ 8, 16, 12, 20 },
        rightLegRight{ 0, 20, 4, 32 }, rightLegFront{ 4, 20, 8, 32 },
        rightLegLeft{ 8, 20, 12, 32 }, rightLegBack{ 12, 20, 16, 32 };
    CharacterUVRect leftArmTop = rightArmTop, leftArmBottom = rightArmBottom,
        leftArmRight = rightArmRight, leftArmFront = rightArmFront,
        leftArmLeft = rightArmLeft, leftArmBack = rightArmBack;
    CharacterUVRect leftLegTop = rightLegTop, leftLegBottom = rightLegBottom,
        leftLegRight = rightLegRight, leftLegFront = rightLegFront,
        leftLegLeft = rightLegLeft, leftLegBack = rightLegBack;
    if (skinHeight > 32.5f) {
        // 64x64: dedicated left arm/leg regions.
        leftArmTop = { 36, 48, 40, 52 }; leftArmBottom = { 40, 48, 44, 52 };
        leftArmRight = { 32, 52, 36, 64 }; leftArmFront = { 36, 52, 40, 64 };
        leftArmLeft = { 40, 52, 44, 64 }; leftArmBack = { 44, 52, 48, 64 };
        leftLegTop = { 20, 48, 24, 52 }; leftLegBottom = { 24, 48, 28, 52 };
        leftLegRight = { 16, 52, 20, 64 }; leftLegFront = { 20, 52, 24, 64 };
        leftLegLeft = { 24, 52, 28, 64 }; leftLegBack = { 28, 52, 32, 64 };
    }
    verts.clear();
    indices.clear();
    // Right leg (+X), left leg (-X): 0.25 x 0.75 x 0.25 m.
    append_character_box(verts, indices, 0.0f, 0.0f, -0.125f, 0.25f, 0.75f, 0.125f, skinHeight,
                         rightLegRight, rightLegLeft, rightLegFront, rightLegBack,
                         rightLegTop, rightLegBottom);
    append_character_box(verts, indices, -0.25f, 0.0f, -0.125f, 0.0f, 0.75f, 0.125f, skinHeight,
                         leftLegRight, leftLegLeft, leftLegFront, leftLegBack,
                         leftLegTop, leftLegBottom);
    // Body: 0.5 x 0.75 x 0.25 m.
    append_character_box(verts, indices, -0.25f, 0.75f, -0.125f, 0.25f, 1.5f, 0.125f, skinHeight,
                         bodyRight, bodyLeft, bodyFront, bodyBack, bodyTop, bodyBottom);
    // Right arm (+X), left arm (-X): 0.25 x 0.75 x 0.25 m.
    append_character_box(verts, indices, 0.25f, 0.75f, -0.125f, 0.5f, 1.5f, 0.125f, skinHeight,
                         rightArmRight, rightArmLeft, rightArmFront, rightArmBack,
                         rightArmTop, rightArmBottom);
    append_character_box(verts, indices, -0.5f, 0.75f, -0.125f, -0.25f, 1.5f, 0.125f, skinHeight,
                         leftArmRight, leftArmLeft, leftArmFront, leftArmBack,
                         leftArmTop, leftArmBottom);
    // Head: 0.5 x 0.5 x 0.5 m.
    append_character_box(verts, indices, -0.25f, 1.5f, -0.25f, 0.25f, 2.0f, 0.25f, skinHeight,
                         headRight, headLeft, headFront, headBack, headTop, headBottom);
}

// Appends a cube (from build_cube) transformed by `model`; returns its index range.
std::pair<uint32_t, uint32_t> append_transformed_cube(std::vector<EditorVertex>& verts,
                                                      std::vector<uint32_t>& indices,
                                                      const glm::mat4& model, const glm::vec3& color) {
    std::vector<EditorVertex> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    build_cube(cubeVerts, cubeIndices);
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (const EditorVertex& v : cubeVerts) {
        EditorVertex out;
        out.pos = glm::vec3(model * glm::vec4(v.pos, 1.0f));
        out.normal = glm::normalize(glm::mat3(model) * v.normal);
        out.color = color;
        verts.push_back(out);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (uint32_t i : cubeIndices) indices.push_back(base + i);
    return { indexBase, static_cast<uint32_t>(cubeIndices.size()) };
}

// Cone along +Y from baseY to tipY with `segments` around the axis, then rotated by `rot`.
std::pair<uint32_t, uint32_t> append_cone(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                                          float baseY, float tipY, float radius, int segments,
                                          const glm::mat3& rot, const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const float step = glm::two_pi<float>() / static_cast<float>(segments);
    EditorVertex apex;
    apex.pos = glm::vec3(rot * glm::vec3(0.0f, tipY, 0.0f));
    apex.normal = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
    apex.color = color;
    verts.push_back(apex);
    std::vector<uint32_t> ring;
    for (int s = 0; s < segments; ++s) {
        const float a = step * static_cast<float>(s);
        const glm::vec3 local(std::cos(a) * radius, baseY, std::sin(a) * radius);
        EditorVertex v;
        v.pos = glm::vec3(rot * local);
        const glm::vec3 toApex = glm::normalize(glm::vec3(0.0f, tipY, 0.0f) - local);
        const glm::vec3 tangent(std::sin(a), 0.0f, -std::cos(a));
        v.normal = glm::normalize(rot * glm::normalize(glm::cross(tangent, toApex)));
        v.color = color;
        verts.push_back(v);
        ring.push_back(static_cast<uint32_t>(verts.size()) - 1);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (int s = 0; s < segments; ++s) {
        const uint32_t next = ring[(s + 1) % segments];
        indices.push_back(base);
        indices.push_back(ring[s]);
        indices.push_back(next);
    }
    return { indexBase, static_cast<uint32_t>(segments) * 3u };
}

// Circle of `segments` in the plane perpendicular to `axis` (LINE_LIST).
std::pair<uint32_t, uint32_t> append_ring(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                                          const glm::vec3& axis, float radius, int segments,
                                          const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    glm::vec3 u = glm::normalize(glm::cross(axis, glm::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    glm::vec3 v = glm::normalize(glm::cross(axis, u));
    for (int s = 0; s < segments; ++s) {
        const float a = glm::two_pi<float>() * static_cast<float>(s) / static_cast<float>(segments);
        EditorVertex vert;
        vert.pos = u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        vert.normal = axis;
        vert.color = color;
        verts.push_back(vert);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (int s = 0; s < segments; ++s) {
        indices.push_back(base + static_cast<uint32_t>(s));
        indices.push_back(base + static_cast<uint32_t>((s + 1) % segments));
    }
    return { indexBase, static_cast<uint32_t>(segments) * 2u };
}

// Rotation matrix that maps +Y onto `axis` (used to place gizmo cones along an axis).
glm::mat3 rotation_axis_from_y(const glm::vec3& axis) {
    const glm::vec3 y(0, 1, 0);
    if (glm::length(glm::cross(y, axis)) < 1e-5f) {
        return axis.y > 0 ? glm::mat3(1.0f) : glm::mat3(glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1));
    }
    const float angle = std::acos(glm::clamp(glm::dot(y, axis), -1.0f, 1.0f));
    return glm::mat3(glm::rotate(glm::mat4(1.0f), angle, glm::normalize(glm::cross(y, axis))));
}

glm::mat4 model_from_transform(const TransformComponent& t) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!finite(t.position.x) || !finite(t.position.y) || !finite(t.position.z) ||
        !finite(t.rotation.x) || !finite(t.rotation.y) || !finite(t.rotation.z) ||
        !finite(t.scale.x) || !finite(t.scale.y) || !finite(t.scale.z)) {
        // NaN/inf guard: a non-finite transform would poison the MVP matrix and
        // black out the viewport. Draw this entity at the origin instead.
        return glm::mat4(1.0f);
    }
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}

void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp,
                    const glm::vec4& color, const glm::mat4& model = glm::mat4(1.0f)) {
    const ScenePushConstants pc{ mvp, color, g_fogParams, g_fogColor, model };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}

} // namespace

// ─── Material-graph pipelines (README §16-18: graph → GLSL → Vulkan) ───
namespace {

// Compiles GLSL to SPIR-V via glslc (same tool as the ShaderCompiler target).
std::vector<uint32_t> compile_material_glsl(VkShaderStageFlagBits stage, const std::string& source) {
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "vc_editor_material_tmp";
    const std::string stageArg = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "vert" : "frag";
    const std::filesystem::path srcFile = std::filesystem::path(tmp.string() + "." + stageArg);
    const std::filesystem::path spvFile = std::filesystem::path(tmp.string() + ".spv");
    {
        std::ofstream out(srcFile, std::ios::binary);
        out << source;
    }
    const std::string cmd = "glslc \"" + srcFile.string() + "\" -fshader-stage=" + stageArg +
                            " -o \"" + spvFile.string() + "\" 2>nul";
    const int rc = std::system(cmd.c_str());
    std::vector<uint32_t> spirv;
    if (rc == 0) {
        std::ifstream in(spvFile, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            if (size > 0 && size % 4 == 0) {
                spirv.resize(static_cast<size_t>(size) / 4);
                in.read(reinterpret_cast<char*>(spirv.data()), size);
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove(srcFile, ec);
    std::filesystem::remove(spvFile, ec);
    return spirv;
}

// std140 sizes/alignments for the material params UBO (matching the generated
// GLSL layout): vec3 has 16-byte base alignment, so float/vec2 after it must be
// written at padded offsets.
size_t material_std140_size(Rendering::MaterialValueType type) {
    switch (type) {
        case Rendering::MaterialValueType::Bool: return 4;
        case Rendering::MaterialValueType::Float: return 4;
        case Rendering::MaterialValueType::Vec2: return 8;
        case Rendering::MaterialValueType::Vec3: return 12;
        case Rendering::MaterialValueType::Vec4: return 16;
        case Rendering::MaterialValueType::Texture2D: return 16;
    }
    return 4;
}

size_t material_std140_alignment(Rendering::MaterialValueType type) {
    switch (type) {
        case Rendering::MaterialValueType::Bool: return 4;
        case Rendering::MaterialValueType::Float: return 4;
        case Rendering::MaterialValueType::Vec2: return 8;
        case Rendering::MaterialValueType::Vec3: return 16;
        case Rendering::MaterialValueType::Vec4: return 16;
        case Rendering::MaterialValueType::Texture2D: return 16;
    }
    return 4;
}

size_t align_material_offset(size_t offset, size_t alignment) {
    return (offset + alignment - 1) / alignment * alignment;
}

void write_ubo_value(std::byte* dst, Rendering::MaterialValueType type, const Rendering::MaterialValue& value) {
    std::memset(dst, 0, material_std140_size(type));
    switch (type) {
        case Rendering::MaterialValueType::Bool:
            *reinterpret_cast<uint32_t*>(dst) = std::holds_alternative<bool>(value) && std::get<bool>(value) ? 1u : 0u;
            break;
        case Rendering::MaterialValueType::Float:
            if (std::holds_alternative<float>(value)) *reinterpret_cast<float*>(dst) = std::get<float>(value);
            break;
        case Rendering::MaterialValueType::Vec2:
            if (std::holds_alternative<glm::vec2>(value)) {
                const glm::vec2 v = std::get<glm::vec2>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Vec3:
            if (std::holds_alternative<glm::vec3>(value)) {
                const glm::vec3 v = std::get<glm::vec3>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Vec4:
            if (std::holds_alternative<glm::vec4>(value)) {
                const glm::vec4 v = std::get<glm::vec4>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Texture2D:
            break;
    }
}

// Content hash of a material graph → rebuild pipelines when the graph changes.
uint64_t hash_material_graph(const Rendering::MaterialGraph& graph) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& node : graph.nodes()) {
        mix(&node.id, sizeof(node.id));
        const auto kind = static_cast<uint8_t>(node.kind);
        mix(&kind, 1);
        const auto outputType = static_cast<uint8_t>(node.outputType);
        mix(&outputType, 1);
        mix(node.label.data(), node.label.size());
        mix(node.parameter.data(), node.parameter.size());
        std::visit([&](const auto& v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                mix(v.data(), v.size());
            } else {
                mix(&v, sizeof(v));
            }
        }, node.value);
    }
    for (const auto& p : graph.parameters()) {
        mix(p.name.data(), p.name.size());
        const auto type = static_cast<uint8_t>(p.type);
        mix(&type, 1);
        mix(&p.exposed, 1);
    }
    return h;
}

// Default graph for a cooked MaterialAsset: PBR params exposed as UBO members.
Rendering::MaterialGraph material_graph_from_asset(const MaterialAsset& mat) {
    Rendering::MaterialGraph graph;
    graph.define_parameter({ "Albedo", Rendering::MaterialValueType::Vec3, mat.albedo, true });
    graph.define_parameter({ "Roughness", Rendering::MaterialValueType::Float, mat.roughness, true });
    graph.define_parameter({ "Metallic", Rendering::MaterialValueType::Float, mat.metallic, true });
    graph.define_parameter({ "Emissive", Rendering::MaterialValueType::Vec3,
                             mat.emissiveColor * mat.emissiveIntensity, true });
    const auto roughness = graph.add_parameter("Roughness");
    const auto metallic = graph.add_parameter("Metallic");
    const auto emissive = graph.add_parameter("Emissive");
    const auto baseOut = graph.add_output("BaseColor", Rendering::MaterialValueType::Vec3);
    const auto roughOut = graph.add_output("Roughness", Rendering::MaterialValueType::Float);
    const auto metalOut = graph.add_output("Metallic", Rendering::MaterialValueType::Float);
    const auto emisOut = graph.add_output("Emissive", Rendering::MaterialValueType::Vec3);
    if (mat.albedoMapID.is_valid()) {
        // Albedo map: a TextureSample drives BaseColor (RGBA→RGB at the sink,
        // same path the Material editor uses). The pipeline binds the texture
        // by asset UUID, so any cooked texture works.
        const auto tex = graph.add_texture_sample("Albedo Map");
        if (auto* node = graph.find_node(tex)) node->value = mat.albedoMapID.to_string();
        (void)graph.connect(tex, baseOut, 0);
    } else {
        const auto albedo = graph.add_parameter("Albedo");
        (void)graph.connect(albedo, baseOut, 0);
    }
    (void)graph.connect(roughness, roughOut, 0);
    (void)graph.connect(metallic, metalOut, 0);
    (void)graph.connect(emissive, emisOut, 0);
    return graph;
}

} // namespace

uint32_t EditorApplication::find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {



// ===========================================================================
// Vulkan Infrastructure (split from EditorApplication.cpp)
// ===========================================================================
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

void EditorApplication::create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                     VkMemoryPropertyFlags memProps, VkImage& image, VkDeviceMemory& memory,
                                     uint32_t mipLevels /* = 1 */,
                                     VkSampleCountFlagBits samples /* = VK_SAMPLE_COUNT_1_BIT */) {
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = { w, h, 1 };
    info.mipLevels = std::max(mipLevels, 1u);
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = samples;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &info, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image");
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(m_device, image, &requirements);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, memProps);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate image memory");
    }
    vkBindImageMemory(m_device, image, memory, 0);
}

VkImageView EditorApplication::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                                 uint32_t mipLevels /* = 1 */) {
    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = { aspect, 0, std::max(mipLevels, 1u), 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_device, &info, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image view");
    }
    return view;
}

void EditorApplication::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                      VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &info, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, props);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }
    vkBindBufferMemory(m_device, buffer, memory, 0);
}

void EditorApplication::destroy_buffer(GPUBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = GPUBuffer{};
}

void EditorApplication::transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                                VkImageAspectFlags aspect, uint32_t baseMipLevel /* = 0 */,
                                                uint32_t levelCount /* = 1 */) {
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, baseMipLevel, std::max(levelCount, 1u), 0, 1 };
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkCommandBuffer EditorApplication::begin_single_time_commands() {
    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void EditorApplication::end_single_time_commands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

// ===========================================================================
// Viewport initialization
// ===========================================================================

void EditorApplication::init_offscreen_target() {
    // Render passes and sampler are size-independent and referenced by the
    // pipelines, so they are created once and kept until final cleanup.
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // Clamp MSAA to what the device actually supports (4x on virtually all
    // desktop GPUs; falls back to 2x/1x on weak/software adapters).
    if (m_physicalDevice != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts;
        m_viewportSamples =
            (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT :
            (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT :
            VK_SAMPLE_COUNT_1_BIT;
    }

    // Viewport scene render pass with MSAA + resolve:
    //   0 = scene color (multisampled, transient)
    //   1 = scene depth (multisampled, transient)
    //   2 = resolve color (1x, stored, sampled by ImGui)
    VkAttachmentDescription colorMsaa{};
    colorMsaa.format = colorFormat;
    colorMsaa.samples = m_viewportSamples;
    colorMsaa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorMsaa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorMsaa.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorMsaa.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorMsaa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorMsaa.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthMsaa{};
    depthMsaa.format = depthFormat;
    depthMsaa.samples = m_viewportSamples;
    depthMsaa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthMsaa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthMsaa.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthMsaa.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthMsaa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthMsaa.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription resolveColor{};
    resolveColor.format = colorFormat;
    resolveColor.samples = VK_SAMPLE_COUNT_1_BIT;
    resolveColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolveColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolveColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolveColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolveColor.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    resolveColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resolveRef{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = &resolveRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkAttachmentDescription attachments[3] = { colorMsaa, depthMsaa, resolveColor };
    VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 3;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_offscreen.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create MSAA offscreen render pass");
    }

    // Pick render pass: color-ID pass stays 1x with its OWN 1x depth (the
    // scene depth is now multisampled and cannot be shared with a 1x pass).
    VkAttachmentDescription pickColor{};
    pickColor.format = colorFormat;
    pickColor.samples = VK_SAMPLE_COUNT_1_BIT;
    pickColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pickColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    pickColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pickColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickColor.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pickColor.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentDescription pickDepth{};
    pickDepth.format = depthFormat;
    pickDepth.samples = VK_SAMPLE_COUNT_1_BIT;
    pickDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pickDepth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pickDepth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickDepth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pickDepth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference pickColorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference pickDepthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription pickSubpass{};
    pickSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pickSubpass.colorAttachmentCount = 1;
    pickSubpass.pColorAttachments = &pickColorRef;
    pickSubpass.pDepthStencilAttachment = &pickDepthRef;
    VkAttachmentDescription pickAttachments[2] = { pickColor, pickDepth };
    VkRenderPassCreateInfo pickRpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    pickRpInfo.attachmentCount = 2;
    pickRpInfo.pAttachments = pickAttachments;
    pickRpInfo.subpassCount = 1;
    pickRpInfo.pSubpasses = &pickSubpass;
    if (vkCreateRenderPass(m_device, &pickRpInfo, nullptr, &m_offscreen.pickRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pick render pass");
    }

    // Material-texture sampler: trilinear mipmapping + 16x anisotropic
    // filtering so surfaces seen at oblique angles (e.g. ~75°) stop shimmering
    // and aliasing. The device enables samplerAnisotropy at creation
    // (Engine.cpp), so this is safe on every supported adapter.
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_offscreen.sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen sampler");
    }

    create_offscreen_buffers(800, 600);
    create_shadow_map();
    init_thumbnail_target();
    init_block_cube();
}

// Creates the size-dependent resources (images, views, framebuffers, staging).
// Render passes and the sampler are kept across resizes.
void EditorApplication::create_offscreen_buffers(uint32_t w, uint32_t h) {
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // Resolve target (1x) — what ImGui samples.
    create_image(w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.colorImage, m_offscreen.colorMemory);
    m_offscreen.colorView = create_image_view(m_offscreen.colorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    // Scene color, multisampled.
    create_image(w, h, colorFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.msaaColorImage, m_offscreen.msaaColorMemory,
                 1, m_viewportSamples);
    m_offscreen.msaaColorView =
        create_image_view(m_offscreen.msaaColorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    // Scene depth, multisampled.
    create_image(w, h, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.depthImage, m_offscreen.depthMemory,
                 1, m_viewportSamples);
    m_offscreen.depthView = create_image_view(m_offscreen.depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Pick buffers stay 1x, with their own 1x depth.
    create_image(w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.pickImage, m_offscreen.pickMemory);
    m_offscreen.pickView = create_image_view(m_offscreen.pickImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    create_image(w, h, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_offscreen.pickDepthImage, m_offscreen.pickDepthMemory);
    m_offscreen.pickDepthView =
        create_image_view(m_offscreen.pickDepthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(w) * h * 4;
    create_buffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_offscreen.pickStagingBuffer, m_offscreen.pickStagingMemory);

    // Bring freshly created images into the layouts their render passes expect.
    {
        VkCommandBuffer transitionCmd = begin_single_time_commands();
        transition_image_layout(transitionCmd, m_offscreen.colorImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_image_layout(transitionCmd, m_offscreen.pickImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition_image_layout(transitionCmd, m_offscreen.pickDepthImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        end_single_time_commands(transitionCmd);
    }

    // Scene framebuffer: [MSAA color, MSAA depth, resolve].
    VkImageView attachments[3] = { m_offscreen.msaaColorView, m_offscreen.depthView, m_offscreen.colorView };
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_offscreen.renderPass;
    fbInfo.attachmentCount = 3;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_offscreen.framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen framebuffer");
    }

    // Pick framebuffer: [pick color, pick depth] — both 1x.
    VkImageView pickAttachments[2] = { m_offscreen.pickView, m_offscreen.pickDepthView };
    VkFramebufferCreateInfo pickFbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    pickFbInfo.renderPass = m_offscreen.pickRenderPass;
    pickFbInfo.attachmentCount = 2;
    pickFbInfo.pAttachments = pickAttachments;
    pickFbInfo.width = w;
    pickFbInfo.height = h;
    pickFbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &pickFbInfo, nullptr, &m_offscreen.pickFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pick framebuffer");
    }

    m_offscreen.width = w;
    m_offscreen.height = h;
    m_offscreen.imguiTextureID = ImGui_ImplVulkan_AddTexture(
        m_offscreen.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Sun shadow map: fixed-size depth-only target, rebuilt once at startup. The
// viewport records a shadow pass before the scene pass and the material
// pipelines sample this map through the comparison sampler.
void EditorApplication::create_shadow_map() {
    destroy_shadow_map();
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    create_image(m_shadowMap.size, m_shadowMap.size, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_shadowMap.image, m_shadowMap.memory);
    m_shadowMap.view = create_image_view(m_shadowMap.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Depth-only render pass: the map ends in SHADER_READ_ONLY so the material
    // shaders can sample it without an extra transition.
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_shadowMap.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow render pass");
    }
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_shadowMap.renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_shadowMap.view;
    fbInfo.width = m_shadowMap.size;
    fbInfo.height = m_shadowMap.size;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_shadowMap.framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow framebuffer");
    }

    // Comparison sampler: depth values are fetched raw (compareEnable is
    // inert for non-shadow sampler types) and the shader does the PCF-style
    // bias compare — same arrangement as the game's shadow path.
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowMap.sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow sampler");
    }

    // Position-only shaders (vertex reads all EditorVertex attributes to keep
    // the same vertex input state; only pos is used).
    const std::string vertSrc =
        "#version 450\n"
        "layout(push_constant) uniform Push { mat4 mvp; } pc;\n"
        "layout(location = 0) in vec3 inPos;\n"
        "layout(location = 1) in vec3 inNormal;\n"
        "layout(location = 2) in vec3 inColor;\n"
        "layout(location = 3) in vec2 inUv;\n"
        "void main() { gl_Position = pc.mvp * vec4(inPos, 1.0); }\n";
    const std::string fragSrc = "#version 450\nvoid main() {}\n";
    const std::vector<uint32_t> vertSpv = compile_material_glsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    const std::vector<uint32_t> fragSpv = compile_material_glsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vertSpv.empty() || fragSpv.empty()) {
        throw std::runtime_error("Failed to compile shadow shaders");
    }
    m_shadowMap.vertShader = make_module(m_device, vertSpv);
    m_shadowMap.fragShader = make_module(m_device, fragSpv);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_shadowMap.pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline layout");
    }

    // Depth-only graphics pipeline (no color attachment, depth write on).
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_shadowMap.vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_shadowMap.fragShader;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(EditorVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, normal)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, color)) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, uv)) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.attachmentCount = 0;
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlending;
    info.pDynamicState = &dynamicState;
    info.layout = m_shadowMap.pipelineLayout;
    info.renderPass = m_shadowMap.renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_shadowMap.pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline");
    }
}

void EditorApplication::destroy_shadow_map() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_shadowMap.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_shadowMap.pipeline, nullptr);
    if (m_shadowMap.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_shadowMap.pipelineLayout, nullptr);
    if (m_shadowMap.vertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_shadowMap.vertShader, nullptr);
    if (m_shadowMap.fragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_shadowMap.fragShader, nullptr);
    if (m_shadowMap.sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_shadowMap.sampler, nullptr);
    if (m_shadowMap.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_shadowMap.framebuffer, nullptr);
    if (m_shadowMap.renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_shadowMap.renderPass, nullptr);
    if (m_shadowMap.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_shadowMap.view, nullptr);
    if (m_shadowMap.image != VK_NULL_HANDLE) vkDestroyImage(m_device, m_shadowMap.image, nullptr);
    if (m_shadowMap.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_shadowMap.memory, nullptr);
    m_shadowMap = EditorShadowMap{};
}

void EditorApplication::recreate_offscreen_if_needed(uint32_t w, uint32_t h) {
    if (m_offscreen.framebuffer != VK_NULL_HANDLE && m_offscreen.width == w && m_offscreen.height == h) {
        return;
    }
    if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device); // old attachments are in flight
    cleanup_offscreen_target();
    create_offscreen_buffers(w, h);
    // The offscreen cleanup also destroys the shadow map (size-independent
    // resources live together in cleanup_offscreen_target); bring it back so a
    // resize never silently kills the editor shadows.
    if (m_shadowMap.pipeline == VK_NULL_HANDLE) create_shadow_map();
}

void EditorApplication::cleanup_offscreen_target() {
    if (m_device == VK_NULL_HANDLE) return;
    destroy_shadow_map();
    if (m_offscreen.imguiTextureID != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_offscreen.imguiTextureID);
        m_offscreen.imguiTextureID = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickStagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_offscreen.pickStagingBuffer, nullptr);
        m_offscreen.pickStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickStagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickStagingMemory, nullptr);
        m_offscreen.pickStagingMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_offscreen.pickFramebuffer, nullptr);
        m_offscreen.pickFramebuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.pickView, nullptr);
        m_offscreen.pickView = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.pickImage, nullptr);
        m_offscreen.pickImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickMemory, nullptr);
        m_offscreen.pickMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickDepthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.pickDepthView, nullptr);
        m_offscreen.pickDepthView = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickDepthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.pickDepthImage, nullptr);
        m_offscreen.pickDepthImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickDepthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickDepthMemory, nullptr);
        m_offscreen.pickDepthMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_offscreen.framebuffer, nullptr);
        m_offscreen.framebuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.depthView, nullptr);
        m_offscreen.depthView = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.depthImage, nullptr);
        m_offscreen.depthImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.depthMemory, nullptr);
        m_offscreen.depthMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.msaaColorView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.msaaColorView, nullptr);
        m_offscreen.msaaColorView = VK_NULL_HANDLE;
    }
    if (m_offscreen.msaaColorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.msaaColorImage, nullptr);
        m_offscreen.msaaColorImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.msaaColorMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.msaaColorMemory, nullptr);
        m_offscreen.msaaColorMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.colorView, nullptr);
        m_offscreen.colorView = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.colorImage, nullptr);
        m_offscreen.colorImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.colorMemory, nullptr);
        m_offscreen.colorMemory = VK_NULL_HANDLE;
    }
    m_offscreen.width = 0;
    m_offscreen.height = 0;
}

void EditorApplication::init_scene_pipeline() {
    m_viewportVertShader = make_module(m_device, read_spv("editor_viewport.vert.spv"));
    m_viewportFragShader = make_module(m_device, read_spv("editor_viewport.frag.spv"));
    m_pickFragShader = make_module(m_device, read_spv("editor_pick.frag.spv"));
    if (!m_viewportVertShader || !m_viewportFragShader || !m_pickFragShader) {
        throw std::runtime_error("Editor viewport shaders failed to compile (run the compile_shaders target)");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ScenePushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_scenePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scene pipeline layout");
    }

    m_scenePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                            m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                            false, true, true);
    m_wireframePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                                m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                                true, false, false);
    m_gizmoPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                            m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                            false, false, false);
    // Pick stays 1x (its render pass is 1x — sample counts must match).
    m_pickPipeline = create_scene_pipeline(m_device, m_offscreen.pickRenderPass, m_scenePipelineLayout,
                                           m_viewportVertShader, m_pickFragShader, VK_SAMPLE_COUNT_1_BIT,
                                           false, true, true);
    if (!m_scenePipeline || !m_wireframePipeline || !m_gizmoPipeline || !m_pickPipeline) {
        throw std::runtime_error("Failed to create viewport pipelines");
    }

    // Analytic infinite grid: fullscreen triangle, no vertex buffer. Per-family
    // X/Z screen-density filtering with constant screen-space line widths,
    // premultiplied alpha output; tests depth (LEQUAL) but does not write it.
    m_gridVertShader = make_module(m_device, read_spv("editor_grid.vert.spv"));
    m_gridFragShader = make_module(m_device, read_spv("editor_grid.frag.spv"));
    if (!m_gridVertShader || !m_gridFragShader) {
        throw std::runtime_error("Grid shaders failed to compile (run the compile_shaders target)");
    }
    VkPushConstantRange gridRange{};
    gridRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    gridRange.offset = 0;
    gridRange.size = sizeof(GridPushConstants);
    VkPipelineLayoutCreateInfo gridLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    gridLayoutInfo.pushConstantRangeCount = 1;
    gridLayoutInfo.pPushConstantRanges = &gridRange;
    if (vkCreatePipelineLayout(m_device, &gridLayoutInfo, nullptr, &m_gridPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create grid pipeline layout");
    }
    m_gridPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_gridPipelineLayout,
                                           m_gridVertShader, m_gridFragShader, m_viewportSamples,
                                           false /*wireframe*/, true /*depthTest*/, false /*cull*/,
                                           false /*withUv*/, true /*noVertexInput*/, true /*blend*/,
                                           true /*lessOrEqualDepth*/, true /*depthBias*/,
                                           false /*depthWrite*/);
    if (!m_gridPipeline) {
        throw std::runtime_error("Failed to create grid pipeline");
    }

    // Sky pass (Clima panel): the engine's procedural day/night sky, drawn as
    // the first fullscreen layer of the viewport. Push constants: mvp +
    // cameraPos + sunDirection + sunColor + environment = 128 bytes exactly.
    m_skyVertShader = make_module(m_device, read_spv("sky.vert.spv"));
    m_skyFragShader = make_module(m_device, read_spv("sky.frag.spv"));
    if (m_skyVertShader && m_skyFragShader) {
        VkPushConstantRange skyRange{};
        skyRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        skyRange.offset = 0;
        skyRange.size = sizeof(SkyPushConstants);
        VkPipelineLayoutCreateInfo skyLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        skyLayoutInfo.pushConstantRangeCount = 1;
        skyLayoutInfo.pPushConstantRanges = &skyRange;
        if (vkCreatePipelineLayout(m_device, &skyLayoutInfo, nullptr, &m_skyPipelineLayout) == VK_SUCCESS) {
            // Depth test LEQUAL against the cleared depth (1.0), no depth write,
            // opaque — entities and the grid draw over it with LESS.
            m_skyPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_skyPipelineLayout,
                                                  m_skyVertShader, m_skyFragShader, m_viewportSamples,
                                                  false /*wireframe*/, true /*depthTest*/, false /*cull*/,
                                                  false /*withUv*/, true /*noVertexInput*/, false /*blend*/,
                                                  true /*lessOrEqualDepth*/, false /*depthBias*/);
        }
    }

    // Hair strands: LINE_LIST with depth test (the editor viewport shaders
    // color strands per-vertex; the geometry is rebuilt every frame).
    m_hairPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                           m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                           true /*wireframe*/, true /*depthTest*/, false /*cull*/);

    // Gaussian splats: soft point clouds (POINT_LIST, alpha blend, depth test
    // without depth write so splats composite like real gaussian primitives).
    m_splatVertShader = make_module(m_device, read_spv("editor_splat.vert.spv"));
    m_splatFragShader = make_module(m_device, read_spv("editor_splat.frag.spv"));
    if (m_splatVertShader && m_splatFragShader) {
        VkPushConstantRange splatRange{};
        splatRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        splatRange.offset = 0;
        splatRange.size = sizeof(SplatPushConstants);
        VkPipelineLayoutCreateInfo splatLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        splatLayoutInfo.pushConstantRangeCount = 1;
        splatLayoutInfo.pPushConstantRanges = &splatRange;
        if (vkCreatePipelineLayout(m_device, &splatLayoutInfo, nullptr, &m_splatPipelineLayout) == VK_SUCCESS) {
            m_splatPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_splatPipelineLayout,
                                                    m_splatVertShader, m_splatFragShader, m_viewportSamples,
                                                    false /*wireframe*/, true /*depthTest*/, false /*cull*/,
                                                    false /*withUv*/, false /*noVertexInput*/, true /*blend*/,
                                                    false /*lessOrEqual*/, false /*depthBias*/,
                                                    false /*depthWrite*/, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
        }
    }

    // Env probe: reflective sphere sampling the captured cubemap.
    m_envSphereVertShader = make_module(m_device, read_spv("editor_envsphere.vert.spv"));
    m_envSphereFragShader = make_module(m_device, read_spv("editor_envsphere.frag.spv"));
    if (m_envSphereVertShader && m_envSphereFragShader) {
        VkDescriptorSetLayoutBinding envBinding{};
        envBinding.binding = 0;
        envBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        envBinding.descriptorCount = 1;
        envBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo envDescInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        envDescInfo.bindingCount = 1;
        envDescInfo.pBindings = &envBinding;
        vkCreateDescriptorSetLayout(m_device, &envDescInfo, nullptr, &m_envSphereDescLayout);
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_envSphereDescPool);
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = m_envSphereDescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_envSphereDescLayout;
        vkAllocateDescriptorSets(m_device, &allocInfo, &m_envCapture.descriptorSet);
        VkPushConstantRange envRange{};
        envRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        envRange.offset = 0;
        envRange.size = sizeof(EnvSpherePushConstants);
        VkPipelineLayoutCreateInfo envLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        envLayoutInfo.setLayoutCount = 1;
        envLayoutInfo.pSetLayouts = &m_envSphereDescLayout;
        envLayoutInfo.pushConstantRangeCount = 1;
        envLayoutInfo.pPushConstantRanges = &envRange;
        if (vkCreatePipelineLayout(m_device, &envLayoutInfo, nullptr, &m_envSpherePipelineLayout) == VK_SUCCESS) {
            m_envSpherePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_envSpherePipelineLayout,
                                                        m_envSphereVertShader, m_envSphereFragShader, m_viewportSamples,
                                                        false /*wireframe*/, true /*depthTest*/, false /*cull*/);
        }
    }
}

void EditorApplication::init_geometry_buffers() {
    // Cube
    std::vector<EditorVertex> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    generate_cube_geometry(cubeVerts, cubeIndices);
    m_cubeIndexCount = static_cast<uint32_t>(cubeIndices.size());
    VkDeviceSize cubeVBsize = sizeof(EditorVertex) * cubeVerts.size();
    VkDeviceSize cubeIBsize = sizeof(uint32_t) * cubeIndices.size();
    create_buffer(cubeVBsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cubeVB.buffer, m_cubeVB.memory);
    create_buffer(cubeIBsize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cubeIB.buffer, m_cubeIB.memory);
    safe_map_and_copy(m_device, m_cubeVB.memory, 0, cubeVBsize, cubeVerts.data());
    safe_map_and_copy(m_device, m_cubeIB.memory, 0, cubeIBsize, cubeIndices.data());

    // (No grid vertex buffer: the grid is now analytic — a fullscreen
    // triangle rasterized by editor_grid.vert/frag with fwidth AA.)

    // Light icon (octahedron edges)
    std::vector<EditorVertex> lightVerts;
    generate_light_icon(lightVerts);
    m_lightIconVertexCount = static_cast<uint32_t>(lightVerts.size());
    VkDeviceSize lightSize = sizeof(EditorVertex) * lightVerts.size();
    create_buffer(lightSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_lightIconVB.buffer, m_lightIconVB.memory);
    safe_map_and_copy(m_device, m_lightIconVB.memory, 0, lightSize, lightVerts.data());

    // Camera icon (pyramid edges)
    std::vector<EditorVertex> cameraVerts;
    generate_camera_icon(cameraVerts);
    m_cameraIconVertexCount = static_cast<uint32_t>(cameraVerts.size());
    VkDeviceSize cameraSize = sizeof(EditorVertex) * cameraVerts.size();
    create_buffer(cameraSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cameraIconVB.buffer, m_cameraIconVB.memory);
    safe_map_and_copy(m_device, m_cameraIconVB.memory, 0, cameraSize, cameraVerts.data());

    // Env probe preview sphere (UV sphere).
    {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        constexpr int kSeg = 32, kRings = 18;
        for (int r = 0; r <= kRings; ++r) {
            const float v = static_cast<float>(r) / kRings;
            const float phi = v * glm::pi<float>();
            for (int s = 0; s <= kSeg; ++s) {
                const float u = static_cast<float>(s) / kSeg;
                const float theta = u * glm::two_pi<float>();
                const glm::vec3 p(std::sin(phi) * std::cos(theta), std::cos(phi),
                                  std::sin(phi) * std::sin(theta));
                EditorVertex vert;
                vert.pos = p;
                vert.normal = p;
                vert.color = glm::vec3(1.0f);
                vert.uv = { u, 1.0f - v };
                verts.push_back(vert);
            }
        }
        for (int r = 0; r < kRings; ++r) {
            for (int s = 0; s < kSeg; ++s) {
                const uint32_t a = static_cast<uint32_t>(r * (kSeg + 1) + s);
                const uint32_t b = a + static_cast<uint32_t>(kSeg + 1);
                indices.push_back(a); indices.push_back(b); indices.push_back(a + 1);
                indices.push_back(a + 1); indices.push_back(b); indices.push_back(b + 1);
            }
        }
        m_envSphereIndexCount = static_cast<uint32_t>(indices.size());
        const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize is = sizeof(uint32_t) * indices.size();
        create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_envSphereVB.buffer, m_envSphereVB.memory);
        create_buffer(is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_envSphereIB.buffer, m_envSphereIB.memory);
        safe_map_and_copy(m_device, m_envSphereVB.memory, 0, vs, verts.data());
        safe_map_and_copy(m_device, m_envSphereIB.memory, 0, is, indices.data());
    }

    // Decal quad (1x1, facing +Z, UVs 0..1).
    {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        const float h = 0.5f;
        verts.push_back({ { -h, -h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 0.0f, 1.0f } });
        verts.push_back({ { h, -h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 1.0f, 1.0f } });
        verts.push_back({ { h, h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 1.0f, 0.0f } });
        verts.push_back({ { -h, h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 0.0f, 0.0f } });
        indices = { 0, 1, 2, 0, 2, 3 };
        m_decalIndexCount = static_cast<uint32_t>(indices.size());
        const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize is = sizeof(uint32_t) * indices.size();
        create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_decalVB.buffer, m_decalVB.memory);
        create_buffer(is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_decalIB.buffer, m_decalIB.memory);
        safe_map_and_copy(m_device, m_decalVB.memory, 0, vs, verts.data());
        safe_map_and_copy(m_device, m_decalIB.memory, 0, is, indices.data());
    }

    // Gizmo geometry (all modes)
    generate_gizmo_geometry();
}

void EditorApplication::generate_cube_geometry(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices) {
    build_cube(verts, indices);
}

void EditorApplication::generate_light_icon(std::vector<EditorVertex>& verts) {
    verts.clear();
    const glm::vec3 color(1.0f);
    const float r = 0.45f;
    const glm::vec3 pts[6] = {
        { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r }, { 0, 0, -r }
    };
    const int edges[12][2] = {
        {0, 2}, {0, 3}, {0, 4}, {0, 5},
        {1, 2}, {1, 3}, {1, 4}, {1, 5},
        {2, 4}, {4, 3}, {3, 5}, {5, 2}
    };
    for (const auto& e : edges) {
        EditorVertex a, b;
        a.pos = pts[e[0]]; a.normal = { 0, 1, 0 }; a.color = color;
        b.pos = pts[e[1]]; b.normal = { 0, 1, 0 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_camera_icon(std::vector<EditorVertex>& verts) {
    verts.clear();
    const glm::vec3 color(1.0f);
    // Pyramid pointing +Z: apex behind, near rectangle in front.
    const glm::vec3 apex(0.0f, 0.0f, -0.55f);
    const glm::vec3 corners[4] = {
        { -0.42f, -0.30f, 0.45f }, { 0.42f, -0.30f, 0.45f },
        { 0.42f, 0.30f, 0.45f }, { -0.42f, 0.30f, 0.45f }
    };
    const int edges[8][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {0, 4}, {1, 4}, {2, 4}, {3, 4}
    };
    glm::vec3 pts[5];
    pts[0] = corners[0]; pts[1] = corners[1]; pts[2] = corners[2]; pts[3] = corners[3]; pts[4] = apex;
    for (const auto& e : edges) {
        EditorVertex a, b;
        a.pos = pts[e[0]]; a.normal = { 0, 0, 1 }; a.color = color;
        b.pos = pts[e[1]]; b.normal = { 0, 0, 1 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_gizmo_geometry() {
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    const float shaftLen = 1.55f;
    const float ringRadius = 1.45f;

    // Shafts (LINE_LIST) + cones (translate) + rings (rotate) + cubes (scale).
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 dir = kAxisDirs[axis];
        const glm::vec3 color = kAxisColors[axis];

        // Shaft from origin to 82% of the length.
        const uint32_t shaftBase = static_cast<uint32_t>(verts.size());
        EditorVertex origin, tip;
        origin.pos = glm::vec3(0.0f); origin.normal = dir; origin.color = color;
        tip.pos = dir * (shaftLen * 0.82f); tip.normal = dir; tip.color = color;
        verts.push_back(origin);
        verts.push_back(tip);
        m_gizmoShaftRanges[axis] = { static_cast<uint32_t>(indices.size()), 2 };
        indices.push_back(shaftBase);
        indices.push_back(shaftBase + 1);

        // Translate arrow cone.
        {
            const auto [offset, count] = append_cone(verts, indices, shaftLen * 0.72f, shaftLen, 0.09f, 12,
                                                     rotation_axis_from_y(dir), color);
            m_gizmoArrowRanges[axis] = GizmoDrawRange{ offset, count };
        }

        // Rotate ring (perpendicular to the axis).
        {
            const auto [offset, count] = append_ring(verts, indices, dir, ringRadius, 48, color);
            m_gizmoRingRanges[axis] = GizmoDrawRange{ offset, count };
        }

        // Scale tip cube at the end of the shaft.
        {
            glm::mat4 tipModel = glm::translate(glm::mat4(1.0f), dir * shaftLen);
            tipModel = glm::scale(tipModel, glm::vec3(0.17f));
            const auto [offset, count] = append_transformed_cube(verts, indices, tipModel, color);
            m_gizmoTipRanges[axis] = GizmoDrawRange{ offset, count };
        }
    }

    VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gizmoVB.buffer, m_gizmoVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gizmoIB.buffer, m_gizmoIB.memory);
    safe_map_and_copy(m_device, m_gizmoVB.memory, 0, vbSize, verts.data());
    safe_map_and_copy(m_device, m_gizmoIB.memory, 0, ibSize, indices.data());
}

// ===========================================================================
// Viewport rendering
// ===========================================================================

namespace {

void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color,
                       const glm::mat4& model = glm::mat4(1.0f)) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color, model);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

void draw_line_list(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, uint32_t vertexCount,
                    const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    push_constants(cmd, layout, mvp, color);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
}

void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color, const glm::mat4& model = glm::mat4(1.0f)) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color, model);
}

} // namespace

void EditorApplication::record_shadow_pass(VkCommandBuffer cmd, const Scene* scene) {
    m_shadowMap.enabled = false;
    if (m_shadowMap.pipeline == VK_NULL_HANDLE) return;

    // Sun direction from the scene's directional sun (or a fixed default).
    glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
    bool hasSun = false;
    if (scene) {
        for (const auto& [id, light] : scene->lightComponents) {
            if (!is_directional_sun(light)) continue;
            const auto tit = scene->transformComponents.find(id);
            if (tit != scene->transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                sunDir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            hasSun = true;
            break;
        }
    }
    if (!hasSun) return;

    // Ortho fit around the camera, depth remapped to [0,1] so the shared
    // computeShadow (sc.z in [0,1] after the divide) matches the stored depth.
    const glm::vec3 lightDir = glm::normalize(-sunDir);
    const glm::vec3 center = m_editorCamera.position;
    constexpr float kExtent = 35.0f;
    const glm::mat4 lightView = glm::lookAt(center + lightDir * 80.0f, center, glm::vec3(0, 1, 0));
    const glm::mat4 lightProj = glm::ortho(-kExtent, kExtent, -kExtent, kExtent, 0.1f, 200.0f);
    glm::mat4 depthRemap(1.0f);
    depthRemap[2][2] = 0.5f;
    depthRemap[2][3] = 0.5f;
    m_shadowMap.viewProj = depthRemap * lightProj * lightView;
    m_shadowMap.enabled = true;

    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = m_shadowMap.renderPass;
    info.framebuffer = m_shadowMap.framebuffer;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = { m_shadowMap.size, m_shadowMap.size };
    VkClearValue clear;
    clear.depthStencil = { 1.0f, 0 };
    info.clearValueCount = 1;
    info.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, m_shadowMap.size, m_shadowMap.size);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowMap.pipeline);

    // Casters: every entity with a mesh renderer and a loaded .vcmesh.
    for (const auto& [id, ent] : scene->get_entities()) {
        const auto transformIt = scene->transformComponents.find(id);
        if (transformIt == scene->transformComponents.end()) continue;
        const auto meshComp = scene->meshRendererComponents.find(id);
        if (meshComp == scene->meshRendererComponents.end() ||
            !meshComp->second.meshAssetID.is_valid()) continue;
        const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID);
        if (!mesh) continue;
        const glm::mat4 mvp = m_shadowMap.viewProj * model_from_transform(transformIt->second);
        vkCmdPushConstants(cmd, m_shadowMap.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &mvp);
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vb.buffer, &vertexOffset);
        if (mesh->ib.buffer != VK_NULL_HANDLE)
            vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const auto& range : mesh->ranges) {
            if (range.indexed)
                vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
            else
                vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
}

void EditorApplication::render_scene_to_offscreen(VkCommandBuffer cmd) {
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();
    record_shadow_pass(cmd, renderScene);

    build_viewport_render_graph();
    if (!m_viewportRenderGraphExecutor.valid() || m_offscreen.framebuffer == VK_NULL_HANDLE) return;

    // Re-register the scene pass every frame so the framebuffer stays current
    // after offscreen recreations (resize) — the executor keeps its own copy.
    Rendering::VulkanRenderGraphExecutor::PassFrame sceneFrame;
    sceneFrame.renderPass = m_offscreen.renderPass;
    sceneFrame.framebuffers = { m_offscreen.framebuffer };
    sceneFrame.clearValues.resize(2);
    sceneFrame.clearValues[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
    sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
    sceneFrame.draw = [this](VkCommandBuffer cb) { record_viewport_scene_content(cb); };
    m_viewportRenderGraphExecutor.register_pass(m_viewportScenePass, std::move(sceneFrame));

    // Scene pass recorded by the compiled graph: begins the offscreen render
    // pass, runs the content callback, ends — same executor the game uses.
    m_viewportRenderGraphExecutor.record(cmd, 0, { m_offscreen.width, m_offscreen.height });

    // Env-probe cubemap capture: recorded AFTER the viewport pass so the
    // command buffer is free to open its own 6-face render passes.
    record_env_capture(cmd, renderScene);
}

void EditorApplication::build_viewport_render_graph() {
    if (m_viewportRenderGraphBuilt) return;
    using namespace Engine::Rendering;
    const auto colorRes = m_viewportRenderGraph.add_resource({ "Viewport Color", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto depthRes = m_viewportRenderGraph.add_resource({ "Viewport Depth", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    m_viewportScenePass = m_viewportRenderGraph.add_pass({ "Scene", RenderQueue::Graphics,
        { { colorRes, RenderAccess::Write, RenderResourceState::ColorAttachment },
          { depthRes, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
    std::string error;
    if (!m_viewportRenderGraphExecutor.initialize(m_device, m_viewportRenderGraph, &error)) {
        std::cerr << "[Editor] viewport render graph init failed: " << error << std::endl;
        return;
    }
    m_viewportRenderGraphBuilt = true;
    std::cout << "[Editor] Viewport render graph wired ("
              << m_viewportRenderGraphExecutor.compile_result().order.size() << " passes, "
              << m_viewportRenderGraphExecutor.compile_result().barriers.size() << " barriers)\n";
}

void EditorApplication::record_viewport_scene_content(VkCommandBuffer cmd) {
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();

    set_viewport_scissor(cmd, m_offscreen.width, m_offscreen.height);

    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();

    // Sky pass (Clima panel): procedural sky driven by the scene's first
    // WeatherComponent and directional sun (fallbacks: warm white sun at noon).
    if (m_skyPipeline != VK_NULL_HANDLE) {
        glm::vec3 sunColor(1.0f, 0.95f, 0.85f);
        glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
        float windSpeed = 5.0f;
        if (renderScene) {
            for (const auto& [id, w] : renderScene->weatherComponents) {
                (void)id;
                sunColor = w.sunColor;
                windSpeed = w.windSpeed;
                g_fogParams = glm::vec4(w.fogDensity, w.fogStart, w.heightFog ? 1.0f : 0.0f, 0.0f);
                g_fogColor = glm::vec4(sunColor * 0.3f + glm::vec3(0.5f, 0.6f, 0.7f) * 0.7f, 1.0f);
                break;
            }
            for (const auto& [id, light] : renderScene->lightComponents) {
                if (!is_directional_sun(light)) continue;
                const auto tit = renderScene->transformComponents.find(id);
                if (tit != renderScene->transformComponents.end()) {
                    const float yaw = glm::radians(tit->second.rotation.y);
                    const float pitch = glm::radians(tit->second.rotation.x);
                    sunDir = glm::normalize(glm::vec3(
                        std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                        std::cos(pitch) * std::cos(yaw)));
                }
                break;
            }
        }
        m_skyTime += 0.016f; // ~one frame; cloud drift driven by windSpeed below
        const SkyPushConstants skyPC{
            viewProj,
            glm::vec4(m_editorCamera.position, 1.0f),
            glm::vec4(sunDir, 0.0f),
            glm::vec4(sunColor, 0.0f),
            glm::vec4(m_skyTime * windSpeed * 0.02f, sunDir.y > -0.08f ? 1.0f : 0.0f, 0.0f, 0.0f),
        };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
        vkCmdPushConstants(cmd, m_skyPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(SkyPushConstants), &skyPC);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // Terrain (Terreno panel): procedural heightmap mesh on the ground.
    if (m_terrainValid && m_terrainVB.buffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        const glm::mat4 model(1.0f);
        draw_indexed_editor_mesh(cmd, m_scenePipelineLayout, m_terrainVB.buffer, m_terrainIB.buffer,
                                 m_terrainIndexCount, viewProj * model, glm::vec4(1.0f));
    }

    // Voxel sculpting volumes (colored cubes, paintable in the viewport).
    draw_voxel_volumes(cmd, viewProj, renderScene);

    // Analytic infinite grid (fullscreen triangle, fwidth AA, distance fade).
    // Gated by the viewport ⋯ menu (m_showGrid). The CPU passes the inverse
    // view-projection and the camera position so the shader unprojects rays
    // without inverting matrices per vertex.
    if (m_showGrid && m_gridPipeline != VK_NULL_HANDLE) {
        const GridPushConstants gridPC{
            glm::inverse(m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix()),
            glm::vec4(m_editorCamera.position, 1.0f) };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        vkCmdPushConstants(cmd, m_gridPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GridPushConstants), &gridPC);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // Scene entities (renderScene resolved at the top of this function).
    if (renderScene) {
        for (const auto& [id, ent] : renderScene->get_entities()) {
            const auto transformIt = renderScene->transformComponents.find(id);
            if (transformIt == renderScene->transformComponents.end()) continue;
            // Layers: entities on a hidden layer are not rendered (the panel
            // toggle propagates to every entity sharing the layer name).
            const auto layerIt = renderScene->layerComponents.find(id);
            if (layerIt != renderScene->layerComponents.end() && !layerIt->second.visible) continue;
            const TransformComponent& t = transformIt->second;
            const bool selected = m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id;

            if (renderScene->lightComponents.contains(id)) {
                draw_light_icon(cmd, viewProj, t, selected);
            } else if (renderScene->cameraComponents.contains(id)) {
                draw_camera_frustum(cmd, viewProj, t, selected);
            } else if (renderScene->meshRendererComponents.contains(id) ||
                       renderScene->materialComponents.contains(id)) {
                glm::vec3 baseColor(0.72f, 0.75f, 0.82f);
                if (renderScene->materialComponents.contains(id)) {
                    baseColor = renderScene->materialComponents.at(id).albedo;
                }
                const glm::vec4 color = selected
                    ? glm::vec4(0.45f, 0.50f, 1.00f, 1.0f)
                    : glm::vec4(baseColor, 1.0f);
                bool drewMesh = false;
                const auto meshComp = renderScene->meshRendererComponents.find(id);
                if (meshComp != m_editorScene->meshRendererComponents.end() &&
                    meshComp->second.meshAssetID.is_valid()) {
                    if (const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID)) {
                        // Material-graph path: the mesh renderer's material asset,
                        // or the Material Editor's live graph on the selected entity.
                        GraphMaterialPipeline* gmp = nullptr;
                        const bool useLive = m_specializedEditors.previewOnSelected && selected;
                        if (useLive) {
                            const uint64_t liveHash = hash_material_graph(m_specializedEditors.live_material_graph());
                            if (liveHash != m_liveGraphHash || !m_liveGraphPipeline.valid) {
                                destroy_graph_pipeline(m_liveGraphPipeline);
                                if (!build_graph_pipeline(m_specializedEditors.live_material_graph(), m_liveGraphPipeline)) {
                                    if (!m_liveGraphLastErrorLogged) {
                                        std::cerr << "[Editor] Material preview: " << m_liveGraphPipeline.lastError << std::endl;
                                        m_liveGraphLastErrorLogged = true;
                                    }
                                } else {
                                    m_liveGraphLastErrorLogged = false;
                                }
                                m_liveGraphHash = liveHash;
                            }
                            gmp = m_liveGraphPipeline.valid ? &m_liveGraphPipeline : nullptr;
                        } else if (const auto vidIt = renderScene->videoComponents.find(id);
                                   vidIt != renderScene->videoComponents.end() &&
                                   !vidIt->second.framePaths.empty()) {
                            // Video flipbook: the mesh's texture is the current
                            // frame of the image sequence (cached per frame).
                            const VideoComponent& vc = vidIt->second;
                            const int frame = std::clamp(vc.currentFrame, 0,
                                static_cast<int>(vc.framePaths.size()) - 1);
                            const UUID frameTex =
                                resolve_texture_asset_by_name(vc.framePaths[frame]);
                            if (frameTex.is_valid()) {
                                gmp = ensure_texture_pipeline(frameTex, m_videoGraphPipelines);
                            }
                        } else if (const auto blockMeta = m_assetRegistry.find(meshComp->second.meshAssetID);
                                   blockMeta && blockMeta->type == AssetType::Block) {
                            // Block model in the scene: a textured cube. The
                            // pipeline binds the block texture (TextureSample,
                            // same path as the Material editor), cached per
                            // texture UUID and rebuilt if the texture changes.
                            const UUID texId = resolve_block_texture(meshComp->second.meshAssetID);
                            if (texId.is_valid()) gmp = ensure_texture_pipeline(texId, m_blockGraphPipelines);
                        } else if (const auto skinMeta = m_assetRegistry.find(meshComp->second.meshAssetID);
                                   skinMeta && skinMeta->type == AssetType::Texture &&
                                   is_character_texture(*skinMeta)) {
                            // Minecraft character/mob skin in the scene: the
                            // humanoid mesh with the skin sampled directly —
                            // no sidecar, the texture IS the character.
                            gmp = ensure_texture_pipeline(meshComp->second.meshAssetID, m_skinGraphPipelines, true);
                            if (!gmp) {
                                std::cerr << "[Editor] skin pipeline failed for "
                                          << meshComp->second.meshAssetID.to_string() << std::endl;
                            }
                        } else if (meshComp->second.materialAssetID.is_valid() &&
                                   load_material_asset(meshComp->second.materialAssetID)) {
                            const UUID matId = meshComp->second.materialAssetID;
                            const MaterialAsset& mat = m_materialAssets.at(matId);
                            const Rendering::MaterialGraph graph = material_graph_from_asset(mat);
                            const uint64_t graphHash = hash_material_graph(graph);
                            auto it = m_graphMaterialPipelines.find(matId);
                            if (it == m_graphMaterialPipelines.end() ||
                                !it->second.valid || it->second.graphHash != graphHash) {
                                if (it != m_graphMaterialPipelines.end()) destroy_graph_pipeline(it->second);
                                GraphMaterialPipeline built;
                                built.graphHash = graphHash;
                                if (!build_graph_pipeline(graph, built)) {
                                    std::cerr << "[Editor] Material pipeline: " << built.lastError << std::endl;
                                }
                                it = m_graphMaterialPipelines.insert_or_assign(matId, std::move(built)).first;
                            }
                            if (it->second.valid) gmp = &it->second;
                        }
                        if (gmp) {
                            const MaterialAsset* matAsset = nullptr;
                            const auto matAssetIt = m_materialAssets.find(meshComp->second.materialAssetID);
                            if (matAssetIt != m_materialAssets.end()) matAsset = &matAssetIt->second;
                            const MaterialComponent* comp = nullptr;
                            const auto compIt = renderScene->materialComponents.find(id);
                            if (compIt != m_editorScene->materialComponents.end()) comp = &compIt->second;
                            write_material_ubo(*gmp, matAsset, comp);
                            write_light_ubo(*gmp, renderScene, m_editorCamera.position);
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                                                    0, 1, &gmp->descriptorSet, 0, nullptr);
                            const glm::mat4 model = model_from_transform(t);
                            const Rendering::MaterialPushConstants pc{ viewProj * model, model };
                            vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                               sizeof(pc), &pc);
                            const VkDeviceSize vertexOffset = 0;
                            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vb.buffer, &vertexOffset);
                            if (mesh->ib.buffer != VK_NULL_HANDLE)
                                vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                            for (const auto& range : mesh->ranges) {
                                if (range.indexed)
                                    vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
                                else
                                    vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
                            }
                            drewMesh = true;
                        } else {
                            // Vertex painting: entities with painted colors
                            // draw from their per-vertex-color buffer (the
                            // scene pipeline shades vertex colors).
                            const auto paintIt = renderScene->paintComponents.find(id);
                            const bool hasPaint = paintIt != renderScene->paintComponents.end() &&
                                                  paintIt->second.enabled &&
                                                  !paintIt->second.vertexColors.empty();
                            if (hasPaint) {
                                rebuild_paint_buffer(id, const_cast<PaintComponent&>(paintIt->second), mesh);
                                const auto pb = m_paintBuffers.find(id);
                                if (pb != m_paintBuffers.end() && pb->second.vb.buffer != VK_NULL_HANDLE) {
                                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                                    const VkDeviceSize off = 0;
                                    vkCmdBindVertexBuffers(cmd, 0, 1, &pb->second.vb.buffer, &off);
                                    if (mesh->ib.buffer != VK_NULL_HANDLE)
                                        vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                                    push_constants(cmd, m_scenePipelineLayout,
                                                   viewProj * model_from_transform(t), glm::vec4(1.0f),
                                                   model_from_transform(t));
                                    for (const auto& range : mesh->ranges) {
                                        if (range.indexed)
                                            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
                                        else
                                            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
                                    }
                                    drewMesh = true;
                                }
                            }
                            if (!drewMesh) {
                                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                                draw_mesh_resource(cmd, viewProj * model_from_transform(t), color, *mesh,
                                                   model_from_transform(t));
                                drewMesh = true;
                            }
                        }
                    }
                }
                if (!drewMesh) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                      m_cubeIndexCount, viewProj * model_from_transform(t), color,
                                      model_from_transform(t));
                }
                if (selected) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                      m_cubeIndexCount, viewProj * model_from_transform(t),
                                      glm::vec4(0.55f, 0.60f, 1.00f, 1.0f), model_from_transform(t));
                }
            } else {
                // Transform-only entity: subtle wireframe box.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                  m_cubeIndexCount, viewProj * model_from_transform(t),
                                  selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f)
                                           : glm::vec4(0.35f, 0.38f, 0.50f, 1.0f),
                                  model_from_transform(t));
            }

            // Hair strands: verlet-simulated LINE_LIST, rebuilt each frame.
            if (m_hairPipeline != VK_NULL_HANDLE) {
                const auto hairIt = renderScene->hairParticleComponents.find(id);
                if (hairIt != renderScene->hairParticleComponents.end() && hairIt->second.enabled) {
                    const auto sim = m_hairs.find(id);
                    if (sim != m_hairs.end() && sim->second.vb.buffer != VK_NULL_HANDLE) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_hairPipeline);
                        const VkDeviceSize off = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &sim->second.vb.buffer, &off);
                        push_constants(cmd, m_scenePipelineLayout, viewProj, glm::vec4(1.0f));
                        vkCmdDraw(cmd, sim->second.vertexCount, 1, 0, 0);
                    }
                }
            }

            // Soft body: verlet cloth mesh (local space, entity transform).
            const auto softIt = renderScene->softBodyComponents.find(id);
            if (softIt != renderScene->softBodyComponents.end() && softIt->second.enabled) {
                const auto sim = m_softBodies.find(id);
                if (sim != m_softBodies.end() && sim->second.vb.buffer != VK_NULL_HANDLE) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                    const VkDeviceSize off = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &sim->second.vb.buffer, &off);
                    vkCmdBindIndexBuffer(cmd, sim->second.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                    push_constants(cmd, m_scenePipelineLayout,
                                   viewProj * model_from_transform(t), glm::vec4(1.0f),
                                   model_from_transform(t));
                    vkCmdDrawIndexed(cmd, sim->second.indexCount, 1, 0, 0, 0);
                }
            }

            // Decal: textured quad at the entity transform (texture pipeline).
            const auto decalIt = renderScene->decalComponents.find(id);
            if (decalIt != renderScene->decalComponents.end() && decalIt->second.enabled &&
                m_decalVB.buffer != VK_NULL_HANDLE) {
                const DecalComponent& dec = decalIt->second;
                const UUID texId = resolve_texture_asset_by_name(dec.texturePath);
                if (texId.is_valid()) {
                    if (GraphMaterialPipeline* dgmp = ensure_texture_pipeline(texId, m_blockGraphPipelines, true)) {
                        write_material_ubo(*dgmp, nullptr, nullptr);
                        write_light_ubo(*dgmp, renderScene, m_editorCamera.position);
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dgmp->pipeline);
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dgmp->layout,
                                                0, 1, &dgmp->descriptorSet, 0, nullptr);
                        glm::mat4 model = model_from_transform(t);
                        model = model * glm::scale(glm::mat4(1.0f),
                                                   glm::vec3(std::max(dec.size.x, 0.01f),
                                                             std::max(dec.size.y, 0.01f), 1.0f));
                        const Rendering::MaterialPushConstants dpc{ viewProj * model, model };
                        vkCmdPushConstants(cmd, dgmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                           sizeof(dpc), &dpc);
                        const VkDeviceSize off = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &m_decalVB.buffer, &off);
                        vkCmdBindIndexBuffer(cmd, m_decalIB.buffer, 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, m_decalIndexCount, 1, 0, 0, 0);
                    }
                }
            }

            if (m_showColliders && renderScene->rigidbodyComponents.contains(id)) {
                draw_collider_wireframe(cmd, viewProj, t, selected);
            }
        }
    }

    // Gaussian splat clouds: soft point splats (cached per entity, rebuilt
    // when the parameters change or regenerate is requested).
    if (m_splatPipeline != VK_NULL_HANDLE) {
        for (const auto& [id, gs] : renderScene->gaussianSplatComponents) {
            if (!gs.enabled) continue;
            const auto tit = renderScene->transformComponents.find(id);
            if (tit == renderScene->transformComponents.end()) continue;
            auto& cloud = m_splatClouds[id];
            if (cloud.dirty || cloud.count != gs.count || cloud.scale != gs.scale) {
                std::vector<EditorVertex> verts;
                generate_splat_cloud(gs, verts);
                const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
                if (cloud.vb.buffer != VK_NULL_HANDLE && cloud.vb.size < vs) {
                    destroy_buffer(cloud.vb);
                    cloud.vb = GPUBuffer{};
                }
                if (cloud.vb.buffer == VK_NULL_HANDLE) {
                    create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  cloud.vb.buffer, cloud.vb.memory);
                }
                safe_map_and_copy(m_device, cloud.vb.memory, 0, vs, verts.data());
                cloud.count = gs.count;
                cloud.scale = gs.scale;
                cloud.dirty = false;
            }
            if (cloud.vb.buffer == VK_NULL_HANDLE) continue;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_splatPipeline);
            const VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &cloud.vb.buffer, &off);
            const SplatPushConstants spc{ viewProj * model_from_transform(tit->second),
                glm::vec4(gs.pointSize, static_cast<float>(m_offscreen.height), gs.opacity, 0.0f) };
            vkCmdPushConstants(cmd, m_splatPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(spc), &spc);
            vkCmdDraw(cmd, cloud.count, 1, 0, 0);
        }
    }

    // Env probe: the captured cubemap previewed on a reflective sphere at the
    // probe position (capture happens after the viewport pass, so this frame
    // samples the previous capture — one frame of latency, like real probes).
    if (m_envCapture.valid && m_envSpherePipeline != VK_NULL_HANDLE) {
        const auto tit = renderScene->transformComponents.find(m_envCapture.entity);
        if (tit != renderScene->transformComponents.end()) {
            const glm::mat4 model = glm::translate(glm::mat4(1.0f), tit->second.position)
                                  * glm::scale(glm::mat4(1.0f), glm::vec3(1.4f));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_envSpherePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_envSpherePipelineLayout,
                                    0, 1, &m_envCapture.descriptorSet, 0, nullptr);
            const EnvSpherePushConstants epc{ viewProj * model, model,
                                              glm::vec4(m_editorCamera.position, 1.0f) };
            vkCmdPushConstants(cmd, m_envSpherePipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(epc), &epc);
            const VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_envSphereVB.buffer, &off);
            vkCmdBindIndexBuffer(cmd, m_envSphereIB.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m_envSphereIndexCount, 1, 0, 0, 0);
        }
    }

    // Gizmo on the selected entity (drawn every frame; the active axis is
    // highlighted while dragging). Gated by the viewport ⋯ menu (m_showGizmos)
    // and hidden entirely in Select mode.
    if (m_showGizmos && m_gizmoMode != GizmoMode::Select) {
        draw_gizmo_overlay(cmd, viewProj);
    }
}

void EditorApplication::draw_light_icon(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                        const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), t.position) * glm::scale(glm::mat4(1.0f), glm::vec3(1.2f));
    const glm::vec4 color = selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f) : glm::vec4(1.0f, 0.85f, 0.35f, 1.0f);
    draw_line_list(cmd, m_scenePipelineLayout, m_lightIconVB.buffer, m_lightIconVertexCount,
                   viewProj * model, color);
}

void EditorApplication::draw_camera_frustum(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                            const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), t.position)
                          * glm::rotate(glm::mat4(1.0f), glm::radians(t.rotation.y), glm::vec3(0, 1, 0))
                          * glm::rotate(glm::mat4(1.0f), glm::radians(t.rotation.x), glm::vec3(1, 0, 0))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(1.4f));
    const glm::vec4 color = selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f) : glm::vec4(0.35f, 0.75f, 1.00f, 1.0f);
    draw_line_list(cmd, m_scenePipelineLayout, m_cameraIconVB.buffer, m_cameraIconVertexCount,
                   viewProj * model, color);
}

void EditorApplication::draw_collider_wireframe(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                                const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::vec4 color = selected ? glm::vec4(1.00f, 0.75f, 0.35f, 1.0f) : glm::vec4(0.95f, 0.55f, 0.25f, 0.85f);
    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                      m_cubeIndexCount, viewProj * model_from_transform(t), color);
}

void EditorApplication::draw_entity_bounds(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                           UUID id, const TransformComponent& t) {
    (void)id;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                      m_cubeIndexCount, viewProj * model_from_transform(t),
                      glm::vec4(0.55f, 0.60f, 1.00f, 1.0f));
}

void EditorApplication::draw_gizmo_overlay(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    const auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;
    // World/Local: in local mode the whole gizmo rotates with the entity so
    // the axes follow its orientation.
    glm::mat4 gizmoModel = glm::translate(glm::mat4(1.0f), it->second.position);
    if (m_gizmoLocal) {
        gizmoModel = gizmoModel * glm::mat4_cast(glm::quat(glm::radians(it->second.rotation)));
    }

    const glm::vec4 highlight(1.0f, 0.85f, 0.30f, 1.0f);
    const glm::vec4 normal(1.0f);

    const VkDeviceSize zeroOffset = 0;
    const auto bind_gizmo = [&]() {
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_gizmoVB.buffer, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, m_gizmoIB.buffer, 0, VK_INDEX_TYPE_UINT32);
    };

    // Solid pieces: arrow cones (translate) or tip cubes (scale).
    if (m_gizmoMode == GizmoMode::Translate || m_gizmoMode == GizmoMode::Scale) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gizmoPipeline);
        for (int axis = 0; axis < 3; ++axis) {
            const bool active = (m_activeAxis == static_cast<GizmoAxis>(axis + 1));
            const EditorApplication::GizmoDrawRange& range =
                (m_gizmoMode == GizmoMode::Translate) ? m_gizmoArrowRanges[axis] : m_gizmoTipRanges[axis];
            bind_gizmo();
            push_constants(cmd, m_scenePipelineLayout, viewProj * gizmoModel,
                           active ? highlight : normal, gizmoModel);
            vkCmdDrawIndexed(cmd, range.count, 1, range.offset, 0, 0);
        }
    }

    // Wireframe pieces: shafts (translate/scale) or rings (rotate).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    for (int axis = 0; axis < 3; ++axis) {
        const bool active = (m_activeAxis == static_cast<GizmoAxis>(axis + 1));
        const EditorApplication::GizmoDrawRange& range =
            (m_gizmoMode == GizmoMode::Rotate) ? m_gizmoRingRanges[axis] : m_gizmoShaftRanges[axis];
        bind_gizmo();
        push_constants(cmd, m_scenePipelineLayout, viewProj * gizmoModel,
                       active ? highlight : normal);
        vkCmdDrawIndexed(cmd, range.count, 1, range.offset, 0, 0);
    }
}

void EditorApplication::render_pick_pass(VkCommandBuffer cmd) {
    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = m_offscreen.pickRenderPass;
    info.framebuffer = m_offscreen.pickFramebuffer;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = { m_offscreen.width, m_offscreen.height };
    VkClearValue clears[2];
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, m_offscreen.width, m_offscreen.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pickPipeline);

    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();

    Scene* pickScene = m_playMode.get_active_scene();
    if (!pickScene) pickScene = m_editorScene.get();
    m_pickColorToEntity.clear();
    uint32_t nextId = 1;
    if (pickScene) {
        for (const auto& [id, ent] : pickScene->get_entities()) {
            const auto transformIt = pickScene->transformComponents.find(id);
            if (transformIt == pickScene->transformComponents.end()) continue;
            const uint32_t pickId = nextId++;
            m_pickColorToEntity[pickId] = id;
            const glm::vec4 color(
                static_cast<float>(pickId & 0xFF) / 255.0f,
                static_cast<float>((pickId >> 8) & 0xFF) / 255.0f,
                static_cast<float>((pickId >> 16) & 0xFF) / 255.0f,
                1.0f);
            bool drewMesh = false;
            const auto meshComp = pickScene->meshRendererComponents.find(id);
            if (meshComp != m_editorScene->meshRendererComponents.end() &&
                meshComp->second.meshAssetID.is_valid()) {
                if (const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID)) {
                    draw_mesh_resource(cmd, viewProj * model_from_transform(transformIt->second), color, *mesh);
                    drewMesh = true;
                }
            }
            if (!drewMesh) {
                draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                  m_cubeIndexCount, viewProj * model_from_transform(transformIt->second), color);
            }
        }
    }
    vkCmdEndRenderPass(cmd);
}

void EditorApplication::perform_pick_readback() {
    if (!m_editorScene || m_offscreen.framebuffer == VK_NULL_HANDLE || !m_pickPipeline) return;
    if (m_pickPixel.x < 0 || m_pickPixel.y < 0 ||
        m_pickPixel.x >= static_cast<float>(m_offscreen.width) ||
        m_pickPixel.y >= static_cast<float>(m_offscreen.height)) return;

    VkCommandBuffer cmd = begin_single_time_commands();
    render_pick_pass(cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { m_offscreen.width, m_offscreen.height, 1 };
    vkCmdCopyImageToBuffer(cmd, m_offscreen.pickImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_offscreen.pickStagingBuffer, 1, &region);
    end_single_time_commands(cmd);

    void* mapped = nullptr;
    vkMapMemory(m_device, m_offscreen.pickStagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (!mapped) return;

    // Click pick
    const size_t x = static_cast<size_t>(m_pickPixel.x);
    const size_t y = static_cast<size_t>(m_pickPixel.y);
    const uint8_t* pixel = static_cast<const uint8_t*>(mapped) + (y * m_offscreen.width + x) * 4;
    const uint32_t id = static_cast<uint32_t>(pixel[0]) |
                        (static_cast<uint32_t>(pixel[1]) << 8) |
                        (static_cast<uint32_t>(pixel[2]) << 16);

    // Hover pick (same buffer, different pixel — no extra GPU pass)
    const size_t hx = std::clamp(static_cast<size_t>(m_hoverPickPixel.x), size_t(0), static_cast<size_t>(m_offscreen.width) - 1);
    const size_t hy = std::clamp(static_cast<size_t>(m_hoverPickPixel.y), size_t(0), static_cast<size_t>(m_offscreen.height) - 1);
    const uint8_t* hp = static_cast<const uint8_t*>(mapped) + (hy * m_offscreen.width + hx) * 4;
    const uint32_t hoverId = static_cast<uint32_t>(hp[0]) |
                             (static_cast<uint32_t>(hp[1]) << 8) |
                             (static_cast<uint32_t>(hp[2]) << 16);
    vkUnmapMemory(m_device, m_offscreen.pickStagingMemory);

    const auto found = m_pickColorToEntity.find(id);
    if (found != m_pickColorToEntity.end()) {
        m_selectedEntity = m_editorScene->find_entity_by_id(found->second);
        m_editorGui.select_entity(m_selectedEntity);
    }

    // Resolve hover entity name for the viewport tooltip
    if (hoverId != 0) {
        const auto hIt = m_pickColorToEntity.find(hoverId);
        if (hIt != m_pickColorToEntity.end()) {
            const Entity he = m_editorScene->find_entity_by_id(hIt->second);
            if (he.is_valid()) {
                m_hoverEntityName = he.get_name();
            } else {
                m_hoverEntityName.clear();
            }
        } else {
            m_hoverEntityName.clear();
        }
    } else {
        m_hoverEntityName.clear();
    }
}

// ===========================================================================
// Camera and gizmo interaction
// ===========================================================================

// Shared executor for Control API commands — used both by the loopback HTTP
// server and by the Control Console window buttons.



// ===========================================================================
// Play Mode, Control API & Gizmo (split from EditorApplication.cpp)
// ===========================================================================
namespace {

// Voxel grid dims (shared with the sculpting section below).
constexpr int kVoxelSizeX = 32;
constexpr int kVoxelSizeY = 24;
constexpr int kVoxelSizeZ = 32;

// Deterministic terrain height used by BOTH the visual sheet generation
// (generate_terrain_mesh) and play-mode world collision so colliders match
// exactly what is drawn. Hash-based value noise + fBm octaves, seeded.
float terrain_surface_height(uint32_t seed, float scale, int octaves,
                             float amount, float falloffParam,
                             float halfExtent, float x, float z) {
    const auto hash2 = [seed](int hx, int hz) -> float {
        uint32_t n = static_cast<uint32_t>(hx) * 374761393u
                   + static_cast<uint32_t>(hz) * 668265263u;
        n ^= seed * 0x9E3779B9u;
        n = (n ^ (n >> 13)) * 1274126177u;
        n ^= (n >> 16);
        return static_cast<float>(n & 0xFFFFu) / 65535.0f;
    };
    const auto smoothT = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const auto valueNoise = [&](float vx, float vz) {
        const int xi = static_cast<int>(std::floor(vx));
        const int zi = static_cast<int>(std::floor(vz));
        const float xf = smoothT(vx - std::floor(vx));
        const float zf = smoothT(vz - std::floor(vz));
        const float a = hash2(xi, zi), b = hash2(xi + 1, zi);
        const float c = hash2(xi, zi + 1), d = hash2(xi + 1, zi + 1);
        return a + (b - a) * xf + (c - a) * zf + (a - b - c + d) * xf * zf;
    };
    const auto fbm = [&](float fx, float fz, int oct) {
        float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
        for (int o = 0; o < oct; ++o) {
            sum += amp * valueNoise(fx * freq, fz * freq);
            norm += amp;
            amp *= 0.5f;
            freq *= 2.0f;
        }
        return sum / std::max(norm, 1e-6f);
    };
    const float dist = std::sqrt(x * x + z * z);
    const float falloff = glm::clamp(1.0f - (dist / halfExtent) * falloffParam,
                                     0.0f, 1.0f);
    return (fbm(x / scale, z / scale, octaves) - 0.5f)
           * 2.0f * amount * 20.0f * falloff;
}

} // namespace

// Builds the play world's REAL collision from scene content (FALTANTES item:
// colisão real de terrain/voxel): every voxel volume becomes exact merged-cell
// boxes (Engine::Physics::merge_solid_voxels) positioned by the volume's
// transform, and the procedural terrain becomes sampled column boxes sharing
// the exact height function of the visual sheet. Bodies land on what they see.
void EditorApplication::build_play_world_collision() {
    m_playStaticBodies.clear();
    float minBottom = 0.0f;
    bool any = false;
    size_t voxelBoxes = 0;

    // ---- Voxel volumes: exact merged-cell boxes ----------------------------
    if (m_editorScene) {
        for (const auto& [entityId, gridPtr] : m_voxelStructures) {
            if (!gridPtr) continue;
            glm::vec3 origin(0.0f);
            const auto trIt = m_editorScene->transformComponents.find(entityId);
            if (trIt != m_editorScene->transformComponents.end())
                origin = trIt->second.position;
            const Engine::Voxel::VoxelStructure& grid = *gridPtr;
            const auto solid = [&grid](int x, int y, int z) -> bool {
                return !grid.get(Engine::Voxel::Int3{ x, y, z }).empty();
            };
            const auto boxes = Engine::Physics::merge_solid_voxels(
                kVoxelSizeX, kVoxelSizeY, kVoxelSizeZ, solid);
            for (const auto& b : boxes) {
                const glm::vec3 half(static_cast<float>(b.sx) * 0.5f,
                                     static_cast<float>(b.sy) * 0.5f,
                                     static_cast<float>(b.sz) * 0.5f);
                Physics::BodyDesc desc;
                desc.motion = Physics::MotionType::Static;
                // Same cell -> world mapping as rebuild_voxel_mesh(): grid
                // centered on the volume origin in X/Z, base at origin.y.
                desc.position = origin + glm::vec3(
                    static_cast<float>(b.x - kVoxelSizeX / 2) + half.x,
                    static_cast<float>(b.y) + half.y,
                    static_cast<float>(b.z - kVoxelSizeZ / 2) + half.z);
                desc.collider.shape = Physics::BoxShape{ half };
                desc.collider.friction = 0.7f;
                desc.collider.restitution = 0.05f;
                const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
                if (handle == Physics::InvalidBody) continue;
                m_playStaticBodies.push_back(handle);
                ++voxelBoxes;
                const float bottom = desc.position.y - half.y;
                minBottom = any ? std::min(minBottom, bottom) : bottom;
                any = true;
            }
        }
    }

    // ---- Procedural terrain: sampled column boxes ---------------------------
    // 64x64 columns over the sheet. Each column runs from a common base below
    // the lowest sample up to its own height, so adjacent columns always share
    // faces — no gaps to fall through between samples.
    size_t terrainColumns = 0;
    if (m_terrainValid && m_terrainParams.segments > 0 && m_terrainParams.halfExtent > 0.0f) {
        constexpr int kCols = 64;
        constexpr size_t kMaxTerrainBodies = 4096; // hard cap for play startup
        const float half = m_terrainParams.halfExtent;
        const float cell = (2.0f * half) / static_cast<float>(kCols);
        float heights[kCols * kCols];
        float minH = 0.0f;
        for (int j = 0; j < kCols; ++j) {
            for (int i = 0; i < kCols; ++i) {
                const float x = -half + (static_cast<float>(i) + 0.5f) * cell;
                const float z = -half + (static_cast<float>(j) + 0.5f) * cell;
                const float h = terrain_surface_height(
                    m_terrainParams.seed, m_terrainParams.scale,
                    m_terrainParams.octaves, m_terrainParams.amount,
                    m_terrainParams.falloff, half, x, z);
                heights[j * kCols + i] = h;
                minH = (i == 0 && j == 0) ? h : std::min(minH, h);
            }
        }
        const float base = std::floor(minH) - 2.0f;
        for (int j = 0; j < kCols && m_playStaticBodies.size() < kMaxTerrainBodies + voxelBoxes; ++j) {
            for (int i = 0; i < kCols && m_playStaticBodies.size() < kMaxTerrainBodies + voxelBoxes; ++i) {
                const float top = heights[j * kCols + i];
                const float centerY = (top + base) * 0.5f;
                const float halfY = std::max((top - base) * 0.5f, 0.25f);
                Physics::BodyDesc desc;
                desc.motion = Physics::MotionType::Static;
                desc.position = glm::vec3(
                    -half + (static_cast<float>(i) + 0.5f) * cell, centerY,
                    -half + (static_cast<float>(j) + 0.5f) * cell);
                desc.collider.shape = Physics::BoxShape{
                    glm::vec3(cell * 0.5f, halfY, cell * 0.5f) };
                desc.collider.friction = 0.7f;
                desc.collider.restitution = 0.05f;
                const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
                if (handle == Physics::InvalidBody) continue;
                m_playStaticBodies.push_back(handle);
                ++terrainColumns;
                minBottom = any ? std::min(minBottom, base) : base;
                any = true;
            }
        }
    }

    std::cout << "[PlayRuntime] world collision: " << voxelBoxes << " voxel boxes, "
              << terrainColumns << " terrain columns" << std::endl;

    // Void-failsafe plane goes BELOW everything real (or stays at y=0 when the
    // scene has no collidable content at all).
    m_playCollisionFloorY = any ? (minBottom - 8.0f) : -0.5f;
}

void EditorApplication::setup_play_runtime() {
    teardown_play_runtime();
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;

    // Real world collision first (see build_play_world_collision). The wide
    // thin plane below is now only a void-failsafe placed under the lowest
    // real collider (or at y=0 when the scene has no collidable content).
    build_play_world_collision();
    {
        Physics::BodyDesc ground;
        ground.motion = Physics::MotionType::Static;
        ground.position = glm::vec3(0.0f, m_playCollisionFloorY, 0.0f);
        ground.collider.shape = Physics::BoxShape{ glm::vec3(2000.0f, 0.5f, 2000.0f) };
        ground.collider.friction = 0.7f;
        ground.collider.restitution = 0.05f;
        m_playGroundBody = m_playPhysics.create_body(ground);
    }
    for (const auto& [id, rb] : playScene->rigidbodyComponents) {
        Physics::BodyDesc desc;
        desc.motion = rb.isKinematic ? Physics::MotionType::Kinematic : Physics::MotionType::Dynamic;
        desc.mass = std::max(rb.mass, 0.01f);
        desc.collider.friction = rb.friction;
        desc.collider.restitution = rb.restitution;
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            desc.position = tit->second.position;
            desc.rotation = glm::quat(glm::radians(tit->second.rotation));
        }
        const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
        if (handle != Physics::InvalidBody) m_playBodies[id] = handle;
    }

    // Wicked-port runtime (formerly TODO(frontend-port)): constraints run as
    // soft force-based constraints — the runtime solver exposes no rigid-joint
    // API (PhysicsWorld's joints are a separate, unintegrated world). Each
    // constraint stores the world anchors captured at play start; the tick
    // applies spring forces to keep the anchors together (see
    // tick_play_runtime). Springs just record a rest anchor.
    for (const auto& [id, cn] : playScene->constraintComponents) {
        if (!cn.enabled) continue;
        if (!m_playBodies.contains(id)) continue;
        const auto tit = playScene->transformComponents.find(id);
        const glm::vec3 baseA = (tit != playScene->transformComponents.end())
                                    ? tit->second.position
                                    : glm::vec3(0.0f);
        ConstraintRest rest;
        rest.anchorA = baseA + cn.anchor;
        rest.anchorB = baseA + cn.anchor; // refined when the other body exists
        if (const auto bodyBIt = m_playBodies.find(cn.otherEntity); bodyBIt != m_playBodies.end()) {
            if (Physics::RigidBody* bodyB = m_playPhysics.body(bodyBIt->second)) {
                rest.anchorB = bodyB->position + cn.anchor;
            }
        }
        rest.restLength = glm::length(rest.anchorB - rest.anchorA);
        m_constraintRests[id] = rest;
    }
    for (const auto& [id, sp] : playScene->springComponents) {
        if (sp.disabled || !sp.enabled) continue;
        if (!m_playBodies.contains(id)) continue;
        const auto tit = playScene->transformComponents.find(id);
        m_springRests[id] = (tit != playScene->transformComponents.end())
                                ? tit->second.position
                                : glm::vec3(0.0f);
    }

    // Play particles (Fase 8): one ParticleSimulation emitter per
    // ParticleEmitterComponent entity, positioned at the world transform.
    for (const auto& [id, pe] : playScene->particleEmitterComponents) {
        if (!pe.emitting) continue;
        Engine::Gameplay::ParticleEmitterDesc desc;
        desc.direction = glm::normalize(pe.direction);
        desc.coneAngle = pe.coneAngle;
        desc.rate = pe.rate;
        desc.speedMin = pe.speedMin;
        desc.speedMax = pe.speedMax;
        desc.lifetimeMin = pe.lifetimeMin;
        desc.lifetimeMax = pe.lifetimeMax;
        desc.sizeStart = pe.sizeStart;
        desc.sizeEnd = pe.sizeEnd;
        desc.colorStart = pe.colorStart;
        desc.colorEnd = pe.colorEnd;
        desc.acceleration = pe.acceleration;
        desc.drag = pe.drag;
        desc.turbulence = pe.turbulence;
        desc.restitution = pe.restitution;
        desc.collide = pe.collide;
        desc.emitting = pe.emitting;
        const auto tit = playScene->transformComponents.find(id);
        desc.position = (tit != playScene->transformComponents.end())
                            ? tit->second.position + pe.position
                            : pe.position;
        m_playEmitters[id] = m_playParticles.add_emitter(desc);
        if (pe.burstCount > 0) m_playParticles.emit_burst(m_playEmitters[id], pe.burstCount);
    }

    // Play vehicles (Fase 8): chassis body + four wheels derived from the
    // component's wheelBase/trackWidth, driven by the arrow keys in play.
    for (const auto& [id, veh] : playScene->vehicleComponents) {
        if (!veh.enabled) continue;
        Physics::BodyDesc chassis;
        chassis.motion = Physics::MotionType::Dynamic;
        chassis.mass = std::max(veh.mass, 1.0f);
        chassis.collider.shape = Physics::BoxShape{{veh.wheelBase * 0.35f, 0.35f, veh.trackWidth * 0.35f}};
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            chassis.position = tit->second.position;
            chassis.rotation = glm::quat(glm::radians(tit->second.rotation));
        }
        const Physics::BodyHandle body = m_playPhysics.create_body(chassis);
        if (body == Physics::InvalidBody) continue;
        m_playVehicleChassis[id] = body;
        const float halfBase = veh.wheelBase * 0.5f;
        const float halfTrack = veh.trackWidth * 0.5f;
        const glm::vec3 locals[4] = {
            {-halfBase, -0.1f, -halfTrack}, {-halfBase, -0.1f, halfTrack},
            {halfBase, -0.1f, -halfTrack},  {halfBase, -0.1f, halfTrack},
        };
        std::vector<Engine::Gameplay::WheelDesc> wheels(4);
        for (int i = 0; i < 4; ++i) {
            wheels[i].localPosition = locals[i];
            wheels[i].radius = veh.wheelRadius;
            wheels[i].suspensionRestLength = veh.suspensionRest;
            wheels[i].maxDriveForce = veh.enginePower;
            wheels[i].maxBrakeForce = veh.brakeForce;
            wheels[i].maxSteerAngle = veh.maxSteerAngle;
            wheels[i].steering = i < 2;
            wheels[i].driven = veh.frontWheelDrive ? i < 2 : i >= 2;
        }
        m_playVehicles.emplace(id, Engine::Gameplay::VehicleRuntime(body, std::move(wheels)));
    }

    // Play ragdolls (Fase 6): physics bodies per bone. With fromSkeleton set,
    // the bones come from the entity's skin skeleton (a sibling Skeleton asset
    // matching the mesh stem); otherwise a two-bone fallback is used. The play
    // physics simulates them each frame; the pose drives skinned rendering.
    for (const auto& [id, rg] : playScene->ragdollComponents) {
        if (!rg.enabled) continue;
        glm::vec3 rootPos{0.0f};
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) rootPos = tit->second.position;
        rootPos += rg.spawnOffset;
        std::vector<Physics::RagdollBoneDesc> bones;
        bool fromSkin = false;
        if (rg.fromSkeleton) {
            std::string meshStem;
            if (const auto mit = playScene->meshRendererComponents.find(id); mit != playScene->meshRendererComponents.end()) {
                for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
                    if (asset.type == AssetType::Mesh && asset.id == mit->second.meshAssetID) {
                        meshStem = asset.sourcePath.stem().string();
                        break;
                    }
                }
            }
            if (!meshStem.empty()) {
                for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
                    if (asset.type != AssetType::Skeleton || asset.sourcePath.stem().string() != meshStem) continue;
                    SkeletonAsset skeleton;
                    if (AnimationAssetIO::load_skeleton(skeleton, asset.cookedPath)) {
                        bones = Physics::build_ragdoll_bones(skeleton, rg.massPerBone);
                        fromSkin = !bones.empty();
                    }
                    break;
                }
            }
            if (!fromSkin) {
                std::cout << "[Editor] Ragdoll entity " << id.to_string() << ": no sibling skeleton for mesh '"
                          << meshStem << "' — using two-bone fallback\n";
            }
        }
        if (bones.empty()) {
            bones.push_back(Physics::RagdollBoneDesc{"Root", "", glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, rg.massPerBone, glm::vec3(0.0f)});
            bones.push_back(Physics::RagdollBoneDesc{"Tip", "Root", glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, rg.massPerBone, glm::vec3(0.0f)});
        }
        Physics::Ragdoll ragdoll;
        if (ragdoll.create(m_playPhysics, bones, rootPos)) {
            m_playRagdolls.emplace(id, std::move(ragdoll));
            std::cout << "[Editor] Play ragdoll active (entity=" << id.to_string() << ", bones="
                      << bones.size() << (fromSkin ? ", from skin skeleton)\n" : ", fallback)\n");
        }
    }

    // Play missions (Fase 8): Start -> SetObjective -> WaitForEvent -> Complete.
    // The completeEvent is dispatched to the mission system by the play tick
    // when another component raises it (e.g. a weapon kill or script emit).
    for (const auto& [id, mc] : playScene->missionComponents) {
        std::vector<Engine::Gameplay::MissionNode> nodes;
        nodes.push_back(Engine::Gameplay::start_node("start", "obj"));
        nodes.push_back(Engine::Gameplay::set_objective_node("obj", "objective", mc.objectiveText, mc.objectiveTarget, "wait"));
        nodes.push_back(Engine::Gameplay::wait_for_event_node("wait", mc.completeEvent, 1, "done"));
        nodes.push_back(Engine::Gameplay::complete_mission_node("done"));
        Engine::Gameplay::Mission mission("mission_" + id.to_string(), mc.missionId, std::move(nodes));
        m_playMissions.register_mission(std::move(mission));
        m_playMissionIds[id] = mc.missionId;
        if (mc.autoStart) m_playMissions.start("mission_" + id.to_string());
    }

    // Play dialogues (Fase 8): a one-node graph with a single choice that can
    // chain to another dialogue; played on start when playOnStart is set.
    for (const auto& [id, dc] : playScene->dialogueComponents) {
        Engine::Gameplay::DialogueGraph graph;
        graph.id = dc.dialogueId;
        Engine::Gameplay::DialogueNode node;
        node.id = "line";
        node.line.character = dc.character;
        node.line.text = dc.line;
        if (!dc.choiceText.empty()) {
            Engine::Gameplay::DialogueChoice choice;
            choice.text = dc.choiceText;
            choice.nextNode = dc.nextDialogueId;   // empty = end
            node.choices.push_back(std::move(choice));
        }
        graph.nodes.push_back(std::move(node));
        m_playDialogues.register_graph(std::move(graph));
        m_playDialogueIds[id] = dc.dialogueId;
        if (dc.playOnStart) m_playDialogues.play(dc.dialogueId);
    }

    // Play audio (Fase 8): resolve the .ogg through the asset registry, decode
    // it into an AudioClip and start a voice on the mixer (spatial vs the
    // camera listener). Voices advance when the mixer is rendered each tick.
    for (const auto& [id, ac] : playScene->audioComponents) {
        if (!ac.playOnStart || ac.clipPath.empty()) continue;
        std::filesystem::path clipSource;
        std::error_code clipEc;
        const std::filesystem::path clipCanonical = std::filesystem::weakly_canonical(ac.clipPath, clipEc);
        const std::filesystem::path clipName = std::filesystem::path(ac.clipPath).filename();
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type != AssetType::Audio) continue;
            // Match the authored clip path robustly: exact string, canonical
            // equivalence, or the same file name (the user may have typed a
            // relative path or a different separator style).
            if (asset.sourcePath == ac.clipPath || asset.sourcePath == clipCanonical ||
                asset.sourcePath.filename() == clipName) {
                clipSource = asset.sourcePath;
                break;
            }
        }
        if (clipSource.empty()) {
            std::cout << "[Editor] Play audio: no registered asset for '" << ac.clipPath << "'\n";
            continue;
        }
        const auto decoded = Engine::Audio::OggDecoder::decode_file(clipSource);
        if (!decoded || !decoded->valid()) {
            std::cout << "[Editor] Play audio: failed to decode '" << clipSource.string() << "'\n";
            continue;
        }
        auto clip = std::make_shared<Engine::Audio::AudioClip>(clipSource.filename().string());
        Engine::Audio::AudioBuffer buffer;
        buffer.sampleRate = decoded->sampleRate;
        buffer.channels = decoded->channels;
        buffer.samples = decoded->samples;
        clip->hot_swap(std::move(buffer));
        Engine::Audio::VoiceDescription desc;
        desc.clip = std::move(clip);
        desc.bus = m_playAudio.master_bus();
        desc.gain = ac.volume;
        desc.pitch = ac.pitch;
        desc.looping = ac.looping;
        desc.spatial = ac.spatial;
        const auto tit = playScene->transformComponents.find(id);
        desc.position = (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        const Engine::Audio::VoiceId voice = m_playAudio.play(std::move(desc));
        m_playVoices[id] = voice;
        std::cout << "[Editor] Play audio voice started ('" << clipSource.filename().string() << "')\n";
    }

    // Play destructibles (Fase 8): chunkCount boxes laid out in a square grid
    // around the entity transform; weapon hits apply radial damage.
    for (const auto& [id, dc] : playScene->destructionComponents) {
        if (!dc.enabled) continue;
        const glm::vec3 center = [&]() {
            const auto tit = playScene->transformComponents.find(id);
            return (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        }();
        const glm::quat rotation = [&]() {
            const auto tit = playScene->transformComponents.find(id);
            return (tit != playScene->transformComponents.end())
                       ? glm::quat(glm::radians(tit->second.rotation))
                       : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }();
        const uint32_t n = std::max(dc.chunkCount, 1u);
        const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(n))));
        std::vector<Engine::Gameplay::DestructionChunkDesc> chunks;
        chunks.reserve(n);
        const glm::vec3 half = dc.chunkSize * 0.5f;
        for (uint32_t i = 0; i < n; ++i) {
            const int cx = static_cast<int>(i % cols);
            const int cy = static_cast<int>(i / cols);
            const glm::vec3 local = glm::vec3((static_cast<float>(cx) - (cols - 1) * 0.5f) * dc.chunkSize.x,
                                              (static_cast<float>(cy) - (cols - 1) * 0.5f) * dc.chunkSize.y, 0.0f);
            Engine::Gameplay::DestructionChunkDesc chunk;
            chunk.localPosition = local;
            chunk.halfExtents = half;
            chunk.mass = 1.0f;
            chunk.health = dc.chunkHealth;
            chunks.push_back(chunk);
        }
        Engine::Gameplay::DestructibleRuntime runtime;
        if (runtime.create(m_playPhysics, center, rotation, chunks)) {
            m_playDestructibles.emplace(id, std::move(runtime));
        }
    }

    // Play navigation (Fase 8): bake the public navmesh — one column per grid
    // cell of the NavigationComponent; cells covered by a play physics body
    // are omitted (blocked), exactly the footprint the legacy grid used. The
    // promoted INavigationProvider (Recast + Detour) is the navigation
    // authority (FALTANTES item 12: the grid track was removed).
    for (const auto& [id, nc] : playScene->navigationComponents) {
        if (!nc.enabled) continue;
        const auto tit = playScene->transformComponents.find(id);
        const glm::vec3 start = (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        if (!m_playNav) m_playNav = engine::navigation::create_recast_navigation_provider();

        engine::navigation::NavmeshConfig config;
        config.boundsMinX = start.x - nc.gridWidth * nc.cellSize * 0.5f;
        config.boundsMaxX = start.x + nc.gridWidth * nc.cellSize * 0.5f;
        config.boundsMinZ = start.z - nc.gridHeight * nc.cellSize * 0.5f;
        config.boundsMaxZ = start.z + nc.gridHeight * nc.cellSize * 0.5f;
        config.boundsMinY = start.y - 8.0f;
        config.boundsMaxY = start.y + 200.0f;
        config.cellSize = nc.cellSize;
        config.cellHeight = 0.2f;
        config.agentRadius = 0.4f;
        config.agentHeight = 1.8f;
        config.agentMaxClimb = 1.0f;
        config.agentMaxSlope = 45.0f;

        std::vector<engine::navigation::VoxelColumn> columns;
        const float floorY = start.y;
        for (int gx = 0; gx < nc.gridWidth; ++gx) {
            for (int gz = 0; gz < nc.gridHeight; ++gz) {
                const float cx = config.boundsMinX + (gx + 0.5f) * nc.cellSize;
                const float cz = config.boundsMinZ + (gz + 0.5f) * nc.cellSize;
                bool blocked = false;
                for (const auto& [bid, handle] : m_playBodies) {
                    (void)bid;
                    Physics::RigidBody* body = m_playPhysics.body(handle);
                    if (!body) continue;
                    const glm::vec3 half = std::visit([](const auto& s) -> glm::vec3 {
                        using T = std::decay_t<decltype(s)>;
                        if constexpr (std::is_same_v<T, Physics::BoxShape>) return s.halfExtents;
                        else if constexpr (std::is_same_v<T, Physics::SphereShape>) return glm::vec3(s.radius);
                        else return glm::vec3(s.radius, s.halfHeight, s.radius);
                    }, body->collider.shape);
                    const glm::vec3 min = body->position - half;
                    const glm::vec3 max = body->position + half;
                    if (cx >= min.x && cx <= max.x && cz >= min.z && cz <= max.z) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) continue;
                columns.push_back({ cx, cz, floorY, floorY + 1.0f, true });
            }
        }
        if (!columns.empty()) {
            std::string navError;
            m_playNav->build(config, columns, navError);
        }
        PlayNavAgent agent;
        agent.position = start;
        agent.speed = nc.agentSpeed;
        m_playNavAgents.emplace(id, agent);
    }

    // Play-mode script: watch the scene's companion .script and compile it into
    // the play VM. OnStart starts immediately; a "Tick" event runs each frame
    // (same convention as the packaged game's player controller).
    m_playScriptPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Content" / "Scenes" / "Initial.script";
    if (m_playScriptReloader.watch(m_playScriptPath)) {
        ScriptGraphAsset graph;
        if (graph.load(m_playScriptPath)) {
            const auto compiled = ScriptCompiler::compile(graph);
            if (compiled) {
                m_playScript.load(std::move(compiled.program));
                m_playScript.start_event("OnStart");
                m_playScriptLoaded = true;
                m_scriptDebugGraph = graph;
                m_scriptDebugger.attach(m_playScript);
                m_scriptDebuggerAttached = true;
                m_scriptPauseRequested = false;
                std::cout << "[Editor] Play script loaded: " << m_playScriptPath.string() << std::endl;
            }
        }
    }
}

// Play-world animation runtime (Animation/Timeline/IK/Retarget editors now
// Apply to the scene): the timeline animates transforms from Property tracks,
// the state machine samples clips into bone-entity transforms (bone order =
// entity hierarchy order), IK bends a two-bone chain to a target entity, and
// retargeting copies mapped bone transforms between skeletons. Animated
// entities are treated as kinematic: their play bodies follow the transforms.
void EditorApplication::tick_animation_runtime(Scene* playScene, float deltaTime) {
    if (!playScene) return;
    const auto syncBody = [&](UUID entityId) {
        const auto bodyIt = m_playBodies.find(entityId);
        if (bodyIt == m_playBodies.end()) return;
        Physics::RigidBody* body = m_playPhysics.body(bodyIt->second);
        const auto tit = playScene->transformComponents.find(entityId);
        if (!body || tit == playScene->transformComponents.end()) return;
        body->position = tit->second.position;
        body->rotation = glm::quat(glm::radians(tit->second.rotation));
        body->linearVelocity = glm::vec3(0.0f);
        body->angularVelocity = glm::vec3(0.0f);
    };
    const auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    // ------------------------------------------------------------------
    // Timeline: Property tracks (type 4) animate a transform. Track names
    // are "Position"/"Rotation"/"Scale" (case-insensitive) and may be
    // prefixed with an entity name ("Cube.Position") to target another
    // entity. Key values are "x y z" (position), "rx ry rz" (rotation
    // degrees) or "sx sy sz" (scale), linearly interpolated.
    // ------------------------------------------------------------------
    for (auto& [id, tl] : playScene->timelineComponents) {
        if (tl.duration <= 0.0f || tl.tracks.empty()) continue;
        tl.playhead += deltaTime;
        if (tl.playhead >= tl.duration) {
            tl.playhead = tl.loop ? std::fmod(tl.playhead, tl.duration) : tl.duration;
        }
        const float t = tl.playhead;
        for (const auto& tr : tl.tracks) {
            if (tr.muted || tr.type != 4 || tr.keys.size() < 2) continue;
            size_t i = 0;
            while (i + 1 < tr.keys.size() && tr.keys[i + 1].time <= t) ++i;
            const auto& k0 = tr.keys[i];
            const auto& k1 = (i + 1 < tr.keys.size()) ? tr.keys[i + 1] : tr.keys[i];
            float f = 0.0f;
            if (k1.time > k0.time) f = glm::clamp((t - k0.time) / (k1.time - k0.time), 0.0f, 1.0f);
            glm::vec3 v0(0.0f), v1(0.0f);
            if (std::sscanf(k0.value.c_str(), "%f %f %f", &v0.x, &v0.y, &v0.z) != 3) continue;
            if (std::sscanf(k1.value.c_str(), "%f %f %f", &v1.x, &v1.y, &v1.z) != 3) continue;
            const glm::vec3 value = glm::mix(v0, v1, f);
            UUID targetId = id;
            std::string prop = tr.name;
            const size_t dot = tr.name.find('.');
            if (dot != std::string::npos) {
                const std::string entName = tr.name.substr(0, dot);
                prop = tr.name.substr(dot + 1);
                for (const auto& [eid, ent] : playScene->get_entities()) {
                    if (ent.get_name() == entName) { targetId = eid; break; }
                }
            }
            auto target = playScene->transformComponents.find(targetId);
            if (target == playScene->transformComponents.end()) continue;
            const std::string lower = toLower(prop);
            if (lower.find("pos") != std::string::npos) target->second.position = value;
            else if (lower.find("rot") != std::string::npos) target->second.rotation = value;
            else if (lower.find("scl") != std::string::npos || lower.find("scale") != std::string::npos) target->second.scale = value;
            else continue;
            syncBody(targetId);
        }
    }

    // ------------------------------------------------------------------
    // IK: two-bone chain root -> mid -> end reaches the target entity
    // (weight-blended). If midEntity is invalid it is derived from the
    // hierarchy (the child of root that is an ancestor of end). The root
    // entity orients toward the target so the bend is visible.
    // ------------------------------------------------------------------
    for (const auto& [id, ik] : playScene->ikComponents) {
        if (!ik.enabled) continue;
        const auto rootIt = playScene->transformComponents.find(ik.rootEntity);
        const auto endIt = playScene->transformComponents.find(ik.endEntity);
        const auto tgtIt = playScene->transformComponents.find(ik.targetEntity);
        if (rootIt == playScene->transformComponents.end() ||
            endIt == playScene->transformComponents.end() ||
            tgtIt == playScene->transformComponents.end()) {
            continue;
        }
        UUID midId = ik.midEntity;
        if (!midId.is_valid()) {
            for (const auto& [eid, hc] : playScene->hierarchyComponents) {
                if (hc.parentID != ik.rootEntity) continue;
                UUID cur = ik.endEntity;
                while (cur.is_valid()) {
                    if (cur == eid) { midId = eid; break; }
                    const auto hIt = playScene->hierarchyComponents.find(cur);
                    if (hIt == playScene->hierarchyComponents.end()) break;
                    cur = hIt->second.parentID;
                }
                if (midId.is_valid()) break;
            }
        }
        const auto midIt = playScene->transformComponents.find(midId);
        if (midIt == playScene->transformComponents.end()) continue;
        const glm::vec3 a = rootIt->second.position;
        const glm::vec3 b = midIt->second.position;
        const glm::vec3 c = endIt->second.position;
        const glm::vec3 target = tgtIt->second.position;
        const float l1 = glm::length(b - a);
        const float l2 = glm::length(c - b);
        if (l1 < 1e-5f || l2 < 1e-5f) continue;
        glm::vec3 toTarget = target - a;
        const float dist = glm::length(toTarget);
        const float maxReach = l1 + l2;
        const glm::vec3 desiredEnd = (dist > maxReach && dist > 1e-5f)
                                         ? a + toTarget / dist * maxReach
                                         : target;
        // Bend axis: perpendicular to the reach direction and the pole.
        glm::vec3 reachDir = (dist > 1e-5f) ? toTarget / dist : glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 bend = glm::cross(reachDir, ik.poleVector);
        if (glm::length(bend) < 1e-5f) bend = glm::cross(reachDir, glm::vec3(0.0f, 1.0f, 0.0f));
        bend = glm::normalize(bend);
        // Angle at the root (law of cosines).
        const float cosA = glm::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * std::max(dist, 1e-5f)), -1.0f, 1.0f);
        const float angleA = std::acos(cosA);
        const glm::vec3 dirAB = glm::normalize(b - a);
        const glm::quat rotA = glm::angleAxis(angleA, bend);
        const glm::vec3 newB = a + rotA * dirAB * l1;
        // Angle at the mid joint.
        const float cosB = glm::clamp((l1 * l1 + l2 * l2 - dist * dist) / (2.0f * l1 * l2), -1.0f, 1.0f);
        const float angleB = std::acos(cosB);
        const glm::vec3 dirBC = glm::normalize(c - b);
        const glm::vec3 dirB = glm::normalize(desiredEnd - newB);
        glm::vec3 axisB = glm::cross(dirBC, dirB);
        if (glm::length(axisB) < 1e-5f) axisB = bend;
        const glm::quat rotB = glm::angleAxis(angleB, glm::normalize(axisB));
        const glm::vec3 newC = newB + rotB * dirBC * l2;
        const float w = glm::clamp(ik.weight, 0.0f, 1.0f);
        midIt->second.position = glm::mix(b, newB, w);
        endIt->second.position = glm::mix(c, newC, w);
        // Root orients toward the target so the bend reads clearly.
        const glm::vec3 fwd = glm::normalize(desiredEnd - a);
        rootIt->second.rotation = { glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f))),
                                    glm::degrees(std::atan2(fwd.x, fwd.z)), 0.0f };
        syncBody(ik.rootEntity);
        syncBody(midId);
        syncBody(ik.endEntity);
    }

    // ------------------------------------------------------------------
    // Animation state machine: sample the current state's clip and write the
    // local pose onto the bone entities under the component entity (hierarchy
    // order = bone order). Transitions with an empty/"auto"/"true" condition
    // fire immediately; "name OP value" conditions read the state parameters.
    // ------------------------------------------------------------------
    const auto transitionSatisfied = [](const std::string& condition,
                                        const std::unordered_map<std::string, float>& params) {
        std::string c = condition;
        const auto trim = [](std::string& s) {
            const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        };
        trim(c);
        if (c.empty() || c == "auto" || c == "true" || c == "1") return true;
        size_t opPos = std::string::npos;
        size_t opLen = 0;
        for (const char* op : {">=", "<=", "==", "!=", ">", "<"}) {
            const size_t p = c.find(op);
            if (p != std::string::npos) { opPos = p; opLen = std::strlen(op); break; }
        }
        if (opPos == std::string::npos) {
            const auto it = params.find(c);
            return it != params.end() && it->second != 0.0f;
        }
        std::string name = c.substr(0, opPos);
        trim(name);
        std::string rhs = c.substr(opPos + opLen);
        trim(rhs);
        const float lhs = [&] { const auto it = params.find(name); return it != params.end() ? it->second : 0.0f; }();
        const float rhsV = static_cast<float>(std::atof(rhs.c_str()));
        const std::string op = c.substr(opPos, opLen);
        if (op == ">") return lhs > rhsV;
        if (op == "<") return lhs < rhsV;
        if (op == ">=") return lhs >= rhsV;
        if (op == "<=") return lhs <= rhsV;
        if (op == "==") return std::abs(lhs - rhsV) < 1e-4f;
        return std::abs(lhs - rhsV) >= 1e-4f;
    };
    for (const auto& [id, an] : playScene->animationComponents) {
        if (!an.playing || an.states.empty()) continue;
        auto& rt = m_animStates[id];
        if (rt.currentState.empty()) {
            rt.currentState = an.entryState.empty() ? an.states.front().id : an.entryState;
        }
        const AnimationStateDef* state = nullptr;
        for (const auto& s : an.states) {
            if (s.id == rt.currentState) { state = &s; break; }
        }
        if (!state) {
            rt.currentState = an.states.front().id;
            state = &an.states.front();
        }
        for (const auto& tr : an.transitions) {
            if (tr.from != rt.currentState) continue;
            if (!transitionSatisfied(tr.condition, rt.params)) continue;
            for (const auto& s : an.states) {
                if (s.id == tr.to) { rt.currentState = s.id; rt.time = 0.0f; break; }
            }
            for (const auto& s : an.states) {
                if (s.id == rt.currentState) { state = &s; break; }
            }
            break;
        }
        if (state->clip == UUID{0, 0}) continue;
        auto clipIt = m_animClips.find(state->clip);
        if (clipIt == m_animClips.end()) {
            const auto metaOpt = m_assetRegistry.find(state->clip);
            if (!metaOpt) continue;
            AnimationClip clip;
            clip.id = state->clip;
            if (!AnimationAssetIO::load_clip(clip, metaOpt->sourcePath)) continue;
            clipIt = m_animClips.emplace(state->clip, std::move(clip)).first;
        }
        const AnimationClip& clip = clipIt->second;
        rt.time += deltaTime * std::max(state->speed, 0.01f);
        const float dur = std::max(clip.duration, 0.001f);
        rt.time = state->loop ? std::fmod(rt.time, dur) : std::min(rt.time, dur);
        // Build the runtime skeleton from the entity hierarchy (bone order =
        // hierarchy order) and write the sampled local pose onto the entities.
        std::vector<UUID> boneIds;
        SkeletonAsset skeleton;
        skeleton.id = id;
        skeleton.name = "PlayRuntime";
        std::function<void(UUID, int)> collect = [&](UUID eid, int parentIndex) {
            BoneNode bn;
            bn.parentIndex = parentIndex;
            const auto eIt = playScene->get_entities().find(eid);
            bn.name = (eIt != playScene->get_entities().end()) ? eIt->second.get_name() : "bone";
            skeleton.bones.push_back(bn);
            boneIds.push_back(eid);
            const int idx = static_cast<int>(skeleton.bones.size()) - 1;
            const auto hc = playScene->hierarchyComponents.find(eid);
            if (hc != playScene->hierarchyComponents.end()) {
                for (const auto& child : hc->second.childrenIDs) collect(child, idx);
            }
        };
        collect(id, -1);
        if (skeleton.bones.empty()) continue;
        const Pose pose = AnimationSampler::sample(skeleton, clip, rt.time);
        for (size_t i = 0; i < pose.local.size() && i < boneIds.size(); ++i) {
            const auto tIt = playScene->transformComponents.find(boneIds[i]);
            if (tIt == playScene->transformComponents.end()) continue;
            tIt->second.position = pose.local[i].translation;
            tIt->second.rotation = glm::degrees(glm::eulerAngles(pose.local[i].rotation));
            tIt->second.scale = pose.local[i].scale;
            syncBody(boneIds[i]);
        }
    }

    // ------------------------------------------------------------------
    // Retarget: copy mapped source-bone transforms onto target-bone entities
    // (translation scaled, rotation offset applied). Runs independently, so a
    // mapped "Hand.L" -> "Hand.R" pair mirrors even without an animation.
    // ------------------------------------------------------------------
    for (const auto& [id, rt] : playScene->retargetComponents) {
        for (const auto& m : rt.mapping) {
            UUID srcId{0, 0}, dstId{0, 0};
            for (const auto& [eid, ent] : playScene->get_entities()) {
                if (ent.get_name() == m.sourceBone) srcId = eid;
                else if (ent.get_name() == m.targetBone) dstId = eid;
            }
            if (!srcId.is_valid() || !dstId.is_valid()) continue;
            const auto src = playScene->transformComponents.find(srcId);
            const auto dst = playScene->transformComponents.find(dstId);
            if (src == playScene->transformComponents.end() ||
                dst == playScene->transformComponents.end()) {
                continue;
            }
            dst->second.position = src->second.position * m.translationScale;
            dst->second.rotation = src->second.rotation + m.rotationOffset;
            dst->second.scale = src->second.scale;
            syncBody(dstId);
        }
    }
}

// ===========================================================================
// Runtime-wired Wicked-port features (frontend port): hair strands, soft-body
// cloth, video flipbooks, gaussian splats and env-probe cubemap captures.
// Runs in Edit AND Play so authored features preview live in the viewport.
// ===========================================================================

void EditorApplication::tick_special_runtimes(Scene* scene, float deltaTime) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    const float dt = glm::clamp(deltaTime, 0.0f, 1.0f / 20.0f);

    // ---- Hair strands (verlet: gravity * gravityPower, drag, stiffness) ----
    for (auto& [id, h] : scene->hairParticleComponents) {
        if (!h.enabled) continue;
        const auto tit = scene->transformComponents.find(id);
        if (tit == scene->transformComponents.end()) continue;
        ensure_hair_sim(id, h, tit->second);
        auto it = m_hairs.find(id);
        if (it == m_hairs.end()) continue;
        HairSim& sim = it->second;
        const float stiffness = 0.15f + 0.8f * h.stiffness;
        const float drag = 1.0f - glm::clamp(h.drag, 0.0f, 0.92f);
        const float g = -9.81f * h.gravityPower;
        const float windAmp = 0.5f;
        const float windPhase = m_skyTime * 2.0f;
        for (size_t i = 0; i < sim.pos.size(); ++i) {
            const glm::vec3 vel = (sim.pos[i] - sim.prev[i]) * drag;
            glm::vec3 next = sim.pos[i] + vel;
            next.y += g * dt;
            next.x += windAmp * std::sin(windPhase + sim.pos[i].x * 1.3f + sim.pos[i].z * 0.7f) * dt;
            // Stiffness pulls each segment back toward its rest position.
            next += (sim.rest[i] - sim.pos[i]) * stiffness * dt;
            sim.prev[i] = sim.pos[i];
            sim.pos[i] = next;
        }
        upload_hair(sim, h);
    }

    // ---- Soft body cloth (verlet grid, top row pinned, ground collision) ----
    for (auto& [id, s] : scene->softBodyComponents) {
        if (!s.enabled) continue;
        const auto tit = scene->transformComponents.find(id);
        if (tit == scene->transformComponents.end()) continue;
        ensure_softbody_sim(id, s, tit->second);
        auto it = m_softBodies.find(id);
        if (it == m_softBodies.end()) continue;
        SoftBodySim& sim = it->second;
        const size_t side = static_cast<size_t>(s.detail) + 1;
        const float drag = 1.0f - glm::clamp(s.friction, 0.0f, 0.9f) * 0.45f;
        const float g = -9.81f * s.mass;
        const float windAmp = s.wind ? 1.1f : 0.0f;
        const float windPhase = m_skyTime * 3.0f;
        for (size_t i = 0; i < sim.pos.size(); ++i) {
            glm::vec3 vel = (sim.pos[i] - sim.prev[i]) * drag;
            vel.y += g * dt;
            if (windAmp > 0.0f) {
                vel.x += windAmp * std::sin(windPhase + sim.pos[i].z * 0.9f) * dt;
                vel.z += windAmp * 0.35f * std::cos(windPhase + sim.pos[i].x * 0.7f) * dt;
            }
            glm::vec3 next = sim.pos[i] + vel;
            sim.prev[i] = sim.pos[i];
            sim.pos[i] = next;
        }
        // Pressure inflates the cloth outward from its center (local space).
        if (s.pressure > 0.0f) {
            const glm::vec3 center(0.0f, -0.4f, 0.0f);
            for (size_t i = 0; i < sim.pos.size(); ++i) {
                const glm::vec3 dir = sim.pos[i] - center;
                sim.pos[i] += glm::normalize(dir) * (s.pressure * 0.15f * dt);
            }
        }
        // Structural distance constraints (a few iterations keep it stable).
        for (int iter = 0; iter < 3; ++iter) {
            for (size_t r = 0; r < s.detail; ++r) {
                for (size_t c = 0; c < s.detail; ++c) {
                    const size_t i = r * side + c;
                    const size_t right = i + 1;
                    const float restR = glm::length(sim.rest[right] - sim.rest[i]);
                    glm::vec3 delta = sim.pos[right] - sim.pos[i];
                    const float dist = glm::length(delta);
                    if (dist > 1e-6f && restR > 1e-6f) {
                        const glm::vec3 corr = delta / dist * (dist - restR) * 0.5f;
                        sim.pos[i] += corr;
                        sim.pos[right] -= corr;
                    }
                    const size_t down = i + side;
                    const float restD = glm::length(sim.rest[down] - sim.rest[i]);
                    delta = sim.pos[down] - sim.pos[i];
                    const float dist2 = glm::length(delta);
                    if (dist2 > 1e-6f && restD > 1e-6f) {
                        const glm::vec3 corr = delta / dist2 * (dist2 - restD) * 0.5f;
                        sim.pos[i] += corr;
                        sim.pos[down] -= corr;
                    }
                }
            }
        }
        // Pin the top row to the rest pose (a hanging curtain).
        for (size_t c = 0; c < side; ++c) {
            sim.pos[c] = sim.rest[c];
            sim.prev[c] = sim.rest[c];
        }
        // Ground collision in local space (entity origin plane).
        for (size_t i = 0; i < sim.pos.size(); ++i) {
            if (sim.pos[i].y < 0.0f) {
                sim.pos[i].y = 0.0f;
                sim.prev[i].y = 0.0f;
            }
        }
        upload_softbody(sim, s);
    }

    // ---- Video flipbooks: advance the playhead at fps ----
    for (auto& [id, v] : scene->videoComponents) {
        (void)id;
        if (!v.enabled || !v.playing || v.framePaths.empty()) continue;
        v.time += dt;
        const float frameDur = 1.0f / std::max(v.fps, 0.1f);
        if (v.time >= frameDur) {
            v.time = 0.0f;
            v.currentFrame++;
            if (v.currentFrame >= static_cast<int>(v.framePaths.size())) {
                if (v.loop) v.currentFrame = 0;
                else {
                    v.currentFrame = static_cast<int>(v.framePaths.size()) - 1;
                    v.playing = false;
                }
            }
        }
    }

    // ---- Gaussian splats: mark for rebuild when regenerate is requested ----
    for (auto& [id, gs] : scene->gaussianSplatComponents) {
        if (!gs.regenerate) continue;
        gs.regenerate = false;
        auto it = m_splatClouds.find(id);
        if (it != m_splatClouds.end()) it->second.dirty = true;
    }

    // ---- Expressions: apply facial weights as squash/stretch of the head ----
    for (const auto& [id, ex] : scene->expressionComponents) {
        (void)id;
        if (!ex.enabled || !ex.headEntity.is_valid()) continue;
        const auto hit = scene->transformComponents.find(ex.headEntity);
        if (hit == scene->transformComponents.end()) continue;
        const float sx = 1.0f + ex.smile * 0.15f - ex.frown * 0.10f + ex.surprised * 0.22f + ex.anger * 0.06f;
        const float sy = 1.0f - ex.blink * 0.35f + ex.surprised * 0.26f - ex.smile * 0.06f - ex.frown * 0.04f;
        const float sz = 1.0f + ex.frown * 0.08f - ex.blink * 0.10f + ex.anger * 0.05f + ex.surprised * 0.06f;
        hit->second.scale = ex.baseScale * glm::vec3(sx, sy, sz);
    }

    // ---- Env probes: one-shot capture request + periodic real-time ----
    m_envCaptureTimer += dt;
    if (m_envCaptureTimer >= 0.5f) m_envCaptureTimer = 0.0f;
    for (auto& [id, ep] : scene->envProbeComponents) {
        if (!ep.enabled) continue;
        const bool captureNow = ep.captureRequested || (ep.realTime && m_envCaptureTimer <= 0.0f);
        if (ep.captureRequested) ep.captureRequested = false;
        if (!captureNow) continue;
        const auto tit = scene->transformComponents.find(id);
        if (tit != scene->transformComponents.end()) capture_env_probe(id, ep, tit->second);
    }
}

void EditorApplication::ensure_hair_sim(const UUID& id, const HairParticleComponent& h,
                                        const TransformComponent& t) {
    auto it = m_hairs.find(id);
    const uint32_t expectedVerts = (h.segments + 1) * h.count;
    if (it != m_hairs.end() && it->second.built &&
        it->second.pos.size() == expectedVerts && h.seed == 0) {
        return;
    }
    HairSim sim;
    sim.pos.resize(expectedVerts);
    sim.prev.resize(expectedVerts);
    sim.rest.resize(expectedVerts);
    std::mt19937 rng(h.seed != 0 ? h.seed
                                 : 1337u + static_cast<uint32_t>(id.get_high() ^ id.get_low()));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    // Roots: sample the mesh surface (first vertex of the mesh asset) or a
    // head-like sphere when no mesh is set.
    const glm::mat4 model = model_from_transform(t);
    const glm::vec3 up = glm::normalize(glm::mat3(model) * glm::vec3(0, 1, 0));
    const glm::vec3 tangent = glm::normalize(glm::mat3(model) * glm::vec3(1, 0, 0));
    const glm::vec3 bitangent = glm::normalize(glm::cross(tangent, up));
    for (uint32_t s = 0; s < h.count; ++s) {
        const float u = unit(rng), v = unit(rng);
        const float theta = u * glm::two_pi<float>();
        const float phi = std::acos(1.0f - 2.0f * v);
        // Head shell radius 0.24 (matches the humanoid head box half-extent).
        const glm::vec3 rootLocal(0.24f * std::sin(phi) * std::cos(theta),
                                  0.18f + 0.24f * std::cos(phi),
                                  0.24f * std::sin(phi) * std::sin(theta));
        const glm::vec3 root = glm::vec3(model * glm::vec4(rootLocal, 1.0f));
        // Strand direction: mostly up with a random tilt (randomness).
        const glm::vec3 tilt = glm::normalize(
            tangent * (unit(rng) - 0.5f) * 2.0f * h.randomness +
            bitangent * (unit(rng) - 0.5f) * 2.0f * h.randomness);
        const glm::vec3 dir = glm::normalize(up + tilt * 0.6f);
        const float segLen = h.length / static_cast<float>(h.segments);
        for (uint32_t seg = 0; seg <= h.segments; ++seg) {
            const size_t idx = static_cast<size_t>(s) * (h.segments + 1) + seg;
            sim.rest[idx] = root + dir * (segLen * static_cast<float>(seg));
            sim.pos[idx] = sim.rest[idx];
            sim.prev[idx] = sim.rest[idx];
        }
    }
    sim.built = true;
    m_hairs[id] = std::move(sim);
}

void EditorApplication::upload_hair(HairSim& sim, const HairParticleComponent& h) {
    if (sim.pos.empty()) return;
    std::vector<EditorVertex> verts;
    verts.reserve(sim.pos.size() * 2);
    for (uint32_t s = 0; s < h.count; ++s) {
        for (uint32_t seg = 0; seg < h.segments; ++seg) {
            const size_t a = static_cast<size_t>(s) * (h.segments + 1) + seg;
            const size_t b = a + 1;
            EditorVertex va, vb;
            va.pos = sim.pos[a];
            vb.pos = sim.pos[b];
            va.normal = vb.normal = glm::vec3(0, 1, 0);
            va.color = vb.color = h.color;
            verts.push_back(va);
            verts.push_back(vb);
        }
    }
    const VkDeviceSize size = sizeof(EditorVertex) * verts.size();
    if (sim.vb.buffer == VK_NULL_HANDLE || sim.vb.size < size) {
        if (sim.vb.buffer != VK_NULL_HANDLE) destroy_buffer(sim.vb);
        create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sim.vb.buffer, sim.vb.memory);
    }
    safe_map_and_copy(m_device, sim.vb.memory, 0, size, verts.data());
    sim.vertexCount = static_cast<uint32_t>(verts.size());
}

void EditorApplication::ensure_softbody_sim(const UUID& id, const SoftBodyComponent& s,
                                            const TransformComponent& t) {
    auto it = m_softBodies.find(id);
    const size_t side = static_cast<size_t>(s.detail) + 1;
    const size_t expected = side * side;
    if (it != m_softBodies.end() && it->second.built && it->second.pos.size() == expected) return;
    (void)t;
    SoftBodySim sim;
    sim.pos.resize(expected);
    sim.prev.resize(expected);
    sim.rest.resize(expected);
    const float half = 1.0f;
    for (size_t r = 0; r < side; ++r) {
        for (size_t c = 0; c < side; ++c) {
            const size_t i = r * side + c;
            const float x = (static_cast<float>(c) / s.detail - 0.5f) * 2.0f * half;
            const float z = (static_cast<float>(r) / s.detail - 0.5f) * 2.0f * half;
            sim.rest[i] = glm::vec3(x, 0.0f, z);
            sim.pos[i] = sim.rest[i];
            sim.prev[i] = sim.rest[i];
        }
    }
    sim.indices.clear();
    sim.indices.reserve(s.detail * s.detail * 6);
    for (size_t r = 0; r < s.detail; ++r) {
        for (size_t c = 0; c < s.detail; ++c) {
            const uint32_t a = static_cast<uint32_t>(r * side + c);
            const uint32_t b = static_cast<uint32_t>(a + 1);
            const uint32_t cc = static_cast<uint32_t>(a + side);
            const uint32_t d = static_cast<uint32_t>(cc + 1);
            sim.indices.push_back(a); sim.indices.push_back(b); sim.indices.push_back(d);
            sim.indices.push_back(a); sim.indices.push_back(d); sim.indices.push_back(cc);
        }
    }
    sim.indexCount = static_cast<uint32_t>(sim.indices.size());
    sim.built = true;
    m_softBodies[id] = std::move(sim);
}

void EditorApplication::upload_softbody(SoftBodySim& sim, const SoftBodyComponent& s) {
    if (sim.pos.empty()) return;
    std::vector<EditorVertex> verts;
    verts.reserve(sim.pos.size());
    const glm::vec3 color = glm::vec3(0.75f, 0.45f, 0.95f);
    for (const glm::vec3& p : sim.pos) {
        EditorVertex v;
        v.pos = p;
        v.normal = glm::vec3(0, 1, 0);
        v.color = color;
        verts.push_back(v);
    }
    const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize is = sizeof(uint32_t) * sim.indices.size();
    if (sim.vb.buffer == VK_NULL_HANDLE || sim.vb.size < vs) {
        if (sim.vb.buffer != VK_NULL_HANDLE) destroy_buffer(sim.vb);
        create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sim.vb.buffer, sim.vb.memory);
    }
    if (sim.ib.buffer == VK_NULL_HANDLE || sim.ib.size < is) {
        if (sim.ib.buffer != VK_NULL_HANDLE) destroy_buffer(sim.ib);
        create_buffer(is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sim.ib.buffer, sim.ib.memory);
    }
    safe_map_and_copy(m_device, sim.vb.memory, 0, vs, verts.data());
    safe_map_and_copy(m_device, sim.ib.memory, 0, is, sim.indices.data());
}

void EditorApplication::generate_splat_cloud(const GaussianSplatComponent& gs,
                                             std::vector<EditorVertex>& verts) const {
    verts.clear();
    verts.reserve(gs.count);
    std::mt19937 rng(gs.seed != 0 ? gs.seed : 1u);
    std::uniform_real_distribution<float> box(-gs.scale * 0.5f, gs.scale * 0.5f);
    std::uniform_real_distribution<float> hue(0.0f, 1.0f);
    for (uint32_t i = 0; i < gs.count; ++i) {
        EditorVertex v;
        v.pos = glm::vec3(box(rng), box(rng), box(rng));
        // Pastel palette via golden-ratio hue.
        const float hh = hue(rng);
        const float s = 0.55f + 0.4f * hue(rng);
        const float l = 0.45f + 0.3f * hue(rng);
        glm::vec3 rgb;
        const float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
        const float x = c * (1.0f - std::abs(std::fmod(hh * 6.0f, 2.0f) - 1.0f));
        const float m = l - c * 0.5f;
        if (hh < 1.0f / 6.0f) rgb = { c, x, 0 };
        else if (hh < 2.0f / 6.0f) rgb = { x, c, 0 };
        else if (hh < 3.0f / 6.0f) rgb = { 0, c, x };
        else if (hh < 4.0f / 6.0f) rgb = { 0, x, c };
        else if (hh < 5.0f / 6.0f) rgb = { x, 0, c };
        else rgb = { c, 0, x };
        v.color = rgb + m;
        v.normal = glm::vec3(0, 1, 0);
        verts.push_back(v);
    }
}

void EditorApplication::rebuild_paint_buffer(const UUID& id, PaintComponent& pc,
                                             const EditorMeshResource* mesh) {
    auto it = m_paintBuffers.find(id);
    if (it == m_paintBuffers.end() || it->second.dirty) {
        if (!mesh || mesh->cpuPositions.empty()) return;
        const size_t n = mesh->cpuPositions.size();
        std::vector<EditorVertex> verts;
        verts.reserve(n);
        const bool hasColors = pc.vertexColors.size() == n;
        for (size_t i = 0; i < n; ++i) {
            EditorVertex v;
            v.pos = mesh->cpuPositions[i];
            v.normal = glm::vec3(0, 1, 0);
            v.color = hasColors ? glm::vec3(pc.vertexColors[i]) : glm::vec3(1.0f);
            verts.push_back(v);
        }
        const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
        if (it == m_paintBuffers.end()) it = m_paintBuffers.emplace(id, PaintData{}).first;
        if (it->second.vb.buffer != VK_NULL_HANDLE && it->second.vb.size < vs) {
            destroy_buffer(it->second.vb);
            it->second.vb = GPUBuffer{};
        }
        if (it->second.vb.buffer == VK_NULL_HANDLE) {
            create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          it->second.vb.buffer, it->second.vb.memory);
        }
        safe_map_and_copy(m_device, it->second.vb.memory, 0, vs, verts.data());
        it->second.vertexCount = static_cast<uint32_t>(n);
        it->second.dirty = false;
    }
}

void EditorApplication::capture_env_probe(const UUID& id, const EnvProbeComponent& ep,
                                          const TransformComponent& t) {
    if (m_device == VK_NULL_HANDLE) return;
    const uint32_t size = ep.resolution >= 64 ? ep.resolution : 256;
    const bool recreate = !m_envCapture.valid || m_envCapture.size != size ||
                          m_envCapture.entity != id;
    // Rebuild the cubemap target when the probe/resolution changes.
    if (recreate) {
        if (m_envCapture.valid) {
            for (int i = 0; i < 6; ++i) {
                if (m_envCapture.framebuffers[i]) vkDestroyFramebuffer(m_device, m_envCapture.framebuffers[i], nullptr);
                if (m_envCapture.views[i]) vkDestroyImageView(m_device, m_envCapture.views[i], nullptr);
            }
            if (m_envCapture.image) vkDestroyImage(m_device, m_envCapture.image, nullptr);
            if (m_envCapture.memory) vkFreeMemory(m_device, m_envCapture.memory, nullptr);
            if (m_envCapture.renderPass) vkDestroyRenderPass(m_device, m_envCapture.renderPass, nullptr);
            if (m_envCapture.sampler) vkDestroySampler(m_device, m_envCapture.sampler, nullptr);
            m_envCapture = EnvProbeCapture{};
        }
        const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        // Cubemap color image (6 layers).
        VkImageCreateInfo img{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        img.imageType = VK_IMAGE_TYPE_2D;
        img.extent = { size, size, 1 };
        img.mipLevels = 1;
        img.arrayLayers = 6;
        img.format = colorFormat;
        img.tiling = VK_IMAGE_TILING_OPTIMAL;
        img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img.samples = VK_SAMPLE_COUNT_1_BIT;
        img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if (vkCreateImage(m_device, &img, nullptr, &m_envCapture.image) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_device, m_envCapture.image, &req);
        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_envCapture.memory) != VK_SUCCESS) return;
        vkBindImageMemory(m_device, m_envCapture.image, m_envCapture.memory, 0);
        // 2D-array views, one per face.
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = m_envCapture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = colorFormat;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < 6; ++i) {
            view.subresourceRange.baseArrayLayer = static_cast<uint32_t>(i);
            if (vkCreateImageView(m_device, &view, nullptr, &m_envCapture.views[i]) != VK_SUCCESS) return;
        }
        // Depth image (single 2D, shared by all faces).
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        create_image(size, size, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthMemory);
        depthView = create_image_view(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
        // Render pass: color + depth.
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = colorFormat;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[1].format = depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments = attachments;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_envCapture.renderPass) != VK_SUCCESS) return;
        for (int i = 0; i < 6; ++i) {
            VkImageView fbViews[2] = { m_envCapture.views[i], depthView };
            VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = m_envCapture.renderPass;
            fb.attachmentCount = 2;
            fb.pAttachments = fbViews;
            fb.width = size;
            fb.height = size;
            fb.layers = 1;
            if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_envCapture.framebuffers[i]) != VK_SUCCESS) return;
        }
        // Sampler + descriptor update.
        VkSamplerCreateInfo samp{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samp.magFilter = VK_FILTER_LINEAR;
        samp.minFilter = VK_FILTER_LINEAR;
        samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(m_device, &samp, nullptr, &m_envCapture.sampler);
        VkImageView cubeView = VK_NULL_HANDLE;
        VkImageViewCreateInfo cv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        cv.image = m_envCapture.image;
        cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        cv.format = colorFormat;
        cv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        vkCreateImageView(m_device, &cv, nullptr, &cubeView);
        VkDescriptorImageInfo descImg{};
        descImg.sampler = m_envCapture.sampler;
        descImg.imageView = cubeView;
        descImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = m_envCapture.descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descImg;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        // Keep the cube view alive for the lifetime of the capture.
        m_envCubeView = cubeView;
        m_envDepthView = depthView;
        m_envDepthImage = depthImage;
        m_envDepthMemory = depthMemory;
        m_envCapture.size = size;
        m_envCapture.entity = id;
        m_envCapture.valid = true;
    }
    m_envCapturePending = true;
}

UUID EditorApplication::resolve_texture_asset_by_name(const std::string& name) const {
    for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
        if (meta.type == AssetType::Texture && meta.sourcePath.filename().string() == name) {
            return meta.id;
        }
    }
    return UUID{ 0, 0 };
}

void EditorApplication::record_env_face(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj,
                                        const glm::vec3& pos, Scene* scene) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    (void)pos;
    const glm::mat4 viewProj = proj * view;
    // Terrain + voxel volumes (the shared scene helpers).
    if (m_terrainValid && m_terrainVB.buffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        const glm::mat4 model(1.0f);
        draw_indexed_editor_mesh(cmd, m_scenePipelineLayout, m_terrainVB.buffer, m_terrainIB.buffer,
                                 m_terrainIndexCount, viewProj * model, glm::vec4(1.0f));
    }
    draw_voxel_volumes(cmd, viewProj, scene);
    // Entities with a mesh renderer (flat shading — the capture is a
    // geometry/lighting preview for the reflection probe).
    for (const auto& [id, ent] : scene->get_entities()) {
        (void)ent;
        const auto tit = scene->transformComponents.find(id);
        if (tit == scene->transformComponents.end()) continue;
        const auto layerIt = scene->layerComponents.find(id);
        if (layerIt != scene->layerComponents.end() && !layerIt->second.visible) continue;
        const auto meshIt = scene->meshRendererComponents.find(id);
        if (meshIt == scene->meshRendererComponents.end()) continue;
        glm::vec3 baseColor(0.72f, 0.75f, 0.82f);
        const auto matIt = scene->materialComponents.find(id);
        if (matIt != scene->materialComponents.end()) baseColor = matIt->second.albedo;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        if (const auto* mesh = get_mesh_resource(meshIt->second.meshAssetID)) {
            draw_mesh_resource(cmd, viewProj * model_from_transform(tit->second),
                               glm::vec4(baseColor, 1.0f), *mesh);
        } else {
            draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                              m_cubeIndexCount, viewProj * model_from_transform(tit->second),
                              glm::vec4(baseColor, 1.0f));
        }
    }
}

glm::vec3 EditorApplication::viewport_mouse_dir(const glm::vec2& mouseScreen) const {
    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 invViewProj =
        glm::inverse(m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    const float ndcX = (mouseScreen.x - m_viewportImagePos.x) / std::max(1.0f, m_viewportImageSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouseScreen.y - m_viewportImagePos.y) / std::max(1.0f, m_viewportImageSize.y) * 2.0f;
    const glm::vec4 near4 = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 far4 = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearP = glm::vec3(near4) / near4.w;
    const glm::vec3 farP = glm::vec3(far4) / far4.w;
    return glm::normalize(farP - nearP);
}

bool EditorApplication::paint_mesh_stroke(const glm::vec3& origin, const glm::vec3& dir) {
    if (!m_editorScene) return false;
    Scene* scene = m_editorScene.get();
    const UUID target = m_selectedEntity.is_valid() ? m_selectedEntity.get_id() : UUID{ 0, 0 };
    if (!target.is_valid() || !scene->paintComponents.contains(target)) return false;
    const auto meshComp = scene->meshRendererComponents.find(target);
    if (meshComp == scene->meshRendererComponents.end() ||
        !meshComp->second.meshAssetID.is_valid()) {
        return false;
    }
    const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID);
    if (!mesh || mesh->cpuPositions.empty()) return false;
    const auto tit = scene->transformComponents.find(target);
    if (tit == scene->transformComponents.end()) return false;
    const glm::mat4 model = model_from_transform(tit->second);
    const glm::mat4 invModel = glm::inverse(model);
    const glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(origin, 1.0f));
    const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(dir, 0.0f)));
    const std::vector<glm::vec3>& P = mesh->cpuPositions;
    const std::vector<uint32_t>& idx = mesh->cpuIndices;
    // Möller–Trumbore against the mesh triangles (local space).
    const auto rayTri = [](const glm::vec3& o, const glm::vec3& d,
                           const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           float& t, glm::vec3& bary) -> bool {
        const glm::vec3 e1 = b - a;
        const glm::vec3 e2 = c - a;
        const glm::vec3 pv = glm::cross(d, e2);
        const float det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-8f) return false;
        const float invDet = 1.0f / det;
        const glm::vec3 tv = o - a;
        const float u = glm::dot(tv, pv) * invDet;
        if (u < 0.0f || u > 1.0f) return false;
        const glm::vec3 qv = glm::cross(tv, e1);
        const float v = glm::dot(d, qv) * invDet;
        if (v < 0.0f || u + v > 1.0f) return false;
        t = glm::dot(e2, qv) * invDet;
        if (t < 0.0f) return false;
        bary = { 1.0f - u - v, u, v };
        return true;
    };
    float bestT = 1e30f;
    glm::vec3 bestHit(0.0f);
    if (idx.empty()) {
        for (size_t i = 0; i + 2 < P.size(); i += 3) {
            float t;
            glm::vec3 bary;
            if (rayTri(localOrigin, localDir, P[i], P[i + 1], P[i + 2], t, bary) && t < bestT) {
                bestT = t;
                bestHit = P[i] * bary.x + P[i + 1] * bary.y + P[i + 2] * bary.z;
            }
        }
    } else {
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const glm::vec3& a = P[idx[i]];
            const glm::vec3& b = P[idx[i + 1]];
            const glm::vec3& c = P[idx[i + 2]];
            float t;
            glm::vec3 bary;
            if (rayTri(localOrigin, localDir, a, b, c, t, bary) && t < bestT) {
                bestT = t;
                bestHit = a * bary.x + b * bary.y + c * bary.z;
            }
        }
    }
    if (bestT > 1e29f) return false;
    PaintComponent& pc = scene->paintComponents[target];
    if (pc.vertexColors.size() != P.size()) pc.vertexColors.assign(P.size(), glm::vec4(1.0f));
    const float brush = std::max(pc.brushSize, 0.01f);
    for (size_t i = 0; i < P.size(); ++i) {
        const float d = glm::distance(P[i], bestHit);
        if (d <= brush) {
            const float falloff = 1.0f - d / brush;
            const float op = pc.opacity * falloff;
            const glm::vec3 brushCol = pc.brushColor;
            const glm::vec3 oldCol(pc.vertexColors[i]);
            pc.vertexColors[i] = glm::vec4(glm::mix(oldCol, brushCol, glm::clamp(op, 0.0f, 1.0f)), 1.0f);
        }
    }
    const auto pit = m_paintBuffers.find(target);
    if (pit != m_paintBuffers.end()) pit->second.dirty = true;
    mark_scene_dirty();
    return true;
}

void EditorApplication::record_env_capture(VkCommandBuffer cmd, Scene* scene) {
    if (!m_envCapturePending || !m_envCapture.valid || !scene) return;
    m_envCapturePending = false;
    const auto tit = scene->transformComponents.find(m_envCapture.entity);
    if (tit == scene->transformComponents.end()) return;
    const glm::vec3 pos = tit->second.position;
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 500.0f);
    const glm::vec3 faces[6][2] = {
        { { 1, 0, 0 }, { 0, -1, 0 } }, { { -1, 0, 0 }, { 0, -1, 0 } },
        { { 0, 1, 0 }, { 0, 0, 1 } },  { { 0, -1, 0 }, { 0, 0, -1 } },
        { { 0, 0, 1 }, { 0, -1, 0 } }, { { 0, 0, -1 }, { 0, -1, 0 } },
    };
    for (int i = 0; i < 6; ++i) {
        VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        info.renderPass = m_envCapture.renderPass;
        info.framebuffer = m_envCapture.framebuffers[i];
        info.renderArea.offset = { 0, 0 };
        info.renderArea.extent = { m_envCapture.size, m_envCapture.size };
        VkClearValue clears[2];
        clears[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
        clears[1].depthStencil = { 1.0f, 0 };
        info.clearValueCount = 2;
        info.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
        set_viewport_scissor(cmd, m_envCapture.size, m_envCapture.size);
        record_env_face(cmd, glm::lookAt(pos, pos + faces[i][0], faces[i][1]), proj, pos, scene);
        vkCmdEndRenderPass(cmd);
    }
}

void EditorApplication::tick_play_runtime(float deltaTime) {
    const PlayState state = m_playMode.get_state();
    if (state != PlayState::Play && state != PlayState::Simulate) return;
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;

    // Wicked-port runtime: force fields push/pull bodies within range;
    // springs pull their body back toward the authored rest anchor. Both run
    // before the solver step so the forces feed this frame's simulation.
    for (const auto& [id, ff] : playScene->forceFieldComponents) {
        if (!ff.enabled) continue;
        const auto fit = playScene->transformComponents.find(id);
        const glm::vec3 center = (fit != playScene->transformComponents.end())
                                     ? fit->second.position
                                     : glm::vec3(0.0f);
        glm::vec3 forward(0.0f, 0.0f, 1.0f);
        if (fit != playScene->transformComponents.end()) {
            forward = glm::quat(glm::radians(fit->second.rotation)) * glm::vec3(0.0f, 0.0f, 1.0f);
        }
        for (const auto& [bid, handle] : m_playBodies) {
            Physics::RigidBody* body = m_playPhysics.body(handle);
            if (!body || !body->dynamic()) continue;
            const glm::vec3 delta = body->position - center;
            const float dist = glm::length(delta);
            if (dist > ff.range) continue;
            const float falloff = 1.0f - (ff.range > 0.01f ? dist / ff.range : 0.0f);
            glm::vec3 force(0.0f);
            switch (ff.type) {
                case ForceFieldType::Gravity:
                    force = glm::vec3(0.0f, -ff.strength * 9.81f, 0.0f);
                    break;
                case ForceFieldType::Push:
                    force = forward * (ff.strength * 40.0f);
                    break;
                case ForceFieldType::Wind:
                    force = forward * (ff.strength * 15.0f);
                    break;
                case ForceFieldType::Vortex: {
                    const glm::vec3 tangent = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), delta));
                    force = tangent * (ff.strength * 30.0f) + glm::vec3(0.0f, ff.strength * 2.0f, 0.0f);
                    break;
                }
            }
            m_playPhysics.add_force(handle, force * falloff);
        }
    }
    for (const auto& [id, sp] : playScene->springComponents) {
        if (sp.disabled || !sp.enabled) continue;
        const auto restIt = m_springRests.find(id);
        const auto bodyIt = m_playBodies.find(id);
        if (restIt == m_springRests.end() || bodyIt == m_playBodies.end()) continue;
        Physics::RigidBody* body = m_playPhysics.body(bodyIt->second);
        if (!body || !body->dynamic()) continue;
        const glm::vec3 force = (restIt->second - body->position) * (12.0f * sp.stiffness)
                              - body->linearVelocity * (2.0f * sp.drag);
        m_playPhysics.add_force(bodyIt->second, force);
    }
    // Wicked-port runtime: constraints as soft point-to-point springs between
    // the entity's anchor and the target body's anchor (fixed when the other
    // entity is missing). Broken when the required force exceeds breakForce.
    for (const auto& [id, cn] : playScene->constraintComponents) {
        if (!cn.enabled) continue;
        auto restIt = m_constraintRests.find(id);
        const auto bodyIt = m_playBodies.find(id);
        if (restIt == m_constraintRests.end() || bodyIt == m_playBodies.end()) continue;
        ConstraintRest& rest = restIt->second;
        if (rest.broken) continue;
        Physics::RigidBody* bodyA = m_playPhysics.body(bodyIt->second);
        if (!bodyA || !bodyA->dynamic()) continue;
        Physics::RigidBody* bodyB = nullptr;
        Physics::BodyHandle bodyBHandle = Physics::InvalidBody;
        if (const auto bodyBIt = m_playBodies.find(cn.otherEntity); bodyBIt != m_playBodies.end()) {
            bodyB = m_playPhysics.body(bodyBIt->second);
            bodyBHandle = bodyBIt->second;
        }
        const glm::vec3 anchorA = bodyA->position + cn.anchor;
        glm::vec3 anchorB = rest.anchorB;
        if (bodyB && bodyB->dynamic()) anchorB = bodyB->position + cn.anchor;
        const glm::vec3 delta = anchorB - anchorA;
        const float dist = glm::length(delta);
        glm::vec3 force(0.0f);
        if (dist > 1e-5f) {
            const glm::vec3 dir = delta / dist;
            if (cn.type == ConstraintType::Spring) {
                force = dir * ((dist - rest.restLength) * 25.0f);
            } else {
                // Fixed / Hinge / Point: pull the anchors together (stiff).
                force = dir * (dist * 40.0f);
            }
        }
        force -= bodyA->linearVelocity * 0.8f; // axial damping, no ringing
        if (cn.breakForce > 0.0f && glm::length(force) > cn.breakForce) {
            rest.broken = true;
            continue;
        }
        m_playPhysics.add_force(bodyIt->second, force);
        if (bodyB && bodyB->dynamic() && bodyBHandle != Physics::InvalidBody) {
            m_playPhysics.add_force(bodyBHandle, -force);
        }
    }

    m_playPhysics.step(deltaTime);
    for (const auto& [id, handle] : m_playBodies) {
        Physics::RigidBody* body = m_playPhysics.body(handle);
        if (!body) continue;
        // NaN/inf guard: a solver blow-up (e.g. a body catapulted by a broken
        // constraint) must never poison a transform, or the view/projection
        // matrices become NaN and the whole viewport renders black.
        const auto finite3 = [](const glm::vec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const auto finiteQ = [](const glm::quat& q) {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        };
        if (!finite3(body->position) || !finiteQ(body->rotation)) {
            // Reset the body to a sane resting state instead of propagating NaN.
            body->position = glm::vec3(0.0f, 1.0f, 0.0f);
            body->linearVelocity = glm::vec3(0.0f);
            body->angularVelocity = glm::vec3(0.0f);
            body->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            continue;
        }
        auto tit = playScene->transformComponents.find(id);
        if (tit == playScene->transformComponents.end()) continue;
        tit->second.position = body->position;
        tit->second.rotation = glm::degrees(glm::eulerAngles(body->rotation));
    }

    // Wicked-port runtime: spline followers drive their entity along the
    // Catmull-Rom path (looped) in play mode; kinematic bodies follow too.
    for (const auto& [id, sp] : playScene->splineComponents) {
        if (!sp.enabled || sp.points.size() < 2) continue;
        auto tit = playScene->transformComponents.find(id);
        if (tit == playScene->transformComponents.end()) continue;
        auto [progIt, inserted] = m_splineProgress.try_emplace(id, 0.0f);
        (void)inserted;
        float total = 0.0f;
        for (size_t i = 1; i < sp.points.size(); ++i) {
            total += glm::length(sp.points[i] - sp.points[i - 1]);
        }
        if (total < 1e-4f) continue;
        constexpr float kSplineSpeed = 2.0f; // m/s
        progIt->second += kSplineSpeed * deltaTime / total;
        if (sp.looped) {
            progIt->second -= std::floor(progIt->second);
        } else {
            progIt->second = glm::clamp(progIt->second, 0.0f, 1.0f);
        }
        const float t = progIt->second * static_cast<float>(sp.points.size() - 1);
        const size_t i = std::min<size_t>(static_cast<size_t>(t), sp.points.size() - 2);
        const float f = t - static_cast<float>(i);
        const glm::vec3& p0 = sp.points[i > 0 ? i - 1 : i];
        const glm::vec3& p1 = sp.points[i];
        const glm::vec3& p2 = sp.points[i + 1];
        const glm::vec3& p3 = sp.points[std::min(i + 2, sp.points.size() - 1)];
        const float f2 = f * f, f3 = f2 * f;
        const glm::vec3 pos = 0.5f * ((2.0f * p1) + (-p0 + p2) * f
            + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2
            + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
        tit->second.position = pos;
        const auto bodyIt = m_playBodies.find(id);
        if (bodyIt != m_playBodies.end()) {
            Physics::RigidBody* body = m_playPhysics.body(bodyIt->second);
            if (body) {
                body->position = pos;
                body->linearVelocity = glm::vec3(0.0f);
                body->angularVelocity = glm::vec3(0.0f);
            }
        }
    }

    // Play-world animation (Animation/Timeline/IK/Retarget): the editors now
    // Apply to the scene, so the runtime advances timelines, samples clips
    // into bone-entity transforms, solves IK chains and mirrors retarget
    // mappings. Runs after physics so animation wins over the solver.
    tick_animation_runtime(playScene, deltaTime);

    // Hot reload: recompile + swap the program when the .script changes on
    // disk (variables survive — load() keeps the variable map).
    if (m_playScriptLoaded) {
        std::string reloadError;
        if (m_playScriptReloader.reload_if_changed(m_playScript, &reloadError)) {
            std::cout << "[Editor] Script hot-reloaded: " << m_playScriptPath.filename().string()
                      << (reloadError.empty() ? "" : " (" + reloadError + ")") << std::endl;
            m_playScript.start_event("OnStart");
        }
        // Debugger-aware tick: hold on a panel pause or a breakpoint pause;
        // otherwise drive the VM through the debugger so breakpoints halt it
        // and the panel stays in sync (variables/ip/call stack).
        const bool breakpointPaused = m_playScript.status() == VMStatus::Paused;
        if (!m_scriptPauseRequested && !breakpointPaused) {
            if (m_scriptDebuggerAttached) m_scriptDebugger.continue_run(10000, deltaTime);
            else m_playScript.run(deltaTime, 10000);
            if (m_playScript.status() == VMStatus::Paused) return; // hit a breakpoint
            std::vector<std::string> emitted;
            m_playScript.consume_emitted_events(emitted);
            for (const std::string& event : emitted) m_playScript.start_event(event);
        if (m_playScript.has_event("Tick")) {
            m_playScript.start_event("Tick");
            if (m_scriptDebuggerAttached) m_scriptDebugger.continue_run(10000, deltaTime);
            else m_playScript.run(deltaTime, 10000);
        }
    }

    // Play weapons (Fase 8): one WeaponRuntime per WeaponComponent entity,
    // fired with the viewport camera ray against the play physics on SPACE.
    const bool fireHeld = glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS;
    const glm::mat4 camView = m_editorCamera.get_view_matrix();
    const glm::vec3 camFront = glm::normalize(
        glm::vec3(-camView[2][0], -camView[2][1], -camView[2][2]));
    for (const auto& [id, comp] : playScene->weaponComponents) {
        auto it = m_playWeapons.find(id);
        if (it == m_playWeapons.end()) {
            Engine::WeaponDefinition def;
            def.id = id;
            def.name = "Scene Weapon";
            def.fireMode = comp.automatic ? Engine::FireMode::Automatic : Engine::FireMode::Single;
            def.magazineSize = comp.magazineSize;
            def.reserveAmmo = comp.reserveAmmo;
            def.roundsPerMinute = comp.roundsPerMinute;
            def.damage = comp.damage;
            def.range = 120.0f;
            def.spreadDegrees = comp.spreadDegrees;
            def.hitscan = comp.hitscan;
            it = m_playWeapons.emplace(id, Engine::WeaponRuntime(std::move(def))).first;
            it->second.set_raycast([this](const glm::vec3& o, const glm::vec3& d, float maxDist)
                                       -> std::optional<Engine::WeaponHit> {
                const auto hit = m_playPhysics.raycast(o, d, maxDist);
                if (!hit) return std::nullopt;
                Engine::WeaponHit out;
                out.position = hit->point;
                out.normal = hit->normal;
                out.distance = hit->distance;
                return out;
            });
        }
        if (fireHeld) it->second.trigger_pressed(m_editorCamera.position, camFront);
        else it->second.trigger_released();
        it->second.update(deltaTime, m_editorCamera.position, camFront);
    }
    if (fireHeld && !m_playWeaponStatusLogged && !m_playWeapons.empty()) {
        m_playWeaponStatusLogged = true;
        std::cout << "[Editor] Play weapon firing via physics raycast ("
                  << m_playWeapons.size() << " weapon entity/entities)\n";
    }

    // Play particles (Fase 8): keep each emitter at its entity's world
    // position and step the simulation against the play physics.
    for (const auto& [id, emitter] : m_playEmitters) {
        auto* desc = m_playParticles.emitter(emitter);
        if (!desc) continue;
        const auto tit = playScene->transformComponents.find(id);
        const auto pe = playScene->particleEmitterComponents.find(id);
        const glm::vec3 localPos =
            (pe != playScene->particleEmitterComponents.end()) ? pe->second.position : glm::vec3(0.0f);
        desc->position = (tit != playScene->transformComponents.end())
                             ? tit->second.position + localPos
                             : localPos;
    }
    m_playParticles.update(deltaTime, &m_playPhysics);

    // Play vehicles (Fase 8): drive with the arrow keys.
    const bool throttle = glfwGetKey(m_window, GLFW_KEY_UP) == GLFW_PRESS;
    const bool brake = glfwGetKey(m_window, GLFW_KEY_DOWN) == GLFW_PRESS;
    const float steer = (glfwGetKey(m_window, GLFW_KEY_RIGHT) == GLFW_PRESS ? 1.0f : 0.0f) -
                        (glfwGetKey(m_window, GLFW_KEY_LEFT) == GLFW_PRESS ? 1.0f : 0.0f);
    for (auto& [id, vehicle] : m_playVehicles) {
        (void)id;
        Engine::Gameplay::VehicleInput input;
        input.throttle = throttle ? 1.0f : 0.0f;
        input.brake = brake ? 1.0f : 0.0f;
        input.steering = steer;
        vehicle.set_input(input);
        vehicle.update(m_playPhysics, deltaTime);
    }

    // Play missions (Fase 8): step the graph and mirror the live state back
    // to the component. Events emitted by the play script (consume_emitted)
    // are dispatched to the mission system, so authored script events can
    // complete missions.
    m_playMissions.update(deltaTime);
    for (auto& [id, mc] : playScene->missionComponents) {
        const Engine::Gameplay::Mission* mission = m_playMissions.mission("mission_" + id.to_string());
        mc.active = mission && mission->is_active();
    }
    {
        std::vector<std::string> emitted;
        if (m_playScriptLoaded) m_playScript.consume_emitted_events(emitted);
        for (const std::string& event : emitted) m_playMissions.dispatch_event(event);
    }

    // Play dialogues (Fase 8): mirror the playing state.
    for (auto& [id, dc] : playScene->dialogueComponents) {
        dc.playing = m_playDialogues.is_playing();
    }

    // Play audio (Fase 8): keep the listener on the camera and render one
    // mix block per frame so voices advance; drop voices that finished.
    m_playAudio.set_listener(m_editorCamera.position, camFront);
    for (const auto& [id, voice] : m_playVoices) {
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            m_playAudio.set_voice_position(voice, tit->second.position);
        }
        auto ac = playScene->audioComponents.find(id);
        if (ac != playScene->audioComponents.end()) ac->second.playing = m_playAudio.is_active(voice);
    }

    // Play destructibles (Fase 8): weapon hits from this frame apply radial
    // damage (chunks detach with an impulse) and the destroyed flag syncs.
    std::vector<glm::vec3> hitPoints;
    for (const auto& [id, comp] : playScene->weaponComponents) {
        (void)id;
        auto it = m_playWeapons.find(id);
        if (it == m_playWeapons.end()) continue;
        for (const Engine::WeaponHit& hit : it->second.hits()) hitPoints.push_back(hit.position);
        it->second.clear_hits();
    }
    for (auto& [id, runtime] : m_playDestructibles) {
        auto dc = playScene->destructionComponents.find(id);
        for (const glm::vec3& point : hitPoints) {
            runtime.apply_radial_damage(m_playPhysics, point, dc->second.damageRadius, 25.0f, dc->second.damageImpulse);
        }
        if (dc != playScene->destructionComponents.end()) {
            dc->second.destroyed = runtime.fully_destroyed();
        }
    }

    // Play navigation (Fase 8): repath toward the camera entity when the agent
    // arrives (or the target moved), then write the agent position back.
    glm::vec3 target{0.0f};
    bool haveTarget = false;
    for (const auto& [cid, cam] : playScene->cameraComponents) {
        (void)cam;
        const auto tit = playScene->transformComponents.find(cid);
        if (tit != playScene->transformComponents.end()) {
            target = tit->second.position;
            haveTarget = true;
            break;
        }
    }
    if (!haveTarget) target = m_editorCamera.position;
    for (auto& [id, agent] : m_playNavAgents) {
        if (m_playNav &&
            (agent.reached_destination() ||
             glm::distance(agent.position, target) > 1.0f)) {
            engine::navigation::PathResult result;
            if (m_playNav->find_path(agent.position.x, agent.position.y,
                                     agent.position.z, target.x, target.y,
                                     target.z, result) && result.found) {
                std::vector<glm::vec3> points;
                points.reserve(result.waypoints.size() / 3);
                for (std::size_t i = 0; i + 2 < result.waypoints.size(); i += 3) {
                    points.emplace_back(result.waypoints[i], result.waypoints[i + 1],
                                        result.waypoints[i + 2]);
                }
                agent.set_path(std::move(points));
            }
        }
        agent.update(deltaTime);
        auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) tit->second.position = agent.position;
    }
}
}

void EditorApplication::teardown_play_runtime() {
    m_constraintRests.clear();
    m_springRests.clear();
    m_splineProgress.clear();
    m_animStates.clear();
    m_animClips.clear();
    if (m_playGroundBody != Physics::InvalidBody) {
        m_playPhysics.destroy_body(m_playGroundBody);
        m_playGroundBody = Physics::InvalidBody;
    }
    for (const Physics::BodyHandle handle : m_playStaticBodies) {
        m_playPhysics.destroy_body(handle);
    }
    m_playStaticBodies.clear();
    for (const auto& [id, handle] : m_playBodies) {
        (void)id;
        m_playPhysics.destroy_body(handle);
    }
    m_playBodies.clear();        m_playScriptLoaded = false;
    m_scriptDebugger.detach();
    m_scriptDebuggerAttached = false;
    m_scriptPauseRequested = false;
    m_playWeapons.clear();
    m_playWeaponStatusLogged = false;
    for (const auto& [id, handle] : m_playVehicleChassis) {
        (void)id;
        m_playPhysics.destroy_body(handle);
    }
    m_playVehicleChassis.clear();
    m_playVehicles.clear();
    m_playParticles.clear();
    m_playEmitters.clear();
    for (auto& [id, ragdoll] : m_playRagdolls) {
        (void)id;
        ragdoll.destroy(m_playPhysics);
    }
    m_playRagdolls.clear();
    m_playMissions.clear();
    m_playMissionIds.clear();
    m_playDialogues.clear();
    m_playDialogueIds.clear();
    for (const auto& [id, voice] : m_playVoices) {
        (void)id;
        m_playAudio.stop(voice);
    }
    m_playVoices.clear();
    for (auto& [id, runtime] : m_playDestructibles) {
        (void)id;
        runtime.destroy(m_playPhysics);
    }
    m_playDestructibles.clear();
    m_playNavAgents.clear();
    m_playNav.reset();
}

// Captures the offscreen viewport (the previous rendered frame) to a PNG file.
// Runs on the main/render thread at the top of the frame, so it first waits
// for the device to idle, copies the color image into a host-visible staging
// buffer, then encodes via Windows Imaging Component (same tech as the PNG
// decoder). Returns empty on success, or a human-readable error.
std::string EditorApplication::capture_viewport_screenshot(const std::string& path) {
    if (m_offscreen.colorImage == VK_NULL_HANDLE || m_offscreen.width == 0 || m_offscreen.height == 0)
        return "screenshot: viewport not initialized";
    const uint32_t w = m_offscreen.width, h = m_offscreen.height;
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    // Make sure no frame is in flight before we read the image back.
    vkDeviceWaitIdle(m_device);
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) return "screenshot: staging buffer allocation failed";
    {
        VkCommandBuffer cmd = begin_single_time_commands();
        transition_image_layout(cmd, m_offscreen.colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, m_offscreen.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);
        transition_image_layout(cmd, m_offscreen.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        end_single_time_commands(cmd);
    }
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, size, 0, &mapped);
    if (!mapped) {
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return "screenshot: staging buffer map failed";
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(size));
    std::memcpy(rgba.data(), mapped, static_cast<size_t>(size));
    vkUnmapMemory(m_device, stagingMemory);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    // ImGui displays the offscreen texture with UV v inverted (uv0=(0,1) →
    // uv1=(1,0)), so the readback's row 0 is the BOTTOM of what the user
    // sees. Flip rows so the saved PNG matches the displayed viewport.
    std::vector<uint8_t> flipped(static_cast<size_t>(size));
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t srcY = h - 1 - y;
        std::memcpy(flipped.data() + static_cast<size_t>(y) * w * 4,
                    rgba.data() + static_cast<size_t>(srcY) * w * 4, w * 4);
    }

    // Encode RGBA -> PNG via WIC (same tech as the PNG decoder).
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))))
            return "screenshot: WIC factory init failed";
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return "screenshot: WIC stream creation failed";
    {
        const std::wstring wide(path.begin(), path.end());
        if (FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)))
            return "screenshot: cannot open file for writing: " + path;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
        return "screenshot: PNG encoder creation failed";
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return "screenshot: PNG encoder init failed";
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return "screenshot: PNG frame creation failed";
    if (FAILED(frame->Initialize(props.Get()))) return "screenshot: PNG frame init failed";
    if (FAILED(frame->SetSize(w, h))) return "screenshot: PNG frame size failed";
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return "screenshot: PNG pixel format failed";
    if (FAILED(frame->WritePixels(h, w * 4, static_cast<UINT>(flipped.size()), flipped.data())))
        return "screenshot: PNG write failed";
    if (FAILED(frame->Commit())) return "screenshot: PNG frame commit failed";
    if (FAILED(encoder->Commit())) return "screenshot: PNG encoder commit failed";
    return std::string();
}

void EditorApplication::mark_scene_dirty() {
    m_sceneDirty = true;
    m_sceneLastChange = glfwGetTime();
}

void EditorApplication::autosave_scene(bool force) {
    if (!m_sceneDirty || !m_editorScene) return;
    // Mutations always target m_editorScene (the play world is a clone), so
    // saving it is safe in any play state — the dirty flag itself is the gate.
    // Debounce: wait ~1.5s after the last change so gizmo drags / paint
    // strokes don't write the file every frame.
    if (!force && glfwGetTime() - m_sceneLastChange < 1.5) return;
    std::string path = m_activeScenePath;
    if (path.empty()) {
        // Untitled scene: a stable autosave file in the scenes folder
        // (overwritten each time, unlike the API's timestamped fallback).
        if (m_autosavePath.empty()) {
            const auto scenesDir = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
            std::error_code ec;
            std::filesystem::create_directories(scenesDir, ec);
            m_autosavePath = (scenesDir / "autosave.scene").string();
        }
        path = m_autosavePath;
    }
    if (m_editorScene->save_to_file(path)) {
        m_sceneDirty = false;
        persist_terrain_sidecar(path);
        std::cout << "[Autosave] scene saved: " << path << std::endl;
    } else {
        std::cerr << "[Autosave] save failed: " << path << std::endl;
    }
}

void EditorApplication::handle_control_command(const std::string& cmd) {
    if (cmd == "play" && m_playMode.get_state() == PlayState::Edit) {
        m_playMode.start_play(m_editorScene.get());
        setup_play_runtime();
        std::cout << "[ControlApi] play started" << std::endl;
    } else if (cmd == "pause" && m_playMode.get_state() == PlayState::Play) {
        m_playMode.pause_play();
        std::cout << "[ControlApi] paused" << std::endl;
    } else if (cmd == "resume" && m_playMode.get_state() == PlayState::Pause) {
        m_playMode.pause_play();
        std::cout << "[ControlApi] resumed" << std::endl;
    } else if (cmd == "step" && m_playMode.get_state() == PlayState::Pause) {
        m_stepRequested = true;
        std::cout << "[ControlApi] step" << std::endl;
    } else if (cmd == "stop" && m_playMode.get_state() != PlayState::Edit) {
        teardown_play_runtime();
        m_playMode.stop_play();
        m_playMode.set_editor_scene(m_editorScene.get());
        m_selectedEntity = Entity();
        m_editorGui.select_entity(m_selectedEntity);
        std::cout << "[ControlApi] stopped" << std::endl;
    } else if (cmd.rfind("zoom ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(5), f) || f.empty()) {
            m_controlResult = "zoom: expected a number";
        } else {
            const float amount = f[0];
            m_editorCamera.orbitDistance =
                glm::clamp(m_editorCamera.orbitDistance * (1.0f - amount), 0.5f, 5000.0f);
            recompute_editor_camera_position();
            std::cout << "[ControlApi] zoom " << amount << " -> " << m_editorCamera.orbitDistance << std::endl;
        }
    } else if (cmd.rfind("move ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(5), f) || f.size() < 3) {
            m_controlResult = "move: expected 3 numbers";
        } else {
            m_editorCamera.orbitTarget +=
                m_editorCamera.get_front() * f[0] +
                m_editorCamera.get_right() * f[1] +
                m_editorCamera.get_up() * f[2];
            recompute_editor_camera_position();
            std::cout << "[ControlApi] move " << f[0] << " " << f[1] << " " << f[2] << std::endl;
        }
    } else if (cmd.rfind("turn ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(5), f) || f.size() < 2) {
            m_controlResult = "turn: expected 2 numbers";
        } else {
            m_editorCamera.yaw += f[0];
            m_editorCamera.pitch = glm::clamp(m_editorCamera.pitch + f[1], -89.0f, 89.0f);
            recompute_editor_camera_position();
            std::cout << "[ControlApi] turn " << f[0] << " " << f[1] << std::endl;
        }
    } else if (cmd.rfind("focus ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(6), f) || f.size() < 3) {
            m_controlResult = "focus: expected 3 numbers (x y z)";
        } else {
            m_editorCamera.orbitTarget = glm::vec3(f[0], f[1], f[2]);
            recompute_editor_camera_position();
            std::cout << "[ControlApi] focus " << f[0] << " " << f[1] << " " << f[2] << std::endl;
        }
    } else if (cmd.rfind("terrain ", 0) == 0) {
        // Defaults match the TerrainParams / panel defaults: scale 120 gives
        // rolling hills — 1.0 turns the sheet into high-frequency spikes.
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(8), f)) {
            m_controlResult = "terrain: expected numbers (scale octaves amount falloff extent segments seed)";
        } else {
            const float scale = f.size() > 0 ? f[0] : 120.0f;
            const int octaves = f.size() > 1 ? static_cast<int>(f[1]) : 5;
            const float amount = f.size() > 2 ? f[2] : 0.5f;
            const float falloff = f.size() > 3 ? f[3] : 0.4f;
            const float halfExtent = f.size() > 4 ? f[4] : 500.0f;
            const int segments = f.size() > 5 ? static_cast<int>(f[5]) : 256;
            const uint32_t seed = f.size() > 6 ? static_cast<uint32_t>(f[6]) : 1u;
            generate_terrain_mesh(TerrainParams{ scale, octaves, amount, falloff,
                                                 halfExtent, segments, seed });
            mark_scene_dirty();
            std::cout << "[ControlApi] terrain scale=" << scale << " octaves=" << octaves
                      << " amount=" << amount << " falloff=" << falloff
                      << " extent=" << halfExtent << " segments=" << segments
                      << " seed=" << seed << std::endl;
        }
    } else if (cmd.rfind("graphics ", 0) == 0) {
        std::vector<float> f;
        if (!parse_all_floats(cmd.substr(9), f) || f.size() < 2) {
            m_controlResult = "graphics: expected 2 numbers";
        } else {
            const int vsyncInt = static_cast<int>(f[0]);
            const int quality = static_cast<int>(f[1]);
            apply_graphics_settings(vsyncInt != 0, quality);
            std::cout << "[ControlApi] graphics vsync=" << vsyncInt << " quality=" << quality << std::endl;
        }
    } else if (cmd == "save-settings") {
        save_settings();
        std::cout << "[ControlApi] save-settings" << std::endl;
    } else if (cmd.rfind("project ", 0) == 0) {
        const std::string result = create_project(cmd.substr(8), "");
        if (result.rfind("OK", 0) != 0) m_controlResult = result;
        std::cout << "[ControlApi] project -> " << result << std::endl;
    } else if (cmd.rfind("mesh ", 0) == 0) {
        int mode = 0;
        std::istringstream ss(cmd.substr(5));
        ss >> mode;
        const std::string result = apply_mesh_normals(mode);
        if (result.rfind("Normais recalculadas", 0) != 0 && result.rfind("OK", 0) != 0)
            m_controlResult = result;
        std::cout << "[ControlApi] mesh " << mode << " -> " << result << std::endl;
    } else if (cmd == "simulate" && m_playMode.get_state() == PlayState::Edit) {
        m_playMode.start_simulate(m_editorScene.get());
        setup_play_runtime();
        std::cout << "[ControlApi] simulate started" << std::endl;
    } else if (cmd == "new-scene") {
        init_default_scene();
        m_sceneDirty = false;
        std::cout << "[ControlApi] new scene" << std::endl;
    } else if (cmd.rfind("open-scene ", 0) == 0) {
        // Resolve relative paths against the source root: the editor process
        // cwd is not guaranteed to be the engine folder.
        std::string scenePath = cmd.substr(11);
        std::filesystem::path rel(scenePath);
        if (rel.is_relative()) {
            const auto abs = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / rel;
            if (std::filesystem::exists(abs)) scenePath = abs.string();
        }
        if (!std::filesystem::exists(scenePath)) {
            m_controlResult = "open-scene: file not found: " + scenePath;
            std::cout << "[ControlApi] open-scene: file not found: " << scenePath << std::endl;
            return;
        }
        load_scene_file(scenePath);
        // API-driven scene open must leave the launcher hub: the control-API
        // drain and play runtime are gated on !m_inLauncherMode.
        m_inLauncherMode = false;
        std::cout << "[ControlApi] open scene '" << scenePath << "'" << std::endl;
    } else if (cmd == "save-scene") {
        // API-safe: never open a blocking native dialog from the HTTP thread
        // path (that would wedge the main loop). If there is no active scene
        // path yet, fall back to a timestamped file in the scenes folder.
        if (!m_editorScene) {
            m_controlResult = "save-scene: no scene";
            std::cout << "[ControlApi] save-scene: no scene" << std::endl;
        } else if (!m_activeScenePath.empty()) {
            if (m_editorScene->save_to_file(m_activeScenePath)) {
                m_sceneDirty = false;
                persist_terrain_sidecar(m_activeScenePath);
                std::cout << "[ControlApi] scene saved: " << m_activeScenePath << std::endl;
            } else {
                m_controlResult = "save-scene failed: " + m_activeScenePath;
                std::cerr << "[ControlApi] save-scene failed: " << m_activeScenePath << std::endl;
            }
        } else {
            const auto scenesDir = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
            std::error_code ec;
            std::filesystem::create_directories(scenesDir, ec);
            const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
            const std::filesystem::path fallback = scenesDir / ("api_" + stamp + ".scene");
            if (m_editorScene->save_to_file(fallback.string())) {
                m_activeScenePath = fallback.string();
                m_sceneDirty = false;
                persist_terrain_sidecar(m_activeScenePath);
                std::cout << "[ControlApi] scene saved (new): " << m_activeScenePath << std::endl;
            } else {
                m_controlResult = "save-scene failed: " + fallback.string();
                std::cerr << "[ControlApi] save-scene failed: " << fallback << std::endl;
            }
        }
    } else if (cmd.rfind("add-entity ", 0) == 0) {
        if (!m_editorScene) { std::cout << "[ControlApi] no scene" << std::endl; return; }
        const std::string type = cmd.substr(11);
        const auto create = [&](const char* name) {
            Entity e = m_editorScene->create_entity(name);
            m_selectedEntity = e;
            m_editorGui.select_entity(e);
            mark_scene_dirty();
            return e;
        };
        Entity e;
        if (type == "empty") e = create("Novo Objeto");
        else if (type == "cube") { e = create("Cubo 3D"); if (e.is_valid()) m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{}; }
        else if (type == "camera") { e = create("Câmera"); if (e.is_valid()) m_editorScene->cameraComponents[e.get_id()] = CameraComponent{}; }
        else if (type == "sun") { e = create("Luz do Sol"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{}; }
        else if (type == "point") { e = create("Luz de Lâmpada"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true }; }
        else if (type == "spot") { e = create("Luz Spot"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot }; }
        else if (type == "area") { e = create("Luz de Área"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area }; }
        else if (type == "particles") { e = create("Emissor de Partículas"); if (e.is_valid()) m_editorScene->particleEmitterComponents[e.get_id()] = ParticleEmitterComponent{}; }
        else if (type == "audio") { e = create("Fonte de Áudio"); if (e.is_valid()) m_editorScene->audioComponents[e.get_id()] = AudioComponent{}; }
        else if (type == "rigidbody") { e = create("Corpo Rígido"); if (e.is_valid()) m_editorScene->rigidbodyComponents[e.get_id()] = RigidbodyComponent{}; }
        else if (type == "vehicle") { e = create("Veículo"); if (e.is_valid()) m_editorScene->vehicleComponents[e.get_id()] = VehicleComponent{}; }
        else if (type == "destructible") { e = create("Destrutível"); if (e.is_valid()) m_editorScene->destructionComponents[e.get_id()] = DestructionComponent{}; }
        else if (type == "navagent") { e = create("Agente de Navegação"); if (e.is_valid()) m_editorScene->navigationComponents[e.get_id()] = NavigationComponent{}; }
        else if (type == "mission") { e = create("Missão"); if (e.is_valid()) m_editorScene->missionComponents[e.get_id()] = MissionComponent{}; }
        else if (type == "dialogue") { e = create("Diálogo"); if (e.is_valid()) m_editorScene->dialogueComponents[e.get_id()] = DialogueComponent{}; }
#if VC_ENABLE_VOXEL_PLUGIN
        else if (type == "voxelworld") { e = create("Mundo de Blocos"); if (e.is_valid()) m_editorScene->voxelVolumeComponents[e.get_id()] = VoxelVolumeComponent{}; }
#endif
        std::cout << "[ControlApi] add-entity '" << type << "' -> "
                  << (e.is_valid() ? e.get_id().to_string() : "unknown type") << std::endl;
    } else if (cmd.rfind("add-component ", 0) == 0) {
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, type;
        ss >> uuidStr >> type;
        const UUID id = UUID::from_string(uuidStr);
        if (!m_editorScene || !m_editorScene->get_entities().contains(id)) {
            m_controlResult = "add-component: entity not found";
            std::cout << "[ControlApi] add-component: entity not found" << std::endl;
            return;
        }
        Scene* scene = m_editorScene.get();
        if (type == "light") scene->lightComponents[id] = LightComponent{};
        else if (type == "camera") scene->cameraComponents[id] = CameraComponent{};
        else if (type == "mesh") scene->meshRendererComponents[id] = MeshRendererComponent{};
        else if (type == "material") scene->materialComponents[id] = MaterialComponent{};
        else if (type == "rigidbody") scene->rigidbodyComponents[id] = RigidbodyComponent{};
        else if (type == "weapon") scene->weaponComponents[id] = WeaponComponent{};
        else if (type == "vehicle") scene->vehicleComponents[id] = VehicleComponent{};
        else if (type == "ragdoll") scene->ragdollComponents[id] = RagdollComponent{};
        else if (type == "destructible") scene->destructionComponents[id] = DestructionComponent{};
        else if (type == "navigation") scene->navigationComponents[id] = NavigationComponent{};
        else if (type == "particle") scene->particleEmitterComponents[id] = ParticleEmitterComponent{};
        else if (type == "audio") scene->audioComponents[id] = AudioComponent{};
        else if (type == "mission") scene->missionComponents[id] = MissionComponent{};
        else if (type == "dialogue") scene->dialogueComponents[id] = DialogueComponent{};
        else if (type == "animation") scene->animationComponents[id] = AnimationComponent{};
        else if (type == "timeline") scene->timelineComponents[id] = TimelineComponent{};
        else if (type == "ik") scene->ikComponents[id] = IKComponent{};
        else if (type == "retarget") scene->retargetComponents[id] = RetargetComponent{};
#if VC_ENABLE_VOXEL_PLUGIN
        else if (type == "voxel") scene->voxelVolumeComponents[id] = VoxelVolumeComponent{};
#endif
        else { m_controlResult = "add-component: unknown type '" + type + "'"; std::cout << "[ControlApi] add-component: unknown type '" << type << "'" << std::endl; return; }
        mark_scene_dirty();
        std::cout << "[ControlApi] add-component " << type << " on " << uuidStr << std::endl;
    } else if (cmd.rfind("delete-entity ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(14));
        if (m_editorScene && m_editorScene->get_entities().contains(id)) {
            m_editorScene->destroy_entity(id);
            if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) m_selectedEntity = Entity();
            mark_scene_dirty();
            std::cout << "[ControlApi] deleted entity" << std::endl;
        } else {
            m_controlResult = "delete-entity: not found";
            std::cout << "[ControlApi] delete-entity: not found" << std::endl;
        }
    } else if (cmd.rfind("rename-entity ", 0) == 0) {
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const UUID id = UUID::from_string(uuidStr);
        if (m_editorScene && m_editorScene->get_entities().contains(id) && !name.empty()) {
            m_editorScene->rename_entity(id, name);
            mark_scene_dirty();
            std::cout << "[ControlApi] renamed to '" << name << "'" << std::endl;
        } else {
            m_controlResult = "rename-entity: not found or empty name";
            std::cout << "[ControlApi] rename-entity: not found or empty name" << std::endl;
        }
    } else if (cmd.rfind("select ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(7));
        if (m_editorScene && m_editorScene->get_entities().contains(id)) {
            m_selectedEntity = Entity();
            m_selectedEntity = m_editorScene->get_entities().at(id);
            m_editorGui.select_entity(m_selectedEntity);
            std::cout << "[ControlApi] selected " << id.to_string() << std::endl;
        } else {
            m_controlResult = "select: entity not found";
            std::cout << "[ControlApi] select: entity not found" << std::endl;
        }
    } else if (cmd.rfind("select-name ", 0) == 0) {
        if (!m_editorScene) return;
        const std::string name = cmd.substr(12);
        for (const auto& [id, entity] : m_editorScene->get_entities()) {
            if (entity.get_name() == name || entity.get_name().find(name) != std::string::npos) {
                m_selectedEntity = Entity();
                m_selectedEntity = m_editorScene->get_entities().at(id);
                m_editorGui.select_entity(m_selectedEntity);
                std::cout << "[ControlApi] selected '" << entity.get_name() << "'" << std::endl;
                return;
            }
        }
        m_controlResult = "select-name: no match";
        std::cout << "[ControlApi] select-name: no match" << std::endl;
    } else if (cmd.rfind("set-transform ", 0) == 0) {
        // Field-masked PATCH, not positional: <uuid> <mask> p0 p1 p2 r0 r1 r2 s0 s1 s2
        // mask = 3 chars ('1'/'0') for position/rotation/scale, so the agent can
        // change ONLY scale (mask "001") without teleporting the object to the
        // origin or having scale floats reinterpreted as rotation.
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, maskStr;
        ss >> uuidStr >> maskStr;
        const UUID id = UUID::from_string(uuidStr);
        std::vector<float> values;
        float v;
        while (ss >> v) values.push_back(v);
        auto it = m_editorScene ? m_editorScene->transformComponents.find(id) : m_editorScene->transformComponents.end();
        if (it == m_editorScene->transformComponents.end()) {
            m_controlResult = "set-transform: entity not found";
            std::cout << "[ControlApi] set-transform: entity not found" << std::endl;
        } else if (values.size() < 9) {
            m_controlResult = "set-transform: expected <uuid> <mask> + 9 floats";
            std::cout << "[ControlApi] set-transform: expected 9 floats" << std::endl;
        } else {
            if (maskStr.size() > 0 && maskStr[0] == '1')
                it->second.position = glm::vec3(values[0], values[1], values[2]);
            if (maskStr.size() > 1 && maskStr[1] == '1')
                it->second.rotation = glm::vec3(values[3], values[4], values[5]);
            if (maskStr.size() > 2 && maskStr[2] == '1')
                it->second.scale = glm::vec3(values[6], values[7], values[8]);
            mark_scene_dirty();
            std::cout << "[ControlApi] transform set (mask " << maskStr << ")" << std::endl;
        }
    } else if (cmd.rfind("gizmo ", 0) == 0) {
        const std::string mode = cmd.substr(6);
        if (mode == "select") m_gizmoMode = GizmoMode::Select;
        else if (mode == "move") m_gizmoMode = GizmoMode::Translate;
        else if (mode == "rotate") m_gizmoMode = GizmoMode::Rotate;
        else if (mode == "scale") m_gizmoMode = GizmoMode::Scale;
        std::cout << "[ControlApi] gizmo " << mode << std::endl;
    } else if (cmd.rfind("gizmo-space ", 0) == 0) {
        m_gizmoLocal = cmd.substr(12) == "local";
        std::cout << "[ControlApi] gizmo-space " << (m_gizmoLocal ? "local" : "world") << std::endl;
    } else if (cmd.rfind("snap ", 0) == 0) {
        m_snapTranslate = std::max(0.0f, std::stof(cmd.substr(5)));
        std::cout << "[ControlApi] snap " << m_snapTranslate << std::endl;
    } else if (cmd.rfind("import ", 0) == 0) {
        if (!m_assetPipeline) {
            m_controlResult = "import: asset pipeline not ready";
        } else {
            const std::filesystem::path cookedRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
            const ImportResult result = m_assetPipeline->import({ cmd.substr(7), cookedRoot, 1 });
            if (!result) m_controlResult = "import: " + result.error;
            std::cout << "[ControlApi] import -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("import-pack ", 0) == 0) {
        const size_t count = import_texture_pack(std::filesystem::path(cmd.substr(12)));
        if (count == 0) m_controlResult = "import-pack: no assets imported from the given path";
        std::cout << "[ControlApi] import-pack -> " << count << " assets imported" << std::endl;
    } else if (cmd.rfind("block-model ", 0) == 0) {
        const UUID texId = UUID::from_string(cmd.substr(12));
        const auto meta = m_assetRegistry.find(texId);
        if (meta && meta->type == AssetType::Texture) {
            create_block_asset(*meta);
            std::cout << "[ControlApi] block model created" << std::endl;
        } else {
            m_controlResult = "block-model: texture not found";
            std::cout << "[ControlApi] block-model: texture not found" << std::endl;
        }
    } else if (cmd.rfind("block-faces ", 0) == 0) {
        // block-faces {blockId} {top} {side} {bottom} — "0" keeps the current
        // face. Rewrites the .vblock sidecar and invalidates the atlas so the
        // block renders with per-face textures (grass top / grass side / dirt).
        std::istringstream ss(cmd.substr(12));
        std::string blockStr, topStr, sideStr, bottomStr;
        ss >> blockStr >> topStr >> sideStr >> bottomStr;
        const auto face = [](const std::string& s) {
            return (s.empty() || s == "0") ? UUID{ 0, 0 } : UUID::from_string(s);
        };
        const UUID blockId = UUID::from_string(blockStr);
        if (set_block_faces(blockId, face(topStr), face(sideStr), face(bottomStr))) {
            std::cout << "[ControlApi] block-faces updated " << blockId.to_string() << std::endl;
        } else {
            m_controlResult = "block-faces: block not found or face UUID is not a registered texture";
        }
    } else if (cmd.rfind("block-model-faces ", 0) == 0) {
        // block-model-faces {base} {top} {side} {bottom} {name} — "0" = no face.
        // Creates a NEW block asset with per-face textures from texture UUIDs.
        std::istringstream ss(cmd.substr(18));
        std::string baseStr, topStr, sideStr, bottomStr, name;
        ss >> baseStr >> topStr >> sideStr >> bottomStr >> name;
        const auto face = [](const std::string& s) {
            return (s.empty() || s == "0") ? UUID{ 0, 0 } : UUID::from_string(s);
        };
        const UUID newId = create_block_from_faces(face(baseStr), face(topStr),
                                                   face(sideStr), face(bottomStr), name);
        if (!newId.is_valid()) {
            m_controlResult = "block-model-faces: at least one face must be a registered texture";
        } else {
            std::cout << "[ControlApi] block-model-faces created " << newId.to_string() << std::endl;
        }
    } else if (cmd.rfind("spawn-block ", 0) == 0) {
        const UUID blockId = UUID::from_string(cmd.substr(12));
        const auto blockMeta = m_assetRegistry.find(blockId);
        if (!blockMeta || blockMeta->type != AssetType::Block) {
            m_controlResult = "spawn-block: block asset not found";
            std::cout << "[ControlApi] spawn-block: block asset not found" << std::endl;
        } else {
            spawn_block_entity(blockId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[ControlApi] spawn-block " << blockId.to_string() << std::endl;
        }
    } else if (cmd.rfind("spawn-character ", 0) == 0) {
        const UUID texId = UUID::from_string(cmd.substr(16));
        const auto meta = m_assetRegistry.find(texId);
        if (meta && meta->type == AssetType::Texture && is_character_texture(*meta)) {
            spawn_character_entity(texId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[ControlApi] spawn-character " << texId.to_string() << std::endl;
        } else {
            m_controlResult = "spawn-character: skin texture not found";
            std::cout << "[ControlApi] spawn-character: skin texture not found" << std::endl;
        }
    } else if (cmd.rfind("layer ", 0) == 0) {
        // layer {uuid} {name} — sets the entity's layer name.
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(6));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && (name.front() == ' ')) name.erase(name.begin());
        if (scene && !uuidStr.empty() && !name.empty()) {
            const UUID id = UUID::from_string(uuidStr);
            scene->layerComponents[id].name = name;
            mark_scene_dirty();
            std::cout << "[ControlApi] layer " << uuidStr << " -> '" << name << "'" << std::endl;
        }
    } else if (cmd.rfind("layer-vis ", 0) == 0) {
        // layer-vis {name} {0|1} — show/hide every entity on that layer.
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(10));
        std::string name;
        int visible = 1;
        std::getline(ss, name, '|');
        ss >> visible;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (scene && !name.empty()) {
            for (auto& [id, lc] : scene->layerComponents) {
                if (lc.name == name) lc.visible = visible != 0;
            }
            mark_scene_dirty();
            std::cout << "[ControlApi] layer-vis '" << name << "' visible=" << visible << std::endl;
        }
    } else if (cmd.rfind("decal-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(10));
        std::string uuidStr, texture;
        ss >> uuidStr;
        std::getline(ss, texture);
        while (!texture.empty() && texture.front() == ' ') texture.erase(texture.begin());
        if (scene && !uuidStr.empty()) {
            DecalComponent dec;
            dec.texturePath = texture;
            scene->decalComponents[UUID::from_string(uuidStr)] = dec;
            mark_scene_dirty();
            std::cout << "[ControlApi] decal-add " << uuidStr << " texture='" << texture << "'" << std::endl;
        }
    } else if (cmd.rfind("hair-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(9));
        if (scene && id.is_valid()) {
            scene->hairParticleComponents[id] = HairParticleComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] hair-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("softbody-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(13));
        if (scene && id.is_valid()) {
            scene->softBodyComponents[id] = SoftBodyComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] softbody-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("env-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(8));
        if (scene && id.is_valid()) {
            scene->envProbeComponents[id] = EnvProbeComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] env-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("env-capture ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(12));
        if (scene && scene->envProbeComponents.contains(id)) {
            scene->envProbeComponents[id].captureRequested = true;
            mark_scene_dirty();
            std::cout << "[ControlApi] env-capture " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("paint-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(10));
        if (scene && id.is_valid()) {
            scene->paintComponents[id] = PaintComponent{};
            mark_scene_dirty();
            std::cout << "[ControlApi] paint-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("paint-mode ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(11));
        std::string uuidStr;
        int mode = 0;
        ss >> uuidStr >> mode;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->paintComponents.contains(id)) {
            scene->paintComponents[id].paintMode = mode != 0;
            std::cout << "[ControlApi] paint-mode " << uuidStr << " " << mode << std::endl;
        }
    } else if (cmd.rfind("paint-color ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr;
        float r = 1.0f, g = 0.3f, b = 0.22f;
        ss >> uuidStr >> r >> g >> b;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->paintComponents.contains(id)) {
            scene->paintComponents[id].brushColor = { r, g, b };
            std::cout << "[ControlApi] paint-color " << uuidStr << " " << r << " " << g << " " << b << std::endl;
        }
    } else if (cmd.rfind("video-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(10));
        if (scene && id.is_valid()) {
            scene->videoComponents[id] = VideoComponent{};
            std::cout << "[ControlApi] video-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("video-frame ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->videoComponents.contains(id) && !name.empty()) {
            scene->videoComponents[id].framePaths.push_back(name);
            std::cout << "[ControlApi] video-frame " << uuidStr << " '" << name << "'" << std::endl;
        }
    } else if (cmd.rfind("video-play ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(11));
        std::string uuidStr;
        int mode = 1;
        ss >> uuidStr >> mode;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->videoComponents.contains(id)) {
            scene->videoComponents[id].playing = mode != 0;
            std::cout << "[ControlApi] video-play " << uuidStr << " " << mode << std::endl;
        }
    } else if (cmd.rfind("gaussian-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(13));
        if (scene && id.is_valid()) {
            scene->gaussianSplatComponents[id] = GaussianSplatComponent{};
            std::cout << "[ControlApi] gaussian-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("gaussian-regen ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(15));
        if (scene && scene->gaussianSplatComponents.contains(id)) {
            scene->gaussianSplatComponents[id].regenerate = true;
            std::cout << "[ControlApi] gaussian-regen " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("expression-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(15));
        std::string uuidStr, headStr;
        ss >> uuidStr >> headStr;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && id.is_valid()) {
            ExpressionComponent ex;
            ex.headEntity = UUID::from_string(headStr);
            scene->expressionComponents[id] = ex;
            std::cout << "[ControlApi] expression-add " << uuidStr << " head=" << headStr << std::endl;
        }
    } else if (cmd.rfind("asset-duplicate ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(16));
        const auto meta = m_assetRegistry.find(id);
        if (!meta) {
            m_controlResult = "asset-duplicate: asset not found";
        } else {
            const std::filesystem::path dup = meta->sourcePath.parent_path() /
                (meta->sourcePath.stem().string() + "_copy" + meta->sourcePath.extension().string());
            AssetBrowserModel browser{ m_assetRegistry };
            const auto result = browser.duplicate_asset(id, dup);
            if (!result) m_controlResult = "asset-duplicate: " + result.error;
            m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
            std::cout << "[ControlApi] asset-duplicate -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("asset-delete ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(13));
        AssetBrowserModel browser{ m_assetRegistry };
        const auto result = browser.delete_asset(id);
        if (!result) m_controlResult = "asset-delete: " + result.error;
        m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
        std::cout << "[ControlApi] asset-delete -> " << (result ? "ok" : result.error) << std::endl;
    } else if (cmd.rfind("reimport ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(9));
        const auto meta = m_assetRegistry.find(id);
        if (meta && m_assetPipeline) {
            const std::filesystem::path cookedRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
            const ImportResult result = m_assetPipeline->import({
                .source = meta->sourcePath, .cookedDirectory = cookedRoot,
                .importerVersion = meta->importerVersion, .settings = meta->importSettings });
            m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
            std::cout << "[ControlApi] reimport -> " << (result ? "ok" : result.error) << std::endl;
        } else {
            m_controlResult = "reimport: asset not found";
        }
    } else if (cmd.rfind("screenshot", 0) == 0) {
        // Save the current viewport to a PNG so an agent can SEE the result.
        // The path is absolute or relative to the engine root. Returns the
        // saved path on success (through the Control-API result).
        std::string path = (cmd.size() > 10) ? cmd.substr(10) : std::string();
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        if (path.empty()) {
            const auto shots = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "screenshots";
            std::error_code ec;
            std::filesystem::create_directories(shots, ec);
            const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
            path = (shots / ("viewport_" + stamp + ".png")).string();
        } else {
            std::filesystem::path p(path);
            if (p.is_relative()) p = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / p;
            path = p.string();
        }
        const std::string err = capture_viewport_screenshot(path);
        if (!err.empty()) {
            m_controlResult = err;
            std::cout << "[ControlApi] screenshot FAILED: " << err << std::endl;
        } else {
            m_controlData = path;
            std::cout << "[ControlApi] screenshot saved: " << path << std::endl;
        }
    } else if (cmd.rfind("voxel-generate ", 0) == 0) {
        std::istringstream ss(cmd.substr(15));
        std::string uuidStr; uint32_t seed = 1337; float seaLevel = 24.0f;
        ss >> uuidStr; if (ss >> seed) {} if (ss >> seaLevel) {}
        const UUID id = UUID::from_string(uuidStr);
        m_voxelStructures.erase(id);
        ensure_voxel_volume(id, seed, seaLevel);
        m_voxelMeshesDirty.insert(id);
        mark_scene_dirty();
        std::cout << "[ControlApi] voxel-generate " << uuidStr << " seed=" << seed << std::endl;
    } else if (cmd.rfind("voxel-clear ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(12));
        const auto gridIt = m_voxelStructures.find(id);
        if (gridIt != m_voxelStructures.end()) {
            const auto& size = gridIt->second->size();
            for (int x = 0; x < size.x; ++x)
                for (int y = 0; y < size.y; ++y)
                    for (int z = 0; z < size.z; ++z)
                        gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
            m_voxelMeshesDirty.insert(id);
            mark_scene_dirty();
            std::cout << "[ControlApi] voxel-clear " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("voxel-paint ", 0) == 0) {
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr; int x = 0, y = 0, z = 0, type = 1, mode = 0;
        ss >> uuidStr >> x >> y >> z >> type >> mode;
        const UUID id = UUID::from_string(uuidStr);
        const auto gridIt = m_voxelStructures.find(id);
        if (gridIt == m_voxelStructures.end()) {
            std::cout << "[ControlApi] voxel-paint: volume not generated yet" << std::endl;
        } else {
            if (mode == 1) gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
            else gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ static_cast<uint16_t>(type), 0, 255 });
            m_voxelMeshesDirty.insert(id);
            mark_scene_dirty();
            std::cout << "[ControlApi] voxel-paint " << uuidStr << " (" << x << "," << y << "," << z << ") type=" << type << " mode=" << mode << std::endl;
        }
    } else if (cmd.rfind("voxel-block ", 0) == 0) {
        // Assign a Block asset to a voxel type (1=dirt, 2=grass, 3=stone,
        // 4=water, or any painted type): `voxel-block 2 <block-uuid>`. The
        // volume then samples that block's per-face atlas [top|side|bottom]
        // instead of a flat color. Auto-resolution by texture name is the
        // fallback when no override was set for the type.
        std::istringstream ss(cmd.substr(12));
        int type = 0;
        std::string uuidStr;
        ss >> type >> uuidStr;
        const UUID blockId = UUID::from_string(uuidStr);
        const auto meta = m_assetRegistry.find(blockId);
        if (type < 1 || uuidStr.empty() || !blockId.is_valid() || !meta || meta->type != AssetType::Block) {
            m_controlResult = "voxel-block: expected <type> <block-asset-uuid>";
            std::cout << "[ControlApi] voxel-block: invalid type/block " << uuidStr << std::endl;
        } else {
            m_voxelTypeBlocks[static_cast<uint16_t>(type)] = blockId;
            for (auto& [id, mesh] : m_voxelMeshes) {
                (void)mesh;
                m_voxelMeshesDirty.insert(id);
            }
            mark_scene_dirty();
            std::cout << "[ControlApi] voxel-block type=" << type
                      << " block=" << blockId.to_string() << std::endl;
        }
    } else if (cmd.rfind("script-event ", 0) == 0) {
        const std::string ev = cmd.substr(13);
        if (m_playScript.start_event(ev)) std::cout << "[ControlApi] script-event '" << ev << "'" << std::endl;
        else std::cout << "[ControlApi] script-event: no such event handler" << std::endl;
    } else if (cmd == "script-pause") {
        m_scriptPauseRequested = true;
        std::cout << "[ControlApi] script paused" << std::endl;
    } else if (cmd == "script-continue") {
        m_scriptPauseRequested = false;
        if (m_playScript.status() == VMStatus::Paused) m_scriptDebugger.continue_run(10000, 0.0f);
        std::cout << "[ControlApi] script resumed" << std::endl;
    } else if (cmd == "script-step") {
        m_scriptPauseRequested = true;
        if (m_playScript.status() == VMStatus::Paused) m_scriptDebugger.step_into(0.0f);
        std::cout << "[ControlApi] script step" << std::endl;
    } else if (cmd.rfind("editor ", 0) == 0) {
        std::string tab = cmd.substr(7);
        if (tab == "render-graph" || tab == "render graph") tab = "Render Graph";
        else if (!tab.empty()) tab[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tab[0])));
        m_specializedEditors.open_editor(tab);
        std::cout << "[ControlApi] editor tab '" << tab << "'" << std::endl;
    } else if (cmd.rfind("window ", 0) == 0) {
        const std::string w = cmd.substr(7);
        const auto toggle = [](bool& flag) { flag = !flag; };
        if (w == "viewport") toggle(m_showViewport);
        else if (w == "scene") toggle(m_showHierarchy);
        else if (w == "inspector") toggle(m_showInspector);
        else if (w == "assets") toggle(m_showContentBrowser);
        else if (w == "console") toggle(m_showConsole);
        else if (w == "dev") toggle(m_wickedTools.showDevWindow);
        else if (w == "guide") toggle(m_wickedTools.showGuideWindow);
        else if (w == "name") toggle(m_wickedTools.showNameWindow);
        else if (w == "layers") toggle(m_wickedTools.showLayerWindow);
        else if (w == "object") toggle(m_wickedTools.showObjectWindow);
        else if (w == "light") toggle(m_wickedTools.showLightWindow);
        else if (w == "camera") toggle(m_wickedTools.showCameraWindow);
        else if (w == "material") toggle(m_wickedTools.showMaterialWindow);
        else if (w == "sound") toggle(m_wickedTools.showSoundWindow);
        else if (w == "rigidbody") toggle(m_wickedTools.showRigidBodyWindow);
        else if (w == "collider") toggle(m_wickedTools.showColliderWindow);
        else if (w == "constraint") toggle(m_wickedTools.showConstraintWindow);
        else if (w == "softbody") toggle(m_wickedTools.showSoftBodyWindow);
        else if (w == "spring") toggle(m_wickedTools.showSpringWindow);
        else if (w == "decal") toggle(m_wickedTools.showDecalWindow);
        else if (w == "emitter") toggle(m_wickedTools.showEmitterWindow);
        else if (w == "hair") toggle(m_wickedTools.showHairParticleWindow);
        else if (w == "spline") toggle(m_wickedTools.showSplineWindow);
        else if (w == "forcefield") toggle(m_wickedTools.showForceFieldWindow);
        else if (w == "envprobe") toggle(m_wickedTools.showEnvProbeWindow);
        else if (w == "weather") toggle(m_wickedTools.showWeatherWindow);
        else if (w == "animation-tools") toggle(m_wickedTools.showAnimationWindow);
        else if (w == "armature") toggle(m_wickedTools.showArmatureWindow);
        else if (w == "humanoid") toggle(m_wickedTools.showHumanoidWindow);
        else if (w == "ik-tools") toggle(m_wickedTools.showIKWindow);
        else if (w == "expression") toggle(m_wickedTools.showExpressionWindow);
        else if (w == "terrain") toggle(m_wickedTools.showTerrainWindow);
        else if (w == "paint") toggle(m_wickedTools.showPaintToolWindow);
        else if (w == "mesh") toggle(m_wickedTools.showMeshWindow);
        else if (w == "importer") toggle(m_wickedTools.showModelImporterWindow);
        else if (w == "video") toggle(m_wickedTools.showVideoWindow);
        else if (w == "gaussian") toggle(m_wickedTools.showGaussianSplatWindow);
        else if (w == "theme") toggle(m_wickedTools.showThemeEditorWindow);
        else if (w == "project-creator") toggle(m_wickedTools.showProjectCreatorWindow);
        else if (w == "general") toggle(m_wickedTools.showGeneralWindow);
        else if (w == "graphics") toggle(m_wickedTools.showGraphicsWindow);
        else if (w == "profiler") toggle(m_wickedTools.showProfilerWindow);
        else { std::cout << "[ControlApi] window: unknown '" << w << "'" << std::endl; return; }
        std::cout << "[ControlApi] window toggled '" << w << "'" << std::endl;
    } else if (cmd.rfind("theme ", 0) == 0) {
        float r = 0.1f, g = 0.11f, b = 0.14f, pr = 0.2f, pg = 0.2f, pb = 0.2f;
        std::istringstream ss(cmd.substr(6));
        ss >> r >> g >> b >> pr >> pg >> pb;
        m_wickedTools.set_theme(glm::vec3(r, g, b), glm::vec3(pr, pg, pb));
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(r, g, b, 1.0f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(pr, pg, pb, 1.0f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(pr, pg, pb, 1.0f);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(pr, pg, pb, 1.0f);
        const float lift = 0.08f;
        style.Colors[ImGuiCol_FrameBg] = ImVec4(pr + lift, pg + lift, pb + lift, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(pr + lift, pg + lift, pb + lift, 1.0f);
        std::cout << "[ControlApi] theme applied" << std::endl;
    } else if (cmd.rfind("weather ", 0) == 0) {
        float sunR = 1.0f, sunG = 0.9f, sunB = 0.7f, fogDensity = 0.0f, fogStart = 0.0f, skyExposure = 1.0f, rain = 0.0f;
        std::istringstream ss(cmd.substr(8));
        ss >> sunR >> sunG >> sunB >> fogDensity >> fogStart >> skyExposure >> rain;
        if (m_editorScene) {
            UUID weatherId{ 0, 0 };
            for (const auto& [id, entity] : m_editorScene->get_entities()) {
                (void)entity;
                if (m_editorScene->weatherComponents.contains(id)) { weatherId = id; break; }
            }
            if (!weatherId.is_valid()) {
                Entity w = m_editorScene->create_entity("Weather");
                weatherId = w.get_id();
                m_editorScene->weatherComponents[weatherId] = WeatherComponent{};
            }
            auto& w = m_editorScene->weatherComponents[weatherId];
            w.sunColor = glm::vec3(sunR, sunG, sunB);
            w.fogDensity = fogDensity; w.fogStart = fogStart; w.skyExposure = skyExposure; w.rainAmount = rain;
            std::cout << "[ControlApi] weather applied" << std::endl;
        }
    } else if (cmd.rfind("selftest ", 0) == 0) {
        // Accept both numeric indices (0-4) and the friendly names
        // (rendergraph/hdr/material/play/build) so a typo like "material"
        // never crashes the editor with a std::stoi exception.
        std::string arg = cmd.substr(9);
        int which = -1;
        try {
            which = std::stoi(arg);
        } catch (...) {
            static const char* kTestNames[] = { "rendergraph", "hdr", "material", "play", "build" };
            for (int i = 0; i < 5; ++i) {
                if (arg == kTestNames[i]) { which = i; break; }
            }
        }
        if (which < 0 || which >= 5) {
            std::cout << "[ControlApi] selftest: invalid test '" << arg << "'" << std::endl;
        } else {
            m_lastSelfTestResult = run_editor_self_test(which);
            std::cout << "[ControlApi] selftest " << arg << " -> " << m_lastSelfTestResult << std::endl;
        }
    } else if (cmd == "package") {
        const std::string result = package_assets_only();
        std::cout << "[ControlApi] package -> " << result << std::endl;
    } else if (cmd == "hot-reload") {
        if (m_assetHotReload) m_assetHotReload->watch_registered_assets();
        const auto reloaded = m_assetHotReload ? m_assetHotReload->poll() : std::vector<AssetMetadata>{};
        std::cout << "[ControlApi] hot-reload -> " << reloaded.size() << " asset(s) reimported" << std::endl;
    } else {
        m_controlResult = "unrecognized command: " + cmd;
        std::cout << "[ControlApi] ignored '" << cmd << "' (state="
                  << static_cast<int>(m_playMode.get_state()) << ")" << std::endl;
    }
}

std::string EditorApplication::run_editor_self_test(int which) {
    static const char* kTestEnv[] = {
        "VC_EDITOR_TEST_RENDERGRAPH",
        "VC_EDITOR_TEST_HDR",
        "VC_EDITOR_TEST_MATERIAL",
        "VC_EDITOR_TEST_PLAY",
        "VC_EDITOR_TEST_BUILD",
    };
    if (which < 0 || which >= 5) return "Erro: teste inválido";
#ifdef _WIN32
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return "Erro: não foi possível localizar o executável";
    }
    // The child inherits the environment at creation time; set the test flag
    // only for the duration of the spawn (the parent never re-reads it after
    // startup). CREATE_NO_WINDOW keeps the headless run out of the user's way.
    SetEnvironmentVariableA(kTestEnv[which], "1");
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::string cmdLine = std::string("\"") + exePath + "\"";
    if (!CreateProcessA(exePath, cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SetEnvironmentVariableA(kTestEnv[which], nullptr);
        return std::string("Erro: falha ao iniciar o teste (code ") +
               std::to_string(GetLastError()) + ")";
    }
    // Never wait forever: a hung headless child would wedge the editor's main
    // loop (and with it the Control API). 120s is generous for any build test.
    const DWORD waitMs = 120000;
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, waitMs);
    DWORD code = 0;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 1);
        code = 1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    SetEnvironmentVariableA(kTestEnv[which], nullptr);
    return code == 0 ? "PASS" : ("FAIL (exit " + std::to_string(code) + ")");
#else
    // Non-Windows fallback: spawn via shell and read the exit status.
    const std::string cmd =
        std::string(kTestEnv[which]) + "=1 ./VulkanEngineEditor >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    return rc == 0 ? "PASS" : ("FAIL (exit " + std::to_string(rc) + ")");
#endif
}

std::string EditorApplication::package_assets_only() {
    std::vector<UUID> roots;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) roots.push_back(asset.id);
    }
    if (roots.empty()) {
        return tr("Erro: nenhum asset cozido para empacotar (importe assets primeiro).",
                  "Error: no cooked assets to package (import assets first).");
    }
    const std::filesystem::path out =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "Package";
    const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, out);
    if (!packaged) {
        return std::string(tr("Erro: ", "Error: ")) + packaged.error;
    }
    std::cout << "[Editor] standalone package: " << packaged.assets.size()
              << " asset(s) -> " << out.string() << std::endl;
    return tr("OK: ", "OK: ") + std::to_string(packaged.assets.size()) +
           tr(" asset(s) empacotados em ", " asset(s) packaged to ") + out.string();
}

void EditorApplication::update_editor_camera(float deltaTime) {
    // Respond to the mouse over the rendered image, not to ImGui window focus:
    // focus can go stale (another panel taking it), which made the viewport
    // appear to stop answering the mouse entirely.
    if (!m_viewportImageHovered) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const glm::vec2 mouse(static_cast<float>(mx), static_cast<float>(my));
    const glm::vec2 mouseDelta = mouse - m_lastMousePos;
    m_lastMousePos = mouse;

    EditorCamera& cam = m_editorCamera;
    const glm::vec3 front = cam.get_front();
    const glm::vec3 right = cam.get_right();
    const glm::vec3 up = cam.get_up();

    // Don't let camera keys fight the user typing in Inspector/text fields.
    // NOTE: io.WantCaptureKeyboard is true whenever the mouse hovers ANY
    // window, which would kill WASD the moment the cursor is over the 3D view;
    // io.WantTextInput is true only while an actual text field is being typed.
    ImGuiIO& io = ImGui::GetIO();
    const bool keysFree = !io.WantTextInput;

    // Orbit (right drag) / pan (middle drag). The clamps and the pan scale
    // live in the IEditorCamera contract (plano agente 2 §B) — the editor
    // feeds raw mouse deltas and mirrors the resulting state back.
    const bool orbitHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool panHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    if (orbitHeld && !m_gizmoDragging) {
        m_cameraContract->orbit(mouseDelta.x * cam.sensitivity,
                                mouseDelta.y * cam.sensitivity);
        cam.yaw = m_cameraContract->state().yaw;
        cam.pitch = m_cameraContract->state().pitch;
    }
    if (panHeld) {
        m_cameraContract->pan(static_cast<float>(mouseDelta.x),
                              static_cast<float>(mouseDelta.y));
        const engine::editor::CamVec3 t = m_cameraContract->state().target;
        cam.orbitTarget = glm::vec3(t.x, t.y, t.z);
    }

    // Fly (WASD): free-fly whenever the mouse is over the viewport and the
    // keyboard is not captured by a text field — no right-button required.
    if (keysFree) {
        const float speed = cam.speed * (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.0f : 1.0f);
        glm::vec3 move(0.0f);
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) move += front;
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) move -= front;
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) move += right;
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) move -= right;
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) move += up;
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) move -= up;
        if (glm::length(move) > 0.0f) {
            m_cameraContract->fly(
                engine::editor::CamVec3(move.x, move.y, move.z), speed, deltaTime);
            const engine::editor::CamVec3 t = m_cameraContract->state().target;
            cam.orbitTarget = glm::vec3(t.x, t.y, t.z);
        }
    }

    // Scroll zoom: the wheel inside the 3D view ALWAYS dollies toward/away
    // from the orbit focus — it never scrolls any panel (the viewport is
    // NoScrollbar|NoScrollWithMouse, and the delta is consumed here). The
    // delta comes from our own GLFW callback accumulator, not io.MouseWheel,
    // which ImGui zeroes at the end of NewFrame before we can read it. When
    // the viewport is NOT hovered the accumulator is dropped so ImGui keeps
    // scrolling other panels normally.
    if (m_viewportHovered || m_viewportImageHovered) {
        if (m_scrollAccum != 0.0) {
            m_cameraContract->dolly(static_cast<float>(m_scrollAccum) * 0.1f);
            cam.orbitDistance = m_cameraContract->state().distance;
            m_scrollAccum = 0.0;
        }
    } else {
        m_scrollAccum = 0.0;
    }

    recompute_editor_camera_position();
}

void EditorApplication::recompute_editor_camera_position() {
    // Recompute the camera position from target + spherical offset — the
    // derivation now lives in the IEditorCamera contract (plano agente 2 §B):
    // position = target - dir(yaw,pitch) * distance.
    const engine::editor::CamVec3 p = m_cameraContract->position();
    m_editorCamera.position = glm::vec3(p.x, p.y, p.z);
}

void EditorApplication::process_viewport_input() {
    // Gizmo keys work on hover (mouse over the 3D image), not on ImGui window
    // focus — focus can sit on another panel and would freeze the keys.
    if (!m_viewportImageHovered) return;
    ImGuiIO& io = ImGui::GetIO();
    // Gizmo mode switching: Q / W / E / R
    if (!io.WantCaptureKeyboard) {
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS && m_gizmoMode != GizmoMode::Select) {
            m_gizmoMode = GizmoMode::Select;
        }
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS && m_gizmoMode != GizmoMode::Translate) {
            m_gizmoMode = GizmoMode::Translate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS && m_gizmoMode != GizmoMode::Rotate) {
            m_gizmoMode = GizmoMode::Rotate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_R) == GLFW_PRESS && m_gizmoMode != GizmoMode::Scale) {
            m_gizmoMode = GizmoMode::Scale;
        }
    }
}

glm::vec3 EditorApplication::unproject_to_plane(glm::vec2 mouseScreen, const glm::vec3& planePoint,
                                                const glm::vec3& planeNormal, const glm::mat4& invViewProj) const {
    const float ndcX = (mouseScreen.x - m_viewportImagePos.x) / std::max(1.0f, m_viewportImageSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouseScreen.y - m_viewportImagePos.y) / std::max(1.0f, m_viewportImageSize.y) * 2.0f;
    const glm::vec4 near4 = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 far4 = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearP = glm::vec3(near4) / near4.w;
    const glm::vec3 farP = glm::vec3(far4) / far4.w;
    const glm::vec3 dir = glm::normalize(farP - nearP);
    const float denom = glm::dot(dir, planeNormal);
    if (std::abs(denom) < 1e-6f) return planePoint;
    const float t = glm::dot(planePoint - nearP, planeNormal) / denom;
    return nearP + dir * t;
}

bool EditorApplication::gizmo_axis_hit_test(glm::vec2 mouseScreen) {
    m_hoveredAxis = GizmoAxis::None;
    if (!m_editorScene || !m_selectedEntity.is_valid()) return false;
    const auto it = m_editorScene->transformComponents.find(m_selectedEntity.get_id());
    if (it == m_editorScene->transformComponents.end()) return false;
    const glm::vec3 origin = it->second.position;
    // World/Local hit test: axes rotate with the entity in local mode.
    const glm::quat gizmoRotation = m_gizmoLocal
        ? glm::quat(glm::radians(it->second.rotation))
        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const auto axisWorld = [&](int axis) -> glm::vec3 { return gizmoRotation * kAxisDirs[axis]; };

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();
    const auto project = [&](const glm::vec3& world) -> glm::vec2 {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (std::abs(clip.w) < 1e-6f) return { -1e9f, -1e9f };
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2(m_viewportImagePos.x + (ndc.x * 0.5f + 0.5f) * m_viewportImageSize.x,
                         m_viewportImagePos.y + (-ndc.y * 0.5f + 0.5f) * m_viewportImageSize.y);
    };

    const glm::vec2 originScreen = project(origin);
    float bestDist = 1e18f;
    GizmoAxis best = GizmoAxis::None;
    const float gizmoLen = (m_gizmoMode == GizmoMode::Rotate) ? 1.45f : 1.55f;
    for (int axis = 0; axis < 3; ++axis) {
        float dist = 1e18f;
        if (m_gizmoMode == GizmoMode::Rotate) {
            // Distance to the projected ring polyline.
            for (int s = 0; s < 48; ++s) {
                const float a0 = glm::two_pi<float>() * static_cast<float>(s) / 48.0f;
                const float a1 = glm::two_pi<float>() * static_cast<float>(s + 1) / 48.0f;
                const glm::vec3 dir = axisWorld(axis);
                glm::vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                glm::vec3 v = glm::normalize(glm::cross(dir, u));
                const glm::vec3 p0 = origin + u * (std::cos(a0) * gizmoLen) + v * (std::sin(a0) * gizmoLen);
                const glm::vec3 p1 = origin + u * (std::cos(a1) * gizmoLen) + v * (std::sin(a1) * gizmoLen);
                dist = std::min(dist, m_gizmoContract->dist_point_segment(
                    engine::editor::GizVec2(mouseScreen.x, mouseScreen.y),
                    engine::editor::GizVec2(project(p0).x, project(p0).y),
                    engine::editor::GizVec2(project(p1).x, project(p1).y)));
            }
        } else {
            const glm::vec2 tipScreen = project(origin + axisWorld(axis) * gizmoLen);
            dist = m_gizmoContract->dist_point_segment(
                engine::editor::GizVec2(mouseScreen.x, mouseScreen.y),
                engine::editor::GizVec2(originScreen.x, originScreen.y),
                engine::editor::GizVec2(tipScreen.x, tipScreen.y));
        }
        if (dist < 14.0f && dist < bestDist) {
            bestDist = dist;
            best = static_cast<GizmoAxis>(axis + 1);
        }
    }
    m_hoveredAxis = best;
    return best != GizmoAxis::None;
}

void EditorApplication::start_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    if (!m_editorScene->transformComponents.contains(id)) return;
    const TransformComponent& t = m_editorScene->transformComponents.at(id);

    m_gizmoDragging = true;
    m_gizmoDragEntityStart = t.position;
    m_gizmoDragRotStart = t.rotation;
    m_gizmoDragScaleStart = t.scale;
    // World/Local: in local mode the drag axis follows the entity rotation.
    if (m_gizmoLocal) {
        m_gizmoAxisWorld = glm::quat(glm::radians(t.rotation)) * kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    } else {
        m_gizmoAxisWorld = kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    }
    m_gizmoDragPlaneNormal = glm::normalize(m_editorCamera.orbitTarget - m_editorCamera.position);
    if (glm::length(m_gizmoDragPlaneNormal) < 1e-5f) m_gizmoDragPlaneNormal = glm::vec3(0, 0, 1);

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    m_gizmoDragPlanePoint = unproject_to_plane(mouseScreen, t.position, m_gizmoDragPlaneNormal, invViewProj);

    if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = m_gizmoDragPlanePoint - t.position;
        if (glm::length(toPoint) < 1e-5f) toPoint = m_gizmoAxisWorld == glm::vec3(0, 1, 0) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 ref = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(ref, ref) < 1e-6f) ref = glm::normalize(glm::cross(m_gizmoAxisWorld, glm::vec3(0, 0, 1)));
        m_gizmoDragAngleRef = glm::normalize(ref);
    }
}

void EditorApplication::update_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid() || !m_gizmoDragging) return;
    const UUID id = m_selectedEntity.get_id();
    auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    const glm::vec3 planePoint = unproject_to_plane(mouseScreen, m_gizmoDragPlanePoint,
                                                    m_gizmoDragPlaneNormal, invViewProj);
    const bool snap = ImGui::GetIO().KeyCtrl;
    const int axisIndex = static_cast<int>(m_activeAxis) - 1;

    if (m_gizmoMode == GizmoMode::Translate) {
        // Delta along the gizmo axis + snap — math delegates to the
        // IGizmoController contract (plano agente 2 §B).
        float delta = m_gizmoContract->translate_delta(
            glm_to_giz(planePoint - m_gizmoDragPlanePoint),
            glm_to_giz(m_gizmoAxisWorld), snap ? m_snapTranslate : 0.0f);
        const glm::vec3 newPos = m_gizmoDragEntityStart + m_gizmoAxisWorld * delta;
        m_undo.execute_or_merge_property(
            "Move Entity",
            [this, id, newPos] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.position = newPos; },
            [this, id, start = m_gizmoDragEntityStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.position = start;
            });
    } else if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = planePoint - m_gizmoDragEntityStart;
        if (glm::length(toPoint) < 1e-5f) return;
        glm::vec3 v = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(v, v) < 1e-6f) return;
        v = glm::normalize(v);
        // Signed angle around the axis + snap — math delegates to the
        // IGizmoController contract (plano agente 2 §B).
        const float snapped = m_gizmoContract->rotate_delta(
            glm_to_giz(m_gizmoAxisWorld), glm_to_giz(m_gizmoDragAngleRef),
            glm_to_giz(v), snap ? m_snapRotate : 0.0f);
        glm::vec3 newRot = m_gizmoDragRotStart;
        newRot[axisIndex] += snapped;
        m_undo.execute_or_merge_property(
            "Rotate Entity",
            [this, id, newRot] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.rotation = newRot; },
            [this, id, start = m_gizmoDragRotStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.rotation = start;
            });
    } else if (m_gizmoMode == GizmoMode::Scale) {
        // Axis projection delegates to the contract; the factor semantics
        // (1+delta, snap no fator, clamp 0.02) stay in the editor.
        float delta = m_gizmoContract->scale_delta(
            glm_to_giz(planePoint - m_gizmoDragPlanePoint),
            glm_to_giz(m_gizmoAxisWorld), 0.0f);
        float factor = 1.0f + delta / 1.0f;
        if (snap) factor = std::round(factor / m_snapScale) * m_snapScale;
        factor = std::max(factor, 0.02f);
        glm::vec3 newScale = m_gizmoDragScaleStart;
        newScale[axisIndex] = m_gizmoDragScaleStart[axisIndex] * factor;
        m_undo.execute_or_merge_property(
            "Scale Entity",
            [this, id, newScale] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.scale = newScale; },
            [this, id, start = m_gizmoDragScaleStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.scale = start;
            });
    }
}

// ===========================================================================
// Cooked mesh resources (real imported geometry in the viewport)
// ===========================================================================



// ===========================================================================
// Assets, Materials, Thumbnails, Block/Character (split from EditorApplication.cpp)
// ===========================================================================
bool EditorApplication::load_mesh_resource(const UUID& assetId) {
    const auto cached = m_meshResources.find(assetId);
    if (cached != m_meshResources.end()) return cached->second.valid;
    if (m_meshLoadFailed.contains(assetId)) return false;

    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Mesh || !found->isCooked ||
        found->cookedPath.empty() || !std::filesystem::is_regular_file(found->cookedPath)) {
        m_meshLoadFailed.insert(assetId);
        return false;
    }
    std::string error;
    const GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(found->cookedPath, &error);
    if (!geometry.success || geometry.primitives.empty()) {
        std::cerr << "[Editor] Cannot load mesh " << assetId.to_string() << ": " << error << std::endl;
        m_meshLoadFailed.insert(assetId);
        return false;
    }
    const float meshScale = found->importSettings.meshScale > 0.0f ? found->importSettings.meshScale : 1.0f;

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    EditorMeshResource resource;
    for (const GltfMeshPrimitive& primitive : geometry.primitives) {
        const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
        verts.reserve(verts.size() + primitive.positions.size());
        for (size_t i = 0; i < primitive.positions.size(); ++i) {
            EditorVertex v;
            v.pos = primitive.positions[i] * meshScale;
            v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = glm::vec3(1.0f);
            v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
            verts.push_back(v);
            if (!resource.hasBounds) {
                resource.boundsMin = resource.boundsMax = v.pos;
                resource.hasBounds = true;
            } else {
                resource.boundsMin = glm::min(resource.boundsMin, v.pos);
                resource.boundsMax = glm::max(resource.boundsMax, v.pos);
            }
        }
        if (primitive.indexed) {
            const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
            for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
            resource.ranges.push_back({ firstIndex, static_cast<uint32_t>(primitive.indices.size()), 0, true });
        } else {
            resource.ranges.push_back({ 0, static_cast<uint32_t>(primitive.positions.size()), vertexOffset, false });
        }
    }
    resource.vertexCount = static_cast<uint32_t>(verts.size());
    resource.valid = true;
    resource.cpuPositions.reserve(verts.size());
    for (const EditorVertex& v : verts) resource.cpuPositions.push_back(v.pos);

    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = indices.empty() ? 0 : sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  resource.vb.buffer, resource.vb.memory);
    if (ibSize > 0) {
        create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      resource.ib.buffer, resource.ib.memory);
    }
    safe_map_and_copy(m_device, resource.vb.memory, 0, vbSize, verts.data());
    if (ibSize > 0) {
        safe_map_and_copy(m_device, resource.ib.memory, 0, ibSize, indices.data());
    }
    resource.cpuIndices = std::move(indices);

    m_meshResources[assetId] = std::move(resource);
    return true;
}

const EditorApplication::EditorMeshResource* EditorApplication::get_mesh_resource(const UUID& assetId) {
    if (!assetId.is_valid()) return nullptr;
    // Block models: a Block asset is a textured cube — build the GPU mesh on
    // demand so spawned block entities survive restarts (no asset file, the
    // cube geometry is generated and uploaded here).
    if (const auto blockFound = m_assetRegistry.find(assetId);
        blockFound && blockFound->type == AssetType::Block) {
        ensure_block_cube_resource(assetId);
        const auto it = m_meshResources.find(assetId);
        return (it != m_meshResources.end() && it->second.valid) ? &it->second : nullptr;
    }
    // Minecraft character/mob skins: the texture IS the character. The
    // humanoid mesh is generated from the skin's UV layout (64x64 or legacy
    // 64x32) and cached per texture UUID, same on-demand pattern as blocks.
    if (const auto skinFound = m_assetRegistry.find(assetId);
        skinFound && skinFound->type == AssetType::Texture && is_character_texture(*skinFound)) {
        ensure_character_mesh_resource(assetId);
        const auto it = m_meshResources.find(assetId);
        return (it != m_meshResources.end() && it->second.valid) ? &it->second : nullptr;
    }
    if (!load_mesh_resource(assetId)) return nullptr;
    const auto found = m_meshResources.find(assetId);
    return (found != m_meshResources.end() && found->second.valid) ? &found->second : nullptr;
}

void EditorApplication::ensure_block_cube_resource(const UUID& blockId) {
    const auto cached = m_meshResources.find(blockId);
    if (cached != m_meshResources.end()) {
        if (cached->second.valid) return;
        if (cached->second.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.vb.buffer, nullptr);
        if (cached->second.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.vb.memory, nullptr);
        if (cached->second.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.ib.buffer, nullptr);
        if (cached->second.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.ib.memory, nullptr);
        m_meshResources.erase(cached);
    }
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    generate_cube_geometry(verts, indices);
    // Per-face atlas UVs: the block texture is a 3-wide [top|side|bottom]
    // atlas. build_cube face order: 0=-Z,1=+Z,2=-X,3=+X (side), 4=-Y
    // (bottom), 5=+Y (top) — remap each face's u into its atlas region.
    if (verts.size() >= 24) {
        for (uint32_t f = 0; f < 6; ++f) {
            float u0 = 1.0f / 3.0f, u1 = 2.0f / 3.0f; // sides
            if (f == 5) { u0 = 0.0f; u1 = 1.0f / 3.0f; }        // +Y top
            else if (f == 4) { u0 = 2.0f / 3.0f; u1 = 1.0f; }   // -Y bottom
            for (int c = 0; c < 4; ++c) {
                EditorVertex& v = verts[f * 4 + static_cast<uint32_t>(c)];
                v.uv.x = u0 + v.uv.x * (u1 - u0);
            }
        }
    }
    EditorMeshResource cube;
    cube.vertexCount = static_cast<uint32_t>(verts.size());
    cube.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  cube.vb.buffer, cube.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  cube.ib.buffer, cube.ib.memory);
    // #19: a failed upload must NOT leave a "valid" mesh with uninitialized
    // buffers (rendering garbage). Abort and keep the resource invalid.
    if (!safe_map_and_copy(m_device, cube.vb.memory, 0, vbSize, verts.data()) ||
        !safe_map_and_copy(m_device, cube.ib.memory, 0, ibSize, indices.data())) {
        if (cube.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cube.vb.buffer, nullptr);
        if (cube.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cube.vb.memory, nullptr);
        if (cube.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cube.ib.buffer, nullptr);
        if (cube.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cube.ib.memory, nullptr);
        return;
    }
    cube.valid = true;
    m_meshResources[blockId] = std::move(cube);
}

// GPU mesh for a Minecraft-style character: the humanoid (head/body/arms/legs
// boxes UV-mapped to the standard skin layout) built from skinHeight (64 for
// 64x64/HD skins, 32 for legacy 64x32) and uploaded on demand.
void EditorApplication::ensure_character_mesh_resource(const UUID& texId) {
    const auto cached = m_meshResources.find(texId);
    if (cached != m_meshResources.end()) {
        if (cached->second.valid) return;
        if (cached->second.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.vb.buffer, nullptr);
        if (cached->second.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.vb.memory, nullptr);
        if (cached->second.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.ib.buffer, nullptr);
        if (cached->second.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.ib.memory, nullptr);
        m_meshResources.erase(cached);
    }
    float skinHeight = 64.0f;
    if (const auto meta = m_assetRegistry.find(texId); meta && meta->height > 0) {
        skinHeight = static_cast<float>(meta->height);
    }
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    build_character_geometry(skinHeight, verts, indices);
    EditorMeshResource character;
    character.vertexCount = static_cast<uint32_t>(verts.size());
    character.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  character.vb.buffer, character.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  character.ib.buffer, character.ib.memory);
    safe_map_and_copy(m_device, character.vb.memory, 0, vbSize, verts.data());
    safe_map_and_copy(m_device, character.ib.memory, 0, ibSize, indices.data());
    character.valid = true;
    m_meshResources[texId] = std::move(character);
}

// A Minecraft character/mob skin becomes a humanoid entity in the scene: the
// MeshRenderer references the texture asset directly (the renderer builds the
// humanoid mesh + skin pipeline on demand), so there is no sidecar file and no
// duplicate asset in the browser.
void EditorApplication::spawn_character_entity(const UUID& texId, const glm::vec3& position) {
    if (!m_editorScene) {
        std::cerr << "[Editor] spawn_character_entity: m_editorScene is null" << std::endl;
        return;
    }
    const auto meta = m_assetRegistry.find(texId);
    if (!meta || meta->type != AssetType::Texture) {
        std::cerr << "[Editor] spawn_character_entity: texture not found " << texId.to_string() << std::endl;
        return;
    }
    Entity e = m_editorScene->create_entity(meta->sourcePath.stem().string());
    m_editorScene->transformComponents[e.get_id()].position = position;
    m_editorScene->meshRendererComponents[e.get_id()] =
        MeshRendererComponent{ texId, UUID{ 0, 0 }, true, true };
    m_selectedEntity = e;
    m_editorGui.select_entity(e);
    mark_scene_dirty();
}

// Shared material-graph pipeline that samples one texture (block faces and
// character skins both land here). Cached per texture UUID so two blocks that
// share a texture reuse the same pipeline; rebuilt when the graph hash changes.
EditorApplication::GraphMaterialPipeline* EditorApplication::ensure_texture_pipeline(
    const UUID& texId, std::unordered_map<UUID, GraphMaterialPipeline>& cache, bool withAlpha) {
    if (!texId.is_valid()) return nullptr;
    auto it = cache.find(texId);
    Rendering::MaterialGraph graph;
    const auto texNode = graph.add_texture_sample("Texture");
    if (auto* node = graph.find_node(texNode)) node->value = texId.to_string();
    const auto baseOut = graph.add_output("BaseColor", Rendering::MaterialValueType::Vec3);
    (void)graph.connect(texNode, baseOut, 0);
    if (withAlpha) {
        // Alpha cutout: skins/decals sample the texture's alpha into Opacity
        // so fully transparent texels are discarded by the generated shader
        // (no more white/black sides from ignored alpha).
        const auto opacityOut = graph.add_output("Opacity", Rendering::MaterialValueType::Float);
        (void)graph.connect(texNode, opacityOut, 0);
    }
    const uint64_t graphHash = hash_material_graph(graph);
    // Rebuild when the sampled texture's content changed (hot reload) — the
    // graph hash alone cannot see that, so pipelines kept stale GPU copies.
    uint64_t contentHash = 0;
    if (const auto meta = m_assetRegistry.find(texId)) contentHash = meta->contentHash;
    if (it == cache.end() || !it->second.valid || it->second.graphHash != graphHash ||
        it->second.textureContentHash != contentHash) {
        if (it != cache.end()) destroy_graph_pipeline(it->second);
        GraphMaterialPipeline built;
        built.graphHash = graphHash;
        if (!build_graph_pipeline(graph, built)) {
            std::cerr << "[Editor] Texture pipeline: " << built.lastError << std::endl;
        }
        built.textureContentHash = contentHash;
        it = cache.insert_or_assign(texId, std::move(built)).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

void EditorApplication::spawn_block_entity(const UUID& blockId, const glm::vec3& position) {
    if (!m_editorScene) return;
    const auto meta = m_assetRegistry.find(blockId);
    if (!meta || meta->type != AssetType::Block) return;
    Entity e = m_editorScene->create_entity(meta->sourcePath.stem().string());
    m_editorScene->transformComponents[e.get_id()].position = position;
    // meshAssetID = the block asset: the renderer builds the textured cube on
    // demand (see get_mesh_resource / the block material branch in the mesh
    // draw loop). Persists with the scene; regenerated after restart.
    m_editorScene->meshRendererComponents[e.get_id()] =
        MeshRendererComponent{ blockId, UUID{ 0, 0 }, true, true };
    m_selectedEntity = e;
    m_editorGui.select_entity(e);
    mark_scene_dirty();
}void EditorApplication::draw_mesh_resource(VkCommandBuffer cmd, const glm::mat4& mvp, const glm::vec4& color,
                                           const EditorMeshResource& resource,
                                           const glm::mat4& model) {
    if (!resource.valid || resource.vb.buffer == VK_NULL_HANDLE) return;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &resource.vb.buffer, &offset);
    if (resource.ib.buffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(cmd, resource.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    }
    push_constants(cmd, m_scenePipelineLayout, mvp, color, model);
    for (const EditorMeshResource::DrawRange& range : resource.ranges) {
        if (range.indexed) {
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        } else {
            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
        }
    }
}

bool EditorApplication::load_material_asset(const UUID& assetId) {
    if (!assetId.is_valid()) return false;
    if (m_materialAssets.contains(assetId)) return true;
    if (m_materialLoadFailed.contains(assetId)) return false;
    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Material || found->sourcePath.empty() ||
        !std::filesystem::is_regular_file(found->sourcePath)) {
        m_materialLoadFailed.insert(assetId);
        return false;
    }
    MaterialAsset mat;
    if (!mat.load_from_file(found->sourcePath)) {
        std::cerr << "[Editor] Cannot load material asset " << assetId.to_string() << std::endl;
        m_materialLoadFailed.insert(assetId);
        return false;
    }
    m_materialAssets[assetId] = std::move(mat);
    return true;
}

namespace {
// Decode a PNG payload via Windows Imaging Component into 8-bit RGBA.
bool decode_png_rgba(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba) {
    if (png.size() < 8) return false;
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) return false;
    }
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!hGlobal) return false;
    void* dst = GlobalLock(hGlobal);
    if (!dst) { GlobalFree(hGlobal); return false; }
    std::memcpy(dst, png.data(), png.size());
    GlobalUnlock(hGlobal);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stream.ReleaseAndGetAddressOf()))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return false;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
}

// Box-downscale a single-level RGBA8 image so its longest side fits within
// maxDim (aspect preserved). When already small enough, dst is left untouched
// and the caller keeps the original (outW/outH are still set).
void downscale_rgba8(const uint8_t* src, uint32_t w, uint32_t h, uint32_t maxDim,
                     std::vector<uint8_t>& dst, uint32_t& outW, uint32_t& outH) {
    const uint32_t longest = std::max(w, h);
    if (longest <= maxDim) {
        outW = w;
        outH = h;
        return;
    }
    outW = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(w) * maxDim) / longest));
    outH = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(h) * maxDim) / longest));
    dst.assign(static_cast<size_t>(outW) * outH * 4, 0);
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t y0 = static_cast<uint32_t>((static_cast<uint64_t>(y) * h) / outH);
        const uint32_t y1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(y + 1) * h) / outH), y0 + 1);
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t x0 = static_cast<uint32_t>((static_cast<uint64_t>(x) * w) / outW);
            const uint32_t x1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(x + 1) * w) / outW), x0 + 1);
            uint64_t acc[4] = { 0, 0, 0, 0 };
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src + static_cast<size_t>(sy) * w * 4;
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint8_t* d = dst.data() + (static_cast<size_t>(y) * outW + x) * 4;
            d[0] = static_cast<uint8_t>(acc[0] / n); d[1] = static_cast<uint8_t>(acc[1] / n);
            d[2] = static_cast<uint8_t>(acc[2] / n); d[3] = static_cast<uint8_t>(acc[3] / n);
        }
    }
}

float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            int e = -14;
            uint32_t m = mant;
            while ((m & 0x400u) == 0) { m <<= 1; --e; }
            m &= 0x3FFu;
            bits = sign | (static_cast<uint32_t>(e + 127) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t float_to_half(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = bits & 0x7FFFFFu;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = mant | 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        const uint32_t rounded = (m >> shift) + 0x1FFu + ((m >> (shift + 1)) & 1u);
        return static_cast<uint16_t>(sign | (rounded >> 13));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// Same box downscale for RGBA16F (HDR) thumbnails.
void downscale_half4(const uint8_t* src, uint32_t w, uint32_t h, uint32_t maxDim,
                     std::vector<uint8_t>& dst, uint32_t& outW, uint32_t& outH) {
    const uint32_t longest = std::max(w, h);
    if (longest <= maxDim) {
        outW = w;
        outH = h;
        return;
    }
    outW = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(w) * maxDim) / longest));
    outH = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(h) * maxDim) / longest));
    dst.assign(static_cast<size_t>(outW) * outH * 8, 0);
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t y0 = static_cast<uint32_t>((static_cast<uint64_t>(y) * h) / outH);
        const uint32_t y1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(y + 1) * h) / outH), y0 + 1);
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t x0 = static_cast<uint32_t>((static_cast<uint64_t>(x) * w) / outW);
            const uint32_t x1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(x + 1) * w) / outW), x0 + 1);
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(sy) * w * 8);
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint16_t* p = row + static_cast<size_t>(sx) * 4;
                    acc[0] += half_to_float(p[0]); acc[1] += half_to_float(p[1]);
                    acc[2] += half_to_float(p[2]); acc[3] += half_to_float(p[3]);
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint16_t* d = reinterpret_cast<uint16_t*>(dst.data() + (static_cast<size_t>(y) * outW + x) * 8);
            const float inv = 1.0f / static_cast<float>(n);
            d[0] = float_to_half(acc[0] * inv); d[1] = float_to_half(acc[1] * inv);
            d[2] = float_to_half(acc[2] * inv); d[3] = float_to_half(acc[3] * inv);
        }
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Cooked-texture CPU decode (shared by the viewport material path and the
// async Content Browser thumbnails). Pure file I/O + WIC decode + box
// downscale — safe to call from a worker thread.
// ---------------------------------------------------------------------------

struct DecodedTexturePixels {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    bool srgb = false;
    bool halfFloat = false; // true => rgba holds RGBA16F half-float pairs
    std::vector<uint8_t> rgba;
};

bool decode_cooked_texture_pixels(const std::filesystem::path& cookedPath, uint32_t maxDim,
                                  DecodedTexturePixels& out, std::string& error) {
    std::ifstream in(cookedPath, std::ios::binary);
    if (!in) {
        error = "cannot open cooked texture: " + cookedPath.string();
        return false;
    }
    std::array<char, 5> magic{};
    in.read(magic.data(), magic.size());
    uint32_t version = 0, width = 0, height = 0, channels = 0;
    uint8_t bitDepth = 0; // the importer stores bitDepth as a single byte
    uint32_t mipCount = 1; // v2 = single level; v3 reads mipCount + flags
    uint8_t flags = 0;
    uint64_t payloadSize = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&width), sizeof(width));
    in.read(reinterpret_cast<char*>(&height), sizeof(height));
    in.read(reinterpret_cast<char*>(&channels), sizeof(channels));
    in.read(reinterpret_cast<char*>(&bitDepth), sizeof(bitDepth));
    if (version == 2) {
        mipCount = 1;
        flags = 0;
    } else if (version == 3) {
        in.read(reinterpret_cast<char*>(&mipCount), sizeof(mipCount));
        in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    }
    in.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
    if (!in || std::string_view(magic.data(), magic.size()) != "VCTEX" ||
        (version != 2 && version != 3) || width == 0 || height == 0 || mipCount == 0 ||
        payloadSize == 0 || payloadSize > (1ull << 30)) {
        error = "invalid or unsupported VCTEX cooked texture (magic=" +
                std::string(magic.data(), magic.size()) + " version=" + std::to_string(version) +
                " size=" + std::to_string(width) + "x" + std::to_string(height) +
                " ch=" + std::to_string(channels) + " mips=" + std::to_string(mipCount) +
                " payload=" + std::to_string(payloadSize) +
                " path=" + cookedPath.string() + ")";
        return false;
    }
    std::vector<uint8_t> payload(static_cast<size_t>(payloadSize));
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
    if (!in) {
        error = "truncated VCTEX payload";
        return false;
    }
    out.width = width;
    out.height = height;
    out.mipCount = mipCount;
    out.srgb = (flags & 1u) != 0;
    const bool isPng = payload.size() >= 8 &&
                       std::memcmp(payload.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    if (isPng) {
        if (!decode_png_rgba(payload, out.rgba)) {
            error = "PNG decode failed (WIC)";
            return false;
        }
        // PNG stays a single level (raw payload); srgb is still applied.
        // Thumbnails: box-downscale before upload so a full-res texture never
        // gets copied to VRAM just to be shown at 135x48 in the asset grid.
        out.mipCount = 1;
        if (maxDim > 0 && (out.width > maxDim || out.height > maxDim)) {
            std::vector<uint8_t> thumb;
            downscale_rgba8(out.rgba.data(), out.width, out.height, maxDim, thumb, out.width, out.height);
            out.rgba = std::move(thumb);
        }
        return true;
    }
    // TGA/HDR importers store decoded pixels in the payload. Radiance HDR
    // (bitDepth 32, channels 4) stores RGBA16F half-float pairs (w*h*8 bytes)
    // and is uploaded as an R16G16B16A16_SFLOAT image; TGA stores 8-bit
    // RGB/RGBA (w*h*3/4 bytes per level, mip chain when mipCount > 1).
    if (bitDepth == 32 && channels == 4 &&
        payload.size() == static_cast<size_t>(width) * height * 8) {
        out.halfFloat = true;
        out.mipCount = 1;
        if (maxDim > 0 && (width > maxDim || height > maxDim)) {
            uint32_t tw = width, th = height;
            downscale_half4(payload.data(), width, height, maxDim, out.rgba, tw, th);
            out.width = tw;
            out.height = th;
        } else {
            out.rgba = std::move(payload);
        }
        return true;
    }
    uint64_t expectedTotal = 0;
    for (uint32_t m = 0; m < mipCount; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        expectedTotal += static_cast<uint64_t>(mw) * mh * channels;
    }
    if (payload.size() != expectedTotal) {
        error = "unsupported cooked texture payload layout (expected " +
                std::to_string(expectedTotal) + " bytes, got " + std::to_string(payload.size()) +
                " mips=" + std::to_string(mipCount) + ")";
        return false;
    }
    out.rgba.reserve(static_cast<size_t>(expectedTotal) / channels * 4);
    size_t offset = 0;
    for (uint32_t m = 0; m < mipCount; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        const size_t levelBytes = static_cast<size_t>(mw) * mh * channels;
        const uint8_t* level = payload.data() + offset;
        if (channels == 4) {
            out.rgba.insert(out.rgba.end(), level, level + levelBytes);
        } else if (channels == 3) {
            for (size_t i = 0; i < levelBytes; i += 3) {
                out.rgba.push_back(level[i]);
                out.rgba.push_back(level[i + 1]);
                out.rgba.push_back(level[i + 2]);
                out.rgba.push_back(255);
            }
        } else {
            error = "unsupported cooked texture channel count";
            return false;
        }
        offset += levelBytes;
    }
    if (maxDim > 0 && (width > maxDim || height > maxDim)) {
        // Thumbnail: keep only level 0 (mip chain is irrelevant at 192 px) and
        // box-downscale it before the upload.
        std::vector<uint8_t> level0(out.rgba.begin(),
                                    out.rgba.begin() + static_cast<size_t>(width) * height * 4);
        std::vector<uint8_t> thumb;
        downscale_rgba8(level0.data(), width, height, maxDim, thumb, width, height);
        out.rgba = std::move(thumb);
        out.width = width;
        out.height = height;
        out.mipCount = 1;
    }
    return true;
}

void EditorApplication::destroy_graph_texture(GraphTexture& t) {
    if (m_device == VK_NULL_HANDLE) return;
    if (t.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, t.view, nullptr);
    if (t.image != VK_NULL_HANDLE) vkDestroyImage(m_device, t.image, nullptr);
    if (t.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, t.memory, nullptr);
    t = GraphTexture{};
}

// ---------------------------------------------------------------------------
// Asset previews (Content Browser)
// ---------------------------------------------------------------------------

// Lazy texture thumbnails, on demand: the grid only requests assets whose
// cards are visible; the decode runs on a worker thread and the main thread
// uploads one small image per frame (see pump_asset_thumbnail_decodes).
void EditorApplication::request_asset_thumbnail_decode(const AssetMetadata& asset) {
    if (asset.type != AssetType::Texture || asset.cookedPath.empty()) {
        m_assetThumbnailFailed.insert(asset.id);
        return;
    }
    // Content-hash invalidation: a reimported texture (hot reload) must not
    // keep showing its stale flat thumbnail.
    const auto hashIt = m_assetThumbnailHashes.find(asset.id);
    if (hashIt != m_assetThumbnailHashes.end() && hashIt->second != asset.contentHash) {
        const auto thumbIt = m_assetThumbnails.find(asset.id);
        if (thumbIt != m_assetThumbnails.end()) {
            if (thumbIt->second.imguiId != VK_NULL_HANDLE)
                ImGui_ImplVulkan_RemoveTexture(thumbIt->second.imguiId);
            if (thumbIt->second.view != VK_NULL_HANDLE)
                vkDestroyImageView(m_device, thumbIt->second.view, nullptr);
            if (thumbIt->second.image != VK_NULL_HANDLE)
                vkDestroyImage(m_device, thumbIt->second.image, nullptr);
            if (thumbIt->second.memory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, thumbIt->second.memory, nullptr);
            m_assetThumbnails.erase(thumbIt);
        }
        m_assetThumbnailFailed.erase(asset.id);
        m_assetThumbnailHashes.erase(hashIt);
    }
    if (m_assetThumbnails.contains(asset.id) || m_assetThumbnailFailed.contains(asset.id)) return;
    std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
    if (m_thumbDecodeRequested.contains(asset.id)) return;
    // Bound the queue: if the user scrolls very fast, drop the oldest pending
    // request (it is re-requested when that row scrolls back into view).
    if (m_thumbDecodeQueue.size() >= 256) m_thumbDecodeQueue.pop_front();
    m_thumbDecodeRequested.insert(asset.id);
    m_thumbDecodeQueue.push_back(asset.id);
}

// Consumes finished decodes on the main thread (one small GPU upload per
// frame — no multi-second stalls) and starts one worker decode at a time.
// The queue only ever contains visible assets, so a big folder loads the
// screenful lazily and the rest stays as placeholders until scrolled into
// view, exactly like lazy loading in a web UI.
void EditorApplication::pump_asset_thumbnail_decodes() {
    PendingThumbDecode ready;
    bool haveReady = false;
    {
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        if (m_thumbDecodeReady) {
            ready = std::move(*m_thumbDecodeReady);
            m_thumbDecodeReady.reset();
            haveReady = true;
        }
    }
    if (haveReady) {
        if (!ready.rgba.empty()) {
            GraphTexture gt;
            std::string error;
            const bool ok = ready.halfFloat
                ? upload_texture_half_pixels(ready.width, ready.height, ready.rgba, gt, error)
                : upload_texture_pixels(ready.width, ready.height, ready.rgba, 1, ready.srgb, gt, error);
            if (ok) {
                const VkDescriptorSet imguiId =
                    ImGui_ImplVulkan_AddTexture(gt.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                m_assetThumbnails[ready.assetId] =
                    AssetThumbnail{ gt.image, gt.memory, gt.view, imguiId };
            } else {
                destroy_graph_texture(gt);
                m_assetThumbnailFailed.insert(ready.assetId);
            }
        } else {
            // Corrupt/undecodable cooked file: remember so we never retry it.
            m_assetThumbnailFailed.insert(ready.assetId);
        }
        // Remember the content this preview was made from, so a reimport
        // invalidates it (success and failure alike — a fixed file retries).
        if (const auto meta = m_assetRegistry.find(ready.assetId))
            m_assetThumbnailHashes[ready.assetId] = meta->contentHash;
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        m_thumbDecodeRequested.erase(ready.assetId);
    }
    UUID next;
    {
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        if (m_thumbDecodeBusy.load() || m_thumbDecodeQueue.empty()) return;
        next = m_thumbDecodeQueue.front();
        m_thumbDecodeQueue.pop_front();
        m_thumbDecodeBusy.store(true);
    }
    const auto meta = m_assetRegistry.find(next);
    if (!meta || meta->type != AssetType::Texture || meta->cookedPath.empty()) {
        m_assetThumbnailFailed.insert(next);
        {
            std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
            m_thumbDecodeRequested.erase(next);
            m_thumbDecodeBusy.store(false);
        }
        return;
    }
    const std::filesystem::path cookedPath = meta->cookedPath;
    // Join previous worker before launching a new one (cheap: the previous
    // one already signaled m_thumbDecodeBusy=false by the time we get here).
    if (m_thumbDecodeThread.joinable()) m_thumbDecodeThread.join();
    m_thumbDecodeThread = std::thread([this, id = next, cookedPath]() {
        PendingThumbDecode pending;
        pending.assetId = id;
        DecodedTexturePixels px;
        std::string error;
        if (decode_cooked_texture_pixels(cookedPath, 192, px, error)) {
            pending.width = px.width;
            pending.height = px.height;
            pending.srgb = px.srgb;
            pending.halfFloat = px.halfFloat;
            pending.rgba = std::move(px.rgba);
        }
        {
            std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
            m_thumbDecodeReady = std::move(pending);
        }
        m_thumbDecodeBusy.store(false);
    });
}

// Single active audio preview: clicking ▶ on another asset stops the current
// voice first, like a professional content browser. The decode itself is async
// (worker thread + bounded LRU cache) so long clips never freeze the editor.
void EditorApplication::toggle_audio_preview(const AssetMetadata& asset) {
    const bool alreadyPlaying = m_audioPreviewAsset == asset.id && m_audioPreviewVoice != 0 &&
                                m_playAudio.is_active(m_audioPreviewVoice);
    if (alreadyPlaying) {
        m_playAudio.stop(m_audioPreviewVoice);
        m_audioPreviewVoice = 0;
        m_audioPreviewAsset = UUID{ 0, 0 };
        m_audioPreviewRequest = UUID{ 0, 0 };
        return;
    }
    // Clicking again while this asset is still decoding cancels the request.
    const bool pendingThis = m_audioPreviewRequest == asset.id && m_audioPreviewVoice == 0;
    if (m_audioPreviewVoice != 0) m_playAudio.stop(m_audioPreviewVoice);
    m_audioPreviewVoice = 0;
    m_audioPreviewAsset = UUID{ 0, 0 };
    if (pendingThis) {
        m_audioPreviewRequest = UUID{ 0, 0 };
        return;
    }
    m_audioPreviewRequest = UUID{ 0, 0 };
    if (asset.cookedPath.empty()) return;
    m_audioPreviewRequest = asset.id;

    const auto cached = m_audioPreviewCache.find(asset.id);
    if (cached != m_audioPreviewCache.end()) {
        start_preview_voice(asset.id);
        return;
    }
    // No cached decode: the per-frame pump (pump_audio_preview_decodes) sees
    // the request and kicks the worker thread, then plays when it finishes.
}

// ---------------------------------------------------------------------------
// Playback sink: a miniaudio pull-mode device whose data callback renders the
// play-in-editor Mixer. The callback runs on miniaudio's thread; the Mixer
// locks internally, and the main thread only touches it briefly (play/stop /
// set_listener), so contention just produces an occasional underrun, never a
// deadlock. If the device cannot open (no audio hardware / sandbox), the
// editor falls back to silent rendering as before.
// ---------------------------------------------------------------------------
namespace {

void editor_audio_data_callback(ma_device* device, void* pOutput, const void* pInput, ma_uint32 frameCount);

class EditorAudioSink final {
public:
    EditorAudioSink() = default;
    ~EditorAudioSink() { shutdown(); }

    bool init(Engine::Audio::Mixer* mixer) {
        mixer_ = mixer;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2; // matches Mixer::outputChannels_ (default 2)
        config.sampleRate = 48000;    // matches Mixer::sampleRate_ (default 48000)
        config.dataCallback = editor_audio_data_callback;
        config.pUserData = this;
        device_ = new ma_device{};
        if (ma_device_init(nullptr, &config, device_) != MA_SUCCESS) {
            delete device_;
            device_ = nullptr;
            return false;
        }
        if (ma_device_start(device_) != MA_SUCCESS) {
            ma_device_uninit(device_);
            delete device_;
            device_ = nullptr;
            return false;
        }
        return true;
    }

    void shutdown() {
        if (device_ != nullptr) {
            ma_device_uninit(device_);
            delete device_;
            device_ = nullptr;
        }
    }

    void render_output(void* output, unsigned int frameCount) {
        const std::span<const float> samples = mixer_->render(frameCount);
        std::memcpy(output, samples.data(), static_cast<std::size_t>(frameCount) * 2 * sizeof(float));
    }

private:
    Engine::Audio::Mixer* mixer_{ nullptr };
    ma_device* device_{ nullptr };
};

void editor_audio_data_callback(ma_device* device, void* pOutput, const void*, ma_uint32 frameCount) {
    static_cast<EditorAudioSink*>(device->pUserData)->render_output(pOutput, frameCount);
}

} // namespace

void EditorApplication::init_audio_output() {
    if (m_audioDevice != nullptr) return;
    auto* sink = new EditorAudioSink{};
    if (!sink->init(&m_playAudio)) {
        std::cerr << "[Audio] No playback device available; play-in-editor audio stays silent." << std::endl;
        delete sink;
        return;
    }
    m_audioDevice = sink;
    m_audioDeviceStarted = true;
}

// Join any in-flight worker threads before we tear down Vulkan resources.
// Called at the very top of cleanup() so no detached thread is accessing
// member data while we destroy GPU buffers, images, and pipelines.
void EditorApplication::join_worker_threads() {
    m_thumbDecodeBusy.store(true);   // prevent new launches
    m_audioDecodeBusy.store(true);
    if (m_thumbDecodeThread.joinable()) m_thumbDecodeThread.join();
    if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
}

void EditorApplication::shutdown_audio_output() {
    if (m_audioDevice != nullptr) {
        delete static_cast<EditorAudioSink*>(m_audioDevice);
        m_audioDevice = nullptr;
    }
    m_audioDeviceStarted = false;
}

// Picks up finished background decodes, plays the one that is still requested,
// and kicks off a decode for any outstanding request not yet cached.
void EditorApplication::pump_audio_preview_decodes() {
    PendingAudioDecode ready;
    bool haveReady = false;
    {
        std::lock_guard<std::mutex> lock(m_audioDecodeMutex);
        if (m_audioDecodeReady) {
            ready = std::move(*m_audioDecodeReady);
            m_audioDecodeReady.reset();
            haveReady = true;
        }
    }
    if (haveReady) {
        if (ready.buffer.valid()) {
            cache_audio_preview(ready.assetId, ready.buffer);
        } else {
            // Corrupt/undecodable file: remember so we never retry it.
            m_audioPreviewDecodeFailed.insert(ready.assetId);
        }
        if (ready.assetId == m_audioPreviewRequest && m_audioPreviewVoice == 0) {
            start_preview_voice(ready.assetId);
        }
    }
    if (m_audioPreviewRequest != UUID{ 0, 0 } && m_audioPreviewVoice == 0 &&
        !m_audioPreviewCache.contains(m_audioPreviewRequest) &&
        !m_audioPreviewDecodeFailed.contains(m_audioPreviewRequest) &&
        !m_audioDecodeBusy.exchange(true)) {
        const UUID id = m_audioPreviewRequest;
        const auto meta = m_assetRegistry.find(id);
        if (meta && !meta->sourcePath.empty()) {
            if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
            // Decode the SOURCE file (wav/ogg/flac/mp3), not the cooked
            // .vcaudio: the cooked file has a custom "VCAUDIO" header that
            // miniaudio does not understand, so previewing used to fail for
            // every audio asset. miniaudio sniffs the container, so files
            // whose extension does not match their content still work.
            m_audioDecodeThread = std::thread([this, id, path = meta->sourcePath]() {
                const auto decoded = Engine::Audio::OggDecoder::decode_file(path);
                PendingAudioDecode pending;
                pending.assetId = id;
                if (decoded && decoded->valid()) {
                    pending.buffer.sampleRate = decoded->sampleRate;
                    pending.buffer.channels = decoded->channels;
                    pending.buffer.samples = std::move(decoded->samples);
                }
                {
                    std::lock_guard<std::mutex> lock(m_audioDecodeMutex);
                    m_audioDecodeReady = std::move(pending);
                }
                m_audioDecodeBusy.store(false);
            });
        } else {
            m_audioDecodeBusy.store(false);
            m_audioPreviewRequest = UUID{ 0, 0 };
        }
    }
    // A failed asset must not keep a stale request alive.
    if (m_audioPreviewDecodeFailed.contains(m_audioPreviewRequest)) {
        m_audioPreviewRequest = UUID{ 0, 0 };
    }
}

void EditorApplication::start_preview_voice(const UUID& assetId) {
    const auto meta = m_assetRegistry.find(assetId);
    const auto cached = m_audioPreviewCache.find(assetId);
    if (!meta || cached == m_audioPreviewCache.end()) return;
    if (m_audioPreviewVoice != 0) m_playAudio.stop(m_audioPreviewVoice);
    m_audioPreviewVoice = 0;
    auto clip = std::make_shared<Engine::Audio::AudioClip>(meta->sourcePath.stem().string());
    Engine::Audio::AudioBuffer playable = *cached->second;
    clip->hot_swap(std::move(playable));
    Engine::Audio::VoiceDescription desc;
    desc.clip = std::move(clip);
    desc.bus = m_playAudio.master_bus();
    desc.gain = 1.0f;
    desc.looping = false;
    desc.spatial = false;
    m_audioPreviewVoice = m_playAudio.play(std::move(desc));
    m_audioPreviewAsset = assetId;
    m_audioPreviewRequest = UUID{ 0, 0 };
}

void EditorApplication::cache_audio_preview(const UUID& assetId, const Engine::Audio::AudioBuffer& buffer) {
    // Bounded LRU: ~60s of stereo @48 kHz worth of decoded previews in memory.
    constexpr std::size_t kMaxCachedFrames = 48000u * 60u * 2u;
    const auto existing = m_audioPreviewCache.find(assetId);
    if (existing != m_audioPreviewCache.end()) {
        m_audioPreviewCacheFrames -= existing->second->frame_count();
        m_audioPreviewCache.erase(existing);
        std::erase(m_audioPreviewCacheOrder, assetId);
    }
    m_audioPreviewCache[assetId] = std::make_shared<Engine::Audio::AudioBuffer>(buffer);
    m_audioPreviewCacheFrames += buffer.frame_count();
    m_audioPreviewCacheOrder.push_back(assetId);
    while (m_audioPreviewCacheFrames > kMaxCachedFrames && m_audioPreviewCacheOrder.size() > 1) {
        const UUID oldest = m_audioPreviewCacheOrder.front();
        m_audioPreviewCacheOrder.pop_front();
        const auto it = m_audioPreviewCache.find(oldest);
        if (it != m_audioPreviewCache.end()) {
            m_audioPreviewCacheFrames -= it->second->frame_count();
            m_audioPreviewCache.erase(it);
        }
    }
}

void EditorApplication::destroy_asset_thumbnails() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_audioPreviewVoice != 0)    m_playAudio.stop(m_audioPreviewVoice);
    m_audioPreviewVoice = 0;
    m_audioPreviewAsset = UUID{ 0, 0 };
    m_audioPreviewRequest = UUID{ 0, 0 };
    for (auto& [id, thumb] : m_assetThumbnails) {
        (void)id;
        if (thumb.imguiId != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(thumb.imguiId);
        if (thumb.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, thumb.view, nullptr);
        if (thumb.image != VK_NULL_HANDLE) vkDestroyImage(m_device, thumb.image, nullptr);
        if (thumb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, thumb.memory, nullptr);
    }
    m_assetThumbnails.clear();
    m_assetThumbnailHashes.clear();
    for (auto& [id, desc] : m_asset3dThumbnails) {
        (void)id;
        if (desc != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(desc);
    }
    m_asset3dThumbnails.clear();
    m_asset3dThumbnailHashes.clear();
    m_assetThumbnailFailed.clear();
    {
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        m_thumbDecodeQueue.clear();
        m_thumbDecodeRequested.clear();
        m_thumbDecodeReady.reset();
        m_thumbDecodeBusy.store(false);
    }
}

// ---------------------------------------------------------------------------
// 3D asset thumbnails (Content Browser)
// ---------------------------------------------------------------------------

// Small dedicated offscreen (thumbSize x thumbSize) that reuses the viewport
// MSAA render pass, so any viewport pipeline (scene / block) can render one
// asset into it. The result becomes an ImGui texture cached in m_asset3dThumbnails.
void EditorApplication::init_thumbnail_target() {
    if (m_device == VK_NULL_HANDLE || m_offscreen.renderPass == VK_NULL_HANDLE) return;
    if (m_thumbImage != VK_NULL_HANDLE) return;
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    // Resolve target (1x) — what ImGui shows as the thumbnail.
    create_image(m_thumbSize, m_thumbSize, colorFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_thumbImage, m_thumbMemory);
    m_thumbView = create_image_view(m_thumbImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    // Multisampled color + depth (same render pass as the viewport).
    create_image(m_thumbSize, m_thumbSize, colorFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_thumbMsaaImage, m_thumbMsaaMemory, 1, m_viewportSamples);
    m_thumbMsaaView = create_image_view(m_thumbMsaaImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    create_image(m_thumbSize, m_thumbSize, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_thumbDepthImage, m_thumbDepthMemory, 1, m_viewportSamples);
    m_thumbDepthView = create_image_view(m_thumbDepthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView attachments[3] = { m_thumbMsaaView, m_thumbDepthView, m_thumbView };
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_offscreen.renderPass;
    fbInfo.attachmentCount = 3;
    fbInfo.pAttachments = attachments;
    fbInfo.width = m_thumbSize;
    fbInfo.height = m_thumbSize;
    fbInfo.layers = 1;
    vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_thumbFramebuffer);
}

void EditorApplication::destroy_thumbnail_target() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_thumbFramebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(m_device, m_thumbFramebuffer, nullptr); m_thumbFramebuffer = VK_NULL_HANDLE; }
    if (m_thumbMsaaView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_thumbMsaaView, nullptr); m_thumbMsaaView = VK_NULL_HANDLE; }
    if (m_thumbMsaaImage != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_thumbMsaaImage, nullptr); m_thumbMsaaImage = VK_NULL_HANDLE; }
    if (m_thumbMsaaMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_thumbMsaaMemory, nullptr); m_thumbMsaaMemory = VK_NULL_HANDLE; }
    if (m_thumbView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_thumbView, nullptr); m_thumbView = VK_NULL_HANDLE; }
    if (m_thumbImage != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_thumbImage, nullptr); m_thumbImage = VK_NULL_HANDLE; }
    if (m_thumbMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_thumbMemory, nullptr); m_thumbMemory = VK_NULL_HANDLE; }
    if (m_thumbDepthView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_thumbDepthView, nullptr); m_thumbDepthView = VK_NULL_HANDLE; }
    if (m_thumbDepthImage != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_thumbDepthImage, nullptr); m_thumbDepthImage = VK_NULL_HANDLE; }
    if (m_thumbDepthMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_thumbDepthMemory, nullptr); m_thumbDepthMemory = VK_NULL_HANDLE; }
}

// Textured unit cube: the "block" pipeline used to assemble a Minecraft-style
// block model from a PNG texture (thumbnail preview + scene preview).
void EditorApplication::init_block_cube() {
    if (m_device == VK_NULL_HANDLE || m_blockPipeline != VK_NULL_HANDLE) return;

    // Unit cube with per-face UVs: 24 vertices / 36 indices.
    struct BlockVert { glm::vec3 pos; glm::vec2 uv; };
    const BlockVert verts[24] = {
        { { -0.5f, -0.5f,  0.5f }, { 0, 0 } }, { {  0.5f, -0.5f,  0.5f }, { 1, 0 } },
        { {  0.5f,  0.5f,  0.5f }, { 1, 1 } }, { { -0.5f,  0.5f,  0.5f }, { 0, 1 } }, // +Z
        { {  0.5f, -0.5f, -0.5f }, { 0, 0 } }, { { -0.5f, -0.5f, -0.5f }, { 1, 0 } },
        { { -0.5f,  0.5f, -0.5f }, { 1, 1 } }, { {  0.5f,  0.5f, -0.5f }, { 0, 1 } }, // -Z
        { {  0.5f, -0.5f,  0.5f }, { 0, 0 } }, { {  0.5f, -0.5f, -0.5f }, { 1, 0 } },
        { {  0.5f,  0.5f, -0.5f }, { 1, 1 } }, { {  0.5f,  0.5f,  0.5f }, { 0, 1 } }, // +X
        { { -0.5f, -0.5f, -0.5f }, { 0, 0 } }, { { -0.5f, -0.5f,  0.5f }, { 1, 0 } },
        { { -0.5f,  0.5f,  0.5f }, { 1, 1 } }, { { -0.5f,  0.5f, -0.5f }, { 0, 1 } }, // -X
        { { -0.5f,  0.5f,  0.5f }, { 0, 0 } }, { {  0.5f,  0.5f,  0.5f }, { 1, 0 } },
        { {  0.5f,  0.5f, -0.5f }, { 1, 1 } }, { { -0.5f,  0.5f, -0.5f }, { 0, 1 } }, // +Y
        { { -0.5f, -0.5f, -0.5f }, { 0, 0 } }, { {  0.5f, -0.5f, -0.5f }, { 1, 0 } },
        { {  0.5f, -0.5f,  0.5f }, { 1, 1 } }, { { -0.5f, -0.5f,  0.5f }, { 0, 1 } }, // -Y
    };
    const uint32_t indices[36] = {
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    };
    // Per-face atlas UVs: remap the shared thumbnail cube into the 3-wide
    // [top|side|bottom] atlas regions. Face groups: 0-3=+Z, 4-7=-Z, 8-11=+X,
    // 12-15=-X (sides), 16-19=+Y (top), 20-23=-Y (bottom).
    BlockVert cube[24];
    std::memcpy(cube, verts, sizeof(cube));
    for (int i = 0; i < 24; ++i) {
        float u0 = 1.0f / 3.0f, u1 = 2.0f / 3.0f; // sides
        if (i >= 16 && i < 20) { u0 = 0.0f; u1 = 1.0f / 3.0f; }     // +Y top
        else if (i >= 20) { u0 = 2.0f / 3.0f; u1 = 1.0f; }          // -Y bottom
        cube[i].uv.x = u0 + cube[i].uv.x * (u1 - u0);
    }
    const VkDeviceSize vbSize = sizeof(BlockVert) * 24;
    const VkDeviceSize ibSize = sizeof(uint32_t) * 36;
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_blockCubeVB.buffer, m_blockCubeVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_blockCubeIB.buffer, m_blockCubeIB.memory);
    if (!safe_map_and_copy(m_device, m_blockCubeVB.memory, 0, vbSize, cube) ||
        !safe_map_and_copy(m_device, m_blockCubeIB.memory, 0, ibSize, indices)) {
        std::cerr << "[Editor] block cube upload failed" << std::endl;
    }
    m_blockCubeIndexCount = 36;

    // Pixel-art sampler: Minecraft blocks/skins are nearest-filtered, no
    // mipmap bleeding. (PBR/HD textures keep the trilinear sampler elsewhere.)
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f; // the atlas is a single mip
    vkCreateSampler(m_device, &samplerInfo, nullptr, &m_blockSampler);

    // Same pixel-art filtering for the block atlases sampled by the
    // material-graph pipelines (voxel volumes + scene block cubes): NEAREST,
    // no mipmap bleeding — the crisp Minecraft look instead of LINEAR blur.
    VkSamplerCreateInfo blockDrawInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    blockDrawInfo.magFilter = VK_FILTER_NEAREST;
    blockDrawInfo.minFilter = VK_FILTER_NEAREST;
    blockDrawInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    blockDrawInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    blockDrawInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    blockDrawInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    blockDrawInfo.minLod = 0.0f;
    blockDrawInfo.maxLod = 0.0f; // the block atlas is a single mip
    vkCreateSampler(m_device, &blockDrawInfo, nullptr, &m_blockDrawSampler);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    descLayoutInfo.bindingCount = 1;
    descLayoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr, &m_blockDescSetLayout);

    // Block descriptors come from their OWN pool, not the ImGui pool:
    // ImGui resets its descriptor pools every frame, which invalidated these
    // sets right after allocation — draws with the reset sets corrupted the
    // whole frame (viewport went blank; occasionally the device faulted).
    // A dedicated pool keeps the sets alive for the block pipeline's lifetime.
    VkDescriptorPoolSize blockPoolSize{};
    blockPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    blockPoolSize.descriptorCount = 2048;
    VkDescriptorPoolCreateInfo blockPoolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    blockPoolInfo.maxSets = 1024;
    blockPoolInfo.poolSizeCount = 1;
    blockPoolInfo.pPoolSizes = &blockPoolSize;
    vkCreateDescriptorPool(m_device, &blockPoolInfo, nullptr, &m_blockDescPool);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 64; // mat4 mvp
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_blockDescSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_blockPipelineLayout);

    m_blockVertShader = make_module(m_device, read_spv("block.vert.spv"));
    m_blockFragShader = make_module(m_device, read_spv("block.frag.spv"));
    if (!m_blockVertShader || !m_blockFragShader) {
        std::cerr << "[Editor] block shaders missing (run compile_shaders)" << std::endl;
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_blockVertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_blockFragShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0; bindings[0].stride = sizeof(BlockVert); bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32_SFLOAT; attrs[1].offset = sizeof(glm::vec3);
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = m_viewportSamples;
    multisample.alphaToCoverageEnable = VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;
    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_blockPipelineLayout;
    pipelineInfo.renderPass = m_offscreen.renderPass;
    pipelineInfo.subpass = 0;
    vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_blockPipeline);
}

void EditorApplication::destroy_block_cube() {
    if (m_device == VK_NULL_HANDLE) return;
    for (auto& [id, gt] : m_blockTextures) {
        (void)id;
        destroy_graph_texture(gt);
    }
    m_blockTextures.clear();
    m_blockDescriptors.clear();
    m_blockTextureHashes.clear();
    if (m_blockPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_blockPipeline, nullptr); m_blockPipeline = VK_NULL_HANDLE; }
    if (m_blockPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_blockPipelineLayout, nullptr); m_blockPipelineLayout = VK_NULL_HANDLE; }
    if (m_blockDescSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_blockDescSetLayout, nullptr); m_blockDescSetLayout = VK_NULL_HANDLE; }
    if (m_blockDescPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_blockDescPool, nullptr); m_blockDescPool = VK_NULL_HANDLE; }
    if (m_blockSampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_blockSampler, nullptr); m_blockSampler = VK_NULL_HANDLE; }
    if (m_blockDrawSampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_blockDrawSampler, nullptr); m_blockDrawSampler = VK_NULL_HANDLE; }
    // Per-face atlas textures (same lifecycle as the block cube).
    for (auto& [uuid, gt] : m_blockAtlasTextures) {
        (void)uuid;
        destroy_graph_texture(gt);
    }
    m_blockAtlasTextures.clear();
    m_blockAtlasHashes.clear();
    if (m_blockVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_blockVertShader, nullptr); m_blockVertShader = VK_NULL_HANDLE; }
    if (m_blockFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_blockFragShader, nullptr); m_blockFragShader = VK_NULL_HANDLE; }
    if (m_blockCubeVB.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_blockCubeVB.buffer, nullptr); vkFreeMemory(m_device, m_blockCubeVB.memory, nullptr); m_blockCubeVB = GPUBuffer{}; }
    if (m_blockCubeIB.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_blockCubeIB.buffer, nullptr); vkFreeMemory(m_device, m_blockCubeIB.memory, nullptr); m_blockCubeIB = GPUBuffer{}; }
}

// Lazy descriptor set for a block texture (my layout, allocated from the ImGui
// descriptor pool which carries COMBINED_IMAGE_SAMPLER).
VkDescriptorSet EditorApplication::get_block_descriptor(const UUID& textureAsset) {
    // Block assets resolve to the per-face atlas, which is OWNED by
    // m_blockAtlasTextures (not m_blockTextures) — avoid double-owning/destroying.
    const bool isAtlas = [&] {
        const auto meta = m_assetRegistry.find(textureAsset);
        return meta && meta->type == AssetType::Block;
    }();
    // Content-hash invalidation: a reimported texture (hot reload) must not
    // keep its stale GPU copy (thumbnails and scene block faces share it).
    uint64_t contentHash = 0;
    if (const auto meta = m_assetRegistry.find(textureAsset)) contentHash = meta->contentHash;
    const auto hashIt = m_blockTextureHashes.find(textureAsset);
    if (hashIt != m_blockTextureHashes.end() && hashIt->second != contentHash) {
        const auto descIt = m_blockDescriptors.find(textureAsset);
        if (descIt != m_blockDescriptors.end()) {
            if (descIt->second != VK_NULL_HANDLE && m_blockDescPool != VK_NULL_HANDLE)
                vkFreeDescriptorSets(m_device, m_blockDescPool, 1, &descIt->second);
            m_blockDescriptors.erase(descIt);
        }
        if (!isAtlas) {
            const auto texIt = m_blockTextures.find(textureAsset);
            if (texIt != m_blockTextures.end()) { destroy_graph_texture(texIt->second); m_blockTextures.erase(texIt); }
        }
        m_blockTextureHashes.erase(hashIt);
    }
    const auto cached = m_blockDescriptors.find(textureAsset);
    if (cached != m_blockDescriptors.end()) return cached->second;
    if (m_blockDescSetLayout == VK_NULL_HANDLE || m_blockSampler == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    GraphTexture gt;
    std::string error;
    if (!load_viewport_texture(textureAsset, gt, error, 192)) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_blockDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_blockDescSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) {
        if (!isAtlas) destroy_graph_texture(gt); // atlas is owned by its cache
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo imageInfo{ m_blockSampler, gt.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    if (!isAtlas) m_blockTextures[textureAsset] = gt; // atlas kept alive by its cache
    m_blockDescriptors[textureAsset] = set;
    m_blockTextureHashes[textureAsset] = contentHash;
    return set;
}

void EditorApplication::request_3d_thumbnail(const UUID& assetId) {
    if (!assetId.is_valid()) return;
    if (m_assetThumbnails.contains(assetId) || m_asset3dThumbnails.contains(assetId) ||
        m_assetThumbnailFailed.contains(assetId) || m_thumbnailQueued.contains(assetId)) {
        return;
    }
    m_thumbnailQueued.insert(assetId);
    m_thumbnailQueue.push_back(assetId);
}

// Renders pending mesh/block thumbnails, a few per frame (each render is a
// submit + wait, so the budget keeps the editor responsive).
void EditorApplication::pump_asset_thumbnails(int budget) {
    if (m_thumbFramebuffer == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE) return;
    while (budget-- > 0 && !m_thumbnailQueue.empty()) {
        const UUID id = m_thumbnailQueue.front();
        m_thumbnailQueue.pop_front();
        m_thumbnailQueued.erase(id);
        if (m_assetThumbnails.contains(id) || m_asset3dThumbnails.contains(id) ||
            m_assetThumbnailFailed.contains(id)) {
            continue;
        }
        const auto meta = m_assetRegistry.find(id);
        if (!meta) { m_assetThumbnailFailed.insert(id); continue; }
        // Content-hash invalidation: a reimported asset (hot reload) must not
        // keep showing its stale 3D thumbnail.
        const auto hashIt = m_asset3dThumbnailHashes.find(id);
        if (hashIt != m_asset3dThumbnailHashes.end() && hashIt->second != meta->contentHash) {
            const auto thumbIt = m_asset3dThumbnails.find(id);
            if (thumbIt != m_asset3dThumbnails.end()) {
                if (thumbIt->second != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(thumbIt->second);
                m_asset3dThumbnails.erase(thumbIt);
            }
            m_asset3dThumbnailHashes.erase(hashIt);
        }
        if (meta->type == AssetType::Mesh) {
            if (!load_mesh_resource(id)) { m_assetThumbnailFailed.insert(id); continue; }
            const EditorMeshResource* mesh = get_mesh_resource(id);
            if (!mesh || !mesh->valid) { m_assetThumbnailFailed.insert(id); continue; }
            render_mesh_thumbnail(id, *mesh);
        } else if (meta->type == AssetType::Block) {
            const UUID tex = resolve_block_texture(id);
            if (!tex.is_valid()) { m_assetThumbnailFailed.insert(id); continue; }
            const VkDescriptorSet desc = get_block_descriptor(tex);
            if (desc == VK_NULL_HANDLE) { m_assetThumbnailFailed.insert(id); continue; }
            render_block_thumbnail(id, desc);
        } else if (meta->type == AssetType::Texture && is_block_texture(*meta)) {
            // The PNG is the block: its card shows the textured cube instead
            // of the flat image.
            const VkDescriptorSet desc = get_block_descriptor(meta->id);
            if (desc == VK_NULL_HANDLE) { m_assetThumbnailFailed.insert(id); continue; }
            render_block_thumbnail(id, desc);
        } else if (meta->type == AssetType::Texture && is_character_texture(*meta)) {
            // The PNG is the character: its card shows the humanoid mesh with
            // the skin applied (same pipeline the viewport uses), not the flat
            // skin atlas.
            const EditorMeshResource* mesh = get_mesh_resource(id);
            if (!mesh || !mesh->valid) { m_assetThumbnailFailed.insert(id); continue; }
            render_character_thumbnail(id, *mesh);
        } else {
            m_assetThumbnailFailed.insert(id);
        }
    }
}

// Renders a cooked mesh into the thumbnail offscreen with the scene pipeline
// (neutral material color), framed from its bounds, and caches an ImGui
// texture. The color image itself stays owned by the thumbnail target.
void EditorApplication::render_mesh_thumbnail(const UUID& assetId, const EditorMeshResource& mesh) {
    if (m_scenePipeline == VK_NULL_HANDLE) return;
    const glm::vec3 center = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
    const float radius = std::max(glm::length(mesh.boundsMax - mesh.boundsMin) * 0.5f, 1e-4f);
    const float camDist = radius * 2.6f;
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, camDist * 20.0f);
    const glm::mat4 view = glm::lookAt(center + glm::vec3(0.75f, 0.60f, 0.90f) * camDist, center, glm::vec3(0, 1, 0));

    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = m_offscreen.renderPass;
    rp.framebuffer = m_thumbFramebuffer;
    rp.renderArea = { { 0, 0 }, { m_thumbSize, m_thumbSize } };
    const VkClearValue clears[2] = {
        { { { 0.10f, 0.11f, 0.14f, 1.0f } } }, // surface background
        { { 1.0f, 0 } },
    };
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{ 0, 0, static_cast<float>(m_thumbSize), static_cast<float>(m_thumbSize), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{ { 0, 0 }, { m_thumbSize, m_thumbSize } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    // The thumbnail shares the viewport's MSAA render pass, so the scene
    // pipeline (same samples) renders the mesh into it.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
    draw_mesh_resource(cmd, proj * view, glm::vec4(0.62f, 0.66f, 0.75f, 1.0f), mesh);
    vkCmdEndRenderPass(cmd);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);

    m_asset3dThumbnails[assetId] =
        ImGui_ImplVulkan_AddTexture(m_thumbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (const auto meta = m_assetRegistry.find(assetId))
        m_asset3dThumbnailHashes[assetId] = meta->contentHash;
}

// Same, but a textured unit cube: the Minecraft-style block assembled from its
// PNG face texture (block pipeline).
void EditorApplication::render_block_thumbnail(const UUID& assetId, VkDescriptorSet textureDesc) {
    if (m_blockPipeline == VK_NULL_HANDLE) return;
    // Frame the full character: look at its vertical center (the model spans
    // y 0..2 with the feet at the origin) from a bit further out.
    const glm::vec3 charCenter(0.0f, 1.0f, 0.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 50.0f);
    const glm::mat4 view = glm::lookAt(charCenter + glm::vec3(2.6f, 2.2f, 3.0f), charCenter, glm::vec3(0, 1, 0));
    const glm::mat4 mvp = proj * view;

    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = m_offscreen.renderPass;
    rp.framebuffer = m_thumbFramebuffer;
    rp.renderArea = { { 0, 0 }, { m_thumbSize, m_thumbSize } };
    const VkClearValue clears[2] = {
        { { { 0.10f, 0.11f, 0.14f, 1.0f } } },
        { { 1.0f, 0 } },
    };
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{ 0, 0, static_cast<float>(m_thumbSize), static_cast<float>(m_thumbSize), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{ { 0, 0 }, { m_thumbSize, m_thumbSize } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blockPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blockPipelineLayout,
                            0, 1, &textureDesc, 0, nullptr);
    vkCmdPushConstants(cmd, m_blockPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, &mvp);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_blockCubeVB.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_blockCubeIB.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_blockCubeIndexCount, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);

    m_asset3dThumbnails[assetId] =
        ImGui_ImplVulkan_AddTexture(m_thumbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (const auto meta = m_assetRegistry.find(assetId))
        m_asset3dThumbnailHashes[assetId] = meta->contentHash;
}

// A Minecraft character/mob skin rendered as the humanoid mesh with the skin
// applied (material-graph texture pipeline, the exact path the viewport uses
// for character entities) — the card shows the 3D character instead of the
// flat skin atlas PNG.
void EditorApplication::render_character_thumbnail(const UUID& assetId, const EditorMeshResource& mesh) {
    GraphMaterialPipeline* gmp = ensure_texture_pipeline(assetId, m_skinGraphPipelines, true);
    if (!gmp) { m_assetThumbnailFailed.insert(assetId); return; }
    write_material_ubo(*gmp, nullptr, nullptr);
    write_light_ubo(*gmp, m_editorScene.get(), m_editorCamera.position);
    // Frame the humanoid: the model spans y 0..2 with the feet at the origin.
    const glm::vec3 center(0.0f, 1.0f, 0.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 100.0f);
    const glm::mat4 view = glm::lookAt(center + glm::vec3(0.85f, 0.55f, 1.10f) * 3.0f,
                                       center, glm::vec3(0, 1, 0));
    const Rendering::MaterialPushConstants pc{ proj * view, glm::mat4(1.0f) };

    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = m_offscreen.renderPass;
    rp.framebuffer = m_thumbFramebuffer;
    rp.renderArea = { { 0, 0 }, { m_thumbSize, m_thumbSize } };
    const VkClearValue clears[2] = {
        { { { 0.10f, 0.11f, 0.14f, 1.0f } } },
        { { 1.0f, 0 } },
    };
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{ 0, 0, static_cast<float>(m_thumbSize), static_cast<float>(m_thumbSize), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{ { 0, 0 }, { m_thumbSize, m_thumbSize } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                            0, 1, &gmp->descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vb.buffer, &vertexOffset);
    if (mesh.ib.buffer != VK_NULL_HANDLE)
        vkCmdBindIndexBuffer(cmd, mesh.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const EditorMeshResource::DrawRange& range : mesh.ranges) {
        if (range.indexed)
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        else
            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
    }
    vkCmdEndRenderPass(cmd);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);

    m_asset3dThumbnails[assetId] =
        ImGui_ImplVulkan_AddTexture(m_thumbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (const auto meta = m_assetRegistry.find(assetId))
        m_asset3dThumbnailHashes[assetId] = meta->contentHash;
}

// ---------------------------------------------------------------------------
// Minecraft-style block model assets
// ---------------------------------------------------------------------------

// Minecraft character/mob skins are square POT too (player 64x64, mobs
// 64x64...), so entity/mob path + filename signals classify them as MODELS.
// Resource-pack block folders ("/textures/block/", "/blocks/") always win —
// vanilla names like mob_spawner live there and ARE blocks.
bool EditorApplication::is_character_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.sourcePath.empty()) return false;
    std::string p = meta.sourcePath.generic_string();
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kSkinPathMarkers[] = {
        "/entity/", "/entities/", "/mob/", "/mobs/", "/char/", "/chars/",
        "/character/", "/characters/", "/player/", "/players/", "/actor/",
        "/actors/", "/humanoid/", "/creature/", "/creatures/", "/monster/",
        "/monsters/", "/npc/", "/npcs/", "/zombie/", "/villager/", "/village/",
        "/skin/", "/skins/",
    };
    for (const char* marker : kSkinPathMarkers) {
        if (p.find(marker) != std::string::npos) return true;
    }
    std::string stem = meta.sourcePath.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kSkinNameMarkers[] = {
        "skin", "char", "player", "npc", "actor", "humanoid", "steve",
        "alex", "villager", "zombie", "creeper", "skeleton", "enderman",
        "spider", "chicken", "wolf", "horse", "rabbit", "squid", "slime",
        "ghast", "blaze", "witch", "wither", "dragon", "guardian",
        "shulker", "phantom", "drowned", "husk", "stray", "vex",
        "pillager", "ravager", "panda", "parrot", "turtle", "dolphin",
        "llama", "salmon", "pufferfish", "hoglin", "piglin", "zoglin",
        "strider", "trader", "golem", "silverfish", "magma", "sheep",
        "cow", "pig", "bee", "fox", "bat",
    };
    // Word-boundary match: "char_01" is a skin, but "charcoal" (a block) is
    // not — the marker must sit between non-alphanumeric separators.
    const auto hasMarker = [](const std::string& s, const char* marker) {
        const size_t pos = s.find(marker);
        if (pos == std::string::npos) return false;
        if (pos > 0 && std::isalnum(static_cast<unsigned char>(s[pos - 1]))) return false;
        const size_t end = pos + std::strlen(marker);
        if (end < s.size() && std::isalnum(static_cast<unsigned char>(s[end]))) return false;
        return true;
    };
    for (const char* marker : kSkinNameMarkers) {
        if (hasMarker(stem, marker)) return true;
    }
    return false;
}

// Heuristic: small square power-of-two textures are the classic Minecraft
// block face format (16/32/64/128/256). Character/mob skins are excluded
// (they are models, not blocks); block folders always win.
// Auxiliary material maps follow the classic <base>_<suffix> naming
// (andesite_n.png = normal, _s = specular, _h = height, _e = emissive, …).
// They are material inputs for a block, never a block face themselves — the
// heuristic used to classify every square POT texture as a Block, flooding the
// browser with fake "blocks" that were really normal/specular maps.
bool EditorApplication::is_aux_map_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.sourcePath.empty()) return false;
    std::string stem = meta.sourcePath.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (stem.size() < 3) return false;
    static const char* kAuxSuffixes[] = {
        "_n", "_s", "_h", "_e", "_bump", "_bumpmap", "_normal", "_normalmap",
        "_spec", "_specular", "_specmap", "_height", "_heightmap", "_emissive",
        "_emission", "_glow", "_ao", "_ambientocclusion", "_rough", "_roughness",
        "_metal", "_metallic", "_metalness", "_disp", "_displacement", "_mask",
        "_detail", "_overlay", "_gloss", "_glossmap",
    };
    for (const char* suffix : kAuxSuffixes) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

bool EditorApplication::looks_like_block_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.width == 0 || meta.height == 0) return false;
    if (meta.width != meta.height) return false;
    const uint32_t s = meta.width;
    if (s < 8 || s > 256) return false;
    if ((s & (s - 1)) != 0) return false;
    if (is_aux_map_texture(meta)) return false;
    std::string p = meta.sourcePath.generic_string();
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (p.find("/block/") != std::string::npos || p.find("/blocks/") != std::string::npos ||
        p.find("/tile/") != std::string::npos) {
        return true;
    }
    if (is_character_texture(meta)) return false;
    // Non-block decorative/UI textures: even when square POT, particle
    // atlases, icons, GUI sprites, noise maps and similar are not block
    // faces. Only the fallback path (no /block/ folder) is filtered — a
    // texture explicitly inside a block folder always wins above.
    std::string stem = meta.sourcePath.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kNonBlockStems[] = {
        "particle", "particles", "icon", "icons", "noise", "noises",
        "gui", "font", "fontsheet", "cursor", "logo", "splash",
        "toolbar", "badge", "badges", "emoji", "emojis", "widget",
        "widgets", "button", "buttons", "panel", "panels", "frame",
        "frames", "loading", "menu", "menus", "title", "options",
        "settings", "achievement", "achievements", "recipe", "recipes",
        "inventory", "hotbar", "crosshair", "hud", "map", "maps",
        "book", "books", "painting", "paintings", "slot", "slots",
        "container", "containers", "banner", "banners", "arrow",
        "arrows", "experience", "xp", "effect", "effects",
    };
    for (const char* marker : kNonBlockStems) {
        if (stem == marker) return false;
    }
    static const char* kNonBlockSuffixes[] = {
        "_icon", "_icons", "_particle", "_particles", "_noise", "_sprite",
        "_sprites", "_gui", "_ui", "_hud",
    };
    for (const char* suffix : kNonBlockSuffixes) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) return false;
    }
    static const char* kNonBlockPrefixes[] = {
        "gui_", "icon_", "particle_", "noise_", "ui_", "hud_", "menu_", "title_",
    };
    for (const char* prefix : kNonBlockPrefixes) {
        const size_t n = std::strlen(prefix);
        if (stem.size() > n && stem.compare(0, n, prefix) == 0) return false;
    }
    return true;
}

// The PNG is the block: a texture counts as a block when it looks like one
// (square POT 8-256, excluding character/mob skins) or when an existing
// .vblock sidecar references it (the explicit user mark). The registry scan
// is cached per texture UUID — it runs once, and sidecars are permanent once
// created.
bool EditorApplication::is_block_texture(const AssetMetadata& meta) {
    if (meta.type != AssetType::Texture || !meta.id.is_valid()) return false;
    // Explicit "not a block" (user override) wins over everything. The marker
    // file is stat()'d once per texture UUID (cached), so restarts honor it.
    if (!m_noblockChecked.contains(meta.id)) {
        m_noblockChecked.insert(meta.id);
        if (!meta.sourcePath.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(meta.sourcePath.string() + ".noblock", ec)) {
                m_noblockTextures.insert(meta.id);
            }
        }
    }
    if (m_noblockTextures.contains(meta.id)) return false;
    if (is_aux_map_texture(meta)) {
        // Aux maps are never blocks. Heal registries created before this rule
        // existed (their .vblock sidecar made andesite_n.png a "Block").
        if (!m_auxBlockHealed.contains(meta.id)) {
            m_auxBlockHealed.insert(meta.id);
            heal_aux_block_sidecars(meta);
        }
        return false;
    }
    if (looks_like_block_texture(meta)) return true;
    if (m_blockSidecarChecked.contains(meta.id)) return m_blockTextureSet.contains(meta.id);
    bool found = false;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (load_block_asset(candidate.id, data) &&
            (data.texture == meta.id || data.top == meta.id ||
             data.side == meta.id || data.bottom == meta.id)) {
            found = true;
            break;
        }
    }
    m_blockSidecarChecked.insert(meta.id);
    if (found) m_blockTextureSet.insert(meta.id);
    return found;
}

// Removes .vblock sidecars that were auto-created for an auxiliary map (a
// sidecar whose main face IS the aux texture, e.g. andesite_n.vblock). Blocks
// that merely reference the aux texture as their normal/specular are kept.
// Runs once per texture UUID (see is_block_texture).
void EditorApplication::heal_aux_block_sidecars(const AssetMetadata& textureMeta) {
    if (!textureMeta.id.is_valid()) return;
    AssetBrowserModel browser(m_assetRegistry);
    bool changed = false;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (!load_block_asset(candidate.id, data)) continue;
        const bool isMainFace = data.texture == textureMeta.id || data.top == textureMeta.id ||
                                data.side == textureMeta.id || data.bottom == textureMeta.id;
        if (!isMainFace) continue;
        const AssetFileOperationResult removed = browser.delete_asset(candidate.id);
        if (!removed) {
            std::cerr << "[ContentBrowser] Could not remove aux-map block: " << removed.error << std::endl;
        } else {
            m_blockAssetCache.erase(candidate.id);
            m_blockAssetFailed.erase(candidate.id);
            changed = true;
        }
    }
    if (changed) {
        const auto registryPath =
            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[AssetRegistry] Could not persist aux-map block cleanup" << std::endl;
        std::cout << "[ContentBrowser] Removed block sidecar(s) for aux map '"
                  << textureMeta.sourcePath.filename().string() << "'" << std::endl;
    }
}

// One-time pass (after the Content Browser indexes): base blocks whose sidecar
// predates material-map grouping get their sibling _n/_s textures recorded as
// normal/specular, so andesite.vblock owns the whole material set even when it
// was created in an older session.
void EditorApplication::enrich_block_material_maps() {
    bool changed = false;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block || candidate.sourcePath.empty()) continue;
        BlockAssetData data;
        if (!load_block_asset(candidate.id, data)) continue;
        if (data.normal.is_valid() && data.specular.is_valid()) continue;
        if (!data.texture.is_valid()) continue;
        const auto texMeta = m_assetRegistry.find(data.texture);
        if (!texMeta || texMeta->sourcePath.empty()) continue;
        UUID normalId = data.normal, specularId = data.specular;
        std::string baseLower = texMeta->sourcePath.stem().string();
        std::transform(baseLower.begin(), baseLower.end(), baseLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const AssetMetadata& cand : m_assetRegistry.snapshot()) {
            if (cand.type != AssetType::Texture || cand.sourcePath.empty()) continue;
            std::string cStem = cand.sourcePath.stem().string();
            std::transform(cStem.begin(), cStem.end(), cStem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (cStem == baseLower + "_n" || cStem == baseLower + "_normal" ||
                cStem == baseLower + "_normalmap") {
                normalId = cand.id;
            } else if (cStem == baseLower + "_s" || cStem == baseLower + "_spec" ||
                       cStem == baseLower + "_specular") {
                specularId = cand.id;
            }
        }
        if (normalId == data.normal && specularId == data.specular) continue;
        data.normal = normalId;
        data.specular = specularId;
        m_blockAssetCache[candidate.id] = data;
        std::ofstream out(candidate.sourcePath);
        out << "{\"texture\":\"" << data.texture.to_string() << "\"";
        if (data.top.is_valid()) out << ",\"top\":\"" << data.top.to_string() << "\"";
        if (data.bottom.is_valid()) out << ",\"bottom\":\"" << data.bottom.to_string() << "\"";
        if (data.side.is_valid()) out << ",\"side\":\"" << data.side.to_string() << "\"";
        if (normalId.is_valid()) out << ",\"normal\":\"" << normalId.to_string() << "\"";
        if (specularId.is_valid()) out << ",\"specular\":\"" << specularId.to_string() << "\"";
        out << "}";
        changed = true;
    }
    if (changed) {
        std::cout << "[ContentBrowser] Enriched block sidecars with material maps" << std::endl;
    }
}

// User override: delete the .vblock sidecar (file + registry entry) so a
// texture that was misclassified as a block (a character/mob skin) becomes a
// plain texture again. The texture card then shows the flat image and stops
// spawning cubes.
void EditorApplication::unmark_block_texture(const AssetMetadata& textureMeta) {
    if (!textureMeta.id.is_valid()) return;
    // Remove the .vblock sidecar if one exists (the positive block mark).
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (!load_block_asset(candidate.id, data)) continue;
        if (data.texture != textureMeta.id && data.top != textureMeta.id &&
            data.side != textureMeta.id && data.bottom != textureMeta.id) {
            continue;
        }
        AssetBrowserModel browser(m_assetRegistry);
        const AssetFileOperationResult removed = browser.delete_asset(candidate.id);
        if (!removed) {
            std::cerr << "[ContentBrowser] Could not unmark block: " << removed.error << std::endl;
        } else {
            m_blockAssetCache.erase(candidate.id);
            m_blockAssetFailed.erase(candidate.id);
            const auto registryPath =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
            if (!m_assetRegistry.save(registryPath))
                std::cerr << "[AssetRegistry] Could not persist unmarked block" << std::endl;
        }
        break;
    }
    // Negative marker: even a heuristic block (e.g. inside a /block/ folder)
    // stops being one. The file is checked once per texture UUID (cached).
    if (!textureMeta.sourcePath.empty()) {
        const std::filesystem::path marker = textureMeta.sourcePath.string() + ".noblock";
        std::error_code ec;
        std::ofstream out(marker, std::ios::trunc);
        out << "noblock\n";
        out.close();
    }
    m_noblockTextures.insert(textureMeta.id);
    m_blockSidecarChecked.erase(textureMeta.id);
    m_blockTextureSet.erase(textureMeta.id);
    std::cout << "[ContentBrowser] Unmarked '" << textureMeta.sourcePath.filename().string()
              << "' as a block" << std::endl;
}

// Find-or-create the .vblock sidecar for a texture (JSON: texture UUID per
// face; all default to the source texture) and register it as AssetType::Block.
// The PNG itself IS the Minecraft-style block, so a texture that already has a
// sidecar referencing it is returned as-is instead of duplicating.
UUID EditorApplication::create_block_asset(const AssetMetadata& textureMeta) {
    if (!textureMeta.id.is_valid() || textureMeta.type != AssetType::Texture) return UUID{ 0, 0 };
    // "Marcar como Bloco" also clears a previous "noblock" override.
    if (m_noblockTextures.erase(textureMeta.id) > 0 && !textureMeta.sourcePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(textureMeta.sourcePath.string() + ".noblock", ec);
    }
    const auto lowerStem = [](const std::filesystem::path& p) {
        std::string s = p.stem().string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string baseLower = lowerStem(textureMeta.sourcePath);
    // Minecraft-style face textures (<base>_top/_side/_bottom) have no
    // <base>.png of their own (grass_block = grass_block_top + grass_block_side
    // + dirt). Every face import (re)assembles the parent <base>.vblock so the
    // block renders with the correct per-face atlas — converges as the pack
    // import visits the sibling faces in any order.
    static const char* kFaceSuffixes[] = { "_top", "_up", "_side", "_bottom", "_down" };
    std::string parentLower = baseLower;
    for (const char* suffix : kFaceSuffixes) {
        const size_t n = std::strlen(suffix);
        if (parentLower.size() > n && parentLower.compare(parentLower.size() - n, n, suffix) == 0) {
            parentLower.resize(parentLower.size() - n);
            break;
        }
    }
    if (!parentLower.empty() && parentLower != baseLower) {
        UUID pBase, pTop, pSide, pBottom, dirtTex;
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
            if (candidate.type != AssetType::Texture || candidate.sourcePath.empty()) continue;
            const std::string cStem = lowerStem(candidate.sourcePath);
            if (cStem == parentLower) pBase = candidate.id;
            else if (cStem == parentLower + "_top" || cStem == parentLower + "_up") pTop = candidate.id;
            else if (cStem == parentLower + "_side") pSide = candidate.id;
            else if (cStem == parentLower + "_bottom" || cStem == parentLower + "_down") pBottom = candidate.id;
            else if (cStem == "dirt") dirtTex = candidate.id;
        }
        const UUID mainTex = pBase.is_valid() ? pBase
                           : (pTop.is_valid() ? pTop : (pSide.is_valid() ? pSide : pBottom));
        if (mainTex.is_valid() && (pTop.is_valid() || pSide.is_valid() || pBottom.is_valid())) {
            const UUID faceTop = pTop.is_valid() ? pTop : mainTex;
            const UUID faceSide = pSide.is_valid() ? pSide : mainTex;
            // Minecraft convention: no dedicated bottom → dirt (grass_block);
            // otherwise the block's own texture (stone) or the top (logs).
            const UUID faceBottom = pBottom.is_valid() ? pBottom
                                  : (pBase.is_valid() ? pBase
                                  : (dirtTex.is_valid() ? dirtTex : faceTop));
            const std::filesystem::path parentPath = textureMeta.sourcePath.parent_path() /
                (parentLower + ".vblock");
            UUID parentId{ 0, 0 };
            if (const auto existing = m_assetRegistry.find_id(parentPath)) parentId = *existing;
            else parentId = UUID();
            {
                std::ofstream out(parentPath);
                out << "{\"texture\":\"" << mainTex.to_string() << "\"";
                out << ",\"top\":\"" << faceTop.to_string() << "\"";
                out << ",\"side\":\"" << faceSide.to_string() << "\"";
                if (faceBottom.is_valid()) out << ",\"bottom\":\"" << faceBottom.to_string() << "\"";
                out << "}";
            }
            AssetMetadata pmeta;
            pmeta.id = parentId;
            pmeta.type = AssetType::Block;
            pmeta.sourcePath = parentPath;
            pmeta.cookedPath = parentPath;
            pmeta.isCooked = true;
            pmeta.contentHash = textureMeta.contentHash;
            if (m_assetRegistry.register_asset(pmeta)) {
                m_blockAssetCache[parentId] = BlockAssetData{ mainTex, faceTop, faceBottom, faceSide,
                                                              UUID{ 0, 0 }, UUID{ 0, 0 } };
                m_blockAssetFailed.erase(parentId);
                const auto stale = m_blockAtlasTextures.find(parentId);
                if (stale != m_blockAtlasTextures.end()) {
                    destroy_graph_texture(stale->second);
                    m_blockAtlasTextures.erase(stale);
                }
                m_blockAtlasHashes.erase(parentId);
            }
        }
    }
    // Reuse an existing sidecar that already references this texture (repeat
    // drops/clicks/API calls must not pile up grass_2.vblock, grass_3.vblock…).
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (load_block_asset(candidate.id, data) &&
            (data.texture == textureMeta.id || data.top == textureMeta.id ||
             data.side == textureMeta.id || data.bottom == textureMeta.id)) {
            return candidate.id;
        }
    }
    std::filesystem::path blockPath = textureMeta.sourcePath.parent_path() /
        (textureMeta.sourcePath.stem().string() + ".vblock");
    unsigned suffix = 2;
    while (std::filesystem::exists(blockPath)) {
        blockPath = textureMeta.sourcePath.parent_path() /
            (textureMeta.sourcePath.stem().string() + "_" + std::to_string(suffix++) + ".vblock");
    }
    // Group the block's material set: sibling <base>_n / <base>_s textures
    // (normal/specular maps, already registered as plain textures) are recorded
    // in the sidecar so the block asset owns its maps, not just the albedo
    // face — andesite_n.png is no longer a separate "Block". Per-face sibling
    // textures (<base>_top/_side/_bottom) are recorded too, so the renderer's
    // per-face atlas shows grass_block_top on +Y, grass_block_side on the
    // sides and dirt on -Y instead of one texture everywhere.
    UUID normalId, specularId, topId, sideId, bottomId;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Texture || candidate.sourcePath.empty()) continue;
        const std::string cStem = lowerStem(candidate.sourcePath);
        if (cStem == baseLower + "_n" || cStem == baseLower + "_normal" ||
            cStem == baseLower + "_normalmap") {
            normalId = candidate.id;
        } else if (cStem == baseLower + "_s" || cStem == baseLower + "_spec" ||
                   cStem == baseLower + "_specular") {
            specularId = candidate.id;
        } else if (cStem == baseLower + "_top" || cStem == baseLower + "_up") {
            topId = candidate.id;
        } else if (cStem == baseLower + "_side") {
            sideId = candidate.id;
        } else if (cStem == baseLower + "_bottom" || cStem == baseLower + "_down") {
            bottomId = candidate.id;
        }
    }
    {
        std::ofstream out(blockPath);
        out << "{\"texture\":\"" << textureMeta.id.to_string() << "\"";
        if (topId.is_valid()) out << ",\"top\":\"" << topId.to_string() << "\"";
        if (sideId.is_valid()) out << ",\"side\":\"" << sideId.to_string() << "\"";
        if (bottomId.is_valid()) out << ",\"bottom\":\"" << bottomId.to_string() << "\"";
        if (normalId.is_valid()) out << ",\"normal\":\"" << normalId.to_string() << "\"";
        if (specularId.is_valid()) out << ",\"specular\":\"" << specularId.to_string() << "\"";
        out << "}";
    }
    AssetMetadata meta;
    meta.id = UUID();
    meta.type = AssetType::Block;
    meta.sourcePath = blockPath;
    meta.cookedPath = blockPath;
    meta.isCooked = true;
    meta.contentHash = textureMeta.contentHash;
    if (!m_assetRegistry.register_asset(meta)) {
        std::cerr << "[ContentBrowser] Failed to register block asset " << blockPath.string() << std::endl;
        return UUID{ 0, 0 };
    }
    m_blockAssetCache[meta.id] =
        BlockAssetData{ textureMeta.id,
                        topId.is_valid() ? topId : textureMeta.id,
                        bottomId.is_valid() ? bottomId : textureMeta.id,
                        sideId.is_valid() ? sideId : textureMeta.id,
                        normalId, specularId };
    m_blockAssetFailed.erase(meta.id);
    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (!m_assetRegistry.save(registryPath)) {
        std::cerr << "[AssetRegistry] Could not persist block asset" << std::endl;
    }
    return meta.id;
}

bool EditorApplication::set_block_faces(const UUID& blockId, const UUID& top,
                                        const UUID& side, const UUID& bottom) {
    const auto meta = m_assetRegistry.find(blockId);
    if (!meta || meta->type != AssetType::Block || meta->sourcePath.empty()) {
        std::cerr << "[BlockFaces] block asset not found" << std::endl;
        return false;
    }
    BlockAssetData data;
    if (!load_block_asset(blockId, data)) return false;
    const auto isTex = [&](const UUID& id) {
        if (!id.is_valid()) return true;
        const auto tm = m_assetRegistry.find(id);
        return tm && tm->type == AssetType::Texture;
    };
    if (!isTex(top) || !isTex(side) || !isTex(bottom)) {
        std::cerr << "[BlockFaces] a face UUID is not a registered texture" << std::endl;
        return false;
    }
    const UUID newTop = top.is_valid() ? top : (data.top.is_valid() ? data.top : data.texture);
    const UUID newSide = side.is_valid() ? side : (data.side.is_valid() ? data.side : data.texture);
    const UUID newBottom = bottom.is_valid() ? bottom : (data.bottom.is_valid() ? data.bottom : data.texture);
    data.top = newTop;
    data.side = newSide;
    data.bottom = newBottom;
    {
        std::ofstream out(meta->sourcePath);
        out << "{\"texture\":\"" << data.texture.to_string() << "\"";
        if (newTop.is_valid()) out << ",\"top\":\"" << newTop.to_string() << "\"";
        if (newSide.is_valid()) out << ",\"side\":\"" << newSide.to_string() << "\"";
        if (newBottom.is_valid()) out << ",\"bottom\":\"" << newBottom.to_string() << "\"";
        if (data.normal.is_valid()) out << ",\"normal\":\"" << data.normal.to_string() << "\"";
        if (data.specular.is_valid()) out << ",\"specular\":\"" << data.specular.to_string() << "\"";
        out << "}";
    }
    m_blockAssetCache[blockId] = data;
    m_blockAssetFailed.erase(blockId);
    // Invalidate the per-face atlas so the next render composites the new faces.
    const auto staleIt = m_blockAtlasTextures.find(blockId);
    if (staleIt != m_blockAtlasTextures.end()) {
        destroy_graph_texture(staleIt->second);
        m_blockAtlasTextures.erase(staleIt);
    }
    m_blockAtlasHashes.erase(blockId);
    std::cout << "[BlockFaces] updated " << blockId.to_string() << std::endl;
    return true;
}

UUID EditorApplication::create_block_from_faces(const UUID& base, const UUID& top,
                                                const UUID& side, const UUID& bottom,
                                                const std::string& name) {
    const auto isTex = [&](const UUID& id) {
        if (!id.is_valid()) return false;
        const auto tm = m_assetRegistry.find(id);
        return tm && tm->type == AssetType::Texture;
    };
    if (!isTex(base) && !isTex(top) && !isTex(side) && !isTex(bottom)) {
        std::cerr << "[BlockModelFaces] at least one face must be a registered texture" << std::endl;
        return UUID{ 0, 0 };
    }
    const UUID fallback = base.is_valid() ? base : (side.is_valid() ? side : top);
    std::filesystem::path blockPath;
    if (const auto bm = m_assetRegistry.find(fallback); bm && !bm->sourcePath.empty()) {
        blockPath = bm->sourcePath.parent_path();
    } else {
        blockPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "textures";
    }
    std::string stem = name;
    if (stem.empty()) stem = "block";
    std::string sanitized;
    for (char c : stem) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') sanitized += c;
    }
    if (sanitized.empty()) sanitized = "block";
    blockPath /= sanitized + ".vblock";
    unsigned suffix = 2;
    while (std::filesystem::exists(blockPath)) {
        blockPath = blockPath.parent_path() /
            (sanitized + "_" + std::to_string(suffix++) + ".vblock");
    }
    {
        std::ofstream out(blockPath);
        out << "{\"texture\":\"" << fallback.to_string() << "\"";
        if (top.is_valid()) out << ",\"top\":\"" << top.to_string() << "\"";
        if (side.is_valid()) out << ",\"side\":\"" << side.to_string() << "\"";
        if (bottom.is_valid()) out << ",\"bottom\":\"" << bottom.to_string() << "\"";
        out << "}";
    }
    AssetMetadata meta;
    meta.id = UUID();
    meta.type = AssetType::Block;
    meta.sourcePath = blockPath;
    meta.cookedPath = blockPath;
    meta.isCooked = true;
    meta.contentHash = 0;
    if (const auto bm = m_assetRegistry.find(fallback); bm) meta.contentHash = bm->contentHash;
    if (!m_assetRegistry.register_asset(meta)) {
        std::cerr << "[BlockModelFaces] failed to register block asset " << blockPath.string() << std::endl;
        return UUID{ 0, 0 };
    }
    m_blockAssetCache[meta.id] = BlockAssetData{ fallback, top, bottom, side, UUID{ 0, 0 }, UUID{ 0, 0 } };
    m_blockAssetFailed.erase(meta.id);
    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (!m_assetRegistry.save(registryPath)) {
        std::cerr << "[AssetRegistry] Could not persist block asset" << std::endl;
    }
    std::cout << "[BlockModelFaces] created " << meta.id.to_string() << " ("
              << blockPath.filename().string() << ")" << std::endl;
    return meta.id;
}

// Parses the .vblock sidecar (JSON is simple enough for a targeted string
// scan — no JSON dependency needed).
bool EditorApplication::load_block_asset(const UUID& blockAssetId, BlockAssetData& out) {
    const auto cached = m_blockAssetCache.find(blockAssetId);
    if (cached != m_blockAssetCache.end()) { out = cached->second; return true; }
    if (m_blockAssetFailed.contains(blockAssetId)) return false;
    const auto meta = m_assetRegistry.find(blockAssetId);
    if (!meta || meta->type != AssetType::Block || meta->sourcePath.empty() ||
        !std::filesystem::is_regular_file(meta->sourcePath)) {
        m_blockAssetFailed.insert(blockAssetId);
        return false;
    }
    std::ifstream in(meta->sourcePath);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const auto grab = [&](const char* key) -> UUID {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t p = text.find(needle);
        if (p == std::string::npos) return UUID{ 0, 0 };
        const size_t q = text.find('"', p + needle.size());
        if (q == std::string::npos) return UUID{ 0, 0 };
        const size_t r = text.find('"', q + 1);
        if (r == std::string::npos) return UUID{ 0, 0 };
        return UUID::from_string(text.substr(q + 1, r - q - 1));
    };
    BlockAssetData data;
    data.texture = grab("texture");
    data.top = grab("top");
    data.bottom = grab("bottom");
    data.side = grab("side");
    data.normal = grab("normal");
    data.specular = grab("specular");
    if (!data.texture.is_valid() && !data.top.is_valid() && !data.side.is_valid() && !data.bottom.is_valid()) {
        m_blockAssetFailed.insert(blockAssetId);
        return false;
    }
    m_blockAssetCache[blockAssetId] = data;
    return true;
}

UUID EditorApplication::resolve_block_texture(const UUID& blockAssetId) {
    // Block assets now resolve to themselves: load_viewport_texture hooks them
    // to the per-face atlas [top|side|bottom], so the renderer samples the
    // right face per side instead of one texture on the whole cube.
    if (const auto meta = m_assetRegistry.find(blockAssetId);
        meta && meta->type == AssetType::Block) {
        return blockAssetId;
    }
    BlockAssetData data;
    if (!load_block_asset(blockAssetId, data)) return UUID{ 0, 0 };
    if (data.texture.is_valid()) return data.texture;
    if (data.side.is_valid()) return data.side;
    if (data.top.is_valid()) return data.top;
    return data.bottom;
}

// ---------------------------------------------------------------------------
// Voxel sculpting (Escultura de Blocos) — real grid, real rendering, real
// painting. Each VoxelVolumeComponent entity owns an editable
// Engine::Voxel::VoxelStructure (32x24x32 cells, 1 m each) rendered as colored
// cubes; the brush panel paints into it via Engine::Voxel::VoxelTools.
// ---------------------------------------------------------------------------
namespace {
// kVoxelSizeX/Y/Z live in the anonymous namespace above setup_play_runtime()
// (shared with play-mode real world collision).
uint32_t voxel_hash2(int x, int z, uint32_t seed) {
    uint32_t h = seed ^ (static_cast<uint32_t>(x) * 374761393u) ^ (static_cast<uint32_t>(z) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 0xFFFFu;
}

glm::vec3 voxel_type_color(uint16_t type) {
    switch (type) {
        case 1: return glm::vec3(0.55f, 0.42f, 0.30f); // terra
        case 2: return glm::vec3(0.30f, 0.72f, 0.30f); // grama
        case 3: return glm::vec3(0.55f, 0.55f, 0.58f); // pedra
        case 4: return glm::vec3(0.25f, 0.45f, 0.85f); // água
        default: return glm::vec3(0.62f, 0.66f, 0.75f);
    }
}
} // namespace

void EditorApplication::ensure_voxel_volume(const UUID& entityId, uint32_t seed, float seaLevel) {
    if (m_voxelStructures.contains(entityId)) return;
    auto grid = std::make_unique<Engine::Voxel::VoxelStructure>(
        Engine::Voxel::Int3{ kVoxelSizeX, kVoxelSizeY, kVoxelSizeZ }, "Voxel");
    // Deterministic terrain from the volume seed (noise height per column).
    const int sea = std::clamp(static_cast<int>(seaLevel), 0, kVoxelSizeY - 2);
    for (int x = 0; x < kVoxelSizeX; ++x) {
        for (int z = 0; z < kVoxelSizeZ; ++z) {
            const uint32_t n = voxel_hash2(x, z, seed);
            const float v = static_cast<float>(n) / 65535.0f;
            const float hills = 6.0f * std::sin(x * 0.35f + seed * 0.001f) * std::cos(z * 0.28f);
            const int height = std::clamp(static_cast<int>(8.0f + v * 9.0f + hills * 0.5f), 2, kVoxelSizeY - 1);
            for (int y = 0; y < height; ++y) {
                const uint16_t type = (y == height - 1) ? 2 : ((y > sea) ? 3 : 1);
                grid->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ type, 0, 255 });
            }
            if (height < sea) {
                for (int y = height; y < sea; ++y) {
                    grid->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ 4, 0, 255 });
                }
            }
        }
    }
    m_voxelStructures[entityId] = std::move(grid);
    m_voxelMeshesDirty.insert(entityId);
}

UUID EditorApplication::resolve_voxel_type_block(uint16_t type) {
    // Explicit agent override (API `voxel-block <type> <uuid>`) wins.
    const auto overrideIt = m_voxelTypeBlocks.find(type);
    if (overrideIt != m_voxelTypeBlocks.end()) return overrideIt->second;
    // Name-based defaults from the BlockRegistry: 1=dirt, 2=grass, 3=stone,
    // 4=water. A texture pack import creates .vblock assets for these, so the
    // voxel volume picks them up automatically.
    static const char* keywords[5] = { nullptr, "dirt", "grass", "stone", "water" };
    if (type >= 5 || keywords[type] == nullptr) return UUID{ 0, 0 };
    const std::string keyword = keywords[type];
    const auto assets = m_assetRegistry.snapshot();
    // Prefer blocks with a real per-face map (top/side/bottom differ): the
    // assembled grass_block (top + side + dirt) must win over the single-face
    // grass_block_top/grass_block_side sidecars a pack import also produces.
    const auto faceScore = [&](const AssetMetadata& meta) {
        BlockAssetData data;
        if (!load_block_asset(meta.id, data)) return 0;
        int score = 0;
        if (data.top.is_valid() && data.top != data.side) ++score;
        if (data.bottom.is_valid() && data.bottom != data.side) ++score;
        return score;
    };
    const AssetMetadata* best = nullptr;
    int bestFaces = -1;
    bool bestHasBlock = false;
    for (const AssetMetadata& meta : assets) {
        if (meta.type != AssetType::Block) continue;
        std::string stem = meta.sourcePath.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stem.find(keyword) == std::string::npos) continue;
        const int faces = faceScore(meta);
        const bool hasBlock = stem.find("block") != std::string::npos;
        // Face-composited block > explicitly-named block > first match.
        if (!best || faces > bestFaces ||
            (faces == bestFaces && hasBlock && !bestHasBlock)) {
            best = &meta;
            bestFaces = faces;
            bestHasBlock = hasBlock;
        }
    }
    if (best) {
        std::cerr << "[Editor] voxel type " << static_cast<int>(type) << " -> block "
                  << best->id.to_string() << " (" << best->sourcePath.filename().string() << ")" << std::endl;
    } else {
        std::cerr << "[Editor] voxel type " << static_cast<int>(type)
                  << " -> no block match (registry " << m_assetRegistry.size() << ")" << std::endl;
    }
    return best ? best->id : UUID{ 0, 0 };
}

void EditorApplication::rebuild_voxel_mesh(const UUID& entityId) {
    const auto gridIt = m_voxelStructures.find(entityId);
    if (gridIt == m_voxelStructures.end()) return;
    const Engine::Voxel::VoxelStructure& grid = *gridIt->second;
    const auto trIt = m_editorScene->transformComponents.find(entityId);
    const glm::vec3 origin = (trIt != m_editorScene->transformComponents.end())
                                 ? trIt->second.position
                                 : glm::vec3(0.0f);

    auto& mesh = m_voxelMeshes[entityId];
    if (mesh.valid) {
        // Same in-flight hazard as terrain regeneration: wait for the GPU
        // before freeing buffers the previous frame may still read.
        if (mesh.vb.buffer != VK_NULL_HANDLE || mesh.ib.buffer != VK_NULL_HANDLE)
            vkDeviceWaitIdle(m_device);
        if (mesh.vb.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.vb);
        if (mesh.ib.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.ib);
        mesh = EditorVoxelMesh{};
    }

    // Surface-only meshing: emit a face only when the neighbouring voxel is
    // air (or out of bounds). This renders the visible shell with per-face
    // normals (correct shading) instead of every solid cell as an up-shaded
    // box — the old version looked shapeless and wasted ~6x the geometry on
    // internal faces that were never visible.
    const auto solid = [&](int x, int y, int z) -> bool {
        if (x < 0 || y < 0 || z < 0 ||
            x >= kVoxelSizeX || y >= kVoxelSizeY || z >= kVoxelSizeZ) return false;
        return !grid.get(Engine::Voxel::Int3{ x, y, z }).empty();
    };
    struct Face { glm::vec3 n; glm::vec3 c[4]; };
    const Face faces[6] = {
        { { 0, 0,-1 }, { {0,0,0},{1,0,0},{1,1,0},{0,1,0} } }, // -Z
        { { 0, 0, 1 }, { {1,0,1},{0,0,1},{0,1,1},{1,1,1} } }, // +Z
        { {-1, 0, 0 }, { {0,0,1},{0,0,0},{0,1,0},{0,1,1} } }, // -X
        { { 1, 0, 0 }, { {1,0,0},{1,0,1},{1,1,1},{1,1,0} } }, // +X
        { { 0,-1, 0 }, { {0,0,0},{1,0,0},{1,0,1},{0,0,1} } }, // -Y
        { { 0, 1, 0 }, { {0,1,1},{1,1,1},{1,1,0},{0,1,0} } }, // +Y
    };
    const int noff[6][3] = {
        { 0, 0,-1 }, { 0, 0, 1 }, {-1, 0, 0 }, { 1, 0, 0 }, { 0,-1, 0 }, { 0, 1, 0 },
    };
    // Per voxel type: its own vertex/index group so each type can be drawn
    // with a different block atlas pipeline. The block atlas layout is the
    // same for every block ([top|side|bottom], 3 wide), so the UV mapping is
    // identical across types — only the sampled texture differs.
    std::map<uint16_t, std::vector<EditorVertex>> typeVerts;
    std::map<uint16_t, std::vector<uint32_t>> typeIndices;
    std::unordered_map<uint16_t, UUID> typeBlock;
    std::unordered_set<uint16_t> typeResolved;
    const glm::vec2 cornerUv[4] = { {0,0},{1,0},{1,1},{0,1} };
    for (int x = 0; x < kVoxelSizeX; ++x) {
        for (int y = 0; y < kVoxelSizeY; ++y) {
            for (int z = 0; z < kVoxelSizeZ; ++z) {
                const Engine::Voxel::VoxelValue v = grid.get(Engine::Voxel::Int3{ x, y, z });
                if (v.empty()) continue;
                const glm::vec3 base = origin + glm::vec3(x - kVoxelSizeX / 2, y, z - kVoxelSizeZ / 2);
                auto& tv = typeVerts[v.type];
                auto& ti = typeIndices[v.type];
                // Resolve the block once per type. NOTE: UUID's default ctor is
                // RANDOM, so we must track resolution explicitly — comparing
                // typeBlock[type] against UUID{0,0} would never match.
                if (!typeResolved.contains(v.type)) {
                    typeResolved.insert(v.type);
                    typeBlock[v.type] = resolve_voxel_type_block(v.type);
                }
                const UUID block = typeBlock[v.type];
                // Vertex color is only used by the untextured fallback path;
                // textured types sample white so the albedo comes from the atlas.
                const glm::vec3 color = block.is_valid() ? glm::vec3(1.0f) : voxel_type_color(v.type);
                for (int f = 0; f < 6; ++f) {
                    if (solid(x + noff[f][0], y + noff[f][1], z + noff[f][2])) continue;
                    // Atlas regions: +Y top [0,1/3], sides [1/3,2/3], -Y bottom [2/3,1].
                    float u0 = 1.0f / 3.0f, u1 = 2.0f / 3.0f;
                    if (f == 5) { u0 = 0.0f; u1 = 1.0f / 3.0f; }
                    else if (f == 4) { u0 = 2.0f / 3.0f; u1 = 1.0f; }
                    const uint32_t first = static_cast<uint32_t>(tv.size());
                    for (int c = 0; c < 4; ++c) {
                        EditorVertex ev;
                        ev.pos = base + faces[f].c[c];
                        ev.normal = faces[f].n;
                        ev.color = color;
                        ev.uv = glm::vec2(u0 + cornerUv[c].x * (u1 - u0), cornerUv[c].y);
                        tv.push_back(ev);
                    }
                    ti.push_back(first);
                    ti.push_back(first + 1);
                    ti.push_back(first + 2);
                    ti.push_back(first);
                    ti.push_back(first + 2);
                    ti.push_back(first + 3);
                }
            }
        }
    }
    if (typeVerts.empty()) return;
    // Concatenate the per-type groups into one VB/IB, recording a range per
    // type (indices are rebased onto the shared vertex buffer).
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(32768);
    indices.reserve(49152);
    size_t vertBase = 0;
    for (auto& [type, tv] : typeVerts) {
        auto& ti = typeIndices[type];
        const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
        for (const uint32_t idx : ti) indices.push_back(idx + static_cast<uint32_t>(vertBase));
        verts.insert(verts.end(), tv.begin(), tv.end());
        vertBase += tv.size();
        mesh.ranges.push_back(EditorVoxelRange{
            firstIndex, static_cast<uint32_t>(ti.size()), type, typeBlock[type] });
    }
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  mesh.vb.buffer, mesh.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  mesh.ib.buffer, mesh.ib.memory);
    safe_map_and_copy(m_device, mesh.vb.memory, 0, vbSize, verts.data());
    safe_map_and_copy(m_device, mesh.ib.memory, 0, ibSize, indices.data());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.valid = true;
}

void EditorApplication::ensure_voxel_pipelines() {
    // Rebuild dirty meshes and pre-build the block atlas pipelines OUTSIDE the
    // render pass (see the draw-site comment: building GPU resources mid-pass
    // hung the device). Runs every frame; the pipeline cache makes it a no-op
    // after the first successful build, and a failed build is retried.
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();
    if (!renderScene) return;
    for (const auto& [id, vol] : renderScene->voxelVolumeComponents) {
        (void)vol;
        if (!renderScene->transformComponents.contains(id)) continue;
        if (m_voxelMeshesDirty.erase(id) != 0 || !m_voxelMeshes[id].valid) {
            ensure_voxel_volume(id, renderScene->voxelVolumeComponents[id].seed,
                                renderScene->voxelVolumeComponents[id].seaLevel);
            rebuild_voxel_mesh(id);
        }
        const auto& mesh = m_voxelMeshes[id];
        if (!mesh.valid) continue;
        for (const EditorVoxelRange& range : mesh.ranges) {
            if (range.blockId.is_valid()) {
                ensure_texture_pipeline(range.blockId, m_blockGraphPipelines);
            }
        }
    }
}

void EditorApplication::draw_voxel_volumes(VkCommandBuffer cmd, const glm::mat4& viewProj, Scene* scene) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    for (const auto& [id, vol] : scene->voxelVolumeComponents) {
        (void)vol;
        if (!scene->transformComponents.contains(id)) continue;
        // Meshes are rebuilt and pipelines pre-built by ensure_voxel_pipelines()
        // in the main loop — never create GPU resources while a render pass is
        // being recorded (that hung the device). A volume that is not ready
        // yet simply skips this frame.
        const auto& mesh = m_voxelMeshes[id];
        if (!mesh.valid || mesh.vb.buffer == VK_NULL_HANDLE) continue;
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vb.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const EditorVoxelRange& range : mesh.ranges) {
            // BlockRegistry-backed types render textured (per-face atlas
            // [top|side|bottom], sampled by a material-graph pipeline); types
            // without a matching block fall back to the vertex-color pipeline.
            GraphMaterialPipeline* gmp = nullptr;
            if (range.blockId.is_valid()) {
                gmp = ensure_texture_pipeline(range.blockId, m_blockGraphPipelines);
                if (gmp) {
                    write_material_ubo(*gmp, nullptr, nullptr);
                    write_light_ubo(*gmp, scene, m_editorCamera.position);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                                            0, 1, &gmp->descriptorSet, 0, nullptr);
                    const Rendering::MaterialPushConstants pc{ viewProj, glm::mat4(1.0f) };
                    vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT,
                                       0, sizeof(pc), &pc);
                }
            }
            if (!gmp) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                push_constants(cmd, m_scenePipelineLayout, viewProj, glm::vec4(1.0f));
            }
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        }
    }
}

// Paints with the active brush along a world ray. The brush settings come from
// the sculpt panel (m_activeVoxelBrush); right-drag forces Remove mode.
void EditorApplication::paint_voxel_ray(const glm::vec3& origin, const glm::vec3& dir, bool remove) {
    if (!m_editorScene) return;
    Scene* scene = m_editorScene.get();
    // Prefer the selected volume; otherwise the first one the ray hits.
    UUID target{ 0, 0 };
    if (m_selectedEntity.is_valid() && scene->voxelVolumeComponents.contains(m_selectedEntity.get_id())) {
        target = m_selectedEntity.get_id();
    }
    const auto& vols = scene->voxelVolumeComponents;
    if (!target.is_valid()) {
        float bestT = 1e18f;
        for (const auto& [id, vol] : vols) {
            (void)vol;
            const auto tit = scene->transformComponents.find(id);
            if (tit == scene->transformComponents.end()) continue;
            const glm::vec3 min = tit->second.position + glm::vec3(-kVoxelSizeX / 2, 0, -kVoxelSizeZ / 2);
            const glm::vec3 max = tit->second.position + glm::vec3(kVoxelSizeX / 2, kVoxelSizeY, kVoxelSizeZ / 2);
            const glm::vec3 inv = 1.0f / glm::max(glm::abs(dir), glm::vec3(1e-6f)) * glm::sign(dir);
            float t0 = glm::dot((min - origin), inv);
            float t1 = glm::dot((max - origin), inv);
            if (t0 > t1) std::swap(t0, t1);
            if (t0 <= t1 && t1 > 0.0f && t0 < bestT) {
                bestT = std::max(t0, 0.0f);
                target = id;
            }
        }
    }
    if (!target.is_valid()) return;
    const auto gridIt = m_voxelStructures.find(target);
    if (gridIt == m_voxelStructures.end()) return;
    const auto tit = scene->transformComponents.find(target);
    if (tit == scene->transformComponents.end()) return;

    // Ray vs grid AABB (grid-local space).
    const glm::vec3 gridMin(-kVoxelSizeX / 2, 0, -kVoxelSizeZ / 2);
    const glm::vec3 gridMax(kVoxelSizeX / 2, kVoxelSizeY, kVoxelSizeZ / 2);
    const glm::vec3 inv = 1.0f / glm::max(glm::abs(dir), glm::vec3(1e-6f)) * glm::sign(dir);
    float t0 = glm::dot((gridMin - (origin - tit->second.position)), inv);
    float t1 = glm::dot((gridMax - (origin - tit->second.position)), inv);
    if (t0 > t1) std::swap(t0, t1);
    if (t1 < 0.0f) return;
    const float hitT = std::max(t0, 0.0f);
    const glm::vec3 hitLocal = (origin - tit->second.position) + dir * hitT;
    const int hx = std::clamp(static_cast<int>(std::floor(hitLocal.x + kVoxelSizeX / 2)), 0, kVoxelSizeX - 1);
    const int hy = std::clamp(static_cast<int>(std::floor(hitLocal.y)), 0, kVoxelSizeY - 1);
    const int hz = std::clamp(static_cast<int>(std::floor(hitLocal.z + kVoxelSizeZ / 2)), 0, kVoxelSizeZ - 1);

    VoxelBrushOperation op = m_activeVoxelBrush;
    op.position = glm::vec3(hx + 0.5f, hy + 0.5f, hz + 0.5f); // grid cell space
    op.radius = std::max(m_activeVoxelBrush.radius, 0.5f);
    if (remove) op.mode = VoxelBrushMode::Remove;
    Engine::Voxel::VoxelTools::apply(*gridIt->second, op);
    m_voxelMeshesDirty.insert(target);
}

void EditorApplication::destroy_voxel_editor_meshes() {
    if (m_device == VK_NULL_HANDLE) return;
    for (auto& [id, mesh] : m_voxelMeshes) {
        (void)id;
        if (mesh.vb.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.vb);
        if (mesh.ib.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.ib);
    }
    m_voxelMeshes.clear();
    m_voxelStructures.clear();
    m_voxelMeshesDirty.clear();
}

// Builds (or returns cached) the per-face atlas for a Block asset: a 3-wide
// image [top | side | bottom] composited from the .vblock face maps, with the
// main texture as fallback for missing faces. Blocks without face maps still
// get the 3-region atlas (all regions = the main texture), so the cube UVs
// stay uniform across every block.
bool EditorApplication::ensure_block_atlas(const UUID& blockId, GraphTexture& out) {
    const auto meta = m_assetRegistry.find(blockId);
    if (!meta || meta->type != AssetType::Block) return false;
    const auto hashIt = m_blockAtlasHashes.find(blockId);
    if (hashIt != m_blockAtlasHashes.end()) {
        if (hashIt->second == meta->contentHash) {
            const auto texIt = m_blockAtlasTextures.find(blockId);
            if (texIt != m_blockAtlasTextures.end() && texIt->second.image != VK_NULL_HANDLE) {
                out = texIt->second;
                return true;
            }
        }
        // Stale content (hot reload): rebuild from the new source pixels.
        const auto staleIt = m_blockAtlasTextures.find(blockId);
        if (staleIt != m_blockAtlasTextures.end()) {
            destroy_graph_texture(staleIt->second);
            m_blockAtlasTextures.erase(staleIt);
        }
        m_blockAtlasHashes.erase(hashIt);
    }
    BlockAssetData data;
    if (!load_block_asset(blockId, data)) return false;
    const UUID mainTex = data.texture.is_valid() ? data.texture
                       : (data.side.is_valid() ? data.side : data.top);
    if (!mainTex.is_valid()) return false;
    const auto texMeta = m_assetRegistry.find(mainTex);
    if (!texMeta || texMeta->type != AssetType::Texture || texMeta->cookedPath.empty()) return false;
    std::string err;
    DecodedTexturePixels base;
    if (!decode_cooked_texture_pixels(texMeta->cookedPath, 256, base, err)) return false;
    const uint32_t w = base.width, h = base.height;
    if (w == 0 || h == 0) return false;
    const uint32_t atlasW = w * 3;
    std::vector<uint8_t> atlas(static_cast<size_t>(atlasW) * h * 4);
    const auto blitFace = [&](const UUID& faceId, uint32_t region) {
        uint8_t* dst = atlas.data() + static_cast<size_t>(region) * w * 4;
        const auto fm = m_assetRegistry.find(faceId);
        if (faceId.is_valid() && fm && fm->type == AssetType::Texture && !fm->cookedPath.empty()) {
            DecodedTexturePixels px;
            if (decode_cooked_texture_pixels(fm->cookedPath, 256, px, err) &&
                px.width == w && px.height == h) {
                std::memcpy(dst, px.rgba.data(), static_cast<size_t>(w) * h * 4);
                return;
            }
        }
        std::memcpy(dst, base.rgba.data(), static_cast<size_t>(w) * h * 4);
    };
    blitFace(data.top, 0);     // +Y
    blitFace(data.side, 1);    // ±X/±Z
    blitFace(data.bottom, 2);  // -Y
    GraphTexture atlasTex;
    if (!upload_texture_pixels(atlasW, h, atlas, 1, base.srgb, atlasTex, err)) return false;
    m_blockAtlasTextures[blockId] = atlasTex;
    m_blockAtlasHashes[blockId] = meta->contentHash;
    out = atlasTex;
    return true;
}

bool EditorApplication::load_viewport_texture(const UUID& assetId, GraphTexture& out, std::string& error,
                                              uint32_t maxDim) {
    const auto metaOpt = m_assetRegistry.find(assetId);
    if (!metaOpt) {
        error = "texture asset not found in registry";
        std::cerr << "[Editor] load_viewport_texture: missing asset " << assetId.to_string()
                  << " (registry size " << m_assetRegistry.size() << ")" << std::endl;
        return false;
    }
    const AssetMetadata& meta = *metaOpt;
    // Block assets sample their per-face atlas [top|side|bottom] instead of a
    // single texture on all six faces. This hook serves BOTH the scene
    // material-graph path and the block thumbnail pipeline.
    if (meta.type == AssetType::Block) {
        return ensure_block_atlas(assetId, out);
    }
    if (meta.type != AssetType::Texture || meta.cookedPath.empty()) {
        error = "asset is not a cooked texture";
        return false;
    }
    DecodedTexturePixels px;
    if (!decode_cooked_texture_pixels(meta.cookedPath, maxDim, px, error)) return false;
    if (px.halfFloat) {
        return upload_texture_half_pixels(px.width, px.height, px.rgba, out, error);
    }
    return upload_texture_pixels(px.width, px.height, px.rgba, px.mipCount, px.srgb, out, error);
}

bool EditorApplication::upload_texture_pixels(uint32_t width, uint32_t height,
                                              const std::vector<uint8_t>& rgba,
                                              uint32_t mipCount, bool srgb,
                                              GraphTexture& out, std::string& error) {
    // Import settings applied here (Fase 2): srgb selects the SRGB image
    // format and mipCount uploads the cooked mip chain (level 0 first) into a
    // mip-mapped image + view, so mipmapped textures actually sample the chain.
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    out.format = format;
    const uint32_t mips = std::max(mipCount, 1u);
    VkDeviceSize imageSize = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        imageSize += static_cast<VkDeviceSize>(mw) * mh * 4;
    }
    create_image(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory, mips);
    if (out.image == VK_NULL_HANDLE) {
        error = "texture image allocation failed";
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "texture staging buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    safe_map_and_copy(m_device, stagingMemory, 0, imageSize, rgba.data());
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips);
    VkDeviceSize offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        region.imageExtent = { mw, mh, 1 };
        regions.push_back(region);
        offset += static_cast<VkDeviceSize>(mw) * mh * 4;
    }
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
    end_single_time_commands(cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    out.view = create_image_view(out.image, format, VK_IMAGE_ASPECT_COLOR_BIT, mips);
    if (out.view == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "texture image view creation failed";
        return false;
    }
    return true;
}

// Uploads an RGBA16F (half-float RGBA) payload as an R16G16B16A16_SFLOAT image
// — the HDR path of the material graph (Radiance .hdr cooks to this layout).
bool EditorApplication::upload_texture_half_pixels(uint32_t width, uint32_t height,
                                                   const std::vector<uint8_t>& halfRgba,
                                                   GraphTexture& out, std::string& error) {
    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    out.format = format;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 8;
    create_image(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory);
    if (out.image == VK_NULL_HANDLE) {
        error = "HDR texture image allocation failed";
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "HDR texture staging buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    safe_map_and_copy(m_device, stagingMemory, 0, imageSize, halfRgba.data());
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    out.view = create_image_view(out.image, format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (out.view == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "HDR texture image view creation failed";
        return false;
    }
    return true;
}

bool EditorApplication::build_graph_pipeline(const Rendering::MaterialGraph& graph, GraphMaterialPipeline& out) {
    // Preserve the caller's cache key: out is reset below and graphHash is the
    // rebuild-detection stamp used by the per-material cache.
    const uint64_t callerGraphHash = out.graphHash;
    out = GraphMaterialPipeline{};
    out.graphHash = callerGraphHash;
    // TextureSample nodes: the texture asset UUID lives in the node value
    // (string). Binding i+1 corresponds to the i-th TextureSample in node order.
    std::vector<UUID> textureIds;
    for (const auto& node : graph.nodes()) {
        if (node.kind != Rendering::MaterialNodeKind::TextureSample) continue;
        const auto* value = std::get_if<std::string>(&node.value);
        textureIds.push_back(value && !value->empty() ? UUID::from_string(*value) : UUID{});
    }
    std::vector<GraphTexture> textures;
    textures.reserve(textureIds.size());
    for (const UUID& id : textureIds) {
        GraphTexture tex;
        std::string texError;
        if (!id.is_valid() || !load_viewport_texture(id, tex, texError)) {
            out.lastError = texError.empty()
                ? "a TextureSample node has no texture asset assigned" : texError;
            // Only destroy textures the pipeline owns (atlas textures are
            // borrowed from m_blockAtlasTextures).
            for (size_t i = 0; i < textures.size(); ++i) {
                if (i < out.textureIsAtlas.size() && out.textureIsAtlas[i]) continue;
                destroy_graph_texture(textures[i]);
            }
            out.textureIsAtlas.clear();
            return false;
        }
        // Block assets sample their per-face atlas, which is OWNED by
        // m_blockAtlasTextures — the pipeline only references it, so record
        // that so destroy_graph_pipeline doesn't free it twice.
        const auto meta = m_assetRegistry.find(id);
        out.textureIsAtlas.push_back(meta && meta->type == AssetType::Block);
        textures.push_back(std::move(tex));
    }
    out.textures = std::move(textures);
    const Rendering::GlslGenerationResult gen = material_graph_to_glsl(graph);
    if (!gen) {
        out.lastError = gen.errors.empty() ? "material graph compile failed" : gen.errors[0].message;
        return false;
    }
    // VC_EDITOR_DUMP_MATERIAL_GLSL=1: log the generated fragment source (useful
    // to debug alpha cutout, BRDF and sampler wiring without instrumenting
    // the generator).
    if (std::getenv("VC_EDITOR_DUMP_MATERIAL_GLSL") != nullptr) {
        std::cout << "[MaterialGLSL] --- generated fragment source ---\n"
                  << gen.source << "\n[MaterialGLSL] --- end ---" << std::endl;
    }

    const std::vector<uint32_t> vertSpv = read_spv("editor_material.vert.spv");
    if (vertSpv.empty()) {
        out.lastError = "editor_material.vert.spv is missing (re-run compile_shaders)";
        return false;
    }
    const std::vector<uint32_t> fragSpv = compile_material_glsl(VK_SHADER_STAGE_FRAGMENT_BIT, gen.source);
    if (fragSpv.empty()) {
        out.lastError = "glslc failed to compile the generated material shader";
        return false;
    }
    VkShaderModule vertModule = make_module(m_device, vertSpv);
    VkShaderModule fragModule = make_module(m_device, fragSpv);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragModule, nullptr);
        out.lastError = "VkShaderModule creation failed";
        return false;
    }

    // Descriptor set layout: binding 0 = material params UBO; bindings 1..N =
    // combined image samplers (one per TextureSample node, node order); then the
    // LightParams UBO and the shadow-map sampler at the bindings the generated
    // shader declared.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(3 + out.textures.size());
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(uboBinding);
    for (size_t i = 0; i < out.textures.size(); ++i) {
        VkDescriptorSetLayoutBinding texBinding{};
        texBinding.binding = static_cast<uint32_t>(i + 1);
        texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texBinding.descriptorCount = 1;
        texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(texBinding);
    }
    out.lightUboBinding = gen.lightUboBinding;
    VkDescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding = out.lightUboBinding;
    lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightBinding.descriptorCount = 1;
    lightBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(lightBinding);

    // Shadow-map sampler (dummy 1x1 white texture; shadows disabled in the
    // editor viewport).
    out.shadowSamplerBinding = gen.shadowSamplerBinding;
    {
        std::string texError;
        if (!upload_texture_pixels(1, 1, { 255, 255, 255, 255 }, 1, false, out.shadowDummy, texError)) {
            destroy_graph_pipeline(out);
            out.lastError = "shadow dummy texture failed: " + texError;
            return false;
        }
    }
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = out.shadowSamplerBinding;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(shadowBinding);
    VkDescriptorSetLayoutCreateInfo dslInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    dslInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &dslInfo, nullptr, &out.descriptorSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        destroy_graph_pipeline(out);
        out.lastError = "descriptor set layout creation failed";
        return false;
    }

    // Pipeline layout: MVP + model push constant (vertex stage) + material set
    // (params + textures + lights, all fragment).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(Rendering::MaterialPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &out.descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &out.layout) != VK_SUCCESS) {
        destroy_graph_pipeline(out);
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        out.lastError = "pipeline layout creation failed";
        return false;
    }

    // UBO sized with std140 offsets; capture parameter defaults for per-frame writes.
    out.uniformNames = gen.uniformNames;
    out.uniformTypes = gen.uniformTypes;
    out.uniformDefaults.reserve(gen.uniformNames.size());
    out.uboSize = 0;
    for (size_t i = 0; i < gen.uniformNames.size(); ++i) {
        const auto* parameter = graph.find_parameter(gen.uniformNames[i]);
        out.uniformDefaults.push_back(parameter ? parameter->defaultValue : Rendering::MaterialValue(0.0f));
        out.uboSize = align_material_offset(out.uboSize, material_std140_alignment(gen.uniformTypes[i]));
        out.uboSize += material_std140_size(gen.uniformTypes[i]);
    }
    out.uboSize = align_material_offset(out.uboSize, 16);
    if (out.uboSize == 0) out.uboSize = 16;
    create_buffer(out.uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.uboBuffer, out.uboMemory);
    create_buffer(sizeof(Rendering::LightUboData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.lightBuffer, out.lightMemory);

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(out.textures.size()) + 1;
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    bool poolOk = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &out.pool) == VK_SUCCESS;
    if (poolOk) {
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = out.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &out.descriptorSetLayout;
        poolOk = vkAllocateDescriptorSets(m_device, &allocInfo, &out.descriptorSet) == VK_SUCCESS;
    }
    if (!poolOk) {
        out.lastError = "descriptor pool/set allocation failed";
        destroy_graph_pipeline(out);
        return false;
    }
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(out.textures.size() + 2);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(3 + out.textures.size());
    VkDescriptorBufferInfo bufferInfo{ out.uboBuffer, 0, out.uboSize };
    VkWriteDescriptorSet uboWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    uboWrite.dstSet = out.descriptorSet;
    uboWrite.descriptorCount = 1;
    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.pBufferInfo = &bufferInfo;
    writes.push_back(uboWrite);
    VkDescriptorBufferInfo lightBufferInfo{ out.lightBuffer, 0, sizeof(Rendering::LightUboData) };
    VkWriteDescriptorSet lightWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    lightWrite.dstSet = out.descriptorSet;
    lightWrite.dstBinding = out.lightUboBinding;
    lightWrite.descriptorCount = 1;
    lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightWrite.pBufferInfo = &lightBufferInfo;
    writes.push_back(lightWrite);
    for (size_t i = 0; i < out.textures.size(); ++i) {
        VkDescriptorImageInfo imageInfo{};
        // Block/pixel-art textures use NEAREST filtering (Minecraft look);
        // skins, decals and PBR textures keep trilinear + anisotropic.
        const bool isBlockAtlas = i < out.textureIsAtlas.size() && out.textureIsAtlas[i];
        imageInfo.sampler = isBlockAtlas ? m_blockDrawSampler : m_offscreen.sampler;
        imageInfo.imageView = out.textures[i].view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.push_back(imageInfo);
        VkWriteDescriptorSet texWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        texWrite.dstSet = out.descriptorSet;
        texWrite.dstBinding = static_cast<uint32_t>(i + 1);
        texWrite.descriptorCount = 1;
        texWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texWrite.pImageInfo = &imageInfos.back();
        writes.push_back(texWrite);
    }
    VkDescriptorImageInfo shadowImageInfo{};
    // Real sun shadow map when available; the 1x1 dummy otherwise (no sun).
    shadowImageInfo.sampler = m_shadowMap.sampler != VK_NULL_HANDLE
        ? m_shadowMap.sampler : m_offscreen.sampler;
    shadowImageInfo.imageView = m_shadowMap.view != VK_NULL_HANDLE
        ? m_shadowMap.view : out.shadowDummy.view;
    shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos.push_back(shadowImageInfo);
    VkWriteDescriptorSet shadowWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    shadowWrite.dstSet = out.descriptorSet;
    shadowWrite.dstBinding = out.shadowSamplerBinding;
    shadowWrite.descriptorCount = 1;
    shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowWrite.pImageInfo = &imageInfos.back();
    writes.push_back(shadowWrite);
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // Graphics pipeline: same EditorVertex layout, no culling (glTF winding varies).
    out.pipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, out.layout,
                                         vertModule, fragModule, m_viewportSamples,
                                         false, true, false, true);
    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragModule, nullptr);
    if (out.pipeline == VK_NULL_HANDLE) {
        out.lastError = "vkCreateGraphicsPipelines failed";
        destroy_graph_pipeline(out);
        return false;
    }
    out.lastError.clear();
    out.valid = true;
    return true;
}

void EditorApplication::destroy_graph_pipeline(GraphMaterialPipeline& p) {
    if (m_device == VK_NULL_HANDLE) return;
    if (p.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, p.pipeline, nullptr);
    if (p.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, p.layout, nullptr);
    if (p.descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, p.descriptorSetLayout, nullptr);
    if (p.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, p.pool, nullptr);
    if (p.uboBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, p.uboBuffer, nullptr);
    if (p.uboMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, p.uboMemory, nullptr);
    if (p.lightBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, p.lightBuffer, nullptr);
    if (p.lightMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, p.lightMemory, nullptr);
    destroy_graph_texture(p.shadowDummy);
    for (size_t i = 0; i < p.textures.size(); ++i) {
        // Block atlases are owned by m_blockAtlasTextures — reference only.
        if (i < p.textureIsAtlas.size() && p.textureIsAtlas[i]) continue;
        destroy_graph_texture(p.textures[i]);
    }
    p.textures.clear();
    p.textureIsAtlas.clear();
    p = GraphMaterialPipeline{};
}

void EditorApplication::destroy_graph_material_pipelines() {
    for (auto& [id, p] : m_graphMaterialPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_graphMaterialPipelines.clear();
    for (auto& [id, p] : m_blockGraphPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_blockGraphPipelines.clear();
    for (auto& [id, p] : m_skinGraphPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_skinGraphPipelines.clear();
    for (auto& [id, p] : m_videoGraphPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_videoGraphPipelines.clear();
    destroy_graph_pipeline(m_liveGraphPipeline);
    m_liveGraphHash = 0;
}

void EditorApplication::write_light_ubo(GraphMaterialPipeline& p, const Scene* scene,
                                        const glm::vec3& cameraPos) {
    if (!p.valid || p.lightBuffer == VK_NULL_HANDLE) return;
    Rendering::LightUboData data{};
    data.cameraPosition = glm::vec4(cameraPos, 1.0f);
    // Real sun shadow map: VP + enabled flag + bias + single-map mode (z=1).
    data.sunViewProj = m_shadowMap.viewProj;
    data.shadowParams = glm::vec4(m_shadowMap.enabled ? 1.0f : 0.0f, 0.0006f,
                                  m_shadowMap.enabled ? 1.0f : 0.0f, 0.0f);
    const glm::mat4 view = m_editorCamera.get_view_matrix();
    data.cameraForward = glm::vec4(glm::normalize(glm::vec3(-view[2][0], -view[2][1], -view[2][2])), 0.0f);
    uint32_t pointCount = 0, spotCount = 0, areaCount = 0;
    if (scene) {
        for (const auto& [id, light] : scene->lightComponents) {
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            glm::vec3 position(0.0f);
            const auto tit = scene->transformComponents.find(id);
            if (tit != scene->transformComponents.end()) {
                position = tit->second.position;
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                dir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            // Editor lights use lux-like intensity (default sun = 10000) but the
            // MaterialPipeline Lambert term adds lightColor.rgb straight into
            // lightAccum (lit = baseColor*(0.22+0.78*lightAccum)), which expects
            // ~1.0 for a full-strength light. Normalize to the default-sun
            // reference so material-graph lit objects (blocks, voxels, skins,
            // decals) get balanced light instead of white-out or ambient-only.
            const glm::vec3 colorIntensity = light.color * (light.intensity / 10000.0f);
            if (is_directional_sun(light)) {
                data.sunDirection = glm::vec4(dir, 1.0f);
                data.sunColor = glm::vec4(colorIntensity, 1.0f);
            } else if (light.type == LightType::Spot && spotCount < Rendering::kMaxSpotLights) {
                data.spotLightPos[spotCount] = glm::vec4(position, light.range);
                data.spotLightDir[spotCount] = glm::vec4(dir, 1.0f);
                data.spotLightParams[spotCount] = glm::vec4(
                    std::cos(glm::radians(25.0f)), std::cos(glm::radians(45.0f)), 0.0f, 0.0f);
                data.spotLightColor[spotCount] = glm::vec4(colorIntensity, 1.0f);
                ++spotCount;
            } else if (light.type == LightType::Area && areaCount < Rendering::kMaxAreaLights) {
                data.areaLightPos[areaCount] = glm::vec4(position, 1.0f);
                data.areaLightNormal[areaCount] = glm::vec4(dir, 1.0f);
                data.areaLightHalf[areaCount] = glm::vec4(2.0f, 1.0f, 0.0f, 0.0f);
                data.areaLightColor[areaCount] = glm::vec4(colorIntensity, 1.0f);
                ++areaCount;
            } else if (pointCount < Rendering::kMaxPointLights) {
                data.pointLightPos[pointCount] = glm::vec4(position, light.range);
                data.pointLightColor[pointCount] = glm::vec4(colorIntensity, 1.0f);
                ++pointCount;
            }
        }
    }
    void* mapped = nullptr;
    if (vkMapMemory(m_device, p.lightMemory, 0, sizeof(data), 0, &mapped) != VK_SUCCESS) return;
    std::memcpy(mapped, &data, sizeof(data));
    vkUnmapMemory(m_device, p.lightMemory);
}

void EditorApplication::write_material_ubo(const GraphMaterialPipeline& p, const MaterialAsset* material,
                                           const MaterialComponent* component) {
    if (!p.valid || p.uboBuffer == VK_NULL_HANDLE || p.uboSize == 0) return;
    const glm::vec3 albedo = material ? material->albedo
                                      : (component ? component->albedo : glm::vec3(1.0f, 1.0f, 1.0f));
    const float roughness = material ? material->roughness : (component ? component->roughness : 0.5f);
    const float metallic = material ? material->metallic : (component ? component->metallic : 0.0f);
    const glm::vec3 emissive = material
        ? material->emissiveColor * material->emissiveIntensity
        : (component ? component->emissiveColor * component->emissiveIntensity : glm::vec3(0.0f));
    const float emissiveIntensity = material ? material->emissiveIntensity
                                             : (component ? component->emissiveIntensity : 0.0f);
    void* mapped = nullptr;
    if (vkMapMemory(m_device, p.uboMemory, 0, p.uboSize, 0, &mapped) != VK_SUCCESS) return;
    auto* bytes = static_cast<std::byte*>(mapped);
    size_t offset = 0;
    for (size_t i = 0; i < p.uniformNames.size(); ++i) {
        offset = align_material_offset(offset, material_std140_alignment(p.uniformTypes[i]));
        if (offset + 16 > p.uboSize) break;
        Rendering::MaterialValue value = p.uniformDefaults[i];
        const std::string& name = p.uniformNames[i];
        if (name == "Albedo" || name == "BaseColor") {
            value = (p.uniformTypes[i] == Rendering::MaterialValueType::Vec4)
                ? Rendering::MaterialValue(glm::vec4(albedo, 1.0f))
                : Rendering::MaterialValue(albedo);
        } else if (name == "Roughness") {
            value = roughness;
        } else if (name == "Metallic") {
            value = metallic;
        } else if (name == "Emissive") {
            value = (p.uniformTypes[i] == Rendering::MaterialValueType::Vec4)
                ? Rendering::MaterialValue(glm::vec4(emissive, 1.0f))
                : Rendering::MaterialValue(emissive);
        } else if (name == "EmissiveIntensity") {
            value = emissiveIntensity;
        } else if (name == "Opacity") {
            value = 1.0f;
        }
        write_ubo_value(bytes + offset, p.uniformTypes[i], value);
        offset += material_std140_size(p.uniformTypes[i]);
    }
    vkUnmapMemory(m_device, p.uboMemory);
}

void EditorApplication::destroy_mesh_resources() {
    for (auto& [id, resource] : m_meshResources) {
        (void)id;
        if (resource.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, resource.vb.buffer, nullptr);
        if (resource.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, resource.vb.memory, nullptr);
        if (resource.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, resource.ib.buffer, nullptr);
        if (resource.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, resource.ib.memory, nullptr);
    }
    m_meshResources.clear();
    m_meshLoadFailed.clear();
}

void EditorApplication::cleanup() {
    join_worker_threads();
    shutdown_audio_output();
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        destroy_mesh_resources();
        destroy_graph_material_pipelines();
        destroy_asset_thumbnails();
        destroy_block_cube();
        destroy_thumbnail_target();
        destroy_voxel_editor_meshes();
        cleanup_offscreen_target();

        if (m_offscreen.renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_offscreen.renderPass, nullptr);
            m_offscreen.renderPass = VK_NULL_HANDLE;
        }
        if (m_offscreen.pickRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_offscreen.pickRenderPass, nullptr);
            m_offscreen.pickRenderPass = VK_NULL_HANDLE;
        }
        if (m_offscreen.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_offscreen.sampler, nullptr);
            m_offscreen.sampler = VK_NULL_HANDLE;
        }

        destroy_buffer(m_cubeVB);
        destroy_buffer(m_cubeIB);
        destroy_buffer(m_lightIconVB);
        destroy_buffer(m_cameraIconVB);
        destroy_buffer(m_gizmoVB);
        destroy_buffer(m_gizmoIB);
        destroy_buffer(m_terrainVB);
        destroy_buffer(m_terrainIB);
        m_terrainValid = false;
        // Runtime-wired Wicked-port resources.
        destroy_buffer(m_envSphereVB);
        destroy_buffer(m_envSphereIB);
        destroy_buffer(m_decalVB);
        destroy_buffer(m_decalIB);
        for (auto& [id, sim] : m_hairs) { (void)id; destroy_buffer(sim.vb); }
        m_hairs.clear();
        for (auto& [id, sim] : m_softBodies) { (void)id; destroy_buffer(sim.vb); destroy_buffer(sim.ib); }
        m_softBodies.clear();
        for (auto& [id, cloud] : m_splatClouds) { (void)id; destroy_buffer(cloud.vb); }
        m_splatClouds.clear();
        for (auto& [id, pb] : m_paintBuffers) { (void)id; destroy_buffer(pb.vb); }
        m_paintBuffers.clear();
        if (m_envCapture.valid) {
            for (int i = 0; i < 6; ++i) {
                if (m_envCapture.framebuffers[i]) vkDestroyFramebuffer(m_device, m_envCapture.framebuffers[i], nullptr);
                if (m_envCapture.views[i]) vkDestroyImageView(m_device, m_envCapture.views[i], nullptr);
            }
            if (m_envCapture.image) vkDestroyImage(m_device, m_envCapture.image, nullptr);
            if (m_envCapture.memory) vkFreeMemory(m_device, m_envCapture.memory, nullptr);
            if (m_envCapture.renderPass) vkDestroyRenderPass(m_device, m_envCapture.renderPass, nullptr);
            if (m_envCapture.sampler) vkDestroySampler(m_device, m_envCapture.sampler, nullptr);
        }
        if (m_envCubeView) vkDestroyImageView(m_device, m_envCubeView, nullptr);
        if (m_envDepthView) vkDestroyImageView(m_device, m_envDepthView, nullptr);
        if (m_envDepthImage) vkDestroyImage(m_device, m_envDepthImage, nullptr);
        if (m_envDepthMemory) vkFreeMemory(m_device, m_envDepthMemory, nullptr);
        if (m_envSphereDescLayout) vkDestroyDescriptorSetLayout(m_device, m_envSphereDescLayout, nullptr);
        if (m_envSphereDescPool) vkDestroyDescriptorPool(m_device, m_envSphereDescPool, nullptr);
        if (m_envSpherePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_envSpherePipeline, nullptr);
        if (m_envSpherePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_envSpherePipelineLayout, nullptr);
        if (m_envSphereFragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_envSphereFragShader, nullptr);
        if (m_envSphereVertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_envSphereVertShader, nullptr);
        if (m_splatPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_splatPipeline, nullptr);
        if (m_splatPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_splatPipelineLayout, nullptr);
        if (m_splatFragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_splatFragShader, nullptr);
        if (m_splatVertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_splatVertShader, nullptr);
        if (m_hairPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_hairPipeline, nullptr);

        if (m_pickPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pickPipeline, nullptr); m_pickPipeline = VK_NULL_HANDLE; }
        if (m_gizmoPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gizmoPipeline, nullptr); m_gizmoPipeline = VK_NULL_HANDLE; }
        if (m_wireframePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_wireframePipeline, nullptr); m_wireframePipeline = VK_NULL_HANDLE; }
        if (m_scenePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_scenePipeline, nullptr); m_scenePipeline = VK_NULL_HANDLE; }
        if (m_scenePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr); m_scenePipelineLayout = VK_NULL_HANDLE; }
        if (m_gridPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gridPipeline, nullptr); m_gridPipeline = VK_NULL_HANDLE; }
        if (m_gridPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_gridPipelineLayout, nullptr); m_gridPipelineLayout = VK_NULL_HANDLE; }
        if (m_gridFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_gridFragShader, nullptr); m_gridFragShader = VK_NULL_HANDLE; }
        if (m_gridVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_gridVertShader, nullptr); m_gridVertShader = VK_NULL_HANDLE; }
        if (m_pickFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_pickFragShader, nullptr); m_pickFragShader = VK_NULL_HANDLE; }
        if (m_viewportFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_viewportFragShader, nullptr); m_viewportFragShader = VK_NULL_HANDLE; }
        if (m_viewportVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_viewportVertShader, nullptr); m_viewportVertShader = VK_NULL_HANDLE; }

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (m_imguiDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);
        }

        for (size_t i = 0; i < 2; i++) {
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
        }

        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }

        for (auto fb : m_framebuffers) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
        }

        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        }

        for (auto view : m_swapchainViews) {
            vkDestroyImageView(m_device, view, nullptr);
        }

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        }

        vkDestroyDevice(m_device, nullptr);
        // run() calls cleanup() and the destructor calls it again — reset the
        // device so the second pass is a no-op. Without this, the second pass
        // called vkDeviceWaitIdle on the already-destroyed handle
        // ("vkDeviceWaitIdle: Invalid device" from the loader at shutdown).
        m_device = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyInstance(m_instance, nullptr);

        if (m_window) {
            glfwDestroyWindow(m_window);
            glfwTerminate();
        }
    }
}

// ===========================================================================
// File/folder pickers + scene loading (Abrir Jogo / Procurar Pasta).
// ===========================================================================

bool EditorApplication::pick_file_dialog(std::string& outPath, const wchar_t* filter,
                                         const wchar_t* title, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH]{ 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(m_window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

bool EditorApplication::pick_folder_dialog(std::string& outPath, const wchar_t* title) {
    wchar_t buf[MAX_PATH]{ 0 };
    BROWSEINFOW bi{};
    bi.hwndOwner = glfwGetWin32Window(m_window);
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    if (!SHGetPathFromIDListW(pidl, buf)) {
        CoTaskMemFree(pidl);
        return false;
    }
    CoTaskMemFree(pidl);
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

void EditorApplication::load_scene_file(const std::string& path) {
    auto scene = std::make_unique<Scene>("Untitled Scene");
    if (!scene->load_from_file(path)) {
        std::cerr << "[Editor] Falha ao abrir cena: " << path << std::endl;
        return;
    }
    if (m_playMode.get_state() != PlayState::Edit) m_playMode.stop_play();
    m_editorScene = std::move(scene);
    m_playMode.set_editor_scene(m_editorScene.get());
    m_activeScenePath = path;
    m_autosavePath.clear();
    m_sceneDirty = false;
    m_editorGui.init(m_editorScene.get(), &m_undo);
    m_editorGui.set_asset_registry(&m_assetRegistry);
    m_selectedEntity = Entity();
    m_editorGui.select_entity(m_selectedEntity);
    // Give the scene a camera if it lacks one, so the viewport is usable.
    bool hasCamera = false;
    for (const auto& [id, ent] : m_editorScene->get_entities()) {
        if (m_editorScene->cameraComponents.contains(id)) { hasCamera = true; break; }
    }
    if (!hasCamera) {
        Entity cam = m_editorScene->create_entity(tr("Câmera Principal", "Main Camera"));
        m_editorScene->transformComponents[cam.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
        m_editorScene->cameraComponents[cam.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };
    }
    // Terrain is editor-owned (the scene serializer stores entity data only),
    // so the heightmap parameters live in the ".terrain" sidecar next to the
    // scene file and are regenerated on load.
    restore_terrain_sidecar(path);
    std::cout << "[Editor] Cena carregada: " << path << " ("
              << m_editorScene->get_entities().size() << " entidades)" << std::endl;
}

void EditorApplication::scan_projects(std::vector<LauncherProject>& out) const {
    const std::filesystem::path projectsDir =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects";
    std::error_code ec;
    if (!std::filesystem::exists(projectsDir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(projectsDir, ec)) {
        if (!entry.is_directory()) continue;
        LauncherProject proj;
        proj.name = entry.path().filename().string();
        proj.path = entry.path().string();
        // Last write time of the folder tree, best-effort.
        auto ftime = std::filesystem::last_write_time(entry.path(), ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            char buf[64]{ 0 };
            std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", std::localtime(&t));
            proj.lastModified = buf;
        } else {
            proj.lastModified = "—";
        }
        // Does the project contain a .scene anywhere under it?
        std::error_code subEc;
        for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path(), subEc)) {
            if (file.is_regular_file() && file.path().extension() == ".scene") {
                proj.hasScene = true;
                break;
            }
        }
        out.push_back(std::move(proj));
    }
    // Stable, predictable order.
    std::sort(out.begin(), out.end(),
              [](const LauncherProject& a, const LauncherProject& b) { return a.name < b.name; });
}

void EditorApplication::save_current_scene() {
    if (!m_editorScene) return;
    if (!m_activeScenePath.empty()) {
        if (!m_editorScene->save_to_file(m_activeScenePath)) {
            std::cerr << "[Editor] Falha ao salvar: " << m_activeScenePath << std::endl;
        } else {
            m_sceneDirty = false;
            persist_terrain_sidecar(m_activeScenePath);
            std::cout << "[Editor] Cena salva: " << m_activeScenePath << std::endl;
        }
        return;
    }
    // No path yet — behave like Salvar Como.
    save_scene_as();
}

void EditorApplication::save_scene_as() {
    if (!m_editorScene) return;
    std::string path;
    if (!pick_save_file_dialog(path, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0",
                               L"Salvar Cena Como", L"scene")) {
        return;
    }
    if (!m_editorScene->save_to_file(path)) {
        std::cerr << "[Editor] Falha ao salvar: " << path << std::endl;
        return;
    }
    m_activeScenePath = path;
    m_autosavePath.clear();
    m_sceneDirty = false;
    persist_terrain_sidecar(path);
    std::cout << "[Editor] Cena salva: " << path << std::endl;
}

void EditorApplication::create_new_scene() {
    // Stop the play world first so it doesn't keep ticking the old scene.
    if (m_playMode.get_state() != PlayState::Edit) {
        teardown_play_runtime();
        m_playMode.stop_play();
    }
    m_selectedEntity = Entity();
    m_editorGui.select_entity(m_selectedEntity);
    const std::string name = (m_newSceneName[0] != '\0') ? m_newSceneName : "Untitled Scene";
    m_editorScene = std::make_unique<Scene>(name);
    m_playMode.set_editor_scene(m_editorScene.get());
    m_activeScenePath.clear();  // new scene has no file until Salvar
    clear_terrain_mesh();
    init_default_scene();
    std::cout << "[Editor] Nova cena: " << name << std::endl;
}

bool EditorApplication::pick_save_file_dialog(std::string& outPath, const wchar_t* filter,
                                              const wchar_t* title, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH]{ 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(m_window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    // Default folder: the engine's scenes folder (works from any cwd).
    const std::filesystem::path defaultDir =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
    std::error_code ec;
    std::filesystem::create_directories(defaultDir, ec);
    std::wstring initialDir = defaultDir.wstring();
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

// ===========================================================================
// Terreno (Terrain panel): procedural heightmap mesh + static play body.
// ===========================================================================
std::filesystem::path EditorApplication::terrain_sidecar_path(const std::string& scenePath) {
    return std::filesystem::path(scenePath).string() + ".terrain";
}

void EditorApplication::persist_terrain_sidecar(const std::string& scenePath) {
    if (!m_terrainValid || scenePath.empty()) return;
    std::error_code ec;
    const std::filesystem::path path = terrain_sidecar_path(scenePath);
    std::ofstream out(path);
    if (!out) {
        std::cerr << "[Editor] Terrain sidecar write failed: " << path << std::endl;
        return;
    }
    const TerrainParams& p = m_terrainParams;
    out << "scale=" << p.scale << "\n"
        << "octaves=" << p.octaves << "\n"
        << "amount=" << p.amount << "\n"
        << "falloff=" << p.falloff << "\n"
        << "halfExtent=" << p.halfExtent << "\n"
        << "segments=" << p.segments << "\n"
        << "seed=" << p.seed << "\n";
    out.close();
    if (!out) std::cerr << "[Editor] Terrain sidecar write failed (close): " << path << std::endl;
}

void EditorApplication::restore_terrain_sidecar(const std::string& scenePath) {
    if (scenePath.empty()) return;
    const std::filesystem::path path = terrain_sidecar_path(scenePath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        // No sidecar: the scene has no terrain authored yet.
        clear_terrain_mesh();
        return;
    }
    std::ifstream in(path);
    if (!in) return;
    TerrainParams p = m_terrainParams;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        try {
            if (key == "scale") p.scale = std::stof(val);
            else if (key == "octaves") p.octaves = std::stoi(val);
            else if (key == "amount") p.amount = std::stof(val);
            else if (key == "falloff") p.falloff = std::stof(val);
            else if (key == "halfExtent") p.halfExtent = std::stof(val);
            else if (key == "segments") p.segments = std::stoi(val);
            else if (key == "seed") p.seed = static_cast<uint32_t>(std::stoul(val));
        } catch (const std::exception&) {
            // Tolerate a malformed line; keep the previous value.
        }
    }
    generate_terrain_mesh(p);
    std::cout << "[Editor] Terrain restaurado: " << path
              << " (scale=" << p.scale << " seed=" << p.seed << ")" << std::endl;
}

void EditorApplication::clear_terrain_mesh() {
    if (m_terrainVB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainVB); m_terrainVB = GPUBuffer{}; }
    if (m_terrainIB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainIB); m_terrainIB = GPUBuffer{}; }
    m_terrainValid = false;
    m_terrainIndexCount = 0;
    m_terrainParams = TerrainParams{};
}

void EditorApplication::generate_terrain_mesh(const TerrainParams& params) {
    // Drop the previous GPU buffers before regenerating. The old buffers may
    // still be referenced by an in-flight command buffer — freeing them
    // without waiting crashes the device (fence wait failed: -4, then the
    // viewport renders black forever). The editor can afford an idle here:
    // terrain regeneration is a user/API action, never per-frame.
    if (m_terrainVB.buffer != VK_NULL_HANDLE || m_terrainIB.buffer != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);
    if (m_terrainVB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainVB); m_terrainVB = GPUBuffer{}; }
    if (m_terrainIB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainIB); m_terrainIB = GPUBuffer{}; }
    m_terrainValid = false;
    m_terrainParams = params;
    m_terrainIndexCount = 0;

    // Height pass: y = terrain_surface_height(...) with a radial falloff that
    // pulls the border back to 0 so the sheet blends with the infinite grid.
    // The math lives in ONE place (anonymous namespace near
    // setup_play_runtime) because play-mode collision shares it.

    const int segments = params.segments;
    const float half = params.halfExtent;
    const float step = (2.0f * half) / static_cast<float>(segments);

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(static_cast<size_t>(segments + 1) * (segments + 1));

    const size_t cols = static_cast<size_t>(segments + 1);
    for (int zi = 0; zi <= segments; ++zi) {
        for (int xi = 0; xi <= segments; ++xi) {
            const float x = -half + static_cast<float>(xi) * step;
            const float z = -half + static_cast<float>(zi) * step;
            const float h = terrain_surface_height(params.seed, params.scale,
                                                   params.octaves, params.amount,
                                                   params.falloff, half, x, z);
            EditorVertex v;
            v.pos = glm::vec3(x, h, z);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = glm::vec3(0.55f, 0.62f, 0.50f);
            v.uv = glm::vec2((x + half) / (2.0f * half), (z + half) / (2.0f * half));
            verts.push_back(v);
        }
    }
    // Indexed grid: two triangles per cell.
    for (int zi = 0; zi < segments; ++zi) {
        for (int xi = 0; xi < segments; ++xi) {
            const uint32_t a = static_cast<uint32_t>(zi) * static_cast<uint32_t>(cols) + static_cast<uint32_t>(xi);
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(cols);
            const uint32_t d = c + 1;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    // Smooth normals: area-weighted accumulation from the triangle faces.
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3& p0 = verts[indices[i]].pos;
        const glm::vec3& p1 = verts[indices[i + 1]].pos;
        const glm::vec3& p2 = verts[indices[i + 2]].pos;
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        verts[indices[i]].normal += n;
        verts[indices[i + 1]].normal += n;
        verts[indices[i + 2]].normal += n;
    }
    for (EditorVertex& v : verts) {
        const float len = glm::length(v.normal);
        v.normal = len > 1e-8f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // GPU buffers (host-visible, same as the other editor meshes).
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_terrainVB.buffer, m_terrainVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_terrainIB.buffer, m_terrainIB.memory);
    m_terrainVB.size = vbSize;
    m_terrainIB.size = ibSize;
    safe_map_and_copy(m_device, m_terrainVB.memory, 0, vbSize, verts.data());
    safe_map_and_copy(m_device, m_terrainIB.memory, 0, ibSize, indices.data());
    m_terrainIndexCount = static_cast<uint32_t>(indices.size());
    m_terrainValid = true;
    std::cout << "[Editor] Terreno gerado: " << cols * cols << " vértices, "
              << indices.size() / 3 << " triângulos" << std::endl;
}

// ===========================================================================
// Criador de Projetos (Project Creator panel): folder + empty scene on disk.
// ===========================================================================
std::string EditorApplication::create_project(const std::string& name, const std::string& folder) {
    if (name.empty()) return "Erro: nome do projeto vazio.";
    // Sanitize into a folder-safe slug.
    std::string slug = name;
    for (char& c : slug) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    }
    std::filesystem::path root;
    if (!folder.empty()) {
        root = std::filesystem::path(folder) / slug;
    } else {
        root = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects" / slug;
    }
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) return "Erro: a pasta já existe: " + root.string();

    const std::filesystem::path scenesDir = root / "assets" / "scenes";
    std::filesystem::create_directories(scenesDir, ec);
    if (ec) return "Erro: não foi possível criar " + root.string();

    // Fresh scene with the same defaults as the editor (camera + sun).
    Scene scene(name);
    Entity camera = scene.create_entity("Câmera Principal");
    scene.transformComponents[camera.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
    scene.cameraComponents[camera.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };
    Entity sun = scene.create_entity("Luz Direcional");
    scene.lightComponents[sun.get_id()] = LightComponent{ glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
    scene.transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);
    const std::filesystem::path scenePath = scenesDir / "active_world.scene";
    if (!scene.save_to_file(scenePath.string())) {
        return "Erro: falha ao salvar a cena inicial.";
    }

    // Empty asset registry (Content Browser starts clean).
    AssetRegistry reg;
    const std::filesystem::path regPath = root / "Intermediate" / "AssetRegistry.db";
    std::filesystem::create_directories(regPath.parent_path(), ec);
    reg.save(regPath.string());

    // README marker so the folder reads as a project at a glance.
    std::ofstream readme(root / "README.md");
    readme << "# " << name << "\n\nProjeto criado pelo Criador de Projetos do editor.\n";
    readme.close();

    std::cout << "[Editor] Projeto criado: " << root.string() << std::endl;
    return "OK: " + root.string();
}

// ===========================================================================
// Configurações (Opções Gerais / Tema): settings.json persistence.
// ===========================================================================
void EditorApplication::save_settings() {
    if (m_settingsPath.empty()) {
        m_settingsPath = (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "settings.json").string();
    }
    std::ofstream out(m_settingsPath, std::ios::trunc);
    if (!out) {
        std::cerr << "[Editor] Não foi possível salvar " << m_settingsPath << std::endl;
        return;
    }
    const glm::vec3 bg = m_wickedTools.theme_background();
    const glm::vec3 panel = m_wickedTools.theme_panel();
    out << "{\n";
    out << "  \"language\": \"" << (m_currentLanguage == EngineLanguage::PT_BR ? "pt" : "en") << "\",\n";
    out << "  \"vsync\": " << (m_vsyncEnabled ? "true" : "false") << ",\n";
    out << "  \"shadowQuality\": " << m_shadowQuality << ",\n";
    out << "  \"themeBg\": [" << bg.r << ", " << bg.g << ", " << bg.b << "],\n";
    out << "  \"themePanel\": [" << panel.r << ", " << panel.g << ", " << panel.b << "]\n";
    out << "}\n";
    std::cout << "[Editor] Configurações salvas: " << m_settingsPath << std::endl;
}

void EditorApplication::load_settings() {
    m_settingsPath = (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "settings.json").string();
    std::ifstream in(m_settingsPath);
    if (!in) return;
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Tiny hand-rolled key/value reader for our own settings format.
    const auto findString = [&](const std::string& key) -> std::string {
        const std::string token = "\"" + key + "\"";
        const size_t pos = content.find(token);
        if (pos == std::string::npos) return {};
        const size_t colon = content.find(':', pos + token.size());
        if (colon == std::string::npos) return {};
        size_t start = colon + 1;
        while (start < content.size() && std::isspace(static_cast<unsigned char>(content[start]))) ++start;
        if (start >= content.size()) return {};
        if (content[start] == '"') {
            const size_t end = content.find('"', start + 1);
            return end == std::string::npos ? std::string() : content.substr(start + 1, end - start - 1);
        }
        const size_t end = content.find_first_of(",}\n", start);
        std::string val = content.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
        return val;
    };
    const auto findVec = [&](const std::string& key) -> glm::vec3 {
        const std::string token = "\"" + key + "\"";
        const size_t pos = content.find(token);
        if (pos == std::string::npos) return glm::vec3(-1.0f);
        const size_t lb = content.find('[', pos);
        const size_t rb = lb == std::string::npos ? std::string::npos : content.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) return glm::vec3(-1.0f);
        const std::string arr = content.substr(lb + 1, rb - lb - 1);
        glm::vec3 v(0.0f);
        size_t i = 0;
        for (int comp = 0; comp < 3; ++comp) {
            while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',')) ++i;
            const size_t start = i;
            while (i < arr.size() && arr[i] != ',' && arr[i] != ' ') ++i;
            if (i > start) v[static_cast<size_t>(comp)] = std::stof(arr.substr(start, i - start));
        }
        return v;
    };

    const std::string lang = findString("language");
    if (lang == "en") m_currentLanguage = EngineLanguage::EN_US;
    else if (lang == "pt") m_currentLanguage = EngineLanguage::PT_BR;
    const std::string vsync = findString("vsync");
    if (vsync == "false") m_vsyncEnabled = false;
    else if (vsync == "true") m_vsyncEnabled = true;
    const std::string quality = findString("shadowQuality");
    if (!quality.empty()) m_shadowQuality = std::clamp(std::atoi(quality.c_str()), 1, 4);
    // Theme colors are parsed but NOT reapplied on boot: the Forge light
    // design system is the base theme, and the Theme Editor panel tunes the
    // live style during the session (persisting it is a TODO(frontend-port)).
    const glm::vec3 bg = findVec("themeBg");
    const glm::vec3 panel = findVec("themePanel");
    if (bg.x >= 0.0f && panel.x >= 0.0f) {
        m_wickedTools.set_theme(bg, panel);
    }
    std::cout << "[Editor] Configurações carregadas: " << m_settingsPath << std::endl;
}

// ===========================================================================
// Opções Gráficas: VSync (swapchain) + resolução do mapa de sombras.
// ===========================================================================
uint32_t EditorApplication::shadow_size_from_quality(int quality) const {
    switch (std::clamp(quality, 1, 4)) {
        case 1: return 512;
        case 2: return 1024;
        case 3: return 2048;
        default: return 4096;
    }
}

void EditorApplication::apply_graphics_settings(bool vsync, int quality) {
    const bool vsyncChanged = m_vsyncEnabled != vsync;
    m_vsyncEnabled = vsync;
    m_shadowQuality = std::clamp(quality, 1, 4);
    if (vsyncChanged) m_recreateSwapchain = true;
    m_recreateShadowMap = true;
    std::cout << "[Editor] Gráficas: vsync=" << (vsync ? "on" : "off")
              << ", sombras=" << shadow_size_from_quality(m_shadowQuality) << std::endl;
}

// ===========================================================================
// Malha (Mesh panel): recalc/flip normals on the selected entity's mesh asset.
// ===========================================================================
std::string EditorApplication::apply_mesh_normals(int mode) {
    if (!m_selectedEntity.is_valid()) return "Nenhum objeto selecionado.";
    Scene* scene = m_editorScene.get();
    if (!scene) return "Sem cena aberta.";
    const UUID id = m_selectedEntity.get_id();
    const auto meshIt = scene->meshRendererComponents.find(id);
    if (meshIt == scene->meshRendererComponents.end() || !meshIt->second.meshAssetID.is_valid()) {
        return "O objeto selecionado não tem malha.";
    }
    const UUID assetId = meshIt->second.meshAssetID;
    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Mesh || found->cookedPath.empty() ||
        !std::filesystem::is_regular_file(found->cookedPath)) {
        return "Asset de malha não encontrado (o modelo precisa ser importado/cookado).";
    }

    std::string error;
    GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(found->cookedPath, &error);
    if (!geometry.success) return "Falha ao ler a malha: " + error;

    for (GltfMeshPrimitive& prim : geometry.primitives) {
        if (mode == 1) {
            // Flip: negate the existing normals.
            for (glm::vec3& n : prim.normals) n = -n;
            continue;
        }
        // Recalc smooth: area-weighted accumulation per vertex.
        std::vector<glm::vec3> acc(prim.positions.size(), glm::vec3(0.0f));
        if (prim.indexed && prim.indices.size() >= 3) {
            for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
                const uint32_t ia = prim.indices[i], ib = prim.indices[i + 1], ic = prim.indices[i + 2];
                if (ia >= prim.positions.size() || ib >= prim.positions.size() || ic >= prim.positions.size()) continue;
                const glm::vec3 n = glm::cross(prim.positions[ib] - prim.positions[ia],
                                               prim.positions[ic] - prim.positions[ia]);
                acc[ia] += n; acc[ib] += n; acc[ic] += n;
            }
        } else {
            for (size_t i = 0; i + 2 < prim.positions.size(); i += 3) {
                const glm::vec3 n = glm::cross(prim.positions[i + 1] - prim.positions[i],
                                               prim.positions[i + 2] - prim.positions[i]);
                acc[i] += n; acc[i + 1] += n; acc[i + 2] += n;
            }
        }
        prim.normals.resize(prim.positions.size());
        for (size_t i = 0; i < prim.positions.size(); ++i) {
            const float len = glm::length(acc[i]);
            prim.normals[i] = len > 1e-8f ? acc[i] / len : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Re-upload the GPU resource in place (same vertex layout as load_mesh_resource).
    const float meshScale = found->importSettings.meshScale > 0.0f ? found->importSettings.meshScale : 1.0f;
    if (const auto it = m_meshResources.find(assetId); it != m_meshResources.end() && it->second.valid) {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        for (const GltfMeshPrimitive& primitive : geometry.primitives) {
            const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + primitive.positions.size());
            for (size_t i = 0; i < primitive.positions.size(); ++i) {
                EditorVertex v;
                v.pos = primitive.positions[i] * meshScale;
                v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
                v.color = glm::vec3(1.0f);
                v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
                verts.push_back(v);
            }
            if (primitive.indexed) {
                for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
            }
        }
        const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize expected = sizeof(EditorVertex) * it->second.vertexCount;
        if (vbSize == expected && it->second.vb.buffer != VK_NULL_HANDLE) {
            void* data = nullptr;
            safe_map_and_copy(m_device, it->second.vb.memory, 0, vbSize, verts.data());
        }
    }

    // Persist: rewrite the cooked mesh so the change survives a restart.
    if (!GltfGeometryParser::write_cooked(found->cookedPath, geometry, &error)) {
        return std::string(mode == 0 ? "Normais recalculadas (somente em memória): "
                                     : "Normais invertidas (somente em memória): ")
               + error;
    }
    m_meshEdited = true;
    return mode == 0 ? "Normais recalculadas e salvas no cook."
                     : "Normais invertidas e salvas no cook.";
}

} // namespace Engine

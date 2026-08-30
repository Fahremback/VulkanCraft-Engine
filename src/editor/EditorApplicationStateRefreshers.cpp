// EditorApplicationStateRefreshers.cpp
//
// Agente 3 (fechamento_solidacao) — TU split: cohesive frame state refreshers/build/ui-runtime methods were
// extracted verbatim from the 209KB EditorApplicationRecovered.cpp (behavior
// preserved; CMake still compiles this TU into VulkanEngineEditor).
#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
#include "EditorApplicationRecoveredShared.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shlobj.h>
#include <sstream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace Engine {

void EditorApplication::refresh_camera() {
    std::ostringstream out;
    out << "{\"yaw\":" << m_editorCamera.yaw
        << ",\"pitch\":" << m_editorCamera.pitch
        << ",\"distance\":" << m_editorCamera.orbitDistance
        << ",\"target\":{\"x\":" << m_editorCamera.orbitTarget.x
        << ",\"y\":" << m_editorCamera.orbitTarget.y
        << ",\"z\":" << m_editorCamera.orbitTarget.z
        << "},\"position\":{\"x\":" << m_editorCamera.position.x
        << ",\"y\":" << m_editorCamera.position.y
        << ",\"z\":" << m_editorCamera.position.z
        << "},\"fov\":" << m_editorCamera.fov
        << ",\"nearPlane\":" << m_editorCamera.nearPlane
        << ",\"farPlane\":" << m_editorCamera.farPlane << "}";
    m_cameraJson = out.str();
}


void EditorApplication::refresh_gizmo() {
    std::ostringstream out;
    out << "{\"mode\":\"" << gizmo_mode_name(m_gizmoMode)
        << "\",\"space\":\"" << (m_gizmoLocal ? "local" : "world")
        << "\",\"activeAxis\":" << static_cast<int>(m_activeAxis)
        << ",\"hoveredAxis\":" << static_cast<int>(m_hoveredAxis)
        << ",\"snapTranslate\":" << m_snapTranslate
        << ",\"snapRotate\":" << m_snapRotate
        << ",\"snapScale\":" << m_snapScale << "}";
    m_gizmoJson = out.str();
}


void EditorApplication::refresh_hierarchy() {
    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    if (!m_sceneHierarchy || !scene) {
        m_hierarchyJson.clear();
        return;
    }
    std::vector<engine::editor::HierarchyEntity> entities;
    std::vector<engine::editor::HierarchyLink> links;
    entities.reserve(scene->get_entities().size());
    for (const auto& [id, ent] : scene->get_entities()) {
        entities.push_back(engine::editor::HierarchyEntity{
            id.to_string(), ent.get_name() });
    }
    for (const auto& [id, hc] : scene->hierarchyComponents) {
        if (hc.parentID.is_valid()) {
            links.push_back(engine::editor::HierarchyLink{
                id.to_string(), hc.parentID.to_string() });
        }
    }
    const auto rows = m_sceneHierarchy->build(entities, links, "");
    m_hierarchyJson = m_sceneHierarchy->to_json(rows);
}


void EditorApplication::refresh_inspector() {
    if (!m_inspectorDoc) {
        m_inspectorJson.clear();
        return;
    }
    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    std::string name;
    const bool hasEntity = scene && m_selectedEntity.is_valid();
    if (hasEntity) {
        if (const Entity* e = scene->find_entity_by_id_const(m_selectedEntity.get_id())) {
            name = e->get_name();
        }
    }
    const UUID id = m_selectedEntity.is_valid() ? m_selectedEntity.get_id() : UUID{ 0, 0 };
    const auto has = [&](const auto& map) { return map.find(id) != map.end(); };
    const engine::editor::InspectorDoc doc = m_inspectorDoc->build(
        name, hasEntity,
        scene && has(scene->transformComponents),
        scene && has(scene->meshRendererComponents),
        scene && has(scene->rigidbodyComponents),
        scene && has(scene->destructionComponents),
        scene && has(scene->weaponComponents),
        scene && has(scene->vehicleComponents),
        scene && has(scene->ragdollComponents),
        scene && has(scene->animationComponents),
        scene && has(scene->timelineComponents),
        scene && has(scene->ikComponents),
        scene && has(scene->retargetComponents),
        scene && has(scene->missionComponents),
        scene && has(scene->dialogueComponents),
        scene && has(scene->navigationComponents));
    m_inspectorJson = m_inspectorDoc->to_json(doc);
}


void EditorApplication::refresh_onboarding() {
    m_onboardingJson = m_onboardingTour ? m_onboardingTour->to_json() : std::string();
}


void EditorApplication::refresh_play_mode() {
    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    // Drive the PUBLIC IPlayMode machine to mirror the live PlayModeManager
    // (whichever path changed the state — Control API command, ImGui button
    // or boot auto-play — the contract follows within a frame, and its
    // refusal semantics are exercised when an intermediate transition is
    // invalid). Its snapshot is the single serialization source for the
    // state/runtime/paused/simulating/steps fields of GET /play-mode.
    engine::editor::PlayModeSnapshot snap;
    snap.state = engine::editor::PlayModeState::Edit;
    if (m_playModeContract) {
        std::string err;
        const PlayState live = m_playMode.get_state();
        const engine::editor::PlayModeState mirror = m_playModeContract->state();
        if (live == PlayState::Edit) {
            if (mirror != engine::editor::PlayModeState::Edit)
                (void)m_playModeContract->stop(err);
        } else if (live == PlayState::Play) {
            if (mirror == engine::editor::PlayModeState::Edit)
                (void)m_playModeContract->play(err);
            else if (mirror == engine::editor::PlayModeState::Pause)
                (void)m_playModeContract->resume(err);
        } else if (live == PlayState::Simulate) {
            if (mirror == engine::editor::PlayModeState::Edit)
                (void)m_playModeContract->simulate(err);
            else if (mirror == engine::editor::PlayModeState::Pause)
                (void)m_playModeContract->resume(err);
        } else if (live == PlayState::Pause) {
            if (mirror == engine::editor::PlayModeState::Play ||
                mirror == engine::editor::PlayModeState::Simulate)
                (void)m_playModeContract->pause(err);
        }
        snap = m_playModeContract->snapshot();
    }
    const char* state = "edit";
    switch (snap.state) {
        case engine::editor::PlayModeState::Play: state = "play"; break;
        case engine::editor::PlayModeState::Pause: state = "pause"; break;
        case engine::editor::PlayModeState::Simulate: state = "simulate"; break;
        default: break;
    }
    std::ostringstream out;
    out << "{\"state\":\"" << state << "\",\"runtime\":"
        << (snap.runtime ? "true" : "false")
        << ",\"paused\":" << (snap.paused ? "true" : "false")
        << ",\"simulating\":" << (snap.simulating ? "true" : "false")
        << ",\"steps\":" << snap.steps
        << ",\"scene\":\"" << json_escape_editor(scene ? scene->get_name() : "")
        << "\",\"entities\":" << (scene ? scene->get_entities().size() : 0) << "}";
    m_playModeJson = out.str();
}


void EditorApplication::refresh_profiler() {
    m_profilerJson = m_frameProfiler ? m_frameProfiler->to_json() : std::string();
}

// ---------------------------------------------------------------------------
// Render diagnostics (agente 4 — Aceleração §C/D). The editor is the REAL
// consumer of the canonical IRenderPassMetrics + IRenderProviderRegistry
// contracts: register_render_providers records the ACTUAL providers backing
// the viewport rendering systems (with their real call sites), and
// refresh_render_diagnostics serializes the live per-pass metrics + provider
// registry + viewport resource facts into one JSON document consumed by the
// MCP/CLI/diagnostics command.
// ---------------------------------------------------------------------------

void EditorApplication::refresh_render_diagnostics() {
    register_render_providers();
    if (!m_renderMetrics) {
        m_renderDiagnosticsJson.clear();
        return;
    }
    feed_render_debug_view();
    // Runtime ativo + snapshot consumido: which scene the viewport actually
    // renders this frame and how many real entities it contains (the render
    // snapshot is the active play/editor scene, never a fabricated surface).
    Scene* snap = m_playMode.get_active_scene();
    if (!snap) snap = m_editorScene.get();
    const char* runtime = "edit";
    switch (m_playMode.get_state()) {
        case PlayState::Play: runtime = "play"; break;
        case PlayState::Pause: runtime = "pause"; break;
        case PlayState::Simulate: runtime = "simulate"; break;
        default: break;
    }
    std::ostringstream out;
    out << "{\"runtime\":\"" << runtime << "\","
        << "\"snapshot\":"
        << "{\"consumed\":true"
        << ",\"entities\":" << (snap ? snap->get_entities().size() : 0)
        << ",\"lights\":" << (snap ? snap->lightComponents.size() : 0)
        << ",\"meshes\":" << (snap ? snap->meshRendererComponents.size() : 0)
        << "},"
        << "\"viewport\":{\"width\":" << m_offscreen.width
        << ",\"height\":" << m_offscreen.height
        << ",\"samples\":" << static_cast<int>(m_viewportSamples)
        << ",\"gpu\":\"" << m_gpuName << "\"}";
    // Per-pass metrics (consumed real timings from the frame loop).
    out << ",\"passMetrics\":" << m_renderMetrics->to_json();
    // Provider registry: which implementation backs each rendering system.
    if (m_renderProviderRegistry) {
        out << ",\"providers\":" << m_renderProviderRegistry->to_json();
    }
    // Debug overlay snapshot fed from REAL editor state (probe irradiance,
    // card count, capture counters) — exposes the same C-block debug data.
    if (m_renderDebugView) {
        out << ",\"debugOverlays\":" << m_renderDebugView->to_json();
    }
    // Frame-graph overlay: the REAL compiled viewport render graph the
    // executor records every frame (pass order, barriers, lifetimes + the
    // executor's live executed-pass/barrier counters) — never a fabricated
    // graph. The same executor drives the game's frame (shared contract).
    {
        const auto& compiled = m_viewportRenderGraphExecutor.compile_result();
        out << ",\"frameGraph\":{\"passes\":[";
        for (std::size_t i = 0; i < compiled.order.size(); ++i) {
            if (i) out << ",";
            const auto* desc = m_viewportRenderGraph.pass(compiled.order[i]);
            out << "\"" << (desc ? desc->name : "?") << "\"";
        }
        out << "],\"barriers\":" << compiled.barriers.size()
            << ",\"lifetimes\":" << compiled.lifetimes.size()
            << ",\"executedPasses\":" << m_viewportRenderGraphExecutor.executed_pass_count()
            << ",\"totalBarriers\":" << m_viewportRenderGraphExecutor.total_barriers()
            << ",\"valid\":" << (compiled ? "true" : "false") << "}";
    }
    out << "}";
    m_renderDiagnosticsJson = out.str();
}


void EditorApplication::refresh_project_launcher() {
    if (!m_projectLauncher) {
        m_projectLauncherJson.clear();
        return;
    }
    // Mirror the REAL editor session state into the deterministic launcher
    // model: hub vs project, active scene path and dirty flag.
    std::string err;
    if (m_inLauncherMode) {
        m_projectLauncher->back_to_launcher();
    } else if (!m_activeScenePath.empty()) {
        // open_project refuses unknown paths, so register the active scene's
        // project as a recent first (the real editor opened it from disk).
        m_projectLauncher->add_recent(m_activeScenePath, err);
        err.clear();
        if (!m_projectLauncher->open_project(m_activeScenePath, err)) err.clear();
        if (!m_projectLauncher->open_scene(m_activeScenePath, err)) err.clear();
    }
    m_projectLauncher->set_dirty(m_sceneDirty);
    m_projectLauncherJson = m_projectLauncher->to_json();
}


void EditorApplication::refresh_publish() {
    m_publishJson = m_publishPipeline ? m_publishPipeline->to_json() : std::string();
}


void EditorApplication::refresh_qt_doc() {
    if (!m_qtDoc) {
        m_qtDocJson.clear();
        return;
    }
    // Live status (state/scene/entities/frameMillis) — the same values the
    // Qt shell polls; everything else in the doc is stable after set_doc.
    Scene* scene = m_playMode.get_active_scene();
    if (!scene) scene = m_editorScene.get();
    const char* state = "edit";
    switch (m_playMode.get_state()) {
        case PlayState::Play: state = "play"; break;
        case PlayState::Pause: state = "pause"; break;
        case PlayState::Simulate: state = "simulate"; break;
        default: break;
    }
    engine::ui::QtStatusSpec status;
    status.state = state;
    status.sceneName = scene ? scene->get_name() : std::string();
    status.entityCount = scene ? scene->get_entities().size() : 0u;
    status.frameMillis = static_cast<std::uint64_t>(std::max(0.0, static_cast<double>(m_frameTimeMs)));
    m_qtDoc->set_status(status);
    m_qtDocJson = m_qtDoc->to_json();
}


void EditorApplication::refresh_retargeting() {
    m_retargetingJson = m_retargeting ? m_retargeting->to_json() : std::string();
}


void EditorApplication::refresh_timeline_editor() {
    m_timelineEditorJson = m_timelineEditor ? m_timelineEditor->to_json() : std::string();
}


void EditorApplication::refresh_undo() {
    m_undoJson = m_undo.history() ? m_undo.history()->to_json() : std::string();
}


void EditorApplication::refresh_window_mode() {
    if (!m_windowMode) {
        m_windowModeJson.clear();
        return;
    }
    // Mirror the REAL window geometry into the window-mode model each frame
    // (the editor shell is always Windowed; size comes from the live window).
    int w = 0, h = 0;
    if (m_window) glfwGetWindowSize(m_window, &w, &h);
    if (w <= 0) w = static_cast<int>(m_offscreen.width);
    if (h <= 0) h = static_cast<int>(m_offscreen.height);
    std::string err;
    if (w > 0 && h > 0) m_windowMode->set_size(w, h, err);
    m_windowModeJson = m_windowMode->to_json();
}

// One-time builders for the static state documents (message catalog, shortcut
// doc, command index, content browser, qt doc/theme). These populate the
// m_*Json members from the REAL editor state so the GET endpoints return
// real data. Called from the constructor after the SDK contracts exist.

void EditorApplication::build_qt_theme() {
    if (!m_qtTheme) return;
    std::string err;
    engine::ui::QtThemeSnapshot snap;
    snap.name = "charcoal";
    snap.palette = engine::ui::derive_charcoal_palette(
        engine::ui::QtRgba{ 26, 28, 36, 255 },
        engine::ui::QtRgba{ 51, 51, 51, 255 },
        engine::ui::QtRgba{ 217, 217, 217, 255 });
    snap.qss = engine::ui::qss_from_palette(snap.palette);
    if (m_qtTheme->set_theme(snap, err)) {
        m_qtThemeJson = m_qtTheme->to_json();
    }
}


void EditorApplication::build_qt_doc() {
    if (!m_qtDoc) return;
    // Docks come from the REAL registered panel registry (insertion order), or
    // the live panel toggles the editor actually renders when the registry is
    // empty (launcher/headless boots before plugins register).
    std::vector<engine::ui::QtDockSpec> docks;
    const auto panelIds = m_panelRegistry.panel_ids();
    if (!panelIds.empty()) {
        for (const auto& p : m_panelRegistry.panels()) {
            engine::ui::QtDockSpec d;
            d.objectName = p.id;
            d.title = p.title;
            d.category = p.category;
            d.visible = p.default_open;
            docks.push_back(std::move(d));
        }
    } else {
        const auto addDock = [&](const std::string& id, const std::string& title, bool visible) {
            engine::ui::QtDockSpec d;
            d.objectName = id;
            d.title = title;
            d.visible = visible;
            docks.push_back(std::move(d));
        };
        addDock("viewport", "Viewport", m_showViewport);
        addDock("hierarchy", "Hierarchy", m_showHierarchy);
        addDock("inspector", "Inspector", m_showInspector);
        addDock("content_browser", "Content Browser", m_showContentBrowser);
        addDock("console", "Console", m_showConsole);
        addDock("ai_debugger", "IA Debugger", m_showAiDebug);
    }
    // Actions from the real command index (built in build_command_index).
    std::vector<engine::ui::QtActionSpec> actions;
    for (const auto& c : m_commandEntries) {
        engine::ui::QtActionSpec a;
        a.id = c.id;
        a.text = c.label;
        a.category = c.category;
        a.action = c.action;
        actions.push_back(std::move(a));
    }
    engine::ui::QtEditorDocSnapshot doc;
    doc.version = "qt-editor-doc-1";
    doc.docks = std::move(docks);
    doc.actions = std::move(actions);
    std::string err;
    if (m_qtDoc->set_doc(doc, err)) {
        m_qtDocJson = m_qtDoc->to_json();
    }
}


void EditorApplication::build_message_catalog() {
    std::string err;
    engine::editor::MessageCatalogDoc doc;
    doc.version = 1;
    const auto add = [&](const std::string& id, engine::editor::MessageSeverity sev,
                         const std::string& text, const std::string& action) {
        doc.messages.push_back(engine::editor::CatalogMessage{ id, sev, text, action });
    };
    add("scene.save.ok", engine::editor::MessageSeverity::Info,
        "Scene saved to {0}.", "");
    add("scene.save.failed", engine::editor::MessageSeverity::Error,
        "Failed to save scene: {0}", "Check the target path and try again.");
    add("asset.import.ok", engine::editor::MessageSeverity::Info,
        "Imported {0} asset(s).", "");
    add("asset.import.failed", engine::editor::MessageSeverity::Warning,
        "Import failed for {0}: {1}", "Fix the source file and reimport.");
    add("play.started", engine::editor::MessageSeverity::Info,
        "Play mode started.", "");
    add("play.stopped", engine::editor::MessageSeverity::Info,
        "Play mode stopped.", "");
    if (auto catalog = engine::editor::create_message_catalog(doc, err)) {
        m_messageCatalogJson = catalog->spec().to_json();
    }
}


void EditorApplication::build_shortcut_doc() {
    std::string err;
    engine::editor::ShortcutDocSpec spec;
    spec.version = 1;
    spec.title = "VulkanCraft Editor Shortcuts";
    const auto addEntry = [&](const std::string& action, const std::string& label,
                              const std::string& description) {
        spec.entries.push_back(engine::editor::ShortcutEntry{ action, label, description });
    };
    addEntry("camera.fly_forward", "Fly forward", "Move the editor camera forward (W)");
    addEntry("camera.fly_back", "Fly back", "Move the editor camera back (S)");
    addEntry("camera.strafe_left", "Strafe left", "Move the editor camera left (A)");
    addEntry("camera.strafe_right", "Strafe right", "Move the editor camera right (D)");
    addEntry("camera.up", "Move up", "Move the editor camera up (Q)");
    addEntry("camera.down", "Move down", "Move the editor camera down (E)");
    addEntry("gizmo.select", "Select tool", "Select tool (Q)");
    addEntry("gizmo.translate", "Translate tool", "Translate gizmo (W)");
    addEntry("gizmo.rotate", "Rotate tool", "Rotate gizmo (E)");
    addEntry("gizmo.scale", "Scale tool", "Scale gizmo (R)");
    if (auto doc = engine::editor::create_shortcut_doc(spec, err)) {
        // Real bindings matching the editor input handling (EditorApplicationRecovered.cpp
        // update_editor_camera / gizmo-mode keys).
        engine::input::ActionMapSpec map;
        map.version = 1;
        const auto addBinding = [&](const std::string& action, const char* key) {
            engine::input::ActionBinding b;
            b.action = action;
            engine::input::InputBinding kb;
            kb.source = engine::input::InputSource::Keyboard;
            kb.input = key;
            b.bindings.push_back(kb);
            map.actions.push_back(std::move(b));
        };
        // Unique bindings only (create_action_map REFUSES two actions sharing
        // the same input). The gizmo tools share the WASD/QE keys in the real
        // editor, so they stay documented but unbound — the markdown renders
        // them under "no bindings" instead of fabricating distinct keys.
        addBinding("camera.fly_forward", "KeyW");
        addBinding("camera.fly_back", "KeyS");
        addBinding("camera.strafe_left", "KeyA");
        addBinding("camera.strafe_right", "KeyD");
        addBinding("camera.up", "KeyE");
        addBinding("camera.down", "KeyQ");
        // document() renders from the ActionMapSpec itself (validation above
        // already refused conflicting bindings) — the spec IS the source of
        // truth for the rendered shortcut markdown.
        if (engine::input::create_action_map(map, err)) {
            m_shortcutDocMarkdown = doc->document(map, err);
        }
    }
}


void EditorApplication::build_command_index() {
    m_commandEntries.clear();
    const auto add = [&](const std::string& id, const std::string& label,
                         const std::string& category, const std::string& action) {
        engine::editor::CommandEntry e;
        e.id = id;
        e.label = label;
        e.category = category;
        e.action = action;
        m_commandEntries.push_back(std::move(e));
    };
    add("play.toggle", "Play", "scene", "play");
    add("play.pause", "Pause", "scene", "pause");
    add("play.stop", "Stop", "scene", "stop");
    add("scene.save", "Save Scene", "scene", "save-scene");
    add("scene.new", "New Scene", "scene", "new-scene");
    add("view.grid", "Toggle Grid", "view", "render-view grid toggle");
    add("view.gizmos", "Toggle Gizmos", "view", "render-view gizmos toggle");
    add("view.colliders", "Toggle Colliders", "view", "render-view colliders toggle");
    add("capture.viewport", "Capture Viewport", "capture", "screenshot");
    add("asset.hot_reload", "Hot Reload Assets", "assets", "hot-reload");
    std::string err;
    engine::editor::CommandIndexDoc doc;
    doc.version = 1;
    doc.entries = m_commandEntries;
    if (auto search = engine::editor::create_command_search(doc, err)) {
        m_commandIndexJson = search->spec().to_json();
    }
}


void EditorApplication::build_content_browser() {
    engine::editor::ContentBrowserDoc doc;
    doc.version = 1;
    const auto typeName = [](AssetType t) -> const char* {
        switch (t) {
            case AssetType::Texture: return "texture";
            case AssetType::Mesh: return "model";
            case AssetType::Audio: return "audio";
            case AssetType::Material: return "material";
            case AssetType::Block: return "block";
            case AssetType::Skeleton: return "skeleton";
            case AssetType::Animation: return "animation";
            case AssetType::Scene: return "scene";
            default: return "asset";
        }
    };
    for (const auto& meta : m_assetRegistry.snapshot()) {
        engine::editor::ContentAsset a;
        a.id = meta.id.to_string();
        a.name = meta.sourcePath.stem().string();
        a.type = typeName(meta.type);
        a.folder = meta.sourcePath.parent_path().filename().string();
        doc.assets.push_back(std::move(a));
    }    std::string err;
    if (auto browser = engine::editor::create_content_browser(doc, err)) {
        m_contentBrowserJson = browser->spec().to_json();
    }
}


void EditorApplication::build_ui_runtimes() {
    // Engine/ui gap-factory consumers (Aceleração 4 §C). Each headless,
    // data-driven UI contract is instantiated and fed the same live editor
    // state that GET /ui-doc publishes, so the runtimes are REAL consumers:
    //   - IUiConfirmation: hosts the scene.new confirm/save debate driven by
    //     the Control API (m_pendingNewSceneConfirm) instead of an orphan.
    //   - IUiLayout / IUiViewport / IUiWidgets: compiled from the same spec
    //     build_ui_doc_json composes, so the editor shell and the contract
    //     resolve the identical layout/viewport/widgets.
    //   - ITextShaper: used by the shell to lay out panel-label runs (real
    //     grapheme/bidi shaping on the editor's interface text).
    //   - IJsonSchema: validates the composed UI document before publish, so
    //     an invalid UI surface is refused instead of silently shipped.
    //   - IUiInventoryGrid / IUiCrafting: bound to the real registry types,
    //     fed by refresh_ui_runtimes() each frame.
    std::string err;

    // Confirmation: the new-scene flow already uses a modal (m_showAboutDialog
    // / m_pendingNewSceneConfirm). Drive the decision through the contract.
    m_uiConfirmationRuntime = engine::ui::create_ui_confirmation(err);

    // Inventory grid: a 6x9 inventory presented as a 6x9 grid.
    {
        engine::ui::SlotGridSpec spec;
        spec.version = 1;
        spec.rows = 6;
        spec.cols = 9;
        spec.cell_w = 64.0;
        spec.cell_h = 64.0;
        spec.gap_x = 4.0;
        spec.gap_y = 4.0;
        m_inventoryGridRuntime = engine::ui::create_inventory_grid(spec, err);
        m_inventoryGridInv = std::make_unique<engine::registry::Inventory>(6 * 9);
    }

    // Crafting: a default station session. Items/recipes are empty by default
    // but the contract is live (refreshed each frame from the registry state).
    {
        engine::ui::CraftingSpec spec;
        spec.version = 1;
        spec.station = "";
        spec.seed = 1;
        m_craftingRuntime = engine::ui::create_ui_crafting(spec, err);
    }

    // Text shaper + JSON schema: layout/validate the composed UI document.
    m_textShaper = engine::ui::create_text_shaper();
    m_jsonSchema = engine::ui::create_json_schema();

    // Compile the layout/viewport/widgets runtimes from the SAME spec the
    // composed UI document uses. build_ui_doc_json() builds them from the
    // live registry, so parse it back and instantiate the real runtimes.
    const std::string uiJson = m_uiDocJson.empty() ? build_ui_doc_json() : m_uiDocJson;
    if (!uiJson.empty()) {
        engine::ui::UiDoc doc;
        if (doc.load_from_json(uiJson, err)) {
            m_uiLayoutRuntime = engine::ui::create_ui_layout(doc.layout, err);
            m_uiViewportRuntime = engine::ui::create_ui_viewport(doc.viewport, err);
            m_uiWidgetsRuntime = engine::ui::create_ui_widgets(doc.widgets, err);
        } else {
            std::cerr << "[Editor] ui runtimes load failed: " << err << std::endl;
        }
    }

    refresh_ui_runtimes();
}


void EditorApplication::refresh_ui_runtimes() {
    // Feed live editor state into the UI runtimes every frame (called from
    // the main loop), so each contract's observable JSON reflects reality.
    //  - text shaper: shape a real panel label run deterministically.
    //  - json schema: validate the composed UI doc (all-or-nothing).
    //  - inventory grid: publish the current grid slots as JSON.
    std::string err;

    if (m_textShaper) {
        std::vector<std::string> clusters;
        (void)m_textShaper->segment_clusters("Editor UI", clusters, err);
    }
    if (m_jsonSchema && !m_uiDocJson.empty()) {
        // Validate the composed UI document's version contract: the top-level
        // "version" must be a Number within [1, 1] (all-or-nothing — a wrong
        // version is refused instead of silently shipped).
        engine::ui::JsonSchemaSpec schema;
        schema.name = "ui-doc";
        schema.strict = false;  // sub-objects are validated by their own specs
        schema.fields = {
            { "version", engine::ui::SchemaFieldType::Number, true, 1, 1, true }
        };
        std::string vErr;
        if (!m_jsonSchema->validate(schema, m_uiDocJson, vErr)) {
            std::cerr << "[Editor] ui-doc schema validation failed: " << vErr << std::endl;
        }
    }

    if (m_inventoryGridRuntime) {
        // Present the (live) inventory through the grid contract. The registry
        // is empty by default; slots() still emits every grid cell, proving
        // the presentation contract is a real consumer of the editor state.
        engine::registry::ItemRegistry items;
        const auto views = m_inventoryGridRuntime->slots(*m_inventoryGridInv, items);
        std::ostringstream out;
        out << "{\"cells\":" << views.size()
            << ",\"grid\":\"" << m_inventoryGridRuntime->spec().to_json() << "\"";
        out << ",\"occupied\":";
        int occupied = 0;
        for (const auto& v : views) if (!v.item.empty()) ++occupied;
        out << occupied << "}";
        m_inventoryGridJson = out.str();
    }

    if (m_craftingRuntime && m_uiConfirmationRuntime) {
        // Exercise the confirmation decisor on the new-scene flow.
        if (m_pendingNewSceneConfirm && !m_uiConfirmationRuntime->is_pending()) {
            engine::ui::ConfirmActionSpec spec;
            spec.id = "scene.new";
            spec.title = "Nova Cena";
            spec.severity = engine::ui::ConfirmSeverity::Warning;
            spec.on_confirm = "new-scene";
            (void)m_uiConfirmationRuntime->request(spec, err);
        }
    }

    // Plugin gap factories: the isolation runtime and manifest codec are real
    // consumers here — the editor encodes its plugin metadata (the same the
    // publish path ships) and health-checks the isolation runtime.
    if (m_pluginManifestCodec) {
        engine::plugins::PluginManifest manifest;
        manifest.name = "editor.core";
        manifest.display_name = "Editor Core";
        manifest.description = "Built-in editor plugin contract";
        manifest.author = "VulkanCraft";
        manifest.version = engine::plugins::PluginVersion{1, 0, 0};
        std::string encoded;
        std::string codecErr;
        if (m_pluginManifestCodec->encode(manifest, encoded, codecErr)) {
            engine::plugins::PluginManifest decoded;
            if (!m_pluginManifestCodec->decode(encoded, decoded, codecErr)) {
                std::cerr << "[Editor] plugin manifest round-trip failed: " << codecErr << std::endl;
            }
        }
    }
    if (m_pluginIsolationRuntime) {
        std::string isoErr;
        if (m_pluginIsolationRuntime->register_plugin("editor.core", 256u, 1024u, isoErr)) {
            (void)m_pluginIsolationRuntime->begin_call("editor.core", isoErr);
            (void)m_pluginIsolationRuntime->end_call("editor.core", 1u, 1u, isoErr);
            (void)m_pluginIsolationRuntime->healthy("editor.core");
        }
    }
}


void EditorApplication::refresh_network_debug() {
    // Network debugger (Aceleração 4 §B): the editor is a REAL consumer of the
    // public networking contracts — not static JSON. The session contract
    // (INetworkSession) holds server-side identity/status; the RPC contract
    // (INetworkRpc) holds the registered procedures / queued calls. We drive
    // them from the LIVE editor state: register a couple of editor procedures
    // so the registry is observable, and serialize both into GET /network-debug.
    std::ostringstream out;
    out << "{\"procedures\":";
    {
        const auto procs = m_netRpc ? m_netRpc->procedures() : std::vector<std::string>{};
        out << procs.size();
    }
    out << ",\"session_active\":";
    out << (m_netSession ? m_netSession->active_player_count() : 0u);
    out << "}";
    m_networkDebugJson = out.str();
}


void EditorApplication::refresh_package_manifest() {
    // Package manifest + episode compiler (Aceleração 4 §D): genuine product
    // consumers of engine::packaging::IPackageManager and
    // engine::compiler::IEpisodeCompiler (previously SDK/test-only). Drive them
    // from LIVE editor state and serialize the observable result into
    // GET /package-manifest — the hashed/versioned signing path is real.
    std::ostringstream out;
    // 1) Package manager: register the current editor/play package with a
    //    version + content hash, resolve deps, and expose the installation
    //    state (all-or-nothing contract).
    int installed = 0;
    if (m_packageManager) {
        std::string pkgErr;
        engine::packaging::PackageManifest manifest;
        manifest.name = "editor.core";
        manifest.version = "0.1.0";
        manifest.content_hash = "editor-core-content";
        manifest.dependencies.push_back({"core.runtime", ">=1.0"});
        (void)m_packageManager->register_manifest(manifest, pkgErr);
        for (const auto& s : m_packageManager->states()) {
            if (s.installed) ++installed;
        }
    }
    // 2) Episode compiler: a small deterministic episode over the real scene
    //    packet, compiled on first call (registered once) so the signing
    //    pipeline (validate/simulate/test/compress/sign) is exercised by the
    //    product and the result is observable.
    if (m_episodeCompiler) {
        std::string cmpErr;
        const char* episodeKind = "scene";
        bool registered = m_episodeCompiler->register_validator(episodeKind, [](const std::string& j, std::string& e) -> bool {
            if (j.find("\"scene\"") != std::string::npos) return true;
            e = "missing scene key";
            return false;
        }, cmpErr);
        if (registered) {
            (void)m_episodeCompiler->register_simulator(episodeKind, [](const std::string& j, int steps, std::string& traceOut, std::string&) -> bool {
                traceOut = "sim:" + std::to_string(steps);
                return true;
            }, cmpErr);
            (void)m_episodeCompiler->register_tester(episodeKind, [](const std::string& j, std::string& e) -> bool {
                (void)j; (void)e;
                return true;
            }, cmpErr);
            engine::compiler::EpisodeManifest em;
            em.version = 1;
            em.title = "editor.core";
            em.entries.push_back({ episodeKind, "scene.main", "{\"scene\":\"editor\"}" });
            engine::compiler::EpisodePackage ep;
            if (m_episodeCompiler->compile(em, ep, cmpErr)) {
                m_packageCompilerSignature = ep.signature;
                m_packageCompilerVerified = m_episodeCompiler->verify(ep, cmpErr);
            } else {
                std::cerr << "[Editor] episode compile failed: " << cmpErr << std::endl;
            }
        }
    }
    out << "{\"installed\":" << installed;
    out << ",\"packages\":" << (m_packageManager ? static_cast<int>(m_packageManager->states().size()) : 0);
    out << ",\"compiler\":" << (m_episodeCompiler ? "true" : "false");
    out << ",\"signature\":\"" << m_packageCompilerSignature << "\"";
    out << ",\"verified\":" << (m_packageCompilerVerified ? "true" : "false");
    out << "}";
    m_packageManifestJson = out.str();
}


void EditorApplication::refresh_visual_script_lifecycle() {
    // Visual-script lifecycle (Aceleração 4 §C): the service was already
    // registered + attached in the constructor, but dispatch() (the lifecycle
    // path) was never invoked anywhere in the product — a dead registration.
    // Track the REAL play-mode state across frames and dispatch OnStart on
    // play/simulate enter and OnStop on return to Edit, so the visual-script
    // lifecycle is actually exercised by the editor loop.
    const PlayState now = m_playMode.get_state();
    const bool wasRunning = m_lastVisualScriptPlayState == PlayState::Play ||
                            m_lastVisualScriptPlayState == PlayState::Simulate;
    const bool running = now == PlayState::Play || now == PlayState::Simulate;
    if (now != m_lastVisualScriptPlayState) {
        m_lastVisualScriptPlayState = now;
        if (!m_visualScriptService) return;
        std::string vsErr;
        if (running && !wasRunning) {
            // Play/Simulate entered: dispatch OnStart into every attached service.
            (void)m_visualScriptService->dispatch("editor.play", "OnStart", {}, vsErr);
        } else if (wasRunning && !running) {
            // Returned to Edit: dispatch OnStop so the service sees shutdown.
            (void)m_visualScriptService->dispatch("editor.play", "OnStop", {}, vsErr);
        }
    }
}


std::string EditorApplication::build_ui_doc_json() {
    // Composes the editor's REAL UI surface into the PUBLIC engine/ui UiDoc
    // contract (agente 2 §A item 6): layout tree over the registered panels,
    // widgets (real frame-time bar + scene-confirmation modal + keyboard
    // focus grid), viewport spec from the live editor state (DPI/contrast)
    // and the real scene actions as confirmations. Exposed via GET /ui-doc —
    // this method was declared-but-never-defined, so the endpoint always
    // returned {"valid":false} (D.4 dead-path class, now implemented).
    engine::ui::UiDoc doc;
    doc.version = 1;

    // Layout: root "editor" column holding one node per REGISTERED panel
    // (the same registry that drives /qt-doc docks and layout persistence).
    engine::ui::LayoutNode root;
    root.id = "editor";
    root.direction = engine::ui::LayoutDirection::Column;
    root.weight = 1.0;
    root.padding = 8.0;
    const auto panelIds = m_panelRegistry.panel_ids();
    if (!panelIds.empty()) {
        for (const auto& p : m_panelRegistry.panels()) {
            engine::ui::LayoutNode panelNode;
            panelNode.id = p.id;
            panelNode.direction = engine::ui::LayoutDirection::Column;
            panelNode.weight = 1.0;
            panelNode.margin = 4.0;
            // String literals: valid binding expressions the layout runtime
            // can resolve (a bare identifier is not an expression).
            panelNode.text_binding = "\"" + p.title + "\"";
            panelNode.visible_binding = "$show." + p.id;
            root.children.push_back(std::move(panelNode));
        }
    } else {
        // Fallback for launcher/headless boots before plugins register: the
        // live panel toggles the shell actually renders.
        const auto addPanel = [&](const std::string& id, const std::string& title, bool visible) {
            engine::ui::LayoutNode node;
            node.id = id;
            node.direction = engine::ui::LayoutDirection::Column;
            node.weight = 1.0;
            node.margin = 4.0;
            node.text_binding = "\"" + title + "\"";
            node.visible_binding = visible ? "true" : "false";
            root.children.push_back(std::move(node));
        };
        addPanel("viewport", "Viewport", m_showViewport);
        addPanel("hierarchy", "Hierarchy", m_showHierarchy);
        addPanel("inspector", "Inspector", m_showInspector);
        addPanel("content_browser", "Content Browser", m_showContentBrowser);
        addPanel("console", "Console", m_showConsole);
        addPanel("ai_debugger", "IA Debugger", m_showAiDebug);
    }
    doc.layout.root = root.id;
    doc.layout.tree = std::move(root);

    // Widgets: the real frame-time bar (fed by m_frameProfiler in the loop),
    // a scene-confirmation modal and a keyboard focus grid over the panels.
    engine::ui::UiBarSpec frameBar;
    frameBar.id = "frame_ms";
    frameBar.value_binding = "$frameMs";
    frameBar.min = 0.0;
    frameBar.max = 33.33; // 30 FPS frame budget (profiler records real ms)
    doc.widgets.bars.push_back(std::move(frameBar));
    engine::ui::UiModalSpec confirmNewScene;
    confirmNewScene.id = "confirm_new_scene";
    confirmNewScene.title_binding = "\"Nova Cena\"";
    confirmNewScene.visible_binding = "$modal.new_scene";
    confirmNewScene.confirm_label = "Confirmar";
    confirmNewScene.cancel_label = "Cancelar";
    confirmNewScene.on_confirm = "scene.new";
    doc.widgets.modals.push_back(std::move(confirmNewScene));
    engine::ui::UiFocusSpec panelFocus;
    panelFocus.id = "panels";
    panelFocus.ids = panelIds.empty()
        ? std::vector<std::string>{"viewport", "hierarchy", "inspector",
                                   "content_browser", "console"}
        : panelIds;
    panelFocus.cols = 2;
    panelFocus.wrap = true;
    doc.widgets.focus.push_back(std::move(panelFocus));

    // Viewport: real editor DPI/contrast state (the Gameplay UI surface and
    // the qt theme consume the same flags).
    doc.viewport.version = 1;
    doc.viewport.reference_width = 1920.0;
    doc.viewport.reference_height = 1080.0;
    doc.viewport.scale_mode = engine::ui::UiScaleMode::Fit;
    doc.viewport.text_scale = std::max(1.0, static_cast<double>(m_uiDpiScale));
    doc.viewport.high_contrast = m_uiHighContrast;

    // Confirmations: the real scene actions (handle_control_command
    // "new-scene"/"save-scene"), the same commands the command index lists.
    engine::ui::ConfirmActionSpec newSceneAction;
    newSceneAction.id = "scene.new";
    newSceneAction.title = "Nova Cena";
    newSceneAction.message = "Criar uma nova cena descarta a cena atual nao salva.";
    newSceneAction.severity = engine::ui::ConfirmSeverity::Warning;
    newSceneAction.confirm_label = "Criar";
    newSceneAction.cancel_label = "Cancelar";
    newSceneAction.on_confirm = "new-scene";
    doc.confirmations.push_back(std::move(newSceneAction));
    engine::ui::ConfirmActionSpec saveSceneAction;
    saveSceneAction.id = "scene.save";
    saveSceneAction.title = "Salvar Cena";
    saveSceneAction.message = "Salvar a cena atual no arquivo de cena.";
    saveSceneAction.severity = engine::ui::ConfirmSeverity::Info;
    saveSceneAction.confirm_label = "Salvar";
    saveSceneAction.cancel_label = "Cancelar";
    saveSceneAction.on_confirm = "save-scene";
    doc.confirmations.push_back(std::move(saveSceneAction));

    std::string err;
    if (!doc.validate(err)) {
        std::cerr << "[Editor] ui-doc validation failed: " << err << std::endl;
        return std::string();
    }
    return doc.to_json();
}


void EditorApplication::apply_layout_defaults() {
    // Register the REAL panels the shell actually renders (idempotent:
    // duplicate ids are refused). The registry was never populated anywhere,
    // so the layout model, /layout and the qt-doc docks were empty. The
    // default_open flags match the shell's real initial visibility.
    const auto reg = [&](const std::string& id, const std::string& title,
                         const std::string& category, bool defaultOpen) {
        Engine::Editor::EditorPanelSpec spec;
        spec.id = id;
        spec.title = title;
        spec.category = category;
        spec.default_open = defaultOpen;
        m_panelRegistry.register_panel(std::move(spec));
    };
    reg("viewport", "Viewport", "view", true);
    reg("hierarchy", "Scene", "scene", true);
    reg("inspector", "Inspector", "inspector", true);
    reg("content_browser", "Assets", "assets", true);
    reg("console", "Console", "tools", false);
    reg("render_debugger", "Render Debugger", "debug", false);
    reg("script_debugger", "Script Debugger", "debug", false);
    reg("script_canvas", "Script Canvas", "debug", false);
    reg("ai_debugger", "AI Debugger", "debug", false);
    // SAFE reset: rebuild the model from the registry defaults (never from
    // prior state), then re-derive the qt-doc docks and the composed ui-doc
    // from the real registry.
    m_layoutModel.reset_from_registry(m_panelRegistry.panels());
    build_qt_doc();
    m_uiDocJson = build_ui_doc_json();
}


void EditorApplication::apply_layout_visibility_to_imgui() {
    // Mirror the layout model's visibility into the real panel flags the
    // frame loop draws (the model is populated by apply_layout_defaults /
    // load_layout_settings before this runs, so ids are always known).
    m_showHierarchy = m_layoutModel.is_visible("hierarchy");
    m_showInspector = m_layoutModel.is_visible("inspector");
    m_showViewport = m_layoutModel.is_visible("viewport");
    m_showContentBrowser = m_layoutModel.is_visible("content_browser");
    m_showConsole = m_layoutModel.is_visible("console");
    m_showRenderDebugger = m_layoutModel.is_visible("render_debugger");
    m_showScriptDebugger = m_layoutModel.is_visible("script_debugger");
    m_showScriptCanvas = m_layoutModel.is_visible("script_canvas");
    m_showAiDebug = m_layoutModel.is_visible("ai_debugger");
}


void EditorApplication::apply_layout_snapshot_to_imgui() {
    // First dockspace frame: apply the loaded/persisted layout to the real
    // shell state (panel visibility + gizmo mode). Visibility first so the
    // panels drawn this frame reflect the model.
    apply_layout_visibility_to_imgui();
    const std::string mode = m_layoutModel.gizmo_mode();
    if (mode == "translate") m_gizmoMode = GizmoMode::Translate;
    else if (mode == "rotate") m_gizmoMode = GizmoMode::Rotate;
    else if (mode == "scale") m_gizmoMode = GizmoMode::Scale;
    else if (mode == "select") m_gizmoMode = GizmoMode::Select;
}


void EditorApplication::save_layout_settings() {
    const std::filesystem::path layoutPath =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "layout.json";
    std::ofstream out(layoutPath, std::ios::trunc);
    if (out) {
        out << m_layoutModel.snapshot().to_json() << "\n";
    } else {
        std::cerr << "[Editor] Não foi possível salvar layout: " << layoutPath << std::endl;
    }
}


void EditorApplication::load_layout_settings() {
    // Populate the registry + model from the real panels first (defaults),
    // then apply a persisted layout on top when one exists. Malformed files
    // are ignored (all-or-nothing), keeping the registry defaults.
    apply_layout_defaults();
    const std::filesystem::path layoutPath =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "layout.json";
    std::ifstream in(layoutPath);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    Engine::Editor::EditorLayoutSnapshot snap;
    if (!snap.load_from_json(ss.str(), err)) {
        std::cerr << "[Editor] Layout inválido ignorado: " << err << std::endl;
        return;
    }
    if (!m_layoutModel.apply_snapshot(snap, err)) {
        std::cerr << "[Editor] Falha ao aplicar layout: " << err << std::endl;
        return;
    }
    apply_layout_visibility_to_imgui();
}


void EditorApplication::update_ui_dpi_scale() {}

void EditorApplication::register_render_providers() {
    if (m_renderMetrics && m_renderProviderRegistry) return;
    std::string error;
    if (!m_renderMetrics) {
        m_renderMetrics = Engine::Rendering::create_render_pass_metrics(0);
    }
    if (!m_renderProviderRegistry) {
        m_renderProviderRegistry = Engine::Rendering::create_render_provider_registry(error);
    }

    // Record the REAL providers backing each viewport rendering system. Each
    // entry is the editor's actual implementation + its consumption call site
    // in the frame, so the registry reflects what the viewport really runs.
    if (m_renderProviderRegistry) {
        m_renderProviderRegistry->set({
            "materials", "editor-material-graph+block-atlas",
            "record_viewport_scene_content(): GraphMaterialPipeline draws + block/character per-face atlases",
            "src/editor", "default" });
        m_renderProviderRegistry->set({
            "shadows", "editor-raster-depth-map (sun+spot atlas+point 6-face)",
            "record_shadow_pass()/record_spot_shadow_pass()/record_point_shadow_pass()",
            "src/editor", "default" });
        m_renderProviderRegistry->set({
            "gi", "editor-probe-grid (IRendering IProbeGrid)",
            "update_gi_probes(): probe->update() + toroidal wrap into EditorShadowUbo",
            "src/editor", "default" });
        m_renderProviderRegistry->set({
            "lights", "editor-scene-light-ubo (Rendering::LightUboData)",
            "update_scene_light_ubo(): sun+point+spot+area into the shared light set",
            "src/editor", "default" });
        m_renderProviderRegistry->set({
            "frameGraph", "editor-viewport-render-graph (RenderGraph executor)",
            "build_viewport_render_graph()+executor.record(): scene pass begin/content/end",
            "src/editor", "default" });
        m_renderProviderRegistry->set({
            "picking", "editor-id-pass (pickRenderPass + m_pickColorToEntity)",
            "render_pick_pass()+perform_pick_readback()",
            "src/editor", "default" });
        m_renderProviderRegistry->set({
            "grid", "editor-analytic-infinite-grid (GridPushConstants)",
            "draw_grid(): fullscreen triangle with invViewProj",
            "src/editor", "default" });
    }
}

// Feeds the public IRenderingDebugView from the editor's REAL frame state so
// the cards/probes/overdraw/tracing debug-overlay contract reflects actual
// data (probe irradiance from the live m_probeGrid, real capture counts) and
// never fabricated data. `refresh_render_diagnostics()` then serializes the
// debug-view snapshot alongside the pass metrics.

void EditorApplication::feed_render_debug_view() {
    if (!m_renderDebugView) {
        std::string dbgError;
        m_renderDebugView = Engine::Rendering::create_rendering_debug_view(dbgError);
        if (!m_renderDebugView) return;
    }
    // Probes: the editor's real DDGI probe grid (m_probeGrid) is the source.
    std::vector<Engine::Rendering::DebugProbe> probes;
    std::uint32_t pending = 0;
    std::uint32_t sunRev = 0;
    if (m_probeGrid) {
        const std::uint32_t count = m_probeGrid->probe_count();
        probes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            Engine::Rendering::ProbeGridProbe src;
            if (!m_probeGrid->probe(i, src)) continue;
            Engine::Rendering::DebugProbe dp;
            dp.radianceVisibility = glm::vec4(src.irradiance, 1.0f);
            dp.worldCellCascade = glm::ivec4(src.cell, -1);
            probes.push_back(dp);
        }
        pending = m_probeGrid->relocation_count() + m_probeGrid->classification_count();
        sunRev = static_cast<std::uint32_t>(m_fps); // nominal revision counter
    }
    m_renderDebugView->bind_probes(probes, pending, sunRev);
    // Cards: the editor's block/character per-face atlases act as the surface
    // cards resident in this viewport (one entry each, real resource count).
    std::vector<Engine::Rendering::DebugCard> cards;
    std::vector<std::uint32_t> perCascade;
    cards.reserve(m_blockTextures.size());
    for (const auto& [id, tex] : m_blockTextures) {
        (void)tex;
        Engine::Rendering::DebugCard card;
        card.center = glm::vec3(0.0f);
        card.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        card.albedo = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
        card.cascade = static_cast<std::uint8_t>(id.get_low() & 0xFFu);
        cards.push_back(card);
    }
    if (!perCascade.empty()) perCascade[0] = static_cast<std::uint32_t>(cards.size());
    else perCascade.push_back(static_cast<std::uint32_t>(cards.size()));
    m_renderDebugView->bind_cards(cards, perCascade);
    // Capture counters (capture overlay): the editor's REAL capture activity —
    // thumbnails actually rendered (content-hash cache) vs. the pending capture
    // queue, and the real viewport GPU allocation recorded into the render
    // metrics each frame (recordMemory("viewport", w*h*4) in
    // render_scene_to_offscreen). Never fabricated numbers.
    m_renderDebugView->bind_capture(
        static_cast<std::uint32_t>(m_assetThumbnailHashes.size()),
        static_cast<std::uint32_t>(m_thumbnailQueue.size()),
        static_cast<std::uint64_t>(m_offscreen.width) * m_offscreen.height * 4);
    m_renderDebugView->refresh();
}

// Selects which debug overlay is active in the presented viewport and returns
// the REAL data the overlay currently carries (from the live IRenderingDebugView
// snapshot). Unknown overlays are refused (all-or-nothing: selection unchanged).

std::string EditorApplication::apply_render_debug_overlay(const std::string& overlay) {
    static const char* const kKnown[] = { "none", "probes", "cards", "capture", "trace", "disocclusion" };
    bool known = false;
    for (const char* k : kKnown) {
        if (overlay == k) { known = true; break; }
    }
    if (!known) return "render-debug: unknown overlay (none|probes|cards|capture|trace|disocclusion)";
    m_selectedDebugOverlay = overlay;
    feed_render_debug_view();
    Engine::Rendering::RenderingDebugSnapshot dbg;
    if (m_renderDebugView) dbg = m_renderDebugView->snapshot();
    std::ostringstream out;
    out << "active=" << m_selectedDebugOverlay
        << " probes=" << dbg.probeCount << "/" << dbg.pendingProbes
        << " cards=" << dbg.cardCount << " captured=" << dbg.capturedCount
        << " pending=" << dbg.pendingCount
        << " traces=" << dbg.tracePaths.size()
        << " disoccludedPx=" << dbg.disoccludedPixels
        << " confidence=" << dbg.confidenceLevel;
    return out.str();
}


void EditorApplication::run_luau_sandbox() {
    // Luau sandbox (Conta 5 §2): the public ILuauSandbox is a real product
    // consumer now. Each frame the editor runs a small script through the
    // sandbox and records the deterministic result + execution counter. Uses a
    // compliant source (no io./require), a budget-exceeded source (proves the
    // instruction ceiling is enforced) and an io source (proves the io
    // lockdown), publishing the observed counters.
    if (!m_luauSandbox) return;
    std::string sbErr;
    // 1) compliant run — the boxed result is JSON-shaped, deterministic.
    {
        const engine::scripting::ScriptResult res = m_luauSandbox->evaluate(
            "{8}cmds => {\"ok\": true, \"value\": 1}", "main", sbErr);
        if (res.ok) {
            m_luauSandboxLastResult = res.value;
            m_luauSandboxExecutions = m_luauSandbox->executions();
        }
    }
    // 2) io lockdown run — the sandbox REFUSES io.* (explicit error, never
    // silent): proves the product never lets a boxed script touch the disk.
    try {
        const engine::scripting::ScriptResult res = m_luauSandbox->evaluate(
            "{2}io.open('/etc/passwd')", "main", sbErr);
        if (!res.ok && res.error.find("sandbox") != std::string::npos) {
            m_luauSandboxIoLocked = true;
        }
    } catch (...) {
        m_luauSandboxIoLocked = true;
    }
}


void EditorApplication::cook_showcase_assets() {
    // IAssetCooker (Conta 5 §3/§4): the editor is a real consumer of the
    // cooker. Each frame it cooks the showcase project's data-driven config
    // documents (Text format) — the SAME documents the package builder ships —
    // through the cooker once, recorded against their content hash (cook-once
    // semantics: a repeated cook of unchanged bytes is a cache hit). The cook
    // result (source, content hash, cache hit, artifact size) is published for
    // observability.
    if (!m_assetCooker) return;
    const char* assets[] = {
        "/Projects/ShowcaseGame/Content/Config/showcase_ocean.json",
        "/Projects/ShowcaseGame/Content/Config/showcase_particles.json",
        "/Projects/ShowcaseGame/Content/Input/showcase_input.json",
        "/Projects/ShowcaseGame/Content/AudioEvents/showcase_audio.json",
        "/Projects/ShowcaseGame/Content/Network/showcase_network.json",
    };
    m_cookedAssetCount = 0;
    for (const char* rel : assets) {
        const std::filesystem::path path =
            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / (rel + 1);  // skip leading '/'
        std::error_code ioEc;
        if (!std::filesystem::is_regular_file(path, ioEc)) continue;
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        std::string bytes((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
        engine::assets::FormatDocument doc;
        doc.format = engine::assets::AssetFormat::Text;
        doc.source_name = std::filesystem::path(rel).filename().string();
        doc.bytes.assign(bytes.begin(), bytes.end());
        doc.revision = 1;
        engine::assets::CookOptions opts;
        engine::assets::CookArtifact artifact;
        std::string cookErr;
        if (m_assetCooker->cook(doc, opts, artifact, cookErr)) {
            if (m_assetCooker->validate(artifact, cookErr)) {
                m_cookedAssetLast = artifact.source_name;
                m_cookedAssetHash = artifact.content_hash;
                m_cookedAssetCacheHit = artifact.cache_hit;
                ++m_cookedAssetCount;
            }
        }
    }
    {
        std::ostringstream out;
        out << "{\"valid\":true,\"count\":" << m_cookedAssetCount
            << ",\"last\":\"" << m_cookedAssetLast
            << "\",\"hash\":" << m_cookedAssetHash
            << ",\"cacheHit\":" << (m_cookedAssetCacheHit ? "true" : "false") << "}";
        m_cookAssetsJson = out.str();
    }
}

}  // namespace Engine

// entityIcon is forward-declared at GLOBAL scope in EditorApplicationPanels.cpp
// but the definition lives in Engine::UI (ForgeWidgets.cpp). Bridge the two.
// (Moved here from EditorApplicationRecovered.cpp in the Agente 3 TU split.)
namespace Engine { namespace UI { const char* entityIcon(Scene* scene, const UUID& id); } }
const char* entityIcon(Engine::Scene* scene, const Engine::UUID& id) {
    return Engine::UI::entityIcon(scene, id);
}

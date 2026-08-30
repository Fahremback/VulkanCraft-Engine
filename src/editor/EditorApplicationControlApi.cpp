// EditorApplicationControlApi.cpp
//
// Agente 3 (fechamento_solidacao) — TU split: cohesive Control-API methods were
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
        if (m_playModeContract) {
            std::string stepErr;
            (void)m_playModeContract->step(stepErr);
        }
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
    } else if (cmd.rfind("render-view ", 0) == 0) {
        // D.1: selective render-view toggles that gate the PRESENTED viewport
        // (the same state the ⋯ menu and record_viewport_scene_content read).
        // Syntax: render-view grid 0|1|toggle | gizmos ... | colliders ...
        std::istringstream ss(cmd.substr(12));
        std::string key;
        std::string value;
        ss >> key >> value;
        auto apply_bool = [](bool current, const std::string& v) {
            if (v == "toggle") return !current;
            if (v == "1" || v == "true" || v == "on") return true;
            if (v == "0" || v == "false" || v == "off") return false;
            return current;
        };
        if (key == "grid") m_showGrid = apply_bool(m_showGrid, value);
        else if (key == "gizmos") m_showGizmos = apply_bool(m_showGizmos, value);
        else if (key == "colliders") m_showColliders = apply_bool(m_showColliders, value);
        else {
            m_controlResult = "render-view: expected 'grid', 'gizmos' or 'colliders' + 0|1|toggle";
        }
        std::cout << "[ControlApi] render-view " << key << " " << value
                  << " (grid=" << m_showGrid << " gizmos=" << m_showGizmos
                  << " colliders=" << m_showColliders << ")" << std::endl;
    } else if (cmd.rfind("render-debug ", 0) == 0) {
        // D.1: selectable debug overlay. The confirmation carries the REAL
        // snapshot counts of the overlay now active (probes/cards/capture/…).
        const std::string result = apply_render_debug_overlay(cmd.substr(13));
        if (result.rfind("render-debug: unknown", 0) == 0) m_controlResult = result;
        else m_controlData = result; // success payload read by the MCP tool
        std::cout << "[ControlApi] render-debug -> " << result << std::endl;
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
        else if (w == "ai" || w == "ai-debug" || w == "aidebug") toggle(m_showAiDebug);
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


}  // namespace Engine

#pragma once

// ===========================================================================
// WickedToolsPanel — frontend port from the Wicked Engine editor (MIT,
// commit 2aa9fdf, (c) Turánszki János; see src/editor/frontend/PORTS.md).
//
// Policy (per the user decision recorded in PORTS.md):
//   - Feature the VulkanCraft editor HAS        → button wired to ours.
//   - Feature we DON'T have yet                 → panel kept with an explicit
//     TODO(frontend-port) comment, wired when implemented.
//   - Feature the donor DOESN'T have (our own)  → panel created and wired.
//
// Each window edits the active scene/selected entity. Components that the
// play world does not simulate yet are still authored and serialized; the
// runtime pass carries a TODO(frontend-port) marker on the panel.
// ===========================================================================

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>

#include <glm/glm.hpp>
#include "../engine/scene/Scene.hpp"
#include "../engine/scene/Entity.hpp"
#include "../engine/assets/AssetRegistry.hpp"

namespace Engine {

class WickedToolsPanel {
public:
    // Window visibility toggles (one per ported window; menu + toolbar use
    // these so a single flag controls each window).
    bool showNameWindow{ false };
    bool showLayerWindow{ false };
    bool showObjectWindow{ false };
    bool showLightWindow{ false };
    bool showCameraWindow{ false };
    bool showMaterialWindow{ false };
    bool showSoundWindow{ false };
    bool showRigidBodyWindow{ false };
    bool showColliderWindow{ false };
    bool showConstraintWindow{ false };
    bool showSoftBodyWindow{ false };
    bool showSpringWindow{ false };
    bool showDecalWindow{ false };
    bool showEmitterWindow{ false };
    bool showHairParticleWindow{ false };
    bool showSplineWindow{ false };
    bool showForceFieldWindow{ false };
    bool showEnvProbeWindow{ false };
    bool showWeatherWindow{ false };
    bool showAnimationWindow{ false };
    bool showArmatureWindow{ false };
    bool showHumanoidWindow{ false };
    bool showIKWindow{ false };
    bool showExpressionWindow{ false };
    bool showTerrainWindow{ false };
    bool showPaintToolWindow{ false };
    bool showMeshWindow{ false };
    bool showModelImporterWindow{ false };
    bool showVideoWindow{ false };
    bool showGaussianSplatWindow{ false };
    bool showThemeEditorWindow{ false };
    bool showProjectCreatorWindow{ false };
    bool showGeneralWindow{ false };
    bool showGraphicsWindow{ false };
    bool showProfilerWindow{ false };
    // Ours: developer panel (Control API buttons, self-tests, honest status)
    // and the in-editor "Como Usar" guide.
    bool showDevWindow{ false };
    bool showGuideWindow{ false };

    // Live editor context, refreshed every frame by EditorApplication.
    void set_context(Scene* scene, UUID selectedEntity, void* language) {
        m_scene = scene;
        m_selectedEntity = selectedEntity;
        m_language = language;
    }
    void set_asset_registry(AssetRegistry* registry) { m_assetRegistry = registry; }
    // Callback to open the existing specialized editors (Material tab etc.).
    void set_open_specialized_editors(bool* openFlag) { m_openSpecializedEditors = openFlag; }
    // Callback invoked when a panel deletes an entity, so the editor can clear
    // its selection and stay consistent.
    void set_on_entity_deleted(std::function<void(UUID)> cb) { m_onEntityDeleted = std::move(cb); }
    // Callback invoked when the Model Importer panel wants to import a file
    // (the editor owns the AssetPipeline). Returns a status line.
    void set_import_asset_callback(std::function<std::string(const std::string&)> cb) {
        m_importAsset = std::move(cb);
    }
    // Callback invoked when the Project Creator panel creates a project.
    // Returns a status line ("OK: ..." or "Erro: ...").
    void set_create_project_callback(std::function<std::string(const std::string&, const std::string&)> cb) {
        m_createProject = std::move(cb);
    }
    // Callback invoked when the Terrain panel generates the heightmap mesh.
    void set_terrain_callback(std::function<void(float, int, float, float)> cb) {
        m_applyTerrain = std::move(cb);
    }
    // Callback invoked when the Graphics panel applies VSync/shadow quality.
    void set_graphics_callback(std::function<void(bool, int)> cb) {
        m_applyGraphics = std::move(cb);
    }
    // Callback invoked when General/Theme panels persist settings to disk.
    void set_save_settings_callback(std::function<void()> cb) {
        m_saveSettings = std::move(cb);
    }
    // Callback invoked when the Mesh panel edits the selected entity's mesh
    // normals (0 = recalc smooth, 1 = flip). Returns a status line.
    void set_mesh_callback(std::function<std::string(int)> cb) {
        m_applyMesh = std::move(cb);
    }
    // Callback invoked by the Dev panel for Control-API commands ("play",
    // "step", "zoom 0.1", ...). The editor routes them through the same
    // handler the HTTP API uses.
    void set_control_command_callback(std::function<void(const std::string&)> cb) {
        m_controlCmd = std::move(cb);
    }
    // Callback that runs one self-test (spawns the editor headless with the
    // test env var) and returns "PASS"/"FAIL"/error line.
    void set_self_test_callback(std::function<std::string(int)> cb) {
        m_selfTest = std::move(cb);
    }
    // Current play state (PlayState enum as int) for enabling the right
    // Control-API buttons. Refreshed every frame by the editor.
    void set_play_state(int state) { m_playState = state; }
    // Standalone "Empacotar Assets" (packages cooked assets without a full
    // build). Returns a status line.
    void set_package_assets_callback(std::function<std::string()> cb) {
        m_packageAssets = std::move(cb);
    }
    // Returns the hot-reload service status line (watched assets, activity).
    void set_hot_reload_status_callback(std::function<std::string()> cb) {
        m_hotReloadStatus = std::move(cb);
    }
    // Frame stats fed every frame by the editor for the Profiler graph.
    void set_frame_stats(float fps, float frameMs) {
        m_statFps = fps;
        m_statFrameMs = frameMs;
        m_frameTimes.push_back(frameMs);
        if (m_frameTimes.size() > kProfilerHistory) m_frameTimes.erase(m_frameTimes.begin());
    }
    void set_theme_colors(glm::vec3 bg, glm::vec3 panel) { m_themeBg = bg; m_themePanel = panel; }
    void set_theme(glm::vec3 bg, glm::vec3 panel) { m_themeBg = bg; m_themePanel = panel; }
    glm::vec3 theme_background() const { return m_themeBg; }
    glm::vec3 theme_panel() const { return m_themePanel; }

    // Draws the "Ferramentas" (Tools) menu content; call inside a BeginMenu.
    void draw_tools_menu();
    // Draws every visible window.
    void draw();

private:
    Scene* m_scene{ nullptr };
    UUID m_selectedEntity{ 0, 0 };
    // Points at the editor's EngineLanguage enum (opaque to avoid a hard
    // dependency on EditorApplication; null falls back to Português).
    void* m_language{ nullptr };
    bool* m_openSpecializedEditors{ nullptr };
    AssetRegistry* m_assetRegistry{ nullptr };
    std::function<void(UUID)> m_onEntityDeleted;
    std::function<std::string(const std::string&)> m_importAsset;
    std::function<std::string(const std::string&, const std::string&)> m_createProject;
    std::function<void(float, int, float, float)> m_applyTerrain;
    std::function<void(bool, int)> m_applyGraphics;
    std::function<void()> m_saveSettings;
    std::function<std::string(int)> m_applyMesh;
    std::function<void(const std::string&)> m_controlCmd;
    std::function<std::string(int)> m_selfTest;
    std::function<std::string()> m_packageAssets;
    std::function<std::string()> m_hotReloadStatus;
    int m_playState{ 0 }; // PlayState::Edit
    static constexpr size_t kProfilerHistory = 240;
    std::vector<float> m_frameTimes;
    float m_statFps{ 0.0f };
    float m_statFrameMs{ 0.0f };
    std::string m_projectStatus;
    std::string m_terrainStatus;
    std::string m_graphicsStatus;
    std::string m_meshStatus;

    // Per-window draw functions.
    void draw_name_window();
    void draw_layer_window();
    void draw_object_window();
    void draw_light_window();
    void draw_camera_window();
    void draw_material_window();
    void draw_sound_window();
    void draw_rigidbody_window();
    void draw_collider_window();
    void draw_constraint_window();
    void draw_softbody_window();
    void draw_spring_window();
    void draw_decal_window();
    void draw_emitter_window();
    void draw_hair_particle_window();
    void draw_spline_window();
    void draw_force_field_window();
    void draw_env_probe_window();
    void draw_weather_window();
    void draw_animation_window();
    void draw_armature_window();
    void draw_humanoid_window();
    void draw_ik_window();
    void draw_expression_window();
    void draw_terrain_window();
    void draw_paint_tool_window();
    void draw_mesh_window();
    void draw_model_importer_window();
    void draw_video_window();
    void draw_gaussian_splat_window();
    void draw_theme_editor_window();
    void draw_project_creator_window();
    void draw_general_window();
    void draw_graphics_window();
    void draw_profiler_window();
    void draw_dev_window();
    void draw_guide_window();

    // Helpers.
    const char* tr(const char* pt, const char* en) const;
    // Reads the editor's EngineLanguage enum through the opaque pointer.
    bool is_pt() const;
    bool has_selection() const { return m_scene && m_selectedEntity.is_valid(); }
    bool entity_exists(UUID id) const;
    void entity_combo(const char* label, UUID& id, bool includeNone = true) const;
    // Generic component panel scaffold: shows the header + a remove button,
    // returns true when the caller should draw the body (component present).
    bool component_panel_begin(const char* title, bool present);
    void component_panel_end();
    // Texture path picker fed by the asset registry.
    void texture_path_input(const char* label, std::string& path) const;
    // Colored "no runtime yet" badge shown on TODO(frontend-port) windows so
    // authoring something that does nothing is never surprising.
    void todo_badge(const char* pt, const char* en);

    // Panel-local state (animation playback preview, IK strength, ...).
    float m_animTime{ 0.0f };
    float m_animSpeed{ 1.0f };
    float m_ikStrength{ 1.0f };
    float m_terrainScale{ 1.0f };
    int m_terrainOctaves{ 4 };
    float m_terrainAmount{ 0.5f };
    float m_terrainFalloff{ 0.4f };
    float m_paintRadius{ 0.5f };
    float m_paintAmount{ 0.5f };
    float m_paintSmooth{ 0.5f };
    glm::vec3 m_paintColor{ 1.0f, 1.0f, 1.0f };
    bool m_meshDoubleSided{ false };
    bool m_videoLoop{ true };
    // Armature: rest poses captured by the panel (session-scoped).
    std::unordered_map<UUID, TransformComponent> m_boneRestPose;
    void create_humanoid_rig(UUID parentId);
    glm::vec3 m_themeBg{ 0.10f, 0.11f, 0.14f };
    glm::vec3 m_themePanel{ 0.20f, 0.20f, 0.20f };
    bool m_gfxVSync{ true };
    int m_gfxShadowQuality{ 3 };
    std::string m_importerStatus;
    std::string m_devStatus;
    std::string m_packageStatus;
};

} // namespace Engine

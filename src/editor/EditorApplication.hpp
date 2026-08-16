#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <array>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "../engine/scene/Scene.hpp"
#include "../engine/physics/PhysicsRuntime.hpp"
#include "../engine/physics/Ragdoll.hpp"
#include "../engine/rendering/materials/Material.hpp"
#include "../engine/rendering/MaterialGraph.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/scene/Entity.hpp"
#include "../engine/editor/play_mode/PlayMode.hpp"
#include "../engine/editor/undo/UndoSystem.hpp"
#include "../engine/scripting/ScriptRuntime.hpp"
#include "../engine/scripting/ScriptDebugger.hpp"
#include "../engine/scripting/VisualScriptCanvas.hpp"
#include "../engine/scripting/ScriptGraphBridge.hpp"
#include "../engine/gameplay/WeaponSystem.hpp"
#include "../engine/gameplay/ParticleSimulation.hpp"
#include "../engine/gameplay/VehicleRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "../engine/editor/ui/EditorGUI.hpp"

// Play-mode nav agent (Fase 8): follows a path from the public navigation
// provider toward the primary camera entity, writing its position back to the
// entity transform each frame. Replaces the legacy NavigationAgent (the grid
// track was removed — FALTANTES item 12).
struct PlayNavAgent {
    glm::vec3 position{ 0.0f };
    float speed{ 3.0f };
    float stoppingDistance{ 0.05f };
    bool reached{ true };
    std::vector<glm::vec3> path;
    std::size_t waypoint{ 0 };

    void set_path(std::vector<glm::vec3> points);
    void update(float deltaTime);
    bool reached_destination() const { return reached; }
};
#include "../engine/assets/AssetRegistry.hpp"
#if VC_ENABLE_VOXEL_PLUGIN
#include "../plugins/voxel/VoxelPlugin.hpp"
#endif
#include "tools/SpecializedEditorsPanel.hpp"

namespace Engine {

// -----------------------------------------------------------------------
// Gizmo operation mode
// -----------------------------------------------------------------------
enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoAxis { None, X, Y, Z };

// -----------------------------------------------------------------------
// Editor camera for free-fly navigation
// -----------------------------------------------------------------------
struct EditorCamera {
    glm::vec3 position{ 0.0f, 5.0f, 12.0f };
    float yaw{ -90.0f };   // degrees
    float pitch{ -15.0f }; // degrees
    float speed{ 8.0f };
    float sensitivity{ 0.15f };
    float fov{ 60.0f };
    float nearPlane{ 0.1f };
    float farPlane{ 2000.0f };
    float scrollSpeed{ 2.0f };

    // Orbit
    float orbitDistance{ 15.0f };
    glm::vec3 orbitTarget{ 0.0f };

    glm::vec3 get_front() const;
    glm::vec3 get_right() const;
    glm::vec3 get_up() const;
    glm::mat4 get_view_matrix() const;
    glm::mat4 get_projection_matrix(float aspectRatio) const;
};

// -----------------------------------------------------------------------
// Simple vertex for procedural geometry (position + normal + color)
// -----------------------------------------------------------------------
struct EditorVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 color;
    glm::vec2 uv{ 0.0f };
};

// -----------------------------------------------------------------------
// Offscreen render target for the viewport
// -----------------------------------------------------------------------
struct OffscreenTarget {
    VkImage colorImage{ VK_NULL_HANDLE };
    VkDeviceMemory colorMemory{ VK_NULL_HANDLE };
    VkImageView colorView{ VK_NULL_HANDLE };

    VkImage depthImage{ VK_NULL_HANDLE };
    VkDeviceMemory depthMemory{ VK_NULL_HANDLE };
    VkImageView depthView{ VK_NULL_HANDLE };

    // Pick buffer (color-ID pass)
    VkImage pickImage{ VK_NULL_HANDLE };
    VkDeviceMemory pickMemory{ VK_NULL_HANDLE };
    VkImageView pickView{ VK_NULL_HANDLE };

    // Staging buffer for pick readback
    VkBuffer pickStagingBuffer{ VK_NULL_HANDLE };
    VkDeviceMemory pickStagingMemory{ VK_NULL_HANDLE };

    VkFramebuffer framebuffer{ VK_NULL_HANDLE };
    VkFramebuffer pickFramebuffer{ VK_NULL_HANDLE };
    VkRenderPass renderPass{ VK_NULL_HANDLE };
    VkRenderPass pickRenderPass{ VK_NULL_HANDLE };
    VkSampler sampler{ VK_NULL_HANDLE };

    VkDescriptorSet imguiTextureID{ VK_NULL_HANDLE };

    uint32_t width{ 0 };
    uint32_t height{ 0 };
};

// -----------------------------------------------------------------------
// Sun shadow map for the viewport: a depth-only pass renders the scene into a
// fixed-size depth map that the material shaders sample (real projected
// shadows in the editor — replaces the previous dummy 1x1 white texture).
// -----------------------------------------------------------------------
struct EditorShadowMap {
    VkImage image{ VK_NULL_HANDLE };
    VkDeviceMemory memory{ VK_NULL_HANDLE };
    VkImageView view{ VK_NULL_HANDLE };
    VkRenderPass renderPass{ VK_NULL_HANDLE };
    VkFramebuffer framebuffer{ VK_NULL_HANDLE };
    VkSampler sampler{ VK_NULL_HANDLE };
    VkShaderModule vertShader{ VK_NULL_HANDLE };
    VkShaderModule fragShader{ VK_NULL_HANDLE };
    VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };
    VkPipeline pipeline{ VK_NULL_HANDLE };
    uint32_t size{ 1024 };
    glm::mat4 viewProj{ 1.0f };
    bool enabled{ false };
};

// -----------------------------------------------------------------------
// GPU buffer for procedural geometry
// -----------------------------------------------------------------------
struct GPUBuffer {
    VkBuffer buffer{ VK_NULL_HANDLE };
    VkDeviceMemory memory{ VK_NULL_HANDLE };
    VkDeviceSize size{ 0 };
};

// -----------------------------------------------------------------------
// Push constant for the scene pipeline
// -----------------------------------------------------------------------
struct ScenePushConstants {
    glm::mat4 mvp;
    glm::vec4 color;   // .w = entity pick ID (encoded as float)
};

class EditorApplication {
public:
    EditorApplication();
    ~EditorApplication();

    int run();

private:
    void init_window();
    void init_vulkan();
    void init_imgui();
    void init_default_scene();

    // Viewport rendering
    void init_offscreen_target();
    void create_offscreen_buffers(uint32_t w, uint32_t h);
    void create_shadow_map();
    void destroy_shadow_map();
    // Records the sun shadow pass (depth-only) before the scene pass; also
    // computes m_shadowMap.viewProj consumed by write_light_ubo.
    void record_shadow_pass(VkCommandBuffer cmd, const Scene* scene);
    void init_scene_pipeline();
    void init_geometry_buffers();
    void cleanup_offscreen_target();
    void recreate_offscreen_if_needed(uint32_t w, uint32_t h);
    // Rebuilds the swapchain (framebuffers/views/images) after a resize or an
    // OUT_OF_DATE/SUBOPTIMAL result — previously the editor just returned and
    // kept presenting into a stale swapchain.
    void recreate_swapchain();

    void render_scene_to_offscreen(VkCommandBuffer cmd);
    void render_pick_pass(VkCommandBuffer cmd);
    void perform_pick_readback();

    // Geometry generation
    void generate_cube_geometry(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices);
    void generate_grid_geometry(std::vector<EditorVertex>& verts, float extent, float step);
    void generate_gizmo_geometry();
    void generate_light_icon(std::vector<EditorVertex>& verts);
    void generate_camera_icon(std::vector<EditorVertex>& verts);

    // Gizmo helpers
    void draw_gizmo_overlay(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void draw_entity_bounds(VkCommandBuffer cmd, const glm::mat4& viewProj, UUID id, const TransformComponent& t);
    void draw_light_icon(VkCommandBuffer cmd, const glm::mat4& viewProj, const TransformComponent& t, bool selected);
    void draw_camera_frustum(VkCommandBuffer cmd, const glm::mat4& viewProj, const TransformComponent& t, bool selected);
    void draw_collider_wireframe(VkCommandBuffer cmd, const glm::mat4& viewProj, const TransformComponent& t, bool selected);

    // Camera input
    void update_editor_camera(float deltaTime);
    void process_viewport_input();

    // Vulkan helpers
    uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                      VkMemoryPropertyFlags memProps, VkImage& image, VkDeviceMemory& memory,
                      uint32_t mipLevels = 1);
    VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                  uint32_t mipLevels = 1);
    void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       VkBuffer& buffer, VkDeviceMemory& memory);
    void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                 VkImageLayout oldLayout, VkImageLayout newLayout,
                                 VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                 uint32_t baseMipLevel = 0, uint32_t levelCount = 1);
    VkCommandBuffer begin_single_time_commands();
    void end_single_time_commands(VkCommandBuffer cmd);

    void main_loop();
    void render_frame();
    void cleanup();

    // GUI Panel Renderers
    void draw_dockspace();
    void draw_menu_bar();
    void draw_toolbar();
    void draw_hierarchy_panel();
    void draw_inspector_panel();
    void draw_viewport_panel();
    // Drag & drop do Content Browser no viewport: mesh → nova entidade;
    // material → entidade selecionada (se tiver MeshRenderer).
    void handle_asset_drop(const UUID& assetId);
    void draw_content_browser_panel();
    void draw_voxel_tool_panel();
    void draw_console_panel();

    // Window & Vulkan Data
    GLFWwindow* m_window{ nullptr };
    int m_windowWidth{ 1600 };
    int m_windowHeight{ 900 };

    VkInstance m_instance{ VK_NULL_HANDLE };
    VkPhysicalDevice m_physicalDevice{ VK_NULL_HANDLE };
    VkDevice m_device{ VK_NULL_HANDLE };
    VkQueue m_graphicsQueue{ VK_NULL_HANDLE };
    uint32_t m_graphicsQueueFamily{ 0 };
    VkSurfaceKHR m_surface{ VK_NULL_HANDLE };

    VkSwapchainKHR m_swapchain{ VK_NULL_HANDLE };
    VkFormat m_swapchainFormat;
    VkExtent2D m_swapchainExtent;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainViews;
    std::vector<VkFramebuffer> m_framebuffers;

    VkRenderPass m_renderPass{ VK_NULL_HANDLE };
    VkCommandPool m_commandPool{ VK_NULL_HANDLE };
    std::vector<VkCommandBuffer> m_commandBuffers;
    VkDescriptorPool m_imguiDescriptorPool{ VK_NULL_HANDLE };

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    uint32_t m_currentFrame{ 0 };

    // ---- Offscreen Viewport Rendering ----
    OffscreenTarget m_offscreen;
    EditorShadowMap m_shadowMap;
    VkPipeline m_scenePipeline{ VK_NULL_HANDLE };
    VkPipeline m_wireframePipeline{ VK_NULL_HANDLE };
    VkPipeline m_gizmoPipeline{ VK_NULL_HANDLE };
    VkPipeline m_pickPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout m_scenePipelineLayout{ VK_NULL_HANDLE };

    // Viewport shaders (compiled by the compile_shaders target)
    VkShaderModule m_viewportVertShader{ VK_NULL_HANDLE };
    VkShaderModule m_viewportFragShader{ VK_NULL_HANDLE };
    VkShaderModule m_pickFragShader{ VK_NULL_HANDLE };

    // Geometry buffers
    GPUBuffer m_cubeVB;
    GPUBuffer m_cubeIB;
    uint32_t m_cubeIndexCount{ 0 };

    GPUBuffer m_gridVB;
    uint32_t m_gridVertexCount{ 0 };

    // Light / camera helper icons (LINE_LIST, non-indexed)
    GPUBuffer m_lightIconVB;
    uint32_t m_lightIconVertexCount{ 0 };
    GPUBuffer m_cameraIconVB;
    uint32_t m_cameraIconVertexCount{ 0 };

    // Gizmo geometry (all modes packed into one buffer pair).
    struct GizmoDrawRange {
        uint32_t offset{ 0 };
        uint32_t count{ 0 };
    };
    GPUBuffer m_gizmoVB;
    GPUBuffer m_gizmoIB;
    GizmoDrawRange m_gizmoShaftRanges[3]; // translate/scale axis shaft (wireframe line)
    GizmoDrawRange m_gizmoArrowRanges[3]; // translate arrow cone (solid)
    GizmoDrawRange m_gizmoRingRanges[3];  // rotate ring (wireframe)
    GizmoDrawRange m_gizmoTipRanges[3];   // scale tip cube (solid)

    // Editor Camera
    EditorCamera m_editorCamera;
    bool m_viewportHovered{ false };
    bool m_viewportFocused{ false };
    ImVec2 m_viewportPos{ 0, 0 };
    ImVec2 m_viewportSize{ 800, 600 };
    bool m_rightMouseDown{ false };
    bool m_middleMouseDown{ false };
    glm::vec2 m_lastMousePos{ 0, 0 };
    float m_deltaTime{ 0.016f };

    // Gizmo State
    GizmoMode m_gizmoMode{ GizmoMode::Translate };
    GizmoAxis m_hoveredAxis{ GizmoAxis::None };
    GizmoAxis m_activeAxis{ GizmoAxis::None };
    bool m_gizmoDragging{ false };
    glm::vec3 m_gizmoDragEntityStart{ 0 };   // entity position at drag start
    glm::vec3 m_gizmoDragRotStart{ 0 };      // entity rotation at drag start
    glm::vec3 m_gizmoDragScaleStart{ 1 };    // entity scale at drag start
    glm::vec3 m_gizmoDragPlanePoint{ 0 };    // world point where the drag started
    glm::vec3 m_gizmoDragPlaneNormal{ 0 };   // camera forward at drag start
    glm::vec3 m_gizmoDragAngleRef{ 0 };      // reference vector on the axis plane (rotate)
    glm::vec3 m_gizmoAxisWorld{ 0 };         // active axis direction in world space
    float m_snapTranslate{ 0.5f };
    float m_snapRotate{ 15.0f };
    float m_snapScale{ 0.25f };

    // Viewport image rect (screen coords of the displayed offscreen texture)
    ImVec2 m_viewportImagePos{ 0, 0 };
    ImVec2 m_viewportImageSize{ 0, 0 };
    bool m_viewportImageHovered{ false };

    // Camera drag state
    bool m_orbitDragging{ false };
    bool m_panDragging{ false };

    // Viewport interaction helpers
    void start_gizmo_drag(glm::vec2 mouseScreen);
    void update_gizmo_drag(glm::vec2 mouseScreen);
    bool gizmo_axis_hit_test(glm::vec2 mouseScreen);
    glm::vec3 unproject_to_plane(glm::vec2 mouseScreen, const glm::vec3& planePoint,
                                 const glm::vec3& planeNormal, const glm::mat4& invViewProj) const;

    // Cooked mesh resources loaded from the asset registry (for the viewport).
    struct EditorMeshResource {
        GPUBuffer vb;
        GPUBuffer ib;
        struct DrawRange {
            uint32_t firstIndex{ 0 };
            uint32_t indexCount{ 0 };
            uint32_t vertexOffset{ 0 };
            bool indexed{ false };
        };
        std::vector<DrawRange> ranges;
        uint32_t vertexCount{ 0 };
        bool valid{ false };
    };
    std::unordered_map<UUID, EditorMeshResource> m_meshResources;
    std::unordered_set<UUID> m_meshLoadFailed;
    bool load_mesh_resource(const UUID& assetId);
    const EditorMeshResource* get_mesh_resource(const UUID& assetId);
    void draw_mesh_resource(VkCommandBuffer cmd, const glm::mat4& mvp, const glm::vec4& color,
                            const EditorMeshResource& resource);
    void destroy_mesh_resources();

    // Material-graph rendering: pipelines built from a MaterialGraph via
    // material_graph_to_glsl + glslc, driven by per-entity UBO parameters.
    struct GraphTexture {
        VkImage image{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        VkImageView view{ VK_NULL_HANDLE };
        VkFormat format{ VK_FORMAT_R8G8B8A8_UNORM };
    };
    struct GraphMaterialPipeline {
        VkPipeline pipeline{ VK_NULL_HANDLE };
        VkPipelineLayout layout{ VK_NULL_HANDLE };
        VkDescriptorSetLayout descriptorSetLayout{ VK_NULL_HANDLE };
        VkDescriptorPool pool{ VK_NULL_HANDLE };
        VkDescriptorSet descriptorSet{ VK_NULL_HANDLE };
        VkBuffer uboBuffer{ VK_NULL_HANDLE };
        VkDeviceMemory uboMemory{ VK_NULL_HANDLE };
        VkDeviceSize uboSize{ 0 };
        // LightParams UBO (binding from the generated shader).
        VkBuffer lightBuffer{ VK_NULL_HANDLE };
        VkDeviceMemory lightMemory{ VK_NULL_HANDLE };
        uint32_t lightUboBinding{ 1 };
        // Optional shadow-map sampler: the editor has no shadow pass, so a
        // 1x1 white dummy is bound and shadows stay disabled (shadowParams.x=0).
        GraphTexture shadowDummy;
        uint32_t shadowSamplerBinding{ 2 };
        std::vector<std::string> uniformNames;
        std::vector<Rendering::MaterialValueType> uniformTypes;
        std::vector<Rendering::MaterialValue> uniformDefaults;
        // Combined image samplers, binding i+1 for textures[i] (node-id order).
        std::vector<GraphTexture> textures;
        uint64_t graphHash{ 0 };
        std::string lastError;
        bool valid{ false };
    };
    bool load_viewport_texture(const UUID& assetId, GraphTexture& out, std::string& error);
    bool upload_texture_half_pixels(uint32_t width, uint32_t height,
                                    const std::vector<uint8_t>& halfRgba,
                                    GraphTexture& out, std::string& error);
    bool upload_texture_pixels(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                               uint32_t mipCount, bool srgb, GraphTexture& out, std::string& error);
    void destroy_graph_texture(GraphTexture& t);
    std::unordered_map<UUID, GraphMaterialPipeline> m_graphMaterialPipelines;
    std::unordered_set<UUID> m_materialLoadFailed;
    std::unordered_map<UUID, MaterialAsset> m_materialAssets;
    bool load_material_asset(const UUID& assetId);
    bool build_graph_pipeline(const Rendering::MaterialGraph& graph, GraphMaterialPipeline& out);
    void destroy_graph_pipeline(GraphMaterialPipeline& p);
    void destroy_graph_material_pipelines();
    void write_material_ubo(const GraphMaterialPipeline& p, const MaterialAsset* material,
                            const MaterialComponent* component);
    // Fills the pipeline's LightParams UBO from the scene's LightComponents
    // (range >= 50 ⇒ directional sun; otherwise point light) + the camera.
    void write_light_ubo(GraphMaterialPipeline& p, const Scene* scene, const glm::vec3& cameraPos);
    // Live preview: the Material Editor graph rendered on the selected entity.
    GraphMaterialPipeline m_liveGraphPipeline;
    uint64_t m_liveGraphHash{ 0 };
    bool m_liveGraphLastErrorLogged{ false };
    // VC_EDITOR_TEST_MATERIAL=1: self-test entity exercising the graph pipeline.
    int m_materialTestFramesLeft{ 0 };
    UUID m_materialTestMeshId{ 0, 0 };
    UUID m_materialTestMatId{ 0, 0 };
    // VC_EDITOR_TEST_PLAY=1: play world runs physics and is rendered in the viewport.
    int m_playTestFramesLeft{ 0 };
    UUID m_playTestEntityId{ 0, 0 };

    // Pick
    UUID m_pickedEntityID{ 0, 0 };
    bool m_pickRequested{ false };
    glm::vec2 m_pickPixel{ 0, 0 };
    std::unordered_map<uint32_t, UUID> m_pickColorToEntity;

    // Localization System
    enum class EngineLanguage { PT_BR, EN_US };
    EngineLanguage m_currentLanguage{ EngineLanguage::PT_BR };
    inline const char* tr(const char* pt, const char* en) const {
        return (m_currentLanguage == EngineLanguage::PT_BR) ? pt : en;
    }

    // Launcher Hub State
    bool m_inLauncherMode{ true };
    int m_selectedProjectIndex{ 0 };
    std::string m_currentProjectName{ "EmptyProject" };
    void draw_project_launcher();

    // Build Game (README §39): cook → package → scene → shaders → exe.
    std::vector<std::string> m_buildLog;
    void run_game_build();

    // Editor Scene & State
    std::unique_ptr<Scene> m_editorScene;
    EditorGUI m_editorGui;
    PlayModeManager m_playMode;
    UndoSystem m_undo;
    Entity m_selectedEntity;
    std::string m_activeScenePath;

    // Project asset database and import/cook pipeline used by Content Browser.
    AssetRegistry m_assetRegistry;
    std::unique_ptr<AssetPipeline> m_assetPipeline;
    std::unique_ptr<AssetHotReloadService> m_assetHotReload;

    // Play World runtime: the play scene is ticked (physics) and rendered in
    // the viewport during Play/Simulate, so the authored scene runs visibly.
    Physics::PhysicsRuntime m_playPhysics;
    std::unordered_map<UUID, Physics::BodyHandle> m_playBodies;
    // Play-world weapons: one WeaponRuntime per WeaponComponent entity, fired
    // with the viewport camera ray (SPACE) against the play physics (Fase 8).
    std::unordered_map<UUID, Engine::WeaponRuntime> m_playWeapons;
    bool m_playWeaponStatusLogged{ false };
    // Play-world particles: one ParticleSimulation emitter per
    // ParticleEmitterComponent entity, fed the play physics for collisions.
    Engine::Gameplay::ParticleSimulation m_playParticles;
    std::unordered_map<UUID, std::size_t> m_playEmitters;
    // Play-world vehicles: one VehicleRuntime per VehicleComponent entity with
    // a chassis body in the play physics; driven with the arrow keys.
    std::unordered_map<UUID, Engine::Gameplay::VehicleRuntime> m_playVehicles;
    std::unordered_map<UUID, Physics::BodyHandle> m_playVehicleChassis;
    // Play-world ragdolls (Fase 6): one Ragdoll per RagdollComponent entity,
    // built from the skin skeleton when fromSkeleton is set.
    std::unordered_map<UUID, Physics::Ragdoll> m_playRagdolls;
    // Play-world missions (Fase 8): one Mission per MissionComponent entity
    // (Start -> SetObjective -> WaitForEvent(completeEvent) -> Complete).
    Engine::Gameplay::MissionSystem m_playMissions;
    std::unordered_map<UUID, std::string> m_playMissionIds;
    // Play-world dialogues (Fase 8): one one-node DialogueGraph per
    // DialogueComponent entity; played on start when playOnStart is set.
    Engine::Gameplay::DialogueSystem m_playDialogues;
    std::unordered_map<UUID, std::string> m_playDialogueIds;
    // Play-world audio (Fase 8): the referenced .ogg is decoded into an
    // AudioClip and played through the Audio::Mixer (spatial against the
    // camera listener); rendered each frame so voices advance.
    Engine::Audio::Mixer m_playAudio;
    std::unordered_map<UUID, Engine::Audio::VoiceId> m_playVoices;
    // Play-world destructibles (Fase 8): one DestructibleRuntime per
    // DestructionComponent entity; weapon hits apply radial damage.
    std::unordered_map<UUID, Engine::Gameplay::DestructibleRuntime> m_playDestructibles;
    // Play-world navigation (Fase 8): the public INavigationProvider (Recast
    // + Detour, the promoted authority) bakes from columns of the play
    // physics bodies; one PlayNavAgent per NavigationComponent entity chases
    // the camera entity and writes its position back to the transform. The
    // legacy grid track (NavigationGrid/NavigationWorld) was removed
    // (FALTANTES item 12).
    std::unique_ptr<engine::navigation::INavigationProvider> m_playNav;
    std::unordered_map<UUID, PlayNavAgent> m_playNavAgents;
    void setup_play_runtime();
    void tick_play_runtime(float deltaTime);
    void teardown_play_runtime();
    // Play-mode visual script: runs <Content>/Scenes/Initial.script (if it
    // exists) with per-frame hot reload — editing the file while playing
    // recompiles and swaps the program live.
    ScriptVM m_playScript;
    ScriptHotReloader m_playScriptReloader;
    std::filesystem::path m_playScriptPath;
    bool m_playScriptLoaded{ false };
    // Script Debugger panel (Fase 7): attaches to the play-mode VM, lists the
    // compiled bytecode with breakpoint markers, drives pause/step/continue
    // and shows variables, call stack and watch expressions live.
    Scripting::ScriptDebugger m_scriptDebugger;
    ScriptGraphAsset m_scriptDebugGraph;
    bool m_scriptDebuggerAttached{ false };
    bool m_showScriptDebugger{ false };
    bool m_scriptPauseRequested{ false };

    // Script Canvas panel (Fase 7 — authoring gráfico profundo): the scene's
    // .script is edited as a typed-pin node graph through VisualScriptCanvas
    // and written back; play mode hot-reloads the file live.
    VisualScriptCanvas m_scriptCanvas;
    bool m_showScriptCanvas{ false };
    bool m_scriptCanvasLoaded{ false };
    UUID m_canvasDragPin{ 0, 0 };        // output pin currently being dragged
    glm::vec2 m_canvasDragPinPos{ 0.0f };
    std::string m_canvasAddKind{ "Event" };
    std::filesystem::path m_scriptCanvasPath;

    void load_script_canvas();
    void save_script_canvas();
    void add_canvas_node(const std::string& kind, glm::vec2 worldPos);
    void draw_script_canvas_panel();
    void draw_script_debugger_panel();

    // Viewport render graph: the scene pass (offscreen color + depth) is now
    // driven by VulkanRenderGraphExecutor — the same executor the game frame
    // uses — so the editor viewport and the game share the compiled-graph
    // path instead of a hand-written render pass.
    Rendering::RenderGraph m_viewportRenderGraph;
    Rendering::VulkanRenderGraphExecutor m_viewportRenderGraphExecutor;
    Rendering::RenderPassId m_viewportScenePass{};
    bool m_viewportRenderGraphBuilt{ false };
    void build_viewport_render_graph();
    void record_viewport_scene_content(VkCommandBuffer cmd);

    // VC_EDITOR_TEST_RENDERGRAPH=1: drives a two-pass render graph (Scene →
    // Composite) through VulkanRenderGraphExecutor on the real device and
    // verifies the compiled order + barriers are recorded.
    int run_render_graph_self_test();
    int run_hdr_texture_self_test();

    // Optional voxel tools are absent from non-voxel projects/builds.
#if VC_ENABLE_VOXEL_PLUGIN
    VoxelBrushOperation m_activeVoxelBrush;
#endif
    Editor::SpecializedEditorsPanel m_specializedEditors;

    // Profiler & Stats
    float m_fps{ 60.0f };
    float m_frameTimeMs{ 16.6f };
    size_t m_ramUsageMb{ 240 };
};

} // namespace Engine

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>

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
#include "../engine/physics/PhysicsAdvanced.hpp"
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
#include "EditorControlApi.hpp"
#include "WindowClamp.hpp"
#include "tools/WickedToolsPanel.hpp"

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
enum class GizmoMode { Select, Translate, Rotate, Scale };
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
    float farPlane{ 50000.0f };
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
    // Resolve target, 1x: the final image ImGui shows.
    VkImage colorImage{ VK_NULL_HANDLE };
    VkDeviceMemory colorMemory{ VK_NULL_HANDLE };
    VkImageView colorView{ VK_NULL_HANDLE };

    // Scene color, multisampled (m_viewportSamples).
    VkImage msaaColorImage{ VK_NULL_HANDLE };
    VkDeviceMemory msaaColorMemory{ VK_NULL_HANDLE };
    VkImageView msaaColorView{ VK_NULL_HANDLE };

    // Scene depth, multisampled (m_viewportSamples).
    VkImage depthImage{ VK_NULL_HANDLE };
    VkDeviceMemory depthMemory{ VK_NULL_HANDLE };
    VkImageView depthView{ VK_NULL_HANDLE };

    // Pick buffer (color-ID pass) — stays 1x.
    VkImage pickImage{ VK_NULL_HANDLE };
    VkDeviceMemory pickMemory{ VK_NULL_HANDLE };
    VkImageView pickView{ VK_NULL_HANDLE };
    VkImage pickDepthImage{ VK_NULL_HANDLE };
    VkDeviceMemory pickDepthMemory{ VK_NULL_HANDLE };
    VkImageView pickDepthView{ VK_NULL_HANDLE };

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
    // Fog parameters (from WeatherComponent)
    glm::vec4 fogParams; // x=density, y=start, z=heightFog(0/1), w=unused
    glm::vec4 fogColor;  // xyz=fog color, w=unused
    // Model matrix: the vertex shader computes the WORLD position
    // (fragWorldPos = model * inPosition) so fog distance and rim lighting
    // use real world coordinates, not the mesh's local space.
    glm::mat4 model;
};

// -----------------------------------------------------------------------
// Push constant for the analytic infinite grid (80 bytes, no VBO). The CPU
// hands the inverse view-projection (ray unprojection) and the camera
// position, so the shader never inverts matrices per vertex.
// -----------------------------------------------------------------------
struct GridPushConstants {
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
};

class EditorApplication {
public:
    EditorApplication();
    ~EditorApplication();

    // Loopback HTTP control API (curl http://127.0.0.1:8321/play etc.) so the
    // editor can be driven from a terminal or an agent without mouse clicks.
    EditorControlApi m_controlApi;

    // Result of the last Control-API command executed on the main thread:
    // empty = success, non-empty = human-readable error. The HTTP thread waits
    // for this before answering, so the agent gets the REAL outcome.
    std::string m_controlResult;
    // Optional success payload (e.g. a screenshot path) sent in the response.
    std::string m_controlData;

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
    // Shared executor for Control API commands (loopback HTTP + UI buttons).
    void handle_control_command(const std::string& cmd);
    // Captures the current offscreen viewport (previous frame) to a PNG file
    // via WIC. Returns empty string on success, or an error message. Used by
    // the Control-API `screenshot` command so an agent can SEE the result.
    std::string capture_viewport_screenshot(const std::string& path);
    // Runs one headless self-test (spawns this exe with the test env var,
    // waits for it, returns "PASS"/"FAIL"/error). 0=RenderGraph 1=HDR
    // 2=Material 3=Play 4=Build.
    std::string run_editor_self_test(int which);
    // Last self-test result, published to the Control API /state.
    std::string m_lastSelfTestResult;
    // Standalone packaging: cooks nothing, packages every cooked asset into
    // Intermediate/Package (same AssetPackager the Build uses). Returns a
    // status line.
    std::string package_assets_only();
    void recompute_editor_camera_position();
    void process_viewport_input();

    // Vulkan helpers
    uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                      VkMemoryPropertyFlags memProps, VkImage& image, VkDeviceMemory& memory,
                      uint32_t mipLevels = 1,
                      VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    VkImageView create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                  uint32_t mipLevels = 1);
    void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       VkBuffer& buffer, VkDeviceMemory& memory);
    void destroy_buffer(GPUBuffer& buffer);
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
    void draw_app_bar();
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
    // MSAA sample count for the viewport (clamped to device support). The pick
    // pass and the shadow map stay 1x.
    VkSampleCountFlagBits m_viewportSamples{ VK_SAMPLE_COUNT_4_BIT };
    std::string m_gpuName{ "Unknown GPU" };
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

    // Analytic infinite grid (fullscreen triangle, no vertex buffer)
    VkShaderModule m_gridVertShader{ VK_NULL_HANDLE };
    VkShaderModule m_gridFragShader{ VK_NULL_HANDLE };
    VkPipeline m_gridPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout m_gridPipelineLayout{ VK_NULL_HANDLE };

    // Runtime-wired Wicked-port rendering (frontend port): hair strands,
    // gaussian splat clouds, env-probe reflective sphere and the decal quad.
    VkPipeline m_hairPipeline{ VK_NULL_HANDLE };
    VkShaderModule m_splatVertShader{ VK_NULL_HANDLE };
    VkShaderModule m_splatFragShader{ VK_NULL_HANDLE };
    VkPipeline m_splatPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout m_splatPipelineLayout{ VK_NULL_HANDLE };
    VkShaderModule m_envSphereVertShader{ VK_NULL_HANDLE };
    VkShaderModule m_envSphereFragShader{ VK_NULL_HANDLE };
    VkPipeline m_envSpherePipeline{ VK_NULL_HANDLE };
    VkPipelineLayout m_envSpherePipelineLayout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout m_envSphereDescLayout{ VK_NULL_HANDLE };
    VkDescriptorPool m_envSphereDescPool{ VK_NULL_HANDLE };
    GPUBuffer m_envSphereVB;
    GPUBuffer m_envSphereIB;
    uint32_t m_envSphereIndexCount{ 0 };
    GPUBuffer m_decalVB;
    GPUBuffer m_decalIB;
    uint32_t m_decalIndexCount{ 0 };
    struct SplatPushConstants {
        glm::mat4 mvp;
        glm::vec4 pointSize;  // x = px size, y = viewport height, z = opacity
    };
    struct EnvSpherePushConstants {
        glm::mat4 mvp;
        glm::mat4 model;
        glm::vec4 camPos;
    };

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
    // Raw mouse-wheel accumulator fed by our own GLFW scroll callback (chained
    // before ImGui's). io.MouseWheel is zeroed at the end of ImGui::NewFrame,
    // which runs AFTER the camera update — so the camera reads it too late.
    // This accumulator is readable at any point and is consumed by the camera.
    double m_scrollAccum{ 0.0 };
    bool m_viewportFocused{ false };
    ImVec2 m_viewportPos{ 0, 0 };
    ImVec2 m_viewportSize{ 800, 600 };
    bool m_rightMouseDown{ false };
    bool m_middleMouseDown{ false };
    glm::vec2 m_lastMousePos{ 0, 0 };
    float m_deltaTime{ 0.016f };

    // Gizmo State
    GizmoMode m_gizmoMode{ GizmoMode::Translate };
    bool m_gizmoLocal{ false }; // World/Local: gizmo axes follow entity rotation
    // Viewport display toggles (⋯ overflow menu in the viewport toolbar).
    bool m_showGrid{ true };
    bool m_showGizmos{ true };
    bool m_showColliders{ true };
    // Set by the Ctrl+K shortcut; consumed by the app bar search field.
    bool m_focusGlobalSearch{ false };
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
    // Panel content size (screen coords) - the offscreen target is sized to
    // match this so the rendered image fills the panel at the same aspect.
    ImVec2 m_viewportPanelSize{ 0, 0 };

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
        // CPU copy of the geometry (kept for vertex painting raycasts and
        // paint-buffer rebuilds; meshes are small enough to hold in RAM).
        std::vector<glm::vec3> cpuPositions;
        std::vector<uint32_t> cpuIndices;
        // Object-space bounds (computed at load) — used to frame the 3D
        // thumbnail camera and to auto-fit new entities.
        glm::vec3 boundsMin{ 0.0f };
        glm::vec3 boundsMax{ 0.0f };
        bool hasBounds{ false };
    };
    std::unordered_map<UUID, EditorMeshResource> m_meshResources;
    std::unordered_set<UUID> m_meshLoadFailed;

    bool load_mesh_resource(const UUID& assetId);
    const EditorMeshResource* get_mesh_resource(const UUID& assetId);
    void draw_mesh_resource(VkCommandBuffer cmd, const glm::mat4& mvp, const glm::vec4& color,
                            const EditorMeshResource& resource,
                            const glm::mat4& model = glm::mat4(1.0f));
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
        // Parallel to `textures`: true when the texture is a block per-face
        // atlas owned by m_blockAtlasTextures (the pipeline only references it
        // and must NOT destroy it).
        std::vector<bool> textureIsAtlas;
        uint64_t graphHash{ 0 };
        // Content hash of the sampled texture when the pipeline was built, so a
        // reimported texture rebuilds the pipeline instead of sampling a stale
        // GPU copy.
        uint64_t textureContentHash{ 0 };
        std::string lastError;
        bool valid{ false };
    };
    // maxDim > 0 box-downscales before upload: the Content Browser requests a
    // small thumbnail (192 px) instead of uploading the full-res texture.
    bool load_viewport_texture(const UUID& assetId, GraphTexture& out, std::string& error,
                               uint32_t maxDim = 0);
    bool upload_texture_half_pixels(uint32_t width, uint32_t height,
                                    const std::vector<uint8_t>& halfRgba,
                                    GraphTexture& out, std::string& error);
    bool upload_texture_pixels(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                               uint32_t mipCount, bool srgb, GraphTexture& out, std::string& error);
    void destroy_graph_texture(GraphTexture& t);

    // Asset previews (Content Browser): lazy GPU thumbnails for cooked
    // textures and a single active audio preview voice.
    struct AssetThumbnail {
        VkImage image{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        VkImageView view{ VK_NULL_HANDLE };
        VkDescriptorSet imguiId{ VK_NULL_HANDLE };
    };
    std::unordered_map<UUID, AssetThumbnail> m_assetThumbnails;
    // Content hash per cached thumbnail: reimports/hot reloads invalidate the
    // stale GPU preview instead of showing the old image forever.
    std::unordered_map<UUID, std::uint64_t> m_assetThumbnailHashes;
    std::unordered_set<UUID> m_assetThumbnailFailed;
    // Async texture thumbnails: the cooked file is decoded + downscaled on a
    // worker thread (one at a time); the main thread uploads one small image
    // per frame. Only assets whose cards are visible in the browser grid are
    // ever requested (lazy loading), so scrolling a huge folder never decodes
    // the whole list or stalls the frame.
    struct PendingThumbDecode {
        UUID assetId{ 0, 0 };
        uint32_t width = 0, height = 0;
        bool srgb = false;
        bool halfFloat = false;
        std::vector<uint8_t> rgba;
    };
    std::deque<UUID> m_thumbDecodeQueue;
    std::unordered_set<UUID> m_thumbDecodeRequested;
    std::mutex m_thumbDecodeMutex;
    std::atomic<bool> m_thumbDecodeBusy{ false };
    std::optional<PendingThumbDecode> m_thumbDecodeReady;
    std::thread m_thumbDecodeThread;   // joinable worker (not detached)
    void request_asset_thumbnail_decode(const AssetMetadata& asset);
    void pump_asset_thumbnail_decodes();
    void join_worker_threads();         // join before shutdown
    Engine::Audio::VoiceId m_audioPreviewVoice{ 0 };
    UUID m_audioPreviewAsset{ 0, 0 };
    std::thread m_audioDecodeThread;   // joinable worker (not detached)
    void toggle_audio_preview(const AssetMetadata& asset);
    void destroy_asset_thumbnails();

    // 3D asset thumbnails (Content Browser): a small offscreen renders each
    // cooked mesh / block cube once, cached as an AssetThumbnail. Pending
    // assets are processed a few per frame so the grid never stalls.
    VkImage m_thumbImage{ VK_NULL_HANDLE };
    VkDeviceMemory m_thumbMemory{ VK_NULL_HANDLE };
    VkImageView m_thumbView{ VK_NULL_HANDLE };
    VkImage m_thumbMsaaImage{ VK_NULL_HANDLE };
    VkDeviceMemory m_thumbMsaaMemory{ VK_NULL_HANDLE };
    VkImageView m_thumbMsaaView{ VK_NULL_HANDLE };
    VkImage m_thumbDepthImage{ VK_NULL_HANDLE };
    VkDeviceMemory m_thumbDepthMemory{ VK_NULL_HANDLE };
    VkImageView m_thumbDepthView{ VK_NULL_HANDLE };
    VkFramebuffer m_thumbFramebuffer{ VK_NULL_HANDLE };
    uint32_t m_thumbSize{ 256 };
    std::deque<UUID> m_thumbnailQueue;
    std::unordered_set<UUID> m_thumbnailQueued;
    std::unordered_map<UUID, VkDescriptorSet> m_asset3dThumbnails;
    std::unordered_map<UUID, std::uint64_t> m_asset3dThumbnailHashes; // content hash per 3D thumbnail
    void init_thumbnail_target();
    void destroy_thumbnail_target();
    void request_3d_thumbnail(const UUID& assetId);
    void pump_asset_thumbnails(int budget);
    void render_mesh_thumbnail(const UUID& assetId, const EditorMeshResource& mesh);
    void render_block_thumbnail(const UUID& assetId, VkDescriptorSet textureDesc);
    void render_character_thumbnail(const UUID& assetId, const EditorMeshResource& mesh);

    // Textured unit cube (block pipeline): renders Minecraft-style block
    // models assembled from a PNG texture, both as thumbnails and (via the
    // scene) as cube previews of the block asset.
    VkShaderModule m_blockVertShader{ VK_NULL_HANDLE };
    VkShaderModule m_blockFragShader{ VK_NULL_HANDLE };
    VkPipeline m_blockPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout m_blockPipelineLayout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout m_blockDescSetLayout{ VK_NULL_HANDLE };
    // Dedicated pool for block descriptors (never the ImGui pool, which is
    // reset every frame and invalidated these sets).
    VkDescriptorPool m_blockDescPool{ VK_NULL_HANDLE };
    VkSampler m_blockSampler{ VK_NULL_HANDLE };
    // NEAREST-filtered sampler for block atlases on the voxel/block draw path
    // (the material-graph pipelines sample blocks pixel-art; skins/decals/PBR
    // keep the trilinear anisotropic m_offscreen.sampler).
    VkSampler m_blockDrawSampler{ VK_NULL_HANDLE };
    GPUBuffer m_blockCubeVB;
    GPUBuffer m_blockCubeIB;
    uint32_t m_blockCubeIndexCount{ 0 };
    std::unordered_map<UUID, VkDescriptorSet> m_blockDescriptors;
    std::unordered_map<UUID, GraphTexture> m_blockTextures; // keeps the views alive
    std::unordered_map<UUID, std::uint64_t> m_blockTextureHashes; // content hash per block texture
    void init_block_cube();
    void destroy_block_cube();
    VkDescriptorSet get_block_descriptor(const UUID& textureAsset);
    // Per-face block atlas: builds/caches a 3-wide image [top | side | bottom]
    // from the .vblock face maps so the cube samples per-face textures (grass
    // top / grass side / dirt bottom). Missing faces fall back to the main
    // texture. Keyed by block asset UUID + content hash (hot-reload aware).
    std::unordered_map<UUID, GraphTexture> m_blockAtlasTextures;
    std::unordered_map<UUID, uint64_t> m_blockAtlasHashes;
    bool ensure_block_atlas(const UUID& blockId, GraphTexture& out);

    // Minecraft-style block model recognition: a small square power-of-two
    // texture is treated as a block face. "Criar Modelo de Bloco" writes a
    // .vblock sidecar (texture UUIDs per face) and registers an
    // AssetType::Block asset, which appears under the Modelos filter.
    [[nodiscard]] bool looks_like_block_texture(const AssetMetadata& meta) const;
    // A texture is a block when it looks like one (square POT 8-256) OR has an
    // existing .vblock sidecar referencing it (cached: the registry scan only
    // runs once per texture UUID). The PNG is the single user-visible entry;
    // .vblock sidecars are hidden plumbing that scenes still resolve.
    [[nodiscard]] bool is_block_texture(const AssetMetadata& meta);
    std::unordered_set<UUID> m_blockSidecarChecked;
    std::unordered_set<UUID> m_blockTextureSet;
    // Explicit "not a block" override: a <texture>.noblock marker file next
    // to the PNG (written by "Desmarcar como Bloco") beats both the heuristic
    // and any sidecar, so a misclassified character/mob skin stays a texture.
    std::unordered_set<UUID> m_noblockTextures;
    std::unordered_set<UUID> m_noblockChecked; // one stat() per texture UUID
    // Auxiliary material maps (_n, _s, _normal, _spec… ) are never blocks;
    // m_auxBlockHealed remembers which ones already had their bogus sidecars
    // removed (one registry scan per texture UUID).
    [[nodiscard]] bool is_aux_map_texture(const AssetMetadata& meta) const;
    void heal_aux_block_sidecars(const AssetMetadata& textureMeta);
    // One-time pass after indexing: base blocks created before material-map
    // grouping existed get their sibling _n/_s textures recorded in the
    // .vblock sidecar (normal/specular), so the whole material set belongs to
    // the block asset.
    void enrich_block_material_maps();
    std::unordered_set<UUID> m_auxBlockHealed;
    // Minecraft character/mob skins are also square POT (player 64x64, mobs
    // 64x64...): entity/mob path + filename signals classify them as MODELS,
    // not blocks (resource-pack block folders like /textures/block/ win).
    [[nodiscard]] bool is_character_texture(const AssetMetadata& meta) const;
    // User override: remove the .vblock sidecar so a texture stops being a
    // block (a misclassified character/mob skin). The sidecar file + registry
    // entry are deleted; the texture becomes a plain texture again.
    void unmark_block_texture(const AssetMetadata& textureMeta);
    // Find-or-create the .vblock sidecar for a texture: the PNG IS the block
    // (Minecraft-style), so repeated drops/clicks reuse the same asset instead
    // of piling up duplicates. Returns the block asset UUID (invalid on failure).
    UUID create_block_asset(const AssetMetadata& textureMeta);
    // Per-face authoring: rewrites the .vblock sidecar with explicit top/side/
    // bottom face textures. Invalid UUIDs (0) keep the current face. The atlas
    // cache is invalidated so the next render rebuilds it. Returns false if the
    // block asset doesn't exist or a given face UUID is not a registered texture.
    bool set_block_faces(const UUID& blockId, const UUID& top, const UUID& side,
                         const UUID& bottom);
    // Creates a NEW block asset whose .vblock sidecar carries per-face textures
    // (top/side/bottom). `base` is the fallback texture used on every face where
    // a specific texture isn't given. `name` seeds the .vblock filename. Returns
    // the new block asset UUID (invalid on failure).
    [[nodiscard]] UUID create_block_from_faces(const UUID& base, const UUID& top,
                                               const UUID& side, const UUID& bottom,
                                               const std::string& name);
    struct BlockAssetData {
        UUID texture{ 0, 0 }; // all faces default to this
        UUID top{ 0, 0 }, bottom{ 0, 0 }, side{ 0, 0 };
        // Material maps grouped into the block (aux sibling textures like
        // andesite_n.png / andesite_s.png) — stored in the .vblock sidecar.
        UUID normal{ 0, 0 }, specular{ 0, 0 };
    };
    std::unordered_map<UUID, BlockAssetData> m_blockAssetCache;
    std::unordered_set<UUID> m_blockAssetFailed;
    [[nodiscard]] bool load_block_asset(const UUID& blockAssetId, BlockAssetData& out);
    [[nodiscard]] UUID resolve_block_texture(const UUID& blockAssetId);
    // Block models in the scene: a block asset renders as a textured cube.
    // ensure_block_cube_resource builds the GPU cube mesh on demand (survives
    // restarts — the mesh is regenerated from geometry, no asset file needed).
    void ensure_block_cube_resource(const UUID& blockId);
    // Creates an entity whose MeshRenderer references a Block asset (rendered
    // as the textured cube in the editor and in play).
    void spawn_block_entity(const UUID& blockId, const glm::vec3& position);
    // Minecraft character/mob skins: the texture ITSELF is the character (no
    // sidecar file — avoids the PNG+duplicate problem). The humanoid mesh is
    // built on demand from the standard 64x64/64x32 skin UV layout, and the
    // skin PNG is sampled by a material-graph pipeline, exactly like blocks.
    void ensure_character_mesh_resource(const UUID& texId);
    void spawn_character_entity(const UUID& texId, const glm::vec3& position);
    // Batch import: scans a folder recursively and imports all compatible
    // assets (textures, meshes, audio, materials). Returns the count of
    // successfully imported files. Minecraft texture packs become blocks
    // automatically (square POT 8-256 textures get .vblock sidecars).
    size_t import_texture_pack(const std::filesystem::path& folder);
    // Shared material-graph pipeline that samples a single texture (used by
    // block cubes and character humanoids). Cached per texture UUID in the
    // given map and rebuilt when the graph hash changes.
    GraphMaterialPipeline* ensure_texture_pipeline(
        const UUID& texId, std::unordered_map<UUID, GraphMaterialPipeline>& cache,
        bool withAlpha = false);

    // Voxel sculpting (Escultura de Blocos): each VoxelVolumeComponent entity
    // gets an editable grid (Engine::Voxel::VoxelStructure) rendered as colored
    // cubes in the viewport. The brush panel paints into it in real time — the
    // panel used to be decorative (the brush was never consumed).
    struct EditorVoxelRange {
        uint32_t firstIndex{ 0 };
        uint32_t indexCount{ 0 };
        uint16_t type{ 0 };
        // Block asset sampled by this range (invalid = vertex-color fallback).
        UUID blockId{ 0, 0 };
    };
    struct EditorVoxelMesh {
        GPUBuffer vb;
        GPUBuffer ib;
        uint32_t indexCount{ 0 };
        std::vector<EditorVoxelRange> ranges;
        bool valid{ false };
    };
    std::unordered_map<UUID, std::unique_ptr<Engine::Voxel::VoxelStructure>> m_voxelStructures;
    std::unordered_map<UUID, EditorVoxelMesh> m_voxelMeshes;
    std::unordered_set<UUID> m_voxelMeshesDirty;
    bool m_voxelPaintMode{ false }; // "Pintar" toggle in the sculpt panel
    // Voxel type → Block asset override (API `voxel-block <type> <uuid>`).
    // Empty = auto-resolve by texture name from the BlockRegistry.
    std::unordered_map<uint16_t, UUID> m_voxelTypeBlocks;
    void ensure_voxel_volume(const UUID& entityId, uint32_t seed, float seaLevel);
    // Finds the Block asset to sample for a voxel type: explicit override
    // first, then a name match in the registry (1=dirt, 2=grass, 3=stone,
    // 4=water). Invalid UUID = render the type with vertex colors.
    UUID resolve_voxel_type_block(uint16_t type);
    // Pre-builds the textured block pipelines for every voxel volume in the
    // main loop, OUTSIDE the viewport render pass. Creating pipelines and
    // uploading atlases while a render pass is being recorded hangs the GPU
    // (device lost), so the draw path only ever hits the pipeline cache.
    void ensure_voxel_pipelines();
    void rebuild_voxel_mesh(const UUID& entityId);
    void draw_voxel_volumes(VkCommandBuffer cmd, const glm::mat4& viewProj, Scene* scene);
    void paint_voxel_ray(const glm::vec3& origin, const glm::vec3& dir, bool remove);
    void destroy_voxel_editor_meshes();

    // Playback sink for the play-in-editor mixer: a miniaudio device (pull
    // model) whose data callback renders the mixer each period. Kept opaque
    // (EditorAudioSink*, defined in the .cpp) so the header never has to
    // include miniaudio.h.
    void* m_audioDevice{ nullptr };
    bool m_audioDeviceStarted{ false };
    void init_audio_output();
    void shutdown_audio_output();

    // Async audio preview: the OGG decode runs on a worker thread (decode of
    // a long clip on the UI thread was freezing the whole editor); the result
    // is picked up by the main loop and played if the request is still active.
    struct PendingAudioDecode {
        UUID assetId{ 0, 0 };
        Engine::Audio::AudioBuffer buffer;
    };
    std::mutex m_audioDecodeMutex;
    std::optional<PendingAudioDecode> m_audioDecodeReady;
    std::atomic<bool> m_audioDecodeBusy{ false };
    std::unordered_set<UUID> m_audioPreviewDecodeFailed;
    UUID m_audioPreviewRequest{ 0, 0 };
    // Small bounded LRU of decoded previews so replaying an asset is instant.
    std::unordered_map<UUID, std::shared_ptr<const Engine::Audio::AudioBuffer>> m_audioPreviewCache;
    std::deque<UUID> m_audioPreviewCacheOrder;
    std::size_t m_audioPreviewCacheFrames{ 0 };
    void pump_audio_preview_decodes();
    void start_preview_voice(const UUID& assetId);
    void cache_audio_preview(const UUID& assetId, const Engine::Audio::AudioBuffer& buffer);
    std::unordered_map<UUID, GraphMaterialPipeline> m_graphMaterialPipelines;
    // Block-model cubes in the scene: pipeline cached per block asset UUID
    // (textured cube, rebuilt when the resolved texture changes).
    std::unordered_map<UUID, GraphMaterialPipeline> m_blockGraphPipelines;
    // Minecraft character/mob skins in the scene: textured humanoid pipelines,
    // cached per skin texture UUID (rebuilt when the texture changes).
    std::unordered_map<UUID, GraphMaterialPipeline> m_skinGraphPipelines;
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
    std::string m_hoverEntityName;  // shown as tooltip when hovering the viewport
    glm::vec2 m_hoverPickPixel{ 0, 0 };
    bool m_hoverPickPending{ false };

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

    // Panel visibility (Janelas menu). The EditorGUI duplicate panels are
    // disabled (see init_default_scene) — these flags drive the real panels.
    bool m_showHierarchy{ true };
    bool m_showInspector{ true };
    bool m_showViewport{ true };
    bool m_showContentBrowser{ true };
    bool m_contentBrowserDirty{ true }; // set when folder may have changed externally
    bool m_showConsole{ false };

    // Play-mode frame stepping (PASSO button): advance the play world one
    // frame while paused.
    bool m_stepRequested{ false };

    // Help > Sobre modal.
    bool m_showAboutDialog{ false };

    // New-scene flow: confirm save before discarding (modal) and name the scene.
    bool m_pendingNewSceneConfirm{ false };
    bool m_pendingNewSceneCreate{ false };
    char m_newSceneName[128]{ 'U', 'n', 't', 'i', 't', 'l', 'e', 'd', ' ', 'S', 'c', 'e', 'n', 'e', 0 };
    // Forge UI: Inspector advanced mode (shows UUIDs, near/far planes, raw
    // asset ids) and the Scene panel search filter.
    bool m_advancedInspector{ false };
    char m_hierarchySearch[128]{ 0 };

    // Windows file/folder pickers (Abrir Jogo / Procurar Pasta / Salvar Como).
    bool pick_file_dialog(std::string& outPath, const wchar_t* filter,
                          const wchar_t* title, const wchar_t* defExt);
    bool pick_save_file_dialog(std::string& outPath, const wchar_t* filter,
                               const wchar_t* title, const wchar_t* defExt);
    bool pick_folder_dialog(std::string& outPath, const wchar_t* title);

    // Build Game (README §39): cook → package → scene → shaders → exe.
    std::vector<std::string> m_buildLog;
    void run_game_build();

    // Load a .scene file into the editor (Abrir Jogo / Open Scene).
    void load_scene_file(const std::string& path);
    // Save the current scene; if it has no path yet, opens Salvar Como.
    void save_current_scene();
    void save_scene_as();
    // Project creation (Criador de Projetos panel): folder + empty scene.
    std::string create_project(const std::string& name, const std::string& folder);
    // Create a brand-new scene named m_newSceneName (after Novo Jogo confirm).
    void create_new_scene();
    // Scan Projects/ for the launcher list.
    struct LauncherProject {
        std::string name;
        std::string path;
        std::string lastModified;
        bool hasScene{ false };
    };
    void scan_projects(std::vector<LauncherProject>& out) const;

    // Editor Scene & State
    std::unique_ptr<Scene> m_editorScene;
    EditorGUI m_editorGui;
    // Wicked-port tool windows (frontend; PORTS.md).
    WickedToolsPanel m_wickedTools;
    PlayModeManager m_playMode;
    UndoSystem m_undo;
    Entity m_selectedEntity;
    std::string m_activeScenePath;

    // Scene autosave: every mutation calls mark_scene_dirty(); a debounced
    // autosave_scene() in the main loop persists the scene ~1.5s after the
    // last change, and a final flush runs on window close — closing the
    // engine without Ctrl+S never loses work.
    bool m_sceneDirty{ false };
    double m_sceneLastChange{ 0.0 };   // glfwGetTime() of the last mutation
    std::string m_autosavePath;        // stable fallback target for untitled scenes
    void mark_scene_dirty();
    void autosave_scene(bool force = false);

    // Project asset database and import/cook pipeline used by Content Browser.
    AssetRegistry m_assetRegistry;
    std::unique_ptr<AssetPipeline> m_assetPipeline;
    std::unique_ptr<AssetHotReloadService> m_assetHotReload;

    // Play World runtime: the play scene is ticked (physics) and rendered in
    // the viewport during Play/Simulate, so the authored scene runs visibly.
    Physics::PhysicsRuntime m_playPhysics;
    std::unordered_map<UUID, Physics::BodyHandle> m_playBodies;
    // Real world collision (play mode): static boxes derived from the actual
    // scene content — voxel volumes become exact merged-cell boxes, the
    // procedural terrain becomes sampled column boxes. The wide plane is only
    // a void-failsafe placed below the lowest real collider.
    std::vector<Physics::BodyHandle> m_playStaticBodies;
    float m_playCollisionFloorY{ -0.5f };
    void build_play_world_collision();
    // Static ground plane added to every play world so dynamic bodies land
    // instead of falling forever when a scene has no collidable content.
    Physics::BodyHandle m_playGroundBody{ Physics::InvalidBody };
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
    // Wicked-port runtime (formerly TODO(frontend-port)): constraints run as
    // soft force-based constraints (the runtime solver has no rigid-joint API),
    // springs hold a rest anchor, spline followers keep their progress; the
    // sky pass is driven by WeatherComponent.
    struct ConstraintRest {
        glm::vec3 anchorA{ 0.0f };
        glm::vec3 anchorB{ 0.0f };
        float restLength{ 0.0f };
        bool broken{ false };
    };
    std::unordered_map<UUID, ConstraintRest> m_constraintRests;
    std::unordered_map<UUID, glm::vec3> m_springRests;
    std::unordered_map<UUID, float> m_splineProgress;
    // Play-world animation (the Animation/Timeline/IK/Retarget editors now
    // Apply to the scene): timeline property tracks animate the entity's
    // transform, the Animation state machine samples clips into bone-entity
    // transforms, IK bends a two-bone chain to a target, and retargeting
    // copies mapped bone transforms between skeletons. Clips are cooked once
    // per play session (loaded from the asset registry by UUID).
    struct AnimationRuntimeState {
        float time{ 0.0f };
        std::string currentState;
        std::unordered_map<std::string, float> params;
    };
    std::unordered_map<UUID, AnimationRuntimeState> m_animStates;
    std::unordered_map<UUID, AnimationClip> m_animClips;
    void tick_animation_runtime(Scene* playScene, float deltaTime);

    // Runtime-wired Wicked-port simulation (frontend port): hair verlet
    // strands, soft-body cloth, video flipbooks, gaussian splats, vertex
    // painting and env-probe cubemap captures. tick_special_runtimes runs in
    // both Edit and Play so the editor previews the authored feature.
    struct HairSim {
        std::vector<glm::vec3> pos;
        std::vector<glm::vec3> prev;
        std::vector<glm::vec3> rest;
        GPUBuffer vb;
        uint32_t vertexCount{ 0 };
        bool built{ false };
    };
    std::unordered_map<UUID, HairSim> m_hairs;
    struct SoftBodySim {
        std::vector<glm::vec3> pos;
        std::vector<glm::vec3> prev;
        std::vector<glm::vec3> rest;
        std::vector<uint32_t> indices;
        GPUBuffer vb;
        GPUBuffer ib;
        uint32_t indexCount{ 0 };
        bool built{ false };
    };
    std::unordered_map<UUID, SoftBodySim> m_softBodies;
    // Video flipbook: texture pipelines keyed by frame-texture UUID, cached so
    // frames swap without recompiling (textures downscaled on load).
    std::unordered_map<UUID, GraphMaterialPipeline> m_videoGraphPipelines;
    // Vertex painting: per-entity GPU buffers rebuilt when the colors change.
    struct PaintData {
        GPUBuffer vb;
        uint32_t vertexCount{ 0 };
        bool dirty{ true };
    };
    std::unordered_map<UUID, PaintData> m_paintBuffers;
    bool m_paintToolActive{ false };
    bool m_paintBrushDown{ false };
    // Gaussian splat clouds: cached per-entity GPU buffers, rebuilt when the
    // parameters change or regenerate is requested.
    struct SplatCloud {
        GPUBuffer vb;
        uint32_t count{ 0 };
        float scale{ 0.0f };
        uint32_t seed{ 0 };
        bool dirty{ true };
    };
    std::unordered_map<UUID, SplatCloud> m_splatClouds;
    // Env probe cubemap capture (one shared capture target, re-captured for
    // the active probe on demand or periodically when realTime).
    struct EnvProbeCapture {
        VkImage image{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        VkImageView views[6]{};
        VkFramebuffer framebuffers[6]{};
        VkSampler sampler{ VK_NULL_HANDLE };
        VkDescriptorSet descriptorSet{ VK_NULL_HANDLE };
        VkRenderPass renderPass{ VK_NULL_HANDLE };
        uint32_t size{ 0 };
        UUID entity{ 0, 0 };
        bool valid{ false };
    } m_envCapture;
    // Extra capture resources kept alive for the capture's lifetime.
    VkImageView m_envCubeView{ VK_NULL_HANDLE };
    VkImageView m_envDepthView{ VK_NULL_HANDLE };
    VkImage m_envDepthImage{ VK_NULL_HANDLE };
    VkDeviceMemory m_envDepthMemory{ VK_NULL_HANDLE };
    bool m_envCapturePending{ false };
    float m_envCaptureTimer{ 0.0f };
    void tick_special_runtimes(Scene* scene, float deltaTime);
    void ensure_hair_sim(const UUID& id, const HairParticleComponent& h, const TransformComponent& t);
    void upload_hair(HairSim& sim, const HairParticleComponent& h);
    void ensure_softbody_sim(const UUID& id, const SoftBodyComponent& s, const TransformComponent& t);
    void upload_softbody(SoftBodySim& sim, const SoftBodyComponent& s);
    void rebuild_paint_buffer(const UUID& id, PaintComponent& pc, const EditorMeshResource* mesh);
    void generate_splat_cloud(const GaussianSplatComponent& gs, std::vector<EditorVertex>& verts) const;
    void capture_env_probe(const UUID& id, const EnvProbeComponent& ep, const TransformComponent& t);
    void record_env_face(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj,
                         const glm::vec3& pos, Scene* scene);
    void record_env_capture(VkCommandBuffer cmd, Scene* scene);
    UUID resolve_texture_asset_by_name(const std::string& name) const;
    glm::vec3 viewport_mouse_dir(const glm::vec2& mouseScreen) const;
    bool paint_mesh_stroke(const glm::vec3& origin, const glm::vec3& dir);
    float m_skyTime{ 0.0f };
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
    // Sky pass (Clima panel): procedural day/night sky driven by the scene's
    // first WeatherComponent + directional sun. Drawn first in the viewport.
    struct SkyPushConstants {
        glm::mat4 mvp;
        glm::vec4 cameraPos;
        glm::vec4 sunDirection;
        glm::vec4 sunColor;
        glm::vec4 environment; // x = time, y = daylight, z = unused, w = unused
    };
    VkShaderModule m_skyVertShader{ VK_NULL_HANDLE };
    VkShaderModule m_skyFragShader{ VK_NULL_HANDLE };
    VkPipeline m_skyPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout m_skyPipelineLayout{ VK_NULL_HANDLE };

    // Terrain (Terreno panel): single procedural heightmap mesh in the scene,
    // regenerated by the panel (noise scale/octaves/amount/falloff).
    struct TerrainParams {
        // `scale` is the world size of the base noise feature. 1.0 turns a
        // 1000-unit sheet into high-frequency static ("spikes"); ~120 gives
        // gentle rolling hills across the same sheet.
        float scale{ 120.0f };
        int octaves{ 5 };
        float amount{ 0.5f };
        float falloff{ 0.4f };
        float halfExtent{ 500.0f };
        int segments{ 256 };
        // Deterministic noise seed (hash mixing), so an agent can reproduce
        // an exact terrain with the same seed + parameters.
        uint32_t seed{ 1 };
    };
    GPUBuffer m_terrainVB;
    GPUBuffer m_terrainIB;
    uint32_t m_terrainIndexCount{ 0 };
    bool m_terrainValid{ false };
    TerrainParams m_terrainParams;
    void generate_terrain_mesh(const TerrainParams& params);
    // Terrain survives reloads via a small sidecar ("<scene>.terrain"): the
    // scene serializer owns entity data only, so the heightmap parameters are
    // written next to the scene on every save and restored on load.
    static std::filesystem::path terrain_sidecar_path(const std::string& scenePath);
    void persist_terrain_sidecar(const std::string& scenePath);
    void restore_terrain_sidecar(const std::string& scenePath);
    void clear_terrain_mesh();

    // Editor settings persisted to settings.json (Opções Gerais / Tema).
    std::string m_settingsPath;
    void load_settings();
    void save_settings();

    // Graphics (Opções Gráficas): VSync (swapchain present mode) and shadow
    // map resolution (512/1024/2048/4096). The *_recreate flags are processed
    // at the top of the next frame (before acquire), so the Vulkan resources
    // are recreated while nothing is in flight.
    bool m_vsyncEnabled{ true };
    int m_shadowQuality{ 2 };
    bool m_recreateSwapchain{ false };
    bool m_recreateShadowMap{ false };
    void apply_graphics_settings(bool vsync, int quality);
    uint32_t shadow_size_from_quality(int quality) const;

    // Mesh panel (Malha): recompute/flip normals on the selected entity's
    // mesh asset (CPU geometry → GPU re-upload → cooked file rewrite).
    std::string apply_mesh_normals(int mode);
    bool m_meshEdited{ false };

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
    // Frontend port (Wicked toolbar): voxel sculpting panel visibility, toggled
    // by the toolbar button (our own button — the donor's toolbar has none).
    bool m_showVoxelTools{ true };
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

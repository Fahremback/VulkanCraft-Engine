#pragma once

#include "VulkanGameSupport.hpp"

#include "engine/scene/Scene.hpp"
#include "engine/scene/Components.hpp"
#include "engine/assets/GltfGeometry.hpp"
#include "engine/assets/GltfAssets.hpp"
#include "engine/assets/RuntimePackage.hpp"
#include "engine/rendering/vulkan/MaterialPipeline.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/Ragdoll.hpp"
#include "engine/animation/AnimationRuntime.hpp"
#include "engine/scripting/ScriptRuntime.hpp"
#include "engine/audio/AudioRuntime.hpp"
#include "engine/gameplay/WeaponSystem.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <chrono>
#include <optional>
#include <set>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <algorithm>

using namespace Engine;
using namespace Engine::Rendering;

inline constexpr uint32_t WIDTH = 1280;
inline constexpr uint32_t HEIGHT = 720;
// Shadow map: a 2x2 atlas of per-cascade tiles (shadow cascades). The single
// image stays kShadowMapSize^2 and each cascade owns a kShadowCascadeSize^2 tile.
const uint32_t kShadowMapSize = 2048;
const uint32_t kShadowCascadeSize = 1024;
const uint32_t kGameShadowCascades = 4;

// Volumetric light-shafts UBO (std140), written every frame. The fragment
// shader ray-marches the view ray against the scene depth and the sun shadow
// map to accumulate scattered sunlight (god rays).
struct VolumetricParams {
    glm::vec4 cameraPosition;
    glm::mat4 invViewProj;
    glm::vec4 sunDirection;   // xyz = toward the sun, w = sun active
    glm::vec4 sunColor;       // xyz = color (normalized), w = shaft intensity
    glm::mat4 sunViewProj;
    glm::vec4 shadowParams;   // x = shadow enabled, y = bias, z = cascade count
    glm::mat4 cameraViewProj;
    glm::vec4 nearFar;        // x = near, y = far, z = density, w = steps
    glm::mat4 sunCascadeVP[kGameShadowCascades];
    glm::vec4 sunCascadeSplits;   // xyz = view-depth split points, w = cascade count
    glm::vec4 cameraForward;      // xyz = camera forward (cascade selection)
};
constexpr uint32_t kVolumetricSteps = 32;
constexpr float kVolumetricDensity = 0.04f;
constexpr float kVolumetricIntensity = 0.5f;

// Skinned mesh (Fase 6): bone matrices buffer capacity for the skinned
// pipeline (generated shader declares mat4 bones[N]; 64 * 64B = 4KB, fits the
// minimum UBO limit).
constexpr uint32_t kBoneCount = 64;

// Vertex layout shared with the editor viewport (editor_material.vert):
// pos(0), normal(1), color(2), uv(3). Skinned meshes add joints(4)/weights(5)
// at the tail so existing attribute offsets stay valid.
struct GameVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 color;
    glm::vec2 uv;
    glm::uvec4 joints{ 0, 0, 0, 0 };
    glm::vec4 weights{ 1.0f, 0.0f, 0.0f, 0.0f };
};

struct MeshRange {
    uint32_t firstIndex{ 0 };
    uint32_t indexCount{ 0 };
    uint32_t vertexOffset{ 0 };
    bool indexed{ false };
};

struct GameMeshResource {
    VkBuffer vb{ VK_NULL_HANDLE };
    VmaAllocation vbAlloc{};
    VkBuffer ib{ VK_NULL_HANDLE };
    VmaAllocation ibAlloc{};
    std::vector<MeshRange> ranges;
    // Skinned mesh (Fase 6): primitives carry JOINTS_0/WEIGHTS_0 and the file
    // embeds a skeleton; such meshes draw through the skinned pipeline.
    bool skinned{ false };
    SkeletonAsset skeleton;
    bool valid{ false };
};


class VulkanGame {
public:
    void run();

private:
    GLFWwindow* window;
    VkInstance instance;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VkQueue graphicsQueue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapChain;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImage> swapChainImages;
    std::vector<VkImageView> swapChainImageViews;
    VkRenderPass renderPass;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;
    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;
    VkBuffer uboBuffer;
    VmaAllocation uboAllocation;
    VkBuffer lightBuffer;
    VmaAllocation lightAllocation;
    VkImage depthImage;
    VmaAllocation depthAllocation;
    VkImageView depthImageView;
    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    std::vector<VkFence> imagesInFlight;
    VmaAllocator allocator;
    uint32_t currentFrame = 0;
    bool framebufferResized = false;

    // Free-fly camera (WASD + right-drag look). Initialized from the scene's
    // first CameraComponent so authored cameras frame the scene.
    glm::vec3 camPos{ 0.0f, 1.0f, -6.0f };
    float camYaw{ 0.0f };
    float camPitch{ 0.0f };
    bool cameraDragging{ false };
    double lastMouseX{ 0.0 };
    double lastMouseY{ 0.0 };
    glm::vec3 cameraFront() const;

    Scene scene{ "MainScene" };
    Physics::PhysicsRuntime physics;
    ScriptVM scriptVM;
    ScriptGraphAsset gameplayGraph;
    bool gameplayLoaded{ false };
    UUID playerEntityId{ 0, 0 };
    bool gameplayStatusLogged{ false };

    // Hitscan weapon wired to the physics backend (Fase 8): fires from the
    // camera, raycasts via PhysicsRuntime, and feeds ammo/reload + OnShoot/
    // OnHit events into the gameplay script each frame.
    Engine::WeaponRuntime weapon{ Engine::WeaponDefinition{
        UUID{ 0, 7 }, "Assault Rifle", Engine::FireMode::Automatic, 30, 90, 3,
        600.0f, 1.5f, 25.0f, 120.0f, 1.5f, true } };
    bool mouseLeftHeld{ false };
    bool mouseLeftPrev{ false };
    uint32_t weaponHitsSeen{ 0 };
    UUID muzzleLightEntity{ 0, 0 };
    float muzzleFlashTimer{ 0.0f };
    bool weaponStatusLogged{ false };
    bool weaponHitLogged{ false };

    // FPS mode (Fase FPS): captured-cursor mouse-look, destructible target
    // crates on a physics floor, and a live ammo/targets/FPS HUD in the title.
    bool mouseLookEnabled{ true };
    bool f9WasPressed{ false };
    struct FpsTarget {
        UUID entity{ 0, 0 };
        Physics::BodyHandle body{ Physics::InvalidBody };
        int hits{ 0 };
        bool alive{ true };
    };
    std::vector<FpsTarget> fpsTargets;
    int targetsDestroyed{ 0 };
    Physics::BodyHandle lastHitBody{ Physics::InvalidBody };
    glm::vec3 lastHitPoint{ 0.0f };
    double hudTimer{ 0.0 };

    Audio::Mixer audioMixer;
    RuntimeAssetPackage content;
    std::unordered_map<UUID, GameMeshResource> meshes;
    std::unordered_set<UUID> meshLoadFailed;
    GameMeshResource cubeMesh;
    bool cubeBuilt{ false };

    // Render graph frame (Scene → Composite) driven by the executor.
    Rendering::RenderGraph renderGraph;
    Rendering::VulkanRenderGraphExecutor renderGraphExecutor;
    VkRenderPass compositeRenderPass{ VK_NULL_HANDLE };
    VkImage hdrImage{ VK_NULL_HANDLE };
    VmaAllocation hdrAllocation{};
    VkImageView hdrImageView{ VK_NULL_HANDLE };
    VkSampler compositeSampler{ VK_NULL_HANDLE };
    VkFramebuffer sceneFramebuffer{ VK_NULL_HANDLE };
    std::vector<VkFramebuffer> compositeFramebuffers;
    VkPipelineLayout compositePipelineLayout{ VK_NULL_HANDLE };
    VkPipeline compositePipeline{ VK_NULL_HANDLE };
    VkDescriptorSetLayout compositeSetLayout{ VK_NULL_HANDLE };
    VkDescriptorPool compositeDescriptorPool{ VK_NULL_HANDLE };
    VkDescriptorSet compositeDescriptorSet{ VK_NULL_HANDLE };
    bool renderGraphBuilt{ false };
    VkDebugUtilsMessengerEXT debugMessenger{ VK_NULL_HANDLE };

    // Shared runtime helpers implemented in VulkanGameRuntime.cpp.
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data,
                      VkBuffer& buffer, VmaAllocation& allocation);
    void drawMeshResource(VkCommandBuffer cb, const GameMeshResource& mesh,
                          const glm::mat4& mvp, const glm::mat4& model);

    // Shadow map for the directional sun (Shadow → Scene → Composite).
    VkImage shadowMapImage{ VK_NULL_HANDLE };
    VmaAllocation shadowMapAllocation{};
    VkImageView shadowMapView{ VK_NULL_HANDLE };
    VkSampler shadowMapSampler{ VK_NULL_HANDLE };
    VkRenderPass shadowRenderPass{ VK_NULL_HANDLE };
    VkFramebuffer shadowFramebuffer{ VK_NULL_HANDLE };
    VkPipelineLayout shadowPipelineLayout{ VK_NULL_HANDLE };
    VkPipeline shadowPipeline{ VK_NULL_HANDLE };
    glm::mat4 sunViewProj{ 1.0f };
    glm::mat4 sunCascadeVP[kGameShadowCascades]{ glm::mat4(1.0f) };
    glm::vec4 sunCascadeSplits{ 0.0f, 0.0f, 0.0f, static_cast<float>(kGameShadowCascades) };
    bool sunEnabled{ false };
    bool sunShadowStatusLogged{ false };
    bool lightStatusLogged{ false };
    float shadowBias{ 0.002f };

    // Volumetric light shafts: fullscreen pass that ray-marches the scene
    // depth against the sun shadow map into a half... full-res HDR target,
    // added on top by the Composite pass.
    VkImage volumImage{ VK_NULL_HANDLE };
    VmaAllocation volumAllocation{};
    VkImageView volumImageView{ VK_NULL_HANDLE };
    VkSampler volumSampler{ VK_NULL_HANDLE };
    VkSampler volumDepthSampler{ VK_NULL_HANDLE };
    VkRenderPass volumRenderPass{ VK_NULL_HANDLE };
    VkFramebuffer volumFramebuffer{ VK_NULL_HANDLE };
    VkPipelineLayout volumPipelineLayout{ VK_NULL_HANDLE };
    VkPipeline volumPipeline{ VK_NULL_HANDLE };
    VkDescriptorSetLayout volumSetLayout{ VK_NULL_HANDLE };
    VkDescriptorPool volumDescriptorPool{ VK_NULL_HANDLE };
    VkDescriptorSet volumDescriptorSet{ VK_NULL_HANDLE };
    VkBuffer volumUboBuffer{ VK_NULL_HANDLE };
    VmaAllocation volumUboAllocation{};

    // Skinned mesh (Fase 6): a procedural two-bone "flag" driven through the
    // GpuSkinningBuffer path (bone matrices → UBO → generated vertex shader →
    // real pipeline), lit by the same LightParams UBO as the material pass.
    VkPipeline skinnedPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout skinnedPipelineLayout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout skinnedSetLayout{ VK_NULL_HANDLE };
    VkDescriptorPool skinnedDescriptorPool{ VK_NULL_HANDLE };
    VkDescriptorSet skinnedDescriptorSet{ VK_NULL_HANDLE };
    VkBuffer skinnedBoneBuffer{ VK_NULL_HANDLE };
    VmaAllocation skinnedBoneAllocation{};
    VkBuffer skinnedCameraBuffer{ VK_NULL_HANDLE };
    VmaAllocation skinnedCameraAllocation{};
    VkBuffer skinnedVb{ VK_NULL_HANDLE };
    VmaAllocation skinnedVbAllocation{};
    VkBuffer skinnedIb{ VK_NULL_HANDLE };
    VmaAllocation skinnedIbAllocation{};
    uint32_t skinnedVertexCount{ 0 };
    uint32_t skinnedIndexCount{ 0 };
    SkeletonAsset skinnedSkeleton;
    AnimationClip skinnedClip;
    float skinnedAnimTime{ 0.0f };

    // Ragdoll (Fase 6): the flag's two bones become physics bodies joined by
    // a distance constraint; the physics pose blends with the clip per frame
    // and drives the skinned pipeline (ragdoll coupled to the physics backend).
    Physics::Ragdoll ragdoll;
    bool ragdollBuilt{ false };
    bool ragdollImpulseApplied{ false };
    bool ragdollTipLogged{ false };
    float ragdollBlendWeight{ 0.8f };
    bool skinnedBuilt{ false };
    bool skinnedLogged{ false };

    void initWindow() ;

    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);

    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/);
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    void initVulkan() ;

    void initScene();

    void createInstance() ;

    void setupDebugMessenger() ;

    void destroyDebugMessenger() ;

    void createSurface() ;

    void pickPhysicalDevice() ;

    void createLogicalDevice() ;

    void createAllocator() ;

    void createSwapChain() ;

    void createImageViews() ;

    void createDepthResources() ;

    void createRenderPass() ;

    void createFramebuffers() ;

    void createCommandPool() ;

    void createSyncObjects() ;

    VkShaderModule createShaderModule(const std::string& path) ;

    void initPipelines();

    // Depth-only shadow pass: renders the scene from the sun's view into the
    // shadow map (sampled by the Scene pass lighting).
    void buildShadowResources();

    // Volumetric light-shafts pipeline: fullscreen pass that ray-marches each
    // view ray against the scene depth and the sun shadow map, accumulating
    // scattered sunlight into an HDR target the Composite pass adds on top.
    void buildVolumetricResources();

    // Skinned mesh support (Fase 6): a procedural two-bone "flag" drawn by a
    // real skinned pipeline — bone matrices computed per frame from an
    // animated Pose, packed by GpuSkinningBuffer and uploaded to a UBO the
    // generated vertex shader consumes. Lit by the shared LightParams UBO.
    void buildSkinnedResources();

    // Fullscreen-triangle composite pipeline: samples the offscreen HDR scene
    // color (descriptor binding 0) and writes it to the swapchain.
    void buildCompositePipeline();
    // Builds the game's render graph (Scene → Composite) and wires the
    // executor to the real framebuffers/passes: the graph's compiled order and
    // barriers now drive the frame.
    void buildRenderGraph();
    void drawComposite(VkCommandBuffer cb);
    VkShaderModule createModuleFromSpirv(const std::vector<uint32_t>& spirv);
    // ─── Mesh loading: cooked .vcmesh (v1 or v2) from the content package ───
    GameMeshResource* getMesh(const UUID& assetId);
    

    // Procedural cube used when an entity has no cooked mesh asset (built once).
    GameMeshResource buildCubeMesh();

    // Writes the PBR material params into the std140 UBO (names sorted as the
    // generated GLSL declares them: Albedo, Emissive, Metallic, Roughness).
    void writeMaterialUbo(const MaterialComponent& material);
    

    glm::mat4 modelFromTransform(const TransformComponent& t) const;

    // Fits one light-space ortho per cascade around the camera frustum slices
    // and stores the view-depth split points the shaders use for selection.
    // Split distances blend logarithmic and uniform schemes (practical split
    // scheme, lambda = 0.5); each ortho is texel-snapped for temporal stability.
    void computeShadowCascades(const glm::mat4& lightView);

    // Computes the sun's cascade view-projections from the scene's directional
    // LightComponent. Called once per frame before the UBO writes.
    void updateSunShadow();
    // Shadow pass draw: renders the scene depth-only into each cascade's tile
    // of the atlas (2x2 grid), each with its own light view-projection.
    void drawShadowPass(VkCommandBuffer cb);
    // Fills the LightParams UBO from the scene's LightComponents (range >= 50
    // ⇒ directional sun; otherwise a point light) + the camera position + the
    // shadow map data (view-projection + bias).
    void writeLightUbo(const glm::vec3& camPos);

    glm::mat4 computeCameraView() const;

    glm::mat4 computeCameraProj() const;

    // Fills the VolumetricParams UBO for the light-shafts pass: camera
    // matrices, sun data (normalized color so shafts don't blow out at high
    // intensities) and the march parameters.
    void writeVolumetricUbo();
    void drawVolumetric(VkCommandBuffer cb);
    void drawScene(VkCommandBuffer cb);
    // Skinned mesh pass (Fase 6): uploads packed bone matrices to the UBO the
    // generated vertex shader consumes and draws with the skinned pipeline.
    void drawSkinnedMesh(VkCommandBuffer cb, VkBuffer vb, VkBuffer ib,
                         const std::vector<MeshRange>& ranges,
                         const SkeletonAsset& skeleton, const Pose& pose) {
        if (!skinnedBuilt) return;
        const std::vector<glm::mat4> matrices =
            GpuSkinningBuffer::compute_bone_matrices(skeleton, pose);
        std::vector<float> packed = GpuSkinningBuffer::pack(matrices);
        if (packed.size() > kBoneCount * 16) packed.resize(kBoneCount * 16);
        packed.resize(kBoneCount * 16, 0.0f);
        void* mapped = nullptr;
        vmaMapMemory(allocator, skinnedBoneAllocation, &mapped);
        std::memcpy(mapped, packed.data(), packed.size() * sizeof(float));
        vmaUnmapMemory(allocator, skinnedBoneAllocation);

        const glm::mat4 viewProj = computeCameraProj() * computeCameraView();
        vmaMapMemory(allocator, skinnedCameraAllocation, &mapped);
        std::memcpy(mapped, &viewProj, sizeof(viewProj));
        vmaUnmapMemory(allocator, skinnedCameraAllocation);

        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipelineLayout,
                                0, 1, &skinnedDescriptorSet, 0, nullptr);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &offset);
        if (ib != VK_NULL_HANDLE) vkCmdBindIndexBuffer(cb, ib, 0, VK_INDEX_TYPE_UINT32);
        for (const MeshRange& range : ranges) {
            if (range.indexed && ib != VK_NULL_HANDLE)
                vkCmdDrawIndexed(cb, range.indexCount, 1, range.firstIndex, 0, 0);
            else
                vkCmdDraw(cb, range.indexCount, 1, range.vertexOffset, 0);
        }
    }

    // Procedural two-bone flag demo: keeps a visible skinned mesh on screen
    // even without a skinned asset in Content. The pose comes from the clip
    // sampled through AnimationSampler, blended with the ragdoll's physics
    // pose (ragdoll coupled to the physics backend) → bone matrices → UBO.
    void drawSkinned(VkCommandBuffer cb);

    void destroyMeshResource(GameMeshResource& mesh);
    void mainLoop();
    // Builds the gameplay script: an authored .script next to the scene wins;
    // otherwise a built-in controller accumulates WASD (moveX/moveY) into the
    // player's position (playerX/playerZ). Runs through the real ScriptVM.
    void setupGameplay();

    // Feeds WASD into the script as moveX/moveY, ticks the controller and
    // applies the resulting playerX/playerZ to the Player entity's transform.
    void updateGameplay(float dt);
    // FPS targets: a static physics floor plus dynamic crate bodies with
    // visible mesh entities. The hitscan weapon knocks them; 3 hits destroy a
    // crate (body removed, mesh hidden, HUD counter updated).
    void spawnTargets();

    // FPS HUD: ammo/reserve, targets remaining and live FPS in the window title.
    void updateHud(float dt);

    void setupWeapon();

    void updateWeapon(float dt);

    void update(float dt);
    void drawFrame();
    void cleanup();
};

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
#define VMA_IMPLEMENTATION
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

const uint32_t WIDTH = 1280;
const uint32_t HEIGHT = 720;
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
    void run() {
        initWindow();
        initVulkan();
        initScene();
        spawnTargets();
        setupWeapon();
        initPipelines();
        mainLoop();
        cleanup();
    }

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
    glm::vec3 cameraFront() const {
        const float yaw = glm::radians(camYaw);
        const float pitch = glm::radians(camPitch);
        return glm::normalize(glm::vec3(
            std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)));
    }

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

    void initWindow() {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(WIDTH, HEIGHT, "VulkanCraft - Game Runtime", nullptr, nullptr);
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetCursorPosCallback(window, cursorPosCallback);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        // FPS mode: capture the cursor so raw mouse deltas drive the look.
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }

    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
        auto app = reinterpret_cast<VulkanGame*>(glfwGetWindowUserPointer(window));
        if (app->cameraDragging || app->mouseLookEnabled) {
            const double dx = xpos - app->lastMouseX;
            const double dy = ypos - app->lastMouseY;
            app->camYaw += static_cast<float>(dx) * 0.15f;
            app->camPitch = glm::clamp(app->camPitch - static_cast<float>(dy) * 0.15f, -89.0f, 89.0f);
        }
        app->lastMouseX = xpos;
        app->lastMouseY = ypos;
    }

    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/) {
        auto app = reinterpret_cast<VulkanGame*>(glfwGetWindowUserPointer(window));
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            app->cameraDragging = action == GLFW_PRESS;
        } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
            app->mouseLeftHeld = action == GLFW_PRESS;
        }
    }

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<VulkanGame*>(glfwGetWindowUserPointer(window));
        app->framebufferResized = true;
    }

    void initVulkan() {
        createInstance();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createAllocator();
        createSwapChain();
        createImageViews();
        createDepthResources();
        createRenderPass();
        createFramebuffers();
        createCommandPool();
        createSyncObjects();
    }

    void initScene() {
        // The packaged game ships Content/Scenes/Initial.scene (written by the
        // editor's Build Game step). Development fallbacks: the editor's active
        // scene file, then a small built-in scene.
        bool loaded = false;
        for (const char* candidate : { "Content/Scenes/Initial.scene", "assets/scenes/active_world.scene" }) {
            if (scene.load_from_file(candidate)) {
                std::cout << "[Game] Loaded scene: " << candidate << '\n';
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            std::cout << "[Game] No scene file found; using built-in scene\n";
            auto cam = scene.create_entity("Camera");
            scene.cameraComponents[cam.get_id()] = CameraComponent{};
            auto transform = TransformComponent{};
            transform.position = glm::vec3(0.0f, 1.0f, -6.0f);
            scene.transformComponents[cam.get_id()] = transform;

            auto cube = scene.create_entity("Player");
            scene.meshRendererComponents[cube.get_id()] = MeshRendererComponent{};
            scene.materialComponents[cube.get_id()] = MaterialComponent{
                glm::vec3(0.9f, 0.3f, 0.2f), 0.4f, 0.1f, glm::vec3(0.0f), 0.0f };
            scene.transformComponents[cube.get_id()] = TransformComponent{};
            playerEntityId = cube.get_id();

            // Directional sun: drives the real lighting + shadow map path.
            auto sun = scene.create_entity("Sun");
            scene.lightComponents[sun.get_id()] = LightComponent{
                glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
            scene.transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);

            // Spot light: cone of 25° inner / 45° outer pointing down at the
            // cube — exercises the spotLight* arrays in the LightParams UBO.
            auto spot = scene.create_entity("Spot");
            scene.lightComponents[spot.get_id()] = LightComponent{
                glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
            scene.transformComponents[spot.get_id()].position = glm::vec3(2.5f, 3.0f, 0.0f);
            scene.transformComponents[spot.get_id()].rotation = glm::vec3(-90.0f, 0.0f, 0.0f);

            // Area light: a 4x2 rectangle emitter facing the cube.
            auto area = scene.create_entity("Area");
            scene.lightComponents[area.get_id()] = LightComponent{
                glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
            scene.transformComponents[area.get_id()].position = glm::vec3(-3.0f, 2.5f, 1.0f);
            scene.transformComponents[area.get_id()].rotation = glm::vec3(0.0f, 90.0f, 0.0f);

            auto rb = Physics::BodyDesc{};
            rb.motion = Physics::MotionType::Dynamic;
            rb.position = glm::vec3(0, 10, 0);
            physics.create_body(rb);
        }
        // Player entity for the script-driven controller: an authored "Player"
        // entity in a scene file, or the built-in cube in the fallback scene.
        for (const auto& [id, entity] : scene.get_entities()) {
            if (entity.get_name() == "Player") { playerEntityId = id; break; }
        }
        setupGameplay();
        // Camera starts at the authored camera entity (if any).
        for (const auto& [id, cam] : scene.cameraComponents) {
            (void)cam;
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                camPos = tit->second.position;
                camYaw = tit->second.rotation.y;
                camPitch = tit->second.rotation.x;
            }
            break;
        }
        // Mount the cooked content package so mesh/material assets resolve by
        // UUID. The packaged game runs from the package root (AssetManifest.txt
        // lives there); the dev fallback location is Content/.
        std::string error;
        if (content.mount(".", &error) || content.mount("Content", &error)) {
            std::cout << "[Game] Mounted Content package (" << content.assets().size() << " asset(s))\n";
        } else {
            std::cout << "[Game] No content package found (running without cooked assets)\n";
        }
    }

    void createInstance() {
        // VC_GAME_VALIDATION=1 enables the validation layers + a debug
        // messenger (messages go to stderr) — used for headless verification.
        const bool validate = std::getenv("VC_GAME_VALIDATION") != nullptr;

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "VulkanCraft Game";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "VulkanCraft Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        if (validate) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = 0;
        std::vector<const char*> layers;
        if (validate) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
            createInfo.ppEnabledLayerNames = layers.data();
        }

        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("failed to create instance!");
        }
        if (validate) setupDebugMessenger();
    }

    void setupDebugMessenger() {
        const auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!createFn) return;
        VkDebugUtilsMessengerCreateInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                  VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                  void* /*userData*/) -> VkBool32 {
            (void)severity;
            std::cerr << "[Game Validation] " << data->pMessage << std::endl;
            return VK_FALSE;
        };
        createFn(instance, &info, nullptr, &debugMessenger);
    }

    void destroyDebugMessenger() {
        if (debugMessenger == VK_NULL_HANDLE) return;
        const auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }

    void createSurface() {
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface!");
        }
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) throw std::runtime_error("failed to find GPUs with Vulkan support!");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        physicalDevice = devices[0];
    }

    void createLogicalDevice() {
        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = 0;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceFeatures deviceFeatures{};

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;

        const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
            throw std::runtime_error("failed to create logical device!");
        }
        vkGetDeviceQueue(device, 0, 0, &graphicsQueue);
    }

    void createAllocator() {
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        vmaCreateAllocator(&allocatorInfo, &allocator);
    }

    void createSwapChain() {
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = 2;
        createInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        createInfo.imageExtent = { WIDTH, HEIGHT };
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        createInfo.clipped = VK_TRUE;

        if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            throw std::runtime_error("failed to create swap chain!");
        }

        uint32_t imageCount;
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());
        swapChainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        swapChainExtent = { WIDTH, HEIGHT };
    }

    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create image views!");
            }
        }
    }

    void createDepthResources() {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapChainExtent.width;
        imageInfo.extent.height = swapChainExtent.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; // sampled by the Volumetric pass
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &depthImage, &depthAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create depth image!");
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &depthImageView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create depth image view!");
        }

        // HDR scene color target: written by the Scene pass, sampled by the
        // Composite pass (the render graph's first real frame resource).
        VkImageCreateInfo hdrInfo{};
        hdrInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        hdrInfo.imageType = VK_IMAGE_TYPE_2D;
        hdrInfo.extent = { swapChainExtent.width, swapChainExtent.height, 1 };
        hdrInfo.mipLevels = 1;
        hdrInfo.arrayLayers = 1;
        hdrInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        hdrInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        hdrInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        hdrInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        hdrInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        hdrInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo hdrAllocInfo{};
        hdrAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(allocator, &hdrInfo, &hdrAllocInfo, &hdrImage, &hdrAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create HDR image!");
        }
        VkImageViewCreateInfo hdrViewInfo{};
        hdrViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        hdrViewInfo.image = hdrImage;
        hdrViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        hdrViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        hdrViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hdrViewInfo.subresourceRange.baseMipLevel = 0;
        hdrViewInfo.subresourceRange.levelCount = 1;
        hdrViewInfo.subresourceRange.baseArrayLayer = 0;
        hdrViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &hdrViewInfo, nullptr, &hdrImageView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create HDR image view!");
        }

        // Shadow map for the directional sun (depth written by the Shadow pass,
        // sampled by the Scene pass's lighting).
        VkImageCreateInfo shadowInfo{};
        shadowInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        shadowInfo.imageType = VK_IMAGE_TYPE_2D;
        shadowInfo.extent = { kShadowMapSize, kShadowMapSize, 1 };
        shadowInfo.mipLevels = 1;
        shadowInfo.arrayLayers = 1;
        shadowInfo.format = VK_FORMAT_D32_SFLOAT;
        shadowInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        shadowInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        shadowInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        shadowInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        shadowInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo shadowAllocInfo{};
        shadowAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(allocator, &shadowInfo, &shadowAllocInfo,
                           &shadowMapImage, &shadowMapAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow map image!");
        }
        VkImageViewCreateInfo shadowViewInfo{};
        shadowViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        shadowViewInfo.image = shadowMapImage;
        shadowViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        shadowViewInfo.format = VK_FORMAT_D32_SFLOAT;
        shadowViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        shadowViewInfo.subresourceRange.baseMipLevel = 0;
        shadowViewInfo.subresourceRange.levelCount = 1;
        shadowViewInfo.subresourceRange.baseArrayLayer = 0;
        shadowViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &shadowViewInfo, nullptr, &shadowMapView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow map image view!");
        }
        // Sampler (nearest + clamp; single-tap depth compare in the shader).
        // Created here so the descriptor writes in initPipelines can bind it.
        VkSamplerCreateInfo shadowSamplerInfo{};
        shadowSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        shadowSamplerInfo.magFilter = VK_FILTER_NEAREST;
        shadowSamplerInfo.minFilter = VK_FILTER_NEAREST;
        shadowSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        shadowSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        shadowSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &shadowSamplerInfo, nullptr, &shadowMapSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow map sampler");
        }

        // Volumetric light-shaft target: full-res HDR color written by the
        // Volumetric pass, sampled by the Composite pass.
        VkImageCreateInfo volumInfo{};
        volumInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        volumInfo.imageType = VK_IMAGE_TYPE_2D;
        volumInfo.extent = { swapChainExtent.width, swapChainExtent.height, 1 };
        volumInfo.mipLevels = 1;
        volumInfo.arrayLayers = 1;
        volumInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        volumInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        volumInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        volumInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        volumInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        volumInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo volumAllocInfo{};
        volumAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(allocator, &volumInfo, &volumAllocInfo, &volumImage, &volumAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric image!");
        }
        VkImageViewCreateInfo volumViewInfo{};
        volumViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        volumViewInfo.image = volumImage;
        volumViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        volumViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        volumViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        volumViewInfo.subresourceRange.baseMipLevel = 0;
        volumViewInfo.subresourceRange.levelCount = 1;
        volumViewInfo.subresourceRange.baseArrayLayer = 0;
        volumViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &volumViewInfo, nullptr, &volumImageView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric image view!");
        }
        // Samplers: linear + clamp for the shaft target, and a plain (non-
        // comparison) sampler to read the scene depth values.
        VkSamplerCreateInfo volumSamplerInfo{};
        volumSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        volumSamplerInfo.magFilter = VK_FILTER_LINEAR;
        volumSamplerInfo.minFilter = VK_FILTER_LINEAR;
        volumSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        volumSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        volumSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &volumSamplerInfo, nullptr, &volumSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric sampler");
        }
        VkSamplerCreateInfo depthSamplerInfo = volumSamplerInfo;
        if (vkCreateSampler(device, &depthSamplerInfo, nullptr, &volumDepthSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric depth sampler");
        }
    }

    void createRenderPass() {
        // Scene pass: renders the 3D scene into the offscreen HDR target with
        // depth. The color attachment finishes in SHADER_READ_ONLY so the
        // Composite pass can sample it.
        VkAttachmentDescription sceneColor{};
        sceneColor.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        sceneColor.samples = VK_SAMPLE_COUNT_1_BIT;
        sceneColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sceneColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sceneColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        sceneColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        sceneColor.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sceneColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentDescription sceneDepth{};
        sceneDepth.format = VK_FORMAT_D32_SFLOAT;
        sceneDepth.samples = VK_SAMPLE_COUNT_1_BIT;
        sceneDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sceneDepth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        sceneDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        sceneDepth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        sceneDepth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sceneDepth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // sampled by the Volumetric pass

        VkAttachmentReference sceneColorRef{};
        sceneColorRef.attachment = 0;
        sceneColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference sceneDepthRef{};
        sceneDepthRef.attachment = 1;
        sceneDepthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sceneSubpass{};
        sceneSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sceneSubpass.colorAttachmentCount = 1;
        sceneSubpass.pColorAttachments = &sceneColorRef;
        sceneSubpass.pDepthStencilAttachment = &sceneDepthRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkAttachmentDescription sceneAttachments[] = { sceneColor, sceneDepth };
        VkRenderPassCreateInfo scenePassInfo{};
        scenePassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        scenePassInfo.attachmentCount = 2;
        scenePassInfo.pAttachments = sceneAttachments;
        scenePassInfo.subpassCount = 1;
        scenePassInfo.pSubpasses = &sceneSubpass;
        scenePassInfo.dependencyCount = 1;
        scenePassInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device, &scenePassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create scene render pass!");
        }

        // Composite pass: copies the sampled HDR result to the swapchain.
        VkAttachmentDescription compositeColor{};
        compositeColor.format = swapChainImageFormat;
        compositeColor.samples = VK_SAMPLE_COUNT_1_BIT;
        compositeColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        compositeColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        compositeColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        compositeColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        compositeColor.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        compositeColor.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference compositeColorRef{};
        compositeColorRef.attachment = 0;
        compositeColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription compositeSubpass{};
        compositeSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        compositeSubpass.colorAttachmentCount = 1;
        compositeSubpass.pColorAttachments = &compositeColorRef;

        // Composite has no depth attachment: color-only dependency.
        VkSubpassDependency presentDependency{};
        presentDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        presentDependency.dstSubpass = 0;
        presentDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo compositePassInfo{};
        compositePassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        compositePassInfo.attachmentCount = 1;
        compositePassInfo.pAttachments = &compositeColor;
        compositePassInfo.subpassCount = 1;
        compositePassInfo.pSubpasses = &compositeSubpass;
        compositePassInfo.dependencyCount = 1;
        compositePassInfo.pDependencies = &presentDependency;
        if (vkCreateRenderPass(device, &compositePassInfo, nullptr, &compositeRenderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite render pass!");
        }

        // Volumetric pass: writes the light-shaft HDR target (color only),
        // left in SHADER_READ_ONLY so the Composite pass samples it.
        VkAttachmentDescription volumColor{};
        volumColor.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        volumColor.samples = VK_SAMPLE_COUNT_1_BIT;
        volumColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        volumColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        volumColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        volumColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        volumColor.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        volumColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference volumColorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription volumSubpass{};
        volumSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        volumSubpass.colorAttachmentCount = 1;
        volumSubpass.pColorAttachments = &volumColorRef;
        VkSubpassDependency volumDependency{};
        volumDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        volumDependency.dstSubpass = 0;
        volumDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        volumDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        volumDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo volumPassInfo{};
        volumPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        volumPassInfo.attachmentCount = 1;
        volumPassInfo.pAttachments = &volumColor;
        volumPassInfo.subpassCount = 1;
        volumPassInfo.pSubpasses = &volumSubpass;
        volumPassInfo.dependencyCount = 1;
        volumPassInfo.pDependencies = &volumDependency;
        if (vkCreateRenderPass(device, &volumPassInfo, nullptr, &volumRenderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric render pass!");
        }
    }

    void createFramebuffers() {
        // Scene pass framebuffer: HDR color + depth (shared by every frame).
        VkImageView sceneAttachments[] = { hdrImageView, depthImageView };
        VkFramebufferCreateInfo sceneFbInfo{};
        sceneFbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        sceneFbInfo.renderPass = renderPass;
        sceneFbInfo.attachmentCount = 2;
        sceneFbInfo.pAttachments = sceneAttachments;
        sceneFbInfo.width = swapChainExtent.width;
        sceneFbInfo.height = swapChainExtent.height;
        sceneFbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &sceneFbInfo, nullptr, &sceneFramebuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create scene framebuffer!");
        }

        // Composite pass framebuffers: one per swapchain image (color only).
        compositeFramebuffers.resize(swapChainImageViews.size());
        for (size_t i = 0; i < swapChainImageViews.size(); i++) {
            VkImageView attachments[] = { swapChainImageViews[i] };
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = compositeRenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = swapChainExtent.width;
            framebufferInfo.height = swapChainExtent.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &compositeFramebuffers[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create composite framebuffer!");
            }
        }

        // Volumetric pass framebuffer: the light-shaft target (shared).
        VkImageView volumAttachments[] = { volumImageView };
        VkFramebufferCreateInfo volumFbInfo{};
        volumFbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        volumFbInfo.renderPass = volumRenderPass;
        volumFbInfo.attachmentCount = 1;
        volumFbInfo.pAttachments = volumAttachments;
        volumFbInfo.width = swapChainExtent.width;
        volumFbInfo.height = swapChainExtent.height;
        volumFbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &volumFbInfo, nullptr, &volumFramebuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric framebuffer!");
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = 0;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create command pool!");
        }
        commandBuffers.resize(2);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();
        if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate command buffers!");
        }
    }

    void createSyncObjects() {
        // One slot per swapchain image (not a fixed 2): reusing a semaphore
        // while the swapchain still references it trips VUID-00067, so the
        // in-flight window must cover every image the swapchain can hand out.
        const size_t slotCount = std::max<size_t>(swapChainImages.size(), 2);
        imageAvailableSemaphores.resize(slotCount);
        renderFinishedSemaphores.resize(slotCount);
        inFlightFences.resize(slotCount);
        imagesInFlight.assign(swapChainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (size_t i = 0; i < slotCount; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create synchronization objects for a frame!");
            }
        }
    }

    VkShaderModule createShaderModule(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("failed to read shader: " + path);
        in.seekg(0, std::ios::end);
        const std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);
        if (size <= 0 || size % 4 != 0) throw std::runtime_error("invalid SPIR-V size: " + path);
        std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
        in.read(reinterpret_cast<char*>(code.data()), size);
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size() * sizeof(uint32_t);
        createInfo.pCode = code.data();
        VkShaderModule module;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shader module: " + path);
        }
        return module;
    }

    void initPipelines() {
        // Material-graph fragment shader: standard PBR graph → GLSL → SPIR-V
        // (same pipeline the editor viewport uses).
        MaterialAsset defaults;
        const MaterialGraph graph = material_graph_from_pbr(defaults);
        const GlslGenerationResult gen = material_graph_to_glsl(graph);
        if (!gen) throw std::runtime_error("material graph GLSL generation failed");
        const std::vector<uint32_t> fragSpv = compile_glsl_to_spirv(gen.source, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (fragSpv.empty()) throw std::runtime_error("glslc failed to compile the material graph shader (is glslc on PATH?)");

        // Vertex shader: shared editor_material.vert (MVP push constant, vUv out).
        std::string vertPath = std::string(VULKANCRAFT_SHADER_DIR) + "/editor_material.vert.spv";
        std::ifstream probe(vertPath, std::ios::binary);
        if (!probe) vertPath = "Content/Shaders/editor_material.vert.spv";
        const VkShaderModule vertModule = createShaderModule(vertPath);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // Descriptor set: binding 0 = material params UBO; the generated
        // shader's binding = LightParams UBO (lights from LightComponents);
        // then the shadow-map sampler.
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = gen.lightUboBinding;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = gen.shadowSamplerBinding;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor set layout");
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(Rendering::MaterialPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create pipeline layout");
        }

        // UBO: std140 layout of the PBR graph parameters (sorted by name):
        // Albedo vec3 @0, Emissive vec3 @16, Metallic float @32, Roughness @36 → 48 bytes.
        VkDeviceSize uboSize = 48;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = uboSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &uboBuffer, &uboAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create UBO");
        }

        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 2;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = descriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &descriptorSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &descriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate descriptor set");
        }
        VkDescriptorBufferInfo descInfo{ uboBuffer, 0, uboSize };
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &descInfo;
        // LightParams UBO (second descriptor).
        VkBufferCreateInfo lightBufferInfo{};
        lightBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        lightBufferInfo.size = sizeof(Rendering::LightUboData);
        lightBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        lightBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo lightAllocInfo{};
        lightAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &lightBufferInfo, &lightAllocInfo,
                            &lightBuffer, &lightAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create light UBO");
        }
        VkDescriptorBufferInfo lightDescInfo{ lightBuffer, 0, sizeof(Rendering::LightUboData) };
        VkWriteDescriptorSet lightWrite{};
        lightWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        lightWrite.dstSet = descriptorSet;
        lightWrite.dstBinding = gen.lightUboBinding;
        lightWrite.descriptorCount = 1;
        lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lightWrite.pBufferInfo = &lightDescInfo;
        VkDescriptorImageInfo shadowImageInfo{ shadowMapSampler, shadowMapView,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = descriptorSet;
        shadowWrite.dstBinding = gen.shadowSamplerBinding;
        shadowWrite.descriptorCount = 1;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.pImageInfo = &shadowImageInfo;
        VkWriteDescriptorSet writes[3] = { write, lightWrite, shadowWrite };
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        // Graphics pipeline.
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GameVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[4]{};
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, pos)) };
        attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, normal)) };
        attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, color)) };
        attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, uv)) };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 4;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);

        buildCompositePipeline();
        buildShadowResources();
        buildVolumetricResources();
        buildSkinnedResources();
        buildRenderGraph();
    }

    // Depth-only shadow pass: renders the scene from the sun's view into the
    // shadow map (sampled by the Scene pass lighting).
    void buildShadowResources() {
        // Depth-only render pass (final layout = shader-read for the Scene pass).
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        // One render pass for the whole cascade atlas: the CLEAR wipes all
        // four tiles at pass begin and each cascade renders into its own tile
        // (viewport/scissor + its light VP) inside drawShadowPass.
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
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &depthAttachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device, &rpInfo, nullptr, &shadowRenderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow render pass");
        }

        VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbInfo.renderPass = shadowRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &shadowMapView;
        fbInfo.width = kShadowMapSize;
        fbInfo.height = kShadowMapSize;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &shadowFramebuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow framebuffer");
        }

        // Depth-only vertex shader (same push constant as the material pass).
        const std::string vertSrc = R"(
#version 450
layout (location = 0) in vec3 inPosition;
layout (push_constant) uniform PushConstants {
    mat4 mvp;    // light view-projection * model
    mat4 model;
} push;
void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
}
)";
        const std::string fragSrc = "#version 450\nvoid main() {}\n";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the shadow shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(Rendering::MaterialPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &shadowPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow pipeline layout");
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        // Depth-only shader consumes only position: declare a single attribute
        // (binding stride still matches GameVertex so the mesh buffers bind).
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GameVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[1]{};
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, pos)) };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.5f;
        raster.depthBiasSlopeFactor = 1.5f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = shadowPipelineLayout;
        pipelineInfo.renderPass = shadowRenderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &shadowPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
    }

    // Volumetric light-shafts pipeline: fullscreen pass that ray-marches each
    // view ray against the scene depth and the sun shadow map, accumulating
    // scattered sunlight into an HDR target the Composite pass adds on top.
    void buildVolumetricResources() {
        const std::string vertSrc = R"(
#version 450
layout(location = 0) out vec2 vUv;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    vUv = pos[gl_VertexIndex] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
)";
        const std::string fragSrc = R"(
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D sceneDepth;
layout(binding = 1) uniform sampler2D shadowMap;
layout(binding = 2) uniform VolumetricParams {
    vec4 cameraPosition;
    mat4 invViewProj;
    vec4 sunDirection;
    vec4 sunColor;
    mat4 sunViewProj;
    vec4 shadowParams;
    mat4 cameraViewProj;
    vec4 nearFar;
    mat4 sunCascadeVP[4];
    vec4 sunCascadeSplits;
    vec4 cameraForward;
} vol;
float sunShadowAt(vec3 worldPos) {
    if (vol.shadowParams.x <= 0.5) return 1.0;
    int c = 0;
    vec4 sc;
    if (vol.shadowParams.z > 1.5) {
        float viewDepth = dot(worldPos - vol.cameraPosition.xyz, vol.cameraForward.xyz);
        c = 3;
        if (viewDepth < vol.sunCascadeSplits.x) c = 0;
        else if (viewDepth < vol.sunCascadeSplits.y) c = 1;
        else if (viewDepth < vol.sunCascadeSplits.z) c = 2;
        sc = vol.sunCascadeVP[c] * vec4(worldPos, 1.0);
    } else {
        sc = vol.sunViewProj * vec4(worldPos, 1.0);
    }
    sc.xyz /= sc.w;
    vec2 suv = sc.xy * 0.5 + 0.5;
    if (vol.shadowParams.z > 1.5) {
        vec2 tileOff = vec2(float(c % 2), float(c / 2)) * 0.5;
        suv = suv * 0.5 + tileOff;
    }
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 || sc.z < 0.0 || sc.z > 1.0) {
        return 1.0;
    }
    float d = texture(shadowMap, suv).r;
    return (d < sc.z - vol.shadowParams.y) ? 0.0 : 1.0;
}
void main() {
    float depth = texture(sceneDepth, vUv).r;
    vec4 ndc = vec4(vUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = vol.invViewProj * ndc;
    vec3 surfacePos = wp.xyz / wp.w;

    vec3 ray = surfacePos - vol.cameraPosition.xyz;
    float rayLen = length(ray);
    int steps = max(int(vol.nearFar.w), 2);
    vec3 stepVec = (rayLen > 1e-5) ? (ray / float(steps)) : vec3(0.0);
    float density = vol.nearFar.z;
    float sunActive = vol.sunDirection.w;
    float shadowActive = vol.shadowParams.x;
    float intensity = vol.sunColor.w;

    vec3 accum = vec3(0.0);
    float transmittance = 1.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = vol.cameraPosition.xyz + stepVec * (float(i) + 0.5);
        // Cut the shaft at occluders: if this sample is behind the scene
        // surface along its own screen direction, stop marching.
        vec4 sp = vol.cameraViewProj * vec4(p, 1.0);
        sp.xyz /= sp.w;
        vec2 suv = sp.xy * 0.5 + 0.5;
        if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0) {
            float sd = texture(sceneDepth, suv).r;
            if (sp.z > sd * 2.0 - 1.0 + 0.0005) break;
        }
        float lit = 1.0;
        if (shadowActive > 0.5) lit = sunShadowAt(p);
        float scatter = transmittance * density * (rayLen / float(steps)) * lit;
        accum += vol.sunColor.rgb * (scatter * intensity * sunActive);
        transmittance *= exp(-density * (rayLen / float(steps)));
    }
    outColor = vec4(accum, 1.0);
}
)";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the volumetric shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // UBO buffer for the per-frame volumetric params.
        VkBufferCreateInfo uboInfo{};
        uboInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uboInfo.size = sizeof(VolumetricParams);
        uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo uboAllocInfo{};
        uboAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &uboInfo, &uboAllocInfo, &volumUboBuffer, &volumUboAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric UBO buffer!");
        }

        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &volumSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric descriptor set layout");
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &volumSetLayout;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &volumPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric pipeline layout");
        }

        VkDescriptorPoolSize poolSizes[2]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &volumDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = volumDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &volumSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &volumDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate volumetric descriptor set");
        }

        VkDescriptorImageInfo depthInfo{ volumDepthSampler, depthImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo shadowInfo{ shadowMapSampler, shadowMapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo uboDescInfo{ volumUboBuffer, 0, sizeof(VolumetricParams) };
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = volumDescriptorSet;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depthInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = volumDescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = volumDescriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &uboDescInfo;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = volumPipelineLayout;
        pipelineInfo.renderPass = volumRenderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &volumPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
    }

    // Skinned mesh support (Fase 6): a procedural two-bone "flag" drawn by a
    // real skinned pipeline — bone matrices computed per frame from an
    // animated Pose, packed by GpuSkinningBuffer and uploaded to a UBO the
    // generated vertex shader consumes. Lit by the shared LightParams UBO.
    void buildSkinnedResources() {
        // 5x4 grid flag in the XY plane, x in [-1, 1], y in [0, 1.6].
        constexpr uint32_t cols = 5, rows = 4;
        std::vector<GameVertex> verts;
        verts.reserve(cols * rows);
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < cols; ++c) {
                const float x = -1.0f + 2.0f * static_cast<float>(c) / static_cast<float>(cols - 1);
                const float y = 1.6f * static_cast<float>(r) / static_cast<float>(rows - 1);
                GameVertex v;
                v.pos = glm::vec3(x, y, 0.0f);
                v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                v.color = glm::vec3(1.0f);
                v.uv = glm::vec2(static_cast<float>(c) / static_cast<float>(cols - 1),
                                 static_cast<float>(r) / static_cast<float>(rows - 1));
                // Blend from bone 0 (left edge) to bone 1 (right edge).
                const float w1 = (x + 1.0f) * 0.5f;
                v.joints = glm::uvec4(0, 1, 0, 0);
                v.weights = glm::vec4(1.0f - w1, w1, 0.0f, 0.0f);
                verts.push_back(v);
            }
        }
        std::vector<uint32_t> indices;
        indices.reserve((cols - 1) * (rows - 1) * 6);
        for (uint32_t r = 0; r + 1 < rows; ++r) {
            for (uint32_t c = 0; c + 1 < cols; ++c) {
                const uint32_t a = r * cols + c;
                const uint32_t b = a + 1;
                const uint32_t d = (r + 1) * cols + c;
                const uint32_t e = d + 1;
                indices.insert(indices.end(), { a, b, d, b, e, d });
            }
        }
        skinnedVertexCount = static_cast<uint32_t>(verts.size());
        skinnedIndexCount = static_cast<uint32_t>(indices.size());
        createBuffer(sizeof(GameVertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     verts.data(), skinnedVb, skinnedVbAllocation);
        createBuffer(sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     indices.data(), skinnedIb, skinnedIbAllocation);

        // Skeleton: root at the flag base, tip joint at the right edge.
        skinnedSkeleton.name = "Flag";
        BoneNode root;
        root.name = "Root";
        root.parentIndex = -1;
        root.localTransform = glm::mat4(1.0f);
        root.inverseBindMatrix = glm::inverse(root.localTransform);
        skinnedSkeleton.bones.push_back(root);
        BoneNode tip;
        tip.name = "Tip";
        tip.parentIndex = 0;
        tip.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        tip.inverseBindMatrix = glm::inverse(tip.localTransform);
        skinnedSkeleton.bones.push_back(tip);

        // A real AnimationClip driving the flag: the Tip joint swings between
        // 0 and 60 degrees over 2 seconds (loops) — sampled per frame through
        // AnimationSampler::sample (clip → pose → bone matrices → pipeline).
        skinnedClip.name = "FlagWave";
        skinnedClip.duration = 2.0f;
        skinnedClip.looping = true;
        skinnedClip.tracks.push_back(BoneTrack{ 1, {
            KeyFrame{ 0.0f, glm::vec3(0.0f), glm::angleAxis(0.0f, glm::vec3(0, 0, 1)), glm::vec3(1.0f) },
            KeyFrame{ 2.0f, glm::vec3(0.0f), glm::angleAxis(glm::radians(60.0f), glm::vec3(0, 0, 1)), glm::vec3(1.0f) },
        } });

        // Ragdoll: the same bones become physics bodies (root + tip capsules,
        // distance constraint between them) spawned above the camera view —
        // gravity + a per-frame impulse make the flag topple, and its pose is
        // blended with the clip to drive the skinned mesh.
        std::vector<Physics::RagdollBoneDesc> ragBones;
        ragBones.push_back(Physics::RagdollBoneDesc{
            "Root", "", glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, 1.0f, glm::vec3(0.0f) });
        ragBones.push_back(Physics::RagdollBoneDesc{
            "Tip", "Root", glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, 1.0f, glm::vec3(0.0f) });
        if (ragdoll.create(physics, ragBones, glm::vec3(0.0f, 3.0f, -2.5f))) {
            ragdollBuilt = true;
            std::cout << "[Game] Ragdoll active (bodies=" << ragBones.size()
                      << ", joints=1) — physics drives the skinned flag\n";
        }

        // Generated vertex shader (GpuSkinningBuffer) + dedicated fragment
        // shader consuming the shared LightParams UBO (matches LightUboData).
        const std::string fragSrc = R"(
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec3 vNormal;
layout(location = 0) out vec4 outColor;
layout(binding = 2) uniform LightParams {
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 sunColor;
    mat4 sunViewProj;
    vec4 shadowParams;
    vec4 pointLightPos[8];
    vec4 pointLightColor[8];
    vec4 spotLightPos[4];
    vec4 spotLightDir[4];
    vec4 spotLightParams[4];
    vec4 spotLightColor[4];
    vec4 areaLightPos[4];
    vec4 areaLightNormal[4];
    vec4 areaLightHalf[4];
    vec4 areaLightColor[4];
    mat4 sunCascadeVP[4];
    vec4 sunCascadeSplits;
    vec4 cameraForward;
} lights;
void main() {
    vec3 n = normalize(vNormal);
    vec3 base = mix(vec3(0.30f, 0.38f, 0.58f), vec3(0.85f, 0.45f, 0.25f), vUv.x);
    vec3 lightAccum = vec3(0.18f);
    if (lights.sunDirection.w > 0.5f) {
        float ndl = max(dot(n, -lights.sunDirection.xyz), 0.0f);
        lightAccum += ndl * lights.sunColor.rgb * 0.25f;
    }
    for (int i = 0; i < 4; ++i) {
        if (lights.spotLightDir[i].w <= 0.5f) continue;
        vec3 toLight = lights.spotLightPos[i].xyz - vWorldPos;
        float dist = length(toLight);
        float range = max(lights.spotLightPos[i].w, 0.01f);
        float att = clamp(1.0f - dist / range, 0.0f, 1.0f);
        att *= att;
        vec3 L = toLight / max(dist, 0.0001f);
        float spot = smoothstep(lights.spotLightParams[i].y, lights.spotLightParams[i].x, dot(-L, lights.spotLightDir[i].xyz));
        float ndl = max(dot(n, L), 0.0f);
        lightAccum += ndl * att * spot * lights.spotLightColor[i].rgb;
    }
    for (int i = 0; i < 4; ++i) {
        if (lights.areaLightPos[i].w <= 0.5f) continue;
        vec3 toLight = lights.areaLightPos[i].xyz - vWorldPos;
        float dist = max(length(toLight), 0.0001f);
        float reach = max(lights.areaLightHalf[i].x + lights.areaLightHalf[i].y, 0.01f);
        float att = clamp(1.0f - dist / reach, 0.0f, 1.0f);
        att *= att;
        vec3 L = toLight / dist;
        float facing = max(dot(lights.areaLightNormal[i].xyz, -L), 0.0f);
        float ndl = max(dot(n, L), 0.0f);
        lightAccum += ndl * att * facing * lights.areaLightColor[i].rgb;
    }
    outColor = vec4(base * lightAccum, 1.0f);
}
)";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(
            GpuSkinningBuffer::skinned_vertex_shader(kBoneCount), VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the skinned shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // Bone matrices + camera view-projection + light UBOs.
        const VkDeviceSize boneSize = static_cast<VkDeviceSize>(kBoneCount) * sizeof(glm::mat4);
        VkBufferCreateInfo uboInfo{};
        uboInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uboInfo.size = boneSize;
        uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo uboAllocInfo{};
        uboAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &uboInfo, &uboAllocInfo, &skinnedBoneBuffer, &skinnedBoneAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned bone UBO");
        }
        uboInfo.size = sizeof(glm::mat4);
        if (vmaCreateBuffer(allocator, &uboInfo, &uboAllocInfo, &skinnedCameraBuffer, &skinnedCameraAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned camera UBO");
        }

        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &skinnedSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned descriptor set layout");
        }
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &skinnedSetLayout;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &skinnedPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned pipeline layout");
        }

        VkDescriptorPoolSize poolSizes[1]{ { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 } };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &skinnedDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = skinnedDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &skinnedSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &skinnedDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate skinned descriptor set");
        }
        VkDescriptorBufferInfo boneDesc{ skinnedBoneBuffer, 0, boneSize };
        VkDescriptorBufferInfo camDesc{ skinnedCameraBuffer, 0, sizeof(glm::mat4) };
        VkDescriptorBufferInfo lightDesc{ lightBuffer, 0, sizeof(Rendering::LightUboData) };
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = skinnedDescriptorSet;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &boneDesc;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = skinnedDescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &camDesc;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = skinnedDescriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &lightDesc;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GameVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[5]{};
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, pos)) };
        attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, normal)) };
        attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, uv)) };
        attrs[3] = { 3, 0, VK_FORMAT_R32G32B32A32_UINT, static_cast<uint32_t>(offsetof(GameVertex, joints)) };
        attrs[4] = { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, weights)) };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = skinnedPipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skinnedPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        skinnedBuilt = true;
        std::cout << "[Game] Skinned pipeline built (flag verts=" << skinnedVertexCount
                  << " indices=" << skinnedIndexCount << ", bones=" << skinnedSkeleton.bones.size() << ")\n";
    }

    // Fullscreen-triangle composite pipeline: samples the offscreen HDR scene
    // color (descriptor binding 0) and writes it to the swapchain.
    void buildCompositePipeline() {
        const std::string vertSrc = R"(
#version 450
layout(location = 0) out vec2 vUv;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    vUv = pos[gl_VertexIndex] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
)";
        const std::string fragSrc = R"(
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D sceneColor;
layout(binding = 1) uniform sampler2D volumetric;
void main() {
    outColor = vec4(texture(sceneColor, vUv).rgb + texture(volumetric, vUv).rgb, 1.0);
}
)";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the composite shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // Sampler for the HDR scene color.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &compositeSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite sampler");
        }

        VkDescriptorSetLayoutBinding samplerBindings[2]{};
        samplerBindings[0].binding = 0;
        samplerBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBindings[0].descriptorCount = 1;
        samplerBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        samplerBindings[1].binding = 1;
        samplerBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBindings[1].descriptorCount = 1;
        samplerBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 2;
        dslInfo.pBindings = samplerBindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &compositeSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite descriptor set layout");
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &compositeSetLayout;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &compositePipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite pipeline layout");
        }

        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &compositeDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = compositeDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &compositeSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &compositeDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate composite descriptor set");
        }
        VkDescriptorImageInfo imageInfo{ compositeSampler, hdrImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo volumInfo{ compositeSampler, volumImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = compositeDescriptorSet;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imageInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = compositeDescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &volumInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = compositePipelineLayout;
        pipelineInfo.renderPass = compositeRenderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &compositePipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
    }

    // Builds the game's render graph (Scene → Composite) and wires the
    // executor to the real framebuffers/passes: the graph's compiled order and
    // barriers now drive the frame.
    void buildRenderGraph() {
        if (renderGraphBuilt) return;
        using namespace Rendering;
        const auto hdrRes = renderGraph.add_resource({ "HDR Color", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, true, false, RenderResourceState::Undefined });
        const auto depthRes = renderGraph.add_resource({ "Scene Depth", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, true, false, RenderResourceState::Undefined });
        const auto swapRes = renderGraph.add_resource({ "Swapchain", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, false, true, RenderResourceState::Present });
        const auto shadowRes = renderGraph.add_resource({ "Shadow Map", RenderResourceKind::Image, 0,
            kShadowMapSize, kShadowMapSize, 1, true, false, RenderResourceState::Undefined });
        const auto volumRes = renderGraph.add_resource({ "Volumetric", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, true, false, RenderResourceState::Undefined });
        const auto shadowPass = renderGraph.add_pass({ "Shadow", RenderQueue::Graphics,
            { { shadowRes, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
        const auto scenePass = renderGraph.add_pass({ "Scene", RenderQueue::Graphics,
            { { hdrRes, RenderAccess::Write, RenderResourceState::ColorAttachment },
              { depthRes, RenderAccess::Write, RenderResourceState::DepthAttachment },
              { shadowRes, RenderAccess::Read, RenderResourceState::ShaderRead } }, true });
        const auto volumPass = renderGraph.add_pass({ "Volumetric", RenderQueue::Graphics,
            { { depthRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { shadowRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { volumRes, RenderAccess::Write, RenderResourceState::ColorAttachment } }, true });
        const auto compositePass = renderGraph.add_pass({ "Composite", RenderQueue::Graphics,
            { { hdrRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { volumRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { swapRes, RenderAccess::Write, RenderResourceState::Present } }, true });
        (void)renderGraph.add_dependency(shadowPass, scenePass);
        (void)renderGraph.add_dependency(scenePass, volumPass);
        (void)renderGraph.add_dependency(volumPass, compositePass);

        std::string error;
        if (!renderGraphExecutor.initialize(device, renderGraph, &error)) {
            throw std::runtime_error("render graph init failed: " + error);
        }

        VulkanRenderGraphExecutor::PassFrame shadowFrame;
        shadowFrame.renderPass = shadowRenderPass;
        shadowFrame.framebuffers = { shadowFramebuffer };
        shadowFrame.clearValues.resize(1);
        shadowFrame.clearValues[0].depthStencil = { 1.0f, 0 };
        shadowFrame.draw = [this](VkCommandBuffer cb) { drawShadowPass(cb); };
        renderGraphExecutor.register_pass(shadowPass, std::move(shadowFrame));

        VulkanRenderGraphExecutor::PassFrame sceneFrame;
        sceneFrame.renderPass = renderPass;
        sceneFrame.framebuffers = { sceneFramebuffer };
        sceneFrame.clearValues.resize(2);
        sceneFrame.clearValues[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
        sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
        sceneFrame.draw = [this](VkCommandBuffer cb) { drawScene(cb); };
        renderGraphExecutor.register_pass(scenePass, std::move(sceneFrame));

        VulkanRenderGraphExecutor::PassFrame volumFrame;
        volumFrame.renderPass = volumRenderPass;
        volumFrame.framebuffers = { volumFramebuffer };
        volumFrame.clearValues.resize(1);
        volumFrame.clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        volumFrame.draw = [this](VkCommandBuffer cb) { drawVolumetric(cb); };
        renderGraphExecutor.register_pass(volumPass, std::move(volumFrame));

        VulkanRenderGraphExecutor::PassFrame compositeFrame;
        compositeFrame.renderPass = compositeRenderPass;
        compositeFrame.framebuffers = compositeFramebuffers;
        compositeFrame.clearValues.resize(1);
        compositeFrame.clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        compositeFrame.draw = [this](VkCommandBuffer cb) { drawComposite(cb); };
        renderGraphExecutor.register_pass(compositePass, std::move(compositeFrame));

        renderGraphBuilt = true;
        std::cout << "[Game] Render graph frame wired ("
                  << renderGraphExecutor.compile_result().order.size() << " passes, "
                  << renderGraphExecutor.compile_result().barriers.size() << " barriers)\n";
    }

    void drawComposite(VkCommandBuffer cb) {
        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipelineLayout,
                                0, 1, &compositeDescriptorSet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }

    VkShaderModule createModuleFromSpirv(const std::vector<uint32_t>& spirv) {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = spirv.size() * sizeof(uint32_t);
        info.pCode = spirv.data();
        VkShaderModule module;
        if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shader module from SPIR-V");
        }
        return module;
    }

    // ─── Mesh loading: cooked .vcmesh (v1 or v2) from the content package ───
    GameMeshResource* getMesh(const UUID& assetId) {
        if (!assetId.is_valid()) return nullptr;
        const auto cached = meshes.find(assetId);
        if (cached != meshes.end()) return cached->second.valid ? &cached->second : nullptr;
        if (meshLoadFailed.contains(assetId)) return nullptr;

        std::filesystem::path cookedPath = content.absolute_path(assetId);
        std::string error;
        const GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(cookedPath, &error);
        if (!geometry.success || geometry.primitives.empty()) {
            std::cerr << "[Game] Cannot load mesh " << assetId.to_string() << ": " << error << '\n';
            meshLoadFailed.insert(assetId);
            return nullptr;
        }
        GameMeshResource resource;
        std::vector<GameVertex> verts;
        std::vector<uint32_t> indices;
        for (const GltfMeshPrimitive& primitive : geometry.primitives) {
            const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + primitive.positions.size());
            const bool primitiveSkinned = !primitive.joints.empty() && primitive.joints.size() == primitive.positions.size() &&
                                          primitive.weights.size() == primitive.positions.size();
            if (primitiveSkinned) resource.skinned = true;
            for (size_t i = 0; i < primitive.positions.size(); ++i) {
                GameVertex v;
                v.pos = primitive.positions[i];
                v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
                v.color = glm::vec3(1.0f);
                v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
                if (primitiveSkinned) {
                    v.joints = primitive.joints[i];
                    v.weights = primitive.weights[i];
                }
                verts.push_back(v);
            }
            if (primitive.indexed) {
                const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
                for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
                resource.ranges.push_back({ firstIndex, static_cast<uint32_t>(primitive.indices.size()), 0, true });
            } else {
                resource.ranges.push_back({ 0, static_cast<uint32_t>(primitive.positions.size()), vertexOffset, false });
            }
        }
        // Embed the first skin as the mesh skeleton (skinned draw path).
        if (resource.skinned && !geometry.skins.empty()) {
            const GltfGeometrySkin& source = geometry.skins.front();
            resource.skeleton.name = source.name.empty() ? "MeshSkeleton" : source.name;
            resource.skeleton.bones.resize(source.jointNames.size());
            for (size_t i = 0; i < source.jointNames.size(); ++i) {
                BoneNode& bone = resource.skeleton.bones[i];
                bone.name = source.jointNames[i];
                bone.parentIndex = i < source.jointParents.size() ? source.jointParents[i] : -1;
                bone.inverseBindMatrix = i < source.inverseBindMatrices.size()
                    ? source.inverseBindMatrices[i] : glm::mat4(1.0f);
                bone.localTransform = glm::inverse(bone.inverseBindMatrix);
            }
        }
        resource.valid = true;
        createBuffer(sizeof(GameVertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data(), resource.vb, resource.vbAlloc);
        if (!indices.empty()) {
            createBuffer(sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), resource.ib, resource.ibAlloc);
        }
        meshes[assetId] = std::move(resource);
        return &meshes[assetId];
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data,
                      VkBuffer& buffer, VmaAllocation& allocation) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create GPU buffer");
        }
        void* mapped = nullptr;
        vmaMapMemory(allocator, allocation, &mapped);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        vmaUnmapMemory(allocator, allocation);
    }

    // Procedural cube used when an entity has no cooked mesh asset (built once).
    GameMeshResource buildCubeMesh() {
        std::vector<GameVertex> verts;
        std::vector<uint32_t> indices;
        const glm::vec3 n[6] = {
            { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 } };
        for (int f = 0; f < 6; ++f) {
            const uint32_t base = static_cast<uint32_t>(verts.size());
            const glm::vec3& axis = n[f];
            glm::vec3 u = glm::abs(axis.x) > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            glm::vec3 v = glm::normalize(glm::cross(axis, u));
            u = glm::normalize(glm::cross(v, axis));
            for (int c = 0; c < 4; ++c) {
                const glm::vec2 corner = glm::vec2((c & 1) ? 1 : 0, (c & 2) ? 1 : 0);
                glm::vec3 p = axis * 0.5f + u * (corner.x - 0.5f) + v * (corner.y - 0.5f);
                verts.push_back({ p, axis, glm::vec3(1.0f), corner });
            }
            indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
        GameMeshResource resource;
        resource.valid = true;
        resource.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
        createBuffer(sizeof(GameVertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data(), resource.vb, resource.vbAlloc);
        createBuffer(sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), resource.ib, resource.ibAlloc);
        return resource;
    }

    // Writes the PBR material params into the std140 UBO (names sorted as the
    // generated GLSL declares them: Albedo, Emissive, Metallic, Roughness).
    void writeMaterialUbo(const MaterialComponent& material) {
        struct PbrUbo {
            glm::vec3 albedo;     // @0
            float pad0;           // @12
            glm::vec3 emissive;   // @16
            float pad1;           // @28
            float metallic;       // @32
            float roughness;      // @36
            float pad2, pad3;     // @40
        };
        PbrUbo ubo;
        ubo.albedo = material.albedo;
        ubo.emissive = material.emissiveColor * material.emissiveIntensity;
        ubo.metallic = material.metallic;
        ubo.roughness = material.roughness;
        ubo.pad0 = ubo.pad1 = ubo.pad2 = ubo.pad3 = 0.0f;
        void* mapped = nullptr;
        vmaMapMemory(allocator, uboAllocation, &mapped);
        std::memcpy(mapped, &ubo, sizeof(ubo));
        vmaUnmapMemory(allocator, uboAllocation);
    }

    void drawMeshResource(VkCommandBuffer cb, const GameMeshResource& mesh,
                          const glm::mat4& mvp, const glm::mat4& model) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vb, &offset);
        if (mesh.ib != VK_NULL_HANDLE) vkCmdBindIndexBuffer(cb, mesh.ib, 0, VK_INDEX_TYPE_UINT32);
        const Rendering::MaterialPushConstants push{ mvp, model };
        vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        for (const MeshRange& range : mesh.ranges) {
            if (range.indexed) vkCmdDrawIndexed(cb, range.indexCount, 1, range.firstIndex, 0, 0);
            else vkCmdDraw(cb, range.indexCount, 1, range.vertexOffset, 0);
        }
    }

    glm::mat4 modelFromTransform(const TransformComponent& t) const {
        glm::mat4 model(1.0f);
        model = glm::translate(model, t.position);
        model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
        model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
        model = glm::scale(model, t.scale);
        return model;
    }

    // Fits one light-space ortho per cascade around the camera frustum slices
    // and stores the view-depth split points the shaders use for selection.
    // Split distances blend logarithmic and uniform schemes (practical split
    // scheme, lambda = 0.5); each ortho is texel-snapped for temporal stability.
    void computeShadowCascades(const glm::mat4& lightView) {
        const glm::vec3 front = cameraFront();
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(front, up));
        const glm::vec3 camUp = glm::normalize(glm::cross(right, front));
        float aspect = static_cast<float>(swapChainExtent.width) / std::max(1u, swapChainExtent.height);
        float fov = 70.0f;
        for (const auto& [id, cam] : scene.cameraComponents) {
            (void)id;
            fov = cam.fov;
            break;
        }
        const float tanHalf = std::tan(glm::radians(fov) * 0.5f);
        constexpr float kNear = 0.1f, kFar = 1000.0f, kLambda = 0.5f;
        float splits[kGameShadowCascades + 1];
        splits[0] = kNear;
        for (uint32_t i = 1; i <= kGameShadowCascades; ++i) {
            const float p = static_cast<float>(i) / static_cast<float>(kGameShadowCascades);
            splits[i] = kLambda * kNear * std::pow(kFar / kNear, p) +
                        (1.0f - kLambda) * (kNear + (kFar - kNear) * p);
        }
        splits[kGameShadowCascades] = kFar;
        constexpr float kPad = 20.0f;
        for (uint32_t c = 0; c < kGameShadowCascades; ++c) {
            const float dNear = splits[c];
            const float dFar = splits[c + 1];
            const glm::vec3 cn = camPos + front * dNear;
            const glm::vec3 cf = camPos + front * dFar;
            const float hn = tanHalf * dNear, wn = hn * aspect;
            const float hf = tanHalf * dFar, wf = hf * aspect;
            const glm::vec3 corners[8] = {
                cn - right * wn - camUp * hn, cn + right * wn - camUp * hn,
                cn - right * wn + camUp * hn, cn + right * wn + camUp * hn,
                cf - right * wf - camUp * hf, cf + right * wf - camUp * hf,
                cf - right * wf + camUp * hf, cf + right * wf + camUp * hf,
            };
            float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f,
                  minZ = 1e30f, maxZ = -1e30f;
            for (const glm::vec3& corner : corners) {
                const glm::vec3 lc = glm::vec3(lightView * glm::vec4(corner, 1.0f));
                minX = std::min(minX, lc.x); maxX = std::max(maxX, lc.x);
                minY = std::min(minY, lc.y); maxY = std::max(maxY, lc.y);
                minZ = std::min(minZ, lc.z); maxZ = std::max(maxZ, lc.z);
            }
            const float halfX = (maxX - minX) * 0.5f + kPad;
            const float halfY = (maxY - minY) * 0.5f + kPad;
            glm::vec3 center((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f);
            const float texelX = (halfX * 2.0f) / static_cast<float>(kShadowCascadeSize);
            const float texelY = (halfY * 2.0f) / static_cast<float>(kShadowCascadeSize);
            center.x = std::round(center.x / texelX) * texelX;
            center.y = std::round(center.y / texelY) * texelY;
            const glm::mat4 lightProj = glm::ortho(center.x - halfX, center.x + halfX,
                                                   center.y - halfY, center.y + halfY,
                                                   -maxZ - kPad, -minZ + kPad);
            // Remap NDC z ([-1,1] from glm::ortho) into [0,1] so computeShadow's
            // `sc.z < 0.0 || sc.z > 1.0` early-out and the `d < sc.z - bias`
            // compare match the depth actually stored in the shadow map.
            // Without this, the near half of the depth range is always lit.
            glm::mat4 depthRemap(1.0f);
            depthRemap[2][2] = 0.5f;
            depthRemap[2][3] = 0.5f;
            sunCascadeVP[c] = depthRemap * lightProj * lightView;
        }
        sunViewProj = sunCascadeVP[0];
        sunCascadeSplits = glm::vec4(splits[1], splits[2], splits[3],
                                     static_cast<float>(kGameShadowCascades));
    }

    // Computes the sun's cascade view-projections from the scene's directional
    // LightComponent. Called once per frame before the UBO writes.
    void updateSunShadow() {
        sunEnabled = false;
        // O gate sunShadowStatusLogged loga o status das cascades uma única
        // vez; o reset por frame aqui anulava o gate (spam de 1 linha/frame).
        for (const auto& [id, light] : scene.lightComponents) {
            if (!is_directional_sun(light)) continue;
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                dir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            const glm::vec3 target(0.0f, 1.0f, 0.0f);
            const glm::vec3 eye = target - dir * 60.0f;
            const glm::mat4 lightView = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
            computeShadowCascades(lightView);
            sunEnabled = true;
            if (!sunShadowStatusLogged) {
                sunShadowStatusLogged = true;
                std::cout << "[Game] Sun shadow cascades active (atlas " << kShadowMapSize
                          << "x" << kShadowMapSize << " = " << kGameShadowCascades
                          << " x " << kShadowCascadeSize << "x" << kShadowCascadeSize << ")\n";
            }
            break;
        }
    }

    // Shadow pass draw: renders the scene depth-only into each cascade's tile
    // of the atlas (2x2 grid), each with its own light view-projection.
    void drawShadowPass(VkCommandBuffer cb) {
        if (!sunEnabled) return;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
        for (uint32_t cascade = 0; cascade < kGameShadowCascades; ++cascade) {
            const uint32_t tileX = (cascade % 2) * kShadowCascadeSize;
            const uint32_t tileY = (cascade / 2) * kShadowCascadeSize;
            VkViewport viewport{ static_cast<float>(tileX), static_cast<float>(tileY),
                                 static_cast<float>(kShadowCascadeSize),
                                 static_cast<float>(kShadowCascadeSize), 0.0f, 1.0f };
            vkCmdSetViewport(cb, 0, 1, &viewport);
            VkRect2D scissor{ { static_cast<int32_t>(tileX), static_cast<int32_t>(tileY) },
                              { kShadowCascadeSize, kShadowCascadeSize } };
            vkCmdSetScissor(cb, 0, 1, &scissor);
            const glm::mat4 cascadeVP = sunCascadeVP[cascade];
            for (const auto& [id, mr] : scene.meshRendererComponents) {
                if (!mr.isVisible) continue;
                const auto tit = scene.transformComponents.find(id);
                if (tit == scene.transformComponents.end()) continue;
                const glm::mat4 model = modelFromTransform(tit->second);
                const Rendering::MaterialPushConstants pc{ cascadeVP * model, model };
                vkCmdPushConstants(cb, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                const VkDeviceSize offset = 0;
                const GameMeshResource* mesh = getMesh(mr.meshAssetID);
                if (!mesh) {
                    if (!cubeBuilt) continue;
                    mesh = &cubeMesh;
                }
                vkCmdBindVertexBuffers(cb, 0, 1, &mesh->vb, &offset);
                if (mesh->ib != VK_NULL_HANDLE) vkCmdBindIndexBuffer(cb, mesh->ib, 0, VK_INDEX_TYPE_UINT32);
                for (const MeshRange& range : mesh->ranges) {
                    if (range.indexed) vkCmdDrawIndexed(cb, range.indexCount, 1, range.firstIndex, 0, 0);
                    else vkCmdDraw(cb, range.indexCount, 1, range.vertexOffset, 0);
                }
            }
        }
    }

    // Fills the LightParams UBO from the scene's LightComponents (range >= 50
    // ⇒ directional sun; otherwise a point light) + the camera position + the
    // shadow map data (view-projection + bias).
    void writeLightUbo(const glm::vec3& camPos) {
        Rendering::LightUboData data{};
        data.cameraPosition = glm::vec4(camPos, 1.0f);
        data.shadowParams = glm::vec4(sunEnabled ? 1.0f : 0.0f, shadowBias,
                                      sunEnabled ? static_cast<float>(kGameShadowCascades) : 0.0f, 0.0f);
        data.sunViewProj = sunCascadeVP[0];
        for (uint32_t i = 0; i < kGameShadowCascades; ++i) data.sunCascadeVP[i] = sunCascadeVP[i];
        data.sunCascadeSplits = sunCascadeSplits;
        data.cameraForward = glm::vec4(cameraFront(), 0.0f);
        uint32_t pointCount = 0, spotCount = 0, areaCount = 0;
        for (const auto& [id, light] : scene.lightComponents) {
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            glm::vec3 position(0.0f);
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                position = tit->second.position;
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                dir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            const glm::vec3 colorIntensity = light.color * light.intensity;
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
        if (!lightStatusLogged) {
            lightStatusLogged = true;
            std::cout << "[Game] Lights in scene: " << (sunEnabled ? 1 : 0) << " sun, "
                      << pointCount << " point, " << spotCount << " spot, "
                      << areaCount << " area\n";
        }
        void* mapped = nullptr;
        vmaMapMemory(allocator, lightAllocation, &mapped);
        std::memcpy(mapped, &data, sizeof(data));
        vmaUnmapMemory(allocator, lightAllocation);
    }

    glm::mat4 computeCameraView() const {
        return glm::lookAt(camPos, camPos + cameraFront(), glm::vec3(0, 1, 0));
    }

    glm::mat4 computeCameraProj() const {
        float aspect = static_cast<float>(swapChainExtent.width) / std::max(1u, swapChainExtent.height);
        float fov = 70.0f;
        for (const auto& [id, cam] : scene.cameraComponents) {
            (void)id;
            fov = cam.fov;
            break;
        }
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, 0.1f, 1000.0f);
        proj[1][1] *= -1.0f; // Vulkan clip space
        return proj;
    }

    // Fills the VolumetricParams UBO for the light-shafts pass: camera
    // matrices, sun data (normalized color so shafts don't blow out at high
    // intensities) and the march parameters.
    void writeVolumetricUbo() {
        const glm::mat4 view = computeCameraView();
        const glm::mat4 proj = computeCameraProj();
        VolumetricParams data{};
        data.cameraPosition = glm::vec4(camPos, 1.0f);
        data.cameraViewProj = proj * view;
        data.invViewProj = glm::inverse(proj * view);
        data.sunViewProj = sunCascadeVP[0];
        data.shadowParams = glm::vec4(sunEnabled ? 1.0f : 0.0f, shadowBias,
                                      sunEnabled ? static_cast<float>(kGameShadowCascades) : 0.0f, 0.0f);
        data.nearFar = glm::vec4(0.1f, 1000.0f, kVolumetricDensity, kVolumetricSteps);
        for (uint32_t i = 0; i < kGameShadowCascades; ++i) data.sunCascadeVP[i] = sunCascadeVP[i];
        data.sunCascadeSplits = sunCascadeSplits;
        data.cameraForward = glm::vec4(cameraFront(), 0.0f);
        glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
        glm::vec3 sunColor(1.0f);
        bool sunActive = false;
        for (const auto& [id, light] : scene.lightComponents) {
            if (!is_directional_sun(light)) continue;
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                sunDir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            sunColor = light.color;
            sunActive = true;
            break;
        }
        const float len = glm::length(sunColor);
        if (len > 1e-5f) sunColor /= len;
        data.sunDirection = glm::vec4(sunDir, sunActive ? 1.0f : 0.0f);
        data.sunColor = glm::vec4(sunColor * kVolumetricIntensity, kVolumetricIntensity);
        void* mapped = nullptr;
        vmaMapMemory(allocator, volumUboAllocation, &mapped);
        std::memcpy(mapped, &data, sizeof(data));
        vmaUnmapMemory(allocator, volumUboAllocation);
    }

    void drawVolumetric(VkCommandBuffer cb) {
        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        writeVolumetricUbo();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, volumPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, volumPipelineLayout,
                                0, 1, &volumDescriptorSet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }

    void drawScene(VkCommandBuffer cb) {
        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);

        // Camera: free-fly state (initialized from the authored camera).
        const glm::mat4 view = computeCameraView();
        const glm::mat4 proj = computeCameraProj();

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        writeLightUbo(camPos);

        if (!cubeBuilt) {
            cubeMesh = buildCubeMesh();
            cubeBuilt = true;
        }
        for (const auto& [id, mr] : scene.meshRendererComponents) {
            if (!mr.isVisible) continue;
            const auto tit = scene.transformComponents.find(id);
            if (tit == scene.transformComponents.end()) continue;
            const TransformComponent& t = tit->second;

            const glm::mat4 model = modelFromTransform(t);

            const MaterialComponent* material = nullptr;
            const auto mit = scene.materialComponents.find(id);
            if (mit != scene.materialComponents.end()) material = &mit->second;
            const MaterialComponent fallback{ glm::vec3(0.8f, 0.8f, 0.8f), 0.5f, 0.0f, glm::vec3(0.0f), 0.0f };
            writeMaterialUbo(material ? *material : fallback);

            const glm::mat4 mvp = proj * view * model;
            const GameMeshResource* mesh = getMesh(mr.meshAssetID);
            if (mesh && mesh->skinned && skinnedBuilt && !mesh->skeleton.bones.empty()) {
                // Real skinned asset: animate the deepest joint and draw it
                // through the skinned pipeline (bind pose otherwise).
                Pose pose = AnimationSampler::bind_pose(mesh->skeleton);
                int32_t deepest = -1, bestDepth = -1;
                for (size_t i = 0; i < mesh->skeleton.bones.size(); ++i) {
                    int32_t depth = 0;
                    for (int32_t p = mesh->skeleton.bones[i].parentIndex; p >= 0; p = mesh->skeleton.bones[p].parentIndex) ++depth;
                    if (depth > bestDepth) { bestDepth = depth; deepest = static_cast<int32_t>(i); }
                }
                if (deepest >= 0) {
                    pose.local[static_cast<size_t>(deepest)].rotation = glm::angleAxis(
                        0.5f * std::sin(skinnedAnimTime * 2.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                }
                drawSkinnedMesh(cb, mesh->vb, mesh->ib, mesh->ranges, mesh->skeleton, pose);
            } else if (mesh) {
                drawMeshResource(cb, *mesh, mvp, model);
            } else {
                drawMeshResource(cb, cubeMesh, mvp, model);
            }
        }

        drawSkinned(cb);
    }

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
    void drawSkinned(VkCommandBuffer cb) {
        if (!skinnedBuilt) return;
        Pose pose = AnimationSampler::sample(skinnedSkeleton, skinnedClip, skinnedAnimTime);
        if (ragdollBuilt && !ragdoll.empty()) {
            std::vector<RagdollBody> bodies;
            for (const Physics::RagdollPoseBone& bone : ragdoll.pose(physics)) {
                const int boneIndex = skinnedSkeleton.find_bone_index(bone.name);
                if (boneIndex < 0) continue;
                const glm::mat4 world = glm::translate(glm::mat4(1.0f), bone.position) *
                                        glm::mat4_cast(bone.rotation);
                bodies.push_back(RagdollBody{ boneIndex, world, 1.0f });
            }
            if (!bodies.empty()) {
                pose = RagdollPoseBridge::blend_physics_pose(skinnedSkeleton, pose, bodies, ragdollBlendWeight);
            }
        } else {
            pose.local[0].translation = glm::vec3(0.0f, 3.0f, -2.5f);
        }
        const std::vector<MeshRange> ranges{ { 0, skinnedIndexCount, 0, true } };
        drawSkinnedMesh(cb, skinnedVb, skinnedIb, ranges, skinnedSkeleton, pose);
        if (!skinnedLogged) {
            skinnedLogged = true;
            std::cout << "[Game] Skinned mesh drawn (verts=" << skinnedVertexCount
                      << ", bones=" << skinnedSkeleton.bones.size()
                      << ") clip+ragdoll -> pose -> bone UBO\n";
        }
    }

    void destroyMeshResource(GameMeshResource& mesh) {
        if (mesh.vb != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, mesh.vb, mesh.vbAlloc);
        if (mesh.ib != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, mesh.ib, mesh.ibAlloc);
        mesh = GameMeshResource{};
    }

    void mainLoop() {
        auto lastTime = std::chrono::high_resolution_clock::now();
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;
            update(deltaTime);
            drawFrame();
            updateHud(deltaTime);
        }
        vkDeviceWaitIdle(device);
    }

    // Builds the gameplay script: an authored .script next to the scene wins;
    // otherwise a built-in controller accumulates WASD (moveX/moveY) into the
    // player's position (playerX/playerZ). Runs through the real ScriptVM.
    void setupGameplay() {
        const char* candidates[] = { "Content/Scenes/Initial.script", "Content/Initial.script" };
        bool loadedFile = false;
        for (const char* candidate : candidates) {
            if (gameplayGraph.load(candidate)) { loadedFile = true; break; }
        }
        if (!loadedFile) {
            gameplayGraph.name = "PlayerController";
            const auto constant = [&](double value) {
                return TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", value };
            };
            const auto setVar = [&](const std::string& var) {
                return TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", var };
            };
            const auto getVar = [&](const std::string& var) {
                return TypedScriptNode{ UUID(), ScriptNodeKind::GetVariable, "", var };
            };
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
            for (const auto& init : { std::pair{ std::string("playerX"), 0.0 }, { std::string("playerY"), 0.0 }, { std::string("playerZ"), 0.0 } }) {
                gameplayGraph.nodes.push_back(constant(init.second));
                gameplayGraph.nodes.push_back(setVar(init.first));
            }
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "Tick" });
            // playerX += moveX
            gameplayGraph.nodes.push_back(getVar("playerX"));
            gameplayGraph.nodes.push_back(getVar("moveX"));
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::AddFloat });
            gameplayGraph.nodes.push_back(setVar("playerX"));
            // playerZ += moveY
            gameplayGraph.nodes.push_back(getVar("playerZ"));
            gameplayGraph.nodes.push_back(getVar("moveY"));
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::AddFloat });
            gameplayGraph.nodes.push_back(setVar("playerZ"));
        }
        const auto compiled = ScriptCompiler::compile(gameplayGraph);
        if (!compiled) return;
        scriptVM.load(std::move(compiled.program));
        scriptVM.start_event("OnStart");
        gameplayLoaded = true;
        std::cout << "[Game] Player controller script " << (loadedFile ? "(authored)" : "(built-in)")
                  << " loaded, " << gameplayGraph.nodes.size() << " nodes\n";
    }

    // Feeds WASD into the script as moveX/moveY, ticks the controller and
    // applies the resulting playerX/playerZ to the Player entity's transform.
    void updateGameplay(float dt) {
        if (!gameplayLoaded) return;
        const float speed = 4.0f;
        const float dx = ((glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ? 1.0f : 0.0f) -
                          (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ? 1.0f : 0.0f)) * speed * dt;
        const float dz = ((glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ? 1.0f : 0.0f) -
                          (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ? 1.0f : 0.0f)) * speed * dt;
        scriptVM.set_variable("moveX", dx);
        scriptVM.set_variable("moveY", dz);
        scriptVM.start_event("Tick");
        scriptVM.run(dt, 256);
        std::vector<std::string> emitted;
        scriptVM.consume_emitted_events(emitted);
        for (const std::string& event : emitted) scriptVM.start_event(event);
        const auto tit = scene.transformComponents.find(playerEntityId);
        if (tit != scene.transformComponents.end()) {
            tit->second.position = glm::vec3(
                static_cast<float>(scriptVM.float_variable("playerX")),
                static_cast<float>(scriptVM.float_variable("playerY")),
                static_cast<float>(scriptVM.float_variable("playerZ")));
        }
        if (!gameplayStatusLogged) {
            gameplayStatusLogged = true;
            std::cout << "[Game] Player controller via script active (WASD -> moveX/moveY -> playerX/playerZ)\n";
        }
    }

    // FPS targets: a static physics floor plus dynamic crate bodies with
    // visible mesh entities. The hitscan weapon knocks them; 3 hits destroy a
    // crate (body removed, mesh hidden, HUD counter updated).
    void spawnTargets() {
        Physics::BodyDesc floor;
        floor.motion = Physics::MotionType::Static;
        floor.position = glm::vec3(0.0f, -0.6f, 0.0f);
        floor.collider.shape = Physics::BoxShape{ glm::vec3(40.0f, 0.6f, 40.0f) };
        floor.collider.friction = 0.8f;
        physics.create_body(floor);

        auto ground = scene.create_entity("Floor");
        scene.meshRendererComponents[ground.get_id()] = MeshRendererComponent{};
        scene.materialComponents[ground.get_id()] = MaterialComponent{
            glm::vec3(0.42f, 0.42f, 0.46f), 0.3f, 0.1f, glm::vec3(0.0f), 0.0f };
        TransformComponent groundTransform;
        groundTransform.position = glm::vec3(0.0f, -0.6f, 0.0f);
        groundTransform.scale = glm::vec3(80.0f, 1.2f, 80.0f);
        scene.transformComponents[ground.get_id()] = groundTransform;

        const glm::vec3 crateColors[] = {
            { 0.9f, 0.30f, 0.22f }, { 0.25f, 0.72f, 0.32f }, { 0.22f, 0.52f, 0.92f },
            { 0.95f, 0.80f, 0.20f }, { 0.80f, 0.30f, 0.80f }, { 0.20f, 0.85f, 0.85f } };
        for (int i = 0; i < 6; ++i) {
            const int row = i / 3;
            const int col = i % 3;
            const glm::vec3 pos{ 8.0f + col * 3.0f, 0.5f + row * 1.8f,
                                 (col == 1 ? 0.0f : (row == 0 ? -1.2f : 1.2f)) };
            auto entity = scene.create_entity("Target " + std::to_string(i + 1));
            const UUID id = entity.get_id();
            scene.meshRendererComponents[id] = MeshRendererComponent{};
            scene.materialComponents[id] = MaterialComponent{
                crateColors[i], 0.55f, 0.15f, glm::vec3(0.0f), 0.0f };
            TransformComponent t;
            t.position = pos;
            scene.transformComponents[id] = t;

            Physics::BodyDesc body;
            body.motion = Physics::MotionType::Dynamic;
            body.position = pos;
            body.collider.shape = Physics::BoxShape{ glm::vec3(0.5f) };
            body.collider.friction = 0.6f;
            body.collider.restitution = 0.35f;
            body.userData = static_cast<std::uint64_t>(i + 1);
            const Physics::BodyHandle handle = physics.create_body(body);
            fpsTargets.push_back(FpsTarget{ id, handle, 0, true });
        }
        std::cout << "[Game] FPS mode: " << fpsTargets.size()
                  << " target crates on a physics floor (3 hits to destroy)\n";
    }

    // FPS HUD: ammo/reserve, targets remaining and live FPS in the window title.
    void updateHud(float dt) {
        hudTimer -= dt;
        if (hudTimer > 0.0) return;
        hudTimer = 0.25;
        const int remaining = static_cast<int>(fpsTargets.size()) - targetsDestroyed;
        char title[192];
        std::snprintf(title, sizeof(title),
                      "VulkanCraft FPS | Ammo %u/%u%s | Targets %d/%d | %.0f FPS",
                      weapon.ammo(), weapon.reserve(),
                      weapon.reloading() ? " [RELOADING]" : "",
                      remaining, static_cast<int>(fpsTargets.size()),
                      1.0f / std::max(dt, 1e-5f));
        glfwSetWindowTitle(window, title);
    }

    void setupWeapon() {
        // The weapon is hitscan: each shot becomes a raycast against the same
        // physics world that steps rigidbodies/ragdolls in this frame.
        weapon.set_raycast([this](const glm::vec3& origin, const glm::vec3& dir, float maxDist)
                               -> std::optional<Engine::WeaponHit> {
            const auto hit = physics.raycast(origin, dir, maxDist);
            if (!hit) return std::nullopt;
            Engine::WeaponHit out;
            out.position = hit->point;
            out.normal = hit->normal;
            out.distance = hit->distance;
            lastHitBody = hit->body;
            lastHitPoint = hit->point;
            return out;
        });
        // Muzzle flash: a transient point light at the camera, lit for ~80ms
        // per shot by updateWeapon (intensity 0 keeps it out of the frame).
        auto muzzle = scene.create_entity("MuzzleFlash");
        muzzleLightEntity = muzzle.get_id();
        scene.lightComponents[muzzleLightEntity] = LightComponent{
            glm::vec3(1.0f, 0.75f, 0.35f), 0.0f, 12.0f, false, LightType::Point };
        scene.transformComponents[muzzleLightEntity].position = camPos;
    }

    void updateWeapon(float dt) {
        const glm::vec3 front = cameraFront();
        const bool wasHeld = mouseLeftPrev;
        mouseLeftPrev = mouseLeftHeld;
        const bool clicked = mouseLeftHeld && !wasHeld;
        const uint32_t ammoBefore = weapon.ammo();
        if (clicked) weapon.trigger_pressed(camPos, front);
        if (!mouseLeftHeld) weapon.trigger_released();
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) weapon.reload();
        weapon.update(dt, camPos, front);
        const bool fired = weapon.ammo() < ammoBefore;

        // Muzzle flash light: position at the muzzle, decay over ~80ms.
        muzzleFlashTimer = std::max(0.0f, muzzleFlashTimer - dt);
        if (fired) muzzleFlashTimer = 0.08f;
        const auto lit = scene.lightComponents.find(muzzleLightEntity);
        if (lit != scene.lightComponents.end()) {
            lit->second.intensity = muzzleFlashTimer > 0.0f
                ? 900.0f * (muzzleFlashTimer / 0.08f) : 0.0f;
            scene.transformComponents[muzzleLightEntity].position = camPos + front * 0.6f;
        }

        // Feed the gameplay script and dispatch weapon events (OnShoot when a
        // shot leaves the barrel, OnHit when the hitscan ray finds a body).
        scriptVM.set_variable("ammo", static_cast<double>(weapon.ammo()));
        scriptVM.set_variable("reserve", static_cast<double>(weapon.reserve()));
        scriptVM.set_variable("reloading", weapon.reloading() ? 1.0 : 0.0);
        std::vector<std::string> events;
        if (fired) events.push_back("OnShoot");
        const auto& hits = weapon.hits();
        if (hits.size() > weaponHitsSeen) {
            weaponHitsSeen = static_cast<uint32_t>(hits.size());
            const Engine::WeaponHit& h = hits.back();
            scriptVM.set_variable("hitDistance", static_cast<double>(h.distance));
            events.push_back("OnHit");
            if (!weaponHitLogged) {
                weaponHitLogged = true;
                std::cout << "[Game] Weapon hitscan raycast hit at ("
                          << h.position.x << ", " << h.position.y << ", "
                          << h.position.z << ") dist=" << h.distance
                          << " dmg=" << h.damage << "\n";
            }
            // FPS targets: knock the crate that was hit; 3 hits destroy it.
            for (FpsTarget& target : fpsTargets) {
                if (!target.alive || target.body != lastHitBody) continue;
                ++target.hits;
                physics.apply_impulse_at_point(target.body,
                    glm::normalize(front) * 7.0f + glm::vec3(0.0f, 1.8f, 0.0f), lastHitPoint);
                if (target.hits >= 3) {
                    target.alive = false;
                    physics.destroy_body(target.body);
                    const auto hitMesh = scene.meshRendererComponents.find(target.entity);
                    if (hitMesh != scene.meshRendererComponents.end()) hitMesh->second.isVisible = false;
                    ++targetsDestroyed;
                    std::cout << "[Game] Target crate destroyed — "
                              << (static_cast<int>(fpsTargets.size()) - targetsDestroyed)
                              << " left\n";
                }
                break;
            }
        }
        for (const std::string& ev : events) {
            scriptVM.start_event(ev);
            scriptVM.run(dt, 256);
            std::vector<std::string> emitted;
            scriptVM.consume_emitted_events(emitted);
            for (const std::string& e : emitted) scriptVM.start_event(e);
        }
        if (fired && !weaponStatusLogged) {
            weaponStatusLogged = true;
            std::cout << "[Game] Weapon 'Assault Rifle' firing via physics raycast"
                      << " (auto, dmg 25, spread 1.5deg, hitscan)\n";
        }
    }

    void update(float dt) {
        physics.step(dt);
        updateGameplay(dt);
        updateWeapon(dt);
        updateSunShadow();
        skinnedAnimTime += dt;

        // Ragdoll: toss the flag once it has settled, and log the Tip position
        // at 2s so headless runs prove the physics pose actually moved.
        if (ragdollBuilt && !ragdollImpulseApplied && skinnedAnimTime > 0.6f) {
            ragdoll.apply_impulse(physics, "Tip", glm::vec3(3.0f, 2.5f, 0.0f));
            ragdollImpulseApplied = true;
            std::cout << "[Game] Ragdoll impulse applied to Tip\n";
        }
        if (ragdollBuilt && !ragdollTipLogged && skinnedAnimTime >= 2.0f) {
            for (const Physics::RagdollPoseBone& bone : ragdoll.pose(physics)) {
                if (bone.name == "Tip") {
                    std::cout << "[Game] Ragdoll Tip pos after 2s: (" << bone.position.x
                              << ", " << bone.position.y << ", " << bone.position.z << ")\n";
                    ragdollTipLogged = true;
                    break;
                }
            }
        }

        // Free-fly camera.
        const float baseSpeed = 6.0f;
        const float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 3.0f : 1.0f) * baseSpeed * dt;
        const glm::vec3 front = cameraFront();
        const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0, 1, 0)));
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camPos += front * speed;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camPos -= front * speed;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camPos += right * speed;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camPos -= right * speed;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) camPos.y += speed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camPos.y -= speed;

        // F9 toggles captured-cursor mouse-look (right-drag look fallback).
        const bool f9Down = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
        if (f9Down && !f9WasPressed) {
            mouseLookEnabled = !mouseLookEnabled;
            glfwSetInputMode(window, GLFW_CURSOR,
                mouseLookEnabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        }
        f9WasPressed = f9Down;
    }

    void drawFrame() {
        vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            framebufferResized = false;
            return;
        }
        // The acquired image must be free before we submit new work on it: the
        // swapchain may still be presenting the previous frame that used it.
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = inFlightFences[currentFrame];

        vkResetFences(device, 1, &inFlightFences[currentFrame]);
        vkResetCommandBuffer(commandBuffers[currentFrame], 0);

        VkCommandBuffer cb = commandBuffers[currentFrame];
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &beginInfo);

        // The whole frame runs through the compiled render graph: the executor
        // begins each pass on its real framebuffer, inserts the compiled
        // barriers between passes, and invokes the per-pass draw callbacks.
        renderGraphExecutor.record(cb, imageIndex, swapChainExtent);

        vkEndCommandBuffer(cb);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cb;
        VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        vkQueuePresentKHR(graphicsQueue, &presentInfo);
        currentFrame = (currentFrame + 1) % imageAvailableSemaphores.size();
    }

    void cleanup() {
        if (ragdollBuilt) ragdoll.destroy(physics);
        destroyMeshResource(cubeMesh);
        for (auto& [id, mesh] : meshes) {
            (void)id;
            destroyMeshResource(mesh);
        }
        meshes.clear();
        renderGraphExecutor.shutdown();
        vkDestroyPipeline(device, compositePipeline, nullptr);
        vkDestroyPipelineLayout(device, compositePipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, compositeDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, compositeSetLayout, nullptr);
        vkDestroySampler(device, compositeSampler, nullptr);
        for (auto framebuffer : compositeFramebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        compositeFramebuffers.clear();
        vkDestroyFramebuffer(device, sceneFramebuffer, nullptr);
        vkDestroyImageView(device, hdrImageView, nullptr);
        vmaDestroyImage(allocator, hdrImage, hdrAllocation);
        vkDestroyPipeline(device, shadowPipeline, nullptr);
        vkDestroyPipelineLayout(device, shadowPipelineLayout, nullptr);
        vkDestroyFramebuffer(device, shadowFramebuffer, nullptr);
        vkDestroyRenderPass(device, shadowRenderPass, nullptr);
        vkDestroySampler(device, shadowMapSampler, nullptr);
        vkDestroyImageView(device, shadowMapView, nullptr);
        vmaDestroyImage(allocator, shadowMapImage, shadowMapAllocation);
        vkDestroyPipeline(device, volumPipeline, nullptr);
        vkDestroyPipelineLayout(device, volumPipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, volumDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, volumSetLayout, nullptr);
        vkDestroySampler(device, volumSampler, nullptr);
        vkDestroySampler(device, volumDepthSampler, nullptr);
        vkDestroyFramebuffer(device, volumFramebuffer, nullptr);
        vkDestroyRenderPass(device, volumRenderPass, nullptr);
        vmaDestroyBuffer(allocator, volumUboBuffer, volumUboAllocation);
        vkDestroyImageView(device, volumImageView, nullptr);
        vmaDestroyImage(allocator, volumImage, volumAllocation);
        vkDestroyPipeline(device, skinnedPipeline, nullptr);
        vkDestroyPipelineLayout(device, skinnedPipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, skinnedDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, skinnedSetLayout, nullptr);
        vmaDestroyBuffer(allocator, skinnedBoneBuffer, skinnedBoneAllocation);
        vmaDestroyBuffer(allocator, skinnedCameraBuffer, skinnedCameraAllocation);
        if (skinnedVb != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, skinnedVb, skinnedVbAllocation);
        if (skinnedIb != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, skinnedIb, skinnedIbAllocation);
        vkDestroyRenderPass(device, compositeRenderPass, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vmaDestroyBuffer(allocator, uboBuffer, uboAllocation);
        vmaDestroyBuffer(allocator, lightBuffer, lightAllocation);
        vkDestroyImageView(device, depthImageView, nullptr);
        vmaDestroyImage(allocator, depthImage, depthAllocation);
        vkDestroyRenderPass(device, renderPass, nullptr);
        for (auto imageView : swapChainImageViews) vkDestroyImageView(device, imageView, nullptr);
        vkDestroySwapchainKHR(device, swapChain, nullptr);
        vmaDestroyAllocator(allocator);
        for (size_t i = 0; i < 2; i++) {
            vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        destroyDebugMessenger();
        vkDestroyInstance(instance, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
    }
};

int main() {
    try {
        VulkanGame game;
        game.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#pragma once

#include "VulkanTypes.hpp"
#include "Player.hpp"
#include "World.hpp"
#include "WorldRenderer.hpp"
#include "Mob.hpp"
#include "TextureManager.hpp"
#include "MobRenderer.hpp"
#include "SoundEngine.hpp"
#include "RadianceCache.hpp"
#include <vector>

struct FrameData {
    VkCommandPool commandPool;
    VkCommandBuffer mainCommandBuffer;
    VkFence renderFence;
    VkSemaphore swapchainSemaphore;
};

class Engine {
public:
    bool isInitialized{ false };
    bool isPaused{ false };
    bool escWasPressed{ false };
    bool showGraphicsMenu{ false };
    bool cinematicEffects{ true };
    bool depthOfFieldEnabled{ true };
    bool thirdPerson{ false };
    bool f5WasPressed{ false };
    bool fullscreenWasPressed{ false };
    bool framebufferResized{ false };
    bool fullscreen{ false };
    int windowedX{ 100 };
    int windowedY{ 100 };
    int windowedWidth{ 1280 };
    int windowedHeight{ 720 };
    bool lodQuality0WasPressed{ false };
    bool lodQuality9WasPressed{ false };
    double lodQualityHeldSince{ 0.0 };
    double lodQualityNextRepeat{ 0.0 };

    int frameNumber{ 0 };
    VkExtent2D windowExtent{ 1280, 720 };
    struct GLFWwindow* window{ nullptr };

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkSurfaceKHR surface;

    VkSwapchainKHR swapchain;
    VkFormat swapchainImageFormat;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    VkExtent2D swapchainExtent;

    VkImage depthImage;
    VmaAllocation depthAllocation;
    VkImageView depthImageView;
    VkImage shadowImage{ VK_NULL_HANDLE };
    VmaAllocation shadowAllocation{};
    VkImageView shadowImageView{ VK_NULL_HANDLE };
    VkSampler shadowSampler{ VK_NULL_HANDLE };
    VkImage minimapImage{ VK_NULL_HANDLE };
    VmaAllocation minimapAllocation{};
    VkImageView minimapImageView{ VK_NULL_HANDLE };
    VkImage minimapDepthImage{ VK_NULL_HANDLE };
    VmaAllocation minimapDepthAllocation{};
    VkImageView minimapDepthView{ VK_NULL_HANDLE };
    VkSampler minimapSampler{ VK_NULL_HANDLE };

    VkImage hdrImage{ VK_NULL_HANDLE };
    VmaAllocation hdrAllocation{ VK_NULL_HANDLE };
    VkImageView hdrImageView{ VK_NULL_HANDLE };
    VkImage opaqueSceneImage{ VK_NULL_HANDLE };
    VmaAllocation opaqueSceneAllocation{ VK_NULL_HANDLE };
    VkImageView opaqueSceneImageView{ VK_NULL_HANDLE };
    VkImage opaqueDepthImage{ VK_NULL_HANDLE };
    VmaAllocation opaqueDepthAllocation{ VK_NULL_HANDLE };
    VkImageView opaqueDepthImageView{ VK_NULL_HANDLE };
    VkSampler waterSceneSampler{ VK_NULL_HANDLE };
    VkSampler postSampler{ VK_NULL_HANDLE };
    VkDescriptorSetLayout postDescriptorLayout{ VK_NULL_HANDLE };
    VkDescriptorPool postDescriptorPool{ VK_NULL_HANDLE };
    VkDescriptorSet postDescriptorSet{ VK_NULL_HANDLE };

    std::vector<VkSemaphore> renderSemaphores;

    FrameData frames[FRAME_OVERLAP];
    FrameData& get_current_frame() { return frames[frameNumber % FRAME_OVERLAP]; }

    VkQueue graphicsQueue;
    uint32_t graphicsQueueFamily;

    VmaAllocator allocator;

    VkPipelineLayout voxelPipelineLayout;
    VkPipeline voxelPipeline;
    VkPipeline farSurfacePipeline{ VK_NULL_HANDLE };
    VkPipeline waterPipeline{ VK_NULL_HANDLE };
    VkPipeline grassPipeline{ VK_NULL_HANDLE };
    VkPipeline foliagePipeline{ VK_NULL_HANDLE };
    VkPipeline skyPipeline{ VK_NULL_HANDLE };
    VkPipeline postPipeline{ VK_NULL_HANDLE };
    VkPipeline shadowPipeline{ VK_NULL_HANDLE };
    VkPipeline shadowFarSurfacePipeline{ VK_NULL_HANDLE };
    VkPipeline shadowFoliagePipeline{ VK_NULL_HANDLE };
    VkPipeline shadowGrassPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout postPipelineLayout{ VK_NULL_HANDLE };

    glm::vec3 currentSunDirection{ 0.42f, 0.86f, 0.29f };
    glm::vec3 currentLightColor{ 1.18f, 1.10f, 0.94f };
    float currentDaylight{ 1.0f };
    float currentExposure{ 1.1f };
    float worldVisualTime{ 0.0f };
    glm::vec3 previousCameraFront{ 0.0f, 0.0f, -1.0f };

    // Buffer da Malha do Braço do Personagem
    AllocatedBuffer armBuffer;
    uint32_t armVertexCount{ 0 };
    AllocatedBuffer heldBlockBuffer;
    uint32_t heldBlockVertexCount{ 0 };
    AllocatedBuffer characterBuffer;
    uint32_t characterVertexCount{ 0 };

    SoundEngine soundEngine;
    MobManager mobManager{soundEngine};
    MobRenderer mobRenderer;
    World world{mobManager};
    WorldRenderer worldRenderer{world};
    Player player;
    TextureManager textureManager;
    RadianceCache radianceCache;

    float deltaTime{ 0.0f };
    float lastFrameTime{ 0.0f };

    Engine() = default;
    ~Engine() = default;

    void init();
    void run();
    void cleanup();

private:
    void init_window();
    void init_vulkan();
    void init_swapchain();
    void create_swapchain(VkSwapchainKHR oldSwapchain);
    bool recreate_swapchain();
    void destroy_screen_targets();
    void initialize_screen_target_layouts();
    void update_screen_descriptors();
    void toggle_fullscreen();
    void init_depth_buffer();
    void init_shadow_map();
    void init_minimap();
    void init_hdr_target();
    void init_commands();
    void init_sync_structures();
    void init_pipeline();
    void init_arm_mesh();
    void draw_arm(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);
    void draw_character(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);

    void draw();

    VkShaderModule load_shader_module(const std::string& filePath);

    static void mouse_callback(GLFWwindow* window, double xpos, double ypos);
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
};

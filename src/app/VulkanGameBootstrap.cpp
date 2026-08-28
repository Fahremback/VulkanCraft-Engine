#include "VulkanGame.hpp"
#include "VulkanGameSupport.hpp"

void VulkanGame::initWindow(){
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

void VulkanGame::initVulkan(){
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

void VulkanGame::createInstance(){
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

void VulkanGame::setupDebugMessenger(){
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

void VulkanGame::destroyDebugMessenger(){
        if (debugMessenger == VK_NULL_HANDLE) return;
        const auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }

void VulkanGame::createSurface(){
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface!");
        }
    }

void VulkanGame::pickPhysicalDevice(){
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) throw std::runtime_error("failed to find GPUs with Vulkan support!");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        physicalDevice = devices[0];
    }

void VulkanGame::createLogicalDevice(){
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

void VulkanGame::createAllocator(){
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        vmaCreateAllocator(&allocatorInfo, &allocator);
    }

void VulkanGame::createSwapChain(){
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

void VulkanGame::createImageViews(){
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

void VulkanGame::createDepthResources(){
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

void VulkanGame::createRenderPass(){
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

void VulkanGame::createFramebuffers(){
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

void VulkanGame::createCommandPool(){
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

void VulkanGame::createSyncObjects(){
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

VkShaderModule VulkanGame::createShaderModule(const std::string& path){
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


void VulkanGame::run() {
    initWindow();
    initVulkan();
    initScene();
    spawnTargets();
    setupWeapon();
    initPipelines();
    mainLoop();
    cleanup();
}

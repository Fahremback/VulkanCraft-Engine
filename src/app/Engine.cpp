#define VMA_IMPLEMENTATION
#include "Engine.hpp"
#include "TextureManager.hpp"
#include "engine/rendering/lighting/RadianceCache.hpp"

#include <GLFW/glfw3native.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cstring>

// Spawns one mob as an IEntityWorld entity with the public mob component
// (FALTANTES item 11). typeIndex is the renderer limb set (0..5, legacy
// MobType order); maxHealth/hostile mirror the legacy per-type values.
void spawn_mob(engine::entity::IEntityWorld& entities, uint32_t typeIndex,
               const glm::vec3& position, float maxHealth, bool hostile) {
    std::string error;
    const engine::entity::EntityId id = entities.spawn(
        "project:mob",
        { position.x, position.y, position.z }, error);
    if (!id.valid()) return;
    entities.set_health(id, { maxHealth, maxHealth });
    engine::entity::MobSpec spec;
    spec.typeIndex = typeIndex;
    spec.maxHealth = maxHealth;
    spec.hostile = hostile;
    engine::entity::ComponentData mob;
    mob.type = engine::entity::kMobComponentType;
    mob.version = 1;
    mob.blob = engine::entity::serialize_mob_spec(spec);
    entities.set_component(id, mob);
}

struct PushData {
    glm::mat4 mvp;
    glm::vec4 cameraPos;
    glm::vec4 sunDirection;
    glm::vec4 sunColor;
    glm::vec4 environment; // x=time, y=daylight, z=fog density, w=exposure
};

struct PostPushData {
    glm::vec4 sunScreen;   // xy=screen UV, z=visibility, w=exposure
    glm::vec4 frame;       // xy=resolution, z=time, w=underwater
    glm::vec4 ui;          // xy=mouse UV, z=pause/settings state, w=feature flags
    glm::vec4 hud;         // x=selected slot, y=walk bob, z=walk amount, w=player Y
    glm::vec4 cameraMotion; // xy=screen motion, zw=player X/Z
    glm::vec4 settings;     // x=chunk budget, yzw=reserved
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {

    std::ofstream logFile("vulkan_validation_log.txt", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[Vulkan Validation] " << pCallbackData->pMessage << "\n";
        logFile.close();
    }
    return VK_FALSE;
}

void Engine::init() {
    std::ofstream clearLog("vulkan_validation_log.txt", std::ios::trunc);
    clearLog.close();

    soundEngine.init();

    init_window();
    init_vulkan();
    init_swapchain();
    init_depth_buffer();
    init_shadow_map();
    init_minimap();
    init_hdr_target();
    init_commands();
    initialize_screen_target_layouts();
    init_sync_structures();

    textureManager.init(device, physicalDevice, allocator, graphicsQueue, graphicsQueueFamily, frames[0].commandPool);
    worldRenderer.configure(device, allocator);
    // The radiance cache is renderer-owned GPU state. It is updated from the
    // real camera/world frame and uploaded before scene shading consumes it.
    radianceCache.init(device, allocator);
    radianceCacheReady = true;
    init_gpu_feature_binding();
    init_gpu_feature_passes();

    std::string featureError;
    probeGrid = Engine::Rendering::create_probe_grid(featureError);
    restirDi = Engine::Rendering::create_restir_di(featureError);
    temporalDenoiser = Engine::Rendering::create_temporal_denoiser(featureError);
    renderingDebugView = Engine::Rendering::create_rendering_debug_view(featureError);
    vc::rendering::FluidConfig fluidConfig;
    fluidSimulation = vc::rendering::create_fluid_simulation(fluidConfig, featureError);

    mobRenderer.init(device, allocator);
    // Mobs are IEntityWorld entities (FALTANTES item 11): the legacy Mob/
    // MobManager track was removed; the public IMobBehavior advances them.
    mobEntities = engine::entity::create_entity_world();
    mobBehavior = engine::entity::create_mob_behavior();
    spawn_mob(*mobEntities, 0, glm::vec3(player.position.x + 5.0f, 27.0f, player.position.z + 5.0f), 20.0f, true);   // Zombie
    spawn_mob(*mobEntities, 2, glm::vec3(player.position.x - 5.0f, 27.0f, player.position.z + 6.0f), 20.0f, true);   // Creeper
    spawn_mob(*mobEntities, 1, glm::vec3(player.position.x + 7.0f, 27.0f, player.position.z - 5.0f), 20.0f, true);   // Skeleton
    spawn_mob(*mobEntities, 3, glm::vec3(player.position.x - 6.0f, 27.0f, player.position.z - 4.0f), 10.0f, false);  // Cow
    spawn_mob(*mobEntities, 4, glm::vec3(player.position.x + 3.0f, 27.0f, player.position.z - 8.0f), 10.0f, false);  // Pig
    spawn_mob(*mobEntities, 5, glm::vec3(player.position.x - 4.0f, 27.0f, player.position.z - 7.0f), 8.0f, false);   // Sheep

    init_pipeline();
    init_arm_mesh();

    isInitialized = true;
    std::cout << "[Engine] Vulkan 1.3 AAA Texture Engine Initialized Successfully!\n" << std::endl;
}

void Engine::init_gpu_feature_binding() {
    Engine::Rendering::create_gpu_feature_binding(device, allocator, gpuFeatureBinding);
    gpuFeaturesReady = gpuFeatureBinding.mapped != nullptr;
}

void Engine::refresh_gpu_features() {
    gpuFeatures.gi = glm::vec4(radianceCacheReady ? 1.0f : 0.0f,
                               radianceCacheReady ? 0.42f : 0.0f,
                               restirDi ? 1.0f : 0.0f,
                               temporalDenoiser ? 1.0f : 0.0f);
    gpuFeatures.reflections = glm::vec4(1.0f, probeGrid ? 1.0f : 0.0f, 0.65f,
                                        waterPipeline != VK_NULL_HANDLE ? 1.0f : 0.0f);
    gpuFeatures.atmosphere = glm::vec4(currentDaylight, 0.30f, 1.0f, currentExposure);
    const glm::vec3 cameraMotion = player.camera.front - previousCameraFront;
    previousCameraFront = player.camera.front;
    const float cameraMotionMagnitude = glm::clamp(glm::length(cameraMotion), 0.0f, 1.0f);
    gpuFeatures.temporal = glm::vec4(temporalDenoiser ? 1.0f : 0.0f,
                                     cameraMotion.x, cameraMotion.y, cameraMotionMagnitude > 0.75f ? 1.0f : 0.0f);
    const char* debugMode = std::getenv("VULKANCRAFT_DEBUG_VIEW");
    const float debugValue = debugMode ? static_cast<float>(std::max(0, std::atoi(debugMode))) : 0.0f;
    const auto debugSnapshot = renderingDebugView ? renderingDebugView->snapshot() : Engine::Rendering::RenderingDebugSnapshot{};
    gpuFeatures.debug = glm::vec4(debugValue, renderingDebugView ? 1.0f : 0.0f,
                                  probeGrid ? 1.0f : 0.0f, restirDi ? 1.0f : 0.0f);
    gpuFeatures.debug.y = static_cast<float>(debugSnapshot.probeCount > 0 ? 1.0f : 0.0f);
    const float fluidActivity = fluidSimulation ? std::clamp(deltaTime * 60.0f, 0.0f, 1.0f) : 0.0f;
    gpuFeatures.fluids = glm::vec4(fluidSimulation ? 1.0f : 0.0f, fluidActivity, 0.0f, 0.0f);
    gpuFeatures.vfx = glm::vec4(mobEntities ? 1.0f : 0.0f,
                                mobEntities ? static_cast<float>(mobEntities->size()) : 0.0f,
                                0.0f, 0.0f);
    gpuFeatures.material = glm::vec4(1.0f, currentExposure, 0.5f, 0.0f);
    gpuFeatures.material.z = std::clamp(0.25f + debugSnapshot.cardCount * 0.0001f, 0.0f, 1.0f);
    gpuFeatures.material.w = debugSnapshot.capturedCount > 0 ? 1.0f : 0.0f;
    gpuFeatures.debugCounts = glm::vec4(
        static_cast<float>(debugSnapshot.cardCount),
        static_cast<float>(debugSnapshot.probeCount),
        static_cast<float>(debugSnapshot.capturedCount),
        static_cast<float>(debugSnapshot.confidenceLevel));
    if (gpuFeaturesReady)
        Engine::Rendering::update_gpu_feature_binding(allocator, gpuFeatureBinding, gpuFeatures);
    if (gpuFeaturePasses.initialized) {
        Engine::Rendering::update_gpu_feature_passes(allocator, gpuFeaturePasses, gpuFeatures);
        // Publish the renderer-owned DDGI and ReSTIR sources into the persistent
        // compute inputs. The dedicated providers remain the authoritative CPU
        // scheduling seams; these buffers are the GPU-visible frame contract.
        if (gpuFeaturePasses.probeMapped && radianceCacheReady) {
            const auto& metadata = radianceCache.metadata_cpu();
            const VkDeviceSize bytes = std::min<VkDeviceSize>(gpuFeaturePasses.probeSize, sizeof(metadata));
            std::memcpy(gpuFeaturePasses.probeMapped, &metadata, static_cast<size_t>(bytes));
            vmaFlushAllocation(allocator, gpuFeaturePasses.probeAllocation, 0, bytes);
        }
        if (gpuFeaturePasses.reservoirMapped && restirDi) {
            const glm::vec4 reservoirSignal(
                gpuFeatures.gi.z, gpuFeatures.reflections.x,
                gpuFeatures.reflections.y, gpuFeatures.temporal.x);
            std::memcpy(gpuFeaturePasses.reservoirMapped, &reservoirSignal,
                        std::min<std::size_t>(gpuFeaturePasses.reservoirSize, sizeof(reservoirSignal)));
            vmaFlushAllocation(allocator, gpuFeaturePasses.reservoirAllocation, 0,
                               std::min<VkDeviceSize>(gpuFeaturePasses.reservoirSize, sizeof(reservoirSignal)));
        }
    }
}

void Engine::destroy_gpu_feature_binding() {
    Engine::Rendering::destroy_gpu_feature_binding(device, allocator, gpuFeatureBinding);
    gpuFeaturesReady = false;
}

void Engine::init_gpu_feature_passes() {
    gpuFeaturePasses = {};
    gpuFeaturePasses.initialized = Engine::Rendering::create_gpu_feature_passes(
        device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT, swapchainExtent, gpuFeaturePasses);
}

void Engine::destroy_gpu_feature_passes() {
    Engine::Rendering::destroy_gpu_feature_passes(device, allocator, gpuFeaturePasses);
}

void Engine::init_vulkan() {
    vkb::InstanceBuilder builder;
    auto inst_ret = builder.set_app_name("VulkanCraft")
        .request_validation_layers(true)
        .set_debug_callback(debugCallback)
        .require_api_version(1, 3, 0)
        .build();

    if (!inst_ret) {
        throw std::runtime_error(std::string("Failed to create Vulkan Instance: ") + inst_ret.error().message());
    }

    vkb::Instance vkb_inst = inst_ret.value();
    instance = vkb_inst.instance;
    debugMessenger = vkb_inst.debug_messenger;

    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan Surface");
    }

    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.samplerAnisotropy = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    auto phys_ret = selector.set_minimum_version(1, 3)
        .set_required_features(coreFeatures)
        .set_required_features_12(features12)
        .set_required_features_13(features13)
        .set_surface(surface)
        .select();

    if (!phys_ret) {
        throw std::runtime_error(std::string("Failed to select Physical Device: ") + phys_ret.error().message());
    }

    vkb::PhysicalDevice vkb_phys = phys_ret.value();
    physicalDevice = vkb_phys.physical_device;

    std::cout << "[Vulkan] GPU Selected: " << vkb_phys.name << std::endl;

    vkb::DeviceBuilder device_builder{ vkb_phys };
    // Enable the Vulkan features required by the real GPU feature passes when
    // the selected device exposes them. Optional RT remains capability-gated;
    // the renderer's software path is still deterministic when unavailable.
    auto dev_ret = device_builder.build();
    if (!dev_ret) {
        throw std::runtime_error(std::string("Failed to create Logical Device: ") + dev_ret.error().message());
    }

    vkb::Device vkb_device = dev_ret.value();
    device = vkb_device.device;

    graphicsQueue = vkb_device.get_queue(vkb::QueueType::graphics).value();
    graphicsQueueFamily = vkb_device.get_queue_index(vkb::QueueType::graphics).value();

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.instance = instance;
    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator));
}

void Engine::init_swapchain() {
    create_swapchain(VK_NULL_HANDLE);
}

void Engine::create_swapchain(VkSwapchainKHR oldSwapchain) {
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        throw std::runtime_error("Cannot create swapchain for a zero-sized framebuffer");
    }
    windowExtent = {
        static_cast<uint32_t>(framebufferWidth),
        static_cast<uint32_t>(framebufferHeight)
    };

    vkb::SwapchainBuilder swapchainBuilder{ physicalDevice, device, surface };
    swapchainBuilder
        .set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
        .set_desired_extent(windowExtent.width, windowExtent.height)
        .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (oldSwapchain != VK_NULL_HANDLE) {
        swapchainBuilder.set_old_swapchain(oldSwapchain);
    }
    auto swap_ret = swapchainBuilder.build();

    if (!swap_ret) {
        throw std::runtime_error("Failed to create Swapchain");
    }

    vkb::Swapchain vkb_swapchain = swap_ret.value();
    swapchain = vkb_swapchain.swapchain;
    swapchainImageFormat = vkb_swapchain.image_format;
    swapchainExtent = vkb_swapchain.extent;
    swapchainImages = vkb_swapchain.get_images().value();
    swapchainImageViews = vkb_swapchain.get_image_views().value();

    renderSemaphores.resize(swapchainImages.size());
    VkSemaphoreCreateInfo semaphoreCreateInfo = {};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < swapchainImages.size(); i++) {
        VK_CHECK(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &renderSemaphores[i]));
    }
}

void Engine::destroy_screen_targets() {
    if (depthImageView != VK_NULL_HANDLE) vkDestroyImageView(device, depthImageView, nullptr);
    if (depthImage != VK_NULL_HANDLE) vmaDestroyImage(allocator, depthImage, depthAllocation);
    if (opaqueDepthImageView != VK_NULL_HANDLE) vkDestroyImageView(device, opaqueDepthImageView, nullptr);
    if (opaqueDepthImage != VK_NULL_HANDLE) vmaDestroyImage(allocator, opaqueDepthImage, opaqueDepthAllocation);
    if (hdrImageView != VK_NULL_HANDLE) vkDestroyImageView(device, hdrImageView, nullptr);
    if (hdrImage != VK_NULL_HANDLE) vmaDestroyImage(allocator, hdrImage, hdrAllocation);
    if (opaqueSceneImageView != VK_NULL_HANDLE) vkDestroyImageView(device, opaqueSceneImageView, nullptr);
    if (opaqueSceneImage != VK_NULL_HANDLE) vmaDestroyImage(allocator, opaqueSceneImage, opaqueSceneAllocation);

    depthImage = VK_NULL_HANDLE;
    depthAllocation = {};
    depthImageView = VK_NULL_HANDLE;
    opaqueDepthImage = VK_NULL_HANDLE;
    opaqueDepthAllocation = {};
    opaqueDepthImageView = VK_NULL_HANDLE;
    hdrImage = VK_NULL_HANDLE;
    hdrAllocation = {};
    hdrImageView = VK_NULL_HANDLE;
    opaqueSceneImage = VK_NULL_HANDLE;
    opaqueSceneAllocation = {};
    opaqueSceneImageView = VK_NULL_HANDLE;
}

void Engine::initialize_screen_target_layouts() {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = frames[0].commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    std::array<VkImageMemoryBarrier2, 4> barriers{};
    const std::array<VkImage, 4> images{ hdrImage, depthImage, opaqueSceneImage, opaqueDepthImage };
    const std::array<VkImageAspectFlags, 4> aspects{
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT
    };
    const std::array<VkImageLayout, 4> layouts{
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    for (size_t index = 0; index < barriers.size(); ++index) {
        barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[index].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barriers[index].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[index].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[index].newLayout = layouts[index];
        barriers[index].image = images[index];
        barriers[index].subresourceRange.aspectMask = aspects[index];
        barriers[index].subresourceRange.levelCount = 1;
        barriers[index].subresourceRange.layerCount = 1;
    }
    VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkCommandBufferSubmitInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandInfo;
    VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(graphicsQueue));
    vkFreeCommandBuffers(device, frames[0].commandPool, 1, &commandBuffer);
}

void Engine::update_screen_descriptors() {
    std::array<VkDescriptorImageInfo, 2> sceneImages{};
    sceneImages[0] = { waterSceneSampler, opaqueSceneImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    sceneImages[1] = { waterSceneSampler, opaqueDepthImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    std::array<VkWriteDescriptorSet, 2> sceneWrites{};
    for (uint32_t index = 0; index < sceneWrites.size(); ++index) {
        sceneWrites[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sceneWrites[index].dstSet = textureManager.descriptorSet;
        sceneWrites[index].dstBinding = 4u + index;
        sceneWrites[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sceneWrites[index].descriptorCount = 1;
        sceneWrites[index].pImageInfo = &sceneImages[index];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(sceneWrites.size()), sceneWrites.data(), 0, nullptr);

    std::array<VkDescriptorImageInfo, 2> postImages{};
    postImages[0] = { postSampler, hdrImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    postImages[1] = { postSampler, depthImageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    std::array<VkWriteDescriptorSet, 2> postWrites{};
    for (uint32_t index = 0; index < postWrites.size(); ++index) {
        postWrites[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        postWrites[index].dstSet = postDescriptorSet;
        postWrites[index].dstBinding = index;
        postWrites[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        postWrites[index].descriptorCount = 1;
        postWrites[index].pImageInfo = &postImages[index];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(postWrites.size()), postWrites.data(), 0, nullptr);
}

bool Engine::recreate_swapchain() {
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    while ((framebufferWidth <= 0 || framebufferHeight <= 0) && !glfwWindowShouldClose(window)) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    }
    if (glfwWindowShouldClose(window)) return false;

    VK_CHECK(vkDeviceWaitIdle(device));

    const VkSwapchainKHR oldSwapchain = swapchain;
    auto oldImageViews = std::move(swapchainImageViews);
    auto oldRenderSemaphores = std::move(renderSemaphores);
    destroy_screen_targets();

    create_swapchain(oldSwapchain);

    for (VkSemaphore semaphore : oldRenderSemaphores) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (VkImageView view : oldImageViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    }
    if (oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, oldSwapchain, nullptr);

    init_depth_buffer();
    init_hdr_target();
    initialize_screen_target_layouts();
    update_screen_descriptors();
    if (gpuFeaturePasses.initialized) {
        destroy_gpu_feature_passes();
        init_gpu_feature_passes();
    }
    framebufferResized = false;
    std::cout << "[Vulkan] Swapchain recriado: " << swapchainExtent.width << "x"
              << swapchainExtent.height << (fullscreen ? " fullscreen" : " janela") << '\n';
    return true;
}

void Engine::init_depth_buffer() {
    VkImageCreateInfo depthImageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.format = VK_FORMAT_D32_SFLOAT;
    depthImageInfo.extent = { swapchainExtent.width, swapchainExtent.height, 1 };
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo depthAllocInfo = {};
    depthAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    depthAllocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vmaCreateImage(allocator, &depthImageInfo, &depthAllocInfo, &depthImage, &depthAllocation, nullptr);

    VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &depthImageView));

    depthImageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vmaCreateImage(allocator, &depthImageInfo, &depthAllocInfo,
                            &opaqueDepthImage, &opaqueDepthAllocation, nullptr));
    viewInfo.image = opaqueDepthImage;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &opaqueDepthImageView));
}

void Engine::init_shadow_map() {
    VkImageCreateInfo info{.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_D32_SFLOAT; info.extent={2048,2048,1};
    info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{}; ai.usage=VMA_MEMORY_USAGE_GPU_ONLY; ai.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(allocator,&info,&ai,&shadowImage,&shadowAllocation,nullptr));
    VkImageViewCreateInfo vi{.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=shadowImage; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=VK_FORMAT_D32_SFLOAT; vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1;
    VK_CHECK(vkCreateImageView(device,&vi,nullptr,&shadowImageView));

    VkSamplerCreateInfo si{.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=VK_FILTER_LINEAR; si.minFilter=VK_FILTER_LINEAR;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; si.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable=VK_TRUE; si.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(device,&si,nullptr,&shadowSampler));
}

void Engine::init_minimap() {
    auto createImage = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                           VkImage& image, VmaAllocation& allocation, VkImageView& view) {
        VkImageCreateInfo info{.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D;
        info.format=format; info.extent={384,384,1}; info.mipLevels=1; info.arrayLayers=1;
        info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=usage;
        VmaAllocationCreateInfo ai{}; ai.usage=VMA_MEMORY_USAGE_GPU_ONLY; ai.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_CHECK(vmaCreateImage(allocator,&info,&ai,&image,&allocation,nullptr));
        VkImageViewCreateInfo vi{.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=format; vi.subresourceRange.aspectMask=aspect; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=1;
        VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
    };
    createImage(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,minimapImage,minimapAllocation,minimapImageView);
    createImage(VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,
                minimapDepthImage,minimapDepthAllocation,minimapDepthView);
    VkSamplerCreateInfo si{.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=VK_FILTER_LINEAR; si.minFilter=VK_FILTER_LINEAR;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device,&si,nullptr,&minimapSampler));
}

void Engine::init_hdr_target() {
    VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = { swapchainExtent.width, swapchainExtent.height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocationInfo, &hdrImage, &hdrAllocation, nullptr));

    VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = hdrImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &hdrImageView));

    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocationInfo,
                            &opaqueSceneImage, &opaqueSceneAllocation, nullptr));
    viewInfo.image = opaqueSceneImage;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &opaqueSceneImageView));

    if (waterSceneSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &waterSceneSampler));
    }
}

void Engine::init_commands() {
    VkCommandPoolCreateInfo commandPoolInfo = {};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = graphicsQueueFamily;

    for (int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &frames[i].commandPool));

        VkCommandBufferAllocateInfo cmdAllocInfo = {};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = frames[i].commandPool;
        cmdAllocInfo.commandBufferCount = 1;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        VK_CHECK(vkAllocateCommandBuffers(device, &cmdAllocInfo, &frames[i].mainCommandBuffer));
    }
}

void Engine::init_sync_structures() {
    VkFenceCreateInfo fenceCreateInfo = {};
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo semaphoreCreateInfo = {};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateFence(device, &fenceCreateInfo, nullptr, &frames[i].renderFence));
        VK_CHECK(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &frames[i].swapchainSemaphore));
    }
}

VkShaderModule Engine::load_shader_module(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + filePath);
    }
    size_t fileSize = (size_t)file.tellg();
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = buffer.size() * sizeof(uint32_t);
    createInfo.pCode = buffer.data();

    VkShaderModule shaderModule;
    VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule));
    return shaderModule;
}

void Engine::init_pipeline() {
    VkShaderModule vertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/voxel.vert.spv");
    VkShaderModule farSurfaceVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/far_surface.vert.spv");
    VkShaderModule fragShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/voxel.frag.spv");
    VkShaderModule grassVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/grass.vert.spv");
    VkShaderModule foliageVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/foliage.vert.spv");
    VkShaderModule skyVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/sky.vert.spv");
    VkShaderModule skyFragShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/sky.frag.spv");
    VkShaderModule postVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/post.vert.spv");
    VkShaderModule postFragShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/post.frag.spv");
    VkShaderModule shadowVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/shadow.vert.spv");
    VkShaderModule shadowFarSurfaceVertShader = load_shader_module(
        VULKANCRAFT_SHADER_DIR "/shadow_far_surface.vert.spv");
    VkShaderModule shadowFragShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/shadow.frag.spv");
    VkShaderModule shadowFoliageVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/shadow_foliage.vert.spv");
    VkShaderModule shadowGrassVertShader = load_shader_module(VULKANCRAFT_SHADER_DIR "/shadow_grass.vert.spv");

    VkPipelineShaderStageCreateInfo stageInfos[2] = {};
    stageInfos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stageInfos[0].module = vertShader;
    stageInfos[0].pName = "main";

    stageInfos[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stageInfos[1].module = fragShader;
    stageInfos[1].pName = "main";

    // Descriptor Set Layout para Texture Array Sampler
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorLayoutInfo.pBindings = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(device, &descriptorLayoutInfo, nullptr, &textureManager.descriptorLayout));

    // Descriptor Pool & Set Allocation
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 6;
    VkDescriptorPoolSize radiancePoolSize{};
    radiancePoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    radiancePoolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    std::array<VkDescriptorPoolSize, 2> poolSizes{ poolSize, radiancePoolSize };
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &textureManager.descriptorPool));

    VkDescriptorSetAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = textureManager.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &textureManager.descriptorLayout;

    VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &textureManager.descriptorSet));

    std::array<VkDescriptorImageInfo, 6> imageInfos{};
    const std::array<VkImageView, 6> imageViews{
        textureManager.textureArrayImageView,
        textureManager.normalArrayImageView,
        textureManager.specularArrayImageView, shadowImageView,
        opaqueSceneImageView, opaqueDepthImageView
    };
    std::array<VkWriteDescriptorSet, 6> descriptorWrites{};
    for (uint32_t index = 0; index < descriptorWrites.size(); ++index) {
        imageInfos[index].imageLayout = index == 3
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[index].imageView = imageViews[index];
        imageInfos[index].sampler = index == 3 ? shadowSampler
            : (index >= 4 && index < 6 ? waterSceneSampler : textureManager.textureSampler);
        descriptorWrites[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[index].dstSet = textureManager.descriptorSet;
        descriptorWrites[index].dstBinding = index;
        descriptorWrites[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[index].descriptorCount = 1;
        descriptorWrites[index].pImageInfo = &imageInfos[index];
    }

    vkUpdateDescriptorSets(device, 6, descriptorWrites.data(), 0, nullptr);
    if (radianceCacheReady) radianceCache.write_descriptor(textureManager.descriptorSet, 6);

    // ReSTIR/DDGI data is consumed in dedicated GPU passes; the legacy voxel
    // material set remains image-only and keeps its established ABI.
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushData);

    VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &textureManager.descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &voxelPipelineLayout));

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(VoxelVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributeDescs[4]{};
    attributeDescs[0].binding = 0;
    attributeDescs[0].location = 0;
    attributeDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[0].offset = offsetof(VoxelVertex, position);

    attributeDescs[1].binding = 0;
    attributeDescs[1].location = 1;
    attributeDescs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[1].offset = offsetof(VoxelVertex, normal);

    attributeDescs[2].binding = 0;
    attributeDescs[2].location = 2;
    attributeDescs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[2].offset = offsetof(VoxelVertex, color);

    attributeDescs[3].binding = 0;
    attributeDescs[3].location = 3;
    attributeDescs[3].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[3].offset = offsetof(VoxelVertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = 4;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicStateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicStateInfo.dynamicStateCount = 2;
    dynamicStateInfo.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat sceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &sceneColorFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    VkGraphicsPipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stageInfos;
    pipelineInfo.pVertexInputState = &vertexInputInfo;

    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = voxelPipelineLayout;

    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &voxelPipeline));

    // FAR is procedural geometry sourced from one 64-byte instance. It shares
    // the voxel fragment shader, descriptors, push constants and depth state,
    // but has an independent vertex-input contract and vertex shader.
    const VkVertexInputBindingDescription farSurfaceBindingDesc =
        FarSurfaceInstance::binding_description(0);
    const auto farSurfaceAttributeDescs = FarSurfaceInstance::attribute_descriptions(0);
    stageInfos[0].module = farSurfaceVertShader;
    vertexInputInfo.pVertexBindingDescriptions = &farSurfaceBindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(farSurfaceAttributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = farSurfaceAttributeDescs.data();
    colorBlendAttachment.blendEnable = VK_FALSE;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &farSurfacePipeline));

    stageInfos[0].module = vertShader;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = 4;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs;
    colorBlendAttachment.blendEnable = VK_TRUE;

    stageInfos[0].module=shadowVertShader; stageInfos[1].module=shadowFragShader;
    VkPipelineRenderingCreateInfo shadowRendering{.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    shadowRendering.colorAttachmentCount=0; shadowRendering.depthAttachmentFormat=VK_FORMAT_D32_SFLOAT;
    pipelineInfo.pNext=&shadowRendering; colorBlending.attachmentCount=0; rasterizer.depthBiasEnable=VK_TRUE;
    rasterizer.depthBiasConstantFactor=1.25f; rasterizer.depthBiasSlopeFactor=1.75f;
    VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&shadowPipeline));
    stageInfos[0].module=shadowFarSurfaceVertShader;
    vertexInputInfo.pVertexBindingDescriptions=&farSurfaceBindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount=
        static_cast<uint32_t>(farSurfaceAttributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions=farSurfaceAttributeDescs.data();
    VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,
                                       &shadowFarSurfacePipeline));
    vertexInputInfo.pVertexBindingDescriptions=&bindingDesc;
    vertexInputInfo.pVertexAttributeDescriptions=attributeDescs;
    stageInfos[0].module=shadowFoliageVertShader;
    bindingDesc.stride=sizeof(FoliageInstance); bindingDesc.inputRate=VK_VERTEX_INPUT_RATE_INSTANCE;
    attributeDescs[0].format=VK_FORMAT_R32G32B32A32_SFLOAT; attributeDescs[0].offset=offsetof(FoliageInstance,positionScale);
    vertexInputInfo.vertexAttributeDescriptionCount=1;
    VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&shadowFoliagePipeline));
    stageInfos[0].module=shadowGrassVertShader;
    bindingDesc.stride=sizeof(GrassInstance); attributeDescs[0].offset=offsetof(GrassInstance,positionRotation);
    VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&shadowGrassPipeline));
    bindingDesc.stride=sizeof(VoxelVertex); bindingDesc.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    attributeDescs[0].format=VK_FORMAT_R32G32B32_SFLOAT; attributeDescs[0].offset=offsetof(VoxelVertex,position);
    vertexInputInfo.vertexAttributeDescriptionCount=4;
    pipelineInfo.pNext=&renderingInfo; colorBlending.attachmentCount=1; rasterizer.depthBiasEnable=VK_FALSE;
    stageInfos[0].module=vertShader; stageInfos[1].module=fragShader;

    // Water is composed as an opaque surface. It must write depth so different
    // water faces occlude one another; disabling this created the stacked/flying
    // sheets seen when several chunk surfaces overlapped on screen.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    colorBlendAttachment.blendEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &waterPipeline));

    stageInfos[0].module = grassVertShader;
    colorBlendAttachment.blendEnable = VK_FALSE;
    bindingDesc.stride = sizeof(GrassInstance);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    attributeDescs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescs[0].offset = offsetof(GrassInstance, positionRotation);
    vertexInputInfo.vertexAttributeDescriptionCount = 1;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &grassPipeline));

    stageInfos[0].module = foliageVertShader;
    bindingDesc.stride = sizeof(FoliageInstance);
    attributeDescs[0].offset = offsetof(FoliageInstance, positionScale);
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &foliagePipeline));

    stageInfos[0].module = skyVertShader;
    stageInfos[1].module = skyFragShader;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    colorBlendAttachment.blendEnable = VK_FALSE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyPipeline));

    std::array<VkDescriptorSetLayoutBinding, 4> postBindings{};
    for (uint32_t index = 0; index < postBindings.size(); ++index) {
        postBindings[index].binding = index;
        postBindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        postBindings[index].descriptorCount = 1;
        postBindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo postLayoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    postLayoutInfo.bindingCount = static_cast<uint32_t>(postBindings.size());
    postLayoutInfo.pBindings = postBindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device, &postLayoutInfo, nullptr, &postDescriptorLayout));

    VkDescriptorPoolSize postPoolSize{};
    postPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postPoolSize.descriptorCount = 4;
    VkDescriptorPoolCreateInfo postPoolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    postPoolInfo.maxSets = 1;
    postPoolInfo.poolSizeCount = 1;
    postPoolInfo.pPoolSizes = &postPoolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &postPoolInfo, nullptr, &postDescriptorPool));

    VkDescriptorSetAllocateInfo postAllocInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    postAllocInfo.descriptorPool = postDescriptorPool;
    postAllocInfo.descriptorSetCount = 1;
    postAllocInfo.pSetLayouts = &postDescriptorLayout;
    VK_CHECK(vkAllocateDescriptorSets(device, &postAllocInfo, &postDescriptorSet));

    VkSamplerCreateInfo postSamplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    postSamplerInfo.magFilter = VK_FILTER_LINEAR;
    postSamplerInfo.minFilter = VK_FILTER_LINEAR;
    postSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    postSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    postSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    postSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    postSamplerInfo.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(device, &postSamplerInfo, nullptr, &postSampler));

    std::array<VkDescriptorImageInfo, 4> postImages{};
    postImages[0] = { postSampler, hdrImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    postImages[1] = { postSampler, depthImageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    postImages[2] = { minimapSampler, minimapImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    postImages[3] = { textureManager.textureSampler, textureManager.textureArrayImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    std::array<VkWriteDescriptorSet, 4> postWrites{};
    for (uint32_t index = 0; index < postWrites.size(); ++index) {
        postWrites[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        postWrites[index].dstSet = postDescriptorSet;
        postWrites[index].dstBinding = index;
        postWrites[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        postWrites[index].descriptorCount = 1;
        postWrites[index].pImageInfo = &postImages[index];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(postWrites.size()), postWrites.data(), 0, nullptr);

    VkPushConstantRange postPushRange{};
    postPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    postPushRange.size = sizeof(PostPushData);
    VkPipelineLayoutCreateInfo postPipelineLayoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    std::array<VkDescriptorSetLayout, 2> postSetLayouts{ postDescriptorLayout, gpuFeatureBinding.layout };
    postPipelineLayoutInfo.setLayoutCount = gpuFeaturesReady ? 2u : 1u;
    postPipelineLayoutInfo.pSetLayouts = postSetLayouts.data();
    postPipelineLayoutInfo.pushConstantRangeCount = 1;
    postPipelineLayoutInfo.pPushConstantRanges = &postPushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &postPipelineLayoutInfo, nullptr, &postPipelineLayout));

    stageInfos[0].module = postVertShader;
    stageInfos[1].module = postFragShader;
    renderingInfo.pColorAttachmentFormats = &swapchainImageFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    pipelineInfo.layout = postPipelineLayout;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &postPipeline));

    vkDestroyShaderModule(device, vertShader, nullptr);
    vkDestroyShaderModule(device, farSurfaceVertShader, nullptr);
    vkDestroyShaderModule(device, fragShader, nullptr);
    vkDestroyShaderModule(device, grassVertShader, nullptr);
    vkDestroyShaderModule(device, foliageVertShader, nullptr);
    vkDestroyShaderModule(device, skyVertShader, nullptr);
    vkDestroyShaderModule(device, skyFragShader, nullptr);
    vkDestroyShaderModule(device, postVertShader, nullptr);
    vkDestroyShaderModule(device, postFragShader, nullptr);
    vkDestroyShaderModule(device, shadowVertShader, nullptr);
    vkDestroyShaderModule(device, shadowFarSurfaceVertShader, nullptr);
    vkDestroyShaderModule(device, shadowFragShader, nullptr);
    vkDestroyShaderModule(device, shadowFoliageVertShader, nullptr);
    vkDestroyShaderModule(device, shadowGrassVertShader, nullptr);
}

void Engine::init_arm_mesh() {
    constexpr float skinLayer = static_cast<float>(TextureIndex::PlayerSkin);
    struct PartUV { float u, v, w, h, d; };

    auto addBox = [&](std::vector<VoxelVertex>& out, glm::vec3 mn, glm::vec3 mx, PartUV a, float marker) {
        auto rect = [&](float u, float v, float w, float h) {
            return std::array<glm::vec2,4>{glm::vec2(u/64, v/64), glm::vec2((u+w)/64,v/64),
                glm::vec2((u+w)/64,(v+h)/64), glm::vec2(u/64,(v+h)/64)};
        };
        auto face = [&](glm::vec3 p0,glm::vec3 p1,glm::vec3 p2,glm::vec3 p3,glm::vec3 n,std::array<glm::vec2,4> uv) {
            glm::vec4 c(1,1,1,marker);
            out.push_back({p0,n,c,{uv[0],skinLayer}}); out.push_back({p1,n,c,{uv[1],skinLayer}});
            out.push_back({p2,n,c,{uv[2],skinLayer}}); out.push_back({p0,n,c,{uv[0],skinLayer}});
            out.push_back({p2,n,c,{uv[2],skinLayer}}); out.push_back({p3,n,c,{uv[3],skinLayer}});
        };
        face({mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},{0,0,1},rect(a.u+a.d,a.v+a.d,a.w,a.h));
        face({mx.x,mn.y,mn.z},{mn.x,mn.y,mn.z},{mn.x,mx.y,mn.z},{mx.x,mx.y,mn.z},{0,0,-1},rect(a.u+a.d+a.w+a.d,a.v+a.d,a.w,a.h));
        face({mx.x,mn.y,mx.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mx.x,mx.y,mx.z},{1,0,0},rect(a.u+a.d+a.w,a.v+a.d,a.d,a.h));
        face({mn.x,mn.y,mn.z},{mn.x,mn.y,mx.z},{mn.x,mx.y,mx.z},{mn.x,mx.y,mn.z},{-1,0,0},rect(a.u,a.v+a.d,a.d,a.h));
        face({mn.x,mx.y,mx.z},{mx.x,mx.y,mx.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z},{0,1,0},rect(a.u+a.d,a.v,a.w,a.d));
        face({mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mn.y,mx.z},{mn.x,mn.y,mx.z},{0,-1,0},rect(a.u+a.d+a.w,a.v,a.w,a.d));
    };
    auto upload = [&](const std::vector<VoxelVertex>& vertices, AllocatedBuffer& buffer, uint32_t& count) {
        count = static_cast<uint32_t>(vertices.size()); VkDeviceSize bytes = vertices.size()*sizeof(VoxelVertex);
        VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=bytes; bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo ai{}; ai.usage=VMA_MEMORY_USAGE_CPU_TO_GPU;
        VK_CHECK(vmaCreateBuffer(allocator,&bi,&ai,&buffer.buffer,&buffer.allocation,nullptr));
        void* data=nullptr; vmaMapMemory(allocator,buffer.allocation,&data); memcpy(data,vertices.data(),bytes); vmaUnmapMemory(allocator,buffer.allocation);
    };

    std::vector<VoxelVertex> arm;
    addBox(arm, {-0.07f,-0.55f,-0.07f}, {0.07f,0.0f,0.07f}, {40,16,4,12,4}, 1.0f);
    addBox(arm, {-0.075f,-0.555f,-0.075f}, {0.075f,0.005f,0.075f}, {40,32,4,12,4}, 1.0f);
    upload(arm, armBuffer, armVertexCount);

    // Uma cópia texturizada de cada bloco selecionável. Todos compartilham o
    // mesmo buffer e a seleção troca apenas o firstVertex do draw.
    std::vector<VoxelVertex> heldBlocks;
    auto addHeldFace = [&](glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3,
                           glm::vec3 normal, BlockType type) {
        const glm::vec4 color = get_block_color(type, normal);
        const float layer = get_block_texture_layer(type, normal);
        const glm::vec3 uv0(0.0f, 0.0f, layer), uv1(1.0f, 0.0f, layer);
        const glm::vec3 uv2(1.0f, 1.0f, layer), uv3(0.0f, 1.0f, layer);
        heldBlocks.push_back({p0,normal,color,uv0}); heldBlocks.push_back({p1,normal,color,uv1});
        heldBlocks.push_back({p2,normal,color,uv2}); heldBlocks.push_back({p0,normal,color,uv0});
        heldBlocks.push_back({p2,normal,color,uv2}); heldBlocks.push_back({p3,normal,color,uv3});
    };
    auto addHeldBlock = [&](BlockType type) {
        constexpr float s = 0.15f;
        const glm::vec3 mn(-s), mx(s);
        addHeldFace({mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},{0,0,1},type);
        addHeldFace({mx.x,mn.y,mn.z},{mn.x,mn.y,mn.z},{mn.x,mx.y,mn.z},{mx.x,mx.y,mn.z},{0,0,-1},type);
        addHeldFace({mx.x,mn.y,mx.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mx.x,mx.y,mx.z},{1,0,0},type);
        addHeldFace({mn.x,mn.y,mn.z},{mn.x,mn.y,mx.z},{mn.x,mx.y,mx.z},{mn.x,mx.y,mn.z},{-1,0,0},type);
        addHeldFace({mn.x,mx.y,mx.z},{mx.x,mx.y,mx.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z},{0,1,0},type);
        addHeldFace({mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mn.y,mx.z},{mn.x,mn.y,mx.z},{0,-1,0},type);
    };
    for (uint32_t type = static_cast<uint32_t>(BlockType::Grass);
         type <= static_cast<uint32_t>(BlockType::Clay); ++type) {
        addHeldBlock(static_cast<BlockType>(type));
    }
    upload(heldBlocks, heldBlockBuffer, heldBlockVertexCount);

    std::vector<VoxelVertex> body;
    addBox(body, {-0.25f,1.50f,-0.25f},{0.25f,2.00f,0.25f},{0,0,8,8,8},0.95f);
    addBox(body, {-0.25f,0.75f,-0.125f},{0.25f,1.50f,0.125f},{16,16,8,12,4},1.0f);
    addBox(body, {-0.50f,0.75f,-0.125f},{-0.25f,1.50f,0.125f},{40,16,4,12,4},0.93f);
    addBox(body, {0.25f,0.75f,-0.125f},{0.50f,1.50f,0.125f},{32,48,4,12,4},0.94f);
    addBox(body, {-0.25f,0.0f,-0.125f},{0.0f,0.75f,0.125f},{0,16,4,12,4},0.91f);
    addBox(body, {0.0f,0.0f,-0.125f},{0.25f,0.75f,0.125f},{16,48,4,12,4},0.92f);
    // Segunda camada oficial do formato Minecraft 64x64.
    addBox(body, {-0.265f,1.485f,-0.265f},{0.265f,2.015f,0.265f},{32,0,8,8,8},0.95f);
    addBox(body, {-0.265f,0.735f,-0.140f},{0.265f,1.515f,0.140f},{16,32,8,12,4},1.0f);
    addBox(body, {-0.515f,0.735f,-0.140f},{-0.235f,1.515f,0.140f},{40,32,4,12,4},0.93f);
    addBox(body, {0.235f,0.735f,-0.140f},{0.515f,1.515f,0.140f},{48,48,4,12,4},0.94f);
    addBox(body, {-0.265f,-0.015f,-0.140f},{0.015f,0.765f,0.140f},{0,32,4,12,4},0.91f);
    addBox(body, {-0.015f,-0.015f,-0.140f},{0.265f,0.765f,0.140f},{0,48,4,12,4},0.92f);
    upload(body, characterBuffer, characterVertexCount);
}

void Engine::draw_arm(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj) {
    if (armVertexCount == 0 || armBuffer.buffer == VK_NULL_HANDLE) return;

    glm::mat4 invView = glm::inverse(view);

    float swingAngle = std::sin(player.swingProgress * 3.14159f) * 0.75f;
    const float walkBob = std::sin(player.walkCycle) * player.walkAmount;
    const float walkSway = std::cos(player.walkCycle * 0.5f) * player.walkAmount;
    glm::vec3 armOffset(0.38f + walkSway * 0.018f, -0.32f - swingAngle * 0.15f + std::abs(walkBob) * 0.018f,
                        -0.52f - swingAngle * 0.20f + walkBob * 0.025f);

    glm::mat4 armModel = invView * glm::translate(glm::mat4(1.0f), armOffset);
    armModel = glm::rotate(armModel, -0.2f + swingAngle * 0.9f, glm::vec3(1, 0, 0));
    armModel = glm::rotate(armModel, 0.25f, glm::vec3(0, 1, 0));
    armModel = glm::rotate(armModel, walkBob * 0.055f, glm::vec3(0, 0, 1));

    glm::mat4 armMVP = proj * view * armModel;

    PushData pushData;
    pushData.mvp = armMVP;
    pushData.cameraPos = glm::vec4(player.camera.position, 1.0f);
    // w carries the quality actually uploaded by the FAR clipmap. Shaders use
    // it for mip/detail retention, so target and visible result cannot diverge.
    pushData.sunDirection = glm::vec4(
        currentSunDirection,
        worldRenderer.applied_endpoint_percent() * 0.01f);
    pushData.sunColor = glm::vec4(currentLightColor, 1.0f);
    // Valor negativo identifica o modelo em primeira pessoa no shader. Ele está em
    // espaço da câmera e não pode usar distância, neblina e shadow map do mundo.
    pushData.environment = glm::vec4(worldVisualTime, currentDaylight, 0.0016f, -1.0f);

    vkCmdPushConstants(cmd, voxelPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushData), &pushData);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &armBuffer.buffer, &offset);
    vkCmdDraw(cmd, armVertexCount, 1, 0, 0);

    if (heldBlockBuffer.buffer != VK_NULL_HANDLE && player.selectedBlock != BlockType::Air) {
        glm::vec3 blockOffset(0.58f + walkSway * 0.020f,
                              -0.28f - swingAngle * 0.12f + std::abs(walkBob) * 0.016f,
                              -0.74f - swingAngle * 0.16f);
        glm::mat4 blockModel = invView * glm::translate(glm::mat4(1.0f), blockOffset);
        blockModel = glm::rotate(blockModel, glm::radians(18.0f) + swingAngle * 0.75f, glm::vec3(1,0,0));
        blockModel = glm::rotate(blockModel, glm::radians(-32.0f), glm::vec3(0,1,0));
        blockModel = glm::rotate(blockModel, glm::radians(8.0f) + walkBob * .05f, glm::vec3(0,0,1));
        pushData.mvp = proj * view * blockModel;
        vkCmdPushConstants(cmd, voxelPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushData), &pushData);
        vkCmdBindVertexBuffers(cmd, 0, 1, &heldBlockBuffer.buffer, &offset);
        constexpr uint32_t verticesPerBlock = 36;
        const uint32_t blockIndex = static_cast<uint32_t>(player.selectedBlock) - 1u;
        vkCmdDraw(cmd, verticesPerBlock, 1, blockIndex * verticesPerBlock, 0);
    }
}

void Engine::draw_character(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj) {
    if (characterVertexCount == 0 || characterBuffer.buffer == VK_NULL_HANDLE) return;
    glm::mat4 model = glm::translate(glm::mat4(1.0f), player.position);
    model = glm::rotate(model, glm::radians(90.0f - player.camera.yaw), glm::vec3(0,1,0));
    PushData data{};
    data.mvp = proj * view * model;
    data.cameraPos = glm::vec4(player.camera.position, 1.001f + player.walkAmount);
    data.sunDirection = glm::vec4(currentSunDirection, 0.0f);
    data.sunColor = glm::vec4(currentLightColor, 1.0f);
    data.environment = glm::vec4(worldVisualTime, currentDaylight, 0.0016f, currentExposure);
    vkCmdPushConstants(cmd, voxelPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushData), &data);
    VkDeviceSize offset=0; vkCmdBindVertexBuffers(cmd,0,1,&characterBuffer.buffer,&offset);
    vkCmdDraw(cmd,characterVertexCount,1,0,0);
}

void Engine::draw() {
    VK_CHECK(vkWaitForFences(device, 1, &get_current_frame().renderFence, VK_TRUE, 1000000000));

    uint32_t swapchainImageIndex;
    VkResult res = vkAcquireNextImageKHR(device, swapchain, 1000000000, get_current_frame().swapchainSemaphore, nullptr, &swapchainImageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(res);
    }
    const bool acquiredSuboptimal = res == VK_SUBOPTIMAL_KHR;
    // Reset only after an image was acquired. Returning with a reset fence
    // leaves the next frame waiting forever after a resize/fullscreen switch.
    VK_CHECK(vkResetFences(device, 1, &get_current_frame().renderFence));

    const bool lodQualityUpDown = glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS;
    const bool lodQualityDownDown = glfwGetKey(window, GLFW_KEY_9) == GLFW_PRESS;
    bool lodQualityChanged = false;
    const int lodQualityDirection = lodQualityUpDown == lodQualityDownDown
        ? 0 : (lodQualityUpDown ? 1 : -1);
    const int previousLodQualityDirection = lodQuality0WasPressed == lodQuality9WasPressed
        ? 0 : (lodQuality0WasPressed ? 1 : -1);
    const double lodInputNow = glfwGetTime();
    if (!isPaused && lodQualityDirection != 0) {
        if (lodQualityDirection != previousLodQualityDirection) {
            world.adjust_far_lod_quality(lodQualityDirection);
            lodQualityHeldSince = lodInputNow;
            lodQualityNextRepeat = lodInputNow + 0.32;
            lodQualityChanged = true;
        } else if (lodInputNow >= lodQualityNextRepeat) {
            const double heldSeconds = lodInputNow - lodQualityHeldSince;
            const int acceleratedSteps = heldSeconds >= 2.0 ? 5 : (heldSeconds >= 1.0 ? 2 : 1);
            world.adjust_far_lod_quality(lodQualityDirection, acceleratedSteps);
            lodQualityNextRepeat = lodInputNow + 0.05;
            lodQualityChanged = true;
        }
    } else if (lodQualityDirection == 0) {
        lodQualityHeldSince = 0.0;
        lodQualityNextRepeat = 0.0;
    }
    lodQuality0WasPressed = lodQualityUpDown;
    lodQuality9WasPressed = lodQualityDownDown;

    if (lodQualityChanged) {
        const std::string immediateTitle = std::format(
            "VulkanCraft | LOD end {:.6g}% | reconstruindo clipmap...",
            world.far_lod_endpoint_percent());
        glfwSetWindowTitle(window, immediateTitle.c_str());
    }

    if (!isPaused) {
        // 0/9 pertencem ao ajuste de LOD. Player::update ainda conhece essas
        // teclas como slots antigos; restaurar a selecao impede que o controle
        // de qualidade troque silenciosamente o bloco da mao.
        const BlockType selectedBlockBeforeLodInput = player.selectedBlock;
        PlayerInput input;
        input.forward = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
        input.backward = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        input.left = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
        input.right = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
        input.jump = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        input.descend = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        for (int slot = 0; slot < 8; ++slot) {
            if (glfwGetKey(window, GLFW_KEY_1 + slot) == GLFW_PRESS) {
                input.selectedBlock = static_cast<BlockType>(slot + 1);
            }
        }
        player.update(deltaTime, input, world, soundEngine);
        if (lodQualityUpDown || lodQualityDownDown) {
            player.selectedBlock = selectedBlockBeforeLodInput;
        }
        world.update(player.position, worldRenderer, deltaTime);
        if (mobEntities && mobBehavior) {
            std::string mobError;
            mobBehavior->tick(deltaTime,
                              { player.position.x, player.position.y,
                                player.position.z },
                              *mobEntities, mobQuery, mobError);
        }
        soundEngine.update_ambience(player.position, world, currentDaylight, player.isSubmerged, deltaTime);
    }

    worldVisualTime = static_cast<float>(glfwGetTime());
    const float dayAngle = std::fmod(worldVisualTime / 180.0f + 0.22f, 1.0f) * glm::two_pi<float>();
    currentSunDirection = glm::normalize(glm::vec3(std::cos(dayAngle), std::sin(dayAngle), 0.24f));
    auto smoothUnit = [](float value) {
        value = std::clamp(value, 0.0f, 1.0f);
        return value * value * (3.0f - 2.0f * value);
    };
    currentDaylight = smoothUnit((currentSunDirection.y + 0.10f) / 0.24f);
    const float horizonWarmth = (1.0f - smoothUnit(std::abs(currentSunDirection.y) / 0.42f)) * currentDaylight;
    const glm::vec3 daylightColor = glm::mix(glm::vec3(1.34f, 0.48f, 0.16f), glm::vec3(1.18f, 1.10f, 0.94f), 1.0f - horizonWarmth);
    currentLightColor = glm::mix(glm::vec3(0.12f, 0.18f, 0.34f), daylightColor, currentDaylight);
    currentExposure = glm::mix(1.32f, 0.86f, currentDaylight);
    refresh_gpu_features();

    // Advance the real renderer-facing feature providers before their GPU
    // state is uploaded. Their results are consumed by the passes below and
    // are also exposed to the debug snapshot, rather than remaining SDK-only.
    if (probeGrid) {
        std::string providerError;
        const auto sampler = [this](const glm::vec3& position, const glm::vec3& direction) {
            Engine::Rendering::ProbeCaptureSample sample;
            const glm::vec3 dir = glm::length(direction) > 1.0e-5f
                ? glm::normalize(direction) : glm::vec3(0.0f, 1.0f, 0.0f);
            const float sunTerm = std::max(glm::dot(dir, glm::normalize(currentSunDirection)), 0.0f);
            const float terrainTerm = std::clamp(0.015f + 0.0004f * std::max(position.y, 0.0f), 0.0f, 0.12f);
            sample.radiance = glm::vec3(0.012f, 0.020f, 0.040f) + terrainTerm +
                               sunTerm * currentLightColor * 0.16f;
            const RuntimeBlockId cell = world.get_block_at(position + dir * 0.5f);
            sample.backface = world.is_solid_block_id(cell) && glm::dot(dir, glm::vec3(0.0f, 1.0f, 0.0f)) < -0.25f;
            return sample;
        };
        probeGrid->update(player.camera.position, sampler, 0, &providerError);
    }
    if (fluidSimulation) {
        // Keep the generic fluid provider on the same fixed simulation cadence
        // as the world; this state is the source for future stream renderables.
        static auto fluidState = fluidSimulation->createState();
        fluidSimulation->simulate(fluidState, fluidSimulation->getConfig());
    }
    if (renderingDebugView) {
        std::vector<Engine::Rendering::DebugProbe> probes;
        if (probeGrid) {
            const std::uint32_t count = std::min<std::uint32_t>(probeGrid->probe_count(), 256u);
            probes.reserve(count);
            for (std::uint32_t slot = 0; slot < count; ++slot) {
                Engine::Rendering::ProbeGridProbe probe{};
                if (!probeGrid->probe(slot, probe)) continue;
                Engine::Rendering::DebugProbe debug{};
                debug.radianceVisibility = glm::vec4(probe.irradiance, probe.age > 0 ? 1.0f : 0.0f);
                debug.worldCellCascade = glm::ivec4(probe.cell, static_cast<int>(probe.slot));
                probes.push_back(debug);
            }
            renderingDebugView->bind_probes(probes, 0, 0);
        }
        renderingDebugView->bind_capture(radianceCache.total_probe_count() -
                                              std::min(radianceCache.total_probe_count(), radianceCache.pending_probe_count()),
                                          radianceCache.pending_probe_count(),
                                          static_cast<std::uint64_t>(radianceCache.total_probe_count()) * sizeof(RadianceCache::ProbeGpu));
        renderingDebugView->bind_disocclusion(0, temporalDenoiser ? 1u : 0u);
        renderingDebugView->refresh();
    }
    if (radianceCacheReady) {
        radianceCache.update(player.camera.position, currentSunDirection,
                             currentLightColor);
    }
    refresh_gpu_features();
    // Advance the renderer-owned upload/retirement epoch before recording this
    // frame. Simulation publishes completed chunk snapshots through this same
    // real Vulkan renderer seam.
    worldRenderer.begin_frame();

    VkCommandBuffer cmd = get_current_frame().mainCommandBuffer;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    VkCommandBufferBeginInfo cmdBeginInfo = {};
    cmdBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    // Make the current probe clipmap visible to the real fragment passes. This
    // is intentionally recorded on the same command buffer as the world draw,
    // not in a headless/test-only path.
    if (radianceCacheReady) radianceCache.record_uploads(cmd);
    if (gpuFeaturePasses.initialized) {
        VkBufferMemoryBarrier2 featureUpload{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        featureUpload.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        featureUpload.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        featureUpload.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        featureUpload.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        featureUpload.buffer = gpuFeaturePasses.featureBuffer;
        featureUpload.size = sizeof(GpuRenderFeatures);
        VkDependencyInfo uploadDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        uploadDependency.bufferMemoryBarrierCount = 1;
        uploadDependency.pBufferMemoryBarriers = &featureUpload;
        vkCmdPipelineBarrier2(cmd, &uploadDependency);
        Engine::Rendering::record_gpu_feature_passes(cmd, gpuFeaturePasses, hdrImage,
                                                       hdrImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                       swapchainExtent, gpuFeatures);
        VkMemoryBarrier2 featureBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        featureBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        featureBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        featureBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        featureBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo featureDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        featureDependency.memoryBarrierCount = 1;
        featureDependency.pMemoryBarriers = &featureBarrier;
        vkCmdPipelineBarrier2(cmd, &featureDependency);
    }

    VkImageMemoryBarrier2 shadowWrite{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    shadowWrite.srcStageMask=frameNumber==0?VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    shadowWrite.srcAccessMask=frameNumber==0?0:VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    shadowWrite.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; shadowWrite.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    shadowWrite.oldLayout=frameNumber==0?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    shadowWrite.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; shadowWrite.image=shadowImage;
    shadowWrite.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; shadowWrite.subresourceRange.levelCount=1; shadowWrite.subresourceRange.layerCount=1;
    VkDependencyInfo shadowDep{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; shadowDep.imageMemoryBarrierCount=1; shadowDep.pImageMemoryBarriers=&shadowWrite;
    vkCmdPipelineBarrier2(cmd,&shadowDep);

    VkImageMemoryBarrier2 mapBarriers[2]{};
    mapBarriers[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    mapBarriers[0].srcStageMask=frameNumber==0?VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mapBarriers[0].srcAccessMask=frameNumber==0?0:VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    mapBarriers[0].dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; mapBarriers[0].dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    mapBarriers[0].oldLayout=frameNumber==0?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mapBarriers[0].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; mapBarriers[0].image=minimapImage;
    mapBarriers[0].subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; mapBarriers[0].subresourceRange.levelCount=1; mapBarriers[0].subresourceRange.layerCount=1;
    mapBarriers[1]=mapBarriers[0]; mapBarriers[1].srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; mapBarriers[1].srcAccessMask=0;
    mapBarriers[1].dstStageMask=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT; mapBarriers[1].dstAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mapBarriers[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; mapBarriers[1].newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    mapBarriers[1].image=minimapDepthImage; mapBarriers[1].subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    VkDependencyInfo mapDep{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; mapDep.imageMemoryBarrierCount=2; mapDep.pImageMemoryBarriers=mapBarriers;
    vkCmdPipelineBarrier2(cmd,&mapDep);
    VkRenderingAttachmentInfo mapColor{.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; mapColor.imageView=minimapImageView;
    mapColor.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; mapColor.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; mapColor.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    mapColor.clearValue.color={{0.12f,0.30f,0.48f,1.0f}};
    VkRenderingAttachmentInfo mapDepth{.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; mapDepth.imageView=minimapDepthView;
    mapDepth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; mapDepth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; mapDepth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    mapDepth.clearValue.depthStencil={1,0};
    VkRenderingInfo mapInfo{.sType=VK_STRUCTURE_TYPE_RENDERING_INFO}; mapInfo.renderArea.extent={384,384}; mapInfo.layerCount=1;
    mapInfo.colorAttachmentCount=1; mapInfo.pColorAttachments=&mapColor; mapInfo.pDepthAttachment=&mapDepth;
    vkCmdBeginRendering(cmd,&mapInfo); VkViewport mapViewport{0,0,384,384,0,1}; VkRect2D mapScissor{{0,0},{384,384}};
    vkCmdSetViewport(cmd,0,1,&mapViewport); vkCmdSetScissor(cmd,0,1,&mapScissor);
    glm::vec3 mapEye=player.position+glm::vec3(0,150,0.01f); glm::mat4 mapView=glm::lookAt(mapEye,player.position,glm::vec3(0,0,-1));
    glm::mat4 mapProj=glm::ortho(-58.0f,58.0f,-58.0f,58.0f,0.1f,260.0f); mapProj[1][1]*=-1;
    PushData mapPush{}; mapPush.mvp=mapProj*mapView; mapPush.cameraPos=glm::vec4(player.camera.position,1);
    mapPush.sunDirection=glm::vec4(currentSunDirection,0);
    mapPush.sunColor=glm::vec4(currentLightColor,static_cast<float>(world.stableVisibleRadius));
    mapPush.environment=glm::vec4(worldVisualTime,currentDaylight,0,currentExposure);
    Frustum mapFrustum; mapFrustum.update(mapPush.mvp); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,voxelPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,voxelPipelineLayout,0,1,&textureManager.descriptorSet,0,nullptr);
    vkCmdPushConstants(cmd,voxelPipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PushData),&mapPush); worldRenderer.draw_details(cmd,mapFrustum);
    vkCmdEndRendering(cmd);
    mapBarriers[0].srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; mapBarriers[0].srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    mapBarriers[0].dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; mapBarriers[0].dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    mapBarriers[0].oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; mapBarriers[0].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mapDep.imageMemoryBarrierCount=1; vkCmdPipelineBarrier2(cmd,&mapDep);
    shadowWrite.srcStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; shadowWrite.srcAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    shadowWrite.dstStageMask=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT; shadowWrite.dstAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadowWrite.oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; shadowWrite.newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier2(cmd,&shadowDep);
    VkRenderingAttachmentInfo shadowDepth{.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; shadowDepth.imageView=shadowImageView;
    shadowDepth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; shadowDepth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; shadowDepth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    shadowDepth.clearValue.depthStencil={1.0f,0};
    VkRenderingInfo shadowInfo{.sType=VK_STRUCTURE_TYPE_RENDERING_INFO}; shadowInfo.renderArea.extent={2048,2048}; shadowInfo.layerCount=1; shadowInfo.pDepthAttachment=&shadowDepth;
    vkCmdBeginRendering(cmd,&shadowInfo);
    VkViewport shadowViewport{0,0,2048,2048,0,1}; VkRect2D shadowScissor{{0,0},{2048,2048}};
    vkCmdSetViewport(cmd,0,1,&shadowViewport); vkCmdSetScissor(cmd,0,1,&shadowScissor); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,voxelPipelineLayout,0,1,&textureManager.descriptorSet,0,nullptr);
    PushData shadowPush{}; shadowPush.mvp=glm::mat4(1.0f); shadowPush.cameraPos=glm::vec4(player.camera.position,1);
    shadowPush.sunDirection=glm::vec4(currentSunDirection,0);
    shadowPush.sunColor=glm::vec4(currentLightColor,static_cast<float>(world.stableVisibleRadius));
    shadowPush.environment=glm::vec4(worldVisualTime,currentDaylight,0,0);
    vkCmdPushConstants(cmd,voxelPipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PushData),&shadowPush);
    // Shadow LOD: o mapa deformado concentra resolução perto da câmera e ainda
    // recebe a geometria sólida/arbórea de todo o horizonte carregado.
    worldRenderer.draw_shadow(cmd, player.camera.position, 32);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowFarSurfacePipeline);
    worldRenderer.draw_far_surface_shadow(cmd);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline);
    // FAR tree cards share the VoxelVertex contract of shadowPipeline. Their
    // precomputed draw ranges cover the useful shadow-map footprint only.
    worldRenderer.draw_far_shadow(cmd);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowFoliagePipeline);
    worldRenderer.draw_foliage_shadow(cmd,player.camera.position,32);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowGrassPipeline);
    worldRenderer.draw_grass_shadow(cmd,player.camera.position,18);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shadowPipeline);
    { shadowPush.mvp=glm::translate(glm::mat4(1),player.position); shadowPush.mvp=glm::rotate(shadowPush.mvp,glm::radians(90.0f-player.camera.yaw),glm::vec3(0,1,0));
        vkCmdPushConstants(cmd,voxelPipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PushData),&shadowPush);
        VkDeviceSize o=0; vkCmdBindVertexBuffers(cmd,0,1,&characterBuffer.buffer,&o); vkCmdDraw(cmd,characterVertexCount,1,0,0); }
    vkCmdEndRendering(cmd);
    shadowWrite.srcStageMask=VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT; shadowWrite.srcAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadowWrite.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; shadowWrite.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    shadowWrite.oldLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; shadowWrite.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd,&shadowDep);

    VkImageMemoryBarrier2 colorBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    colorBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    colorBarrier.srcAccessMask = 0;
    colorBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    colorBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    colorBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorBarrier.image = swapchainImages[swapchainImageIndex];
    colorBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorBarrier.subresourceRange.levelCount = 1;
    colorBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier2 hdrBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    hdrBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    hdrBarrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    hdrBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    hdrBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    hdrBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hdrBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrBarrier.image = hdrImage;
    hdrBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hdrBarrier.subresourceRange.levelCount = 1;
    hdrBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier2 depthBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    depthBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    depthBarrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    depthBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthBarrier.image = depthImage;
    depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthBarrier.subresourceRange.levelCount = 1;
    depthBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier2 barriers[3] = { colorBarrier, hdrBarrier, depthBarrier };
    VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 3;
    depInfo.pImageMemoryBarriers = barriers;


    vkCmdPipelineBarrier2(cmd, &depInfo);

    VkRenderingAttachmentInfo colorAttachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = hdrImageView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = player.isSubmerged
        ? VkClearColorValue{ { 0.015f, 0.16f, 0.22f, 1.0f } }
        : VkClearColorValue{ { 0.45f, 0.65f, 0.95f, 1.0f } };

    VkRenderingAttachmentInfo depthAttachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depthImageView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo renderingInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = { { 0, 0 }, swapchainExtent };
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(swapchainExtent.width), static_cast<float>(swapchainExtent.height), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{ { 0, 0 }, swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    float aspect = static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
    glm::vec3 renderCamera = player.camera.position;
    if (thirdPerson) {
        // ShoulderSurfing & Leawind 3rd Person: Ombro ajustado (Over-The-Shoulder), inércia suave e anti-clip contra blocos
        const glm::vec3 pivot = player.position + glm::vec3(0.0f, 1.45f, 0.0f);
        const float targetDist = 3.8f;
        const glm::vec3 shoulderOffset = player.camera.right * 0.72f + player.camera.up * 0.35f;
        const glm::vec3 desiredCamPos = pivot + shoulderOffset - player.camera.front * targetDist;

        // Anti-Clipping Terrain Raycast: previne câmera de atravessar blocos sólidos
        float actualDist = targetDist;
        const glm::vec3 rayDir = glm::normalize(desiredCamPos - pivot);
        for (float r = 0.2f; r <= targetDist; r += 0.15f) {
            glm::vec3 samplePos = pivot + rayDir * r;
            // A.2: registry-driven — solid OR non-water fluid stops the camera
            // (same rule as the old as_builtin_block != Air/Water, but dynamic
            // blocks now count too). Water id is the builtin Water (=12).
            const RuntimeBlockId sampleId = world.get_block_at(samplePos);
            const RuntimeBlockId waterId =
                static_cast<RuntimeBlockId>(BlockType::Water);
            if (world.is_solid_block_id(sampleId) ||
                (world.is_fluid_runtime_id(sampleId) && sampleId != waterId)) {
                actualDist = std::max(0.4f, r - 0.25f);
                break;
            }
        }
        renderCamera = pivot + rayDir * actualDist;
    }
    glm::mat4 view = glm::lookAt(renderCamera, player.camera.position + player.camera.front * 1.5f, player.camera.up);
    // The setting is a reach in chunks.  Include the square clipmap corner in
    // the frustum instead of silently clipping everything at the old 3500 m.
    const float farPlane = std::max(3500.0f,
        static_cast<float>(world.chunkBudget * CHUNK_SIZE_X) * 1.48f);
    glm::mat4 proj = player.camera.get_projection_matrix(aspect, farPlane);
    glm::mat4 mvp = proj * view;

    Frustum frustum;
    frustum.update(mvp);

    PushData pushData;
    pushData.mvp = mvp;
    pushData.cameraPos = glm::vec4(player.camera.position, player.isSubmerged ? -1.0f : 1.0f);
    pushData.sunDirection = glm::vec4(currentSunDirection, 0.0f);
    pushData.sunColor = glm::vec4(currentLightColor, static_cast<float>(world.stableVisibleRadius));
    const float worldFogDensity = std::min(0.0016f, 1.5f / farPlane);
    pushData.environment = glm::vec4(worldVisualTime, currentDaylight, worldFogDensity, currentExposure);

    // Push constants are command-buffer state, not pipeline-owned state. Entity
    // rendering replaces mvp with viewProj*model, so every following world pass
    // must explicitly restore this immutable per-frame world transform.
    const auto bindWorldPushConstants = [&]() {
        vkCmdPushConstants(cmd, voxelPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushData), &pushData);
    };
    const auto beginWorldPass = [&](VkPipeline pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, voxelPipelineLayout,
                                0, 1, &textureManager.descriptorSet, 0, nullptr);
        bindWorldPushConstants();
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline);
    bindWorldPushConstants();
    vkCmdDraw(cmd, 3, 1, 0, 0);

    beginWorldPass(farSurfacePipeline);
    worldRenderer.draw_far_surface(cmd);

    beginWorldPass(voxelPipeline);
    worldRenderer.draw(cmd, frustum);
    mobRenderer.draw(*mobEntities, cmd, voxelPipelineLayout, mvp, frustum,
                                     player.camera.position, currentSunDirection,
                                     currentLightColor, pushData.environment);

    // draw_mobs publishes a model-space MVP for each articulated limb. Every
    // pass below starts from the complete world-state contract again.
    beginWorldPass(grassPipeline);
    worldRenderer.draw_grass(cmd, frustum);

    beginWorldPass(foliagePipeline);
    worldRenderer.draw_foliage(cmd, frustum);

    // Bliss-style translucent stage: preserve the fully lit opaque scene and its
    // depth before drawing water. These copies drive refraction, absorption and SSR.
    vkCmdEndRendering(cmd);

    std::array<VkImageMemoryBarrier2, 4> waterCopyBarriers{};
    auto prepareCopy = [&](VkImageMemoryBarrier2& barrier, VkImage image,
                           VkImageAspectFlags aspect, VkImageLayout oldLayout,
                           VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                           VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
    };
    prepareCopy(waterCopyBarriers[0], hdrImage, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    prepareCopy(waterCopyBarriers[1], depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    prepareCopy(waterCopyBarriers[2], opaqueSceneImage, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    prepareCopy(waterCopyBarriers[3], opaqueDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkDependencyInfo waterCopyDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    waterCopyDependency.imageMemoryBarrierCount = static_cast<uint32_t>(waterCopyBarriers.size());
    waterCopyDependency.pImageMemoryBarriers = waterCopyBarriers.data();
    vkCmdPipelineBarrier2(cmd, &waterCopyDependency);

    VkImageCopy colorCopy{};
    colorCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    colorCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    colorCopy.extent = {swapchainExtent.width, swapchainExtent.height, 1};
    vkCmdCopyImage(cmd, hdrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   opaqueSceneImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorCopy);
    VkImageCopy depthCopy{};
    depthCopy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depthCopy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depthCopy.extent = {swapchainExtent.width, swapchainExtent.height, 1};
    vkCmdCopyImage(cmd, depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   opaqueDepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);

    std::array<VkImageMemoryBarrier2, 4> waterReadBarriers{};
    auto finishCopy = [&](VkImageMemoryBarrier2& barrier, VkImage image, VkImageAspectFlags aspect,
                          VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
    };
    finishCopy(waterReadBarriers[0], hdrImage, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    finishCopy(waterReadBarriers[1], depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    finishCopy(waterReadBarriers[2], opaqueSceneImage, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,

               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    finishCopy(waterReadBarriers[3], opaqueDepthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkDependencyInfo waterReadDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    waterReadDependency.imageMemoryBarrierCount = static_cast<uint32_t>(waterReadBarriers.size());
    waterReadDependency.pImageMemoryBarriers = waterReadBarriers.data();
    vkCmdPipelineBarrier2(cmd, &waterReadDependency);

    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    vkCmdBeginRendering(cmd, &renderingInfo);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Keep the translucent pass independent from every earlier draw, including
    // entities and future feature passes inserted between the scene copies.
    beginWorldPass(waterPipeline);
    worldRenderer.draw_water(cmd, frustum, player.camera.position);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, voxelPipeline);
    if (thirdPerson) draw_character(cmd, view, proj);
    else draw_arm(cmd, view, proj);

    vkCmdEndRendering(cmd);

    std::array<VkImageMemoryBarrier2, 2> sceneReadBarriers{};
    sceneReadBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    sceneReadBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    sceneReadBarriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    sceneReadBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    sceneReadBarriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    sceneReadBarriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sceneReadBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneReadBarriers[0].image = hdrImage;
    sceneReadBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sceneReadBarriers[0].subresourceRange.levelCount = 1;
    sceneReadBarriers[0].subresourceRange.layerCount = 1;

    sceneReadBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    sceneReadBarriers[1].srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    sceneReadBarriers[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    sceneReadBarriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    sceneReadBarriers[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    sceneReadBarriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    sceneReadBarriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    sceneReadBarriers[1].image = depthImage;
    sceneReadBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    sceneReadBarriers[1].subresourceRange.levelCount = 1;
    sceneReadBarriers[1].subresourceRange.layerCount = 1;

    VkDependencyInfo sceneReadDependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    sceneReadDependency.imageMemoryBarrierCount = static_cast<uint32_t>(sceneReadBarriers.size());
    sceneReadDependency.pImageMemoryBarriers = sceneReadBarriers.data();
    vkCmdPipelineBarrier2(cmd, &sceneReadDependency);

    VkRenderingAttachmentInfo postColorAttachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    postColorAttachment.imageView = swapchainImageViews[swapchainImageIndex];
    postColorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    postColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    postColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    postColorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkRenderingInfo postRenderingInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
    postRenderingInfo.renderArea = { { 0, 0 }, swapchainExtent };
    postRenderingInfo.layerCount = 1;
    postRenderingInfo.colorAttachmentCount = 1;
    postRenderingInfo.pColorAttachments = &postColorAttachment;
    vkCmdBeginRendering(cmd, &postRenderingInfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postDescriptorSet, 0, nullptr);
    if (gpuFeaturesReady)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 1, 1, &gpuFeatureBinding.set, 0, nullptr);

    const bool moonIsActive = currentDaylight <= 0.03f;
    const glm::vec3 activeCelestialDirection = moonIsActive ? -currentSunDirection : currentSunDirection;
    glm::vec4 sunClip = mvp * glm::vec4(player.camera.position + activeCelestialDirection * 1000.0f, 1.0f);
    glm::vec2 sunUV = glm::vec2(-2.0f);
    float sunVisibility = 0.0f;
    if (sunClip.w > 0.0f) {
        sunUV = glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f;
        sunVisibility = moonIsActive ? -(1.0f - currentDaylight) * 0.48f : currentDaylight;
    }
    PostPushData postPush{};
    postPush.sunScreen = glm::vec4(sunUV, sunVisibility, currentExposure);
    postPush.frame = glm::vec4(static_cast<float>(swapchainExtent.width), static_cast<float>(swapchainExtent.height),
                               worldVisualTime, player.isSubmerged ? 1.0f : 0.0f);
    double cursorX = 0.0, cursorY = 0.0;
    int menuWidth = 1, menuHeight = 1;
    glfwGetCursorPos(window, &cursorX, &cursorY);
    glfwGetWindowSize(window, &menuWidth, &menuHeight);
    const float pauseState = isPaused ? (showGraphicsMenu ? 2.0f : 1.0f) : 0.0f;
    const float featureFlags = (cinematicEffects ? 1.0f : 0.0f) + (depthOfFieldEnabled ? 2.0f : 0.0f);
    postPush.ui = glm::vec4(static_cast<float>(cursorX / (std::max)(menuWidth, 1)),
                            1.0f - static_cast<float>(cursorY / (std::max)(menuHeight, 1)),
                            pauseState, featureFlags);
    int selectedSlot = 1;
    switch (player.selectedBlock) {
        case BlockType::Grass: selectedSlot = 0; break; case BlockType::Dirt: selectedSlot = 1; break;
        case BlockType::Stone: selectedSlot = 2; break; case BlockType::Sand: selectedSlot = 3; break;
        case BlockType::Wood: selectedSlot = 4; break; case BlockType::Leaves: selectedSlot = 5; break;
        case BlockType::Planks: selectedSlot = 6; break; case BlockType::Cobblestone: selectedSlot = 7; break;
        case BlockType::Glass: selectedSlot = 8; break; default: selectedSlot = 1; break;
    }
    postPush.hud = glm::vec4(float(selectedSlot), std::sin(player.walkCycle), player.walkAmount,
                             player.camera.position.y);
    glm::vec3 cameraDelta = player.camera.front - previousCameraFront;
    postPush.cameraMotion = glm::vec4(glm::dot(cameraDelta, player.camera.right),
                                      glm::dot(cameraDelta, player.camera.up),
                                      player.camera.position.x, player.camera.position.z);
    postPush.settings = glm::vec4(static_cast<float>(world.chunkBudget), 0.0f, 0.0f, 0.0f);
    previousCameraFront = player.camera.front;
    vkCmdPushConstants(cmd, postPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPushData), &postPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 presentBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    presentBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    presentBarrier.dstAccessMask = 0;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.image = swapchainImages[swapchainImageIndex];
    presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.layerCount = 1;

    VkDependencyInfo presentDepInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    presentDepInfo.imageMemoryBarrierCount = 1;
    presentDepInfo.pImageMemoryBarriers = &presentBarrier;

    vkCmdPipelineBarrier2(cmd, &presentDepInfo);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdSubmit{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmit.commandBuffer = cmd;

    VkSemaphoreSubmitInfo waitSemaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSemaphore.semaphore = get_current_frame().swapchainSemaphore;
    waitSemaphore.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSemaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalSemaphore.semaphore = renderSemaphores[swapchainImageIndex];
    signalSemaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitSemaphore;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalSemaphore;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdSubmit;

    VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submit, get_current_frame().renderFence));


    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.swapchainCount = 1;
    presentInfo.pWaitSemaphores = &renderSemaphores[swapchainImageIndex];
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pImageIndices = &swapchainImageIndex;

    const VkResult presentResult = vkQueuePresentKHR(graphicsQueue, &presentInfo);
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR &&
        presentResult != VK_ERROR_OUT_OF_DATE_KHR) {
        VK_CHECK(presentResult);
    }

    frameNumber++;
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        acquiredSuboptimal || framebufferResized) {
        recreate_swapchain();
    }
}

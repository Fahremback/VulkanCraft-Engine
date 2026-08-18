#include "EditorApplication.hpp"
#include "frontend/FontAwesomeV6.h"
#include "frontend/IconsFontAwesome6.h"
#include "frontend/liberation_sans.h"
#include "frontend/ForgeTheme.hpp"
#include "frontend/ForgeWidgets.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include "../engine/animation/AnimationAssets.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "../engine/audio/OggDecoder.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include <array>
#include <random>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>
#include <chrono>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <VkBootstrap.h>
#include <miniaudio.h>
#include <thread>

namespace {
glm::mat4 model_from_transform(const Engine::TransformComponent& t) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}
void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp, const glm::vec4& color) {
    const Engine::ScenePushConstants pc{ mvp, color };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}
void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}
void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color);
}
} // namespace

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wincodec.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Engine {

// ===========================================================================
// Vulkan Infrastructure (split from EditorApplication.cpp)
// ===========================================================================
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

void EditorApplication::create_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                     VkMemoryPropertyFlags memProps, VkImage& image, VkDeviceMemory& memory,
                                     uint32_t mipLevels /* = 1 */,
                                     VkSampleCountFlagBits samples /* = VK_SAMPLE_COUNT_1_BIT */) {
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = { w, h, 1 };
    info.mipLevels = std::max(mipLevels, 1u);
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = samples;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(m_device, &info, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image");
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(m_device, image, &requirements);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, memProps);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate image memory");
    }
    vkBindImageMemory(m_device, image, memory, 0);
}

VkImageView EditorApplication::create_image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                                 uint32_t mipLevels /* = 1 */) {
    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = { aspect, 0, std::max(mipLevels, 1u), 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_device, &info, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create image view");
    }
    return view;
}

void EditorApplication::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                      VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &info, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, props);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }
    vkBindBufferMemory(m_device, buffer, memory, 0);
}

void EditorApplication::destroy_buffer(GPUBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = GPUBuffer{};
}

void EditorApplication::transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                                VkImageAspectFlags aspect, uint32_t baseMipLevel /* = 0 */,
                                                uint32_t levelCount /* = 1 */) {
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, baseMipLevel, std::max(levelCount, 1u), 0, 1 };
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkCommandBuffer EditorApplication::begin_single_time_commands() {
    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void EditorApplication::end_single_time_commands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
}

// ===========================================================================
// Viewport initialization
// ===========================================================================

void EditorApplication::init_offscreen_target() {
    // Render passes and sampler are size-independent and referenced by the
    // pipelines, so they are created once and kept until final cleanup.
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // Clamp MSAA to what the device actually supports (4x on virtually all
    // desktop GPUs; falls back to 2x/1x on weak/software adapters).
    if (m_physicalDevice != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts;
        m_viewportSamples =
            (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT :
            (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT :
            VK_SAMPLE_COUNT_1_BIT;
    }

    // Viewport scene render pass with MSAA + resolve:
    //   0 = scene color (multisampled, transient)
    //   1 = scene depth (multisampled, transient)
    //   2 = resolve color (1x, stored, sampled by ImGui)
    VkAttachmentDescription colorMsaa{};
    colorMsaa.format = colorFormat;
    colorMsaa.samples = m_viewportSamples;
    colorMsaa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorMsaa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorMsaa.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorMsaa.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorMsaa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorMsaa.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthMsaa{};
    depthMsaa.format = depthFormat;
    depthMsaa.samples = m_viewportSamples;
    depthMsaa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthMsaa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthMsaa.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthMsaa.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthMsaa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthMsaa.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription resolveColor{};
    resolveColor.format = colorFormat;
    resolveColor.samples = VK_SAMPLE_COUNT_1_BIT;
    resolveColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolveColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolveColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolveColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolveColor.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    resolveColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resolveRef{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = &resolveRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkAttachmentDescription attachments[3] = { colorMsaa, depthMsaa, resolveColor };
    VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 3;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_offscreen.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create MSAA offscreen render pass");
    }

    // Pick render pass: color-ID pass stays 1x with its OWN 1x depth (the
    // scene depth is now multisampled and cannot be shared with a 1x pass).
    VkAttachmentDescription pickColor{};
    pickColor.format = colorFormat;
    pickColor.samples = VK_SAMPLE_COUNT_1_BIT;
    pickColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pickColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    pickColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pickColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickColor.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pickColor.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentDescription pickDepth{};
    pickDepth.format = depthFormat;
    pickDepth.samples = VK_SAMPLE_COUNT_1_BIT;
    pickDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pickDepth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    pickDepth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    pickDepth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pickDepth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference pickColorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference pickDepthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription pickSubpass{};
    pickSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pickSubpass.colorAttachmentCount = 1;
    pickSubpass.pColorAttachments = &pickColorRef;
    pickSubpass.pDepthStencilAttachment = &pickDepthRef;
    VkAttachmentDescription pickAttachments[2] = { pickColor, pickDepth };
    VkRenderPassCreateInfo pickRpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    pickRpInfo.attachmentCount = 2;
    pickRpInfo.pAttachments = pickAttachments;
    pickRpInfo.subpassCount = 1;
    pickRpInfo.pSubpasses = &pickSubpass;
    if (vkCreateRenderPass(m_device, &pickRpInfo, nullptr, &m_offscreen.pickRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pick render pass");
    }

    // Material-texture sampler: trilinear mipmapping + 16x anisotropic
    // filtering so surfaces seen at oblique angles (e.g. ~75°) stop shimmering
    // and aliasing. The device enables samplerAnisotropy at creation
    // (Engine.cpp), so this is safe on every supported adapter.
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_offscreen.sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen sampler");
    }

    create_offscreen_buffers(800, 600);
    create_shadow_map();
    init_thumbnail_target();
    init_block_cube();
}

// Creates the size-dependent resources (images, views, framebuffers, staging).
// Render passes and the sampler are kept across resizes.
void EditorApplication::create_offscreen_buffers(uint32_t w, uint32_t h) {
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // Resolve target (1x) — what ImGui samples.
    create_image(w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.colorImage, m_offscreen.colorMemory);
    m_offscreen.colorView = create_image_view(m_offscreen.colorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    // Scene color, multisampled.
    create_image(w, h, colorFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.msaaColorImage, m_offscreen.msaaColorMemory,
                 1, m_viewportSamples);
    m_offscreen.msaaColorView =
        create_image_view(m_offscreen.msaaColorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    // Scene depth, multisampled.
    create_image(w, h, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.depthImage, m_offscreen.depthMemory,
                 1, m_viewportSamples);
    m_offscreen.depthView = create_image_view(m_offscreen.depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Pick buffers stay 1x, with their own 1x depth.
    create_image(w, h, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_offscreen.pickImage, m_offscreen.pickMemory);
    m_offscreen.pickView = create_image_view(m_offscreen.pickImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    create_image(w, h, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_offscreen.pickDepthImage, m_offscreen.pickDepthMemory);
    m_offscreen.pickDepthView =
        create_image_view(m_offscreen.pickDepthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(w) * h * 4;
    create_buffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_offscreen.pickStagingBuffer, m_offscreen.pickStagingMemory);

    // Bring freshly created images into the layouts their render passes expect.
    {
        VkCommandBuffer transitionCmd = begin_single_time_commands();
        transition_image_layout(transitionCmd, m_offscreen.colorImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition_image_layout(transitionCmd, m_offscreen.pickImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition_image_layout(transitionCmd, m_offscreen.pickDepthImage,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        end_single_time_commands(transitionCmd);
    }

    // Scene framebuffer: [MSAA color, MSAA depth, resolve].
    VkImageView attachments[3] = { m_offscreen.msaaColorView, m_offscreen.depthView, m_offscreen.colorView };
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_offscreen.renderPass;
    fbInfo.attachmentCount = 3;
    fbInfo.pAttachments = attachments;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_offscreen.framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen framebuffer");
    }

    // Pick framebuffer: [pick color, pick depth] — both 1x.
    VkImageView pickAttachments[2] = { m_offscreen.pickView, m_offscreen.pickDepthView };
    VkFramebufferCreateInfo pickFbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    pickFbInfo.renderPass = m_offscreen.pickRenderPass;
    pickFbInfo.attachmentCount = 2;
    pickFbInfo.pAttachments = pickAttachments;
    pickFbInfo.width = w;
    pickFbInfo.height = h;
    pickFbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &pickFbInfo, nullptr, &m_offscreen.pickFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pick framebuffer");
    }

    m_offscreen.width = w;
    m_offscreen.height = h;
    m_offscreen.imguiTextureID = ImGui_ImplVulkan_AddTexture(
        m_offscreen.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Sun shadow map: fixed-size depth-only target, rebuilt once at startup. The
// viewport records a shadow pass before the scene pass and the material
// pipelines sample this map through the comparison sampler.
void EditorApplication::create_shadow_map() {
    destroy_shadow_map();
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    create_image(m_shadowMap.size, m_shadowMap.size, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_shadowMap.image, m_shadowMap.memory);
    m_shadowMap.view = create_image_view(m_shadowMap.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Depth-only render pass: the map ends in SHADER_READ_ONLY so the material
    // shaders can sample it without an extra transition.
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
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
    VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_shadowMap.renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow render pass");
    }
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_shadowMap.renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_shadowMap.view;
    fbInfo.width = m_shadowMap.size;
    fbInfo.height = m_shadowMap.size;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_shadowMap.framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow framebuffer");
    }

    // Comparison sampler: depth values are fetched raw (compareEnable is
    // inert for non-shadow sampler types) and the shader does the PCF-style
    // bias compare — same arrangement as the game's shadow path.
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowMap.sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow sampler");
    }

    // Position-only shaders (vertex reads all EditorVertex attributes to keep
    // the same vertex input state; only pos is used).
    const std::string vertSrc =
        "#version 450\n"
        "layout(push_constant) uniform Push { mat4 mvp; } pc;\n"
        "layout(location = 0) in vec3 inPos;\n"
        "layout(location = 1) in vec3 inNormal;\n"
        "layout(location = 2) in vec3 inColor;\n"
        "layout(location = 3) in vec2 inUv;\n"
        "void main() { gl_Position = pc.mvp * vec4(inPos, 1.0); }\n";
    const std::string fragSrc = "#version 450\nvoid main() {}\n";
    const std::vector<uint32_t> vertSpv = compile_material_glsl(VK_SHADER_STAGE_VERTEX_BIT, vertSrc);
    const std::vector<uint32_t> fragSpv = compile_material_glsl(VK_SHADER_STAGE_FRAGMENT_BIT, fragSrc);
    if (vertSpv.empty() || fragSpv.empty()) {
        throw std::runtime_error("Failed to compile shadow shaders");
    }
    m_shadowMap.vertShader = make_module(m_device, vertSpv);
    m_shadowMap.fragShader = make_module(m_device, fragSpv);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_shadowMap.pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline layout");
    }

    // Depth-only graphics pipeline (no color attachment, depth write on).
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_shadowMap.vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_shadowMap.fragShader;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(EditorVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, normal)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, color)) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EditorVertex, uv)) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.attachmentCount = 0;
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlending;
    info.pDynamicState = &dynamicState;
    info.layout = m_shadowMap.pipelineLayout;
    info.renderPass = m_shadowMap.renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_shadowMap.pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline");
    }
}

void EditorApplication::destroy_shadow_map() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_shadowMap.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_shadowMap.pipeline, nullptr);
    if (m_shadowMap.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_shadowMap.pipelineLayout, nullptr);
    if (m_shadowMap.vertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_shadowMap.vertShader, nullptr);
    if (m_shadowMap.fragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_shadowMap.fragShader, nullptr);
    if (m_shadowMap.sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_shadowMap.sampler, nullptr);
    if (m_shadowMap.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_shadowMap.framebuffer, nullptr);
    if (m_shadowMap.renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_shadowMap.renderPass, nullptr);
    if (m_shadowMap.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_shadowMap.view, nullptr);
    if (m_shadowMap.image != VK_NULL_HANDLE) vkDestroyImage(m_device, m_shadowMap.image, nullptr);
    if (m_shadowMap.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_shadowMap.memory, nullptr);
    m_shadowMap = EditorShadowMap{};
}

void EditorApplication::recreate_offscreen_if_needed(uint32_t w, uint32_t h) {
    if (m_offscreen.framebuffer != VK_NULL_HANDLE && m_offscreen.width == w && m_offscreen.height == h) {
        return;
    }
    if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device); // old attachments are in flight
    cleanup_offscreen_target();
    create_offscreen_buffers(w, h);
    // The offscreen cleanup also destroys the shadow map (size-independent
    // resources live together in cleanup_offscreen_target); bring it back so a
    // resize never silently kills the editor shadows.
    if (m_shadowMap.pipeline == VK_NULL_HANDLE) create_shadow_map();
}

void EditorApplication::cleanup_offscreen_target() {
    if (m_device == VK_NULL_HANDLE) return;
    destroy_shadow_map();
    if (m_offscreen.imguiTextureID != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_offscreen.imguiTextureID);
        m_offscreen.imguiTextureID = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickStagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_offscreen.pickStagingBuffer, nullptr);
        m_offscreen.pickStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickStagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickStagingMemory, nullptr);
        m_offscreen.pickStagingMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_offscreen.pickFramebuffer, nullptr);
        m_offscreen.pickFramebuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.pickView, nullptr);
        m_offscreen.pickView = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.pickImage, nullptr);
        m_offscreen.pickImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickMemory, nullptr);
        m_offscreen.pickMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickDepthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.pickDepthView, nullptr);
        m_offscreen.pickDepthView = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickDepthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.pickDepthImage, nullptr);
        m_offscreen.pickDepthImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.pickDepthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.pickDepthMemory, nullptr);
        m_offscreen.pickDepthMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_offscreen.framebuffer, nullptr);
        m_offscreen.framebuffer = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.depthView, nullptr);
        m_offscreen.depthView = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.depthImage, nullptr);
        m_offscreen.depthImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.depthMemory, nullptr);
        m_offscreen.depthMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.msaaColorView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.msaaColorView, nullptr);
        m_offscreen.msaaColorView = VK_NULL_HANDLE;
    }
    if (m_offscreen.msaaColorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.msaaColorImage, nullptr);
        m_offscreen.msaaColorImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.msaaColorMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.msaaColorMemory, nullptr);
        m_offscreen.msaaColorMemory = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_offscreen.colorView, nullptr);
        m_offscreen.colorView = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_offscreen.colorImage, nullptr);
        m_offscreen.colorImage = VK_NULL_HANDLE;
    }
    if (m_offscreen.colorMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_offscreen.colorMemory, nullptr);
        m_offscreen.colorMemory = VK_NULL_HANDLE;
    }
    m_offscreen.width = 0;
    m_offscreen.height = 0;
}

void EditorApplication::init_scene_pipeline() {
    m_viewportVertShader = make_module(m_device, read_spv("editor_viewport.vert.spv"));
    m_viewportFragShader = make_module(m_device, read_spv("editor_viewport.frag.spv"));
    m_pickFragShader = make_module(m_device, read_spv("editor_pick.frag.spv"));
    if (!m_viewportVertShader || !m_viewportFragShader || !m_pickFragShader) {
        throw std::runtime_error("Editor viewport shaders failed to compile (run the compile_shaders target)");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ScenePushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_scenePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scene pipeline layout");
    }

    m_scenePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                            m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                            false, true, true);
    m_wireframePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                                m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                                true, false, false);
    m_gizmoPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                            m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                            false, false, false);
    // Pick stays 1x (its render pass is 1x — sample counts must match).
    m_pickPipeline = create_scene_pipeline(m_device, m_offscreen.pickRenderPass, m_scenePipelineLayout,
                                           m_viewportVertShader, m_pickFragShader, VK_SAMPLE_COUNT_1_BIT,
                                           false, true, true);
    if (!m_scenePipeline || !m_wireframePipeline || !m_gizmoPipeline || !m_pickPipeline) {
        throw std::runtime_error("Failed to create viewport pipelines");
    }

    // Analytic infinite grid: fullscreen triangle, no vertex buffer. Per-family
    // X/Z screen-density filtering with constant screen-space line widths,
    // premultiplied alpha output; tests depth (LEQUAL) but does not write it.
    m_gridVertShader = make_module(m_device, read_spv("editor_grid.vert.spv"));
    m_gridFragShader = make_module(m_device, read_spv("editor_grid.frag.spv"));
    if (!m_gridVertShader || !m_gridFragShader) {
        throw std::runtime_error("Grid shaders failed to compile (run the compile_shaders target)");
    }
    VkPushConstantRange gridRange{};
    gridRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    gridRange.offset = 0;
    gridRange.size = sizeof(GridPushConstants);
    VkPipelineLayoutCreateInfo gridLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    gridLayoutInfo.pushConstantRangeCount = 1;
    gridLayoutInfo.pPushConstantRanges = &gridRange;
    if (vkCreatePipelineLayout(m_device, &gridLayoutInfo, nullptr, &m_gridPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create grid pipeline layout");
    }
    m_gridPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_gridPipelineLayout,
                                           m_gridVertShader, m_gridFragShader, m_viewportSamples,
                                           false /*wireframe*/, true /*depthTest*/, false /*cull*/,
                                           false /*withUv*/, true /*noVertexInput*/, true /*blend*/,
                                           true /*lessOrEqualDepth*/, true /*depthBias*/,
                                           false /*depthWrite*/);
    if (!m_gridPipeline) {
        throw std::runtime_error("Failed to create grid pipeline");
    }

    // Sky pass (Clima panel): the engine's procedural day/night sky, drawn as
    // the first fullscreen layer of the viewport. Push constants: mvp +
    // cameraPos + sunDirection + sunColor + environment = 128 bytes exactly.
    m_skyVertShader = make_module(m_device, read_spv("sky.vert.spv"));
    m_skyFragShader = make_module(m_device, read_spv("sky.frag.spv"));
    if (m_skyVertShader && m_skyFragShader) {
        VkPushConstantRange skyRange{};
        skyRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        skyRange.offset = 0;
        skyRange.size = sizeof(SkyPushConstants);
        VkPipelineLayoutCreateInfo skyLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        skyLayoutInfo.pushConstantRangeCount = 1;
        skyLayoutInfo.pPushConstantRanges = &skyRange;
        if (vkCreatePipelineLayout(m_device, &skyLayoutInfo, nullptr, &m_skyPipelineLayout) == VK_SUCCESS) {
            // Depth test LEQUAL against the cleared depth (1.0), no depth write,
            // opaque — entities and the grid draw over it with LESS.
            m_skyPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_skyPipelineLayout,
                                                  m_skyVertShader, m_skyFragShader, m_viewportSamples,
                                                  false /*wireframe*/, true /*depthTest*/, false /*cull*/,
                                                  false /*withUv*/, true /*noVertexInput*/, false /*blend*/,
                                                  true /*lessOrEqualDepth*/, false /*depthBias*/);
        }
    }

    // Hair strands: LINE_LIST with depth test (the editor viewport shaders
    // color strands per-vertex; the geometry is rebuilt every frame).
    m_hairPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_scenePipelineLayout,
                                           m_viewportVertShader, m_viewportFragShader, m_viewportSamples,
                                           true /*wireframe*/, true /*depthTest*/, false /*cull*/);

    // Gaussian splats: soft point clouds (POINT_LIST, alpha blend, depth test
    // without depth write so splats composite like real gaussian primitives).
    m_splatVertShader = make_module(m_device, read_spv("editor_splat.vert.spv"));
    m_splatFragShader = make_module(m_device, read_spv("editor_splat.frag.spv"));
    if (m_splatVertShader && m_splatFragShader) {
        VkPushConstantRange splatRange{};
        splatRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        splatRange.offset = 0;
        splatRange.size = sizeof(SplatPushConstants);
        VkPipelineLayoutCreateInfo splatLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        splatLayoutInfo.pushConstantRangeCount = 1;
        splatLayoutInfo.pPushConstantRanges = &splatRange;
        if (vkCreatePipelineLayout(m_device, &splatLayoutInfo, nullptr, &m_splatPipelineLayout) == VK_SUCCESS) {
            m_splatPipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_splatPipelineLayout,
                                                    m_splatVertShader, m_splatFragShader, m_viewportSamples,
                                                    false /*wireframe*/, true /*depthTest*/, false /*cull*/,
                                                    false /*withUv*/, false /*noVertexInput*/, true /*blend*/,
                                                    false /*lessOrEqual*/, false /*depthBias*/,
                                                    false /*depthWrite*/, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
        }
    }

    // Env probe: reflective sphere sampling the captured cubemap.
    m_envSphereVertShader = make_module(m_device, read_spv("editor_envsphere.vert.spv"));
    m_envSphereFragShader = make_module(m_device, read_spv("editor_envsphere.frag.spv"));
    if (m_envSphereVertShader && m_envSphereFragShader) {
        VkDescriptorSetLayoutBinding envBinding{};
        envBinding.binding = 0;
        envBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        envBinding.descriptorCount = 1;
        envBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo envDescInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        envDescInfo.bindingCount = 1;
        envDescInfo.pBindings = &envBinding;
        vkCreateDescriptorSetLayout(m_device, &envDescInfo, nullptr, &m_envSphereDescLayout);
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_envSphereDescPool);
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = m_envSphereDescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_envSphereDescLayout;
        vkAllocateDescriptorSets(m_device, &allocInfo, &m_envCapture.descriptorSet);
        VkPushConstantRange envRange{};
        envRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        envRange.offset = 0;
        envRange.size = sizeof(EnvSpherePushConstants);
        VkPipelineLayoutCreateInfo envLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        envLayoutInfo.setLayoutCount = 1;
        envLayoutInfo.pSetLayouts = &m_envSphereDescLayout;
        envLayoutInfo.pushConstantRangeCount = 1;
        envLayoutInfo.pPushConstantRanges = &envRange;
        if (vkCreatePipelineLayout(m_device, &envLayoutInfo, nullptr, &m_envSpherePipelineLayout) == VK_SUCCESS) {
            m_envSpherePipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, m_envSpherePipelineLayout,
                                                        m_envSphereVertShader, m_envSphereFragShader, m_viewportSamples,
                                                        false /*wireframe*/, true /*depthTest*/, false /*cull*/);
        }
    }
}

void EditorApplication::init_geometry_buffers() {
    // Cube
    std::vector<EditorVertex> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    generate_cube_geometry(cubeVerts, cubeIndices);
    m_cubeIndexCount = static_cast<uint32_t>(cubeIndices.size());
    VkDeviceSize cubeVBsize = sizeof(EditorVertex) * cubeVerts.size();
    VkDeviceSize cubeIBsize = sizeof(uint32_t) * cubeIndices.size();
    create_buffer(cubeVBsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cubeVB.buffer, m_cubeVB.memory);
    create_buffer(cubeIBsize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cubeIB.buffer, m_cubeIB.memory);
    void* data = nullptr;
    vkMapMemory(m_device, m_cubeVB.memory, 0, cubeVBsize, 0, &data);
    std::memcpy(data, cubeVerts.data(), static_cast<size_t>(cubeVBsize));
    vkUnmapMemory(m_device, m_cubeVB.memory);
    vkMapMemory(m_device, m_cubeIB.memory, 0, cubeIBsize, 0, &data);
    std::memcpy(data, cubeIndices.data(), static_cast<size_t>(cubeIBsize));
    vkUnmapMemory(m_device, m_cubeIB.memory);

    // (No grid vertex buffer: the grid is now analytic — a fullscreen
    // triangle rasterized by editor_grid.vert/frag with fwidth AA.)

    // Light icon (octahedron edges)
    std::vector<EditorVertex> lightVerts;
    generate_light_icon(lightVerts);
    m_lightIconVertexCount = static_cast<uint32_t>(lightVerts.size());
    VkDeviceSize lightSize = sizeof(EditorVertex) * lightVerts.size();
    create_buffer(lightSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_lightIconVB.buffer, m_lightIconVB.memory);
    vkMapMemory(m_device, m_lightIconVB.memory, 0, lightSize, 0, &data);
    std::memcpy(data, lightVerts.data(), static_cast<size_t>(lightSize));
    vkUnmapMemory(m_device, m_lightIconVB.memory);

    // Camera icon (pyramid edges)
    std::vector<EditorVertex> cameraVerts;
    generate_camera_icon(cameraVerts);
    m_cameraIconVertexCount = static_cast<uint32_t>(cameraVerts.size());
    VkDeviceSize cameraSize = sizeof(EditorVertex) * cameraVerts.size();
    create_buffer(cameraSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_cameraIconVB.buffer, m_cameraIconVB.memory);
    vkMapMemory(m_device, m_cameraIconVB.memory, 0, cameraSize, 0, &data);
    std::memcpy(data, cameraVerts.data(), static_cast<size_t>(cameraSize));
    vkUnmapMemory(m_device, m_cameraIconVB.memory);

    // Env probe preview sphere (UV sphere).
    {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        constexpr int kSeg = 32, kRings = 18;
        for (int r = 0; r <= kRings; ++r) {
            const float v = static_cast<float>(r) / kRings;
            const float phi = v * glm::pi<float>();
            for (int s = 0; s <= kSeg; ++s) {
                const float u = static_cast<float>(s) / kSeg;
                const float theta = u * glm::two_pi<float>();
                const glm::vec3 p(std::sin(phi) * std::cos(theta), std::cos(phi),
                                  std::sin(phi) * std::sin(theta));
                EditorVertex vert;
                vert.pos = p;
                vert.normal = p;
                vert.color = glm::vec3(1.0f);
                vert.uv = { u, 1.0f - v };
                verts.push_back(vert);
            }
        }
        for (int r = 0; r < kRings; ++r) {
            for (int s = 0; s < kSeg; ++s) {
                const uint32_t a = static_cast<uint32_t>(r * (kSeg + 1) + s);
                const uint32_t b = a + static_cast<uint32_t>(kSeg + 1);
                indices.push_back(a); indices.push_back(b); indices.push_back(a + 1);
                indices.push_back(a + 1); indices.push_back(b); indices.push_back(b + 1);
            }
        }
        m_envSphereIndexCount = static_cast<uint32_t>(indices.size());
        const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize is = sizeof(uint32_t) * indices.size();
        create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_envSphereVB.buffer, m_envSphereVB.memory);
        create_buffer(is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_envSphereIB.buffer, m_envSphereIB.memory);
        void* data = nullptr;
        vkMapMemory(m_device, m_envSphereVB.memory, 0, vs, 0, &data);
        std::memcpy(data, verts.data(), static_cast<size_t>(vs));
        vkUnmapMemory(m_device, m_envSphereVB.memory);
        vkMapMemory(m_device, m_envSphereIB.memory, 0, is, 0, &data);
        std::memcpy(data, indices.data(), static_cast<size_t>(is));
        vkUnmapMemory(m_device, m_envSphereIB.memory);
    }

    // Decal quad (1x1, facing +Z, UVs 0..1).
    {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        const float h = 0.5f;
        verts.push_back({ { -h, -h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 0.0f, 1.0f } });
        verts.push_back({ { h, -h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 1.0f, 1.0f } });
        verts.push_back({ { h, h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 1.0f, 0.0f } });
        verts.push_back({ { -h, h, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1, 1, 1 }, { 0.0f, 0.0f } });
        indices = { 0, 1, 2, 0, 2, 3 };
        m_decalIndexCount = static_cast<uint32_t>(indices.size());
        const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize is = sizeof(uint32_t) * indices.size();
        create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_decalVB.buffer, m_decalVB.memory);
        create_buffer(is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_decalIB.buffer, m_decalIB.memory);
        void* data = nullptr;
        vkMapMemory(m_device, m_decalVB.memory, 0, vs, 0, &data);
        std::memcpy(data, verts.data(), static_cast<size_t>(vs));
        vkUnmapMemory(m_device, m_decalVB.memory);
        vkMapMemory(m_device, m_decalIB.memory, 0, is, 0, &data);
        std::memcpy(data, indices.data(), static_cast<size_t>(is));
        vkUnmapMemory(m_device, m_decalIB.memory);
    }

    // Gizmo geometry (all modes)
    generate_gizmo_geometry();
}

void EditorApplication::generate_cube_geometry(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices) {
    build_cube(verts, indices);
}

void EditorApplication::generate_light_icon(std::vector<EditorVertex>& verts) {
    verts.clear();
    const glm::vec3 color(1.0f);
    const float r = 0.45f;
    const glm::vec3 pts[6] = {
        { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r }, { 0, 0, -r }
    };
    const int edges[12][2] = {
        {0, 2}, {0, 3}, {0, 4}, {0, 5},
        {1, 2}, {1, 3}, {1, 4}, {1, 5},
        {2, 4}, {4, 3}, {3, 5}, {5, 2}
    };
    for (const auto& e : edges) {
        EditorVertex a, b;
        a.pos = pts[e[0]]; a.normal = { 0, 1, 0 }; a.color = color;
        b.pos = pts[e[1]]; b.normal = { 0, 1, 0 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_camera_icon(std::vector<EditorVertex>& verts) {
    verts.clear();
    const glm::vec3 color(1.0f);
    // Pyramid pointing +Z: apex behind, near rectangle in front.
    const glm::vec3 apex(0.0f, 0.0f, -0.55f);
    const glm::vec3 corners[4] = {
        { -0.42f, -0.30f, 0.45f }, { 0.42f, -0.30f, 0.45f },
        { 0.42f, 0.30f, 0.45f }, { -0.42f, 0.30f, 0.45f }
    };
    const int edges[8][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {0, 4}, {1, 4}, {2, 4}, {3, 4}
    };
    glm::vec3 pts[5];
    pts[0] = corners[0]; pts[1] = corners[1]; pts[2] = corners[2]; pts[3] = corners[3]; pts[4] = apex;
    for (const auto& e : edges) {
        EditorVertex a, b;
        a.pos = pts[e[0]]; a.normal = { 0, 0, 1 }; a.color = color;
        b.pos = pts[e[1]]; b.normal = { 0, 0, 1 }; b.color = color;
        verts.push_back(a); verts.push_back(b);
    }
}

void EditorApplication::generate_gizmo_geometry() {
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    const float shaftLen = 1.55f;
    const float ringRadius = 1.45f;

    // Shafts (LINE_LIST) + cones (translate) + rings (rotate) + cubes (scale).
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 dir = kAxisDirs[axis];
        const glm::vec3 color = kAxisColors[axis];

        // Shaft from origin to 82% of the length.
        const uint32_t shaftBase = static_cast<uint32_t>(verts.size());
        EditorVertex origin, tip;
        origin.pos = glm::vec3(0.0f); origin.normal = dir; origin.color = color;
        tip.pos = dir * (shaftLen * 0.82f); tip.normal = dir; tip.color = color;
        verts.push_back(origin);
        verts.push_back(tip);
        m_gizmoShaftRanges[axis] = { static_cast<uint32_t>(indices.size()), 2 };
        indices.push_back(shaftBase);
        indices.push_back(shaftBase + 1);

        // Translate arrow cone.
        {
            const auto [offset, count] = append_cone(verts, indices, shaftLen * 0.72f, shaftLen, 0.09f, 12,
                                                     rotation_axis_from_y(dir), color);
            m_gizmoArrowRanges[axis] = GizmoDrawRange{ offset, count };
        }

        // Rotate ring (perpendicular to the axis).
        {
            const auto [offset, count] = append_ring(verts, indices, dir, ringRadius, 48, color);
            m_gizmoRingRanges[axis] = GizmoDrawRange{ offset, count };
        }

        // Scale tip cube at the end of the shaft.
        {
            glm::mat4 tipModel = glm::translate(glm::mat4(1.0f), dir * shaftLen);
            tipModel = glm::scale(tipModel, glm::vec3(0.17f));
            const auto [offset, count] = append_transformed_cube(verts, indices, tipModel, color);
            m_gizmoTipRanges[axis] = GizmoDrawRange{ offset, count };
        }
    }

    VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gizmoVB.buffer, m_gizmoVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_gizmoIB.buffer, m_gizmoIB.memory);
    void* data = nullptr;
    vkMapMemory(m_device, m_gizmoVB.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, m_gizmoVB.memory);
    vkMapMemory(m_device, m_gizmoIB.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, m_gizmoIB.memory);
}

// ===========================================================================
// Viewport rendering
// ===========================================================================

namespace {

void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

void draw_line_list(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, uint32_t vertexCount,
                    const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    push_constants(cmd, layout, mvp, color);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
}

void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color);
}

} // namespace

void EditorApplication::record_shadow_pass(VkCommandBuffer cmd, const Scene* scene) {
    m_shadowMap.enabled = false;
    if (m_shadowMap.pipeline == VK_NULL_HANDLE) return;

    // Sun direction from the scene's directional sun (or a fixed default).
    glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
    bool hasSun = false;
    if (scene) {
        for (const auto& [id, light] : scene->lightComponents) {
            if (!is_directional_sun(light)) continue;
            const auto tit = scene->transformComponents.find(id);
            if (tit != scene->transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                sunDir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            hasSun = true;
            break;
        }
    }
    if (!hasSun) return;

    // Ortho fit around the camera, depth remapped to [0,1] so the shared
    // computeShadow (sc.z in [0,1] after the divide) matches the stored depth.
    const glm::vec3 lightDir = glm::normalize(-sunDir);
    const glm::vec3 center = m_editorCamera.position;
    constexpr float kExtent = 35.0f;
    const glm::mat4 lightView = glm::lookAt(center + lightDir * 80.0f, center, glm::vec3(0, 1, 0));
    const glm::mat4 lightProj = glm::ortho(-kExtent, kExtent, -kExtent, kExtent, 0.1f, 200.0f);
    glm::mat4 depthRemap(1.0f);
    depthRemap[2][2] = 0.5f;
    depthRemap[2][3] = 0.5f;
    m_shadowMap.viewProj = depthRemap * lightProj * lightView;
    m_shadowMap.enabled = true;

    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = m_shadowMap.renderPass;
    info.framebuffer = m_shadowMap.framebuffer;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = { m_shadowMap.size, m_shadowMap.size };
    VkClearValue clear;
    clear.depthStencil = { 1.0f, 0 };
    info.clearValueCount = 1;
    info.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, m_shadowMap.size, m_shadowMap.size);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowMap.pipeline);

    // Casters: every entity with a mesh renderer and a loaded .vcmesh.
    for (const auto& [id, ent] : scene->get_entities()) {
        const auto transformIt = scene->transformComponents.find(id);
        if (transformIt == scene->transformComponents.end()) continue;
        const auto meshComp = scene->meshRendererComponents.find(id);
        if (meshComp == scene->meshRendererComponents.end() ||
            !meshComp->second.meshAssetID.is_valid()) continue;
        const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID);
        if (!mesh) continue;
        const glm::mat4 mvp = m_shadowMap.viewProj * model_from_transform(transformIt->second);
        vkCmdPushConstants(cmd, m_shadowMap.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &mvp);
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vb.buffer, &vertexOffset);
        if (mesh->ib.buffer != VK_NULL_HANDLE)
            vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const auto& range : mesh->ranges) {
            if (range.indexed)
                vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
            else
                vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
}

void EditorApplication::render_scene_to_offscreen(VkCommandBuffer cmd) {
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();
    record_shadow_pass(cmd, renderScene);

    build_viewport_render_graph();
    if (!m_viewportRenderGraphExecutor.valid() || m_offscreen.framebuffer == VK_NULL_HANDLE) return;

    // Re-register the scene pass every frame so the framebuffer stays current
    // after offscreen recreations (resize) — the executor keeps its own copy.
    Rendering::VulkanRenderGraphExecutor::PassFrame sceneFrame;
    sceneFrame.renderPass = m_offscreen.renderPass;
    sceneFrame.framebuffers = { m_offscreen.framebuffer };
    sceneFrame.clearValues.resize(2);
    sceneFrame.clearValues[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
    sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
    sceneFrame.draw = [this](VkCommandBuffer cb) { record_viewport_scene_content(cb); };
    m_viewportRenderGraphExecutor.register_pass(m_viewportScenePass, std::move(sceneFrame));

    // Scene pass recorded by the compiled graph: begins the offscreen render
    // pass, runs the content callback, ends — same executor the game uses.
    m_viewportRenderGraphExecutor.record(cmd, 0, { m_offscreen.width, m_offscreen.height });

    // Env-probe cubemap capture: recorded AFTER the viewport pass so the
    // command buffer is free to open its own 6-face render passes.
    record_env_capture(cmd, renderScene);
}

void EditorApplication::build_viewport_render_graph() {
    if (m_viewportRenderGraphBuilt) return;
    using namespace Engine::Rendering;
    const auto colorRes = m_viewportRenderGraph.add_resource({ "Viewport Color", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    const auto depthRes = m_viewportRenderGraph.add_resource({ "Viewport Depth", RenderResourceKind::Image, 0,
        m_offscreen.width, m_offscreen.height, 1, true, false, RenderResourceState::Undefined });
    m_viewportScenePass = m_viewportRenderGraph.add_pass({ "Scene", RenderQueue::Graphics,
        { { colorRes, RenderAccess::Write, RenderResourceState::ColorAttachment },
          { depthRes, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
    std::string error;
    if (!m_viewportRenderGraphExecutor.initialize(m_device, m_viewportRenderGraph, &error)) {
        std::cerr << "[Editor] viewport render graph init failed: " << error << std::endl;
        return;
    }
    m_viewportRenderGraphBuilt = true;
    std::cout << "[Editor] Viewport render graph wired ("
              << m_viewportRenderGraphExecutor.compile_result().order.size() << " passes, "
              << m_viewportRenderGraphExecutor.compile_result().barriers.size() << " barriers)\n";
}

void EditorApplication::record_viewport_scene_content(VkCommandBuffer cmd) {
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();

    set_viewport_scissor(cmd, m_offscreen.width, m_offscreen.height);

    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();

    // Sky pass (Clima panel): procedural sky driven by the scene's first
    // WeatherComponent and directional sun (fallbacks: warm white sun at noon).
    if (m_skyPipeline != VK_NULL_HANDLE) {
        glm::vec3 sunColor(1.0f, 0.95f, 0.85f);
        glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
        float windSpeed = 5.0f;
        if (renderScene) {
            for (const auto& [id, w] : renderScene->weatherComponents) {
                (void)id;
                sunColor = w.sunColor;
                windSpeed = w.windSpeed;
                break;
            }
            for (const auto& [id, light] : renderScene->lightComponents) {
                if (!is_directional_sun(light)) continue;
                const auto tit = renderScene->transformComponents.find(id);
                if (tit != renderScene->transformComponents.end()) {
                    const float yaw = glm::radians(tit->second.rotation.y);
                    const float pitch = glm::radians(tit->second.rotation.x);
                    sunDir = glm::normalize(glm::vec3(
                        std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                        std::cos(pitch) * std::cos(yaw)));
                }
                break;
            }
        }
        m_skyTime += 0.016f; // ~one frame; cloud drift driven by windSpeed below
        const SkyPushConstants skyPC{
            viewProj,
            glm::vec4(m_editorCamera.position, 1.0f),
            glm::vec4(sunDir, 0.0f),
            glm::vec4(sunColor, 0.0f),
            glm::vec4(m_skyTime * windSpeed * 0.02f, sunDir.y > -0.08f ? 1.0f : 0.0f, 0.0f, 0.0f),
        };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
        vkCmdPushConstants(cmd, m_skyPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(SkyPushConstants), &skyPC);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // Terrain (Terreno panel): procedural heightmap mesh on the ground.
    if (m_terrainValid && m_terrainVB.buffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        const glm::mat4 model(1.0f);
        draw_indexed_editor_mesh(cmd, m_scenePipelineLayout, m_terrainVB.buffer, m_terrainIB.buffer,
                                 m_terrainIndexCount, viewProj * model, glm::vec4(1.0f));
    }

    // Voxel sculpting volumes (colored cubes, paintable in the viewport).
    draw_voxel_volumes(cmd, viewProj, renderScene);

    // Analytic infinite grid (fullscreen triangle, fwidth AA, distance fade).
    // Gated by the viewport ⋯ menu (m_showGrid).
    if (m_showGrid && m_gridPipeline != VK_NULL_HANDLE) {
        const GridPushConstants gridPC{ m_editorCamera.get_view_matrix(),
                                        m_editorCamera.get_projection_matrix(aspect) };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        vkCmdPushConstants(cmd, m_gridPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GridPushConstants), &gridPC);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // Scene entities (renderScene resolved at the top of this function).
    if (renderScene) {
        for (const auto& [id, ent] : renderScene->get_entities()) {
            const auto transformIt = renderScene->transformComponents.find(id);
            if (transformIt == renderScene->transformComponents.end()) continue;
            // Layers: entities on a hidden layer are not rendered (the panel
            // toggle propagates to every entity sharing the layer name).
            const auto layerIt = renderScene->layerComponents.find(id);
            if (layerIt != renderScene->layerComponents.end() && !layerIt->second.visible) continue;
            const TransformComponent& t = transformIt->second;
            const bool selected = m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id;

            if (renderScene->lightComponents.contains(id)) {
                draw_light_icon(cmd, viewProj, t, selected);
            } else if (renderScene->cameraComponents.contains(id)) {
                draw_camera_frustum(cmd, viewProj, t, selected);
            } else if (renderScene->meshRendererComponents.contains(id) ||
                       renderScene->materialComponents.contains(id)) {
                glm::vec3 baseColor(0.72f, 0.75f, 0.82f);
                if (renderScene->materialComponents.contains(id)) {
                    baseColor = renderScene->materialComponents.at(id).albedo;
                }
                const glm::vec4 color = selected
                    ? glm::vec4(0.45f, 0.50f, 1.00f, 1.0f)
                    : glm::vec4(baseColor, 1.0f);
                bool drewMesh = false;
                const auto meshComp = renderScene->meshRendererComponents.find(id);
                if (meshComp != m_editorScene->meshRendererComponents.end() &&
                    meshComp->second.meshAssetID.is_valid()) {
                    if (const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID)) {
                        // Material-graph path: the mesh renderer's material asset,
                        // or the Material Editor's live graph on the selected entity.
                        GraphMaterialPipeline* gmp = nullptr;
                        const bool useLive = m_specializedEditors.previewOnSelected && selected;
                        if (useLive) {
                            const uint64_t liveHash = hash_material_graph(m_specializedEditors.live_material_graph());
                            if (liveHash != m_liveGraphHash || !m_liveGraphPipeline.valid) {
                                destroy_graph_pipeline(m_liveGraphPipeline);
                                if (!build_graph_pipeline(m_specializedEditors.live_material_graph(), m_liveGraphPipeline)) {
                                    if (!m_liveGraphLastErrorLogged) {
                                        std::cerr << "[Editor] Material preview: " << m_liveGraphPipeline.lastError << std::endl;
                                        m_liveGraphLastErrorLogged = true;
                                    }
                                } else {
                                    m_liveGraphLastErrorLogged = false;
                                }
                                m_liveGraphHash = liveHash;
                            }
                            gmp = m_liveGraphPipeline.valid ? &m_liveGraphPipeline : nullptr;
                        } else if (const auto vidIt = renderScene->videoComponents.find(id);
                                   vidIt != renderScene->videoComponents.end() &&
                                   !vidIt->second.framePaths.empty()) {
                            // Video flipbook: the mesh's texture is the current
                            // frame of the image sequence (cached per frame).
                            const VideoComponent& vc = vidIt->second;
                            const int frame = std::clamp(vc.currentFrame, 0,
                                static_cast<int>(vc.framePaths.size()) - 1);
                            const UUID frameTex =
                                resolve_texture_asset_by_name(vc.framePaths[frame]);
                            if (frameTex.is_valid()) {
                                gmp = ensure_texture_pipeline(frameTex, m_videoGraphPipelines);
                            }
                        } else if (const auto blockMeta = m_assetRegistry.find(meshComp->second.meshAssetID);
                                   blockMeta && blockMeta->type == AssetType::Block) {
                            // Block model in the scene: a textured cube. The
                            // pipeline binds the block texture (TextureSample,
                            // same path as the Material editor), cached per
                            // texture UUID and rebuilt if the texture changes.
                            const UUID texId = resolve_block_texture(meshComp->second.meshAssetID);
                            if (texId.is_valid()) gmp = ensure_texture_pipeline(texId, m_blockGraphPipelines);
                        } else if (const auto skinMeta = m_assetRegistry.find(meshComp->second.meshAssetID);
                                   skinMeta && skinMeta->type == AssetType::Texture &&
                                   is_character_texture(*skinMeta)) {
                            // Minecraft character/mob skin in the scene: the
                            // humanoid mesh with the skin sampled directly —
                            // no sidecar, the texture IS the character.
                            gmp = ensure_texture_pipeline(meshComp->second.meshAssetID, m_skinGraphPipelines);
                        } else if (meshComp->second.materialAssetID.is_valid() &&
                                   load_material_asset(meshComp->second.materialAssetID)) {
                            const UUID matId = meshComp->second.materialAssetID;
                            const MaterialAsset& mat = m_materialAssets.at(matId);
                            const Rendering::MaterialGraph graph = material_graph_from_asset(mat);
                            const uint64_t graphHash = hash_material_graph(graph);
                            auto it = m_graphMaterialPipelines.find(matId);
                            if (it == m_graphMaterialPipelines.end() ||
                                !it->second.valid || it->second.graphHash != graphHash) {
                                if (it != m_graphMaterialPipelines.end()) destroy_graph_pipeline(it->second);
                                GraphMaterialPipeline built;
                                built.graphHash = graphHash;
                                if (!build_graph_pipeline(graph, built)) {
                                    std::cerr << "[Editor] Material pipeline: " << built.lastError << std::endl;
                                }
                                it = m_graphMaterialPipelines.insert_or_assign(matId, std::move(built)).first;
                            }
                            if (it->second.valid) gmp = &it->second;
                        }
                        if (gmp) {
                            const MaterialAsset* matAsset = nullptr;
                            const auto matAssetIt = m_materialAssets.find(meshComp->second.materialAssetID);
                            if (matAssetIt != m_materialAssets.end()) matAsset = &matAssetIt->second;
                            const MaterialComponent* comp = nullptr;
                            const auto compIt = renderScene->materialComponents.find(id);
                            if (compIt != m_editorScene->materialComponents.end()) comp = &compIt->second;
                            write_material_ubo(*gmp, matAsset, comp);
                            write_light_ubo(*gmp, renderScene, m_editorCamera.position);
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                                                    0, 1, &gmp->descriptorSet, 0, nullptr);
                            const glm::mat4 model = model_from_transform(t);
                            const Rendering::MaterialPushConstants pc{ viewProj * model, model };
                            vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                               sizeof(pc), &pc);
                            const VkDeviceSize vertexOffset = 0;
                            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vb.buffer, &vertexOffset);
                            if (mesh->ib.buffer != VK_NULL_HANDLE)
                                vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                            for (const auto& range : mesh->ranges) {
                                if (range.indexed)
                                    vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
                                else
                                    vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
                            }
                            drewMesh = true;
                        } else {
                            // Vertex painting: entities with painted colors
                            // draw from their per-vertex-color buffer (the
                            // scene pipeline shades vertex colors).
                            const auto paintIt = renderScene->paintComponents.find(id);
                            const bool hasPaint = paintIt != renderScene->paintComponents.end() &&
                                                  paintIt->second.enabled &&
                                                  !paintIt->second.vertexColors.empty();
                            if (hasPaint) {
                                rebuild_paint_buffer(id, const_cast<PaintComponent&>(paintIt->second), mesh);
                                const auto pb = m_paintBuffers.find(id);
                                if (pb != m_paintBuffers.end() && pb->second.vb.buffer != VK_NULL_HANDLE) {
                                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                                    const VkDeviceSize off = 0;
                                    vkCmdBindVertexBuffers(cmd, 0, 1, &pb->second.vb.buffer, &off);
                                    if (mesh->ib.buffer != VK_NULL_HANDLE)
                                        vkCmdBindIndexBuffer(cmd, mesh->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                                    push_constants(cmd, m_scenePipelineLayout,
                                                   viewProj * model_from_transform(t), glm::vec4(1.0f));
                                    for (const auto& range : mesh->ranges) {
                                        if (range.indexed)
                                            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
                                        else
                                            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
                                    }
                                    drewMesh = true;
                                }
                            }
                            if (!drewMesh) {
                                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                                draw_mesh_resource(cmd, viewProj * model_from_transform(t), color, *mesh);
                                drewMesh = true;
                            }
                        }
                    }
                }
                if (!drewMesh) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                      m_cubeIndexCount, viewProj * model_from_transform(t), color);
                }
                if (selected) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                      m_cubeIndexCount, viewProj * model_from_transform(t),
                                      glm::vec4(0.55f, 0.60f, 1.00f, 1.0f));
                }
            } else {
                // Transform-only entity: subtle wireframe box.
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
                draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                  m_cubeIndexCount, viewProj * model_from_transform(t),
                                  selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f)
                                           : glm::vec4(0.35f, 0.38f, 0.50f, 1.0f));
            }

            // Hair strands: verlet-simulated LINE_LIST, rebuilt each frame.
            if (m_hairPipeline != VK_NULL_HANDLE) {
                const auto hairIt = renderScene->hairParticleComponents.find(id);
                if (hairIt != renderScene->hairParticleComponents.end() && hairIt->second.enabled) {
                    const auto sim = m_hairs.find(id);
                    if (sim != m_hairs.end() && sim->second.vb.buffer != VK_NULL_HANDLE) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_hairPipeline);
                        const VkDeviceSize off = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &sim->second.vb.buffer, &off);
                        push_constants(cmd, m_scenePipelineLayout, viewProj, glm::vec4(1.0f));
                        vkCmdDraw(cmd, sim->second.vertexCount, 1, 0, 0);
                    }
                }
            }

            // Soft body: verlet cloth mesh (local space, entity transform).
            const auto softIt = renderScene->softBodyComponents.find(id);
            if (softIt != renderScene->softBodyComponents.end() && softIt->second.enabled) {
                const auto sim = m_softBodies.find(id);
                if (sim != m_softBodies.end() && sim->second.vb.buffer != VK_NULL_HANDLE) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                    const VkDeviceSize off = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &sim->second.vb.buffer, &off);
                    vkCmdBindIndexBuffer(cmd, sim->second.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                    push_constants(cmd, m_scenePipelineLayout,
                                   viewProj * model_from_transform(t), glm::vec4(1.0f));
                    vkCmdDrawIndexed(cmd, sim->second.indexCount, 1, 0, 0, 0);
                }
            }

            // Decal: textured quad at the entity transform (texture pipeline).
            const auto decalIt = renderScene->decalComponents.find(id);
            if (decalIt != renderScene->decalComponents.end() && decalIt->second.enabled &&
                m_decalVB.buffer != VK_NULL_HANDLE) {
                const DecalComponent& dec = decalIt->second;
                const UUID texId = resolve_texture_asset_by_name(dec.texturePath);
                if (texId.is_valid()) {
                    if (GraphMaterialPipeline* dgmp = ensure_texture_pipeline(texId, m_blockGraphPipelines)) {
                        write_material_ubo(*dgmp, nullptr, nullptr);
                        write_light_ubo(*dgmp, renderScene, m_editorCamera.position);
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dgmp->pipeline);
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dgmp->layout,
                                                0, 1, &dgmp->descriptorSet, 0, nullptr);
                        glm::mat4 model = model_from_transform(t);
                        model = model * glm::scale(glm::mat4(1.0f),
                                                   glm::vec3(std::max(dec.size.x, 0.01f),
                                                             std::max(dec.size.y, 0.01f), 1.0f));
                        const Rendering::MaterialPushConstants dpc{ viewProj * model, model };
                        vkCmdPushConstants(cmd, dgmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                           sizeof(dpc), &dpc);
                        const VkDeviceSize off = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &m_decalVB.buffer, &off);
                        vkCmdBindIndexBuffer(cmd, m_decalIB.buffer, 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, m_decalIndexCount, 1, 0, 0, 0);
                    }
                }
            }

            if (m_showColliders && renderScene->rigidbodyComponents.contains(id)) {
                draw_collider_wireframe(cmd, viewProj, t, selected);
            }
        }
    }

    // Gaussian splat clouds: soft point splats (cached per entity, rebuilt
    // when the parameters change or regenerate is requested).
    if (m_splatPipeline != VK_NULL_HANDLE) {
        for (const auto& [id, gs] : renderScene->gaussianSplatComponents) {
            if (!gs.enabled) continue;
            const auto tit = renderScene->transformComponents.find(id);
            if (tit == renderScene->transformComponents.end()) continue;
            auto& cloud = m_splatClouds[id];
            if (cloud.dirty || cloud.count != gs.count || cloud.scale != gs.scale) {
                std::vector<EditorVertex> verts;
                generate_splat_cloud(gs, verts);
                const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
                if (cloud.vb.buffer != VK_NULL_HANDLE && cloud.vb.size < vs) {
                    destroy_buffer(cloud.vb);
                    cloud.vb = GPUBuffer{};
                }
                if (cloud.vb.buffer == VK_NULL_HANDLE) {
                    create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  cloud.vb.buffer, cloud.vb.memory);
                }
                void* data = nullptr;
                vkMapMemory(m_device, cloud.vb.memory, 0, vs, 0, &data);
                std::memcpy(data, verts.data(), static_cast<size_t>(vs));
                vkUnmapMemory(m_device, cloud.vb.memory);
                cloud.count = gs.count;
                cloud.scale = gs.scale;
                cloud.dirty = false;
            }
            if (cloud.vb.buffer == VK_NULL_HANDLE) continue;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_splatPipeline);
            const VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &cloud.vb.buffer, &off);
            const SplatPushConstants spc{ viewProj * model_from_transform(tit->second),
                glm::vec4(gs.pointSize, static_cast<float>(m_offscreen.height), gs.opacity, 0.0f) };
            vkCmdPushConstants(cmd, m_splatPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(spc), &spc);
            vkCmdDraw(cmd, cloud.count, 1, 0, 0);
        }
    }

    // Env probe: the captured cubemap previewed on a reflective sphere at the
    // probe position (capture happens after the viewport pass, so this frame
    // samples the previous capture — one frame of latency, like real probes).
    if (m_envCapture.valid && m_envSpherePipeline != VK_NULL_HANDLE) {
        const auto tit = renderScene->transformComponents.find(m_envCapture.entity);
        if (tit != renderScene->transformComponents.end()) {
            const glm::mat4 model = glm::translate(glm::mat4(1.0f), tit->second.position)
                                  * glm::scale(glm::mat4(1.0f), glm::vec3(1.4f));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_envSpherePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_envSpherePipelineLayout,
                                    0, 1, &m_envCapture.descriptorSet, 0, nullptr);
            const EnvSpherePushConstants epc{ viewProj * model, model,
                                              glm::vec4(m_editorCamera.position, 1.0f) };
            vkCmdPushConstants(cmd, m_envSpherePipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(epc), &epc);
            const VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_envSphereVB.buffer, &off);
            vkCmdBindIndexBuffer(cmd, m_envSphereIB.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m_envSphereIndexCount, 1, 0, 0, 0);
        }
    }

    // Gizmo on the selected entity (drawn every frame; the active axis is
    // highlighted while dragging). Gated by the viewport ⋯ menu (m_showGizmos)
    // and hidden entirely in Select mode.
    if (m_showGizmos && m_gizmoMode != GizmoMode::Select) {
        draw_gizmo_overlay(cmd, viewProj);
    }
}

void EditorApplication::draw_light_icon(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                        const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), t.position) * glm::scale(glm::mat4(1.0f), glm::vec3(1.2f));
    const glm::vec4 color = selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f) : glm::vec4(1.0f, 0.85f, 0.35f, 1.0f);
    draw_line_list(cmd, m_scenePipelineLayout, m_lightIconVB.buffer, m_lightIconVertexCount,
                   viewProj * model, color);
}

void EditorApplication::draw_camera_frustum(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                            const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), t.position)
                          * glm::rotate(glm::mat4(1.0f), glm::radians(t.rotation.y), glm::vec3(0, 1, 0))
                          * glm::rotate(glm::mat4(1.0f), glm::radians(t.rotation.x), glm::vec3(1, 0, 0))
                          * glm::scale(glm::mat4(1.0f), glm::vec3(1.4f));
    const glm::vec4 color = selected ? glm::vec4(0.55f, 0.60f, 1.00f, 1.0f) : glm::vec4(0.35f, 0.75f, 1.00f, 1.0f);
    draw_line_list(cmd, m_scenePipelineLayout, m_cameraIconVB.buffer, m_cameraIconVertexCount,
                   viewProj * model, color);
}

void EditorApplication::draw_collider_wireframe(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                                const TransformComponent& t, bool selected) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    const glm::vec4 color = selected ? glm::vec4(1.00f, 0.75f, 0.35f, 1.0f) : glm::vec4(0.95f, 0.55f, 0.25f, 0.85f);
    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                      m_cubeIndexCount, viewProj * model_from_transform(t), color);
}

void EditorApplication::draw_entity_bounds(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                           UUID id, const TransformComponent& t) {
    (void)id;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                      m_cubeIndexCount, viewProj * model_from_transform(t),
                      glm::vec4(0.55f, 0.60f, 1.00f, 1.0f));
}

void EditorApplication::draw_gizmo_overlay(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    const auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;
    // World/Local: in local mode the whole gizmo rotates with the entity so
    // the axes follow its orientation.
    glm::mat4 gizmoModel = glm::translate(glm::mat4(1.0f), it->second.position);
    if (m_gizmoLocal) {
        gizmoModel = gizmoModel * glm::mat4_cast(glm::quat(glm::radians(it->second.rotation)));
    }

    const glm::vec4 highlight(1.0f, 0.85f, 0.30f, 1.0f);
    const glm::vec4 normal(1.0f);

    const VkDeviceSize zeroOffset = 0;
    const auto bind_gizmo = [&]() {
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_gizmoVB.buffer, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, m_gizmoIB.buffer, 0, VK_INDEX_TYPE_UINT32);
    };

    // Solid pieces: arrow cones (translate) or tip cubes (scale).
    if (m_gizmoMode == GizmoMode::Translate || m_gizmoMode == GizmoMode::Scale) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gizmoPipeline);
        for (int axis = 0; axis < 3; ++axis) {
            const bool active = (m_activeAxis == static_cast<GizmoAxis>(axis + 1));
            const EditorApplication::GizmoDrawRange& range =
                (m_gizmoMode == GizmoMode::Translate) ? m_gizmoArrowRanges[axis] : m_gizmoTipRanges[axis];
            bind_gizmo();
            push_constants(cmd, m_scenePipelineLayout, viewProj * gizmoModel,
                           active ? highlight : normal);
            vkCmdDrawIndexed(cmd, range.count, 1, range.offset, 0, 0);
        }
    }

    // Wireframe pieces: shafts (translate/scale) or rings (rotate).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
    for (int axis = 0; axis < 3; ++axis) {
        const bool active = (m_activeAxis == static_cast<GizmoAxis>(axis + 1));
        const EditorApplication::GizmoDrawRange& range =
            (m_gizmoMode == GizmoMode::Rotate) ? m_gizmoRingRanges[axis] : m_gizmoShaftRanges[axis];
        bind_gizmo();
        push_constants(cmd, m_scenePipelineLayout, viewProj * gizmoModel,
                       active ? highlight : normal);
        vkCmdDrawIndexed(cmd, range.count, 1, range.offset, 0, 0);
    }
}

void EditorApplication::render_pick_pass(VkCommandBuffer cmd) {
    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = m_offscreen.pickRenderPass;
    info.framebuffer = m_offscreen.pickFramebuffer;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = { m_offscreen.width, m_offscreen.height };
    VkClearValue clears[2];
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    info.clearValueCount = 2;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport_scissor(cmd, m_offscreen.width, m_offscreen.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pickPipeline);

    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();

    Scene* pickScene = m_playMode.get_active_scene();
    if (!pickScene) pickScene = m_editorScene.get();
    m_pickColorToEntity.clear();
    uint32_t nextId = 1;
    if (pickScene) {
        for (const auto& [id, ent] : pickScene->get_entities()) {
            const auto transformIt = pickScene->transformComponents.find(id);
            if (transformIt == pickScene->transformComponents.end()) continue;
            const uint32_t pickId = nextId++;
            m_pickColorToEntity[pickId] = id;
            const glm::vec4 color(
                static_cast<float>(pickId & 0xFF) / 255.0f,
                static_cast<float>((pickId >> 8) & 0xFF) / 255.0f,
                static_cast<float>((pickId >> 16) & 0xFF) / 255.0f,
                1.0f);
            bool drewMesh = false;
            const auto meshComp = pickScene->meshRendererComponents.find(id);
            if (meshComp != m_editorScene->meshRendererComponents.end() &&
                meshComp->second.meshAssetID.is_valid()) {
                if (const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID)) {
                    draw_mesh_resource(cmd, viewProj * model_from_transform(transformIt->second), color, *mesh);
                    drewMesh = true;
                }
            }
            if (!drewMesh) {
                draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                                  m_cubeIndexCount, viewProj * model_from_transform(transformIt->second), color);
            }
        }
    }
    vkCmdEndRenderPass(cmd);
}

void EditorApplication::perform_pick_readback() {
    if (!m_editorScene || m_offscreen.framebuffer == VK_NULL_HANDLE || !m_pickPipeline) return;
    if (m_pickPixel.x < 0 || m_pickPixel.y < 0 ||
        m_pickPixel.x >= static_cast<float>(m_offscreen.width) ||
        m_pickPixel.y >= static_cast<float>(m_offscreen.height)) return;

    VkCommandBuffer cmd = begin_single_time_commands();
    render_pick_pass(cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { m_offscreen.width, m_offscreen.height, 1 };
    vkCmdCopyImageToBuffer(cmd, m_offscreen.pickImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_offscreen.pickStagingBuffer, 1, &region);
    end_single_time_commands(cmd);

    void* mapped = nullptr;
    vkMapMemory(m_device, m_offscreen.pickStagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);

    // Click pick
    const size_t x = static_cast<size_t>(m_pickPixel.x);
    const size_t y = static_cast<size_t>(m_pickPixel.y);
    const uint8_t* pixel = static_cast<const uint8_t*>(mapped) + (y * m_offscreen.width + x) * 4;
    const uint32_t id = static_cast<uint32_t>(pixel[0]) |
                        (static_cast<uint32_t>(pixel[1]) << 8) |
                        (static_cast<uint32_t>(pixel[2]) << 16);

    // Hover pick (same buffer, different pixel — no extra GPU pass)
    const size_t hx = std::clamp(static_cast<size_t>(m_hoverPickPixel.x), size_t(0), static_cast<size_t>(m_offscreen.width) - 1);
    const size_t hy = std::clamp(static_cast<size_t>(m_hoverPickPixel.y), size_t(0), static_cast<size_t>(m_offscreen.height) - 1);
    const uint8_t* hp = static_cast<const uint8_t*>(mapped) + (hy * m_offscreen.width + hx) * 4;
    const uint32_t hoverId = static_cast<uint32_t>(hp[0]) |
                             (static_cast<uint32_t>(hp[1]) << 8) |
                             (static_cast<uint32_t>(hp[2]) << 16);
    vkUnmapMemory(m_device, m_offscreen.pickStagingMemory);

    const auto found = m_pickColorToEntity.find(id);
    if (found != m_pickColorToEntity.end()) {
        m_selectedEntity = m_editorScene->find_entity_by_id(found->second);
        m_editorGui.select_entity(m_selectedEntity);
    }

    // Resolve hover entity name for the viewport tooltip
    if (hoverId != 0) {
        const auto hIt = m_pickColorToEntity.find(hoverId);
        if (hIt != m_pickColorToEntity.end()) {
            const Entity he = m_editorScene->find_entity_by_id(hIt->second);
            if (he.is_valid()) {
                m_hoverEntityName = he.get_name();
            } else {
                m_hoverEntityName.clear();
            }
        } else {
            m_hoverEntityName.clear();
        }
    } else {
        m_hoverEntityName.clear();
    }
}

// ===========================================================================
// Camera and gizmo interaction
// ===========================================================================

// Shared executor for Control API commands — used both by the loopback HTTP
// server and by the Control Console window buttons.

} // namespace Engine

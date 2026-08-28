#pragma once

#include "VulkanTypes.hpp"
#include "GpuRenderFeatures.hpp"

#include <cstdint>

namespace Engine::Rendering {

struct GpuFeaturePasses {
    VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkShaderModule shaderModule{VK_NULL_HANDLE};
    VkBuffer featureBuffer{VK_NULL_HANDLE};
    VmaAllocation featureAllocation{VK_NULL_HANDLE};
    void* featureMapped{nullptr};
    VkBuffer historyBuffer{VK_NULL_HANDLE};
    VmaAllocation historyAllocation{VK_NULL_HANDLE};
    void* historyMapped{nullptr};
    VkBuffer reservoirBuffer{VK_NULL_HANDLE};
    VmaAllocation reservoirAllocation{VK_NULL_HANDLE};
    void* reservoirMapped{nullptr};
    VkBuffer probeBuffer{VK_NULL_HANDLE};
    VmaAllocation probeAllocation{VK_NULL_HANDLE};
    void* probeMapped{nullptr};
    VkDeviceSize historySize{0};
    VkDeviceSize reservoirSize{0};
    VkDeviceSize probeSize{0};
    VkExtent2D extent{};
    VkFormat historyFormat{VK_FORMAT_UNDEFINED};
    bool initialized{false};
};

bool create_gpu_feature_passes(VkDevice device, VmaAllocator allocator,
                               VkFormat historyFormat, VkExtent2D extent,
                               GpuFeaturePasses& out);
void update_gpu_feature_passes(VmaAllocator allocator, GpuFeaturePasses& passes,
                               const GpuRenderFeatures& features);
void record_gpu_feature_passes(VkCommandBuffer commandBuffer,
                               const GpuFeaturePasses& passes,
                               VkImage hdrImage, VkImageView hdrView,
                               VkImageLayout hdrLayout,
                               VkExtent2D extent,
                               const GpuRenderFeatures& features);
void destroy_gpu_feature_passes(VkDevice device, VmaAllocator allocator,
                                GpuFeaturePasses& passes);

} // namespace Engine::Rendering

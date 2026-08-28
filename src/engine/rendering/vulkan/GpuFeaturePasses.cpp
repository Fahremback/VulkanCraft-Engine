#include "GpuFeaturePasses.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>
#include "engine/rendering/lighting/RadianceCache.hpp"

namespace Engine::Rendering {
namespace {
VkShaderModule load_shader(VkDevice device, const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return VK_NULL_HANDLE;
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) return VK_NULL_HANDLE;
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(size);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

bool create_host_buffer(VmaAllocator allocator, VkDeviceSize size, VkBuffer& buffer,
                        VmaAllocation& allocation, void*& mapped) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(allocator, &info, &ai, &buffer, &allocation, &result) != VK_SUCCESS) return false;
    mapped = result.pMappedData;
    if (!mapped) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(size));
    return true;
}
}

bool create_gpu_feature_passes(VkDevice device, VmaAllocator allocator, VkFormat historyFormat,
                               VkExtent2D extent, GpuFeaturePasses& out) {
    if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE || !extent.width || !extent.height) return false;
    out = {};
    out.extent = extent;
    out.historyFormat = historyFormat;
    out.historySize = static_cast<VkDeviceSize>(extent.width) * extent.height * 16;
    out.reservoirSize = std::max<VkDeviceSize>(sizeof(RadianceCache::ReservoirGpu), out.historySize / 4);
    out.probeSize = std::max<VkDeviceSize>(sizeof(RadianceCache::ProbeGpu), out.historySize / 4);
    if (!create_host_buffer(allocator, sizeof(GpuRenderFeatures), out.featureBuffer,
                            out.featureAllocation, out.featureMapped) ||
        !create_host_buffer(allocator, out.historySize, out.historyBuffer,
                            out.historyAllocation, out.historyMapped) ||
        !create_host_buffer(allocator, out.reservoirSize, out.reservoirBuffer,
                            out.reservoirAllocation, out.reservoirMapped) ||
        !create_host_buffer(allocator, out.probeSize, out.probeBuffer,
                            out.probeAllocation, out.probeMapped)) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[4]{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &out.descriptorLayout) != VK_SUCCESS) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &out.descriptorPool) != VK_SUCCESS) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = out.descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &out.descriptorLayout;
    if (vkAllocateDescriptorSets(device, &setInfo, &out.descriptorSet) != VK_SUCCESS) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuRenderFeatures)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &out.descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &out.pipelineLayout) != VK_SUCCESS) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }
    VkShaderModule shader = load_shader(device, VULKANCRAFT_SHADER_DIR "/feature_temporal.comp.spv");
    if (shader == VK_NULL_HANDLE) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = out.pipelineLayout;
    const VkResult pipelineResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                                              &pipelineInfo, nullptr, &out.pipeline);
    vkDestroyShaderModule(device, shader, nullptr);
    if (pipelineResult != VK_SUCCESS) {
        destroy_gpu_feature_passes(device, allocator, out);
        return false;
    }
    VkDescriptorBufferInfo infos[4]{
        {out.featureBuffer, 0, sizeof(GpuRenderFeatures)},
        {out.historyBuffer, 0, out.historySize},
        {out.reservoirBuffer, 0, out.reservoirSize},
        {out.probeBuffer, 0, out.probeSize}
    };
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = out.descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    out.initialized = true;
    return true;
}

void update_gpu_feature_passes(VmaAllocator allocator, GpuFeaturePasses& passes,
                               const GpuRenderFeatures& features) {
    if (!passes.initialized || allocator == VK_NULL_HANDLE || !passes.featureMapped) return;
    std::memcpy(passes.featureMapped, &features, sizeof(features));
    vmaFlushAllocation(allocator, passes.featureAllocation, 0, sizeof(features));
}

void record_gpu_feature_passes(VkCommandBuffer commandBuffer, const GpuFeaturePasses& passes,
                               VkImage hdrImage, VkImageView hdrView, VkImageLayout hdrLayout,
                               VkExtent2D extent, const GpuRenderFeatures& features) {
    if (!passes.initialized || commandBuffer == VK_NULL_HANDLE || passes.pipeline == VK_NULL_HANDLE ||
        passes.pipelineLayout == VK_NULL_HANDLE || passes.descriptorSet == VK_NULL_HANDLE ||
        passes.featureBuffer == VK_NULL_HANDLE || passes.historyBuffer == VK_NULL_HANDLE ||
        passes.reservoirBuffer == VK_NULL_HANDLE || passes.probeBuffer == VK_NULL_HANDLE ||
        hdrImage == VK_NULL_HANDLE || hdrView == VK_NULL_HANDLE || !extent.width || !extent.height) return;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, passes.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, passes.pipelineLayout,
                            0, 1, &passes.descriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, passes.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(GpuRenderFeatures), &features);
    VkBufferMemoryBarrier2 inputBarriers[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        inputBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        inputBarriers[i].srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        inputBarriers[i].srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        inputBarriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        inputBarriers[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }
    inputBarriers[0].buffer = passes.featureBuffer;
    inputBarriers[0].size = sizeof(GpuRenderFeatures);
    inputBarriers[1].buffer = passes.reservoirBuffer;
    inputBarriers[1].size = passes.reservoirSize;
    inputBarriers[2].buffer = passes.probeBuffer;
    inputBarriers[2].size = passes.probeSize;
    VkDependencyInfo inputDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    inputDependency.bufferMemoryBarrierCount = 3;
    inputDependency.pBufferMemoryBarriers = inputBarriers;
    vkCmdPipelineBarrier2(commandBuffer, &inputDependency);
    vkCmdDispatch(commandBuffer, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);

    VkBufferMemoryBarrier2 historyBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    historyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    historyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    historyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    historyBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    historyBarrier.buffer = passes.historyBuffer;
    historyBarrier.offset = 0;
    historyBarrier.size = passes.historySize;
    VkBufferMemoryBarrier2 outputBarriers[3] = {historyBarrier, historyBarrier, historyBarrier};
    outputBarriers[0].buffer = passes.historyBuffer;
    outputBarriers[0].size = passes.historySize;
    outputBarriers[1].buffer = passes.reservoirBuffer;
    outputBarriers[1].size = passes.reservoirSize;
    outputBarriers[2].buffer = passes.probeBuffer;
    outputBarriers[2].size = passes.probeSize;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 3;
    dependency.pBufferMemoryBarriers = outputBarriers;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    (void)hdrImage;
    (void)hdrView;
    (void)hdrLayout;
}

void destroy_gpu_feature_passes(VkDevice device, VmaAllocator allocator, GpuFeaturePasses& passes) {
    if (device != VK_NULL_HANDLE) {
        if (passes.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, passes.pipeline, nullptr);
        if (passes.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, passes.pipelineLayout, nullptr);
        if (passes.descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, passes.descriptorPool, nullptr);
        if (passes.descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, passes.descriptorLayout, nullptr);
    }
    if (allocator != VK_NULL_HANDLE) {
        if (passes.featureBuffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, passes.featureBuffer, passes.featureAllocation);
        if (passes.historyBuffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, passes.historyBuffer, passes.historyAllocation);
        if (passes.reservoirBuffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, passes.reservoirBuffer, passes.reservoirAllocation);
        if (passes.probeBuffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, passes.probeBuffer, passes.probeAllocation);
    }
    passes = {};
}
}

#include "GpuRenderFeatures.hpp"

#include <array>
#include <cstring>

namespace Engine::Rendering {

void create_gpu_feature_binding(VkDevice device, VmaAllocator allocator,
                                GpuFeatureBinding& out) {
    if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE) return;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &out.layout) != VK_SUCCESS) return;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &out.pool) != VK_SUCCESS) {
        destroy_gpu_feature_binding(device, allocator, out);
        return;
    }

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = out.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &out.layout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &out.set) != VK_SUCCESS) {
        destroy_gpu_feature_binding(device, allocator, out);
        return;
    }

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = sizeof(GpuRenderFeatures);
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocationResult{};
    if (vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &out.uniformBuffer,
                       &out.allocation, &allocationResult) != VK_SUCCESS) {
        destroy_gpu_feature_binding(device, allocator, out);
        return;
    }
    out.mapped = allocationResult.pMappedData;
    if (!out.mapped) {
        destroy_gpu_feature_binding(device, allocator, out);
        return;
    }
    std::memset(out.mapped, 0, sizeof(GpuRenderFeatures));

    VkDescriptorBufferInfo descriptor{out.uniformBuffer, 0, sizeof(GpuRenderFeatures)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = out.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &descriptor;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void update_gpu_feature_binding(VmaAllocator allocator, GpuFeatureBinding& binding,
                                const GpuRenderFeatures& features) {
    if (!binding.mapped || allocator == VK_NULL_HANDLE) return;
    *static_cast<GpuRenderFeatures*>(binding.mapped) = features;
    vmaFlushAllocation(allocator, binding.allocation, 0, sizeof(GpuRenderFeatures));
}

void destroy_gpu_feature_binding(VkDevice device, VmaAllocator allocator,
                                 GpuFeatureBinding& binding) {
    if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE) {
        binding = {};
        return;
    }
    if (binding.uniformBuffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator, binding.uniformBuffer, binding.allocation);
        binding.uniformBuffer = VK_NULL_HANDLE;
        binding.allocation = VK_NULL_HANDLE;
    if (binding.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, binding.pool, nullptr);
    if (binding.layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, binding.layout, nullptr);
    binding = {};
}

} // namespace Engine::Rendering

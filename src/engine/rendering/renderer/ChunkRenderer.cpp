#include "ChunkRenderer.hpp"

#include <cstddef>
#include <cstring>

ChunkRenderResource* ChunkRenderer::find(ChunkId id) {
    const auto found = resources_.find(id);
    return found == resources_.end() ? nullptr : &found->second;
}

const ChunkRenderResource* ChunkRenderer::find(ChunkId id) const {
    const auto found = resources_.find(id);
    return found == resources_.end() ? nullptr : &found->second;
}

bool ChunkRenderer::has_resource(ChunkId id) const {
    const auto* resource = find(id);
    return resource && resource->vertexBuffer.buffer != VK_NULL_HANDLE;
}

void ChunkRenderer::upload(ChunkMeshResult result, VkDevice device, VmaAllocator allocator,
                           std::vector<AllocatedBuffer>* retiredBuffers) {
    if (!result.valid) return;
    auto& mesh = result.mesh;
    ChunkRenderResource& resource = resources_[result.chunk];
    auto retireBuffer = [&](AllocatedBuffer& buffer) {
        if (buffer.buffer == VK_NULL_HANDLE) return;
        if (retiredBuffers) retiredBuffers->push_back(buffer);
        else {
            vkDeviceWaitIdle(device);
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
        }
        buffer = {};
    };
    retireBuffer(resource.vertexBuffer);
    retireBuffer(resource.waterVertexBuffer);
    retireBuffer(resource.grassInstanceBuffer);
    retireBuffer(resource.foliageInstanceBuffer);

    constexpr VkDeviceSize streamAlignment = 16;
    const auto alignStream = [](VkDeviceSize value) {
        return (value + streamAlignment - 1) & ~(streamAlignment - 1);
    };
    const VkDeviceSize solidBytes = mesh.meshVertices.size() * sizeof(VoxelVertex);
    const VkDeviceSize waterBytes = mesh.waterMeshVertices.size() * sizeof(VoxelVertex);
    const VkDeviceSize grassBytes = mesh.grassInstances.size() * sizeof(GrassInstance);
    const VkDeviceSize foliageBytes = mesh.foliageInstances.size() * sizeof(FoliageInstance);
    resource.waterVertexOffset = alignStream(solidBytes);
    resource.grassInstanceOffset = alignStream(resource.waterVertexOffset + waterBytes);
    resource.foliageInstanceOffset = alignStream(resource.grassInstanceOffset + grassBytes);
    const VkDeviceSize combinedBytes = resource.foliageInstanceOffset + foliageBytes;

    if (combinedBytes > 0) {
        VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = combinedBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo,
                                 &resource.vertexBuffer.buffer,
                                 &resource.vertexBuffer.allocation, nullptr));
        void* mapped = nullptr;
        VK_CHECK(vmaMapMemory(allocator, resource.vertexBuffer.allocation, &mapped));
        auto* bytes = static_cast<std::byte*>(mapped);
        if (solidBytes) std::memcpy(bytes, mesh.meshVertices.data(), static_cast<size_t>(solidBytes));
        if (waterBytes) std::memcpy(bytes + resource.waterVertexOffset, mesh.waterMeshVertices.data(), static_cast<size_t>(waterBytes));
        if (grassBytes) std::memcpy(bytes + resource.grassInstanceOffset, mesh.grassInstances.data(), static_cast<size_t>(grassBytes));
        if (foliageBytes) std::memcpy(bytes + resource.foliageInstanceOffset, mesh.foliageInstances.data(), static_cast<size_t>(foliageBytes));
        VK_CHECK(vmaFlushAllocation(allocator, resource.vertexBuffer.allocation, 0, combinedBytes));
        vmaUnmapMemory(allocator, resource.vertexBuffer.allocation);
    }

    resource.vertexCount = mesh.pendingVertexCount;
    resource.waterVertexCount = mesh.pendingWaterVertexCount;
    resource.grassInstanceCount = mesh.pendingGrassInstanceCount;
    resource.foliageInstanceCount = mesh.pendingFoliageInstanceCount;
}

void ChunkRenderer::draw(ChunkId id, VkCommandBuffer commandBuffer) const {
    const auto* resource = find(id);
    if (!resource || !resource->vertexCount || resource->vertexBuffer.buffer == VK_NULL_HANDLE) return;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &resource->vertexBuffer.buffer, &offset);
    vkCmdDraw(commandBuffer, resource->vertexCount, 1, 0, 0);
}

void ChunkRenderer::draw_water(ChunkId id, VkCommandBuffer commandBuffer) const {
    const auto* resource = find(id);
    if (!resource || !resource->waterVertexCount || resource->vertexBuffer.buffer == VK_NULL_HANDLE) return;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &resource->vertexBuffer.buffer, &resource->waterVertexOffset);
    vkCmdDraw(commandBuffer, resource->waterVertexCount, 1, 0, 0);
}

void ChunkRenderer::draw_grass(ChunkId id, VkCommandBuffer commandBuffer) const {
    const auto* resource = find(id);
    if (!resource || !resource->grassInstanceCount || resource->vertexBuffer.buffer == VK_NULL_HANDLE) return;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &resource->vertexBuffer.buffer, &resource->grassInstanceOffset);
    vkCmdDraw(commandBuffer, 6, resource->grassInstanceCount, 0, 0);
}

void ChunkRenderer::draw_foliage(ChunkId id, VkCommandBuffer commandBuffer) const {
    const auto* resource = find(id);
    if (!resource || !resource->foliageInstanceCount || resource->vertexBuffer.buffer == VK_NULL_HANDLE) return;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &resource->vertexBuffer.buffer, &resource->foliageInstanceOffset);
    vkCmdDraw(commandBuffer, 18, resource->foliageInstanceCount, 0, 0);
}

void ChunkRenderer::retire(ChunkId id, std::vector<AllocatedBuffer>& retiredBuffers) {
    auto found = resources_.find(id);
    if (found == resources_.end()) return;
    auto retireBuffer = [&](AllocatedBuffer& buffer) {
        if (buffer.buffer != VK_NULL_HANDLE) retiredBuffers.push_back(buffer);
        buffer = {};
    };
    retireBuffer(found->second.vertexBuffer);
    retireBuffer(found->second.waterVertexBuffer);
    retireBuffer(found->second.grassInstanceBuffer);
    retireBuffer(found->second.foliageInstanceBuffer);
    resources_.erase(found);
}

void ChunkRenderer::cleanup(VkDevice device, VmaAllocator allocator, bool deviceAlreadyIdle) {
    if (!deviceAlreadyIdle && !resources_.empty()) vkDeviceWaitIdle(device);
    for (auto& [chunk, resource] : resources_) {
        auto destroy = [&](AllocatedBuffer& buffer) {
            if (buffer.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
            buffer = {};
        };
        destroy(resource.vertexBuffer);
        destroy(resource.waterVertexBuffer);
        destroy(resource.grassInstanceBuffer);
        destroy(resource.foliageInstanceBuffer);
    }
    resources_.clear();
}

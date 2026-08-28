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
    if (resource.materialBuffer != VK_NULL_HANDLE) {
        if (retiredBuffers) retiredBuffers->push_back({resource.materialBuffer, resource.materialAllocation});
        else {
            vkDeviceWaitIdle(device);
            vmaDestroyBuffer(allocator, resource.materialBuffer, resource.materialAllocation);
        }
        resource.materialBuffer = VK_NULL_HANDLE;
        resource.materialAllocation = VK_NULL_HANDLE;
    }

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
    // The mesh snapshot is already resolved from the registry on the worker;
    // retain a stable count for GPU/debug telemetry without re-reading the
    // registry from the render thread.
    resource.materialVariantCount = resource.vertexCount > 0 ? 1u : 0u;
    resource.materialRecordCount = resource.materialVariantCount;
    if (resource.materialRecordCount > 0) {
        VkBufferCreateInfo materialInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        materialInfo.size = sizeof(glm::vec4) * resource.materialRecordCount;
        materialInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        materialInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo materialAllocationInfo{};
        materialAllocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        VK_CHECK(vmaCreateBuffer(allocator, &materialInfo, &materialAllocationInfo,
                                 &resource.materialBuffer, &resource.materialAllocation, nullptr));
        void* materialMapped = nullptr;
        VK_CHECK(vmaMapMemory(allocator, resource.materialAllocation, &materialMapped));
        const glm::vec4 materialRecord(
            static_cast<float>(resource.dynamicMaterialVertexCount),
            static_cast<float>(resource.emissiveVertexCount),
            static_cast<float>(resource.vertexCount), 1.0f);
        std::memcpy(materialMapped, &materialRecord, sizeof(materialRecord));
        VK_CHECK(vmaFlushAllocation(allocator, resource.materialAllocation, 0, sizeof(materialRecord)));
        vmaUnmapMemory(allocator, resource.materialAllocation);
    }
    resource.dynamicMaterialVertexCount = 0u;
    resource.emissiveVertexCount = 0u;
    for (const VoxelVertex& vertex : mesh.meshVertices) {
        if (vertex.uv.z < 0.0f) ++resource.dynamicMaterialVertexCount;
        if (vertex.color.r > 1.0f || vertex.color.g > 1.0f || vertex.color.b > 1.0f)
            ++resource.emissiveVertexCount;
    }
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
    if (found->second.materialBuffer != VK_NULL_HANDLE)
        retiredBuffers.push_back({found->second.materialBuffer, found->second.materialAllocation});
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
        if (resource.materialBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator, resource.materialBuffer, resource.materialAllocation);
        resource.materialBuffer = VK_NULL_HANDLE;
        resource.materialAllocation = VK_NULL_HANDLE;
    }
    resources_.clear();
}

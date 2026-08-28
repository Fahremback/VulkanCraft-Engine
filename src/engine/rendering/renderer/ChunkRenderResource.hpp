#pragma once

#include "VulkanTypes.hpp"

#include <cstdint>

// Renderer-owned representation of one uploaded chunk snapshot.
struct ChunkRenderResource {
    AllocatedBuffer vertexBuffer;
    AllocatedBuffer waterVertexBuffer;
    AllocatedBuffer grassInstanceBuffer;
    AllocatedBuffer foliageInstanceBuffer;
    VkDeviceSize waterVertexOffset{0};
    VkDeviceSize grassInstanceOffset{0};
    VkDeviceSize foliageInstanceOffset{0};
    uint32_t vertexCount{0};
    uint32_t waterVertexCount{0};
    uint32_t grassInstanceCount{0};
    uint32_t foliageInstanceCount{0};
    uint32_t materialVariantCount{0};
    uint32_t dynamicMaterialVertexCount{0};
    uint32_t emissiveVertexCount{0};
    VkBuffer materialBuffer{VK_NULL_HANDLE};
    VmaAllocation materialAllocation{VK_NULL_HANDLE};
    uint32_t materialRecordCount{0};
};

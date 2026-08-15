#pragma once

#include "ChunkRenderResource.hpp"
#include "ChunkMeshResult.hpp"

#include <unordered_map>
#include <vector>

class ChunkRenderer final {
public:
    void upload(ChunkMeshResult result, VkDevice device, VmaAllocator allocator,
                std::vector<AllocatedBuffer>* retiredBuffers = nullptr);
    void draw(ChunkId id, VkCommandBuffer commandBuffer) const;
    void draw_water(ChunkId id, VkCommandBuffer commandBuffer) const;
    void draw_grass(ChunkId id, VkCommandBuffer commandBuffer) const;
    void draw_foliage(ChunkId id, VkCommandBuffer commandBuffer) const;
    void retire(ChunkId id, std::vector<AllocatedBuffer>& retiredBuffers);
    void cleanup(VkDevice device, VmaAllocator allocator, bool deviceAlreadyIdle = false);
    [[nodiscard]] bool has_resource(ChunkId id) const;

private:
    ChunkRenderResource* find(ChunkId id);
    const ChunkRenderResource* find(ChunkId id) const;
    std::unordered_map<ChunkId, ChunkRenderResource, ChunkIdHash> resources_;
};

#pragma once

#include "Frustum.hpp"
#include "VulkanTypes.hpp"
#include "WorldRenderBridge.hpp"
#include "ChunkRenderer.hpp"
#include "FarTerrain.hpp"
#include "ThreadPool.hpp"

class World;

class WorldRenderer final : public WorldRenderBridge {
public:
    explicit WorldRenderer(World& world) : world_(world) {}

    void configure(VkDevice device, VmaAllocator allocator);
    void begin_frame() override;
    void request_far_terrain(int centerChunkX, int centerChunkZ, int reachChunks,
                             float endpointQuality) override;
    void retire_chunk(ChunkId chunk) override;
    void upload_chunk(ChunkMeshResult result) override;
    void cleanup(bool deviceAlreadyIdle = false);

    [[nodiscard]] int represented_reach_chunks() const { return farTerrain_.represented_reach_chunks(); }
    [[nodiscard]] int clipmap_level_count() const { return farTerrain_.clipmap_level_count(); }
    [[nodiscard]] float last_build_milliseconds() const { return farTerrain_.last_build_milliseconds(); }
    [[nodiscard]] float applied_endpoint_percent() const { return farTerrain_.applied_endpoint_percent(); }
    [[nodiscard]] bool is_building() const { return farTerrain_.is_building(); }
    void draw_far_surface_shadow(VkCommandBuffer commandBuffer) { farTerrain_.draw_surface_shadow(commandBuffer); }
    void draw_far_shadow(VkCommandBuffer commandBuffer) { farTerrain_.draw_shadow(commandBuffer); }

    void draw_far_surface(VkCommandBuffer commandBuffer);
    void draw(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_details(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius = 7);
    void draw_foliage_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius = 7);
    void draw_grass_shadow(VkCommandBuffer commandBuffer, const glm::vec3& center, int chunkRadius = 7);
    void draw_grass(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_foliage(VkCommandBuffer commandBuffer, const Frustum& frustum);
    void draw_water(VkCommandBuffer commandBuffer, const Frustum& frustum, const glm::vec3& cameraPosition);

private:
    void draw_details_unlocked(VkCommandBuffer commandBuffer, const Frustum& frustum);
    World& world_;
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    ChunkRenderer chunkRenderer_;
    FarTerrain farTerrain_;
    ThreadPool farTerrainThreadPool_{1};
    std::vector<AllocatedBuffer> retiredBuffers_[FRAME_OVERLAP];
    uint64_t gpuEpoch_{0};
};

#pragma once

#include "VulkanTypes.hpp"

#include "Chunk.hpp"
#include "ThreadPool.hpp"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

// One disposable, render-only representation for the whole distant world.
// It deliberately owns no voxels, entities, fluids or gameplay state.  The
// procedural generator remains the source of truth and detailed Chunk objects
// replace this surface close to the player.
struct alignas(16) FarSurfaceInstance {
    glm::vec4 positionSize;    // world x, top y, world z, cell size
    glm::vec4 neighborHeights; // west, east, south, north
    glm::vec4 color;           // alpha=2 marks render-only FAR geometry
    glm::vec4 material;        // top layer, side layer, lod mode, north-east top

    static VkVertexInputBindingDescription binding_description(uint32_t binding = 0) {
        return { binding, sizeof(FarSurfaceInstance), VK_VERTEX_INPUT_RATE_INSTANCE };
    }
    static std::array<VkVertexInputAttributeDescription, 4>
    attribute_descriptions(uint32_t binding = 0) {
        return {{
            { 0, binding, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FarSurfaceInstance, positionSize) },
            { 1, binding, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FarSurfaceInstance, neighborHeights) },
            { 2, binding, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FarSurfaceInstance, color) },
            { 3, binding, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FarSurfaceInstance, material) }
        }};
    }
};
static_assert(sizeof(FarSurfaceInstance) == 64);

class FarTerrain {
public:
    struct BuildResult {
        uint64_t version{ 0 };
        int centerChunkX{ 0 };
        int centerChunkZ{ 0 };
        int reachChunks{ 0 };
        int clipmapLevels{ 0 };
        float endpointQualityFraction{ 0.0f };
        uint64_t buildMicroseconds{ 0 };
        std::vector<FarSurfaceInstance> surfaceInstances;
        uint32_t nearSurfaceInstanceCount{ 0 };
        uint32_t farSurfaceInstanceCount{ 0 };
        uint32_t shadowSurfaceInstanceCount{ 0 };
        std::vector<VoxelVertex> terrainVertices;
        std::vector<VoxelVertex> waterVertices;
        std::vector<std::array<uint32_t, 2>> shadowDrawRanges; // first vertex, count
    };

    void request(ThreadPool& pool, int centerChunkX, int centerChunkZ,
                 int reachChunks, float endpointQualityFraction);
    void upload_ready(VkDevice device, VmaAllocator allocator,
                      std::vector<AllocatedBuffer>* retiredBuffers);
    void draw(VkCommandBuffer cmd) const;
    void draw_near_surface(VkCommandBuffer cmd) const;
    void draw_far_surface(VkCommandBuffer cmd) const;
    void draw_water(VkCommandBuffer cmd) const;
    void draw_surface_shadow(VkCommandBuffer cmd) const;
    void draw_shadow(VkCommandBuffer cmd) const;
    void cleanup(VkDevice device, VmaAllocator allocator, bool deviceAlreadyIdle = false);

    [[nodiscard]] bool is_building() const { return building.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t terrain_vertex_count() const { return terrainVertexCount; }
    [[nodiscard]] uint32_t water_vertex_count() const { return waterVertexCount; }
    [[nodiscard]] uint32_t near_surface_instance_count() const { return nearSurfaceInstanceCount; }
    [[nodiscard]] uint32_t far_surface_instance_count() const { return farSurfaceInstanceCount; }
    [[nodiscard]] int represented_reach_chunks() const {
        return representedReachChunks.load(std::memory_order_acquire);
    }
    [[nodiscard]] int clipmap_level_count() const {
        return publishedClipmapLevels.load(std::memory_order_acquire);
    }
    [[nodiscard]] double last_build_milliseconds() const {
        return static_cast<double>(lastBuildMicroseconds.load(std::memory_order_acquire)) / 1000.0;
    }
    [[nodiscard]] float applied_endpoint_percent() const {
        return publishedEndpointQuality.load(std::memory_order_acquire) * 100.0f;
    }

private:
    static BuildResult build(uint64_t version, int centerChunkX, int centerChunkZ,
                             int reachChunks, float endpointQualityFraction,
                             const std::atomic_uint64_t* latestRequestedVersion);

    std::mutex requestMutex;
    int requestedCenterChunkX{ 0 };
    int requestedCenterChunkZ{ 0 };
    int requestedBudget{ -1 };
    float requestedEndpointQuality{ 0.001f }; // 0.1 percent at maximum reach
    uint64_t requestedVersion{ 0 };
    std::atomic_uint64_t latestRequestedVersion{ 0 };
    uint64_t uploadedVersion{ 0 };

    std::atomic_bool building{ false };
    std::mutex readyMutex;
    std::optional<BuildResult> readyResult;

    AllocatedBuffer terrainBuffer;
    AllocatedBuffer surfaceBuffer;
    AllocatedBuffer waterBuffer;
    uint32_t terrainVertexCount{ 0 };
    uint32_t nearSurfaceInstanceCount{ 0 };
    uint32_t farSurfaceInstanceCount{ 0 };
    uint32_t shadowSurfaceInstanceCount{ 0 };
    VkDeviceSize farSurfaceBufferOffset{ 0 };
    uint32_t waterVertexCount{ 0 };
    std::vector<std::array<uint32_t, 2>> shadowDrawRanges;
    std::atomic_int representedReachChunks{ 0 };
    std::atomic_int publishedClipmapLevels{ 0 };
    std::atomic_uint64_t lastBuildMicroseconds{ 0 };
    std::atomic<float> publishedEndpointQuality{ 0.0f };
};

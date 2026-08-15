#pragma once

#include "VulkanTypes.hpp"
#include "RadianceCacheMath.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Lightweight, camera-relative diffuse-radiance cache.  The cache is a set of
// toroidal 3D probe clipmaps: moving the camera only invalidates newly exposed
// probe slabs instead of rebuilding a world-sized light volume.
class RadianceCache {
public:
    static constexpr uint32_t MaxCascades = 6;
    static constexpr VkDeviceSize ProbeDataOffset = 32 + MaxCascades * 32;

    struct Config {
        uint32_t cascadeCount{ MaxCascades };
        uint32_t resolution{ 16 };
        uint32_t probesPerFrame{ 192 };
        float baseSpacing{ 4.0f };
        float cascadeScale{ 4.0f };
        float sunRefreshAngleDegrees{ 2.0f };
    };

    // std430-compatible. The slot is valid only when worldCellCascade matches
    // the requested cell/cascade, which makes the toroidal cache race-safe.
    struct alignas(16) ProbeGpu {
        glm::vec4 radianceVisibility{ 0.0f }; // RGB irradiance, A sky visibility
        glm::vec4 directionConfidence{ 0.0f, 1.0f, 0.0f, 0.0f }; // bent direction, confidence
        glm::ivec4 worldCellCascade{
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::min(), -1
        };
    };

    struct alignas(16) CascadeGpu {
        glm::ivec4 minCellResolution{ 0 }; // xyz inclusive minimum, w resolution
        glm::vec4 spacingBase{ 0.0f };     // x spacing, y probe base, z extent, w inverse spacing
    };

    struct alignas(16) MetadataGpu {
        glm::uvec4 counts{ 0 }; // cascades, resolution, total probes, revision
        glm::vec4 sunDirection{ 0.0f, 1.0f, 0.0f, 0.0f };
        std::array<CascadeGpu, MaxCascades> cascades{};
    };

    // Reservoir storage is deliberately optional. It is the ABI needed by a
    // later ReSTIR candidate/reuse pass; this cache does not pretend that merely
    // allocating reservoirs is a complete ReSTIR GI implementation.
    struct alignas(16) ReservoirGpu {
        glm::vec4 sampleRadianceWeight{ 0.0f }; // rgb sample radiance, weight sum
        glm::vec4 sampleDirectionM{ 0.0f };     // xyz direction, accepted sample count
        glm::uvec4 sourceAndAge{ 0u };          // source id/seed, age, flags, padding
    };

    RadianceCache() = default;
    RadianceCache(const RadianceCache&) = delete;
    RadianceCache& operator=(const RadianceCache&) = delete;
    ~RadianceCache() = default;

    void init(VkDevice device, VmaAllocator allocator, const Config& config = {});
    void cleanup();

    // Updates camera origins, schedules only missing toroidal cells, and spends
    // a bounded CPU budget evaluating terrain/sky probes. Returns regenerated
    // probe count. GPU uploads are intentionally recorded separately.
    uint32_t update(const glm::vec3& cameraPosition,
                    const glm::vec3& sunDirection,
                    const glm::vec3& sunColor,
                    uint32_t probeBudgetOverride = 0);

    // Record this before lighting shaders consume the buffers. It emits a small
    // number of transfer copies and the required transfer -> shader barriers.
    void record_uploads(VkCommandBuffer commandBuffer);

    [[nodiscard]] bool initialized() const { return device_ != VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t pending_probe_count() const;
    [[nodiscard]] uint32_t total_probe_count() const { return metadataCpu_.counts.z; }
    [[nodiscard]] const MetadataGpu& metadata_cpu() const { return metadataCpu_; }

    [[nodiscard]] VkDescriptorBufferInfo buffer_info() const;

    static VkDescriptorSetLayoutBinding
    descriptor_binding(uint32_t binding = 6,
                       VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
    void write_descriptor(VkDescriptorSet set, uint32_t binding = 6) const;

private:
    struct GpuBuffer {
        VkBuffer buffer{ VK_NULL_HANDLE };
        VmaAllocation allocation{ VK_NULL_HANDLE };
        VkDeviceSize size{ 0 };
        void* mapped{ nullptr };
    };

    struct PendingCell {
        glm::ivec3 cell{ 0 };
    };

    struct CascadeState {
        glm::ivec3 minCell{ std::numeric_limits<int32_t>::max() };
        uint32_t baseProbe{ 0 };
        uint32_t sunRevision{ 0 };
        uint32_t pendingSunRevision{ 0 };
        std::vector<PendingCell> pending;
        size_t nextPending{ 0 };
    };

    Config config_{};
    VkDevice device_{ VK_NULL_HANDLE };
    VmaAllocator allocator_{ VK_NULL_HANDLE };
    GpuBuffer cacheBuffer_{};
    GpuBuffer stagingBuffer_{};

    MetadataGpu metadataCpu_{};
    std::vector<ProbeGpu> probesCpu_;
    std::array<CascadeState, MaxCascades> cascades_{};
    std::array<uint32_t, MaxCascades> dirtyMin_{};
    std::array<uint32_t, MaxCascades> dirtyMax_{};
    glm::vec3 cachedSunDirection_{ 0.0f, 1.0f, 0.0f };
    glm::vec3 cachedSunColor_{ 1.0f };
    uint32_t sunRevision_{ 1 };
    bool metadataDirty_{ true };

    void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags flags,
                       GpuBuffer& output);
    void destroy_buffer(GpuBuffer& buffer);
    void rebuild_pending(uint32_t cascadeIndex, bool includeSunStale);
    bool contains_cell(uint32_t cascadeIndex, const glm::ivec3& cell) const;
    uint32_t slot_index(uint32_t cascadeIndex, const glm::ivec3& cell) const;
    ProbeGpu evaluate_probe(uint32_t cascadeIndex, const glm::ivec3& cell,
                            const glm::vec3& sunDirection, const glm::vec3& sunColor) const;
    void mark_dirty(uint32_t cascadeIndex, uint32_t globalSlot);
};

static_assert(sizeof(RadianceCache::ProbeGpu) == 48);
static_assert(offsetof(RadianceCache::ProbeGpu, radianceVisibility) == 0);
static_assert(offsetof(RadianceCache::ProbeGpu, directionConfidence) == 16);
static_assert(offsetof(RadianceCache::ProbeGpu, worldCellCascade) == 32);
static_assert(sizeof(RadianceCache::CascadeGpu) == 32);
static_assert(sizeof(RadianceCache::MetadataGpu) == 32 + RadianceCache::MaxCascades * 32);
static_assert(offsetof(RadianceCache::MetadataGpu, counts) == 0);
static_assert(offsetof(RadianceCache::MetadataGpu, sunDirection) == 16);
static_assert(offsetof(RadianceCache::MetadataGpu, cascades) == 32);
static_assert(RadianceCache::ProbeDataOffset == sizeof(RadianceCache::MetadataGpu));
static_assert(sizeof(RadianceCache::ReservoirGpu) == 48);

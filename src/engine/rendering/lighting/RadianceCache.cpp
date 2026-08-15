#include "RadianceCache.hpp"

#include "TerrainGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

constexpr uint32_t kInvalidDirtyMin = std::numeric_limits<uint32_t>::max();

glm::vec3 biome_albedo(BiomeType biome) {
    switch (biome) {
        case BiomeType::DeepOcean:     return { 0.025f, 0.075f, 0.13f };
        case BiomeType::Ocean:         return { 0.035f, 0.12f, 0.20f };
        case BiomeType::Coast:         return { 0.43f, 0.37f, 0.23f };
        case BiomeType::River:         return { 0.04f, 0.14f, 0.20f };
        case BiomeType::Desert:
        case BiomeType::DesertOasis:   return { 0.52f, 0.40f, 0.20f };
        case BiomeType::Badlands:      return { 0.42f, 0.18f, 0.09f };
        case BiomeType::Alpine:
        case BiomeType::Glacial:       return { 0.62f, 0.68f, 0.69f };
        case BiomeType::RockyMountains:return { 0.26f, 0.27f, 0.25f };
        case BiomeType::Volcanic:
        case BiomeType::VolcanicCrater:return { 0.075f, 0.07f, 0.065f };
        case BiomeType::Swamp:         return { 0.10f, 0.17f, 0.08f };
        case BiomeType::Savanna:       return { 0.33f, 0.34f, 0.13f };
        default:                       return { 0.11f, 0.25f, 0.085f };
    }
}

glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback) {
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared > 1.0e-8f ? value * glm::inversesqrt(lengthSquared) : fallback;
}

} // namespace

void RadianceCache::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags flags,
                                  GpuBuffer& output) {
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = memoryUsage;
    allocationInfo.flags = flags;

    VmaAllocationInfo resultInfo{};
    VK_CHECK(vmaCreateBuffer(allocator_, &bufferInfo, &allocationInfo,
                            &output.buffer, &output.allocation, &resultInfo));
    output.size = size;
    output.mapped = resultInfo.pMappedData;
}

void RadianceCache::destroy_buffer(GpuBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void RadianceCache::init(VkDevice device, VmaAllocator allocator, const Config& requestedConfig) {
    if (initialized()) cleanup();
    if (device == VK_NULL_HANDLE || allocator == VK_NULL_HANDLE) {
        throw std::invalid_argument("RadianceCache requires a valid Vulkan device and VMA allocator");
    }

    config_ = requestedConfig;
    config_.cascadeCount = std::clamp(config_.cascadeCount, 1u, MaxCascades);
    config_.resolution = std::clamp(config_.resolution, 4u, 32u);
    config_.probesPerFrame = std::max(config_.probesPerFrame, 1u);
    config_.baseSpacing = std::max(config_.baseSpacing, 0.5f);
    config_.cascadeScale = std::max(config_.cascadeScale, 2.0f);
    config_.sunRefreshAngleDegrees = std::clamp(config_.sunRefreshAngleDegrees, 0.25f, 15.0f);
    device_ = device;
    allocator_ = allocator;

    const uint64_t probesPerCascade = static_cast<uint64_t>(config_.resolution) *
                                      config_.resolution * config_.resolution;
    const uint64_t totalProbes = probesPerCascade * config_.cascadeCount;
    if (totalProbes > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("RadianceCache probe count exceeds 32-bit indexing");
    }

    metadataCpu_ = {};
    metadataCpu_.counts = glm::uvec4(config_.cascadeCount, config_.resolution,
                                    static_cast<uint32_t>(totalProbes), 1u);
    probesCpu_.assign(static_cast<size_t>(totalProbes), ProbeGpu{});

    for (uint32_t cascade = 0; cascade < MaxCascades; ++cascade) {
        dirtyMin_[cascade] = kInvalidDirtyMin;
        dirtyMax_[cascade] = 0;
        cascades_[cascade] = {};
        if (cascade < config_.cascadeCount) {
            cascades_[cascade].baseProbe = static_cast<uint32_t>(probesPerCascade * cascade);
            const float spacing = config_.baseSpacing * std::pow(config_.cascadeScale, static_cast<float>(cascade));
            metadataCpu_.cascades[cascade].spacingBase = glm::vec4(
                spacing, static_cast<float>(cascades_[cascade].baseProbe),
                spacing * static_cast<float>(config_.resolution), 1.0f / spacing);
        }
    }

    const VkDeviceSize metadataBytes = sizeof(MetadataGpu);
    const VkDeviceSize probeBytes = sizeof(ProbeGpu) * totalProbes;
    const VkDeviceSize cacheBytes = ProbeDataOffset + probeBytes;
    create_buffer(cacheBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, cacheBuffer_);
    const auto mappedFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;
    create_buffer(cacheBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VMA_MEMORY_USAGE_AUTO_PREFER_HOST, mappedFlags, stagingBuffer_);

    if (!stagingBuffer_.mapped) {
        cleanup();
        throw std::runtime_error("RadianceCache staging allocations are not host mapped");
    }

    std::memcpy(stagingBuffer_.mapped, &metadataCpu_, sizeof(metadataCpu_));
    std::memcpy(static_cast<std::byte*>(stagingBuffer_.mapped) + ProbeDataOffset,
                probesCpu_.data(), static_cast<size_t>(probeBytes));
    vmaFlushAllocation(allocator_, stagingBuffer_.allocation, 0, cacheBytes);
    metadataDirty_ = true;
    for (uint32_t cascade = 0; cascade < config_.cascadeCount; ++cascade) {
        dirtyMin_[cascade] = cascades_[cascade].baseProbe;
        dirtyMax_[cascade] = cascades_[cascade].baseProbe + static_cast<uint32_t>(probesPerCascade) - 1;
    }
}

void RadianceCache::cleanup() {
    if (allocator_ != VK_NULL_HANDLE) {
        destroy_buffer(stagingBuffer_);
        destroy_buffer(cacheBuffer_);
    }
    probesCpu_.clear();
    metadataCpu_ = {};
    cascades_ = {};
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
    metadataDirty_ = true;
}

bool RadianceCache::contains_cell(uint32_t cascadeIndex, const glm::ivec3& cell) const {
    const glm::ivec3 local = cell - cascades_[cascadeIndex].minCell;
    const int resolution = static_cast<int>(config_.resolution);
    return local.x >= 0 && local.y >= 0 && local.z >= 0 &&
           local.x < resolution && local.y < resolution && local.z < resolution;
}

uint32_t RadianceCache::slot_index(uint32_t cascadeIndex, const glm::ivec3& cell) const {
    return cascades_[cascadeIndex].baseProbe +
           radiance_cache_math::toroidal_local_index(cell, config_.resolution);
}

void RadianceCache::rebuild_pending(uint32_t cascadeIndex, bool includeSunStale) {
    CascadeState& cascade = cascades_[cascadeIndex];
    cascade.pending.clear();
    cascade.nextPending = 0;
    const int resolution = static_cast<int>(config_.resolution);
    cascade.pending.reserve(static_cast<size_t>(resolution) * resolution * resolution);

    for (int z = 0; z < resolution; ++z) {
        for (int y = 0; y < resolution; ++y) {
            for (int x = 0; x < resolution; ++x) {
                const glm::ivec3 cell = cascade.minCell + glm::ivec3(x, y, z);
                const ProbeGpu& probe = probesCpu_[slot_index(cascadeIndex, cell)];
                const bool wrongCell = glm::any(glm::notEqual(glm::ivec3(probe.worldCellCascade), cell)) ||
                                       probe.worldCellCascade.w != static_cast<int>(cascadeIndex);
                if (wrongCell || includeSunStale) cascade.pending.push_back({ cell });
            }
        }
    }
    // Commit the revision only after the complete traversal finishes. A
    // camera scroll may interrupt it and replace the queue with exposed slabs.
    cascade.pendingSunRevision = includeSunStale ? sunRevision_ : 0u;
}

RadianceCache::ProbeGpu RadianceCache::evaluate_probe(uint32_t cascadeIndex,
                                                       const glm::ivec3& cell,
                                                       const glm::vec3& sunDirection,
                                                       const glm::vec3& sunColor) const {
    const float spacing = metadataCpu_.cascades[cascadeIndex].spacingBase.x;
    const glm::vec3 position = (glm::vec3(cell) + 0.5f) * spacing;
    const TerrainSample center = TerrainGenerator::sample_coarse(position.x, position.z);
    const float surfaceY = static_cast<float>(center.height) + 1.0f;
    const float altitude = position.y - surfaceY;
    const glm::vec3 ground = biome_albedo(center.biome);
    const glm::vec3 sun = safe_normalize(sunDirection, glm::vec3(0.0f, 1.0f, 0.0f));

    ProbeGpu result{};
    result.worldCellCascade = glm::ivec4(cell, static_cast<int>(cascadeIndex));

    if (altitude < -0.35f * spacing) {
        const float depth = std::min(-altitude / std::max(spacing * 2.0f, 1.0f), 1.0f);
        result.radianceVisibility = glm::vec4(ground * glm::mix(0.035f, 0.008f, depth), 0.015f);
        result.directionConfidence = glm::vec4(0.0f, 1.0f, 0.0f, 0.25f);
        return result;
    }

    const bool nearSurface = altitude < spacing * 8.0f;
    float skyVisibility = std::clamp(0.50f + altitude / std::max(spacing * 4.0f, 1.0f), 0.12f, 1.0f);
    float directVisibility = sun.y > 0.015f ? 1.0f : 0.0f;
    glm::vec3 bentNormal(0.0f, 1.0f, 0.0f);

    if (nearSurface) {
        const float sampleDistance = std::max(spacing * 2.0f, 2.0f);
        const float hx0 = static_cast<float>(TerrainGenerator::sample_coarse(position.x - sampleDistance, position.z).height);
        const float hx1 = static_cast<float>(TerrainGenerator::sample_coarse(position.x + sampleDistance, position.z).height);
        const float hz0 = static_cast<float>(TerrainGenerator::sample_coarse(position.x, position.z - sampleDistance).height);
        const float hz1 = static_cast<float>(TerrainGenerator::sample_coarse(position.x, position.z + sampleDistance).height);
        bentNormal = safe_normalize(glm::vec3(hx0 - hx1, sampleDistance * 2.0f, hz0 - hz1),
                                    glm::vec3(0.0f, 1.0f, 0.0f));

        const float maxNeighbor = std::max(std::max(hx0, hx1), std::max(hz0, hz1));
        const float localHorizon = std::clamp((maxNeighbor - position.y) / sampleDistance, 0.0f, 1.0f);
        skyVisibility *= 1.0f - localHorizon * 0.62f;

        if (directVisibility > 0.0f) {
            const glm::vec2 horizontal(sun.x, sun.z);
            const float horizontalLength = glm::length(horizontal);
            if (horizontalLength > 1.0e-4f) {
                const glm::vec2 rayDirection = horizontal / horizontalLength;
                constexpr std::array<float, 4> raySteps{ 2.0f, 5.0f, 11.0f, 23.0f };
                for (float stepScale : raySteps) {
                    const float horizontalTravel = spacing * stepScale;
                    const float rayY = position.y + horizontalTravel * (sun.y / horizontalLength);
                    const glm::vec2 xz = glm::vec2(position.x, position.z) + rayDirection * horizontalTravel;
                    if (static_cast<float>(TerrainGenerator::sample_coarse(xz.x, xz.y).height) + 1.0f > rayY) {
                        directVisibility = 0.0f;
                        break;
                    }
                }
            }
        }
    }

    const float day = std::clamp(sun.y * 4.0f + 0.12f, 0.025f, 1.0f);
    const glm::vec3 skyColor = glm::mix(glm::vec3(0.008f, 0.012f, 0.03f),
                                       glm::vec3(0.16f, 0.29f, 0.48f), day);
    const float groundBounce = std::clamp(glm::dot(bentNormal, glm::vec3(0.0f, 1.0f, 0.0f)), 0.0f, 1.0f);
    const glm::vec3 ambient = skyColor * skyVisibility + ground * (0.055f + 0.10f * groundBounce) * day;
    const float sunLambert = std::max(glm::dot(bentNormal, sun), 0.0f);
    // The regular shadowed sun pass owns direct lighting.  The cache stores
    // only low-frequency sky and first-bounce terrain radiance, preventing
    // double sun energy when it is sampled by the material shader.
    const glm::vec3 sunBounce = ground * sunColor * directVisibility *
                                sunLambert * day * 0.16f;
    result.radianceVisibility = glm::vec4(ambient + sunBounce, skyVisibility);
    result.directionConfidence = glm::vec4(safe_normalize(bentNormal + sun * directVisibility * 0.35f,
                                                           glm::vec3(0.0f, 1.0f, 0.0f)),
                                            directVisibility);
    return result;
}

void RadianceCache::mark_dirty(uint32_t cascadeIndex, uint32_t globalSlot) {
    dirtyMin_[cascadeIndex] = std::min(dirtyMin_[cascadeIndex], globalSlot);
    dirtyMax_[cascadeIndex] = std::max(dirtyMax_[cascadeIndex], globalSlot);
}

uint32_t RadianceCache::update(const glm::vec3& cameraPosition,
                               const glm::vec3& sunDirection,
                               const glm::vec3& sunColor,
                               uint32_t probeBudgetOverride) {
    if (!initialized()) return 0;

    const glm::vec3 normalizedSun = safe_normalize(sunDirection, cachedSunDirection_);
    const float cosineThreshold = std::cos(glm::radians(config_.sunRefreshAngleDegrees));
    const bool sunChanged = glm::dot(normalizedSun, cachedSunDirection_) < cosineThreshold ||
                            glm::length(sunColor - cachedSunColor_) > 0.08f;
    if (sunChanged) {
        cachedSunDirection_ = normalizedSun;
        cachedSunColor_ = sunColor;
        ++sunRevision_;
        metadataCpu_.counts.w = sunRevision_;
        metadataCpu_.sunDirection = glm::vec4(normalizedSun, 0.0f);
        metadataDirty_ = true;
    }

    for (uint32_t cascade = 0; cascade < config_.cascadeCount; ++cascade) {
        const float inverseSpacing = metadataCpu_.cascades[cascade].spacingBase.w;
        const glm::ivec3 newMin = radiance_cache_math::clipmap_min_cell(
            cameraPosition, inverseSpacing, config_.resolution);
        const bool moved = glm::any(glm::notEqual(newMin, cascades_[cascade].minCell));
        if (moved) {
            cascades_[cascade].minCell = newMin;
            metadataCpu_.cascades[cascade].minCellResolution = glm::ivec4(newMin, config_.resolution);
            metadataDirty_ = true;
        }
        if (moved) {
            rebuild_pending(cascade, false);
        }
        // Never restart an in-flight traversal when the sun advances. The old
        // code reset nextPending to zero every few seconds, starving the tail
        // of the outer clipmaps forever. Finish bounded work, then enqueue the
        // latest revision once.
        CascadeState& state = cascades_[cascade];
        const bool hasPending = state.nextPending < state.pending.size();
        if (!hasPending && state.sunRevision != sunRevision_) {
            rebuild_pending(cascade, true);
        }
    }

    const uint32_t budget = probeBudgetOverride > 0 ? probeBudgetOverride : config_.probesPerFrame;
    static constexpr std::array<uint32_t, MaxCascades> weights{ 40, 24, 14, 10, 7, 5 };
    uint32_t generated = 0;
    uint32_t remaining = budget;
    const auto finish_pending = [](CascadeState& state) {
        if (state.nextPending < state.pending.size()) return;
        if (state.pendingSunRevision != 0u) {
            state.sunRevision = state.pendingSunRevision;
            state.pendingSunRevision = 0u;
        }
        state.pending.clear();
        state.nextPending = 0;
    };

    // Fixed shares keep distant clipmaps alive even while the near cache scrolls.
    for (uint32_t cascade = 0; cascade < config_.cascadeCount && remaining > 0; ++cascade) {
        CascadeState& state = cascades_[cascade];
        const uint32_t share = cascade + 1 == config_.cascadeCount
            ? remaining
            : std::max(1u, budget * weights[cascade] / 100u);
        uint32_t spent = 0;
        while (state.nextPending < state.pending.size() && spent < share && remaining > 0) {
            const glm::ivec3 cell = state.pending[state.nextPending++].cell;
            if (!contains_cell(cascade, cell)) continue;
            const uint32_t slot = slot_index(cascade, cell);
            probesCpu_[slot] = evaluate_probe(cascade, cell, normalizedSun, sunColor);
            std::memcpy(static_cast<std::byte*>(stagingBuffer_.mapped) + ProbeDataOffset + slot * sizeof(ProbeGpu),
                        &probesCpu_[slot], sizeof(ProbeGpu));
            mark_dirty(cascade, slot);
            ++spent;
            ++generated;
            --remaining;
        }
        finish_pending(state);
    }

    // Redistribute unused shares from already complete cascades.
    while (remaining > 0) {
        bool progressed = false;
        for (uint32_t cascade = 0; cascade < config_.cascadeCount && remaining > 0; ++cascade) {
            CascadeState& state = cascades_[cascade];
            while (state.nextPending < state.pending.size()) {
                const glm::ivec3 cell = state.pending[state.nextPending++].cell;
                if (!contains_cell(cascade, cell)) continue;
                const uint32_t slot = slot_index(cascade, cell);
                probesCpu_[slot] = evaluate_probe(cascade, cell, normalizedSun, sunColor);
                std::memcpy(static_cast<std::byte*>(stagingBuffer_.mapped) + ProbeDataOffset + slot * sizeof(ProbeGpu),
                            &probesCpu_[slot], sizeof(ProbeGpu));
                mark_dirty(cascade, slot);
                --remaining;
                ++generated;
                progressed = true;
                break;
            }
            finish_pending(state);
        }
        if (!progressed) break;
    }

    if (metadataDirty_) {
        std::memcpy(stagingBuffer_.mapped, &metadataCpu_, sizeof(metadataCpu_));
    }
    return generated;
}

void RadianceCache::record_uploads(VkCommandBuffer commandBuffer) {
    if (!initialized() || commandBuffer == VK_NULL_HANDLE) return;

    bool probesDirty = false;
    for (uint32_t cascade = 0; cascade < config_.cascadeCount; ++cascade) {
        probesDirty |= dirtyMin_[cascade] != kInvalidDirtyMin;
    }
    if (!metadataDirty_ && !probesDirty) return;

    VkBufferMemoryBarrier before{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = cacheBuffer_.buffer;
    before.offset = 0;
    before.size = cacheBuffer_.size;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 1, &before, 0, nullptr);

    if (metadataDirty_) {
        vmaFlushAllocation(allocator_, stagingBuffer_.allocation, 0, sizeof(MetadataGpu));
        VkBufferCopy copy{ 0, 0, sizeof(MetadataGpu) };
        vkCmdCopyBuffer(commandBuffer, stagingBuffer_.buffer, cacheBuffer_.buffer, 1, &copy);
    }
    if (probesDirty) {
        for (uint32_t cascade = 0; cascade < config_.cascadeCount; ++cascade) {
            if (dirtyMin_[cascade] == kInvalidDirtyMin) continue;
            const VkDeviceSize offset = ProbeDataOffset + static_cast<VkDeviceSize>(dirtyMin_[cascade]) * sizeof(ProbeGpu);
            const VkDeviceSize size = static_cast<VkDeviceSize>(dirtyMax_[cascade] - dirtyMin_[cascade] + 1) * sizeof(ProbeGpu);
            vmaFlushAllocation(allocator_, stagingBuffer_.allocation, offset, size);
            VkBufferCopy copy{ offset, offset, size };
            vkCmdCopyBuffer(commandBuffer, stagingBuffer_.buffer, cacheBuffer_.buffer, 1, &copy);
        }
    }

    VkBufferMemoryBarrier after{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.buffer = cacheBuffer_.buffer;
    after.offset = 0;
    after.size = cacheBuffer_.size;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &after, 0, nullptr);

    metadataDirty_ = false;
    for (uint32_t cascade = 0; cascade < config_.cascadeCount; ++cascade) {
        dirtyMin_[cascade] = kInvalidDirtyMin;
        dirtyMax_[cascade] = 0;
    }
}

uint32_t RadianceCache::pending_probe_count() const {
    uint64_t total = 0;
    for (uint32_t cascade = 0; cascade < config_.cascadeCount; ++cascade) {
        const CascadeState& state = cascades_[cascade];
        total += state.pending.size() - std::min(state.nextPending, state.pending.size());
    }
    return static_cast<uint32_t>(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

VkDescriptorBufferInfo RadianceCache::buffer_info() const {
    return VkDescriptorBufferInfo{ cacheBuffer_.buffer, 0, cacheBuffer_.size };
}

VkDescriptorSetLayoutBinding RadianceCache::descriptor_binding(uint32_t binding,
                                                               VkShaderStageFlags stages) {
    return VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr };
}

void RadianceCache::write_descriptor(VkDescriptorSet set, uint32_t binding) const {
    if (!initialized() || set == VK_NULL_HANDLE) return;
    const VkDescriptorBufferInfo info = buffer_info();
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

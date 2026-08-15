#include "RenderingFoundation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>

namespace Engine::Rendering {
namespace {

bool finite_vec3(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool nonnegative_color(const glm::vec3& value) noexcept {
    return finite_vec3(value) && value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f;
}

uint32_t round_tile(uint32_t requested, uint32_t minimum) noexcept {
    uint32_t value = std::max(requested, minimum);
    if (value <= 1) return 1;
    --value;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    return value == std::numeric_limits<uint32_t>::max() ? value : value + 1;
}

} // namespace

bool DirectionalLight::valid() const noexcept {
    return finite_vec3(direction) && glm::dot(direction, direction) > 0.000001f && nonnegative_color(color) &&
           std::isfinite(intensityLux) && intensityLux >= 0.0f && std::isfinite(indirectInfluence) && indirectInfluence >= 0.0f;
}

void DirectionalLight::sanitize() noexcept {
    direction = finite_vec3(direction) && glm::dot(direction, direction) > 0.000001f
        ? glm::normalize(direction) : glm::vec3(0.0f, -1.0f, 0.0f);
    if (!finite_vec3(color)) color = glm::vec3(1.0f);
    color = glm::max(color, glm::vec3(0.0f));
    if (!std::isfinite(intensityLux)) intensityLux = 0.0f;
    intensityLux = std::max(0.0f, intensityLux);
    if (!std::isfinite(indirectInfluence)) indirectInfluence = 0.0f;
    indirectInfluence = std::max(0.0f, indirectInfluence);
    shadow.resolution = std::max(1u, shadow.resolution);
    shadow.bias = std::max(0.0f, std::isfinite(shadow.bias) ? shadow.bias : 0.0f);
    shadow.normalBias = std::max(0.0f, std::isfinite(shadow.normalBias) ? shadow.normalBias : 0.0f);
}

bool PointLight::valid() const noexcept {
    return finite_vec3(position) && nonnegative_color(color) && std::isfinite(intensityLumens) && intensityLumens >= 0.0f &&
           std::isfinite(range) && range > 0.0f && std::isfinite(indirectInfluence) && indirectInfluence >= 0.0f;
}

float PointLight::attenuation(float distance) const noexcept {
    if (!std::isfinite(distance) || distance < 0.0f || range <= 0.0f || distance >= range) return 0.0f;
    const float normalized = distance / range;
    const float smooth = std::max(0.0f, 1.0f - normalized * normalized * normalized * normalized);
    return (smooth * smooth) / std::max(distance * distance, 0.01f);
}

void PointLight::sanitize() noexcept {
    if (!finite_vec3(position)) position = glm::vec3(0.0f);
    if (!finite_vec3(color)) color = glm::vec3(1.0f);
    color = glm::max(color, glm::vec3(0.0f));
    intensityLumens = std::max(0.0f, std::isfinite(intensityLumens) ? intensityLumens : 0.0f);
    range = std::max(0.001f, std::isfinite(range) ? range : 0.001f);
    indirectInfluence = std::max(0.0f, std::isfinite(indirectInfluence) ? indirectInfluence : 0.0f);
    shadow.resolution = std::max(1u, shadow.resolution);
    shadow.bias = std::max(0.0f, std::isfinite(shadow.bias) ? shadow.bias : 0.0f);
    shadow.normalBias = std::max(0.0f, std::isfinite(shadow.normalBias) ? shadow.normalBias : 0.0f);
}

bool SpotLight::valid() const noexcept {
    return finite_vec3(position) && finite_vec3(direction) && glm::dot(direction, direction) > 0.000001f &&
           nonnegative_color(color) && std::isfinite(intensityLumens) && intensityLumens >= 0.0f &&
           std::isfinite(range) && range > 0.0f && std::isfinite(innerAngleDegrees) &&
           std::isfinite(outerAngleDegrees) && innerAngleDegrees >= 0.0f &&
           outerAngleDegrees > innerAngleDegrees && outerAngleDegrees < 90.0f;
}

float SpotLight::attenuation(const glm::vec3& samplePosition) const noexcept {
    const glm::vec3 offset = samplePosition - position;
    const float distance = glm::length(offset);
    if (!std::isfinite(distance) || distance <= 0.00001f || distance >= range) return 0.0f;
    const glm::vec3 normalizedDirection = glm::normalize(direction);
    const float cosine = glm::dot(glm::normalize(offset), normalizedDirection);
    const float inner = std::cos(glm::radians(innerAngleDegrees));
    const float outer = std::cos(glm::radians(outerAngleDegrees));
    const float cone = std::clamp((cosine - outer) / std::max(inner - outer, 0.00001f), 0.0f, 1.0f);
    PointLight radial;
    radial.range = range;
    return radial.attenuation(distance) * cone * cone;
}

void SpotLight::sanitize() noexcept {
    if (!finite_vec3(position)) position = glm::vec3(0.0f);
    direction = finite_vec3(direction) && glm::dot(direction, direction) > 0.000001f
        ? glm::normalize(direction) : glm::vec3(0.0f, -1.0f, 0.0f);
    if (!finite_vec3(color)) color = glm::vec3(1.0f);
    color = glm::max(color, glm::vec3(0.0f));
    intensityLumens = std::max(0.0f, std::isfinite(intensityLumens) ? intensityLumens : 0.0f);
    range = std::max(0.001f, std::isfinite(range) ? range : 0.001f);
    innerAngleDegrees = std::clamp(std::isfinite(innerAngleDegrees) ? innerAngleDegrees : 20.0f, 0.0f, 88.0f);
    outerAngleDegrees = std::clamp(std::isfinite(outerAngleDegrees) ? outerAngleDegrees : 35.0f,
                                   innerAngleDegrees + 0.1f, 89.0f);
    indirectInfluence = std::max(0.0f, std::isfinite(indirectInfluence) ? indirectInfluence : 0.0f);
    shadow.resolution = std::max(1u, shadow.resolution);
    shadow.bias = std::max(0.0f, std::isfinite(shadow.bias) ? shadow.bias : 0.0f);
    shadow.normalBias = std::max(0.0f, std::isfinite(shadow.normalBias) ? shadow.normalBias : 0.0f);
}

glm::vec4 ShadowAtlasRegion::normalized(uint32_t atlasWidth, uint32_t atlasHeight) const noexcept {
    if (atlasWidth == 0 || atlasHeight == 0) return glm::vec4(0.0f);
    return {static_cast<float>(x) / atlasWidth, static_cast<float>(y) / atlasHeight,
            static_cast<float>(width) / atlasWidth, static_cast<float>(height) / atlasHeight};
}

ShadowAtlasAllocator::ShadowAtlasAllocator(uint32_t width, uint32_t height, uint32_t minimumTile)
    : width_(std::max(1u, width)), height_(std::max(1u, height)),
      minimumTile_(std::max(1u, minimumTile)) {
    free_.push_back({0, 0, width_, height_});
}

std::optional<ShadowAtlasRegion> ShadowAtlasAllocator::allocate(uint64_t owner, uint32_t resolution) {
    if (const auto* existing = find_owner(owner)) return *existing;
    const uint32_t tile = round_tile(resolution, minimumTile_);
    if (tile > width_ || tile > height_) return std::nullopt;
    size_t best = free_.size();
    uint64_t bestWaste = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].width < tile || free_[i].height < tile) continue;
        const uint64_t waste = static_cast<uint64_t>(free_[i].width) * free_[i].height - static_cast<uint64_t>(tile) * tile;
        if (waste < bestWaste || (waste == bestWaste && (free_[i].y < free_[best].y ||
            (free_[i].y == free_[best].y && free_[i].x < free_[best].x)))) {
            best = i;
            bestWaste = waste;
        }
    }
    if (best == free_.size()) return std::nullopt;

    const FreeRect selected = free_[best];
    free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(best));
    if (selected.width > tile) free_.push_back({selected.x + tile, selected.y, selected.width - tile, tile});
    if (selected.height > tile) free_.push_back({selected.x, selected.y + tile, selected.width, selected.height - tile});
    ShadowAtlasRegion region{nextId_++, owner, selected.x, selected.y, tile, tile};
    allocations_.push_back(region);
    return region;
}

bool ShadowAtlasAllocator::release(ShadowAllocationId id) {
    const auto it = std::find_if(allocations_.begin(), allocations_.end(), [id](const auto& region) { return region.id == id; });
    if (it == allocations_.end()) return false;
    free_.push_back({it->x, it->y, it->width, it->height});
    allocations_.erase(it);
    merge_free_rectangles();
    return true;
}

size_t ShadowAtlasAllocator::release_owner(uint64_t owner) {
    std::vector<ShadowAllocationId> ids;
    for (const auto& allocation : allocations_) if (allocation.owner == owner) ids.push_back(allocation.id);
    for (ShadowAllocationId id : ids) release(id);
    return ids.size();
}

const ShadowAtlasRegion* ShadowAtlasAllocator::find(ShadowAllocationId id) const noexcept {
    const auto it = std::find_if(allocations_.begin(), allocations_.end(), [id](const auto& region) { return region.id == id; });
    return it == allocations_.end() ? nullptr : &*it;
}

const ShadowAtlasRegion* ShadowAtlasAllocator::find_owner(uint64_t owner) const noexcept {
    const auto it = std::find_if(allocations_.begin(), allocations_.end(), [owner](const auto& region) { return region.owner == owner; });
    return it == allocations_.end() ? nullptr : &*it;
}

float ShadowAtlasAllocator::utilization() const noexcept {
    uint64_t used = 0;
    for (const auto& allocation : allocations_) used += static_cast<uint64_t>(allocation.width) * allocation.height;
    return static_cast<float>(used) / static_cast<float>(static_cast<uint64_t>(width_) * height_);
}

void ShadowAtlasAllocator::reset() {
    allocations_.clear();
    free_.clear();
    free_.push_back({0, 0, width_, height_});
    nextId_ = 1;
}

void ShadowAtlasAllocator::merge_free_rectangles() {
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t a = 0; a < free_.size() && !merged; ++a) {
            for (size_t b = a + 1; b < free_.size(); ++b) {
                FreeRect combined{};
                bool canMerge = false;
                if (free_[a].y == free_[b].y && free_[a].height == free_[b].height &&
                    (free_[a].x + free_[a].width == free_[b].x || free_[b].x + free_[b].width == free_[a].x)) {
                    combined = {std::min(free_[a].x, free_[b].x), free_[a].y,
                                free_[a].width + free_[b].width, free_[a].height};
                    canMerge = true;
                } else if (free_[a].x == free_[b].x && free_[a].width == free_[b].width &&
                    (free_[a].y + free_[a].height == free_[b].y || free_[b].y + free_[b].height == free_[a].y)) {
                    combined = {free_[a].x, std::min(free_[a].y, free_[b].y),
                                free_[a].width, free_[a].height + free_[b].height};
                    canMerge = true;
                }
                if (canMerge) {
                    free_[a] = combined;
                    free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(b));
                    merged = true;
                    break;
                }
            }
        }
    }
}

void RenderStatsCollector::begin_frame(uint64_t frameIndex) {
    std::scoped_lock lock(mutex_);
    current_ = {};
    current_.frameIndex = frameIndex;
}

void RenderStatsCollector::record_pass(RenderPassStats pass) {
    std::scoped_lock lock(mutex_);
    current_.drawCalls += pass.drawCalls;
    current_.dispatchCalls += pass.dispatchCalls;
    current_.triangles += pass.triangles;
    current_.passes.push_back(std::move(pass));
}

void RenderStatsCollector::record_visibility(uint64_t visible, uint64_t culled) {
    std::scoped_lock lock(mutex_);
    current_.visibleObjects += visible;
    current_.culledObjects += culled;
}

void RenderStatsCollector::record_transient_bytes(uint64_t bytes) {
    std::scoped_lock lock(mutex_);
    current_.transientBytes = std::max(current_.transientBytes, bytes);
}

void RenderStatsCollector::record_barriers(uint64_t count) {
    std::scoped_lock lock(mutex_);
    current_.barrierCount += count;
}

void RenderStatsCollector::record_shader_cache(bool hit) {
    std::scoped_lock lock(mutex_);
    if (hit) ++current_.shaderCacheHits; else ++current_.shaderCacheMisses;
}

void RenderStatsCollector::end_frame(double cpuMilliseconds, double gpuMilliseconds) {
    std::scoped_lock lock(mutex_);
    current_.cpuFrameMilliseconds = std::max(0.0, cpuMilliseconds);
    current_.gpuFrameMilliseconds = std::max(0.0, gpuMilliseconds);
}

RenderStats RenderStatsCollector::snapshot() const {
    std::scoped_lock lock(mutex_);
    return current_;
}

void RenderDebugStats::push(RenderDebugMessage message) {
    std::scoped_lock lock(mutex_);
    if (capacity_ == 0) return;
    if (messages_.size() == capacity_) messages_.erase(messages_.begin());
    messages_.push_back(std::move(message));
}

std::vector<RenderDebugMessage> RenderDebugStats::messages(std::optional<RenderDebugMessage::Severity> severity) const {
    std::scoped_lock lock(mutex_);
    if (!severity) return messages_;
    std::vector<RenderDebugMessage> result;
    std::copy_if(messages_.begin(), messages_.end(), std::back_inserter(result),
                 [&](const auto& message) { return message.severity == *severity; });
    return result;
}

size_t RenderDebugStats::count(RenderDebugMessage::Severity severity) const {
    std::scoped_lock lock(mutex_);
    return static_cast<size_t>(std::count_if(messages_.begin(), messages_.end(),
        [&](const auto& message) { return message.severity == severity; }));
}

void RenderDebugStats::clear() {
    std::scoped_lock lock(mutex_);
    messages_.clear();
}

} // namespace Engine::Rendering

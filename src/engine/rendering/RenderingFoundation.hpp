#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Engine::Rendering {

enum class LightKind : uint8_t { Directional, Point, Spot };

struct ShadowSettings {
    bool enabled{false};
    uint32_t resolution{1024};
    float bias{0.001f};
    float normalBias{0.01f};
};

struct DirectionalLight {
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensityLux{100000.0f};
    float indirectInfluence{1.0f};
    ShadowSettings shadow{true, 2048, 0.001f, 0.01f};
    [[nodiscard]] bool valid() const noexcept;
    void sanitize() noexcept;
};

struct PointLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensityLumens{1000.0f};
    float range{10.0f};
    float indirectInfluence{1.0f};
    ShadowSettings shadow{};
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] float attenuation(float distance) const noexcept;
    void sanitize() noexcept;
};

struct SpotLight {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensityLumens{1000.0f};
    float range{10.0f};
    float innerAngleDegrees{20.0f};
    float outerAngleDegrees{35.0f};
    float indirectInfluence{1.0f};
    ShadowSettings shadow{};
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] float attenuation(const glm::vec3& samplePosition) const noexcept;
    void sanitize() noexcept;
};

using ShadowAllocationId = uint64_t;
inline constexpr ShadowAllocationId InvalidShadowAllocation = 0;

struct ShadowAtlasRegion {
    ShadowAllocationId id{InvalidShadowAllocation};
    uint64_t owner{};
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
    [[nodiscard]] glm::vec4 normalized(uint32_t atlasWidth, uint32_t atlasHeight) const noexcept;
};

class ShadowAtlasAllocator final {
public:
    ShadowAtlasAllocator(uint32_t width = 4096, uint32_t height = 4096,
                         uint32_t minimumTile = 128);
    [[nodiscard]] std::optional<ShadowAtlasRegion> allocate(uint64_t owner, uint32_t resolution);
    [[nodiscard]] bool release(ShadowAllocationId id);
    [[nodiscard]] size_t release_owner(uint64_t owner);
    [[nodiscard]] const ShadowAtlasRegion* find(ShadowAllocationId id) const noexcept;
    [[nodiscard]] const ShadowAtlasRegion* find_owner(uint64_t owner) const noexcept;
    [[nodiscard]] float utilization() const noexcept;
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    void reset();

private:
    struct FreeRect { uint32_t x; uint32_t y; uint32_t width; uint32_t height; };
    uint32_t width_;
    uint32_t height_;
    uint32_t minimumTile_;
    ShadowAllocationId nextId_{1};
    std::vector<FreeRect> free_;
    std::vector<ShadowAtlasRegion> allocations_;
    void merge_free_rectangles();
};

enum class RenderDebugView : uint8_t {
    Lit,
    LightingOnly,
    Albedo,
    Normals,
    Roughness,
    Metallic,
    ShadowCascades,
    Overdraw,
    LightComplexity,
    GIProbes
};

struct RenderPassStats {
    std::string name;
    double cpuMilliseconds{};
    double gpuMilliseconds{};
    uint64_t drawCalls{};
    uint64_t dispatchCalls{};
    uint64_t triangles{};
};

struct RenderStats {
    uint64_t frameIndex{};
    double cpuFrameMilliseconds{};
    double gpuFrameMilliseconds{};
    uint64_t drawCalls{};
    uint64_t dispatchCalls{};
    uint64_t triangles{};
    uint64_t visibleObjects{};
    uint64_t culledObjects{};
    uint64_t transientBytes{};
    uint64_t barrierCount{};
    uint64_t shaderCacheHits{};
    uint64_t shaderCacheMisses{};
    std::vector<RenderPassStats> passes;
};

class RenderStatsCollector final {
public:
    void begin_frame(uint64_t frameIndex);
    void record_pass(RenderPassStats pass);
    void record_visibility(uint64_t visible, uint64_t culled);
    void record_transient_bytes(uint64_t bytes);
    void record_barriers(uint64_t count);
    void record_shader_cache(bool hit);
    void end_frame(double cpuMilliseconds, double gpuMilliseconds);
    [[nodiscard]] RenderStats snapshot() const;

private:
    mutable std::mutex mutex_;
    RenderStats current_;
};

struct RenderDebugMessage {
    enum class Severity : uint8_t { Info, Warning, Error };
    Severity severity{Severity::Info};
    std::string category;
    std::string text;
    uint64_t frameIndex{};
};

class RenderDebugStats final {
public:
    explicit RenderDebugStats(size_t capacity = 256) : capacity_(capacity) {}
    void push(RenderDebugMessage message);
    [[nodiscard]] std::vector<RenderDebugMessage> messages(
        std::optional<RenderDebugMessage::Severity> severity = std::nullopt) const;
    [[nodiscard]] size_t count(RenderDebugMessage::Severity severity) const;
    void clear();

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<RenderDebugMessage> messages_;
};

} // namespace Engine::Rendering

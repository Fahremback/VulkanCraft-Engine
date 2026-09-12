#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <mutex>
#include <utility>
#include <vector>

// Small in-process bridge between the public probe-grid providers and the
// renderer-owned RadianceCache GPU buffer.  It deliberately carries only the
// data already consumed by radiance_cache.glsl; Vulkan ownership remains in
// RadianceCache.
namespace vc::rendering::gi_bridge {

enum class PublicationClass : std::uint8_t {
    Auxiliary = 0,
    CanonicalGi = 1,
};

struct Probe {
    glm::vec3 irradiance{0.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    glm::vec3 position{0.0f};
    glm::ivec3 cell{0};
    float visibility{1.0f};
    float confidence{0.0f};
    // DDGI distance moments in world metres. x=mean, y=mean-square,
    // z=minimum hit distance, w=maximum sampled distance.
    glm::vec4 depthMoments{0.0f};
};

struct Publication {
    const void* source{nullptr};
    PublicationClass publicationClass{PublicationClass::Auxiliary};
    float cellSize{1.0f};
    std::uint32_t resolution{0u};
    std::uint64_t revision{0u};
    std::vector<Probe> probes;
};

inline std::vector<Publication>& publications() {
    static std::vector<Publication> value;
    return value;
}

inline std::atomic<std::uint64_t>& next_revision() {
    static std::atomic<std::uint64_t> value{1u};
    return value;
}

inline std::mutex& publication_mutex() {
    static std::mutex value;
    return value;
}

inline void publish(const void* source, float cellSize, std::uint32_t resolution,
                    std::vector<Probe> probes,
                    PublicationClass publicationClass = PublicationClass::Auxiliary) {
    if (source == nullptr || resolution == 0u || probes.empty()) return;
    std::scoped_lock lock(publication_mutex());
    auto& all = publications();
    const auto it = std::find_if(all.begin(), all.end(),
        [source](const Publication& p) { return p.source == source; });
    Publication value;
    value.source = source;
    value.publicationClass = publicationClass;
    value.cellSize = cellSize;
    value.resolution = resolution;
    value.revision = next_revision()++;
    value.probes = std::move(probes);
    if (it == all.end()) all.push_back(std::move(value));
    else *it = std::move(value);
}

inline std::vector<Publication> snapshot() {
    std::scoped_lock lock(publication_mutex());
    return publications();
}

inline void retire(const void* source) {
    std::scoped_lock lock(publication_mutex());
    auto& all = publications();
    all.erase(std::remove_if(all.begin(), all.end(),
        [source](const Publication& p) { return p.source == source; }), all.end());
}

// Renderer-driven reflection runtime.  The public IReflectionProvider remains
// ABI-stable; its concrete implementation registers this internal frame seam so
// the real Vulkan material path can drive capture/trace/history every frame.
struct ReflectionProbeSeed {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::ivec3 cell{0};
    std::uint32_t cascade{0u};
};

struct ReflectionProbeResult {
    glm::vec3 radiance{0.0f};
    float hitDistance{0.0f};
    float confidence{0.0f};
    bool rayTraced{false};
};

struct ReflectionFrameInput {
    glm::vec3 cameraPosition{0.0f};
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 sunColor{1.0f};
    std::uint64_t frameRevision{0u};
    std::uint64_t sceneRevision{0u};
    float maxTraceDistance{128.0f};
    std::function<float(float, float)> heightAt;
    std::function<glm::vec3(float, float)> albedoAt;
};

using ReflectionUpdateFn = std::function<bool(
    const ReflectionFrameInput&,
    const std::vector<ReflectionProbeSeed>&,
    std::vector<ReflectionProbeResult>&)>;

struct ReflectionRuntimeRegistration {
    const void* source{nullptr};
    std::uint64_t revision{0u};
    ReflectionUpdateFn update;
};

inline ReflectionRuntimeRegistration& reflection_runtime() {
    static ReflectionRuntimeRegistration value;
    return value;
}

inline std::mutex& reflection_mutex() {
    static std::mutex value;
    return value;
}

inline void register_reflection_runtime(const void* source, ReflectionUpdateFn update) {
    if (source == nullptr || !update) return;
    std::scoped_lock lock(reflection_mutex());
    auto& runtime = reflection_runtime();
    runtime.source = source;
    runtime.revision = next_revision()++;
    runtime.update = std::move(update);
}

inline void retire_reflection_runtime(const void* source) {
    std::scoped_lock lock(reflection_mutex());
    auto& runtime = reflection_runtime();
    if (runtime.source == source) runtime = {};
}

inline bool update_reflections(const ReflectionFrameInput& frame,
                               const std::vector<ReflectionProbeSeed>& seeds,
                               std::vector<ReflectionProbeResult>& results) {
    ReflectionUpdateFn update;
    {
        std::scoped_lock lock(reflection_mutex());
        update = reflection_runtime().update;
    }
    if (!update) return false;
    return update(frame, seeds, results);
}

} // namespace vc::rendering::gi_bridge

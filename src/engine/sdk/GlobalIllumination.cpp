// GlobalIllumination.cpp — Agente 1 (task_plan A.1/A.2): the PUBLIC adapter
// for IGlobalIlluminationProvider / IGiCore. It consolidates the existing
// `RadianceCache` (src/engine/rendering/lighting) as the deterministic,
// HEADLESS pure core (toroidal probe clipmaps, fixed per-cascade shares,
// sun-revision invalidation) and exposes it behind the public contract.
//
// The GPU half (VkDevice/VmaAllocator, upload recording, descriptors) stays in
// the Vulkan renderer (RadianceCache.hpp) — this adapter is self-contained
// (std + glm) and headless, so tests, the profiler and the editor debug views
// can drive GI without a GPU.

#include "engine/rendering/IGlobalIlluminationProvider.hpp"
#include "engine/rendering/IReflectionProvider.hpp"

#include "RadianceCacheMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace Engine::Rendering {
namespace {

constexpr std::uint32_t kInvalidDirtyMin = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kMaxCascades = 6;

glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback) {
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared > 1.0e-8f ? value * glm::inversesqrt(lengthSquared)
                                    : fallback;
}

// The default deterministic sampler: flat neutral ground (height 0, gray
// albedo) — a valid, bit-exact stand-in until the renderer binds the real
// voxel TerrainGenerator (handoff 3->1).
GiSurfaceSample flat_ground(float, float) {
    return GiSurfaceSample{ 0.0f, glm::vec3(0.5f) };
}

class GiCore final : public IGiCore {
public:
    GiCore() = default;

    bool configure(const GiClipmapConfig& requested, std::string& errorOut) override {
        if (requested.cascadeCount < 1 || requested.cascadeCount > kMaxCascades) {
            errorOut = "gi: cascadeCount must be in [1, 6]";
            return false;
        }
        if (requested.resolution < 4 || requested.resolution > 32) {
            errorOut = "gi: resolution must be in [4, 32]";
            return false;
        }
        if (requested.probesPerFrame < 1) {
            errorOut = "gi: probesPerFrame must be >= 1";
            return false;
        }
        if (requested.baseSpacing < 0.5f) {
            errorOut = "gi: baseSpacing must be >= 0.5";
            return false;
        }
        if (requested.cascadeScale < 2.0f) {
            errorOut = "gi: cascadeScale must be >= 2";
            return false;
        }
        if (requested.sunRefreshAngleDegrees < 0.25f ||
            requested.sunRefreshAngleDegrees > 15.0f) {
            errorOut = "gi: sunRefreshAngleDegrees must be in [0.25, 15]";
            return false;
        }

        config_ = requested;
        const std::uint64_t perCascade =
            static_cast<std::uint64_t>(config_.resolution) * config_.resolution *
            config_.resolution;
        const std::uint64_t total = perCascade * config_.cascadeCount;
        if (total > std::numeric_limits<std::uint32_t>::max()) {
            errorOut = "gi: probe count exceeds 32-bit indexing";
            return false;
        }

        probes_.assign(static_cast<std::size_t>(total), Probe{});
        for (std::uint32_t c = 0; c < kMaxCascades; ++c) {
            cascades_[c] = CascadeState{};
            if (c < config_.cascadeCount) {
                cascades_[c].baseProbe =
                    static_cast<std::uint32_t>(perCascade * c);
                const float spacing =
                    config_.baseSpacing *
                    std::pow(config_.cascadeScale, static_cast<float>(c));
                spacing_[c] = spacing;
                inverseSpacing_[c] = 1.0f / spacing;
            }
        }
        sunRevision_ = 1;
        cachedSunDirection_ = glm::vec3(0.0f, 1.0f, 0.0f);
        cachedSunColor_ = glm::vec3(1.0f);
        errorOut.clear();
        return true;
    }

    bool configure_json(const std::string& jsonText, std::string& errorOut) override {
        // Minimal, dependency-free JSON for the config surface (the engine has
        // no external JSON dependency; the public contracts parse their own).
        GiClipmapConfig next;
        // Expect a flat object of the six numeric keys. Accept only exact keys.
        std::string s = jsonText;
        const auto has_key = [&s](const std::string& key) {
            return s.find("\"" + key + "\"") != std::string::npos;
        };
        if (s.find("\"version\":1") == std::string::npos) {
            errorOut = "gi: config JSON must declare version 1";
            return false;
        }
        const auto read_num = [&s](const std::string& key, double& out) {
            const std::size_t k = s.find("\"" + key + "\"");
            if (k == std::string::npos) return false;
            const std::size_t colon = s.find(':', k);
            if (colon == std::string::npos) return false;
            const std::size_t v = colon + 1;
            char* end = nullptr;
            out = std::strtod(s.c_str() + v, &end);
            return end != s.c_str() + v;
        };
        (void)has_key;
        double v = 0.0;
        if (read_num("cascadeCount", v)) next.cascadeCount = static_cast<std::uint32_t>(v);
        if (read_num("resolution", v)) next.resolution = static_cast<std::uint32_t>(v);
        if (read_num("probesPerFrame", v)) next.probesPerFrame = static_cast<std::uint32_t>(v);
        if (read_num("baseSpacing", v)) next.baseSpacing = static_cast<float>(v);
        if (read_num("cascadeScale", v)) next.cascadeScale = static_cast<float>(v);
        if (read_num("sunRefreshAngleDegrees", v))
            next.sunRefreshAngleDegrees = static_cast<float>(v);
        return configure(next, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"cascadeCount\":" << config_.cascadeCount
            << ",\"resolution\":" << config_.resolution
            << ",\"probesPerFrame\":" << config_.probesPerFrame
            << ",\"baseSpacing\":" << config_.baseSpacing
            << ",\"cascadeScale\":" << config_.cascadeScale
            << ",\"sunRefreshAngleDegrees\":" << config_.sunRefreshAngleDegrees
            << "}";
        return out.str();
    }

    const GiClipmapConfig& config() const noexcept override { return config_; }

    std::uint32_t update(const glm::vec3& cameraPosition,
                         const glm::vec3& sunDirection,
                         const glm::vec3& sunColor,
                         const GiTerrainSampler& sampler,
                         std::uint32_t budgetOverride) override {
        const GiTerrainSampler& sample = sampler ? sampler
                                                 : GiTerrainSampler(&flat_ground);

        const glm::vec3 normalizedSun =
            safe_normalize(sunDirection, cachedSunDirection_);
        const float cosineThreshold =
            std::cos(glm::radians(config_.sunRefreshAngleDegrees));
        const bool sunChanged =
            glm::dot(normalizedSun, cachedSunDirection_) < cosineThreshold ||
            glm::length(sunColor - cachedSunColor_) > 0.08f;
        if (sunChanged) {
            cachedSunDirection_ = normalizedSun;
            cachedSunColor_ = sunColor;
            ++sunRevision_;
        }

        for (std::uint32_t c = 0; c < config_.cascadeCount; ++c) {
            CascadeState& state = cascades_[c];
            const glm::ivec3 newMin = radiance_cache_math::clipmap_min_cell(
                cameraPosition, inverseSpacing_[c], config_.resolution);
            if (glm::any(glm::notEqual(newMin, state.minCell))) {
                state.minCell = newMin;
                rebuild_pending(c, false);
            }
            const bool hasPending = state.nextPending < state.pending.size();
            if (!hasPending && state.sunRevision != sunRevision_) {
                rebuild_pending(c, true);
            }
        }

        const std::uint32_t budget =
            budgetOverride > 0 ? budgetOverride : config_.probesPerFrame;
        static constexpr std::array<std::uint32_t, kMaxCascades> weights{
            40, 24, 14, 10, 7, 5 };
        std::uint32_t generated = 0;
        std::uint32_t remaining = budget;

        const auto finish_pending = [](CascadeState& state) {
            if (state.nextPending < state.pending.size()) return;
            if (state.pendingSunRevision != 0u) {
                state.sunRevision = state.pendingSunRevision;
                state.pendingSunRevision = 0u;
            }
            state.pending.clear();
            state.nextPending = 0;
        };

        for (std::uint32_t c = 0; c < config_.cascadeCount && remaining > 0; ++c) {
            CascadeState& state = cascades_[c];
            const std::uint32_t share =
                c + 1 == config_.cascadeCount
                    ? remaining
                    : std::max(1u, budget * weights[c] / 100u);
            std::uint32_t spent = 0;
            while (state.nextPending < state.pending.size() && spent < share &&
                   remaining > 0) {
                const glm::ivec3 cell = state.pending[state.nextPending++].cell;
                if (!contains_cell(c, cell)) continue;
                const std::uint32_t slot = slot_index(c, cell);
                probes_[slot] = evaluate_probe(c, cell, normalizedSun, sunColor,
                                               sample);
                ++spent;
                ++generated;
                --remaining;
            }
            finish_pending(state);
        }

        while (remaining > 0) {
            bool progressed = false;
            for (std::uint32_t c = 0; c < config_.cascadeCount && remaining > 0; ++c) {
                CascadeState& state = cascades_[c];
                while (state.nextPending < state.pending.size()) {
                    const glm::ivec3 cell =
                        state.pending[state.nextPending++].cell;
                    if (!contains_cell(c, cell)) continue;
                    const std::uint32_t slot = slot_index(c, cell);
                    probes_[slot] = evaluate_probe(c, cell, normalizedSun,
                                                   sunColor, sample);
                    --remaining;
                    ++generated;
                    progressed = true;
                    break;
                }
                finish_pending(state);
            }
            if (!progressed) break;
        }
        return generated;
    }

    std::uint32_t total_probe_count() const noexcept override {
        return static_cast<std::uint32_t>(probes_.size());
    }
    std::uint32_t pending_probe_count() const override {
        std::uint64_t total = 0;
        for (std::uint32_t c = 0; c < config_.cascadeCount; ++c) {
            const CascadeState& s = cascades_[c];
            total += s.pending.size() - std::min(s.nextPending, s.pending.size());
        }
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
    }
    bool probe(std::uint32_t index, Probe& out) const override {
        if (index >= probes_.size()) return false;
        out = probes_[index];
        return true;
    }
    std::uint32_t sun_revision() const noexcept override { return sunRevision_; }
    std::vector<GiClipmapRange> clipmap_ranges() const override {
        std::vector<GiClipmapRange> ranges;
        ranges.reserve(config_.cascadeCount);
        for (std::uint32_t c = 0; c < config_.cascadeCount; ++c) {
            GiClipmapRange range;
            range.minCell = cascades_[c].minCell;
            range.resolution = static_cast<int>(config_.resolution);
            range.inverseSpacing = inverseSpacing_[c];
            ranges.push_back(range);
        }
        return ranges;
    }

private:
    struct PendingCell {
        glm::ivec3 cell{ 0 };
    };
    struct CascadeState {
        glm::ivec3 minCell{ std::numeric_limits<int>::max() };
        std::uint32_t baseProbe{ 0 };
        std::uint32_t sunRevision{ 0 };
        std::uint32_t pendingSunRevision{ 0 };
        std::vector<PendingCell> pending;
        std::size_t nextPending{ 0 };
    };

    bool contains_cell(std::uint32_t cascade, const glm::ivec3& cell) const {
        const glm::ivec3 local = cell - cascades_[cascade].minCell;
        const int resolution = static_cast<int>(config_.resolution);
        return local.x >= 0 && local.y >= 0 && local.z >= 0 &&
               local.x < resolution && local.y < resolution &&
               local.z < resolution;
    }
    std::uint32_t slot_index(std::uint32_t cascade,
                             const glm::ivec3& cell) const {
        return cascades_[cascade].baseProbe +
               radiance_cache_math::toroidal_local_index(cell, config_.resolution);
    }
    void rebuild_pending(std::uint32_t cascade, bool includeSunStale) {
        CascadeState& state = cascades_[cascade];
        state.pending.clear();
        state.nextPending = 0;
        const int resolution = static_cast<int>(config_.resolution);
        state.pending.reserve(static_cast<std::size_t>(resolution) * resolution *
                              resolution);
        for (int z = 0; z < resolution; ++z) {
            for (int y = 0; y < resolution; ++y) {
                for (int x = 0; x < resolution; ++x) {
                    const glm::ivec3 cell =
                        state.minCell + glm::ivec3(x, y, z);
                    const Probe& p = probes_[slot_index(cascade, cell)];
                    const bool wrongCell =
                        glm::any(glm::notEqual(glm::ivec3(p.worldCellCascade),
                                               cell)) ||
                        p.worldCellCascade.w != static_cast<int>(cascade);
                    if (wrongCell || includeSunStale)
                        state.pending.push_back({ cell });
                }
            }
        }
        state.pendingSunRevision = includeSunStale ? sunRevision_ : 0u;
    }

    Probe evaluate_probe(std::uint32_t cascade, const glm::ivec3& cell,
                         const glm::vec3& sunDirection,
                         const glm::vec3& sunColor,
                         const GiTerrainSampler& sample) const {
        const float spacing = spacing_[cascade];
        const glm::vec3 position = (glm::vec3(cell) + 0.5f) * spacing;
        const GiSurfaceSample center = sample(position.x, position.z);
        const float surfaceY = center.height + 1.0f;
        const float altitude = position.y - surfaceY;
        const glm::vec3 ground = center.albedo;
        const glm::vec3 sun = safe_normalize(sunDirection, glm::vec3(0.0f, 1.0f, 0.0f));

        Probe result{};
        result.worldCellCascade = glm::ivec4(cell, static_cast<int>(cascade));

        if (altitude < -0.35f * spacing) {
            const float depth =
                std::min(-altitude / std::max(spacing * 2.0f, 1.0f), 1.0f);
            result.radianceVisibility =
                glm::vec4(ground * glm::mix(0.035f, 0.008f, depth), 0.015f);
            result.directionConfidence = glm::vec4(0.0f, 1.0f, 0.0f, 0.25f);
            return result;
        }

        const bool nearSurface = altitude < spacing * 8.0f;
        float skyVisibility = std::clamp(
            0.50f + altitude / std::max(spacing * 4.0f, 1.0f), 0.12f, 1.0f);
        float directVisibility = sun.y > 0.015f ? 1.0f : 0.0f;
        glm::vec3 bentNormal(0.0f, 1.0f, 0.0f);

        if (nearSurface) {
            const float sampleDistance = std::max(spacing * 2.0f, 2.0f);
            const float hx0 = sample(position.x - sampleDistance, position.z).height;
            const float hx1 = sample(position.x + sampleDistance, position.z).height;
            const float hz0 = sample(position.x, position.z - sampleDistance).height;
            const float hz1 = sample(position.x, position.z + sampleDistance).height;
            bentNormal =
                safe_normalize(glm::vec3(hx0 - hx1, sampleDistance * 2.0f, hz0 - hz1),
                               glm::vec3(0.0f, 1.0f, 0.0f));
            const float maxNeighbor =
                std::max(std::max(hx0, hx1), std::max(hz0, hz1));
            const float localHorizon =
                std::clamp((maxNeighbor - position.y) / sampleDistance, 0.0f, 1.0f);
            skyVisibility *= 1.0f - localHorizon * 0.62f;
        }

        const float day = std::clamp(sun.y * 4.0f + 0.12f, 0.025f, 1.0f);
        const glm::vec3 skyColor =
            glm::mix(glm::vec3(0.008f, 0.012f, 0.03f),
                     glm::vec3(0.16f, 0.29f, 0.48f), day);
        const float groundBounce = std::clamp(
            glm::dot(bentNormal, glm::vec3(0.0f, 1.0f, 0.0f)), 0.0f, 1.0f);
        const glm::vec3 ambient =
            skyColor * skyVisibility +
            ground * (0.055f + 0.10f * groundBounce) * day;
        const float sunLambert = std::max(glm::dot(bentNormal, sun), 0.0f);
        const glm::vec3 sunBounce = ground * sunColor * directVisibility *
                                    sunLambert * day * 0.16f;
        result.radianceVisibility = glm::vec4(ambient + sunBounce, skyVisibility);
        result.directionConfidence = glm::vec4(
            safe_normalize(bentNormal + sun * directVisibility * 0.35f,
                           glm::vec3(0.0f, 1.0f, 0.0f)),
            directVisibility);
        return result;
    }

    GiClipmapConfig config_{};
    std::vector<Probe> probes_;
    std::array<CascadeState, kMaxCascades> cascades_{};
    std::array<float, kMaxCascades> spacing_{};
    std::array<float, kMaxCascades> inverseSpacing_{};
    glm::vec3 cachedSunDirection_{ 0.0f, 1.0f, 0.0f };
    glm::vec3 cachedSunColor_{ 1.0f };
    std::uint32_t sunRevision_{ 1 };
};

class GlobalIlluminationProvider final : public IGlobalIlluminationProvider {
public:
    GlobalIlluminationProvider(GiBackend backend, GiCapabilities capabilities,
                               std::unique_ptr<IGiCore> core)
        : backend_(backend), capabilities_(capabilities), core_(std::move(core)) {}

    GiBackend backend() const noexcept override { return backend_; }
    GiCapabilities capabilities() const noexcept override { return capabilities_; }
    IGiCore& core() override { return *core_; }
    const IGiCore& core() const override { return *core_; }

private:
    GiBackend backend_;
    GiCapabilities capabilities_;
    std::unique_ptr<IGiCore> core_;
};

class ReflectionProvider final : public IReflectionProvider {
public:
    ReflectionProvider(ReflectionBackend backend, ReflectionCapabilities capabilities)
        : backend_(backend), capabilities_(capabilities) {}

    ReflectionBackend backend() const noexcept override { return backend_; }
    ReflectionCapabilities capabilities() const noexcept override {
        return capabilities_;
    }

    bool configure(const ReflectionConfig& config, std::string& errorOut) override {
        if (config.maxScreenRays < 1) {
            errorOut = "reflection: maxScreenRays must be >= 1";
            return false;
        }
        if (config.screenRoughnessLimit < 0.0f ||
            config.screenRoughnessLimit > 1.0f ||
            config.probeRoughnessFloor < 0.0f ||
            config.probeRoughnessFloor > 1.0f) {
            errorOut = "reflection: roughness limits must be in [0, 1]";
            return false;
        }
        config_ = config;
        errorOut.clear();
        return true;
    }
    const ReflectionConfig& config() const noexcept override { return config_; }

    ReflectionBackend resolve_mode(const ReflectionSurfaceInput& surface) const override {
        // Deterministic, never over-claims: rough surfaces prefer probes,
        // clear-coat always reflects, mirror-smooth prefers RT when available,
        // otherwise screen-space, otherwise None.
        const float roughness =
            std::clamp(surface.roughness, 0.0f, 1.0f);
        if (roughness >= config_.probeRoughnessFloor &&
            capabilities_.probe) {
            return ReflectionBackend::Probe;
        }
        if (surface.clearCoat > 0.5f || roughness <= config_.screenRoughnessLimit) {
            if (roughness <= 0.05f && capabilities_.rayTraced) {
                return ReflectionBackend::RayTraced;
            }
            if (capabilities_.screenSpace) {
                return ReflectionBackend::ScreenSpace;
            }
        }
        return ReflectionBackend::None;
    }

    std::uint32_t surfaces_in_mode(ReflectionBackend mode) const noexcept override {
        std::uint32_t count = 0;
        for (const ReflectionBackend m : lastModes_) {
            if (m == mode) ++count;
        }
        return count;
    }
    std::uint32_t screen_rays_used() const noexcept override {
        return screenRaysUsed_;
    }

    // Internal accounting (the renderer calls this each frame; kept here for
    // the headless test surface).
    void reset_frame() {
        lastModes_.clear();
        screenRaysUsed_ = 0;
    }
    void account_surface(ReflectionBackend mode, std::uint32_t rays) {
        lastModes_.push_back(mode);
        screenRaysUsed_ += rays;
    }

private:
    ReflectionBackend backend_;
    ReflectionCapabilities capabilities_;
    ReflectionConfig config_{};
    std::vector<ReflectionBackend> lastModes_;
    std::uint32_t screenRaysUsed_{ 0 };
};

}  // namespace

std::unique_ptr<IGiCore> create_gi_core(std::string& errorOut) {
    auto core = std::make_unique<GiCore>();
    GiClipmapConfig defaults;
    if (!core->configure(defaults, errorOut)) return nullptr;
    return core;
}

std::unique_ptr<IGiCore> create_gi_core_json(const std::string& jsonText,
                                             std::string& errorOut) {
    auto core = std::make_unique<GiCore>();
    if (!core->configure_json(jsonText, errorOut)) return nullptr;
    return core;
}

std::unique_ptr<IGlobalIlluminationProvider> create_global_illumination_provider(
    GiBackend backend, const GiCapabilities& capabilities, std::string& errorOut) {
    std::unique_ptr<IGiCore> core;
    switch (backend) {
        case GiBackend::RadianceCache:
            core = create_gi_core(errorOut);
            if (!core) return nullptr;
            return std::make_unique<GlobalIlluminationProvider>(
                backend, capabilities, std::move(core));
        case GiBackend::Ddgi:
            if (!capabilities.ddgi) {
                errorOut = "gi: DDGI backend requested but the rtxgi plugin is not linked";
                return nullptr;
            }
            errorOut = "gi: DDGI backend is not implemented yet (task_plan A.10)";
            return nullptr;
        case GiBackend::RayTraced:
            if (!capabilities.rayTraced) {
                errorOut = "gi: ray-traced backend requested but the device has no ray tracing";
                return nullptr;
            }
            errorOut = "gi: ray-traced backend is not implemented yet (task_plan A.8)";
            return nullptr;
        default:
            errorOut = "gi: unknown backend";
            return nullptr;
    }
}

std::unique_ptr<IReflectionProvider> create_reflection_provider(
    ReflectionBackend backend, const ReflectionCapabilities& capabilities,
    std::string& errorOut) {
    if (backend == ReflectionBackend::ScreenSpace) {
        auto provider =
            std::make_unique<ReflectionProvider>(backend, capabilities);
        ReflectionConfig defaults;
        if (!provider->configure(defaults, errorOut)) return nullptr;
        return provider;
    }
    if (backend == ReflectionBackend::Probe) {
        if (!capabilities.probe) {
            errorOut = "reflection: probe backend requested but no radiance cache is bound";
            return nullptr;
        }
        errorOut = "reflection: probe backend is not implemented yet (task_plan A.14)";
        return nullptr;
    }
    if (backend == ReflectionBackend::RayTraced) {
        if (!capabilities.rayTraced) {
            errorOut = "reflection: ray-traced backend requested but the device has no ray tracing";
            return nullptr;
        }
        errorOut = "reflection: ray-traced backend is not implemented yet (task_plan A.8)";
        return nullptr;
    }
    errorOut = "reflection: unknown backend";
    return nullptr;
}

}  // namespace Engine::Rendering
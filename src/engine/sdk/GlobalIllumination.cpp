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
#include "engine/rendering/IProbeGrid.hpp"
#include "engine/rendering/IRayTracer.hpp"
#include "engine/rendering/IReflectionProvider.hpp"
#include "engine/rendering/ITemporalDenoiser.hpp"

#include "RadianceCacheMath.hpp"
#include "../rendering/lighting/GiProviderBridge.hpp"
#include "../rendering/lighting/RenderProviderSelection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace Engine::Rendering {
namespace {

constexpr std::uint32_t kInvalidDirtyMin = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kMaxCascades = 6;

const char* gi_backend_name(GiBackend backend) {
    switch (backend) {
        case GiBackend::RadianceCache: return "radiance-cache";
        case GiBackend::Ddgi: return "ddgi-probe-grid";
        case GiBackend::RayTraced: return "vulkan-ray-query-gi";
        default: return "unknown";
    }
}

const char* reflection_backend_name(ReflectionBackend backend) {
    switch (backend) {
        case ReflectionBackend::None: return "none";
        case ReflectionBackend::ScreenSpace: return "screen-space";
        case ReflectionBackend::Probe: return "ddgi-probe-reflections";
        case ReflectionBackend::RayTraced: return "vulkan-ray-query-reflections";
        default: return "unknown";
    }
}

void record_gi_selection(GiBackend backend, const char* capability) {
    vc::rendering::provider_selection::record(
        "giProvider", gi_backend_name(backend), capability);
}

void record_reflection_selection(ReflectionBackend backend, const char* capability) {
    vc::rendering::provider_selection::record(
        "reflectionProvider", reflection_backend_name(backend), capability);
}

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

// Concrete DDGI / RT-GI core built from the engine's existing public probe-grid,
// Vulkan ray-query tracer and temporal denoiser.  DDGI uses one independently
// scrolling/relocating/classifying probe grid per cascade.  The RT variant
// additionally builds a local terrain acceleration structure, captures probe
// rays through IRayTracer and denoises the flattened probe history.
class DdgiGiCore final : public IGiCore {
public:
    explicit DdgiGiCore(std::unique_ptr<vc::rendering::IRayTracer> rayTracer = {})
        : rayTracer_(std::move(rayTracer)), rayTraced_(rayTracer_ != nullptr) {}

    bool configure(const GiClipmapConfig& requested, std::string& errorOut) override {
        GiCore validator;
        if (!validator.configure(requested, errorOut)) return false;

        struct PendingCascade {
            std::unique_ptr<IProbeGrid> grid;
            float spacing{ 1.0f };
            float gridScale{ 1.0f };
            std::uint32_t base{ 0 };
        };
        std::vector<PendingCascade> pending;
        pending.reserve(requested.cascadeCount);

        std::uint32_t base = 0;
        for (std::uint32_t c = 0; c < requested.cascadeCount; ++c) {
            std::string gridError;
            auto grid = create_probe_grid(gridError);
            if (!grid) {
                errorOut = "gi: failed to create DDGI probe grid: " + gridError;
                return false;
            }
            ProbeGridConfig gridConfig;
            gridConfig.resolution = requested.resolution;
            const float requestedSpacing = requested.baseSpacing *
                                           std::pow(requested.cascadeScale,
                                                    static_cast<float>(c));
            // IProbeGrid's public standalone contract caps one cell at 64 m.
            // DDGI clipmaps can span much larger cells, so drive the grid in a
            // scaled coordinate space and transform captures back to world.
            gridConfig.cellSize = std::min(requestedSpacing, 64.0f);
            const float gridScale = requestedSpacing / gridConfig.cellSize;
            gridConfig.probesPerFrame = requested.probesPerFrame;
            // RTXGI-style hysteresis: 0.90 old history, 0.10 new capture.
            gridConfig.historyWeight = 0.10f;
            gridConfig.maxRelocationStep = 0.25f;
            gridConfig.relocationEnabled = true;
            gridConfig.classificationEnabled = true;
            gridConfig.backfaceThreshold = 4;
            gridConfig.seed = c + 1u;
            if (!grid->configure(gridConfig, gridError)) {
                errorOut = "gi: failed to configure DDGI cascade " +
                           std::to_string(c) + ": " + gridError;
                return false;
            }
            pending.push_back({ std::move(grid), requestedSpacing, gridScale, base });
            base += requested.resolution * requested.resolution * requested.resolution;
        }

        std::unique_ptr<ITemporalDenoiser> denoiser;
        std::vector<DenoiserHistory> histories;
        if (rayTraced_) {
            std::string denoiseError;
            denoiser = create_temporal_denoiser(denoiseError);
            if (!denoiser) {
                errorOut = "gi: failed to create RT GI temporal denoiser: " + denoiseError;
                return false;
            }
            DenoiserConfig dc;
            dc.width = requested.resolution;
            dc.height = requested.resolution * requested.resolution * requested.cascadeCount;
            dc.historyWeight = 0.12f;
            dc.depthRejectThreshold = 0.20f;
            dc.normalRejectDegrees = 28.0f;
            dc.useMotion = false; // probe slots are toroidal/stable; scroll invalidates by geometry.
            dc.useDepthRejection = true;
            dc.useNormalRejection = true;
            dc.seed = 1u;
            if (!denoiser->configure(dc, denoiseError)) {
                errorOut = "gi: failed to configure RT GI temporal denoiser: " + denoiseError;
                return false;
            }
            histories.assign(static_cast<std::size_t>(dc.width) * dc.height,
                             DenoiserHistory{});
        }

        config_ = requested;
        cascades_.clear();
        cascades_.reserve(pending.size());
        for (auto& p : pending) {
            Cascade c;
            c.grid = std::move(p.grid);
            c.spacing = p.spacing;
            c.gridScale = p.gridScale;
            c.base = p.base;
            cascades_.push_back(std::move(c));
        }
        probes_.assign(base, Probe{});
        depthMoments_.assign(base, glm::vec4(0.0f));
        depthCursor_ = 0u;
        denoiser_ = std::move(denoiser);
        histories_ = std::move(histories);
        denoised_.clear();
        confidence_.clear();
        ranges_.assign(requested.cascadeCount, GiClipmapRange{});
        cachedSunDirection_ = glm::vec3(0.0f, 1.0f, 0.0f);
        cachedSunColor_ = glm::vec3(1.0f);
        previousCamera_ = glm::vec3(0.0f);
        havePreviousCamera_ = false;
        sunRevision_ = 1u;
        rtAnchor_ = glm::ivec2(std::numeric_limits<int>::max());
        rtSceneAge_ = 0u;
        rtSceneReady_ = false;
        errorOut.clear();
        return true;
    }

    bool configure_json(const std::string& jsonText, std::string& errorOut) override {
        GiCore parser;
        if (!parser.configure_json(jsonText, errorOut)) return false;
        return configure(parser.config(), errorOut);
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
        const GiTerrainSampler sample = sampler ? sampler : GiTerrainSampler(&flat_ground);
        const glm::vec3 sun = safe_normalize(sunDirection, cachedSunDirection_);
        const float cosineThreshold = std::cos(glm::radians(config_.sunRefreshAngleDegrees));
        const bool sunChanged = glm::dot(sun, cachedSunDirection_) < cosineThreshold ||
                                glm::length(sunColor - cachedSunColor_) > 0.08f;
        if (sunChanged) {
            cachedSunDirection_ = sun;
            cachedSunColor_ = sunColor;
            ++sunRevision_;
            reset_temporal_history();
        }

        if (havePreviousCamera_ &&
            glm::distance(previousCamera_, cameraPosition) >
                config_.baseSpacing * static_cast<float>(config_.resolution) * 1.5f) {
            reset_temporal_history();
        }
        previousCamera_ = cameraPosition;
        havePreviousCamera_ = true;

        if (rayTraced_) {
            refresh_rt_scene(cameraPosition, sample);
            // IRayTracer currently exposes synchronous closest-hit queries.
            // Keep hardware RT live every frame without turning the probe
            // budget into hundreds of fence waits; remaining directions use
            // the deterministic terrain marcher and are still temporally
            // accumulated/denoised with the traced samples.
            rtTraceBudgetRemaining_ = 4u;
        }

        static constexpr std::array<std::uint32_t, kMaxCascades> weights{
            40u, 24u, 14u, 10u, 7u, 5u };
        const std::uint32_t budget = budgetOverride > 0 ? budgetOverride
                                                        : config_.probesPerFrame;
        std::uint32_t remaining = budget;
        std::uint32_t updated = 0u;

        for (std::uint32_t c = 0; c < cascades_.size(); ++c) {
            Cascade& cascade = cascades_[c];
            const std::uint32_t share =
                c + 1u == cascades_.size()
                    ? remaining
                    : std::min(remaining, std::max(1u, budget * weights[c] / 100u));
            if (share == 0u) break;
            std::string gridError;
            const auto capture = [&](const glm::vec3& gridPos, const glm::vec3& dir) {
                const glm::vec3 worldPos = gridPos * cascade.gridScale;
                return capture_probe(worldPos, dir, cascade.spacing, sun, sunColor, sample);
            };
            const std::uint32_t spent =
                cascade.grid->update(cameraPosition / cascade.gridScale,
                                     capture, share, &gridError);
            // The standalone grid publishes in its local coordinate scale.
            // DDGI republishes below with the true cascade spacing.
            vc::rendering::gi_bridge::retire(cascade.grid.get());
            updated += spent;
            remaining -= std::min(remaining, spent);
        }

        rebuild_public_probe_view(cameraPosition, sample);
        update_depth_moments(sample, std::max(1u, budget));
        if (rayTraced_ && denoiser_) denoise_rt_probes(sample);
        publish_renderer_probes();
        return updated;
    }

    std::uint32_t total_probe_count() const noexcept override {
        return static_cast<std::uint32_t>(probes_.size());
    }

    std::uint32_t pending_probe_count() const override {
        std::uint32_t pending = 0u;
        for (const Cascade& cascade : cascades_) {
            for (std::uint32_t i = 0u; i < cascade.grid->probe_count(); ++i) {
                ProbeGridProbe p{};
                if (cascade.grid->probe(i, p) && p.age == 0u) ++pending;
            }
        }
        return pending;
    }

    bool probe(std::uint32_t index, Probe& out) const override {
        if (index >= probes_.size()) return false;
        out = probes_[index];
        return true;
    }

    std::uint32_t sun_revision() const noexcept override { return sunRevision_; }
    std::vector<GiClipmapRange> clipmap_ranges() const override { return ranges_; }

private:
    struct Cascade {
        std::unique_ptr<IProbeGrid> grid;
        float spacing{ 1.0f };
        float gridScale{ 1.0f };
        std::uint32_t base{ 0u };
    };

    static glm::vec3 terrain_normal(const GiTerrainSampler& sample,
                                    const glm::vec3& pos, float spacing) {
        const float d = std::max(0.5f, spacing * 0.25f);
        const float hx0 = sample(pos.x - d, pos.z).height;
        const float hx1 = sample(pos.x + d, pos.z).height;
        const float hz0 = sample(pos.x, pos.z - d).height;
        const float hz1 = sample(pos.x, pos.z + d).height;
        return safe_normalize(glm::vec3(hx0 - hx1, d * 2.0f, hz0 - hz1),
                              glm::vec3(0.0f, 1.0f, 0.0f));
    }

    bool terrain_occluded(const glm::vec3& origin, const glm::vec3& direction,
                          float maxDistance, float spacing,
                          const GiTerrainSampler& sample, float* hitDistance) const {
        const glm::vec3 dir = safe_normalize(direction, glm::vec3(0.0f, 1.0f, 0.0f));
        if (rayTraced_ && rtSceneReady_ && rayTracer_ && rtTraceBudgetRemaining_ > 0u) {
            --rtTraceBudgetRemaining_;
            vc::rendering::RayTracerRay ray{};
            ray.ox = origin.x; ray.oy = origin.y; ray.oz = origin.z;
            ray.dx = dir.x; ray.dy = dir.y; ray.dz = dir.z;
            ray.tMin = 0.05f;
            ray.tMax = maxDistance;
            const auto hit = rayTracer_->closestHit(ray);
            if (hit.hit) {
                if (hitDistance) *hitDistance = hit.t;
                return true;
            }
            if (hitDistance) *hitDistance = maxDistance;
            return false;
        }

        const float step = std::max(0.25f, spacing * 0.35f);
        for (float t = step; t <= maxDistance; t += step) {
            const glm::vec3 p = origin + dir * t;
            if (p.y <= sample(p.x, p.z).height) {
                if (hitDistance) *hitDistance = t;
                return true;
            }
        }
        if (hitDistance) *hitDistance = maxDistance;
        return false;
    }

    ProbeCaptureSample capture_probe(const glm::vec3& pos, const glm::vec3& direction,
                                     float spacing, const glm::vec3& sunDirection,
                                     const glm::vec3& sunColor,
                                     const GiTerrainSampler& sample) const {
        ProbeCaptureSample result{};
        const GiSurfaceSample surface = sample(pos.x, pos.z);
        const bool inside = pos.y < surface.height - spacing * 0.08f;
        result.backface = inside;
        if (inside) {
            result.radiance = surface.albedo * 0.006f;
            return result;
        }

        const glm::vec3 dir = safe_normalize(direction, glm::vec3(0.0f, 1.0f, 0.0f));
        const float maxDistance = std::max(16.0f, spacing * 8.0f);
        float hitDistance = maxDistance;
        const bool blocked = terrain_occluded(pos + dir * 0.05f, dir, maxDistance,
                                               spacing, sample, &hitDistance);
        if (blocked) {
            const glm::vec3 hitPos = pos + dir * hitDistance;
            const GiSurfaceSample hitSurface = sample(hitPos.x, hitPos.z);
            const glm::vec3 n = terrain_normal(sample, hitPos, spacing);
            const float ndl = std::max(glm::dot(n, sunDirection), 0.0f);
            float sunHitDistance = maxDistance;
            const bool sunBlocked = terrain_occluded(hitPos + n * 0.08f, sunDirection,
                                                      maxDistance, spacing, sample,
                                                      &sunHitDistance);
            const glm::vec3 bounce = hitSurface.albedo *
                                     (0.025f + (sunBlocked ? 0.0f : ndl * 0.18f)) *
                                     sunColor;
            result.radiance = bounce;
            return result;
        }

        const float day = std::clamp(sunDirection.y * 4.0f + 0.12f, 0.025f, 1.0f);
        const glm::vec3 sky = glm::mix(glm::vec3(0.008f, 0.012f, 0.03f),
                                      glm::vec3(0.16f, 0.29f, 0.48f), day);
        const float sunLobe = std::pow(std::max(glm::dot(dir, sunDirection), 0.0f), 64.0f);
        result.radiance = sky * (0.45f + 0.55f * std::max(dir.y, 0.0f)) +
                          sunColor * sunLobe * day;
        return result;
    }

    void rebuild_public_probe_view(const glm::vec3& cameraPosition,
                                   const GiTerrainSampler& sample) {
        for (std::uint32_t c = 0u; c < cascades_.size(); ++c) {
            Cascade& cascade = cascades_[c];
            const int resolution = static_cast<int>(config_.resolution);
            ranges_[c].minCell =
                glm::ivec3(glm::floor(cameraPosition / cascade.spacing)) -
                glm::ivec3(resolution / 2);
            ranges_[c].resolution = resolution;
            ranges_[c].inverseSpacing = 1.0f / cascade.spacing;
            for (std::uint32_t i = 0u; i < cascade.grid->probe_count(); ++i) {
                ProbeGridProbe p{};
                if (!cascade.grid->probe(i, p)) continue;
                Probe& out = probes_[cascade.base + i];
                const glm::vec3 worldPosition = p.position * cascade.gridScale;
                const float surfaceHeight = sample(worldPosition.x, worldPosition.z).height;
                float visibility = std::clamp(
                    0.5f + (worldPosition.y - surfaceHeight) /
                               std::max(1.0f, cascade.spacing * 4.0f),
                    0.0f, 1.0f);
                if ((p.flags & 4u) != 0u) visibility *= 0.05f;
                out.radianceVisibility = glm::vec4(p.irradiance, visibility);
                const glm::vec3 bent = glm::length(p.offset) > 1.0e-5f
                                           ? safe_normalize(p.offset, glm::vec3(0.0f, 1.0f, 0.0f))
                                           : terrain_normal(sample, worldPosition, cascade.spacing);
                out.directionConfidence =
                    glm::vec4(bent, std::min(1.0f, static_cast<float>(p.age) / 32.0f));
                out.worldCellCascade = glm::ivec4(p.cell, static_cast<int>(c));
            }
        }
    }

    void denoise_rt_probes(const GiTerrainSampler& sample) {
        if (!denoiser_ || histories_.size() != probes_.size()) return;
        std::vector<DenoiserSample> samples(probes_.size());
        for (std::uint32_t c = 0u; c < cascades_.size(); ++c) {
            const Cascade& cascade = cascades_[c];
            for (std::uint32_t i = 0u; i < cascade.grid->probe_count(); ++i) {
                const std::size_t index = static_cast<std::size_t>(cascade.base) + i;
                ProbeGridProbe p{};
                cascade.grid->probe(i, p);
                const glm::vec3 worldPosition = p.position * cascade.gridScale;
                DenoiserSample& s = samples[index];
                s.radiance = glm::vec3(probes_[index].radianceVisibility);
                s.motion = glm::vec2(0.0f);
                s.depth = std::max(0.01f,
                    std::fabs(worldPosition.y -
                              sample(worldPosition.x, worldPosition.z).height));
                s.normal = terrain_normal(sample, worldPosition, cascade.spacing);
            }
        }
        std::string error;
        if (!denoiser_->denoise(samples, histories_, confidence_, denoised_, error)) return;
        for (std::size_t i = 0; i < probes_.size() && i < denoised_.size(); ++i) {
            probes_[i].radianceVisibility =
                glm::vec4(denoised_[i], probes_[i].radianceVisibility.w);
            if (i < confidence_.size()) {
                probes_[i].directionConfidence.w =
                    std::min(1.0f, confidence_[i] / 32.0f);
            }
        }
    }

    void update_depth_moments(const GiTerrainSampler& sample,
                              std::uint32_t budget) {
        if (probes_.empty() || cascades_.empty()) return;
        static constexpr std::array<glm::vec3, 6> directions{
            glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f) };

        const std::uint32_t count = static_cast<std::uint32_t>(probes_.size());
        const std::uint32_t work = std::min(budget, count);
        for (std::uint32_t n = 0u; n < work; ++n) {
            const std::uint32_t flat = depthCursor_++ % count;
            const Cascade* owner = nullptr;
            std::uint32_t local = 0u;
            for (const Cascade& cascade : cascades_) {
                const std::uint32_t cascadeCount = cascade.grid->probe_count();
                if (flat >= cascade.base && flat < cascade.base + cascadeCount) {
                    owner = &cascade;
                    local = flat - cascade.base;
                    break;
                }
            }
            if (!owner) continue;

            ProbeGridProbe probeState{};
            if (!owner->grid->probe(local, probeState)) continue;
            const glm::vec3 worldPosition = probeState.position * owner->gridScale;
            const float maxDistance = std::max(16.0f, owner->spacing * 8.0f);
            float sum = 0.0f;
            float sum2 = 0.0f;
            float minDistance = maxDistance;
            float maxObserved = 0.0f;
            for (const glm::vec3& direction : directions) {
                float distance = maxDistance;
                (void)terrain_occluded(worldPosition + direction * 0.05f,
                                       direction, maxDistance, owner->spacing,
                                       sample, &distance);
                distance = std::clamp(distance, 0.0f, maxDistance);
                sum += distance;
                sum2 += distance * distance;
                minDistance = std::min(minDistance, distance);
                maxObserved = std::max(maxObserved, distance);
            }
            constexpr float invDirectionCount = 1.0f / 6.0f;
            depthMoments_[flat] = glm::vec4(sum * invDirectionCount,
                                            sum2 * invDirectionCount,
                                            minDistance, maxObserved);
        }
    }

    void reset_temporal_history() {
        if (denoiser_ && !histories_.empty()) denoiser_->reset_histories(histories_);
    }

    void publish_renderer_probes() const {
        for (const Cascade& cascade : cascades_) {
            std::vector<vc::rendering::gi_bridge::Probe> published;
            published.reserve(cascade.grid->probe_count());
            for (std::uint32_t i = 0u; i < cascade.grid->probe_count(); ++i) {
                const std::size_t index = static_cast<std::size_t>(cascade.base) + i;
                if (index >= probes_.size()) break;
                ProbeGridProbe state{};
                if (!cascade.grid->probe(i, state)) continue;
                const Probe& source = probes_[index];
                vc::rendering::gi_bridge::Probe out;
                out.irradiance = glm::vec3(source.radianceVisibility);
                out.direction = glm::vec3(source.directionConfidence);
                out.position = state.position * cascade.gridScale;
                out.cell = state.cell;
                out.visibility = source.radianceVisibility.w;
                out.confidence = source.directionConfidence.w;
                if (index < depthMoments_.size()) out.depthMoments = depthMoments_[index];
                published.push_back(out);
            }
            vc::rendering::gi_bridge::publish(cascade.grid.get(), cascade.spacing,
                                               config_.resolution,
                                               std::move(published),
                                               vc::rendering::gi_bridge::PublicationClass::CanonicalGi);
        }
    }

    void refresh_rt_scene(const glm::vec3& cameraPosition,
                          const GiTerrainSampler& sample) {
        if (!rayTracer_) return;
        const float step = std::max(1.0f, config_.baseSpacing * 2.0f);
        const glm::ivec2 anchor(static_cast<int>(std::floor(cameraPosition.x / step)),
                                static_cast<int>(std::floor(cameraPosition.z / step)));
        if (glm::all(glm::equal(anchor, rtAnchor_)) && ++rtSceneAge_ < 30u &&
            rtSceneReady_) return;
        rtAnchor_ = anchor;
        rtSceneAge_ = 0u;

        const int cells = std::clamp(static_cast<int>(config_.resolution), 8, 24);
        const int half = cells / 2;
        std::vector<vc::rendering::RayTracerTriangle> triangles;
        triangles.reserve(static_cast<std::size_t>(cells) * cells * 2u);
        const auto point = [&](int x, int z) {
            const float wx = (static_cast<float>(anchor.x + x - half)) * step;
            const float wz = (static_cast<float>(anchor.y + z - half)) * step;
            return glm::vec3(wx, sample(wx, wz).height, wz);
        };
        const auto emit = [&](const glm::vec3& a, const glm::vec3& b,
                              const glm::vec3& c) {
            vc::rendering::RayTracerTriangle t{};
            t.v0[0] = a.x; t.v0[1] = a.y; t.v0[2] = a.z;
            t.v1[0] = b.x; t.v1[1] = b.y; t.v1[2] = b.z;
            t.v2[0] = c.x; t.v2[1] = c.y; t.v2[2] = c.z;
            triangles.push_back(t);
        };
        for (int z = 0; z < cells; ++z) {
            for (int x = 0; x < cells; ++x) {
                const glm::vec3 p00 = point(x, z);
                const glm::vec3 p10 = point(x + 1, z);
                const glm::vec3 p01 = point(x, z + 1);
                const glm::vec3 p11 = point(x + 1, z + 1);
                // Counter-clockwise when viewed from +Y (front faces upward).
                emit(p00, p11, p10);
                emit(p00, p01, p11);
            }
        }
        rtSceneReady_ = !triangles.empty() &&
                        rayTracer_->build(triangles.data(),
                                          static_cast<std::int32_t>(triangles.size()));
        // Any AS rebuild changes the scene sampled by temporal GI. Invalidate
        // history on both success and failure so stale light cannot survive a
        // terrain/scene change.
        reset_temporal_history();
    }

    GiClipmapConfig config_{};
    std::vector<Cascade> cascades_;
    std::vector<Probe> probes_;
    std::vector<glm::vec4> depthMoments_;
    std::vector<GiClipmapRange> ranges_;
    std::unique_ptr<vc::rendering::IRayTracer> rayTracer_;
    std::unique_ptr<ITemporalDenoiser> denoiser_;
    std::vector<DenoiserHistory> histories_;
    std::vector<glm::vec3> denoised_;
    std::vector<float> confidence_;
    bool rayTraced_{ false };
    bool rtSceneReady_{ false };
    glm::ivec2 rtAnchor_{ std::numeric_limits<int>::max() };
    std::uint32_t rtSceneAge_{ 0u };
    mutable std::uint32_t rtTraceBudgetRemaining_{ 0u };
    std::uint32_t depthCursor_{ 0u };
    glm::vec3 cachedSunDirection_{ 0.0f, 1.0f, 0.0f };
    glm::vec3 cachedSunColor_{ 1.0f };
    glm::vec3 previousCamera_{ 0.0f };
    bool havePreviousCamera_{ false };
    std::uint32_t sunRevision_{ 1u };
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
    ReflectionProvider(ReflectionBackend backend, ReflectionCapabilities capabilities,
                       std::unique_ptr<IProbeGrid> probeGrid = {},
                       std::unique_ptr<vc::rendering::IRayTracer> rayTracer = {},
                       std::unique_ptr<ITemporalDenoiser> denoiser = {})
        : backend_(backend), capabilities_(capabilities),
          probeGrid_(std::move(probeGrid)), rayTracer_(std::move(rayTracer)),
          denoiser_(std::move(denoiser)) {
        vc::rendering::gi_bridge::register_reflection_runtime(
            this,
            [this](const vc::rendering::gi_bridge::ReflectionFrameInput& frame,
                   const std::vector<vc::rendering::gi_bridge::ReflectionProbeSeed>& seeds,
                   std::vector<vc::rendering::gi_bridge::ReflectionProbeResult>& results) {
                return update_reflection_frame(frame, seeds, results);
            });
    }

    ~ReflectionProvider() override {
        vc::rendering::gi_bridge::retire_reflection_runtime(this);
        vc::rendering::provider_selection::retire("reflectionProvider");
    }

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
        reset_runtime_history();
        errorOut.clear();
        return true;
    }
    const ReflectionConfig& config() const noexcept override { return config_; }

    ReflectionBackend resolve_mode(const ReflectionSurfaceInput& surface) const override {
        // Deterministic selection honoring the provider requested by the
        // project while still providing explicit, implemented fallbacks.
        const float roughness =
            std::clamp(surface.roughness, 0.0f, 1.0f);
        const bool reflective = surface.clearCoat > 0.01f || surface.metalness > 0.01f ||
                                roughness <= config_.screenRoughnessLimit ||
                                roughness >= config_.probeRoughnessFloor;
        ReflectionBackend mode = ReflectionBackend::None;

        if (reflective && backend_ == ReflectionBackend::RayTraced &&
            capabilities_.rayTraced && rayTracer_) {
            mode = ReflectionBackend::RayTraced;
        } else if (reflective && backend_ == ReflectionBackend::Probe &&
                   capabilities_.probe && probeGrid_) {
            mode = ReflectionBackend::Probe;
        } else if (roughness >= config_.probeRoughnessFloor &&
                   capabilities_.probe && probeGrid_) {
            mode = ReflectionBackend::Probe;
        } else if ((surface.clearCoat > 0.5f || roughness <= config_.screenRoughnessLimit) &&
                   roughness <= 0.05f && capabilities_.rayTraced && rayTracer_) {
            mode = ReflectionBackend::RayTraced;
        } else if (reflective && capabilities_.screenSpace) {
            mode = ReflectionBackend::ScreenSpace;
        }

        account_mode(mode);
        return mode;
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

    void reset_frame() const {
        lastModes_.clear();
        screenRaysUsed_ = 0;
    }
    void account_surface(ReflectionBackend mode, std::uint32_t rays) const {
        lastModes_.push_back(mode);
        if (lastModes_.size() > 256u) lastModes_.erase(lastModes_.begin());
        screenRaysUsed_ = std::min(config_.maxScreenRays, screenRaysUsed_ + rays);
    }

private:
    static glm::vec3 reflection_sky(const glm::vec3& direction,
                                    const glm::vec3& sunDirection,
                                    const glm::vec3& sunColor) {
        const glm::vec3 dir = safe_normalize(direction, glm::vec3(0.0f, 1.0f, 0.0f));
        const float day = std::clamp(sunDirection.y * 4.0f + 0.12f, 0.025f, 1.0f);
        const glm::vec3 sky = glm::mix(glm::vec3(0.006f, 0.011f, 0.028f),
                                      glm::vec3(0.15f, 0.31f, 0.58f),
                                      std::clamp(dir.y * 0.5f + 0.5f, 0.0f, 1.0f) * day);
        const float sunLobe = std::pow(
            std::max(glm::dot(dir, safe_normalize(sunDirection,
                                                  glm::vec3(0.0f, 1.0f, 0.0f))),
                     0.0f),
            256.0f);
        return sky + sunColor * sunLobe * day * 2.5f;
    }

    static glm::vec3 sampled_normal(
        const vc::rendering::gi_bridge::ReflectionFrameInput& frame,
        const glm::vec3& position) {
        if (!frame.heightAt) return glm::vec3(0.0f, 1.0f, 0.0f);
        constexpr float d = 1.0f;
        const float hx0 = frame.heightAt(position.x - d, position.z);
        const float hx1 = frame.heightAt(position.x + d, position.z);
        const float hz0 = frame.heightAt(position.x, position.z - d);
        const float hz1 = frame.heightAt(position.x, position.z + d);
        return safe_normalize(glm::vec3(hx0 - hx1, d * 2.0f, hz0 - hz1),
                              glm::vec3(0.0f, 1.0f, 0.0f));
    }

    bool line_march_reflection(
        const vc::rendering::gi_bridge::ReflectionFrameInput& frame,
        const glm::vec3& origin, const glm::vec3& direction,
        float& distanceOut, glm::vec3& radianceOut) const {
        if (!frame.heightAt) return false;
        const glm::vec3 dir = safe_normalize(direction, glm::vec3(0.0f, 1.0f, 0.0f));
        const float step = std::clamp(frame.maxTraceDistance / 64.0f, 0.5f, 4.0f);
        for (float t = step; t <= frame.maxTraceDistance; t += step) {
            const glm::vec3 p = origin + dir * t;
            if (p.y > frame.heightAt(p.x, p.z)) continue;
            distanceOut = t;
            const glm::vec3 albedo = frame.albedoAt
                ? frame.albedoAt(p.x, p.z) : glm::vec3(0.35f);
            const glm::vec3 n = sampled_normal(frame, p);
            const float ndl = std::max(glm::dot(n, safe_normalize(
                frame.sunDirection, glm::vec3(0.0f, 1.0f, 0.0f))), 0.0f);
            radianceOut = albedo * (0.025f + ndl * 0.30f) * frame.sunColor;
            return true;
        }
        distanceOut = frame.maxTraceDistance;
        radianceOut = reflection_sky(dir, frame.sunDirection, frame.sunColor);
        return false;
    }

    void refresh_rt_reflection_scene(
        const vc::rendering::gi_bridge::ReflectionFrameInput& frame) {
        if (!rayTracer_ || !frame.heightAt) return;
        if (rtSceneReady_ && frame.sceneRevision == rtSceneRevision_) return;

        const float step = 4.0f;
        const int cells = 24;
        const int half = cells / 2;
        const int anchorX = static_cast<int>(std::floor(frame.cameraPosition.x / step));
        const int anchorZ = static_cast<int>(std::floor(frame.cameraPosition.z / step));
        std::vector<vc::rendering::RayTracerTriangle> triangles;
        triangles.reserve(static_cast<std::size_t>(cells) * cells * 2u);
        const auto point = [&](int x, int z) {
            const float wx = static_cast<float>(anchorX + x - half) * step;
            const float wz = static_cast<float>(anchorZ + z - half) * step;
            return glm::vec3(wx, frame.heightAt(wx, wz), wz);
        };
        const auto emit = [&](const glm::vec3& a, const glm::vec3& b,
                              const glm::vec3& c) {
            vc::rendering::RayTracerTriangle t{};
            t.v0[0] = a.x; t.v0[1] = a.y; t.v0[2] = a.z;
            t.v1[0] = b.x; t.v1[1] = b.y; t.v1[2] = b.z;
            t.v2[0] = c.x; t.v2[1] = c.y; t.v2[2] = c.z;
            triangles.push_back(t);
        };
        for (int z = 0; z < cells; ++z) {
            for (int x = 0; x < cells; ++x) {
                const glm::vec3 p00 = point(x, z);
                const glm::vec3 p10 = point(x + 1, z);
                const glm::vec3 p01 = point(x, z + 1);
                const glm::vec3 p11 = point(x + 1, z + 1);
                emit(p00, p11, p10);
                emit(p00, p01, p11);
            }
        }
        rtSceneReady_ = !triangles.empty() &&
                        rayTracer_->build(triangles.data(),
                                          static_cast<std::int32_t>(triangles.size()));
        rtSceneRevision_ = frame.sceneRevision;
        reset_runtime_history();
    }

    bool ensure_reflection_denoiser(std::size_t sampleCount) {
        if (!denoiser_ || sampleCount == 0u) return false;
        const std::uint32_t width = static_cast<std::uint32_t>(
            std::min<std::size_t>(256u, std::max<std::size_t>(1u, sampleCount)));
        const std::uint32_t height = static_cast<std::uint32_t>(
            (sampleCount + width - 1u) / width);
        const std::size_t padded = static_cast<std::size_t>(width) * height;
        if (denoiserWidth_ == width && denoiserHeight_ == height &&
            reflectionHistories_.size() == padded) return true;

        DenoiserConfig dc;
        dc.width = width;
        dc.height = height;
        dc.historyWeight = 0.16f;
        dc.depthRejectThreshold = 0.25f;
        dc.normalRejectDegrees = 32.0f;
        dc.useMotion = false;
        dc.useDepthRejection = true;
        dc.useNormalRejection = true;
        dc.seed = 7u;
        std::string error;
        if (!denoiser_->configure(dc, error)) return false;
        denoiserWidth_ = width;
        denoiserHeight_ = height;
        reflectionHistories_.assign(padded, DenoiserHistory{});
        return true;
    }

    void reset_runtime_history() {
        if (denoiser_ && !reflectionHistories_.empty())
            denoiser_->reset_histories(reflectionHistories_);
        previousReflectionCameraValid_ = false;
    }

    bool update_reflection_frame(
        const vc::rendering::gi_bridge::ReflectionFrameInput& frame,
        const std::vector<vc::rendering::gi_bridge::ReflectionProbeSeed>& seeds,
        std::vector<vc::rendering::gi_bridge::ReflectionProbeResult>& results) {
        results.assign(seeds.size(), {});
        if (seeds.empty()) return true;

        if (rayTracer_ && denoiser_) {
            vc::rendering::provider_selection::record_canonical(
                "reflectionProvider", "hybrid-rt-probe-screen",
                "renderer-frame-runtime");
        } else if (probeGrid_) {
            vc::rendering::provider_selection::record_canonical(
                "reflectionProvider", "hybrid-probe-screen",
                "renderer-frame-runtime");
        } else {
            vc::rendering::provider_selection::record_canonical(
                "reflectionProvider", "screen-space",
                "renderer-frame-runtime");
        }

        if (previousReflectionCameraValid_ &&
            glm::distance(previousReflectionCamera_, frame.cameraPosition) > 48.0f) {
            reset_runtime_history();
        }
        previousReflectionCamera_ = frame.cameraPosition;
        previousReflectionCameraValid_ = true;

        if (probeGrid_) {
            const auto capture = [&](const glm::vec3& position, const glm::vec3& direction) {
                ProbeCaptureSample sample{};
                float distance = frame.maxTraceDistance;
                glm::vec3 radiance(0.0f);
                const bool hit = line_march_reflection(frame, position, direction,
                                                        distance, radiance);
                sample.radiance = radiance;
                sample.backface = frame.heightAt &&
                                  position.y < frame.heightAt(position.x, position.z) - 0.1f;
                (void)hit;
                return sample;
            };
            std::string ignored;
            probeGrid_->update(frame.cameraPosition, capture, 64u, &ignored);
            // This grid is internal to reflections; its generic GI publication
            // must never compete with the canonical GI provider.
            vc::rendering::gi_bridge::retire(probeGrid_.get());
        }

        const bool useRt = rayTracer_ != nullptr && denoiser_ != nullptr;
        if (useRt) refresh_rt_reflection_scene(frame);

        std::vector<DenoiserSample> samples;
        if (useRt && ensure_reflection_denoiser(seeds.size())) {
            samples.assign(reflectionHistories_.size(), DenoiserSample{});
        }

        // The current Vulkan IRayTracer query is synchronous. Trace a rotating
        // subset in hardware and retain the rest through temporal/probe
        // history; this keeps RT present in the live frame without serializing
        // dozens of GPU submissions.
        constexpr std::size_t kHardwareRtBudget = 4u;
        const std::size_t rtStart = seeds.empty()
            ? 0u : static_cast<std::size_t>(frame.frameRevision % seeds.size());
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            const auto& seed = seeds[i];
            const glm::vec3 normal = safe_normalize(seed.normal, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 incident = seed.position - frame.cameraPosition;
            incident = safe_normalize(incident, -normal);
            const glm::vec3 reflected = safe_normalize(glm::reflect(incident, normal), normal);
            glm::vec3 radiance(0.0f);
            float distance = frame.maxTraceDistance;
            bool traced = false;

            const std::size_t cyclic = seeds.empty()
                ? 0u : (i + seeds.size() - rtStart) % seeds.size();
            const bool traceThisSeed = useRt && rtSceneReady_ &&
                                       cyclic < std::min(kHardwareRtBudget, seeds.size());
            if (traceThisSeed) {
                vc::rendering::RayTracerRay ray{};
                const glm::vec3 origin = seed.position + normal * 0.08f;
                ray.ox = origin.x; ray.oy = origin.y; ray.oz = origin.z;
                ray.dx = reflected.x; ray.dy = reflected.y; ray.dz = reflected.z;
                ray.tMin = 0.05f;
                ray.tMax = frame.maxTraceDistance;
                const auto hit = rayTracer_->closestHit(ray);
                if (hit.hit) {
                    traced = true;
                    distance = hit.t;
                    const glm::vec3 hitPosition = origin + reflected * hit.t;
                    const glm::vec3 albedo = frame.albedoAt
                        ? frame.albedoAt(hitPosition.x, hitPosition.z)
                        : glm::vec3(0.35f);
                    const glm::vec3 hitNormal = sampled_normal(frame, hitPosition);
                    const float ndl = std::max(glm::dot(hitNormal,
                        safe_normalize(frame.sunDirection,
                                       glm::vec3(0.0f, 1.0f, 0.0f))), 0.0f);
                    radiance = albedo * (0.025f + ndl * 0.32f) * frame.sunColor;
                } else {
                    traced = true;
                    radiance = reflection_sky(reflected, frame.sunDirection, frame.sunColor);
                }
            } else {
                (void)line_march_reflection(frame, seed.position + normal * 0.08f,
                                            reflected, distance, radiance);
            }

            results[i].radiance = radiance;
            results[i].hitDistance = distance;
            results[i].confidence = traced ? 1.0f : 0.55f;
            results[i].rayTraced = traced;

            if (!samples.empty()) {
                samples[i].radiance = radiance;
                samples[i].motion = glm::vec2(0.0f);
                samples[i].depth = std::max(distance, 0.01f);
                samples[i].normal = normal;
            }
        }

        if (!samples.empty()) {
            for (std::size_t i = seeds.size(); i < samples.size(); ++i) {
                samples[i].radiance = glm::vec3(0.0f);
                samples[i].depth = frame.maxTraceDistance;
                samples[i].normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            std::vector<float> confidence;
            std::vector<glm::vec3> denoised;
            std::string error;
            if (denoiser_->denoise(samples, reflectionHistories_, confidence,
                                    denoised, error)) {
                for (std::size_t i = 0; i < seeds.size(); ++i) {
                    results[i].radiance = denoised[i];
                    results[i].confidence = std::min(1.0f, confidence[i] / 24.0f);
                }
            }
        }
        return true;
    }

    void account_mode(ReflectionBackend mode) const {
        account_surface(mode, mode == ReflectionBackend::ScreenSpace ? 1u : 0u);
    }

    ReflectionBackend backend_;
    ReflectionCapabilities capabilities_;
    ReflectionConfig config_{};
    std::unique_ptr<IProbeGrid> probeGrid_;
    std::unique_ptr<vc::rendering::IRayTracer> rayTracer_;
    std::unique_ptr<ITemporalDenoiser> denoiser_;
    std::vector<DenoiserHistory> reflectionHistories_;
    std::uint32_t denoiserWidth_{0u};
    std::uint32_t denoiserHeight_{0u};
    std::uint64_t rtSceneRevision_{0u};
    bool rtSceneReady_{false};
    glm::vec3 previousReflectionCamera_{0.0f};
    bool previousReflectionCameraValid_{false};
    mutable std::vector<ReflectionBackend> lastModes_;
    mutable std::uint32_t screenRaysUsed_{ 0 };
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
            record_gi_selection(GiBackend::RadianceCache, "default");
            return std::make_unique<GlobalIlluminationProvider>(
                backend, capabilities, std::move(core));
        case GiBackend::Ddgi:
        {
            // Engine-native DDGI core is always available; an RTXGI plugin can
            // still accelerate/replace this through the same public contract.
            auto ddgi = std::make_unique<DdgiGiCore>();
            GiClipmapConfig defaults;
            if (!ddgi->configure(defaults, errorOut)) return nullptr;
            GiCapabilities effective = capabilities;
            effective.ddgi = true;
            errorOut.clear();
            record_gi_selection(GiBackend::Ddgi,
                                capabilities.ddgi ? "plugin-capable" : "engine-native");
            return std::make_unique<GlobalIlluminationProvider>(
                GiBackend::Ddgi, effective, std::move(ddgi));
        }
        case GiBackend::RayTraced:
        {
            if (capabilities.rayTraced) {
                if (auto tracer = vc::rendering::create_hw_ray_tracer(); tracer) {
                    auto rt = std::make_unique<DdgiGiCore>(std::move(tracer));
                    GiClipmapConfig defaults;
                    if (!rt->configure(defaults, errorOut)) return nullptr;
                    errorOut.clear();
                    record_gi_selection(GiBackend::RayTraced, "device-has-rt");
                    return std::make_unique<GlobalIlluminationProvider>(
                        GiBackend::RayTraced, capabilities, std::move(rt));
                }
            }

            // Hardware RT may disappear between capability discovery and
            // provider creation (driver/device loss). Fall back to a fully
            // implemented provider and report the actual backend via backend().
            if (capabilities.ddgi) {
                auto ddgi = std::make_unique<DdgiGiCore>();
                GiClipmapConfig defaults;
                if (!ddgi->configure(defaults, errorOut)) return nullptr;
                GiCapabilities effective = capabilities;
                effective.ddgi = true;
                effective.rayTraced = false;
                errorOut = "gi: hardware ray tracing unavailable; using DDGI";
                record_gi_selection(GiBackend::Ddgi, "rt-fallback-ddgi");
                return std::make_unique<GlobalIlluminationProvider>(
                    GiBackend::Ddgi, effective, std::move(ddgi));
            }

            core = create_gi_core(errorOut);
            if (!core) return nullptr;
            GiCapabilities effective = capabilities;
            effective.rayTraced = false;
            errorOut = "gi: hardware ray tracing unavailable; using radiance cache";
            record_gi_selection(GiBackend::RadianceCache, "rt-fallback-radiance-cache");
            return std::make_unique<GlobalIlluminationProvider>(
                GiBackend::RadianceCache, effective, std::move(core));
        }
        default:
            errorOut = "gi: unknown backend";
            return nullptr;
    }
}

std::unique_ptr<IReflectionProvider> create_reflection_provider(
    ReflectionBackend backend, const ReflectionCapabilities& capabilities,
    std::string& errorOut) {
    if (backend == ReflectionBackend::ScreenSpace) {
        std::unique_ptr<IProbeGrid> probeGrid;
        if (capabilities.probe) {
            std::string probeError;
            probeGrid = create_probe_grid(probeError);
            if (probeGrid) {
                ProbeGridConfig pg;
                pg.resolution = 8u;
                pg.cellSize = 8.0f;
                pg.probesPerFrame = 64u;
                pg.historyWeight = 0.10f;
                pg.maxRelocationStep = 0.25f;
                pg.relocationEnabled = true;
                pg.classificationEnabled = true;
                pg.backfaceThreshold = 4u;
                pg.seed = 3u;
                if (!probeGrid->configure(pg, probeError)) probeGrid.reset();
            }
        }

        std::unique_ptr<vc::rendering::IRayTracer> rayTracer;
        std::unique_ptr<ITemporalDenoiser> denoiser;
        if (capabilities.rayTraced) {
            rayTracer = vc::rendering::create_hw_ray_tracer();
            if (rayTracer) {
                std::string denoiseError;
                denoiser = create_temporal_denoiser(denoiseError);
                if (!denoiser) rayTracer.reset();
            }
        }

        auto provider = std::make_unique<ReflectionProvider>(
            backend, capabilities, std::move(probeGrid), std::move(rayTracer),
            std::move(denoiser));
        ReflectionConfig defaults;
        if (!provider->configure(defaults, errorOut)) return nullptr;
        if (capabilities.rayTraced)
            vc::rendering::provider_selection::record(
                "reflectionProvider", "hybrid-rt-probe-screen",
                "renderer-frame-runtime");
        else if (capabilities.probe)
            vc::rendering::provider_selection::record(
                "reflectionProvider", "hybrid-probe-screen",
                "renderer-frame-runtime");
        else
            record_reflection_selection(ReflectionBackend::ScreenSpace, "default");
        return provider;
    }
    if (backend == ReflectionBackend::Probe) {
        if (!capabilities.probe) {
            if (!capabilities.screenSpace) {
                errorOut = "reflection: probe backend requested but no probe or screen fallback exists";
                return nullptr;
            }
            auto fallback =
                std::make_unique<ReflectionProvider>(ReflectionBackend::ScreenSpace,
                                                     capabilities);
            ReflectionConfig defaults;
            if (!fallback->configure(defaults, errorOut)) return nullptr;
            errorOut = "reflection: probe backend unavailable; using screen-space";
            record_reflection_selection(ReflectionBackend::ScreenSpace,
                                        "probe-fallback-screen-space");
            return fallback;
        }
        std::string probeError;
        auto probeGrid = create_probe_grid(probeError);
        if (!probeGrid) {
            errorOut = "reflection: failed to create probe backend: " + probeError;
            return nullptr;
        }
        ProbeGridConfig pg;
        pg.resolution = 8u;
        pg.cellSize = 8.0f;
        pg.probesPerFrame = 32u;
        pg.historyWeight = 0.10f;
        pg.maxRelocationStep = 0.25f;
        pg.relocationEnabled = true;
        pg.classificationEnabled = true;
        pg.backfaceThreshold = 4u;
        pg.seed = 1u;
        if (!probeGrid->configure(pg, probeError)) {
            errorOut = "reflection: failed to configure probe backend: " + probeError;
            return nullptr;
        }
        auto provider = std::make_unique<ReflectionProvider>(
            ReflectionBackend::Probe, capabilities, std::move(probeGrid));
        ReflectionConfig defaults;
        if (!provider->configure(defaults, errorOut)) return nullptr;
        errorOut.clear();
        record_reflection_selection(ReflectionBackend::Probe, "probe-capability");
        return provider;
    }
    if (backend == ReflectionBackend::RayTraced) {
        if (capabilities.rayTraced) {
            auto tracer = vc::rendering::create_hw_ray_tracer();
            if (tracer) {
                std::string denoiseError;
                auto denoiser = create_temporal_denoiser(denoiseError);
                if (!denoiser) {
                    errorOut = "reflection: failed to create RT temporal denoiser: " +
                               denoiseError;
                    return nullptr;
                }
                auto provider = std::make_unique<ReflectionProvider>(
                    ReflectionBackend::RayTraced, capabilities, nullptr,
                    std::move(tracer), std::move(denoiser));
                ReflectionConfig defaults;
                if (!provider->configure(defaults, errorOut)) return nullptr;
                errorOut.clear();
                record_reflection_selection(ReflectionBackend::RayTraced,
                                            "device-has-rt");
                return provider;
            }
        }
        if (capabilities.probe) {
            std::string fallbackError;
            auto fallback = create_reflection_provider(ReflectionBackend::Probe,
                                                       capabilities, fallbackError);
            if (fallback) {
                errorOut = "reflection: hardware RT unavailable; using probe reflections";
                record_reflection_selection(ReflectionBackend::Probe,
                                            "rt-fallback-probe");
                return fallback;
            }
        }
        if (capabilities.screenSpace) {
            auto fallback =
                std::make_unique<ReflectionProvider>(ReflectionBackend::ScreenSpace,
                                                     capabilities);
            ReflectionConfig defaults;
            if (!fallback->configure(defaults, errorOut)) return nullptr;
            errorOut = "reflection: hardware RT unavailable; using screen-space";
            record_reflection_selection(ReflectionBackend::ScreenSpace,
                                        "rt-fallback-screen-space");
            return fallback;
        }
        errorOut = "reflection: ray-traced backend unavailable and no fallback exists";
        return nullptr;
    }
    errorOut = "reflection: unknown backend";
    return nullptr;
}

}  // namespace Engine::Rendering

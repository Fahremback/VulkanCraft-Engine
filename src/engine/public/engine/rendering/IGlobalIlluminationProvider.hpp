#pragma once

// IGlobalIlluminationProvider — Agente 1 (task_plan A.1), the PUBLIC global-
// illumination contract. One surface for the renderer, the editor debug views,
// scripting/CLI/MCP and the profiler to configure and observe GI without
// depending on the concrete backend.
//
// PLUGIN ARCHITECTURE (mirrors IHairProvider): the concrete backends are
// OPTIONAL providers behind one contract, selected DATA-DRIVEN by the project
// + a capability check. A requested backend the runtime cannot provide is
// REFUSED with a diagnostic instead of silently falling back, so a missing
// backend is never mistaken for working GI.
//
// The contract is SPLIT in two halves:
//   1. The PURE core (`IGiCore`) — the deterministic, headless bake/scheduling
//      math (toroidal probe clipmaps, per-cascade budgets, sun-revision
//      invalidation, per-probe diffuse evaluation). This is self-contained
//      (std + glm), no Vulkan, bit-exact for the same inputs on every machine.
//      It is the consolidated public nucleus of the existing `RadianceCache`
//      (task_plan A.2: toroidal clipmaps, buffers layout and budgets preserved).
//   2. `IGlobalIlluminationProvider` — the lifecycle (init/cleanup) and the
//      GPU/backend-specific descriptors and upload recording, which the Vulkan
//      renderer calls directly.
//
// Self-contained (std + glm) for the pure core. Deterministic: identical
// config + terrain-sample function + camera/sun sequence reproduce bit-exact
// probe radiance.

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- capability surface (data-driven selection + capability check) ----

// The concrete GI backends the project may select. Selection is data-driven
// (project config) and gated by a capability check so the engine never
// advertises a backend that is not compiled in.
enum class GiBackend : std::uint8_t {
    // Toroidal diffuse-radiance probe clipmaps — the consolidated public
    // nucleus (task_plan A.2). Implemented; always available.
    RadianceCache,
    // DDGI / RTXGI radiance probes (scroll + relocation/classification).
    // Optional specialized plugin (rtxgi). Refused when not linked.
    Ddgi,
    // Ray-traced hardware GI (task_plan A.8). Refused when the device has no
    // ray tracing support or the backend is not compiled in.
    RayTraced,

    Count
};

struct GiCapabilities {
    bool radianceCache{ true };  // the pure core is always available
    bool ddgi{ false };          // rtxgi plugin linked?
    bool rayTraced{ false };     // hardware ray tracing present?
};

// ---- pure config (validated all-or-nothing via JSON or struct) ----

// One pure probe-clipmap configuration. Fields mirror the existing
// RadianceCache::Config (preserved from task_plan A.2).
struct GiClipmapConfig {
    std::uint32_t cascadeCount{ 6 };      // [1, 6]
    std::uint32_t resolution{ 16 };       // probes per axis [4, 32]
    std::uint32_t probesPerFrame{ 192 };  // bake budget [1, ...]
    float baseSpacing{ 4.0f };            // meters, >= 0.5
    float cascadeScale{ 4.0f };           // >= 2
    float sunRefreshAngleDegrees{ 2.0f }; // [0.25, 15] — sun-revision threshold
};

// The terrain/geometry sample the pure bake consumes (the handoff 3->1:
// voxel snapshots / surface height + albedo). The engine adapter binds this to
// the real TerrainGenerator; tests and headless bakes bind a synthetic one.
struct GiSurfaceSample {
    float height{ 0.0f };            // world-space surface height (Y)
    glm::vec3 albedo{ 0.5f };       // linear diffuse albedo
};

// The seam: returns the surface sample at a world XZ. Must be deterministic
// (pure function of the inputs).
using GiTerrainSampler = std::function<GiSurfaceSample(float worldX, float worldZ)>;

// A pure clipmap range descriptor (cascade bounds + spacing).
struct GiClipmapRange {
    glm::ivec3 minCell{ 0 };
    int resolution{ 0 };
    float inverseSpacing{ 1.0f };
};

// ---- the deterministic, headless pure core (task_plan A.2) ----

class IGiCore {
public:
    virtual ~IGiCore() = default;

    // Validates and applies the clipmap config (all-or-nothing: out-of-range
    // values are refused, never clamped — mirroring the other public configs).
    virtual bool configure(const GiClipmapConfig& config,
                           std::string& errorOut) = 0;
    virtual const GiClipmapConfig& config() const noexcept = 0;

    // Loads the config from JSON {cascadeCount, resolution, probesPerFrame,
    // baseSpacing, cascadeScale, sunRefreshAngleDegrees}. version != 1 or a
    // malformed field refuses all-or-nothing (config unchanged).
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Advances the bake for one frame budget: scrolls the toroidal clipmaps to
    // `cameraPosition`, detects a sun change beyond the refresh threshold (a
    // NEW sun revision re-queues every probe), and spends at most
    // `probesPerFrame` (or `budgetOverride` when > 0) evaluations across
    // cascades with the preserved fixed-share weights. Returns how many probes
    // were (re)baked this frame. Idempotent and deterministic: the same
    // (camera, sun, sampler, budget) sequence reproduces the same radiance.
    virtual std::uint32_t update(const glm::vec3& cameraPosition,
                                 const glm::vec3& sunDirection,
                                 const glm::vec3& sunColor,
                                 const GiTerrainSampler& sampler,
                                 std::uint32_t budgetOverride = 0) = 0;

    // The probe radiance values, in the deterministic cache layout (cascade
    // order, toroidal local indexing — same order as the GPU buffer).
    struct Probe {
        glm::vec4 radianceVisibility{ 0.0f };   // RGB irradiance, A sky visibility
        glm::vec4 directionConfidence{ 0.0f, 1.0f, 0.0f, 0.0f };
        glm::ivec4 worldCellCascade{ 0, 0, 0, -1 };
    };

    virtual std::uint32_t total_probe_count() const noexcept = 0;
    virtual std::uint32_t pending_probe_count() const = 0;
    virtual bool probe(std::uint32_t index, Probe& out) const = 0;
    virtual std::uint32_t sun_revision() const noexcept = 0;

    // Cascade metadata for debug views / profiler.
    virtual std::vector<GiClipmapRange> clipmap_ranges() const = 0;
};

// ---- public factory (the deterministic headless part) ----

std::unique_ptr<IGiCore> create_gi_core(std::string& errorOut);
std::unique_ptr<IGiCore> create_gi_core_json(const std::string& jsonText,
                                             std::string& errorOut);

// ---- the full provider (pure core + lifecycle) ----

// Opaque GPU lifecycle: the renderer owns the Vulkan device/allocator; the
// provider receives them through the Vulkan adapter (task_plan D), never
// through the pure core. `IGlobalIlluminationProvider` is the seam that a
// Vulkan backend implements; the pure `IGiCore` above is always headless.
class IGlobalIlluminationProvider {
public:
    virtual ~IGlobalIlluminationProvider() = default;

    virtual GiBackend backend() const noexcept = 0;
    virtual GiCapabilities capabilities() const noexcept = 0;
    virtual IGiCore& core() = 0;
    virtual const IGiCore& core() const = 0;
};

// Creates the provider for a requested backend with a capability check. The
// pure RadianceCache backend is always available; Ddgi / RayTraced are REFUSED
// with a diagnostic when not linked (the capability tells the truth). The
// caller passes the capability it observed from the device/plugin loader.
std::unique_ptr<IGlobalIlluminationProvider> create_global_illumination_provider(
    GiBackend backend, const GiCapabilities& capabilities, std::string& errorOut);

}  // namespace Engine::Rendering
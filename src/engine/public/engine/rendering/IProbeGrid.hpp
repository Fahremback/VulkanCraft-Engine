#pragma once

// IProbeGrid — Agente 1 (task_plan A.10), the PUBLIC DDGI/radiance-probe grid
// contract. One surface for the renderer, editor debug views, scripting and the
// profiler to run the probe-grid pipeline without depending on the concrete
// backend.
//
// SCOPE: the deterministic, headless ALGORITHM — a toroidal 3D probe grid that
// scrolls with the camera (clipmap), captures per-probe radiance through a
// sampler seam, accumulates history (exponential moving average), RELOCATES
// probes toward regions of high directional variance (clamped per frame) and
// CLASSIFIES probes stuck inside geometry (backface detection) resetting them,
// all under a per-frame budget. This is the pure nucleus of the RTXGI/DDGI
// vendor pipeline; the GPU implementation is the provider seam a Vulkan
// backend implements later. Self-contained (std + glm), bit-exact for the same
// inputs on every machine.
//
// MATH: each probe lives at a world cell center plus a relocation offset
// clamped to half a cell. Every update captures radiance along the six axes
// (+/-X/Y/Z); the backface flags drive classification (>= threshold backfaces
// => inside geometry => history reset + a one-cell step along the least
// occluded axis). Otherwise the accumulated irradiance follows an EMA with
// weight `historyWeight`, and relocation drifts the probe along the dominant
// capture direction by maxRelocationStep * dominance (dominance = (Lmax - Lmean)
// / (Lmax + eps), 0 for uniform light). All decisions are pure functions of the
// sampler and the deterministic iteration order.

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct ProbeGridConfig {
    std::uint32_t resolution{ 8 };        // probes per axis [2, 32] (grid = N^3)
    float cellSize{ 4.0f };               // world meters per cell [0.5, 64]
    std::uint32_t probesPerFrame{ 32 };   // update budget per frame [1, ...]
    float historyWeight{ 0.1f };          // EMA alpha [0.01, 1]
    float maxRelocationStep{ 0.5f };      // fraction of cellSize per frame [0, 1]
    bool relocationEnabled{ true };       // variance-driven drift
    bool classificationEnabled{ true };   // backface reset + step out
    std::uint32_t backfaceThreshold{ 4 }; // backfaces out of 6 to classify [1, 6]
    std::uint32_t seed{ 1 };              // deterministic tie-break seed

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the capture seam ----

// One directional radiance sample at a probe position, with the occlusion flag
// the classification step consumes (true = geometry behind this direction).
struct ProbeCaptureSample {
    glm::vec3 radiance{ 0.0f };
    bool backface{ false };
};

// The seam: captures radiance (and backface) seen from `probeWorldPos` toward
// `dir`. Must be deterministic (pure function of the inputs). The engine
// adapter binds this to the ray-traced / baked scene; tests bind a synthetic
// one. A null sampler is refused by update().
using ProbeCaptureSampler = std::function<ProbeCaptureSample(const glm::vec3& probeWorldPos,
                                                             const glm::vec3& dir)>;

// ---- readable probe state ----

struct ProbeGridProbe {
    glm::vec3 irradiance{ 0.0f };    // accumulated (EMA) average radiance
    glm::vec3 position{ 0.0f };      // world capture point (cell center + offset)
    glm::vec3 offset{ 0.0f };        // relocation offset from the cell center
    glm::ivec3 cell{ 0 };            // world cell this slot currently holds
    std::uint32_t age{ 0 };          // frames since the slot was (re)allocated
    std::uint32_t flags{ 0 };        // bit0: fresh this frame, bit1: relocated, bit2: classified
    std::uint32_t resets{ 0 };       // classification resets seen
    std::uint32_t slot{ 0 };         // stable slot index
};

// ---- the deterministic, headless core (task_plan A.10) ----

class IProbeGrid {
public:
    virtual ~IProbeGrid() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const ProbeGridConfig& config,
                           std::string& errorOut) = 0;
    virtual const ProbeGridConfig& config() const noexcept = 0;

    // JSON {resolution, cellSize, probesPerFrame, historyWeight,
    // maxRelocationStep, relocationEnabled, classificationEnabled,
    // backfaceThreshold, seed, version:1}. version != 1 or a malformed field
    // refuses all-or-nothing (config unchanged).
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Advances the grid one frame: scrolls the toroidal window to
    // `cameraPosition` (cells that left the window are recycled to the newly
    // entered cells, fresh), then spends at most `probesPerFrame` (or
    // `budgetOverride` when > 0) updates round-robin. Returns how many probes
    // were updated this frame. Idempotent and deterministic: the same
    // (camera, sampler, budget) sequence reproduces the same irradiance
    // bit-exact. Refuses a null `sampler`.
    virtual std::uint32_t update(const glm::vec3& cameraPosition,
                                 const ProbeCaptureSampler& sampler,
                                 std::uint32_t budgetOverride = 0,
                                 std::string* errorOut = nullptr) = 0;

    virtual std::uint32_t probe_count() const noexcept = 0;
    virtual bool probe(std::uint32_t slot, ProbeGridProbe& out) const = 0;

    // Frame diagnostics for debug views / profiler.
    virtual std::uint32_t relocation_count() const noexcept = 0;
    virtual std::uint32_t classification_count() const noexcept = 0;
};

// ---- public factory ----

std::unique_ptr<IProbeGrid> create_probe_grid(std::string& errorOut);
std::unique_ptr<IProbeGrid> create_probe_grid_json(const std::string& jsonText,
                                                   std::string& errorOut);

}  // namespace Engine::Rendering

#pragma once

// IReSTIRDI — Agente 1 (task_plan A.9), the PUBLIC ReSTIR Direct-Illumination
// contract. One surface for the renderer, the editor debug views, scripting and
// the profiler to run the ReSTIR candidate/reservoir/reuse/resolve pipeline
// without depending on the concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM — candidate generation from a
// light list, streaming reservoir updates (weighted resampling), temporal and
// spatial reuse, an optional visibility seam and the final resolve. This is the
// pure nucleus the engine already reserves ABI space for in
// `RadianceCache::ReservoirGpu` (48 bytes, std430). The GPU implementation of
// the same pipeline (RTXDI vendor code compiled in) is the provider seam a
// Vulkan backend implements later; this contract is self-contained (std + glm)
// and bit-exact for the same inputs on every machine.
//
// MATH (ReSTIR DI, weighted resampling): each pixel streams M candidate lights
// x_i with weight w_i = p_hat(x_i) / p_sample(x_i), accumulating W = sum(w_i)
// and accepting x_i with probability w_i / W. The target function is the
// diffuse integrand p_hat = radiance * cos(n, dir), so the unbiased irradiance
// estimator of a reservoir (y, W, M) is  L = (W / M) * f(y) / p_hat(y) = W / M
// (f = radiance*cos cancels p_hat). Merging reservoirs (temporal/spatial reuse)
// adds their W and M — the merged reservoir is equivalent to having seen the
// combined candidates. A reused sample may be rejected by the visibility seam.

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- the reservoir (48-byte ABI, mirrors RadianceCache::ReservoirGpu) ----

// One accepted light sample plus its accumulated statistics. The layout is
// deliberately std430-compatible so the same struct can back the GPU storage.
struct RestirReservoir {
    glm::vec3 sampleRadiance{ 0.0f };  // radiance of the accepted sample
    float     weightSum{ 0.0f };       // W — accumulated w = p_hat / p_sample
    glm::vec3 sampleDirection{ 0.0f }; // direction toward the accepted light
    std::uint32_t m{ 0 };              // M — candidates seen (incl. reuse)
    std::uint32_t lightId{ 0 };        // stable id of the accepted light
    std::uint32_t age{ 0 };            // frames since the sample was accepted
    std::uint32_t flags{ 0 };          // bit0: valid (has an accepted sample)
    std::uint32_t padding{ 0 };        // std430 padding to the 48-byte ABI
};

static_assert(sizeof(RestirReservoir) == 48);

// ---- inputs ----

// A candidate light sample produced by the candidate-generation pass.
struct RestirLightSample {
    glm::vec3 direction{ 0.0f, 1.0f, 0.0f }; // from shading point toward light
    glm::vec3 radiance{ 0.0f };              // arriving radiance (pre-visibility)
    float pdf{ 1.0f };                       // sampling pdf of this light
    std::uint32_t lightId{ 0 };              // stable id (reuse key)
};

// A light source descriptor used by the built-in candidate generator. Lights
// are sampled by power (pdf_i = power_i / totalPower); the direction is the
// unit vector from the shading point toward `position`.
struct RestirDiLight {
    glm::vec3 position{ 0.0f };
    glm::vec3 radiance{ 1.0f };
    float power{ 1.0f };  // > 0; lights with power <= 0 are refused
    std::uint32_t id{ 0 };
};

// One shaded pixel of the DI pass.
struct RestirDiPixelInput {
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
};

// Per-pixel result of a full DI frame.
struct RestirDiFrameResult {
    std::vector<float> radiance;   // estimated irradiance per pixel (W / M)
    std::vector<RestirReservoir> reservoirs;  // per pixel, for the next frame
    std::vector<std::uint32_t> effectiveM;    // diagnostics: total candidates
    std::vector<std::uint32_t> temporalAccepted;  // reuse accepted count
    std::vector<std::uint32_t> spatialAccepted;   // reuse accepted count
};

// ---- config (validated all-or-nothing) ----

struct RestirConfig {
    std::uint32_t candidateCount{ 8 }; // M candidates per pixel per frame [1, 256]
    std::uint32_t spatialSamples{ 4 }; // K spatial neighbors per pixel [0, 16]
    bool temporalReuse{ true };        // reuse the previous frame reservoir
    bool spatialReuse{ true };         // reuse neighbor reservoirs
    bool visibilityReuse{ true };      // shadow-test reused samples via the seam
    std::uint32_t seed{ 1 };           // deterministic RNG seed

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan A.9) ----

class IReSTIRDI {
public:
    virtual ~IReSTIRDI() = default;

    // Validates and applies the config (all-or-nothing: refused, never
    // clamped — mirroring the other public configs).
    virtual bool configure(const RestirConfig& config, std::string& errorOut) = 0;
    virtual const RestirConfig& config() const noexcept = 0;

    // JSON {candidateCount, spatialSamples, temporalReuse, spatialReuse,
    // visibilityReuse, seed, version:1}. version != 1 or a malformed field
    // refuses all-or-nothing (config unchanged).
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // ---- reservoir math (stateless, deterministic) ----

    // Streaming update: feed M candidates into a reservoir via weighted
    // resampling, with the target p_hat = radiance * cos(normal, dir) evaluated
    // at the shading normal. `seed` drives the acceptance RNG; identical
    // (candidates, normal, seed) reproduce the same reservoir bit-exact.
    virtual RestirReservoir update(const std::vector<RestirLightSample>& candidates,
                                   const glm::vec3& normal,
                                   std::uint32_t seed) = 0;

    // Merge two reservoirs: W = W_a + W_b * correction, M = M_a + M_b; accepts
    // a's sample with probability W_a / (W_a + W_b * correction). `correction`
    // re-weights b for the current frame (light change); 1 for a static scene.
    virtual RestirReservoir merge(const RestirReservoir& a,
                                  const RestirReservoir& b,
                                  float bCorrection,
                                  std::uint32_t seed) = 0;

    // The unbiased irradiance estimator of a reservoir: W / M (0 when empty).
    virtual float resolve(const RestirReservoir& r) const noexcept = 0;

    // ---- full DI frame (candidate gen + temporal + spatial + resolve) ----

    // The visibility seam: returns whether the ray from `from` toward `dir` up
    // to `maxT` is unoccluded. Only consulted when config.visibilityReuse is
    // set and a reused sample is merged; an empty callback treats everything
    // as visible. The engine adapter binds this to IRayTracer::occluded.
    using VisibilityFn = std::function<bool(const glm::vec3& from,
                                            const glm::vec3& dir, float maxT)>;

    // Runs one ReSTIR DI frame over `pixels`: per pixel, samples
    // candidateCount lights by power, streams them into a reservoir, merges the
    // previous frame reservoir (temporal) and up to spatialSamples neighbor
    // reservoirs of the current frame (spatial), then resolves. `lights` with
    // power <= 0 are refused. `frameIndex` sequences the per-frame RNG stream
    // (0, 1, 2, ...) so consecutive frames draw different candidates;
    // identical (pixels, lights, prev, frameIndex, seed) reproduce the same
    // radiance bit-exact.
    virtual bool diFrame(const std::vector<RestirDiPixelInput>& pixels,
                         const std::vector<RestirDiLight>& lights,
                         const std::vector<RestirReservoir>& prevReservoirs,
                         std::uint32_t frameIndex,
                         const VisibilityFn& visibility,
                         RestirDiFrameResult& out,
                         std::string& errorOut) = 0;
};

// ---- public factory ----

std::unique_ptr<IReSTIRDI> create_restir_di(std::string& errorOut);
std::unique_ptr<IReSTIRDI> create_restir_di_json(const std::string& jsonText,
                                                 std::string& errorOut);

}  // namespace Engine::Rendering

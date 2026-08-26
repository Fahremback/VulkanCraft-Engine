#pragma once

// ITemporalDenoiser — Agente 1 (task_plan A.11), the PUBLIC NRD-style temporal
// denoiser contract. One surface for GI, reflections and shadows denoising
// without depending on the concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM of a temporal denoiser (the
// ReBLUR/ReLAX family): per-pixel history accumulation driven by motion-vector
// reprojection, depth and normal REJECTION (disocclusion detection), an
// exponential moving average and a confidence output (history length). This is
// the pure nucleus of the NRD vendor pipeline; the GPU implementation is the
// provider seam a Vulkan backend implements later. Self-contained (std + glm),
// bit-exact for the same inputs on every machine.
//
// MATH: each pixel keeps a history (accumulated radiance + the depth/normal it
// was built from). On every frame the history is reprojected through the motion
// vector; if the reprojected history disagrees with the current sample beyond
// the depth/normal thresholds the history is REJECTED (disocclusion -> the
// history restarts from the current sample, so new content refreshes in one
// frame instead of ghosting). Otherwise the history accumulates with an EMA of
// weight `historyWeight`, which reduces variance by ~ alpha/(2-alpha) while
// converging to the true signal. `confidence` is the history length in frames
// (0 for a rejected/first frame) — the same signal the debug views and the
// profiler consume.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct DenoiserConfig {
    std::uint32_t width{ 64 };            // [1, 8192]
    std::uint32_t height{ 64 };           // [1, 8192]
    float historyWeight{ 0.1f };          // EMA alpha [0.01, 1]
    float depthRejectThreshold{ 0.2f };   // relative depth change that rejects [0.01, 4]
    float normalRejectDegrees{ 30.0f };   // normal mismatch that rejects [1, 179]
    bool useMotion{ true };               // motion-vector reprojection
    bool useDepthRejection{ true };       // depth-based disocclusion rejection
    bool useNormalRejection{ true };      // normal-based disocclusion rejection
    std::uint32_t seed{ 1 };              // deterministic (reserved for variance)

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the frame inputs ----

// One pixel of the current noisy signal plus the geometry/motion that drives
// the reprojection and the rejection.
struct DenoiserSample {
    glm::vec3 radiance{ 0.0f };  // current-frame noisy signal (GI/reflection/shadow)
    glm::vec2 motion{ 0.0f };    // motion vector in pixels (where this pixel was last frame)
    float depth{ 1.0f };         // current depth (> 0)
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // current shading normal
};

// ---- per-pixel history (in/out across frames) ----

struct DenoiserHistory {
    glm::vec3 radiance{ 0.0f };  // accumulated (EMA) denoised radiance
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // normal the history was built from
    float depth{ 1.0f };         // depth the history was built from
    std::uint32_t frames{ 0 };   // history length (confidence)
    std::uint32_t flags{ 0 };    // bit0: valid history
};

// ---- the deterministic, headless core (task_plan A.11) ----

class ITemporalDenoiser {
public:
    virtual ~ITemporalDenoiser() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const DenoiserConfig& config,
                           std::string& errorOut) = 0;
    virtual const DenoiserConfig& config() const noexcept = 0;

    // JSON {width, height, historyWeight, depthRejectThreshold,
    // normalRejectDegrees, useMotion, useDepthRejection, useNormalRejection,
    // seed, version:1}. version != 1 or a malformed field refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Clears every history (camera cut / light change): the next frame starts
    // fresh at every pixel.
    virtual void reset_histories(std::vector<DenoiserHistory>& histories) const = 0;

    // Denoises one frame: for every pixel, reprojects the history through the
    // motion vector, rejects it on depth/normal disagreement (or missing
    // reprojection target), otherwise accumulates with the EMA. `samples` and
    // `histories` must both have width*height entries (refused otherwise).
    // `confidenceOut` receives the per-pixel history length and `radianceOut`
    // the denoised signal. Deterministic: identical (samples, histories)
    // reproduce the same outputs bit-exact.
    virtual bool denoise(const std::vector<DenoiserSample>& samples,
                         std::vector<DenoiserHistory>& histories,
                         std::vector<float>& confidenceOut,
                         std::vector<glm::vec3>& radianceOut,
                         std::string& errorOut) = 0;
};

// ---- public factory ----

std::unique_ptr<ITemporalDenoiser> create_temporal_denoiser(std::string& errorOut);
std::unique_ptr<ITemporalDenoiser> create_temporal_denoiser_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering

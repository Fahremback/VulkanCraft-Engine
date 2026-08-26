#pragma once

// ISpatialUpscaler — Agente 1 (task_plan A.12), the PUBLIC FSR-style spatial
// upscaler contract. One surface for upscaling and sharpening (FidelityFX
// class: FSR1 EASU / CAS objectives) without depending on the concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM of edge-adaptive spatial
// upsampling: a bilinear baseline, a Lanczos-2 (4x4) candidate and an
// edge-magnitude blend (sharp kernels on strong edges, clean bilinear on flat
// areas — no ringing). This is the pure nucleus of the FSR vendor pipeline; the
// GPU implementation is the provider seam a Vulkan backend implements later.
// Self-contained (std + glm), bit-exact for the same inputs on every machine.
//
// MATH: every output pixel maps to the source grid and computes B (2x2
// bilinear) and L (4x4 Lanczos-2, weight-sums normalized). A Sobel-style
// gradient on the 4x4 neighborhood yields an edge magnitude `g`; the blend
// factor is smoothstep(edgeLo, edgeHi, g) * sharpness, so flat regions keep the
// exact bilinear (no overshoot) while strong edges get the sharper kernel.
// A fully flat image is reproduced exactly in every mode.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

enum class UpscaleMode : std::uint8_t {
    Bilinear,      // reference baseline
    EdgeAdaptive,  // FSR-style: lanczos-2 blended by edge magnitude
    Count
};

// ---- config (validated all-or-nothing) ----

struct UpscaleConfig {
    std::uint32_t srcWidth{ 64 };   // [1, 8192]
    std::uint32_t srcHeight{ 64 };  // [1, 8192]
    float scale{ 2.0f };            // output = round(src * scale) [1, 8]
    float sharpness{ 1.0f };        // edge-blend strength [0, 1] (0 = bilinear)
    float edgeLo{ 0.05f };          // smoothstep low edge threshold [0.001, 1]
    float edgeHi{ 0.30f };          // smoothstep high edge threshold (>= edgeLo) [0.002, 1]
    UpscaleMode mode{ UpscaleMode::EdgeAdaptive };
    std::uint32_t seed{ 1 };        // deterministic (reserved)

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan A.12) ----

class ISpatialUpscaler {
public:
    virtual ~ISpatialUpscaler() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const UpscaleConfig& config, std::string& errorOut) = 0;
    virtual const UpscaleConfig& config() const noexcept = 0;

    // JSON {srcWidth, srcHeight, scale, sharpness, edgeLo, edgeHi, mode,
    // seed, version:1} with mode "bilinear" | "edge-adaptive". version != 1 or
    // a malformed field refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Upscales `src` (srcWidth*srcHeight*4 RGBA floats, linear) into `dst`
    // (dstWidth*dstHeight*4, dst = round(src * scale)), one frame, clamping
    // the output to [0, 1]. `src` size is validated all-or-nothing (refused on
    // mismatch). Deterministic: identical (src, config) reproduce the same dst
    // bit-exact.
    virtual bool upscale(const std::vector<float>& src,
                         std::vector<float>& dst,
                         std::string& errorOut) = 0;
};

// ---- public factory ----

std::unique_ptr<ISpatialUpscaler> create_spatial_upscaler(std::string& errorOut);
std::unique_ptr<ISpatialUpscaler> create_spatial_upscaler_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering

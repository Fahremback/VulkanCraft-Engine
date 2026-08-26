// ICasSharpening — Agente 1 (task_plan C.5 fidelityfx): the PUBLIC CAS
// (Contrast Adaptive Sharpening) contract. Headless, deterministic, std.
//
// FidelityFX CAS: edge-adaptive sharpening that increases local contrast
// at edges while limiting ringing/overshoot in flat regions. The core is
// pure math: per-pixel contrast detection + adaptive sharpening weight.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string>
#include <vector>

namespace vc::rendering {

struct CasConfig {
    // Sharpness: 0.0 = minimal, 1.0 = maximum. Default 0.4.
    float sharpness = 0.4f;
    // Whether to limit contrast to prevent ringing.
    bool clampValues = true;

    bool validate() const;
    std::string toJson() const;
    static CasConfig fromJson(const std::string& json, std::string& errorOut);
};

// CAS operates on a 64x64 tile (matching FFX CAS threadgroup size).
// Input/output are RGB float [0,1] images.
struct CasTile {
    static constexpr int SIZE = 64;
    // Input: 64x64 RGB floats (row-major, 3 channels per pixel).
    std::vector<float> input;
    // Output: 64x64 RGB floats.
    std::vector<float> output;

    CasTile() : input(SIZE * SIZE * 3, 0.0f), output(SIZE * SIZE * 3, 0.0f) {}
};

class ICasSharpening {
public:
    virtual ~ICasSharpening() = default;

    // Sharpen a single pixel given its 3x3 neighborhood.
    // r/g/b: center pixel values [0,1].
    // neighbors[9]: 3x3 neighborhood (row-major, center at index 4).
    // Returns sharpened RGB values.
    virtual void sharpenPixel(
        float r, float g, float b,
        const float neighbors[9][3],
        float& outR, float& outG, float& outB) const = 0;

    // Sharpen an entire 64x64 tile.
    virtual void sharpenTile(CasTile& tile) const = 0;

    // Compute local contrast for a pixel (used internally).
    // Returns contrast value [0,1].
    virtual float localContrast(
        float r, float g, float b,
        const float neighbors[9][3]) const = 0;
};

std::unique_ptr<ICasSharpening> create_cas_sharpening(
    const CasConfig& config, std::string& errorOut);

} // namespace vc::rendering

// CasSharpening.cpp — adapter for ICasSharpening.
// FidelityFX CAS: Contrast Adaptive Sharpening.
// Pure math, headless, deterministic, no GPU required.

#include "engine/rendering/ICasSharpening.hpp"
#include <cmath>
#include <algorithm>
#include <sstream>

namespace vc::rendering {

// ─── Config ───────────────────────────────────────

bool CasConfig::validate() const {
    return sharpness >= 0.0f && sharpness <= 1.0f;
}

static std::string je3(const std::string& s) {
    std::string o;
    for (char c : s) { if(c=='"')o+="\\\""; else o+=c; }
    return o;
}

std::string CasConfig::toJson() const {
    std::ostringstream o;
    o << "{\"sharpness\":" << sharpness << ",\"clampValues\":" << (clampValues?"true":"false") << "}";
    return o.str();
}

CasConfig CasConfig::fromJson(const std::string& json, std::string& err) {
    CasConfig c;
    auto p = json.find("\"sharpness\"");
    if (p != std::string::npos) {
        p = json.find(':', p + 11);
        if (p != std::string::npos) c.sharpness = std::strtof(json.c_str() + p + 1, nullptr);
    }
    c.clampValues = json.find("\"clampValues\":true") != std::string::npos;
    if (!c.validate()) { err = "invalid config"; return {}; }
    return c;
}

// ─── Adapter ──────────────────────────────────────

class CasSharpeningImpl : public ICasSharpening {
public:
    explicit CasSharpeningImpl(const CasConfig& cfg) : config_(cfg) {
        // Pre-compute the sharpening coefficient from sharpness parameter.
        // CAS formula: peak = -(3/8 * sharpness^2 - 3/4 * sharpness) - 3/8
        // This gives the peak sharpening weight at max contrast.
        float s = cfg.sharpness;
        peak_ = -(3.0f/8.0f * s * s - 3.0f/4.0f * s) - 3.0f/8.0f;
    }

    float localContrast(float r, float g, float b,
                        const float neighbors[9][3]) const override {
        // Compute min/max luminance in 3x3 neighborhood.
        float lumaCenter = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float lumaMin = lumaCenter, lumaMax = lumaCenter;
        for (int i = 0; i < 9; i++) {
            if (i == 4) continue; // Skip center.
            float luma = 0.2126f * neighbors[i][0] + 0.7152f * neighbors[i][1] + 0.0722f * neighbors[i][2];
            lumaMin = std::min(lumaMin, luma);
            lumaMax = std::max(lumaMax, luma);
        }
        float range = lumaMax - lumaMin;
        return range;
    }

    void sharpenPixel(float r, float g, float b,
                      const float neighbors[9][3],
                      float& outR, float& outG, float& outB) const override {
        // CAS: detect local contrast, apply adaptive sharpening weight.
        float lumaC = 0.2126f * r + 0.7152f * g + 0.0722f * b;

        // Compute min/max luminance from 3x3 neighborhood.
        float lumaMin = lumaC, lumaMax = lumaC;
        for (int i = 0; i < 9; i++) {
            if (i == 4) continue;
            float luma = 0.2126f * neighbors[i][0] + 0.7152f * neighbors[i][1] + 0.0722f * neighbors[i][2];
            lumaMin = std::min(lumaMin, luma);
            lumaMax = std::max(lumaMax, luma);
        }

        float range = lumaMax - lumaMin;

        // Sharpening weight: higher at edges, lower in flat regions.
        // Formula: w = clamp(peak * range, 0, peak/2)
        float w = std::max(0.0f, std::min(peak_ * range, peak_ / 2.0f));

        // Apply: out = center + w * (center - weighted_average_of_neighbors)
        float sumR = 0, sumG = 0, sumB = 0;
        for (int i = 0; i < 9; i++) {
            if (i == 4) continue;
            sumR += neighbors[i][0];
            sumG += neighbors[i][1];
            sumB += neighbors[i][2];
        }
        float invCount = 1.0f / 8.0f;
        float avgR = sumR * invCount;
        float avgG = sumG * invCount;
        float avgB = sumB * invCount;

        outR = r + w * (r - avgR);
        outG = g + w * (g - avgG);
        outB = b + w * (b - avgB);

        if (config_.clampValues) {
            outR = std::max(0.0f, std::min(1.0f, outR));
            outG = std::max(0.0f, std::min(1.0f, outG));
            outB = std::max(0.0f, std::min(1.0f, outB));
        }
    }

    void sharpenTile(CasTile& tile) const override {
        for (int y = 0; y < CasTile::SIZE; y++) {
            for (int x = 0; x < CasTile::SIZE; x++) {
                float neighbors[9][3];
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = std::max(0, std::min(CasTile::SIZE-1, x+dx));
                        int ny = std::max(0, std::min(CasTile::SIZE-1, y+dy));
                        int idx = (ny * CasTile::SIZE + nx) * 3;
                        int ni = (dy+1)*3 + (dx+1);
                        neighbors[ni][0] = tile.input[idx+0];
                        neighbors[ni][1] = tile.input[idx+1];
                        neighbors[ni][2] = tile.input[idx+2];
                    }
                }
                int outIdx = (y * CasTile::SIZE + x) * 3;
                sharpenPixel(tile.input[outIdx], tile.input[outIdx+1], tile.input[outIdx+2],
                             neighbors, tile.output[outIdx], tile.output[outIdx+1], tile.output[outIdx+2]);
            }
        }
    }

private:
    CasConfig config_;
    float peak_;
};

std::unique_ptr<ICasSharpening> create_cas_sharpening(
    const CasConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<CasSharpeningImpl>(config);
}

} // namespace vc::rendering

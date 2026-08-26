// SpatialUpscaler.cpp — Agente 1 (task_plan A.12), the deterministic FSR-style
// spatial upscaler pure core behind the public ISpatialUpscaler contract.
//
// Self-contained (std): bilinear baseline, Lanczos-2 (4x4) candidate and an
// edge-magnitude blend (Sobel on the 4x4 neighborhood) — flat regions stay
// exact bilinear (no ringing), strong edges get the sharper kernel. Bit-exact
// for the same inputs on every machine. The GPU seam (FSR vendor code) is out
// of scope here, as for the other pure cores.

#include "engine/rendering/ISpatialUpscaler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

const float kPi = 3.14159265358979f;

float lanczos2(float x) {
    if (x < -2.0f || x > 2.0f) return 0.0f;
    if (x == 0.0f) return 1.0f;
    const float px = kPi * x;
    const float s1 = std::sin(px) / px;
    const float half = px * 0.5f;
    const float s2 = std::sin(half) / half;
    return s1 * s2;
}

float smoothstep(float lo, float hi, float x) {
    const float t = std::clamp((x - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float luminance(const float* rgba) {
    return 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
}

class SpatialUpscaler final : public ISpatialUpscaler {
public:
    SpatialUpscaler() : config_(UpscaleConfig{}) {}

    bool configure(const UpscaleConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const UpscaleConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        UpscaleConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"srcWidth\": " << config_.srcWidth
          << ", \"srcHeight\": " << config_.srcHeight
          << ", \"scale\": " << config_.scale
          << ", \"sharpness\": " << config_.sharpness
          << ", \"edgeLo\": " << config_.edgeLo
          << ", \"edgeHi\": " << config_.edgeHi
          << ", \"mode\": \""
          << (config_.mode == UpscaleMode::Bilinear ? "bilinear" : "edge-adaptive")
          << "\", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    bool upscale(const std::vector<float>& src, std::vector<float>& dst,
                 std::string& errorOut) override {
        const std::uint32_t sw = config_.srcWidth;
        const std::uint32_t sh = config_.srcHeight;
        const std::uint32_t expected = sw * sh * 4u;
        if (src.size() != expected) {
            errorOut = "SpatialUpscaler: src size mismatch";
            return false;
        }
        const std::uint32_t dw =
            std::max(1u, static_cast<std::uint32_t>(std::round(sw * config_.scale)));
        const std::uint32_t dh =
            std::max(1u, static_cast<std::uint32_t>(std::round(sh * config_.scale)));
        dst.assign(dw * dh * 4u, 0.0f);

        const bool edgeAdaptive = config_.mode == UpscaleMode::EdgeAdaptive;

        for (std::uint32_t oy = 0; oy < dh; ++oy) {
            const float fy = (static_cast<float>(oy) + 0.5f) / config_.scale - 0.5f;
            for (std::uint32_t ox = 0; ox < dw; ++ox) {
                const float fx = (static_cast<float>(ox) + 0.5f) / config_.scale - 0.5f;
                float* out = &dst[(static_cast<std::size_t>(oy) * dw + ox) * 4u];

                const int i0 = static_cast<int>(std::floor(fx));
                const int j0 = static_cast<int>(std::floor(fy));
                const float tx = fx - static_cast<float>(i0);
                const float ty = fy - static_cast<float>(j0);

                auto sample = [&](int i, int j, float* rgba) {
                    const int ci = std::clamp(i, 0, static_cast<int>(sw) - 1);
                    const int cj = std::clamp(j, 0, static_cast<int>(sh) - 1);
                    const float* p =
                        &src[(static_cast<std::size_t>(cj) * sw + ci) * 4u];
                    rgba[0] = p[0];
                    rgba[1] = p[1];
                    rgba[2] = p[2];
                    rgba[3] = p[3];
                };

                // ---- bilinear (2x2) ----
                float tl[4], tr[4], bl[4], br[4];
                sample(i0, j0, tl);
                sample(i0 + 1, j0, tr);
                sample(i0, j0 + 1, bl);
                sample(i0 + 1, j0 + 1, br);
                float b[4];
                for (int c = 0; c < 4; ++c) {
                    const float top = tl[c] + (tr[c] - tl[c]) * tx;
                    const float bot = bl[c] + (br[c] - bl[c]) * tx;
                    b[c] = top + (bot - top) * ty;
                }

                if (!edgeAdaptive) {
                    for (int c = 0; c < 4; ++c) out[c] = std::clamp(b[c], 0.0f, 1.0f);
                    continue;
                }

                // ---- lanczos-2 (4x4) ----
                float l[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                float wSum = 0.0f;
                for (int j = j0 - 1; j <= j0 + 2; ++j) {
                    const float wy = lanczos2(fy - static_cast<float>(j));
                    for (int i = i0 - 1; i <= i0 + 2; ++i) {
                        const float wx = lanczos2(fx - static_cast<float>(i));
                        const float w = wx * wy;
                        wSum += w;
                        float tap[4];
                        sample(i, j, tap);
                        for (int c = 0; c < 4; ++c) l[c] += w * tap[c];
                    }
                }
                if (wSum > 1e-9f) {
                    for (int c = 0; c < 4; ++c) l[c] /= wSum;
                }

                // ---- edge magnitude (Sobel on the 3x3 around the quad) ----
                float m[3][3];
                for (int dj = -1; dj <= 1; ++dj) {
                    for (int di = -1; di <= 1; ++di) {
                        float tap[4];
                        sample(i0 + di, j0 + dj, tap);
                        m[dj + 1][di + 1] = luminance(tap);
                    }
                }
                const float gx = (m[0][2] + 2.0f * m[1][2] + m[2][2]) -
                                 (m[0][0] + 2.0f * m[1][0] + m[2][0]);
                const float gy = (m[2][0] + 2.0f * m[2][1] + m[2][2]) -
                                 (m[0][0] + 2.0f * m[0][1] + m[0][2]);
                // A unit step yields a Sobel magnitude of 4: normalize.
                const float gNorm = std::min(std::sqrt(gx * gx + gy * gy) / 4.0f,
                                             1.0f);
                const float blend = smoothstep(config_.edgeLo, config_.edgeHi,
                                               gNorm) *
                                    config_.sharpness;

                // RGB edge-blended; alpha stays bilinear (no ringing on alpha).
                for (int c = 0; c < 3; ++c) {
                    out[c] = std::clamp(b[c] * (1.0f - blend) + l[c] * blend,
                                        0.0f, 1.0f);
                }
                out[3] = std::clamp(b[3], 0.0f, 1.0f);
            }
        }
        return true;
    }

private:
    static bool parseJson(const std::string& text, UpscaleConfig& out,
                          std::string& errorOut) {
        struct Pair {
            std::string key;
            std::string value;
        };
        std::vector<Pair> pairs;
        {
            std::size_t i = 0;
            auto skipWs = [&]() {
                while (i < text.size() &&
                       (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                        text[i] == '\r')) {
                    ++i;
                }
            };
            skipWs();
            if (i >= text.size() || text[i] != '{') {
                errorOut = "SpatialUpscaler config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "SpatialUpscaler config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "SpatialUpscaler config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "SpatialUpscaler config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "SpatialUpscaler config: unterminated string";
                        return false;
                    }
                    ++i;
                } else {
                    while (i < text.size() && text[i] != ',' && text[i] != '}') {
                        value.push_back(text[i++]);
                    }
                }
                pairs.push_back({key, value});
                skipWs();
                if (i < text.size() && text[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < text.size() && text[i] == '}') {
                    ++i;
                    break;
                }
                errorOut = "SpatialUpscaler config: expected ',' or '}'";
                return false;
            }
        }

        UpscaleConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "SpatialUpscaler config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "srcWidth") {
                parsed.srcWidth = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "srcHeight") {
                parsed.srcHeight = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "scale") {
                parsed.scale = std::stof(p.value);
            } else if (p.key == "sharpness") {
                parsed.sharpness = std::stof(p.value);
            } else if (p.key == "edgeLo") {
                parsed.edgeLo = std::stof(p.value);
            } else if (p.key == "edgeHi") {
                parsed.edgeHi = std::stof(p.value);
            } else if (p.key == "mode") {
                if (p.value == "bilinear") {
                    parsed.mode = UpscaleMode::Bilinear;
                } else if (p.value == "edge-adaptive") {
                    parsed.mode = UpscaleMode::EdgeAdaptive;
                } else {
                    errorOut = "SpatialUpscaler config: unknown mode";
                    return false;
                }
            } else if (p.key == "seed") {
                parsed.seed = static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "SpatialUpscaler config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "SpatialUpscaler config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "SpatialUpscaler config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    UpscaleConfig config_{};
};

}  // namespace

bool UpscaleConfig::valid(std::string& errorOut) const {
    if (srcWidth < 1 || srcWidth > 8192 || srcHeight < 1 || srcHeight > 8192) {
        errorOut = "srcWidth/srcHeight must be in [1, 8192]";
        return false;
    }
    if (!(scale >= 1.0f && scale <= 8.0f)) {
        errorOut = "scale must be in [1, 8]";
        return false;
    }
    if (!(sharpness >= 0.0f && sharpness <= 1.0f)) {
        errorOut = "sharpness must be in [0, 1]";
        return false;
    }
    if (!(edgeLo >= 0.001f && edgeLo <= 1.0f)) {
        errorOut = "edgeLo must be in [0.001, 1]";
        return false;
    }
    if (!(edgeHi >= 0.002f && edgeHi <= 1.0f && edgeHi >= edgeLo)) {
        errorOut = "edgeHi must be in [0.002, 1] and >= edgeLo";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    return true;
}

std::unique_ptr<ISpatialUpscaler> create_spatial_upscaler(std::string& errorOut) {
    auto impl = std::make_unique<SpatialUpscaler>();
    if (!impl) {
        errorOut = "SpatialUpscaler: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<ISpatialUpscaler> create_spatial_upscaler_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<SpatialUpscaler>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

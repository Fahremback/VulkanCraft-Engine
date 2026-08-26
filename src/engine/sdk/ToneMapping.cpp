// ToneMapping.cpp — Agente 1 (task_plan B.7), the deterministic HDR tone
// mapping core behind the public IToneMapping contract.
//
// Self-contained (std + glm): EV/manual exposure, ACES / Reinhard / Filmic
// operators and exact linear <-> sRGB conversions. Bit-exact for the same
// inputs on every machine. No clock. The GPU pipeline integration (swapchain
// formats, actual frame buffers) is provider-side, as for the other cores.

#include "engine/rendering/IToneMapping.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

// ACES (Narkowicz 2015) saturate form on [0, inf): works on HDR input
// WITHOUT pre-clamping (the asymptote is ~1.033, so any emissive value maps
// consistently into [0, 1] through the curve, not a hard clamp).
float acesChannel(float x) {
    const float v = std::max(0.0f, x);
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return clamp01((v * (a * v + b)) / (v * (c * v + d) + e));
}

// Uncharted 2 style filmic curve, normalized at the white point.
float uncharted2(float x) {
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

class ToneMapping final : public IToneMapping {
public:
    ToneMapping() : config_(ToneMappingConfig{}) {}

    bool configure(const ToneMappingConfig& config,
                   std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const ToneMappingConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        ToneMappingConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        const char* opName = "aces";
        switch (config_.op) {
            case ToneOperator::Reinhard: opName = "reinhard"; break;
            case ToneOperator::ACES: opName = "aces"; break;
            case ToneOperator::Filmic: opName = "filmic"; break;
            case ToneOperator::None: opName = "none"; break;
            default: break;
        }
        o << "{ \"version\": 1, \"op\": \"" << opName
          << "\", \"exposure\": " << config_.exposure
          << ", \"useEV\": " << (config_.useEV ? "true" : "false")
          << ", \"ev100\": " << config_.ev100
          << ", \"whitePoint\": " << config_.whitePoint << " }";
        return o.str();
    }

    float exposureFactor() const noexcept override {
        if (config_.useEV) {
            return 1.0f / (1.2f * std::pow(2.0f, config_.ev100));
        }
        return config_.exposure;
    }

    float tonemapChannel(float x) const noexcept override {
        const float v = std::max(0.0f, x);
        switch (config_.op) {
            case ToneOperator::Reinhard:
                return v / (1.0f + v);
            case ToneOperator::ACES:
                return acesChannel(v);
            case ToneOperator::Filmic: {
                const float w = std::max(config_.whitePoint, 1.0f);
                const float h = uncharted2(v);
                const float hw = uncharted2(w);
                return clamp01(hw > 0.0f ? h / hw : 0.0f);
            }
            case ToneOperator::None:
            default:
                return clamp01(v);
        }
    }

    glm::vec3 apply(const glm::vec3& linear) const noexcept override {
        const float e = exposureFactor();
        glm::vec3 x = linear * e;
        x.x = tonemapChannel(x.x);
        x.y = tonemapChannel(x.y);
        x.z = tonemapChannel(x.z);
        return x;
    }

private:
    static bool parseJson(const std::string& text, ToneMappingConfig& out,
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
                errorOut = "ToneMapping config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "ToneMapping config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "ToneMapping config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "ToneMapping config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "ToneMapping config: unterminated string";
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
                errorOut = "ToneMapping config: expected ',' or '}'";
                return false;
            }
        }

        ToneMappingConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "ToneMapping config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "op") {
                if (p.value == "reinhard") {
                    parsed.op = ToneOperator::Reinhard;
                } else if (p.value == "aces") {
                    parsed.op = ToneOperator::ACES;
                } else if (p.value == "filmic") {
                    parsed.op = ToneOperator::Filmic;
                } else if (p.value == "none") {
                    parsed.op = ToneOperator::None;
                } else {
                    errorOut = "ToneMapping config: unknown op";
                    return false;
                }
            } else if (p.key == "exposure") {
                parsed.exposure = std::stof(p.value);
            } else if (p.key == "useEV") {
                parsed.useEV = (p.value == "true");
                if (p.value != "true" && p.value != "false") {
                    errorOut = "ToneMapping config: useEV must be true/false";
                    return false;
                }
            } else if (p.key == "ev100") {
                parsed.ev100 = std::stof(p.value);
            } else if (p.key == "whitePoint") {
                parsed.whitePoint = std::stof(p.value);
            } else {
                errorOut = "ToneMapping config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "ToneMapping config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "ToneMapping config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    ToneMappingConfig config_{};
};

}  // namespace

bool ToneMappingConfig::valid(std::string& errorOut) const {
    if (!(exposure >= 0.01f && exposure <= 64.0f)) {
        errorOut = "exposure must be in [0.01, 64]";
        return false;
    }
    if (!(ev100 >= -16.0f && ev100 <= 16.0f)) {
        errorOut = "ev100 must be in [-16, 16]";
        return false;
    }
    if (!(whitePoint >= 1.0f && whitePoint <= 64.0f)) {
        errorOut = "whitePoint must be in [1, 64]";
        return false;
    }
    if (op >= ToneOperator::Count) {
        errorOut = "op out of range";
        return false;
    }
    if (!std::isfinite(exposure) || !std::isfinite(ev100) ||
        !std::isfinite(whitePoint)) {
        errorOut = "config values must be finite";
        return false;
    }
    return true;
}

float IToneMapping::linearToSrgb(float c) noexcept {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    if (c <= 0.0031308f) return 12.92f * c;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

float IToneMapping::srgbToLinear(float c) noexcept {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 IToneMapping::linearToSrgb(const glm::vec3& c) noexcept {
    return glm::vec3(linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z));
}

glm::vec3 IToneMapping::srgbToLinear(const glm::vec3& c) noexcept {
    return glm::vec3(srgbToLinear(c.x), srgbToLinear(c.y), srgbToLinear(c.z));
}

std::unique_ptr<IToneMapping> create_tone_mapping(std::string& errorOut) {
    auto impl = std::make_unique<ToneMapping>();
    if (!impl) {
        errorOut = "ToneMapping: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IToneMapping> create_tone_mapping_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<ToneMapping>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

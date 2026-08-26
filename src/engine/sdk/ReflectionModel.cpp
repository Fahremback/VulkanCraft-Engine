// ReflectionModel.cpp — Agente 1 (task_plan A.13), the deterministic
// reflection reflectance core behind the public IReflectionModel contract.
//
// Self-contained (std + glm): Fresnel (Schlick, Frostbite roughness
// correction), GGX roughness -> reflection cone spread, two-layer clear-coat
// Fresnel, water (IOR Fresnel + Beer-Lambert transmission) and the
// roughness-driven screen/probe blend policy. Bit-exact for the same inputs on
// every machine. No RNG, no clock. The GPU seams (screen trace source, probe
// source) are provider-side, as for the other pure cores.

#include "engine/rendering/IReflectionModel.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

float schlick(float cosTheta, float f0) {
    const float c = std::clamp(cosTheta, 0.0f, 1.0f);
    const float m = 1.0f - c;
    const float m5 = m * m * m * m * m;
    return f0 + (1.0f - f0) * m5;
}

class ReflectionModel final : public IReflectionModel {
public:
    ReflectionModel() : config_(ReflectionModelConfig{}) {}

    bool configure(const ReflectionModelConfig& config,
                   std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const ReflectionModelConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        ReflectionModelConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"screenRoughnessLimit\": "
          << config_.screenRoughnessLimit
          << ", \"waterIndexOfRefraction\": " << config_.waterIndexOfRefraction
          << ", \"defaultDielectricF0\": " << config_.defaultDielectricF0
          << ", \"waterAbsorptionPerMeter\": " << config_.waterAbsorptionPerMeter
          << " }";
        return o.str();
    }

    float fresnel(float cosTheta, float f0, float roughness) const noexcept override {
        const float r = std::clamp(roughness, 0.0f, 1.0f);
        const float f = std::clamp(f0, 0.0f, 1.0f);
        const float f0r = std::max(1.0f - r, f);
        // Frostbite form: normal incidence is always F0; roughness raises the
        // grazing asymptote to max(1 - r, F0) instead of 1.
        const float c = std::clamp(cosTheta, 0.0f, 1.0f);
        const float m = 1.0f - c;
        const float m5 = m * m * m * m * m;
        return f + (f0r - f) * m5;
    }

    float roughnessSpread(float roughness) const noexcept override {
        const float r = std::clamp(roughness, 0.0f, 1.0f);
        return std::atan(r);
    }

    float clearCoatFresnel(float cosTheta, float baseF0, float coatF0,
                           float coatRoughness) const noexcept override {
        const float fc = schlick(cosTheta, std::clamp(coatF0, 0.0f, 1.0f));
        const float fb =
            fresnel(cosTheta, baseF0, std::clamp(coatRoughness, 0.0f, 1.0f));
        return fc + (1.0f - fc) * (1.0f - fc) * fb;
    }

    float waterFresnel(float cosTheta) const noexcept override {
        const float n = config_.waterIndexOfRefraction;
        const float f0 = (n - 1.0f) / (n + 1.0f);
        return schlick(cosTheta, f0 * f0);
    }

    float beerLambert(float thickness, float absorption) const noexcept override {
        const float t = std::max(0.0f, thickness);
        const float a = std::max(0.0f, absorption);
        return std::exp(-a * t);
    }

    ReflectionResult evaluate(const ReflectionSurface& s) const noexcept override {
        ReflectionResult out;
        const float cosTheta = std::clamp(glm::dot(s.normal, s.viewDir), 0.0f, 1.0f);
        out.spreadAngleRad = roughnessSpread(s.roughness);

        if (s.water) {
            out.fresnel = waterFresnel(cosTheta);
            out.transmission = (1.0f - out.fresnel) *
                               beerLambert(s.thickness, config_.waterAbsorptionPerMeter);
            // Water reflections are always sharp (screen/RT source).
            out.screenWeight = 1.0f;
            out.probeWeight = 0.0f;
            return out;
        }

        const float f0 = s.f0 > 0.0f ? s.f0 : config_.defaultDielectricF0;
        if (s.clearCoat) {
            const float coatF0 =
                s.coatF0 > 0.0f ? s.coatF0 : config_.defaultDielectricF0;
            out.fresnel = clearCoatFresnel(cosTheta, f0, coatF0, 0.0f);
        } else {
            out.fresnel = fresnel(cosTheta, f0, s.roughness);
        }

        const float t = std::clamp(s.roughness / config_.screenRoughnessLimit,
                                   0.0f, 1.0f);
        out.screenWeight = 1.0f - t;
        out.probeWeight = t;
        return out;
    }

    glm::vec3 blend(const ReflectionSurface& s, const glm::vec3& probe,
                    const glm::vec3& screen) const noexcept override {
        const ReflectionResult e = evaluate(s);
        const glm::vec3 src = glm::mix(probe, screen, e.screenWeight);
        return src * e.fresnel;
    }

private:
    static bool parseJson(const std::string& text, ReflectionModelConfig& out,
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
                errorOut = "ReflectionModel config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "ReflectionModel config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "ReflectionModel config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "ReflectionModel config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "ReflectionModel config: unterminated string";
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
                errorOut = "ReflectionModel config: expected ',' or '}'";
                return false;
            }
        }

        ReflectionModelConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "ReflectionModel config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "screenRoughnessLimit") {
                parsed.screenRoughnessLimit = std::stof(p.value);
            } else if (p.key == "waterIndexOfRefraction") {
                parsed.waterIndexOfRefraction = std::stof(p.value);
            } else if (p.key == "defaultDielectricF0") {
                parsed.defaultDielectricF0 = std::stof(p.value);
            } else if (p.key == "waterAbsorptionPerMeter") {
                parsed.waterAbsorptionPerMeter = std::stof(p.value);
            } else {
                errorOut = "ReflectionModel config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "ReflectionModel config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "ReflectionModel config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    ReflectionModelConfig config_{};
};

}  // namespace

bool ReflectionModelConfig::valid(std::string& errorOut) const {
    if (!(screenRoughnessLimit > 0.0f && screenRoughnessLimit <= 1.0f)) {
        errorOut = "screenRoughnessLimit must be in (0, 1]";
        return false;
    }
    if (!(waterIndexOfRefraction > 1.0f)) {
        errorOut = "waterIndexOfRefraction must be > 1";
        return false;
    }
    if (!(defaultDielectricF0 >= 0.0f && defaultDielectricF0 < 1.0f)) {
        errorOut = "defaultDielectricF0 must be in [0, 1)";
        return false;
    }
    if (!(waterAbsorptionPerMeter >= 0.0f) ||
        !std::isfinite(waterAbsorptionPerMeter)) {
        errorOut = "waterAbsorptionPerMeter must be finite and >= 0";
        return false;
    }
    if (!std::isfinite(screenRoughnessLimit) ||
        !std::isfinite(waterIndexOfRefraction) ||
        !std::isfinite(defaultDielectricF0)) {
        errorOut = "config values must be finite";
        return false;
    }
    return true;
}

std::unique_ptr<IReflectionModel> create_reflection_model(std::string& errorOut) {
    auto impl = std::make_unique<ReflectionModel>();
    if (!impl) {
        errorOut = "ReflectionModel: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IReflectionModel> create_reflection_model_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<ReflectionModel>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

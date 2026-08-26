// MaterialShading.cpp — Agente 1 (task_plan A.14), the deterministic material
// shading core behind the public IMaterialShading contract.
//
// Self-contained (std + glm): two-sided foliage normal flip, wrapped diffuse,
// thickness-based subsurface transmission and interior ambient decay. Bit-exact
// for the same inputs on every machine. No RNG, no clock. The shadow factor
// comes from the occlusion seams (IRayTracer::occluded / ISoftwareTracer /
// a shadow map), as for the other pure cores.

#include "engine/rendering/IMaterialShading.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

class MaterialShading final : public IMaterialShading {
public:
    MaterialShading() : config_(MaterialShadingConfig{}) {}

    bool configure(const MaterialShadingConfig& config,
                   std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const MaterialShadingConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        MaterialShadingConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"subsurfaceScatter\": "
          << config_.subsurfaceScatter
          << ", \"subsurfaceTransmissionMax\": "
          << config_.subsurfaceTransmissionMax
          << ", \"interiorFalloffPerMeter\": " << config_.interiorFalloffPerMeter
          << ", \"interiorAmbientFloor\": " << config_.interiorAmbientFloor
          << " }";
        return o.str();
    }

    glm::vec3 shadingNormal(const ShadingSurface& s) const noexcept override {
        if (s.twoSided && s.backface) {
            return -s.normal;
        }
        return s.normal;
    }

    float wrapLight(const glm::vec3& normal,
                    const glm::vec3& lightDir) const noexcept override {
        const glm::vec3 n = glm::normalize(normal);
        const glm::vec3 l = glm::normalize(lightDir);
        const float d = glm::dot(n, l);
        return std::clamp((d + config_.subsurfaceScatter) /
                              (1.0f + config_.subsurfaceScatter),
                          0.0f, 1.0f);
    }

    float subsurfaceTransmission(float thickness) const noexcept override {
        const float t = std::max(0.0f, thickness);
        return config_.subsurfaceTransmissionMax * std::exp(-t);
    }

    float interiorAmbient(float depth) const noexcept override {
        const float d = std::max(0.0f, depth);
        return config_.interiorAmbientFloor +
               (1.0f - config_.interiorAmbientFloor) *
                   std::exp(-config_.interiorFalloffPerMeter * d);
    }

    ShadingResult evaluate(const ShadingSurface& s, const glm::vec3& lightDir,
                           float shadowFactor) const noexcept override {
        ShadingResult out;
        out.shadingNormal = shadingNormal(s);
        out.wrapLight = wrapLight(out.shadingNormal, lightDir);
        out.subsurface = subsurfaceTransmission(s.thickness);
        out.interiorAmbient = interiorAmbient(s.interiorDepth);
        const float shadow = std::clamp(shadowFactor, 0.0f, 1.0f);
        out.directDiffuse = out.wrapLight * (1.0f - shadow);
        out.total = out.directDiffuse + out.interiorAmbient;
        return out;
    }

private:
    static bool parseJson(const std::string& text, MaterialShadingConfig& out,
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
                errorOut = "MaterialShading config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "MaterialShading config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "MaterialShading config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "MaterialShading config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "MaterialShading config: unterminated string";
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
                errorOut = "MaterialShading config: expected ',' or '}'";
                return false;
            }
        }

        MaterialShadingConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "MaterialShading config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "subsurfaceScatter") {
                parsed.subsurfaceScatter = std::stof(p.value);
            } else if (p.key == "subsurfaceTransmissionMax") {
                parsed.subsurfaceTransmissionMax = std::stof(p.value);
            } else if (p.key == "interiorFalloffPerMeter") {
                parsed.interiorFalloffPerMeter = std::stof(p.value);
            } else if (p.key == "interiorAmbientFloor") {
                parsed.interiorAmbientFloor = std::stof(p.value);
            } else {
                errorOut = "MaterialShading config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "MaterialShading config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "MaterialShading config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    MaterialShadingConfig config_{};
};

}  // namespace

bool MaterialShadingConfig::valid(std::string& errorOut) const {
    if (!(subsurfaceScatter >= 0.0f && subsurfaceScatter <= 1.0f)) {
        errorOut = "subsurfaceScatter must be in [0, 1]";
        return false;
    }
    if (!(subsurfaceTransmissionMax >= 0.0f && subsurfaceTransmissionMax <= 1.0f)) {
        errorOut = "subsurfaceTransmissionMax must be in [0, 1]";
        return false;
    }
    if (!(interiorFalloffPerMeter >= 0.0f) ||
        !std::isfinite(interiorFalloffPerMeter)) {
        errorOut = "interiorFalloffPerMeter must be finite and >= 0";
        return false;
    }
    if (!(interiorAmbientFloor >= 0.0f && interiorAmbientFloor < 1.0f)) {
        errorOut = "interiorAmbientFloor must be in [0, 1)";
        return false;
    }
    if (!std::isfinite(subsurfaceScatter) ||
        !std::isfinite(subsurfaceTransmissionMax) ||
        !std::isfinite(interiorAmbientFloor)) {
        errorOut = "config values must be finite";
        return false;
    }
    return true;
}

std::unique_ptr<IMaterialShading> create_material_shading(std::string& errorOut) {
    auto impl = std::make_unique<MaterialShading>();
    if (!impl) {
        errorOut = "MaterialShading: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IMaterialShading> create_material_shading_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<MaterialShading>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

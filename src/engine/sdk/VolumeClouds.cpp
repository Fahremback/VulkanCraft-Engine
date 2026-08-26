// VolumeClouds.cpp — Agente 1 (task_plan A.15), the deterministic volumetric
// cloud core behind the public IVolumeClouds contract.
//
// Self-contained (std + glm): seeded FBM value-noise density field, HG phase
// function and a single-scattering raymarch with Beer-Lambert transmittance.
// Bit-exact for the same inputs on every machine. No clock. The GPU/visual
// integration (weather maps, real textures) is provider-side, as for the
// other pure cores.

#include "engine/rendering/IVolumeClouds.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

const float kPi = 3.14159265358979f;

std::uint32_t hashMix(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    a = a * 0x9E3779B1u + b * 0x85EBCA77u + c * 0xC2B2AE3Du;
    a ^= a >> 16;
    a *= 0x7FEB352Du;
    a ^= a >> 15;
    a *= 0x846CA68Bu;
    a ^= a >> 16;
    return a;
}

class VolumeClouds final : public IVolumeClouds {
public:
    VolumeClouds() : config_(VolumeCloudsConfig{}) {}

    bool configure(const VolumeCloudsConfig& config,
                   std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const VolumeCloudsConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        VolumeCloudsConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"coverage\": " << config_.coverage
          << ", \"densityScale\": " << config_.densityScale
          << ", \"detailStrength\": " << config_.detailStrength
          << ", \"lightAbsorption\": " << config_.lightAbsorption
          << ", \"lightScatter\": " << config_.lightScatter
          << ", \"phaseG\": " << config_.phaseG
          << ", \"ambientScale\": " << config_.ambientScale
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    float density(float x, float y, float z) const noexcept override {
        if (config_.coverage <= 0.0f) {
            return 0.0f;  // clear sky
        }
        // Base FBM (2 octaves) in [0, 1].
        const float n0 = valueNoise(x, y, z);
        const float n1 = valueNoise(x * 2.0f + 7.31f, y * 2.0f + 3.17f,
                                    z * 2.0f + 1.19f);
        const float n = n0 * 0.7f + n1 * 0.3f;

        // Coverage gate: lifts the noise floor so only the top `coverage`
        // fraction of the noise survives, remapped back to [0, 1].
        const float base = std::max(0.0f, n - (1.0f - config_.coverage)) /
                           config_.coverage;

        // Height bell profile centered at y = 0.5 (band [0, 1]).
        const float dy = (y - 0.5f) * 4.0f;
        const float profile = std::exp(-dy * dy);

        float d = base * profile * config_.densityScale;

        // Detail modulation (higher frequency, stronger where base is denser).
        if (config_.detailStrength > 0.0f) {
            const float det = valueNoise(x * 4.0f + 13.7f, y * 4.0f + 29.3f,
                                         z * 4.0f + 5.9f);
            d *= 1.0f + config_.detailStrength * (det - 0.5f);
        }
        return std::max(0.0f, d);
    }

    float phase(float cosTheta) const noexcept override {
        const float c = std::clamp(cosTheta, -1.0f, 1.0f);
        const float g = std::clamp(config_.phaseG, -1.0f, 1.0f);
        const float g2 = g * g;
        if (g2 >= 1.0f) {
            // Fully forward or fully backward: a Dirac-like phase; return a
            // large value only at the exact peak direction.
            return (g > 0.0f) ? (c >= 0.99999f ? 1000.0f : 0.0f)
                              : (c <= -0.99999f ? 1000.0f : 0.0f);
        }
        const float denom = 1.0f + g2 - 2.0f * g * c;
        return (1.0f - g2) / (4.0f * kPi * std::pow(denom, 1.5f));
    }

    bool march(const glm::vec3& origin, const glm::vec3& dir, float tMin,
               float tMax, std::uint32_t steps, const glm::vec3& sunDir,
               const glm::vec3& sunColor, float& transmittance,
               glm::vec3& inscatter) const noexcept override {
        if (steps < 1 || steps > 128 || !(tMax > tMin) ||
            !(tMin >= 0.0f) || !std::isfinite(tMax)) {
            return false;
        }
        const glm::vec3 d = glm::normalize(dir);
        const glm::vec3 s = glm::normalize(sunDir);
        if (!std::isfinite(d.x) || !std::isfinite(s.x)) {
            return false;
        }
        if (!(std::isfinite(sunColor.x) && std::isfinite(sunColor.y) &&
              std::isfinite(sunColor.z)) ||
            sunColor.x < 0.0f || sunColor.y < 0.0f || sunColor.z < 0.0f) {
            return false;
        }

        const float ds = (tMax - tMin) / static_cast<float>(steps);
        const float ph = phase(glm::dot(d, s));
        float T = 1.0f;
        glm::vec3 L(0.0f, 0.0f, 0.0f);
        for (std::uint32_t i = 0; i < steps; ++i) {
            const float t = tMin + (static_cast<float>(i) + 0.5f) * ds;
            const glm::vec3 p = origin + d * t;
            const float rho = density(p.x, p.y, p.z);
            if (rho > 0.0f) {
                // In-scattered radiance toward the viewer.
                L += T * (config_.lightScatter * rho) * ph * sunColor * ds;
                // Beer-Lambert extinction along this step.
                T *= std::exp(-config_.lightAbsorption * rho * ds);
            }
        }
        transmittance = T;
        inscatter = L;
        return true;
    }

private:
    float lattice(std::int32_t ix, std::int32_t iy, std::int32_t iz) const {
        const std::uint32_t h = hashMix(
            static_cast<std::uint32_t>(static_cast<std::uint32_t>(ix) * 0x9E3779B1u ^
                                       config_.seed),
            static_cast<std::uint32_t>(iy),
            static_cast<std::uint32_t>(iz));
        return (h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    static float smooth01(float t) { return t * t * (3.0f - 2.0f * t); }

    float valueNoise(float x, float y, float z) const {
        const std::int32_t ix = static_cast<std::int32_t>(std::floor(x));
        const std::int32_t iy = static_cast<std::int32_t>(std::floor(y));
        const std::int32_t iz = static_cast<std::int32_t>(std::floor(z));
        const float fx = x - static_cast<float>(ix);
        const float fy = y - static_cast<float>(iy);
        const float fz = z - static_cast<float>(iz);
        const float ux = smooth01(fx);
        const float uy = smooth01(fy);
        const float uz = smooth01(fz);

        const float c000 = lattice(ix, iy, iz);
        const float c100 = lattice(ix + 1, iy, iz);
        const float c010 = lattice(ix, iy + 1, iz);
        const float c110 = lattice(ix + 1, iy + 1, iz);
        const float c001 = lattice(ix, iy, iz + 1);
        const float c101 = lattice(ix + 1, iy, iz + 1);
        const float c011 = lattice(ix, iy + 1, iz + 1);
        const float c111 = lattice(ix + 1, iy + 1, iz + 1);

        const float x00 = c000 + (c100 - c000) * ux;
        const float x10 = c010 + (c110 - c010) * ux;
        const float x01 = c001 + (c101 - c001) * ux;
        const float x11 = c011 + (c111 - c011) * ux;
        const float y0 = x00 + (x10 - x00) * uy;
        const float y1 = x01 + (x11 - x01) * uy;
        return y0 + (y1 - y0) * uz;
    }

    static bool parseJson(const std::string& text, VolumeCloudsConfig& out,
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
                errorOut = "VolumeClouds config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "VolumeClouds config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "VolumeClouds config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "VolumeClouds config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "VolumeClouds config: unterminated string";
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
                errorOut = "VolumeClouds config: expected ',' or '}'";
                return false;
            }
        }

        VolumeCloudsConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "VolumeClouds config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "coverage") {
                parsed.coverage = std::stof(p.value);
            } else if (p.key == "densityScale") {
                parsed.densityScale = std::stof(p.value);
            } else if (p.key == "detailStrength") {
                parsed.detailStrength = std::stof(p.value);
            } else if (p.key == "lightAbsorption") {
                parsed.lightAbsorption = std::stof(p.value);
            } else if (p.key == "lightScatter") {
                parsed.lightScatter = std::stof(p.value);
            } else if (p.key == "phaseG") {
                parsed.phaseG = std::stof(p.value);
            } else if (p.key == "ambientScale") {
                parsed.ambientScale = std::stof(p.value);
            } else if (p.key == "seed") {
                parsed.seed = static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "VolumeClouds config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "VolumeClouds config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "VolumeClouds config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    VolumeCloudsConfig config_{};
};

}  // namespace

bool VolumeCloudsConfig::valid(std::string& errorOut) const {
    if (!(coverage >= 0.0f && coverage <= 1.0f)) {
        errorOut = "coverage must be in [0, 1]";
        return false;
    }
    if (!(densityScale >= 0.01f && densityScale <= 8.0f)) {
        errorOut = "densityScale must be in [0.01, 8]";
        return false;
    }
    if (!(detailStrength >= 0.0f && detailStrength <= 1.0f)) {
        errorOut = "detailStrength must be in [0, 1]";
        return false;
    }
    if (!(lightAbsorption >= 0.0f) || !std::isfinite(lightAbsorption)) {
        errorOut = "lightAbsorption must be finite and >= 0";
        return false;
    }
    if (!(lightScatter >= 0.0f) || !std::isfinite(lightScatter)) {
        errorOut = "lightScatter must be finite and >= 0";
        return false;
    }
    if (!(phaseG >= -1.0f && phaseG <= 1.0f)) {
        errorOut = "phaseG must be in [-1, 1]";
        return false;
    }
    if (!(ambientScale >= 0.0f && ambientScale <= 1.0f)) {
        errorOut = "ambientScale must be in [0, 1]";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    if (!std::isfinite(coverage) || !std::isfinite(densityScale) ||
        !std::isfinite(detailStrength) || !std::isfinite(phaseG) ||
        !std::isfinite(ambientScale)) {
        errorOut = "config values must be finite";
        return false;
    }
    return true;
}

std::unique_ptr<IVolumeClouds> create_volume_clouds(std::string& errorOut) {
    auto impl = std::make_unique<VolumeClouds>();
    if (!impl) {
        errorOut = "VolumeClouds: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IVolumeClouds> create_volume_clouds_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<VolumeClouds>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

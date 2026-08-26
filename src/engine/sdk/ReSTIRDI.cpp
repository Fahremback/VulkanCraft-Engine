// ReSTIRDI.cpp — Agente 1 (task_plan A.9), the deterministic ReSTIR
// Direct-Illumination pure core behind the public IReSTIRDI contract.
//
// Self-contained (std + glm): the only implementation of the candidate
// generation, weighted-resampling reservoir update, temporal/spatial merge and
// resolve math. Bit-exact for the same inputs on every machine (PCG32 RNG with
// per-pixel seeds). The GPU seam (RTXDI vendor code) is out of scope here, as
// for the other pure cores (IGiCore / ISoftwareTracer).

#include "engine/rendering/IReSTIRDI.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace Engine::Rendering {

namespace {

// ---- PCG32 (deterministic, seedable) ----
class Pcg32 {
public:
    explicit Pcg32(std::uint64_t seed) : state_(seed) { next(); }

    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint32_t xorshifted =
            static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform float in [0, 1).
    float unit() {
        return static_cast<float>(next() >> 8u) * (1.0f / 16777216.0f);
    }

private:
    std::uint64_t state_;
};

// Deterministic per-pixel RNG seed: config seed, pixel and an implicit frame
// counter derived from the previous reservoir's age (so reuse changes the
// stream without extra state).
std::uint64_t pixelSeed(std::uint32_t configSeed, std::uint32_t pixel,
                        std::uint32_t frameHint) {
    std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ configSeed;
    h ^= static_cast<std::uint64_t>(pixel) * 0x85EBCA77C2B2AE63ULL;
    h ^= static_cast<std::uint64_t>(frameHint) * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 30u;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27u;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31u;
    return h;
}

float luminance(const glm::vec3& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// The target function p_hat for DI at a shading point: the diffuse integrand
// radiance * cos(n, dir) (albedo folded into the light radiance by the caller
// or treated as 1 — the estimator stays unbiased for irradiance).
float targetPdf(const glm::vec3& normal, const RestirLightSample& s) {
    const float cosTheta = std::max(0.0f, glm::dot(normal, s.direction));
    return luminance(s.radiance) * cosTheta;
}

class ReSTIRDI final : public IReSTIRDI {
public:
    ReSTIRDI() : config_(defaultConfig()) {}

    bool configure(const RestirConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const RestirConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        RestirConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"candidateCount\": " << config_.candidateCount
          << ", \"spatialSamples\": " << config_.spatialSamples
          << ", \"temporalReuse\": " << (config_.temporalReuse ? "true" : "false")
          << ", \"spatialReuse\": " << (config_.spatialReuse ? "true" : "false")
          << ", \"visibilityReuse\": "
          << (config_.visibilityReuse ? "true" : "false")
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    RestirReservoir update(const std::vector<RestirLightSample>& candidates,
                           const glm::vec3& normal,
                           std::uint32_t seed) override {
        RestirReservoir r;
        Pcg32 rng(pixelSeed(config_.seed, seed, 0));
        float wSum = 0.0f;
        for (const RestirLightSample& c : candidates) {
            const float w = targetPdf(normal, c) / std::max(c.pdf, 1e-12f);
            wSum += w;
            if (rng.unit() * wSum < w) {
                r.sampleRadiance = c.radiance;
                r.sampleDirection = c.direction;
                r.lightId = c.lightId;
            }
        }
        r.weightSum = wSum;
        r.m = static_cast<std::uint32_t>(candidates.size());
        r.flags = (r.m > 0) ? 1u : 0u;
        return r;
    }

    RestirReservoir merge(const RestirReservoir& a, const RestirReservoir& b,
                          float bCorrection, std::uint32_t seed) override {
        if (a.m == 0) {
            RestirReservoir out = b;
            out.weightSum *= bCorrection;
            return out;
        }
        if (b.m == 0) {
            return a;
        }
        RestirReservoir out = a;
        const float wB = b.weightSum * bCorrection;
        const float wTotal = a.weightSum + wB;
        Pcg32 rng(pixelSeed(config_.seed, seed, a.age + b.age));
        if (wTotal > 0.0f && rng.unit() * wTotal < wB) {
            out.sampleRadiance = b.sampleRadiance;
            out.sampleDirection = b.sampleDirection;
            out.lightId = b.lightId;
            out.age = b.age;
        }
        out.weightSum = wTotal;
        out.m = a.m + b.m;
        out.flags = 1u;
        return out;
    }

    float resolve(const RestirReservoir& r) const noexcept override {
        if (r.m == 0 || !(r.flags & 1u)) {
            return 0.0f;
        }
        return r.weightSum / static_cast<float>(r.m);
    }

    bool diFrame(const std::vector<RestirDiPixelInput>& pixels,
                 const std::vector<RestirDiLight>& lights,
                 const std::vector<RestirReservoir>& prevReservoirs,
                 std::uint32_t frameIndex,
                 const VisibilityFn& visibility,
                 RestirDiFrameResult& out, std::string& errorOut) override {
        if (pixels.empty()) {
            errorOut = "ReSTIR DI: no pixels";
            return false;
        }
        if (lights.empty()) {
            errorOut = "ReSTIR DI: no lights";
            return false;
        }
        for (const RestirDiLight& l : lights) {
            if (!(l.power > 0.0f) || glm::length(l.radiance) <= 0.0f) {
                errorOut = "ReSTIR DI: light " + std::to_string(l.id) +
                           " has non-positive power/radiance";
                return false;
            }
        }
        if (config_.temporalReuse && prevReservoirs.size() != pixels.size()) {
            errorOut = "ReSTIR DI: prevReservoirs size mismatch";
            return false;
        }

        const bool wantVisibility =
            config_.visibilityReuse && static_cast<bool>(visibility);

        float totalPower = 0.0f;
        for (const RestirDiLight& l : lights) {
            totalPower += l.power;
        }
        if (!(totalPower > 0.0f)) {
            errorOut = "ReSTIR DI: total light power is zero";
            return false;
        }

        out.radiance.assign(pixels.size(), 0.0f);
        out.reservoirs.assign(pixels.size(), RestirReservoir{});
        out.effectiveM.assign(pixels.size(), 0);
        out.temporalAccepted.assign(pixels.size(), 0);
        out.spatialAccepted.assign(pixels.size(), 0);

        // --- pass 1: candidate generation + streaming (fresh reservoirs) ---
        std::vector<RestirReservoir> fresh(pixels.size());
        for (std::size_t p = 0; p < pixels.size(); ++p) {
            const RestirDiPixelInput& px = pixels[p];
            Pcg32 rng(pixelSeed(config_.seed, static_cast<std::uint32_t>(p),
                                frameIndex));
            std::vector<RestirLightSample> candidates;
            candidates.reserve(config_.candidateCount);
            for (std::uint32_t c = 0; c < config_.candidateCount; ++c) {
                float draw = rng.unit() * totalPower;
                const RestirDiLight* chosen = &lights.back();
                for (const RestirDiLight& l : lights) {
                    draw -= l.power;
                    if (draw <= 0.0f) {
                        chosen = &l;
                        break;
                    }
                }
                RestirLightSample s;
                const glm::vec3 delta = chosen->position - px.position;
                const float len = glm::length(delta);
                if (len < 1e-6f) {
                    s.direction = px.normal;
                } else {
                    s.direction = delta / len;
                }
                s.radiance = chosen->radiance;
                s.pdf = chosen->power / totalPower;
                s.lightId = chosen->id;
                candidates.push_back(s);
            }
            fresh[p] =
                update(candidates, px.normal, static_cast<std::uint32_t>(p) * 17u + 3u);
        }
        out.reservoirs = fresh;

        // --- pass 2: temporal + spatial reuse, then resolve ---
        const std::uint32_t side =
            static_cast<std::uint32_t>(std::sqrt(static_cast<double>(pixels.size())));
        const std::uint32_t nside = std::max(side, 1u);
        for (std::size_t p = 0; p < pixels.size(); ++p) {
            const RestirDiPixelInput& px = pixels[p];
            RestirReservoir cur = out.reservoirs[p];

            // Temporal reuse (previous frame, same pixel). The correction
            // re-weights the previous sample for the current frame; a static
            // scene yields 1.0 (identity).
            if (config_.temporalReuse && prevReservoirs[p].m > 0) {
                const RestirReservoir& prev = prevReservoirs[p];
                float correction = 1.0f;
                if (wantVisibility && (prev.flags & 1u) &&
                    !visibility(px.position, prev.sampleDirection, 1000.0f)) {
                    correction = 0.0f;  // occluded reuse contributes nothing
                }
                if (correction > 0.0f) {
                    const std::uint32_t beforeM = cur.m;
                    cur = merge(cur, prev, correction,
                                static_cast<std::uint32_t>(p) * 31u + 7u);
                    out.temporalAccepted[p] = (cur.m > beforeM) ? 1u : 0u;
                }
            }

            // Spatial reuse (fresh neighbor reservoirs of the current frame).
            if (config_.spatialReuse && config_.spatialSamples > 0) {
                const std::uint32_t col = static_cast<std::uint32_t>(p) % nside;
                const std::uint32_t row = static_cast<std::uint32_t>(p) / nside;
                for (std::uint32_t k = 0; k < config_.spatialSamples; ++k) {
                    // Deterministic offsets: the k-th neighbor along the spiral
                    // order at Manhattan distance k+1.
                    const int step = static_cast<int>(k) + 1;
                    const int dcol = (k % 2u == 0u) ? step : -step;
                    const int drow = (k % 2u == 0u) ? 0 : step;
                    const int nc = static_cast<int>(col) + dcol;
                    const int nr = static_cast<int>(row) + drow;
                    if (nc < 0 || nr < 0) continue;
                    if (static_cast<std::uint32_t>(nc) >= nside ||
                        static_cast<std::uint32_t>(nr) >= nside) {
                        continue;
                    }
                    const std::size_t nidx =
                        static_cast<std::size_t>(nr) * nside + static_cast<std::size_t>(nc);
                    if (nidx >= pixels.size() || nidx == p) continue;
                    const RestirReservoir& nres = fresh[nidx];
                    if (nres.m == 0) continue;
                    float correction = 1.0f;
                    if (wantVisibility && (nres.flags & 1u) &&
                        !visibility(px.position, nres.sampleDirection, 1000.0f)) {
                        correction = 0.0f;
                    }
                    if (correction > 0.0f) {
                        const std::uint32_t beforeM = cur.m;
                        cur = merge(cur, nres, correction,
                                    static_cast<std::uint32_t>(p) * 131u + k);
                        if (cur.m > beforeM) {
                            out.spatialAccepted[p] += 1u;
                        }
                    }
                }
            }

            out.reservoirs[p] = cur;
            out.effectiveM[p] = cur.m;
            out.radiance[p] = resolve(cur);
        }
        return true;
    }

private:
    static RestirConfig defaultConfig() {
        RestirConfig c;
        return c;
    }

    // Minimal deterministic JSON reader for the config surface (the other
    // adapters follow the same pattern). Accepts {"key": value, ...}; unknown
    // keys are refused all-or-nothing.
    static bool parseJson(const std::string& text, RestirConfig& out,
                          std::string& errorOut) {
        // Tokenise {"k": v, ...} into pairs.
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
                errorOut = "ReSTIR DI config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;  // empty
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "ReSTIR DI config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "ReSTIR DI config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "ReSTIR DI config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "ReSTIR DI config: unterminated string";
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
                errorOut = "ReSTIR DI config: expected ',' or '}'";
                return false;
            }
        }

        RestirConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "ReSTIR DI config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "candidateCount") {
                parsed.candidateCount = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "spatialSamples") {
                parsed.spatialSamples = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "temporalReuse") {
                parsed.temporalReuse = (p.value == "true");
            } else if (p.key == "spatialReuse") {
                parsed.spatialReuse = (p.value == "true");
            } else if (p.key == "visibilityReuse") {
                parsed.visibilityReuse = (p.value == "true");
            } else if (p.key == "seed") {
                parsed.seed = static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "ReSTIR DI config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "ReSTIR DI config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "ReSTIR DI config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    RestirConfig config_;
};

}  // namespace

bool RestirConfig::valid(std::string& errorOut) const {
    if (candidateCount < 1 || candidateCount > 256) {
        errorOut = "candidateCount must be in [1, 256]";
        return false;
    }
    if (spatialSamples > 16) {
        errorOut = "spatialSamples must be in [0, 16]";
        return false;
    }
    if (spatialReuse && spatialSamples < 1) {
        errorOut = "spatialReuse requires spatialSamples >= 1";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    return true;
}

std::unique_ptr<IReSTIRDI> create_restir_di(std::string& errorOut) {
    auto impl = std::make_unique<ReSTIRDI>();
    if (!impl) {
        errorOut = "ReSTIR DI: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IReSTIRDI> create_restir_di_json(const std::string& jsonText,
                                                 std::string& errorOut) {
    auto impl = std::make_unique<ReSTIRDI>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

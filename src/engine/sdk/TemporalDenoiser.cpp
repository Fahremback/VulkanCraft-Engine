// TemporalDenoiser.cpp — Agente 1 (task_plan A.11), the deterministic NRD-style
// temporal denoiser pure core behind the public ITemporalDenoiser contract.
//
// Self-contained (std + glm): per-pixel history accumulation with motion-vector
// reprojection, depth/normal rejection (disocclusion) and an EMA, plus a
// confidence (history length) output. Bit-exact for the same inputs on every
// machine (no RNG). The GPU seam (NRD vendor code) is out of scope here, as for
// the other pure cores.

#include "engine/rendering/ITemporalDenoiser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

class TemporalDenoiser final : public ITemporalDenoiser {
public:
    TemporalDenoiser() : config_(DenoiserConfig{}) {}

    bool configure(const DenoiserConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const DenoiserConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        DenoiserConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"width\": " << config_.width
          << ", \"height\": " << config_.height
          << ", \"historyWeight\": " << config_.historyWeight
          << ", \"depthRejectThreshold\": " << config_.depthRejectThreshold
          << ", \"normalRejectDegrees\": " << config_.normalRejectDegrees
          << ", \"useMotion\": " << (config_.useMotion ? "true" : "false")
          << ", \"useDepthRejection\": "
          << (config_.useDepthRejection ? "true" : "false")
          << ", \"useNormalRejection\": "
          << (config_.useNormalRejection ? "true" : "false")
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    void reset_histories(std::vector<DenoiserHistory>& histories) const override {
        for (DenoiserHistory& h : histories) {
            h = DenoiserHistory{};
        }
    }

    bool denoise(const std::vector<DenoiserSample>& samples,
                 std::vector<DenoiserHistory>& histories,
                 std::vector<float>& confidenceOut,
                 std::vector<glm::vec3>& radianceOut,
                 std::string& errorOut) override {
        const std::uint32_t n = config_.width * config_.height;
        if (samples.size() != n || histories.size() != n) {
            errorOut = "TemporalDenoiser: samples/histories size mismatch";
            return false;
        }
        confidenceOut.resize(n);
        radianceOut.resize(n);

        const float depthEps = 1e-6f;
        const float normalRejectRad =
            config_.normalRejectDegrees * 3.14159265358979f / 180.0f;
        const std::uint32_t width = config_.width;
        const std::uint32_t height = config_.height;

        // The input histories must not be clobbered before every pixel reads
        // its reprojection target (which can be any pixel, including itself).
        const std::vector<DenoiserHistory> in = histories;

        for (std::uint32_t p = 0; p < n; ++p) {
            const DenoiserSample& s = samples[p];
            bool reject = !(in[p].flags & 1u);

            // Reproject the history through the motion vector: the content now
            // at p was at p - motion last frame.
            if (!reject && config_.useMotion) {
                const int col = static_cast<int>(p % width);
                const int row = static_cast<int>(p / width);
                const int pc = col - static_cast<int>(std::round(s.motion.x));
                const int pr = row - static_cast<int>(std::round(s.motion.y));
                if (pc < 0 || pc >= static_cast<int>(width) ||
                    pr < 0 || pr >= static_cast<int>(height)) {
                    reject = true;  // reprojection target outside the frame
                } else {
                    const DenoiserHistory& reproj =
                        in[static_cast<std::uint32_t>(pr) * width +
                           static_cast<std::uint32_t>(pc)];
                    if (!(reproj.flags & 1u)) {
                        reject = true;
                    } else {
                        if (config_.useDepthRejection) {
                            const float rel =
                                std::fabs(s.depth - reproj.depth) /
                                std::max(s.depth, depthEps);
                            if (rel > config_.depthRejectThreshold) reject = true;
                        }
                        if (!reject && config_.useNormalRejection) {
                            const float cosA = glm::clamp(
                                glm::dot(s.normal, reproj.normal), -1.0f, 1.0f);
                            if (std::acos(cosA) > normalRejectRad) reject = true;
                        }
                        if (!reject) {
                            // The reprojected history is the one we accumulate.
                            DenoiserHistory& h = histories[p];
                            h.radiance = glm::mix(reproj.radiance, s.radiance,
                                                  config_.historyWeight);
                            h.normal = s.normal;
                            h.depth = s.depth;
                            h.frames = std::min(reproj.frames + 1u, 0xFFFFFFu);
                            h.flags = 1u;
                            confidenceOut[p] = static_cast<float>(h.frames);
                            radianceOut[p] = h.radiance;
                            continue;
                        }
                    }
                }
            }

            // No rejection path hit: accumulate from the pixel's own history
            // (zero motion / motion disabled) or restart fresh.
            if (!reject) {
                const DenoiserHistory& own = in[p];
                if (config_.useDepthRejection) {
                    const float rel =
                        std::fabs(s.depth - own.depth) / std::max(s.depth, depthEps);
                    if (rel > config_.depthRejectThreshold) reject = true;
                }
                if (!reject && config_.useNormalRejection) {
                    const float cosA =
                        glm::clamp(glm::dot(s.normal, own.normal), -1.0f, 1.0f);
                    if (std::acos(cosA) > normalRejectRad) reject = true;
                }
                if (!reject && (own.flags & 1u)) {
                    DenoiserHistory& h = histories[p];
                    h.radiance = glm::mix(own.radiance, s.radiance,
                                          config_.historyWeight);
                    h.normal = s.normal;
                    h.depth = s.depth;
                    h.frames = std::min(own.frames + 1u, 0xFFFFFFu);
                    h.flags = 1u;
                    confidenceOut[p] = static_cast<float>(h.frames);
                    radianceOut[p] = h.radiance;
                    continue;
                }
            }

            // Rejected or no history: restart from the current sample.
            DenoiserHistory& h = histories[p];
            h.radiance = s.radiance;
            h.normal = s.normal;
            h.depth = s.depth;
            h.frames = 1u;
            h.flags = 1u;
            confidenceOut[p] = 1.0f;
            radianceOut[p] = s.radiance;
        }
        return true;
    }

private:
    static bool parseJson(const std::string& text, DenoiserConfig& out,
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
                errorOut = "TemporalDenoiser config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "TemporalDenoiser config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "TemporalDenoiser config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "TemporalDenoiser config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "TemporalDenoiser config: unterminated string";
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
                errorOut = "TemporalDenoiser config: expected ',' or '}'";
                return false;
            }
        }

        DenoiserConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "TemporalDenoiser config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "width") {
                parsed.width = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "height") {
                parsed.height = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "historyWeight") {
                parsed.historyWeight = std::stof(p.value);
            } else if (p.key == "depthRejectThreshold") {
                parsed.depthRejectThreshold = std::stof(p.value);
            } else if (p.key == "normalRejectDegrees") {
                parsed.normalRejectDegrees = std::stof(p.value);
            } else if (p.key == "useMotion") {
                parsed.useMotion = (p.value == "true");
            } else if (p.key == "useDepthRejection") {
                parsed.useDepthRejection = (p.value == "true");
            } else if (p.key == "useNormalRejection") {
                parsed.useNormalRejection = (p.value == "true");
            } else if (p.key == "seed") {
                parsed.seed = static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "TemporalDenoiser config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "TemporalDenoiser config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "TemporalDenoiser config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    DenoiserConfig config_{};
};

}  // namespace

bool DenoiserConfig::valid(std::string& errorOut) const {
    if (width < 1 || width > 8192 || height < 1 || height > 8192) {
        errorOut = "width/height must be in [1, 8192]";
        return false;
    }
    if (!(historyWeight >= 0.01f && historyWeight <= 1.0f)) {
        errorOut = "historyWeight must be in [0.01, 1]";
        return false;
    }
    if (!(depthRejectThreshold >= 0.01f && depthRejectThreshold <= 4.0f)) {
        errorOut = "depthRejectThreshold must be in [0.01, 4]";
        return false;
    }
    if (!(normalRejectDegrees >= 1.0f && normalRejectDegrees <= 179.0f)) {
        errorOut = "normalRejectDegrees must be in [1, 179]";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    return true;
}

std::unique_ptr<ITemporalDenoiser> create_temporal_denoiser(std::string& errorOut) {
    auto impl = std::make_unique<TemporalDenoiser>();
    if (!impl) {
        errorOut = "TemporalDenoiser: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<ITemporalDenoiser> create_temporal_denoiser_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<TemporalDenoiser>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

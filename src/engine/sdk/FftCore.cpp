// FftCore.cpp — Agente 1 (task_plan C.14, vkfft), the deterministic FFT
// nucleus behind the public IFftCore contract.
//
// Self-contained (std): iterative radix-2 Cooley-Tukey with bit-reversal.
// Bit-exact for the same inputs on every machine. No clock. The GPU vendor
// (vkfft) is the provider seam a Vulkan backend implements later, as for the
// other pure cores.

#include "engine/rendering/IFftCore.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

class FftCore final : public IFftCore {
public:
    FftCore() : config_(FftCoreConfig{}) {}

    bool configure(const FftCoreConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const FftCoreConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        FftCoreConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"maxSize\": " << config_.maxSize
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    bool fft(const std::vector<std::complex<float>>& input,
             std::vector<std::complex<float>>& output) const override {
        const std::size_t n = input.size();
        if (!validSize(n)) {
            return false;
        }
        transform(input, output, /*inverse=*/false);
        return true;
    }

    bool ifft(const std::vector<std::complex<float>>& input,
              std::vector<std::complex<float>>& output) const override {
        const std::size_t n = input.size();
        if (!validSize(n)) {
            return false;
        }
        transform(input, output, /*inverse=*/true);
        const float inv = 1.0f / static_cast<float>(n);
        for (std::size_t i = 0; i < n; ++i) {
            output[i] *= inv;
        }
        return true;
    }

private:
    bool validSize(std::size_t n) const {
        if (n < 1 || n > config_.maxSize) {
            return false;
        }
        // Must be a power of two.
        return (n & (n - 1)) == 0;
    }

    // Iterative radix-2 Cooley-Tukey: bit-reversal permutation, then
    // log2(N) butterfly stages. Forward uses exp(-2*pi*i*k/N); inverse uses
    // the conjugate (the 1/N scaling is applied by the caller of ifft).
    void transform(const std::vector<std::complex<float>>& input,
                   std::vector<std::complex<float>>& out, bool inverse) const {
        const std::size_t n = input.size();
        out.resize(n);
        // Bit-reversal permutation.
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t j = 0;
            std::size_t bits = n >> 1;
            std::size_t t = i;
            while (bits) {
                j = (j << 1) | (t & 1);
                t >>= 1;
                bits >>= 1;
            }
            if (j < n) {
                out[i] = input[j];
            }
        }
        // Butterfly stages.
        const float sign = inverse ? 1.0f : -1.0f;
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const float ang = sign * 2.0f * 3.14159265358979f /
                              static_cast<float>(len);
            const std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (std::size_t i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                const std::size_t half = len >> 1;
                for (std::size_t k = 0; k < half; ++k) {
                    const std::complex<float> u = out[i + k];
                    const std::complex<float> v = out[i + k + half] * w;
                    out[i + k] = u + v;
                    out[i + k + half] = u - v;
                    w *= wlen;
                }
            }
        }
    }

    static bool parseJson(const std::string& text, FftCoreConfig& out,
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
                errorOut = "FftCore config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "FftCore config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "FftCore config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "FftCore config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "FftCore config: unterminated string";
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
                errorOut = "FftCore config: expected ',' or '}'";
                return false;
            }
        }

        FftCoreConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "FftCore config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "maxSize") {
                parsed.maxSize = static_cast<std::uint32_t>(std::stoul(p.value));
            } else if (p.key == "seed") {
                parsed.seed = static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "FftCore config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "FftCore config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "FftCore config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    FftCoreConfig config_{};
};

}  // namespace

bool FftCoreConfig::valid(std::string& errorOut) const {
    if (!(maxSize >= 2 && maxSize <= 65536) ||
        (maxSize & (maxSize - 1)) != 0) {
        errorOut = "maxSize must be a power of two in [2, 65536]";
        return false;
    }
    if (seed == 0) {
        errorOut = "seed must be non-zero";
        return false;
    }
    return true;
}

bool IFftCore::isPowerOfTwo(std::uint32_t n) noexcept {
    return n >= 1 && (n & (n - 1)) == 0;
}

std::uint32_t IFftCore::nextPowerOfTwo(std::uint32_t n) noexcept {
    std::uint32_t p = 1;
    while (p < n && p < 0x80000000u) {
        p <<= 1;
    }
    return p;
}

std::unique_ptr<IFftCore> create_fft_core(std::string& errorOut) {
    auto impl = std::make_unique<FftCore>();
    if (!impl) {
        errorOut = "FftCore: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IFftCore> create_fft_core_json(const std::string& jsonText,
                                               std::string& errorOut) {
    auto impl = std::make_unique<FftCore>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering

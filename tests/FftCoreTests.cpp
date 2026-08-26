// FftCoreTests.cpp — Agente 1 (task_plan C.14, vkfft): headless gate for the
// PUBLIC deterministic FFT contract (IFftCore). Proves the pure core against
// the naive DFT definition, delta/constant/sine known pairs, round-trip
// identity, Parseval and linearity — no GPU required.

#include "engine/rendering/IFftCore.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

void check(bool condition, const std::string& message) {
    check(condition, message.c_str());
}

bool near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::IFftCore;
using Engine::Rendering::FftCoreConfig;
using Engine::Rendering::create_fft_core;
using Engine::Rendering::create_fft_core_json;

// Naive O(N^2) DFT from the definition, for cross-checking.
std::vector<std::complex<float>> naiveDft(
    const std::vector<std::complex<float>>& x) {
    const std::size_t n = x.size();
    std::vector<std::complex<float>> X(n);
    const float twoPi = 2.0f * 3.14159265358979f;
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<float> sum(0.0f, 0.0f);
        for (std::size_t t = 0; t < n; ++t) {
            const float ang = -twoPi * static_cast<float>(k * t) /
                              static_cast<float>(n);
            sum += x[t] * std::complex<float>(std::cos(ang), std::sin(ang));
        }
        X[k] = sum;
    }
    return X;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto f = create_fft_core(error);
        check(f != nullptr, "default fft core created");
        check(error.empty(), "default config diagnostic empty");

        FftCoreConfig bad = f->config();
        bad.maxSize = 3;  // not a power of two
        check(!f->configure(bad, error) && !error.empty(),
              "maxSize 3 (not pow2) refused");
        bad = f->config();
        bad.maxSize = 1;
        check(!f->configure(bad, error) && !error.empty(), "maxSize 1 refused");
        bad = f->config();
        bad.maxSize = 131072;
        check(!f->configure(bad, error) && !error.empty(),
              "maxSize 131072 refused");
        bad = f->config();
        bad.seed = 0;
        check(!f->configure(bad, error) && !error.empty(), "seed 0 refused");
        check(f->config().maxSize == 4096,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_fft_core(error);
        FftCoreConfig c = a->config();
        c.maxSize = 1024;
        c.seed = 9;
        check(a->configure(c, error), "custom config applied");

        auto b = create_fft_core_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().maxSize == 1024 && b->config().seed == 9,
              "json round-trip bit-exact");

        check(create_fft_core_json("{ \"version\": 1, \"bogus\": 1 }",
                                   error) == nullptr,
              "unknown key refused");
        check(create_fft_core_json("{ \"version\": 2 }", error) == nullptr,
              "unsupported version refused");
        check(create_fft_core_json("{ \"version\": 1, \"maxSize\": 6 }",
                                   error) == nullptr,
              "invalid json config refused all-or-nothing");
    }

    // ---- 3. helpers ----
    {
        check(IFftCore::isPowerOfTwo(1) && IFftCore::isPowerOfTwo(2) &&
                  IFftCore::isPowerOfTwo(1024),
              "isPowerOfTwo true cases");
        check(!IFftCore::isPowerOfTwo(0) && !IFftCore::isPowerOfTwo(3) &&
                  !IFftCore::isPowerOfTwo(1000),
              "isPowerOfTwo false cases");
        check(IFftCore::nextPowerOfTwo(3) == 4 &&
                  IFftCore::nextPowerOfTwo(4) == 4 &&
                  IFftCore::nextPowerOfTwo(1000) == 1024,
              "nextPowerOfTwo rounds up to the next power");
    }

    // ---- 4. known pairs: delta, constant, sine ----
    {
        std::string error;
        auto f = create_fft_core(error);

        // delta at 0 -> all ones
        std::vector<std::complex<float>> delta(8, {0.0f, 0.0f});
        delta[0] = {1.0f, 0.0f};
        std::vector<std::complex<float>> X;
        check(f->fft(delta, X), "delta fft runs");
        bool allOne = X.size() == 8;
        for (const auto& v : X) {
            if (!near(v.real(), 1.0f) || !near(v.imag(), 0.0f)) allOne = false;
        }
        check(allOne, "fft(delta) = all ones");

        // constant -> delta at 0 with magnitude N
        std::vector<std::complex<float>> cnst(8, {1.0f, 0.0f});
        check(f->fft(cnst, X), "constant fft runs");
        check(near(X[0].real(), 8.0f) && near(X[0].imag(), 0.0f),
              "fft(constant) = N at index 0");
        bool restZero = true;
        for (std::size_t i = 1; i < X.size(); ++i) {
            if (std::fabs(X[i].real()) > 1e-3f || std::fabs(X[i].imag()) > 1e-3f)
                restZero = false;
        }
        check(restZero, "fft(constant) = 0 elsewhere");

        // sine of frequency 3 -> spikes at k=3 and k=N-3 with magnitude N/2
        std::vector<std::complex<float>> sine(16, {0.0f, 0.0f});
        const float twoPi = 2.0f * 3.14159265358979f;
        for (std::size_t t = 0; t < 16; ++t) {
            sine[t] = {std::sin(twoPi * 3.0f * static_cast<float>(t) / 16.0f),
                       0.0f};
        }
        check(f->fft(sine, X), "sine fft runs");
        check(near(std::abs(X[3]), 8.0f) && near(std::abs(X[13]), 8.0f),
              "sine of freq 3 -> magnitude N/2 at k=3 and k=N-3");
    }

    // ---- 5. cross-check vs the naive DFT ----
    {
        std::string error;
        auto f = create_fft_core(error);
        std::vector<std::complex<float>> x(16);
        for (std::size_t i = 0; i < 16; ++i) {
            x[i] = {std::sin(0.7f * static_cast<float>(i)) +
                        0.3f * static_cast<float>(i % 5),
                    std::cos(1.3f * static_cast<float>(i))};
        }
        std::vector<std::complex<float>> X;
        check(f->fft(x, X), "cross-check fft runs");
        const auto naive = naiveDft(x);
        bool match = true;
        for (std::size_t k = 0; k < 16; ++k) {
            if (!near(X[k].real(), naive[k].real(), 5e-2f) ||
                !near(X[k].imag(), naive[k].imag(), 5e-2f)) {
                match = false;
            }
        }
        check(match, "fft matches the naive DFT definition");
    }

    // ---- 6. round-trip identity: ifft(fft(x)) / N = x ----
    {
        std::string error;
        auto f = create_fft_core(error);
        std::vector<std::complex<float>> x(32);
        for (std::size_t i = 0; i < 32; ++i) {
            x[i] = {0.1f * static_cast<float>(i),
                    0.2f * std::sin(0.3f * static_cast<float>(i))};
        }
        std::vector<std::complex<float>> X, back;
        check(f->fft(x, X), "roundtrip fft runs");
        check(f->ifft(X, back), "roundtrip ifft runs");
        bool ok = true;
        for (std::size_t i = 0; i < 32; ++i) {
            if (!near(back[i].real(), x[i].real(), 1e-3f) ||
                !near(back[i].imag(), x[i].imag(), 1e-3f)) {
                ok = false;
            }
        }
        check(ok, "ifft(fft(x)) / N reproduces x (identity)");
    }

    // ---- 7. Parseval: sum|x|^2 = sum|X|^2 / N ----
    {
        std::string error;
        auto f = create_fft_core(error);
        std::vector<std::complex<float>> x(64);
        for (std::size_t i = 0; i < 64; ++i) {
            x[i] = {std::sin(2.0f * static_cast<float>(i)),
                    0.5f * std::cos(1.1f * static_cast<float>(i))};
        }
        std::vector<std::complex<float>> X;
        check(f->fft(x, X), "parseval fft runs");
        double eTime = 0.0, eFreq = 0.0;
        for (std::size_t i = 0; i < 64; ++i) {
            eTime += std::norm(x[i]);
            eFreq += std::norm(X[i]);
        }
        eFreq /= 64.0;
        check(std::fabs(eTime - eFreq) / eTime < 1e-3,
              "Parseval: energy preserved (|x|^2 = |X|^2/N)");
    }

    // ---- 8. linearity: fft(a + b) = fft(a) + fft(b) ----
    {
        std::string error;
        auto f = create_fft_core(error);
        std::vector<std::complex<float>> a(16), b(16), sum(16);
        for (std::size_t i = 0; i < 16; ++i) {
            a[i] = {static_cast<float>(i), 0.5f};
            b[i] = {0.25f * static_cast<float>(i % 4), -0.3f};
            sum[i] = a[i] + b[i];
        }
        std::vector<std::complex<float>> A, B, S;
        check(f->fft(a, A) && f->fft(b, B) && f->fft(sum, S), "linearity runs");
        bool ok = true;
        for (std::size_t k = 0; k < 16; ++k) {
            const auto expect = A[k] + B[k];
            if (!near(S[k].real(), expect.real(), 1e-3f) ||
                !near(S[k].imag(), expect.imag(), 1e-3f)) {
                ok = false;
            }
        }
        check(ok, "fft is linear: fft(a+b) = fft(a) + fft(b)");
    }

    // ---- 9. refusals: invalid sizes leave output untouched ----
    {
        std::string error;
        auto f = create_fft_core(error);
        std::vector<std::complex<float>> out = {{9.0f, 9.0f}};
        std::vector<std::complex<float>> empty;
        check(!f->fft(empty, out) && out[0] == std::complex<float>(9.0f, 9.0f),
              "empty input refused, output untouched");
        std::vector<std::complex<float>> odd(3, {1.0f, 0.0f});
        check(!f->fft(odd, out) && out[0] == std::complex<float>(9.0f, 9.0f),
              "non-power-of-two input refused, output untouched");
        std::vector<std::complex<float>> big(8192, {1.0f, 0.0f});
        check(!f->fft(big, out) && out[0] == std::complex<float>(9.0f, 9.0f),
              "size > maxSize refused, output untouched");
    }

    // ---- 10. determinism (bit-exact) ----
    {
        std::string error;
        auto f = create_fft_core(error);
        std::vector<std::complex<float>> x(32);
        for (std::size_t i = 0; i < 32; ++i) {
            x[i] = {std::sin(0.9f * static_cast<float>(i)),
                    std::cos(0.4f * static_cast<float>(i))};
        }
        std::vector<std::complex<float>> a, b;
        check(f->fft(x, a) && f->fft(x, b), "determinism runs");
        check(a.size() == b.size() &&
                  std::memcmp(a.data(), b.data(), a.size() * sizeof(std::complex<float>)) == 0,
              "identical inputs reproduce bit-exact fft output");
    }

    if (g_failures == 0) {
        std::printf("[fft-core] ALL PASSED\n");
        return 0;
    }
    std::printf("[fft-core] %d FAILURE(S)\n", g_failures);
    return 1;
}

#pragma once

// IFftCore — Agente 1 (task_plan C.14, vkfft), the PUBLIC deterministic FFT
// nucleus: an iterative radix-2 Cooley-Tukey transform used by the ocean /
// weather / spectral-effect cores. One surface for the transform MATH a
// renderer needs, without depending on the concrete backend (vkfft is the GPU
// vendor seam; the ALGORITHM is pure).
//
// SCOPE: the deterministic, headless ALGORITHM of the discrete Fourier
// transform for power-of-two sizes:
//   fft   — forward transform (iterative radix-2 Cooley-Tukey with
//           bit-reversal permutation), complex float;
//   ifft  — inverse transform with 1/N scaling (fft then ifft / N is the
//           identity up to float rounding);
//   helpers — power-of-two checks and next-power-of-two for spectrum grids
//           (ocean height-field tiles, weather spectra).
// Verified against the naive DFT definition (O(N^2)) in the gate. Bit-exact
// for the same inputs on every machine. Self-contained (std).

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct FftCoreConfig {
    std::uint32_t maxSize{ 4096 };  // maximum transform size (power of two) [2, 65536]
    std::uint32_t seed{ 1 };        // deterministic (reserved)

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan C.14) ----

class IFftCore {
public:
    virtual ~IFftCore() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const FftCoreConfig& config, std::string& errorOut) = 0;
    virtual const FftCoreConfig& config() const noexcept = 0;

    // JSON {maxSize, seed, version:1}. version != 1 or a malformed field
    // refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Forward FFT. `input` size must be a power of two in [1, maxSize]; the
    // output receives the same number of complex samples. Refuses
    // all-or-nothing (output untouched) otherwise.
    virtual bool fft(const std::vector<std::complex<float>>& input,
                     std::vector<std::complex<float>>& output) const = 0;

    // Inverse FFT with 1/N scaling. Same validation as fft.
    virtual bool ifft(const std::vector<std::complex<float>>& input,
                      std::vector<std::complex<float>>& output) const = 0;

    static bool isPowerOfTwo(std::uint32_t n) noexcept;
    static std::uint32_t nextPowerOfTwo(std::uint32_t n) noexcept;
};

// ---- public factory ----

std::unique_ptr<IFftCore> create_fft_core(std::string& errorOut);
std::unique_ptr<IFftCore> create_fft_core_json(const std::string& jsonText,
                                               std::string& errorOut);

}  // namespace Engine::Rendering

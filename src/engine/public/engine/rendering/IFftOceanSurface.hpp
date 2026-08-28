#pragma once

// IFftOceanSurface — Agente 1 (task_plan C.20 vkfft — NOVO, criado do zero em
// código nativo da engine, sem o vendor VkFFT). Fornece o caminho de produto
// do oceano indicado pelo seam `fft` (IFFT sobre tiles de heightfield): um
// espectro oceânico de Tessendorf (Phillips/JONSWAP-like com componentes
// variando no tempo) que é irtansformado com o `IFftCore` nativo em um campo
// de altura + gradiente (cruzamento/choppiness) por tile — tudo determinístico
// e headless.
//
// SCOPE: o ALGORITMO determinístico de síntese de oceano:
//   spectrum    — energia |zeta0(k)|^2 de Tessendorf a partir de windSpeed,
//                 windDir e tamanho do tile, com termo de direccionalidade e
//                 alisamento por freq pequena;
//   generate    — amostra o espectro em kx/kz, aplica a fase aleatória
//                 (seed) e o termo exp(i ω(k) t), iFFT 2D via IFftCore (2 passadas
//                 1D) e devolve altura (neg em z) + deslocamento horizontal
//                 (choppiness) + normal por vértice;
//   determinism — mesmas seed/parâmetros reproduzem bit-exact o mesmo campo.
// Usado pelo renderer para deformar a malha do oceano (seam fft no task_plan).
// Self-contained (std + glm + IFftCore).

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----
struct FftOceanConfig {
    std::uint32_t size{ 64 };       // tile size per axis (power of two) [16, 1024]
    float tileSizeMeters{ 256.0f }; // world size of the tile [16, 8192]
    float windSpeed{ 18.0f };       // wind speed m/s [0.5, 40]
    float windDirRad{ 0.7f };       // main wave direction (radians) any finite
    float choppiness{ 1.2f };       // horizontal displacement scale [0, 4]
    float amplitude{ 0.9f };        // spectrum amplitude scale [0.01, 8]
    std::uint32_t seed{ 1 };        // deterministic phase seed

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// One synthesized ocean vertex.
struct FftOceanVertex {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };  // displaced world position
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };    // perturbed normal
    glm::vec2 grid{ 0.0f, 0.0f };            // original xz tile coords [0,tileSize]
    float height{ 0.0f };                    // signed height (pre-displacement)
};

class IFftOceanSurface {
public:
    virtual ~IFftOceanSurface() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const FftOceanConfig& config, std::string& errorOut) = 0;
    virtual const FftOceanConfig& config() const noexcept = 0;

    // JSON {size, tileSizeMeters, windSpeed, windDirRad, choppiness,
    // amplitude, seed, version:1}. version != 1 or a malformed field refuses
    // all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Spectrum energy |zeta0|^2 at wave vector (kx, kz) (rad/m). Deterministic.
    virtual float spectrum(float kx, float kz) const noexcept = 0;

    // Synthesize the tile at simulation time `time` (seconds). Output is a
    // size*size grid of vertices in row-major (z-major), grid.x = x, grid.y =
    // z in [0, tileSize]. Deterministic: identical config+time reproduce
    // bit-exact output. Returns false (all-or-nothing, output untouched) on
    // invalid params (non-finite time).
    virtual bool synthesize(float time, std::vector<FftOceanVertex>& output,
                            std::string& errorOut) const = 0;
};

// ---- public factory ----

std::unique_ptr<IFftOceanSurface> create_fft_ocean_surface(std::string& errorOut);
std::unique_ptr<IFftOceanSurface> create_fft_ocean_surface_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering
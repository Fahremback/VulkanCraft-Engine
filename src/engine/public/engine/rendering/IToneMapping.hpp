#pragma once

// IToneMapping — Agente 1 (task_plan B.7), the PUBLIC HDR tone mapping core:
// exposure (manual or EV-based), the ACES / Reinhard / Filmic operators and
// linear <-> sRGB color management, with deterministic handling of HDR
// emissives (values far above 1 map consistently to [0, 1]). One surface for
// the color MATH of the HDR pipeline, without depending on the concrete
// backend.
//
// SCOPE: the deterministic, headless ALGORITHM of the HDR -> LDR mapping:
//   exposure   — photographic EV exposure: 1 / (1.2 * 2^EV), or a manual
//                multiplier; applied to the linear radiance BEFORE the operator;
//   operators  — Reinhard x/(1+x); ACES (Narkowicz 2015) saturate form; Filmic
//                (Uncharted 2 style, normalized at the white point); None
//                (exposure + clamp only). All map any HDR input to [0, 1]
//                monotonically and deterministically;
//   emissives  — HDR emissive values (e.g. 1000) saturate consistently through
//                the same curve (the "consistent emissives" guarantee);
//   color      — exact linear <-> sRGB conversions (piecewise), both
//                directions, bit-stable.
// Self-contained (std + glm), bit-exact for the same inputs on every machine.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

enum class ToneOperator : std::uint8_t {
    Reinhard,
    ACES,
    Filmic,
    None,
    Count
};

// ---- config (validated all-or-nothing) ----

struct ToneMappingConfig {
    ToneOperator op{ ToneOperator::ACES };
    float exposure{ 1.0f };    // manual multiplier [0.01, 64]
    bool useEV{ false };       // when true, EV-based exposure overrides manual
    float ev100{ 0.0f };       // EV value [-16, 16] -> 1/(1.2 * 2^EV)
    float whitePoint{ 11.2f }; // Filmic white point (and Reinhard extended) [1, 64]

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan B.7) ----

class IToneMapping {
public:
    virtual ~IToneMapping() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const ToneMappingConfig& config,
                           std::string& errorOut) = 0;
    virtual const ToneMappingConfig& config() const noexcept = 0;

    // JSON {op, exposure, useEV, ev100, whitePoint, version:1}. version != 1
    // or a malformed field refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Effective exposure: manual, or 1 / (1.2 * 2^EV) when useEV.
    virtual float exposureFactor() const noexcept = 0;

    // One operator on a single channel (>= 0, maps to [0, 1] monotonically).
    virtual float tonemapChannel(float x) const noexcept = 0;

    // Full pipeline: exposure * linear, then the operator, then clamp [0, 1].
    virtual glm::vec3 apply(const glm::vec3& linear) const noexcept = 0;

    // Exact linear <-> sRGB conversions (piecewise), scalar and vec3.
    static float linearToSrgb(float c) noexcept;
    static float srgbToLinear(float c) noexcept;
    static glm::vec3 linearToSrgb(const glm::vec3& c) noexcept;
    static glm::vec3 srgbToLinear(const glm::vec3& c) noexcept;
};

// ---- public factory ----

std::unique_ptr<IToneMapping> create_tone_mapping(std::string& errorOut);
std::unique_ptr<IToneMapping> create_tone_mapping_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering

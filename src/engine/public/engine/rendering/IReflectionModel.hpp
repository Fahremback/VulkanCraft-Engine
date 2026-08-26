#pragma once

// IReflectionModel — Agente 1 (task_plan A.13), the PUBLIC reflection
// reflectance core: roughness-dependent reflections, clear coat, water and
// transparency, plus the screen/probe fallback blend policy. One surface for
// the reflection MATH that a Lumen-style renderer needs, without depending on
// the concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM of reflection shading:
// Fresnel (Schlick, roughness-corrected), the GGX roughness -> reflection
// cone spread, a two-layer clear-coat Fresnel, water (IOR Fresnel +
// Beer-Lambert transmission) and the roughness-driven blend weight between a
// sharp (screen/RT) and a low-frequency (probe) reflection source. Backend
// SELECTION (which source exists) is the IReflectionProvider seam (A.1); this
// core computes the reflectance math and the blend policy. Self-contained
// (std + glm), bit-exact for the same inputs on every machine.
//
// MATH:
//   fresnel(cos, f0, r)  — Schlick with the Frostbite roughness correction:
//        F0' = max(1 - r, f0);  F = F0' + (1 - F0')(1 - cos)^5
//     (r = 0 reproduces plain Schlick, F(0) = 1; rough surfaces saturate at
//      the max(1 - r, f0) asymptote instead of 1).
//   roughnessSpread(r)    — reflection cone half-angle = atan(r) (0 at r = 0,
//     pi/4 at r = 1): the blur radius of the reflected lobe.
//   clearCoatFresnel      — two dielectric layers: F = F_coat +
//     (1 - F_coat)^2 * F_base (coat reflects first, the rest reaches the base
//     and reflects back through the coat). Coat is smooth (plain Schlick);
//     the base uses the roughness-corrected form with coatRoughness.
//   waterFresnel          — plain Schlick from F0 = ((n-1)/(n+1))^2.
//   beerLambert           — exp(-absorption * thickness).
//   evaluate / blend      — the per-surface policy: water is always sharp
//     (screenWeight = 1); otherwise screenWeight = 1 - clamp(r / limit, 0, 1)
//     and probeWeight = 1 - screenWeight. The reflected color is
//     mix(probe, screen, screenWeight) * fresnel; the transmission fraction
//     is exposed for the caller to composite the see-through term.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct ReflectionModelConfig {
    float screenRoughnessLimit{ 0.45f };   // roughness where screen reflections
                                           // fully give way to probes (0, 1]
    float waterIndexOfRefraction{ 1.333f };  // water F0 = ((n-1)/(n+1))^2 (> 1)
    float defaultDielectricF0{ 0.04f };      // F0 used when a surface has no
                                             // explicit f0 [0, 1)
    float waterAbsorptionPerMeter{ 0.6f };   // Beer-Lambert absorption [0, inf)

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan A.13) ----

struct ReflectionSurface {
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // unit, points out of the surface
    glm::vec3 viewDir{ 0.0f, 0.0f, 1.0f };  // unit, surface -> camera
    float roughness{ 0.5f };                // [0, 1] (GGX alpha = r * r)
    float metallic{ 0.0f };                 // [0, 1] (informational; caller
                                            // resolves f0 from albedo when metal)
    float f0{ 0.0f };                       // base reflectance at normal
                                            // incidence (0 = use config default)
    bool clearCoat{ false };                // dielectric coat over the base
    float coatF0{ 0.0f };                   // coat reflectance (0 = config default)
    bool water{ false };                    // water surface: IOR Fresnel + transmission
    float thickness{ 1.0f };                // meters, for Beer-Lambert (>= 0)
};

struct ReflectionResult {
    float fresnel{ 0.0f };         // reflectance of the surface at this angle
    float spreadAngleRad{ 0.0f };  // roughness-driven reflection cone half-angle
    float transmission{ 0.0f };    // see-through fraction (water/transparency)
    float screenWeight{ 0.0f };    // blend weight of the sharp (screen/RT) source
    float probeWeight{ 0.0f };     // blend weight of the low-frequency (probe) source
};

class IReflectionModel {
public:
    virtual ~IReflectionModel() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const ReflectionModelConfig& config,
                           std::string& errorOut) = 0;
    virtual const ReflectionModelConfig& config() const noexcept = 0;

    // JSON {screenRoughnessLimit, waterIndexOfRefraction,
    // defaultDielectricF0, waterAbsorptionPerMeter, version:1}. version != 1
    // or a malformed field refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Fresnel (Schlick, Frostbite roughness correction): cosTheta in [0, 1]
    // (clamped), f0 in [0, 1], roughness in [0, 1].
    virtual float fresnel(float cosTheta, float f0, float roughness) const noexcept = 0;

    // GGX roughness -> reflection cone half-angle in radians (atan(r)).
    virtual float roughnessSpread(float roughness) const noexcept = 0;

    // Two-layer clear coat: F = F_coat + (1 - F_coat)^2 * F_base. The coat is
    // smooth (plain Schlick); the base uses the roughness-corrected form.
    virtual float clearCoatFresnel(float cosTheta, float baseF0, float coatF0,
                                   float coatRoughness) const noexcept = 0;

    // Water Fresnel: plain Schlick from F0 = ((n-1)/(n+1))^2 of the config IOR.
    virtual float waterFresnel(float cosTheta) const noexcept = 0;

    // Beer-Lambert transmission: exp(-absorption * thickness).
    virtual float beerLambert(float thickness, float absorption) const noexcept = 0;

    // Full surface evaluation: fresnel, spread, transmission and the
    // screen/probe blend policy.
    virtual ReflectionResult evaluate(const ReflectionSurface& surface) const noexcept = 0;

    // Reflected color = mix(probe, screen, screenWeight) * fresnel.
    virtual glm::vec3 blend(const ReflectionSurface& surface,
                            const glm::vec3& probe,
                            const glm::vec3& screen) const noexcept = 0;
};

// ---- public factory ----

std::unique_ptr<IReflectionModel> create_reflection_model(std::string& errorOut);
std::unique_ptr<IReflectionModel> create_reflection_model_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering

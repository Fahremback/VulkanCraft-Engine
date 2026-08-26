#pragma once

// IVolumeClouds — Agente 1 (task_plan A.15), the PUBLIC volumetric cloud
// core: a deterministic density field, the Henyey-Greenstein phase function
// and a single-scattering raymarch with Beer-Lambert transmittance. One
// surface for the cloud LIGHTING MATH a renderer needs, without depending on
// the concrete backend. The sky (IAtmosphereScattering, A.15 #229) and the
// GPU/visual integration stay HUMAN-VISUAL-PENDING.
//
// SCOPE: the deterministic, headless ALGORITHM of cloud rendering:
//   density      — analytic, deterministic 3D density field: FBM value noise
//                  (seeded), a height bell profile, a coverage gate and a
//                  detail-noise modulation; zero outside the band;
//   phase        — Henyey-Greenstein P(cos) = (1-g^2) /
//                  (4*pi*(1+g^2-2*g*cos)^1.5): g = 0 isotropic, g > 0 forward
//                  peak, g < 0 backward peak;
//   march        — single-scattering raymarch along a ray: per-step density
//                  -> Beer-Lambert transmittance exp(-a*rho*ds) and
//                  inscatter += T * sigmaS * rho * phase * sun * ds.
// Self-contained (std + glm), bit-exact for the same inputs on every machine.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct VolumeCloudsConfig {
    float coverage{ 0.5f };          // base cloud coverage [0, 1] (0 = clear)
    float densityScale{ 1.0f };      // global density multiplier [0.01, 8]
    float detailStrength{ 0.35f };   // detail-noise modulation [0, 1]
    float lightAbsorption{ 1.2f };   // Beer-Lambert absorption per density [0, inf)
    float lightScatter{ 1.0f };      // scattering coefficient [0, inf)
    float phaseG{ 0.5f };            // HG forward/backward bias [-1, 1]
    float ambientScale{ 0.2f };      // ambient sky light [0, 1]
    std::uint32_t seed{ 1 };         // deterministic value-noise seed

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan A.15) ----

class IVolumeClouds {
public:
    virtual ~IVolumeClouds() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const VolumeCloudsConfig& config,
                           std::string& errorOut) = 0;
    virtual const VolumeCloudsConfig& config() const noexcept = 0;

    // JSON {coverage, densityScale, detailStrength, lightAbsorption,
    // lightScatter, phaseG, ambientScale, seed, version:1}. version != 1 or a
    // malformed field refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Local density at (x, y, z): the height band is y in [0, 1]. Always >= 0;
    // coverage 0 yields 0 everywhere (clear sky).
    virtual float density(float x, float y, float z) const noexcept = 0;

    // Henyey-Greenstein phase at cosTheta in [-1, 1] (solid-angle normalized).
    virtual float phase(float cosTheta) const noexcept = 0;

    // Single-scattering raymarch: accumulates Beer-Lambert transmittance and
    // in-scattered radiance along the ray. `steps` in [1, 128]; tMax > tMin;
    // sunDir must be unit length. Returns false (all-or-nothing, outputs
    // untouched) on invalid parameters. Deterministic: identical inputs
    // reproduce bit-exact outputs.
    virtual bool march(const glm::vec3& origin, const glm::vec3& dir,
                       float tMin, float tMax, std::uint32_t steps,
                       const glm::vec3& sunDir, const glm::vec3& sunColor,
                       float& transmittance, glm::vec3& inscatter) const noexcept = 0;
};

// ---- public factory ----

std::unique_ptr<IVolumeClouds> create_volume_clouds(std::string& errorOut);
std::unique_ptr<IVolumeClouds> create_volume_clouds_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering

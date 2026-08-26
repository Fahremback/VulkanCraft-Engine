#pragma once

// IAtmosphereScattering — Agente 1 (task_plan C): the PUBLIC contract for the
// analytic (texture-free) core of Bruneton's atmospheric scattering model
// (BSD-3-Clause, promoted from external/solutions/atmospheric-scattering).
// Exposes the deterministic CPU primitives that a future sky provider (A.15)
// consumes: density profiles, optical length and spectral transmittance
// (47 samples, 360..830 nm in 10 nm bins) along a ray to the top of the
// atmosphere, plus the ANALYTIC single-scattering sky radiance at a ground
// observer (the reference model's trapezoidal integral with the Rayleigh and
// Mie phase functions). The texture-cached LUTs and multiple scattering stay
// GPU/HUMAN-VISUAL-PENDING.
//
// Self-contained std; no external headers. Deterministic. All-or-nothing:
// invalid geometry (r < bottom radius, |mu| > 1, altitude out of range)
// yields NaN instead of a garbage number.

#include <array>
#include <cstdint>
#include <memory>

namespace Engine::Rendering {

inline constexpr int kAtmosphereSpectralSamples = 47;
inline constexpr double kAtmosphereLambdaMinNm = 360.0;
inline constexpr double kAtmosphereLambdaMaxNm = 830.0;

// Canonical Earth geometry (Bruneton, full precision).
inline constexpr double kEarthBottomRadiusM = 6360000.0;
inline constexpr double kEarthTopRadiusM = 6420000.0;

// Which density profile to query.
enum class AtmosphereProfile : std::uint8_t {
    Rayleigh,   // air molecules
    Mie,        // aerosols
    Absorption, // ozone
};

// One value per wavelength sample (index i -> lambda = 360 + 10*i nm).
using AtmosphereSpectrum = std::array<double, kAtmosphereSpectralSamples>;

class IAtmosphereScattering {
public:
    virtual ~IAtmosphereScattering() = default;

    // Wavelength (nm) of spectral sample `i`.
    static double wavelengthNm(int i) {
        return kAtmosphereLambdaMinNm + 10.0 * i;
    }

    // Dimensionless density (0..1) of `profile` at `altitude` meters above the
    // surface. NaN if altitude is outside [0, top-bottom].
    virtual double density(AtmosphereProfile profile, double altitude) const = 0;

    // Integrated optical length (meters) along the ray from radius `r` (meters
    // from planet center) with direction cosine `mu` = cos(zenith) to the top
    // of the atmosphere, for `profile`. NaN if invalid (r < bottom, |mu| > 1).
    virtual double opticalLength(AtmosphereProfile profile, double r, double mu) const = 0;

    // Spectral transmittance (dimensionless, in (0,1]) from radius `r` with
    // direction cosine `mu` to the top of the atmosphere, combining Rayleigh +
    // Mie extinction + ozone absorption. All samples NaN if invalid.
    virtual AtmosphereSpectrum transmittance(double r, double mu) const = 0;

    // Single-scattering sky radiance (W·m^-2·nm^-1·sr^-1) at a ground observer,
    // spectral (47 samples, same bins as transmittance). The reference model's
    // analytic single-scattering integral (trapezoidal, 50 intervals) including
    // the Rayleigh and Mie phase functions; multiple scattering and the
    // texture-cached LUTs stay GPU/HUMAN-VISUAL-PENDING.
    //   viewZenithCos: cos of the view zenith angle in [0, 1] (1 = zenith,
    //       0 = horizon; below-horizon rays hit the ground and are refused).
    //   sunZenithCos: cos of the sun zenith angle in [-1, 1] (negative = sun
    //       below the horizon, e.g. twilight; the reference clamps at cos(102°)).
    //   viewSunDeltaAzimuthRad: azimuth (radians) between the view and sun
    //       directions, any finite value (wrapped internally).
    // All samples NaN if invalid (non-finite input, viewZenithCos outside
    // [0,1], sunZenithCos outside [-1,1]).
    virtual AtmosphereSpectrum skyRadiance(double viewZenithCos,
                                           double sunZenithCos,
                                           double viewSunDeltaAzimuthRad) const = 0;
};

// Creates the canonical Earth atmosphere (Bruneton's parameters).
std::unique_ptr<IAtmosphereScattering> create_atmosphere_scattering();

}  // namespace Engine::Rendering

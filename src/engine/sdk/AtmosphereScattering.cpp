// AtmosphereScattering.cpp — Agente 1 (task_plan C): the ONLY TU that includes
// the vendored atmospheric-scattering headers (external/solutions/
// atmospheric-scattering, Eric Bruneton, BSD-3-Clause, commit 34f14e7…). Maps
// the analytic core of the reference model to the public contract
// engine/rendering/IAtmosphereScattering.hpp. Pure CPU, deterministic,
// all-or-nothing (NaN on invalid geometry).
//
// BUG-010 lesson: the vendored functions.cc is compiled INTO vc_sdk_public
// (see CMakeLists.txt) so the model symbols travel with this adapter's objects.

#include "engine/rendering/IAtmosphereScattering.hpp"

#include "atmosphere/constants.h"
#include "atmosphere/reference/functions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace Engine::Rendering {
namespace {

using namespace atmosphere::reference;

// Ozone absorption cross-section per 10 nm bin (360..830 nm), m^2.
// Bruneton model_test.cc, values from the MPI-Mainz ozone database (233 K).
constexpr double kOzoneCrossSection[48] = {
    1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27,
    8.451e-27, 1.582e-26, 2.316e-26, 3.669e-26, 4.924e-26, 7.752e-26,
    9.016e-26, 1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25, 3.091e-25, 3.5e-25,
    4.266e-25, 4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25,
    3.74e-25, 3.215e-25, 2.662e-25, 2.238e-25, 1.852e-25, 1.473e-25,
    1.209e-25, 9.423e-26, 7.455e-26, 6.566e-26, 5.105e-26, 4.15e-26,
    4.228e-26, 3.237e-26, 2.451e-26, 2.801e-26, 2.534e-26, 1.624e-26,
    1.465e-26, 2.078e-26, 1.383e-26, 7.105e-27,
};

// Solar irradiance at the top of the atmosphere, W·m^-2·nm^-1 per 10 nm bin
// (360..830 nm). Bruneton model_test.cc, values from
// http://www.iup.uni-bremen.de/gruppen/molspec/databases/referencespectra/
// o3spectra2011 (and standard solar spectra), summed/averaged per bin.
constexpr double kSolarIrradiance[48] = {
    1.11776, 1.14259, 1.01249, 1.14716, 1.72765, 1.73054, 1.6887, 1.61253,
    1.91198, 2.03474, 2.02042, 2.02212, 1.93377, 1.95809, 1.91686, 1.8298,
    1.8685, 1.8931, 1.85149, 1.8504, 1.8341, 1.8345, 1.8147, 1.78158, 1.7533,
    1.6965, 1.68194, 1.64654, 1.6048, 1.52143, 1.55622, 1.5113, 1.474, 1.4482,
    1.41018, 1.36775, 1.34188, 1.31429, 1.28303, 1.26758, 1.2367, 1.2082,
    1.18737, 1.14683, 1.12362, 1.1058, 1.07124, 1.04992,
};

// Canonical Earth atmosphere (Bruneton demo.cc / model_test.cc, full precision).
const AtmosphereParameters& earthAtmosphere() {
    static const AtmosphereParameters p = [] {
        AtmosphereParameters a;

        constexpr ScatteringCoefficient kRayleigh = 1.24062e-6 / m;
        constexpr Length kRayleighScaleHeight = 8000.0 * m;
        constexpr Length kMieScaleHeight = 1200.0 * m;
        constexpr double kMieAngstromAlpha = 0.0;
        constexpr double kMieAngstromBeta = 5.328e-3;
        constexpr double kMieSingleScatteringAlbedo = 0.9;
        constexpr double kMiePhaseFunctionG = 0.8;
        // Dobson unit in molecules.m^-2; max ozone number density in m^-3
        // (300 DU over a 15 km column).
        constexpr dimensional::Scalar<-2, 0, 0, 0, 0> kDobsonUnit = 2.687e20 / m2;
        constexpr NumberDensity kMaxOzoneNumberDensity =
            300.0 * kDobsonUnit / (15.0 * km);

        std::vector<SpectralIrradiance> solar_irradiance;
        std::vector<ScatteringCoefficient> rayleigh_scattering;
        std::vector<ScatteringCoefficient> mie_scattering;
        std::vector<ScatteringCoefficient> mie_extinction;
        std::vector<ScatteringCoefficient> absorption_extinction;
        for (int l = 360; l <= 830; l += 10) {
            double lambda = static_cast<double>(l) * 1e-3;  // micro-meters
            ScatteringCoefficient rayleigh = kRayleigh * std::pow(lambda, -4);
            ScatteringCoefficient mie = kMieAngstromBeta / kMieScaleHeight *
                std::pow(lambda, -kMieAngstromAlpha);
            solar_irradiance.push_back(
                kSolarIrradiance[(l - 360) / 10] * watt_per_square_meter_per_nm);
            rayleigh_scattering.push_back(rayleigh);
            mie_scattering.push_back(mie * kMieSingleScatteringAlbedo);
            mie_extinction.push_back(mie);
            absorption_extinction.push_back(kMaxOzoneNumberDensity *
                kOzoneCrossSection[(l - 360) / 10] * m2);
        }

        a.bottom_radius = 6360.0 * km;
        a.top_radius = 6420.0 * km;
        a.solar_irradiance = IrradianceSpectrum(
            360.0 * nm, 830.0 * nm, solar_irradiance);
        a.rayleigh_density.layers[1] = DensityProfileLayer(
            0.0 * m, 1.0, -1.0 / kRayleighScaleHeight, 0.0 / m, 0.0);
        a.rayleigh_scattering = ScatteringSpectrum(
            360.0 * nm, 830.0 * nm, rayleigh_scattering);
        a.mie_density.layers[1] = DensityProfileLayer(
            0.0 * m, 1.0, -1.0 / kMieScaleHeight, 0.0 / m, 0.0);
        a.mie_scattering = ScatteringSpectrum(
            360.0 * nm, 830.0 * nm, mie_scattering);
        a.mie_extinction = ScatteringSpectrum(
            360.0 * nm, 830.0 * nm, mie_extinction);
        a.mie_phase_function_g = kMiePhaseFunctionG;
        // Ozone: linear 0->1 between 10 and 25 km, 1->0 between 25 and 40 km.
        a.absorption_density.layers[0] = DensityProfileLayer(
            25.0 * km, 0.0, 0.0 / km, 1.0 / (15.0 * km), -2.0 / 3.0);
        a.absorption_density.layers[1] = DensityProfileLayer(
            0.0 * km, 0.0, 0.0 / km, -1.0 / (15.0 * km), 8.0 / 3.0);
        a.absorption_extinction = ScatteringSpectrum(
            360.0 * nm, 830.0 * nm, absorption_extinction);

        return a;
    }();
    return p;
}

const DensityProfile& profileOf(AtmosphereProfile profile) {
    const AtmosphereParameters& a = earthAtmosphere();
    switch (profile) {
        case AtmosphereProfile::Rayleigh: return a.rayleigh_density;
        case AtmosphereProfile::Mie: return a.mie_density;
        case AtmosphereProfile::Absorption: return a.absorption_density;
    }
    return a.rayleigh_density;  // unreachable
}

class AtmosphereScattering final : public IAtmosphereScattering {
public:
    double density(AtmosphereProfile profile, double altitude) const override {
        const double max_altitude = kEarthTopRadiusM - kEarthBottomRadiusM;
        if (!std::isfinite(altitude) || altitude < 0.0 || altitude > max_altitude) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return GetProfileDensity(profileOf(profile), altitude * m)();
    }

    double opticalLength(AtmosphereProfile profile, double r, double mu) const override {
        if (!std::isfinite(r) || !std::isfinite(mu) || mu < -1.0 || mu > 1.0 ||
            r < kEarthBottomRadiusM) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return ComputeOpticalLengthToTopAtmosphereBoundary(
            earthAtmosphere(), profileOf(profile), r * m, mu).to(m);
    }

    AtmosphereSpectrum transmittance(double r, double mu) const override {
        AtmosphereSpectrum out{};
        if (!std::isfinite(r) || !std::isfinite(mu) || mu < -1.0 || mu > 1.0 ||
            r < kEarthBottomRadiusM) {
            out.fill(std::numeric_limits<double>::quiet_NaN());
            return out;
        }
        const DimensionlessSpectrum t =
            ComputeTransmittanceToTopAtmosphereBoundary(earthAtmosphere(), r * m, mu);
        for (int i = 0; i < kAtmosphereSpectralSamples; ++i) {
            out[static_cast<std::size_t>(i)] = t[i]();
        }
        return out;
    }

    AtmosphereSpectrum skyRadiance(double viewZenithCos, double sunZenithCos,
                                   double viewSunDeltaAzimuthRad) const override {
        AtmosphereSpectrum out{};
        if (!std::isfinite(viewZenithCos) || !std::isfinite(sunZenithCos) ||
            !std::isfinite(viewSunDeltaAzimuthRad) ||
            viewZenithCos < 0.0 || viewZenithCos > 1.0 ||
            sunZenithCos < -1.0 || sunZenithCos > 1.0) {
            out.fill(std::numeric_limits<double>::quiet_NaN());
            return out;
        }
        const auto& a = earthAtmosphere();
        // Cosine of the scattering angle between the view and sun directions.
        const double sin_view = std::sqrt(1.0 - viewZenithCos * viewZenithCos);
        const double sin_sun = std::sqrt(1.0 - sunZenithCos * sunZenithCos);
        const double nu = std::clamp(
            viewZenithCos * sunZenithCos +
                sin_view * sin_sun * std::cos(viewSunDeltaAzimuthRad),
            -1.0, 1.0);

        // Fill the reference's transmittance LUT analytically (CPU array) and
        // run the reference's own single-scattering integral.
        TransmittanceTexture texture;
        for (int j = 0; j < atmosphere::TRANSMITTANCE_TEXTURE_HEIGHT; ++j) {
            for (int i = 0; i < atmosphere::TRANSMITTANCE_TEXTURE_WIDTH; ++i) {
                texture.Set(i, j, ComputeTransmittanceToTopAtmosphereBoundaryTexture(
                    a, vec2(i + 0.5, j + 0.5)));
            }
        }
        IrradianceSpectrum rayleigh;
        IrradianceSpectrum mie;
        ComputeSingleScattering(a, texture, a.bottom_radius, viewZenithCos,
                                sunZenithCos, nu, /*intersects_ground=*/false,
                                rayleigh, mie);
        const InverseSolidAngle phase_rayleigh = RayleighPhaseFunction(nu);
        const InverseSolidAngle phase_mie =
            MiePhaseFunction(a.mie_phase_function_g, nu);
        const double phase_rayleigh_per_sr = phase_rayleigh.to(1.0 / sr);
        const double phase_mie_per_sr = phase_mie.to(1.0 / sr);
        for (int i = 0; i < kAtmosphereSpectralSamples; ++i) {
            const double rayleigh_i =
                rayleigh[i].to(watt_per_square_meter_per_nm);
            const double mie_i = mie[i].to(watt_per_square_meter_per_nm);
            out[static_cast<std::size_t>(i)] =
                rayleigh_i * phase_rayleigh_per_sr +
                mie_i * phase_mie_per_sr;
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<IAtmosphereScattering> create_atmosphere_scattering() {
    return std::make_unique<AtmosphereScattering>();
}

}  // namespace Engine::Rendering

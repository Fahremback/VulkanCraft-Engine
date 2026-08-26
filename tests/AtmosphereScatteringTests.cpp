// AtmosphereScatteringTests.cpp — Agente 1 (task_plan C): headless gate for the
// PUBLIC atmosphere-scattering contract (IAtmosphereScattering). Exercises the
// analytic core of Bruneton's model with physical invariants + upstream golden
// values (density profile ozone triangle 10/25/40 km) + determinism. No GPU.

#include "engine/rendering/IAtmosphereScattering.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    auto a1 = create_atmosphere_scattering();
    auto a2 = create_atmosphere_scattering();

    // --- wavelength mapping ---
    check(near(IAtmosphereScattering::wavelengthNm(0), 360.0, 0.0),
          "wavelengthNm(0) != 360");
    check(near(IAtmosphereScattering::wavelengthNm(46), 820.0, 0.0),
          "wavelengthNm(46) != 820");
    check(near(IAtmosphereScattering::wavelengthNm(8), 440.0, 0.0),
          "wavelengthNm(8) != 440 (blue)");

    // --- density: Rayleigh exponential ---
    check(near(a1->density(AtmosphereProfile::Rayleigh, 0.0), 1.0, 1e-9),
          "Rayleigh density at surface != 1");
    check(near(a1->density(AtmosphereProfile::Rayleigh, 8000.0),
               std::exp(-1.0), 1e-6),
          "Rayleigh density at 8km != exp(-1)");
    check(near(a1->density(AtmosphereProfile::Mie, 1200.0), std::exp(-1.0), 1e-6),
          "Mie density at 1.2km != exp(-1)");

    // --- density: ozone triangle (upstream golden: 10km->0, 25km->1, 40km->0) ---
    check(near(a1->density(AtmosphereProfile::Absorption, 10000.0), 0.0, 1e-9),
          "ozone density at 10km != 0");
    check(near(a1->density(AtmosphereProfile::Absorption, 25000.0), 1.0, 1e-9),
          "ozone density at 25km != 1");
    check(near(a1->density(AtmosphereProfile::Absorption, 40000.0), 0.0, 1e-9),
          "ozone density at 40km != 0");

    // --- density: all-or-nothing ---
    check(std::isnan(a1->density(AtmosphereProfile::Rayleigh, -1.0)),
          "negative altitude must refuse (NaN)");
    check(std::isnan(a1->density(AtmosphereProfile::Rayleigh, 70000.0)),
          "altitude above atmosphere must refuse (NaN)");

    // --- optical length: vertical Rayleigh ~ scale height (8000 m) ---
    {
        double od = a1->opticalLength(AtmosphereProfile::Rayleigh,
                                      kEarthBottomRadiusM, 1.0);
        check(near(od, 7995.0, 30.0),
              "vertical Rayleigh optical length ~8km (got " +
                  std::to_string(od) + ")");
    }
    // --- optical length: all-or-nothing ---
    check(std::isnan(a1->opticalLength(AtmosphereProfile::Mie,
                                       kEarthBottomRadiusM - 1.0, 1.0)),
          "r below bottom radius must refuse (NaN)");
    check(std::isnan(a1->opticalLength(AtmosphereProfile::Rayleigh,
                                       kEarthBottomRadiusM, 2.0)),
          "|mu| > 1 must refuse (NaN)");

    // --- transmittance: physical invariants ---
    {
        const AtmosphereSpectrum up = a1->transmittance(kEarthBottomRadiusM, 1.0);
        for (int i = 0; i < kAtmosphereSpectralSamples; ++i) {
            if (!(up[i] > 0.0 && up[i] < 1.0)) {
                check(false, "vertical transmittance sample not in (0,1)");
                break;
            }
        }
        // Transmittance from the top (above atmosphere) is ~1 in every sample.
        const AtmosphereSpectrum top = a1->transmittance(kEarthTopRadiusM, 1.0);
        double max_err = 0.0;
        for (int i = 0; i < kAtmosphereSpectralSamples; ++i) {
            max_err = std::max(max_err, std::fabs(top[i] - 1.0));
        }
        check(max_err < 1e-6, "top-of-atmosphere transmittance ~1");

        // Rayleigh scatters blue more strongly: vertical transmittance at
        // 440 nm (index 8) is LESS than at 700 nm (index 34).
        check(up[8] < up[34], "blue transmittance must be < red transmittance");
    }

    // --- transmittance: horizontal < vertical (longer path -> more extinction) ---
    {
        const AtmosphereSpectrum horiz = a1->transmittance(kEarthBottomRadiusM, 0.0);
        const AtmosphereSpectrum vert  = a1->transmittance(kEarthBottomRadiusM, 1.0);
        check(horiz[34] < vert[34],
              "horizontal transmittance must be < vertical at 700nm");
    }

    // --- transmittance: all-or-nothing ---
    {
        const AtmosphereSpectrum bad = a1->transmittance(kEarthBottomRadiusM, 5.0);
        check(std::isnan(bad[0]), "invalid mu must yield all-NaN spectrum");
    }

    // --- determinism: two instances bit-identical ---
    {
        const AtmosphereSpectrum x1 = a1->transmittance(kEarthBottomRadiusM, 0.3);
        const AtmosphereSpectrum x2 = a2->transmittance(kEarthBottomRadiusM, 0.3);
        bool same = true;
        for (int i = 0; i < kAtmosphereSpectralSamples; ++i) {
            if (x1[i] != x2[i]) { same = false; break; }
        }
        check(same, "transmittance must be bit-identical across instances");
    }

    // === sky radiance (single scattering, analytic) ===

    // --- blue zenith sky is brighter than red (Rayleigh lambda^-4) ---
    {
        // Sun at zenith, looking at zenith.
        const AtmosphereSpectrum z = a1->skyRadiance(1.0, 1.0, 0.0);
        check(z[8] > z[34], "zenith sky: blue (450nm) brighter than red (700nm)");
        check(z[8] > 0.0 && std::isfinite(z[8]), "zenith sky: finite positive blue");
    }

    // --- looking toward the sun is brighter than away (Mie forward peak) ---
    {
        // Sun at 45 deg elevation, view at 45 deg, azimuth 0 (toward sun) vs PI.
        const double mu = std::sqrt(0.5);
        const double mu_s = std::sqrt(0.5);
        const AtmosphereSpectrum toward = a1->skyRadiance(mu, mu_s, 0.0);
        const AtmosphereSpectrum away   = a1->skyRadiance(mu, mu_s, 3.141592653589793);
        check(toward[8] > away[8], "sky toward the sun brighter than away (450nm)");
    }

    // --- horizon is brighter than zenith (longer scattering path) ---
    {
        const AtmosphereSpectrum horiz = a1->skyRadiance(0.0, 1.0, 0.0);
        const AtmosphereSpectrum vert  = a1->skyRadiance(1.0, 1.0, 0.0);
        check(horiz[34] > vert[34],
              "horizon red brighter than zenith red (longer path)");
    }

    // --- twilight: sun below horizon still lights the sky ---
    {
        const AtmosphereSpectrum tw = a1->skyRadiance(0.5, -0.1, 0.0);
        check(tw[8] > 0.0 && std::isfinite(tw[8]),
              "twilight (sun below horizon): positive finite blue");
    }

    // --- sky radiance: all-or-nothing refusals ---
    {
        const AtmosphereSpectrum down = a1->skyRadiance(-0.5, 1.0, 0.0);
        check(std::isnan(down[0]), "below-horizon view ray refused (all NaN)");
        const AtmosphereSpectrum bad_sun = a1->skyRadiance(0.5, 1.5, 0.0);
        check(std::isnan(bad_sun[0]), "|sunZenithCos|>1 refused (all NaN)");
        const AtmosphereSpectrum nan_in = a1->skyRadiance(
            std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0);
        check(std::isnan(nan_in[0]), "NaN input refused (all NaN)");
    }

    // --- sky radiance determinism: bit-identical across instances ---
    {
        const AtmosphereSpectrum s1 = a1->skyRadiance(0.3, 0.8, 1.7);
        const AtmosphereSpectrum s2 = a2->skyRadiance(0.3, 0.8, 1.7);
        bool same = true;
        for (int i = 0; i < kAtmosphereSpectralSamples; ++i) {
            if (s1[i] != s2[i]) { same = false; break; }
        }
        check(same, "sky radiance must be bit-identical across instances");
    }

    if (g_failures == 0) {
        std::printf("[atmosphere-scattering] ALL PASSED\n");
        return 0;
    }
    std::printf("[atmosphere-scattering] %d FAILURE(S)\n", g_failures);
    return 1;
}
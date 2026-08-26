// RenderingIntegrationTests.cpp — Agente 1 (task_plan D.3): the headless
// INTEGRATION gate of the renderer math. Composes the public rendering cores
// (IAtmosphereScattering -> IMaterialShading -> IToneMapping, plus
// IReflectionModel) into a mini scene pipeline and proves the D.3 clause:
//   "Cena interna sem luz fica escura; emissivo, sol, água e reflexos
//    respondem dinamicamente."
// The composition is the real one the renderer performs per surface sample:
//   sunEnergy  = spectral transmittance of the sun beam (0 at night)
//   skyAmbient = spectral single-scattering sky radiance at zenith
//   lit        = IMaterialShading::evaluate (direct diffuse + interior ambient)
//   linear     = direct * sunEnergy * (1-shadow) + ambient * skyAmbient
//                + emissive (HDR, e.g. from IBlockMaterialResolver lightEmission)
//   ldr        = IToneMapping::apply(linear)
// All contracts are the PUBLIC factories; no GPU, no window, no clock.

#include "engine/rendering/IAtmosphereScattering.hpp"
#include "engine/rendering/IMaterialShading.hpp"
#include "engine/rendering/IReflectionModel.hpp"
#include "engine/rendering/IToneMapping.hpp"

#include <cmath>
#include <cstdio>
#include <string>

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

using namespace Engine::Rendering;

double sumSpectrum(const AtmosphereSpectrum& s) {
    double t = 0.0;
    for (double v : s) t += v;
    return t;
}

// The mini scene pipeline of D.3 (per surface sample).
struct PipelineResult {
    double sunEnergy;    // spectral transmittance sum of the sun beam [0, 47]
    double skyAmbient;   // spectral sky radiance sum at zenith (W·m^-2·nm^-1·sr^-1)
    float litTotal;      // IMaterialShading total (direct + interior ambient)
    float linear;        // pre-tonemap radiance
    float ldr;           // after tone mapping (channel 0; scene is achromatic)
};

PipelineResult evaluateScene(IAtmosphereScattering& atmo,
                             IMaterialShading& shading,
                             IToneMapping& tone,
                             double sunZenithCos,
                             float shadowFactor,
                             float interiorDepth,
                             float emissive) {
    const double sunEnergy = sumSpectrum(
        atmo.transmittance(kEarthBottomRadiusM, sunZenithCos));
    const double skyAmbient = sumSpectrum(
        atmo.skyRadiance(1.0, sunZenithCos, 0.0));

    ShadingSurface surface;
    surface.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    surface.viewDir = glm::vec3(0.0f, 0.0f, 1.0f);
    surface.interiorDepth = interiorDepth;
    // Sun direction: above-horizon elevates; below-horizon is a grazing/below
    // direction whose cosine with the up normal is negative -> wrap light = 0.
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.0f, (float)sunZenithCos, 0.5f));
    const ShadingResult r = shading.evaluate(surface, sunDir, shadowFactor);

    // Scales turn the spectral sums into radiance: kSunScale matches the
    // magnitude of the transmittance sum (max ~47 -> ~1.0 sun radiance);
    // kSkyScale is a small ambient floor from the sky dome.
    constexpr double kSunScale = 0.02;
    constexpr double kSkyScale = 0.0001;
    const float linear = static_cast<float>(
        r.directDiffuse * sunEnergy * kSunScale * (1.0 - shadowFactor) +
        r.interiorAmbient * skyAmbient * kSkyScale +
        emissive);
    const float ldr = tone.apply(glm::vec3(linear)).x;
    PipelineResult out;
    out.sunEnergy = sunEnergy;
    out.skyAmbient = skyAmbient;
    out.litTotal = r.total;
    out.linear = linear;
    out.ldr = ldr;
    return out;
}

}  // namespace

int main() {
    std::string error;

    // ---- 1. configs of the four cores apply (the seams compose) ----
    auto atmo = create_atmosphere_scattering();
    auto shading = create_material_shading(error);
    check(shading != nullptr && error.empty(), "material shading factory");
    auto tone = create_tone_mapping(error);
    check(tone != nullptr && error.empty(), "tone mapping factory");
    auto reflections = create_reflection_model(error);
    check(reflections != nullptr && error.empty(), "reflection model factory");

    // ---- 2. "cena interna sem luz fica escura" ----
    {
        // Night (sun below horizon), fully shadowed, 10 m into an interior,
        // no emissive: the scene must stay really dark.
        const auto night = evaluateScene(*atmo, *shading, *tone,
                                         /*sunZenithCos=*/-1.0,
                                         /*shadowFactor=*/1.0f,
                                         /*interiorDepth=*/10.0f,
                                         /*emissive=*/0.0f);
        check(night.litTotal < 0.05f,
              "deep shadowed interior keeps total lit fraction under 0.05");
        check(night.ldr < 0.05f,
              "no-light interior is really dark on screen (ldr < 0.05)");
        check(night.skyAmbient < 1e-6,
              "night sky contributes no ambient radiance");
    }

    // ---- 3. "emissivo responde" ----
    {
        const auto dark = evaluateScene(*atmo, *shading, *tone,
                                        -1.0, 1.0f, 10.0f, 0.0f);
        const auto lit = evaluateScene(*atmo, *shading, *tone,
                                       -1.0, 1.0f, 10.0f, /*emissive=*/4.0f);
        check(lit.ldr > 0.8f,
              "HDR emissive 4.0 saturates through the tone curve (ldr > 0.8)");
        check(lit.ldr - dark.ldr > 0.5f,
              "emissive responds dynamically: +0.5 on screen vs the same dark scene");
    }

    // ---- 4. "sol responde" (monotonic with sun elevation) ----
    {
        const auto night = evaluateScene(*atmo, *shading, *tone,
                                         -1.0, 0.0f, 0.0f, 0.0f);
        const auto lowSun = evaluateScene(*atmo, *shading, *tone,
                                          0.1, 0.0f, 0.0f, 0.0f);
        const auto midSun = evaluateScene(*atmo, *shading, *tone,
                                          0.4, 0.0f, 0.0f, 0.0f);
        const auto highSun = evaluateScene(*atmo, *shading, *tone,
                                           0.6, 0.0f, 0.0f, 0.0f);
        check(midSun.ldr > 0.5f,
              "daylight scene is clearly lit (ldr > 0.5 with the sun up)");
        check(midSun.ldr > 5.0f * night.ldr,
              "sun responds: daylight scene 5x brighter than the same scene at night");
        check(lowSun.ldr < midSun.ldr && midSun.ldr < highSun.ldr,
              "sun responds monotonically to elevation (0.1 < 0.4 < 0.6)");
    }

    // ---- 5. "água responde" ----
    {
        const ReflectionModelConfig& cfg = reflections->config();
        const float deep = reflections->beerLambert(5.0f, cfg.waterAbsorptionPerMeter);
        const float shallow = reflections->beerLambert(0.5f, cfg.waterAbsorptionPerMeter);
        check(deep < 0.1f && shallow > 0.5f,
              "water responds to depth: 5 m transmits < 0.1, 0.5 m transmits > 0.5");
        check(reflections->waterFresnel(0.2f) > reflections->waterFresnel(1.0f),
              "water responds to angle: grazing Fresnel > normal-incidence Fresnel");
        ReflectionSurface water;
        water.water = true;
        check(reflections->evaluate(water).screenWeight == 1.0f,
              "water stays sharp (screen reflections always win on water)");
    }

    // ---- 6. "reflexos respondem" (roughness drives the screen/probe blend) ----
    {
        const glm::vec3 probe(0.1f, 0.1f, 0.1f);
        const glm::vec3 screen(0.9f, 0.9f, 0.9f);
        ReflectionSurface smooth;
        smooth.roughness = 0.1f;
        ReflectionSurface rough;
        rough.roughness = 0.9f;
        const ReflectionResult rs = reflections->evaluate(smooth);
        const ReflectionResult rr = reflections->evaluate(rough);
        check(rs.screenWeight > 0.7f && rr.probeWeight > 0.9f,
              "blend responds to roughness: smooth -> screen, rough -> probe");
        const float bSmooth = reflections->blend(smooth, probe, screen).x;
        const float bRough = reflections->blend(rough, probe, screen).x;
        check(bSmooth > bRough,
              "reflections respond dynamically: smooth surface reflects the sharp "
              "screen source, rough surface the low-frequency probe");
    }

    // ---- 7. determinism: the composed pipeline is bit-exact ----
    {
        const auto a = evaluateScene(*atmo, *shading, *tone, 0.4, 0.0f, 0.0f, 2.0f);
        const auto b = evaluateScene(*atmo, *shading, *tone, 0.4, 0.0f, 0.0f, 2.0f);
        check(a.ldr == b.ldr && a.linear == b.linear && a.litTotal == b.litTotal,
              "identical scenes reproduce bit-exact pipeline output");
    }

    if (g_failures == 0) {
        std::printf("[rendering-integration] ALL PASSED\n");
        return 0;
    }
    std::printf("[rendering-integration] %d FAILURE(S)\n", g_failures);
    return 1;
}

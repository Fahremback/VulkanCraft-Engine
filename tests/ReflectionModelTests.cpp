// ReflectionModelTests.cpp — Agente 1 (task_plan A.13): headless gate for the
// PUBLIC reflection reflectance contract (IReflectionModel). Proves the
// deterministic pure core: Fresnel (Schlick, Frostbite roughness correction),
// GGX roughness -> cone spread, two-layer clear coat, water (IOR Fresnel +
// Beer-Lambert) and the screen/probe blend policy — no GPU required.

#include "engine/rendering/IReflectionModel.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::IReflectionModel;
using Engine::Rendering::ReflectionModelConfig;
using Engine::Rendering::ReflectionSurface;
using Engine::Rendering::ReflectionResult;
using Engine::Rendering::create_reflection_model;
using Engine::Rendering::create_reflection_model_json;

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto m = create_reflection_model(error);
        check(m != nullptr, "default reflection model created");
        check(error.empty(), "default config diagnostic empty");

        ReflectionModelConfig bad = m->config();
        bad.screenRoughnessLimit = 0.0f;
        check(!m->configure(bad, error) && !error.empty(),
              "screenRoughnessLimit 0 refused");
        bad = m->config();
        bad.screenRoughnessLimit = 1.5f;
        check(!m->configure(bad, error) && !error.empty(),
              "screenRoughnessLimit 1.5 refused");
        bad = m->config();
        bad.waterIndexOfRefraction = 1.0f;
        check(!m->configure(bad, error) && !error.empty(),
              "waterIOR 1.0 refused");
        bad = m->config();
        bad.defaultDielectricF0 = 1.0f;
        check(!m->configure(bad, error) && !error.empty(),
              "defaultDielectricF0 1.0 refused");
        bad = m->config();
        bad.waterAbsorptionPerMeter = -1.0f;
        check(!m->configure(bad, error) && !error.empty(),
              "negative absorption refused");
        bad = m->config();
        bad.waterAbsorptionPerMeter =
            std::numeric_limits<float>::quiet_NaN();
        check(!m->configure(bad, error) && !error.empty(), "NaN refused");
        check(m->config().screenRoughnessLimit == 0.45f,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_reflection_model(error);
        ReflectionModelConfig c = a->config();
        c.screenRoughnessLimit = 0.6f;
        c.waterIndexOfRefraction = 1.5f;
        c.defaultDielectricF0 = 0.02f;
        c.waterAbsorptionPerMeter = 0.25f;
        check(a->configure(c, error), "custom config applied");

        auto b = create_reflection_model_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().screenRoughnessLimit == 0.6f &&
                  b->config().waterIndexOfRefraction == 1.5f &&
                  b->config().defaultDielectricF0 == 0.02f &&
                  b->config().waterAbsorptionPerMeter == 0.25f,
              "json round-trip bit-exact");

        check(create_reflection_model_json(
                  "{ \"version\": 1, \"bogus\": 1 }", error) == nullptr,
              "unknown key refused");
        check(create_reflection_model_json(
                  "{ \"version\": 2 }", error) == nullptr,
              "unsupported version refused");
        check(create_reflection_model_json(
                  "{ \"version\": 1, \"waterIndexOfRefraction\": 1.0 }",
                  error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_reflection_model_json(
                  "{ \"screenRoughnessLimit\": 0.3 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. fresnel: normal incidence = F0, grazing -> max(1-r, F0) ----
    {
        std::string error;
        auto m = create_reflection_model(error);
        check(near(m->fresnel(1.0f, 0.04f, 0.0f), 0.04f),
              "fresnel at normal incidence = F0 (smooth)");
        check(near(m->fresnel(1.0f, 0.04f, 0.5f), 0.04f),
              "fresnel at normal incidence = F0 (rough, Frostbite form)");
        check(near(m->fresnel(0.0f, 0.04f, 0.0f), 1.0f),
              "fresnel grazing = 1 (smooth)");
        // roughness 0.5: asymptote = max(1 - 0.5, 0.04) = 0.5
        check(near(m->fresnel(0.0f, 0.04f, 0.5f), 0.5f),
              "fresnel grazing saturates at max(1-r, F0) = 0.5");
        // plain Schlick at cos = 0.5: 0.04 + 0.96 * 0.5^5 = 0.07
        check(near(m->fresnel(0.5f, 0.04f, 0.0f), 0.04f + 0.96f * 0.03125f),
              "fresnel mid-angle matches Schlick");
        check(m->fresnel(1.0f, 0.04f, 0.9f) <= m->fresnel(0.5f, 0.04f, 0.9f) &&
                  m->fresnel(0.5f, 0.04f, 0.9f) <= m->fresnel(0.0f, 0.04f, 0.9f),
              "fresnel monotonic in (1 - cos)");
        check(m->fresnel(0.5f, 0.02f, 0.2f) <= m->fresnel(0.5f, 0.9f, 0.2f),
              "fresnel monotonic in F0");
        check(near(m->fresnel(-0.5f, 0.04f, 0.0f), m->fresnel(0.0f, 0.04f, 0.0f)),
              "fresnel clamps negative cos to grazing");
    }

    // ---- 4. roughnessSpread: cone half-angle ----
    {
        std::string error;
        auto m = create_reflection_model(error);
        check(near(m->roughnessSpread(0.0f), 0.0f, 1e-6f),
              "spread 0 at roughness 0");
        check(near(m->roughnessSpread(1.0f), 3.14159265f / 4.0f),
              "spread pi/4 at roughness 1");
        check(near(m->roughnessSpread(0.5f), std::atan(0.5f)),
              "spread = atan(r)");
        check(m->roughnessSpread(0.2f) < m->roughnessSpread(0.8f),
              "spread monotonic in roughness");
    }

    // ---- 5. clear coat: two-layer Fresnel ----
    {
        std::string error;
        auto m = create_reflection_model(error);
        const float baseF = m->clearCoatFresnel(1.0f, 0.04f, 0.04f, 0.0f);
        check(near(baseF, 0.04f + 0.9216f * 0.04f),
              "coat over dielectric: reflectance adds (0.0769)");
        check(baseF > 0.04f, "coat over dielectric raises reflectance");
        const float metalF = m->clearCoatFresnel(1.0f, 0.9f, 0.04f, 0.0f);
        check(near(metalF, 0.04f + 0.9216f * 0.9f),
              "coat over metal: interface dominates (0.8694)");
        check(metalF < 0.9f, "coat over metal lowers reflectance");
        check(near(m->clearCoatFresnel(0.0f, 0.9f, 0.04f, 0.0f), 1.0f),
              "coat grazing = 1");
        // coatRoughness feeds the roughness-corrected base: at cos = 0.5 the
        // base asymptote is max(1 - 0.5, 0.04) = 0.5, so the rough base
        // reflects LESS than the smooth base at mid angles (0.1170 < 0.1305).
        const float roughBase = m->clearCoatFresnel(0.5f, 0.04f, 0.04f, 0.5f);
        const float smoothBase = m->clearCoatFresnel(0.5f, 0.04f, 0.04f, 0.0f);
        // F_coat = 0.07 (plain Schlick); F_base(rough) = 0.04 + 0.46*0.5^5
        check(near(roughBase, 0.07f + 0.8649f * (0.04f + 0.46f * 0.03125f)),
              "coat base uses roughness-corrected Fresnel");
        check(roughBase < smoothBase,
              "rough coat base reflects less than smooth at mid angle");
        // at grazing the COAT saturates: everything reflects off the outer
        // layer and the base term vanishes.
        check(near(m->clearCoatFresnel(0.0f, 0.04f, 0.04f, 0.5f), 1.0f),
              "coat grazing = 1 regardless of base");
    }

    // ---- 6. water: IOR Fresnel + Beer-Lambert ----
    {
        std::string error;
        auto m = create_reflection_model(error);
        const float n = 1.333f;
        const float f0 = ((n - 1.0f) / (n + 1.0f)) * ((n - 1.0f) / (n + 1.0f));
        check(near(m->waterFresnel(1.0f), f0),
              "water F0 = ((n-1)/(n+1))^2 ~ 0.0204");
        check(near(m->waterFresnel(0.0f), 1.0f), "water grazing = 1");
        check(m->waterFresnel(0.3f) > m->waterFresnel(0.7f),
              "water fresnel rises toward grazing");

        check(near(m->beerLambert(0.0f, 0.6f), 1.0f),
              "beer-lambert thickness 0 = 1");
        check(near(m->beerLambert(2.0f, 0.0f), 1.0f),
              "beer-lambert absorption 0 = 1");
        check(near(m->beerLambert(2.0f, 0.6f), std::exp(-1.2f)),
              "beer-lambert exp(-a*t)");
        check(m->beerLambert(1.0f, 0.6f) < m->beerLambert(1.0f, 0.2f),
              "beer-lambert monotonic in absorption");
    }

    // ---- 7. evaluate: blend policy + water transmission ----
    {
        std::string error;
        auto m = create_reflection_model(error);

        ReflectionSurface s;
        s.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        s.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);  // cos = 1
        s.roughness = 0.1f;
        s.f0 = 0.04f;
        ReflectionResult r = m->evaluate(s);
        check(near(r.screenWeight, 1.0f - 0.1f / 0.45f),
              "smooth surface: screenWeight 0.778");
        check(near(r.probeWeight, 0.1f / 0.45f),
              "smooth surface: probeWeight 0.222");
        check(near(r.fresnel, 0.04f), "smooth dielectric fresnel = F0 at cos 1");
        check(near(r.spreadAngleRad, std::atan(0.1f)), "spread from roughness");
        check(near(r.transmission, 0.0f), "dielectric transmission = 0");

        ReflectionSurface rough;
        rough.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        rough.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        rough.roughness = 0.9f;
        rough.f0 = 0.04f;
        ReflectionResult rr = m->evaluate(rough);
        check(near(rr.screenWeight, 0.0f) && near(rr.probeWeight, 1.0f),
              "rough surface (r >= limit): probe-only");

        ReflectionSurface water;
        water.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        water.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        water.water = true;
        water.thickness = 1.0f;
        ReflectionResult wr = m->evaluate(water);
        const float wf0 = ((1.333f - 1.0f) / (1.333f + 1.0f)) *
                          ((1.333f - 1.0f) / (1.333f + 1.0f));
        check(near(wr.fresnel, wf0), "water fresnel at cos 1");
        check(near(wr.screenWeight, 1.0f) && near(wr.probeWeight, 0.0f),
              "water is always screen-sharp");
        check(near(wr.transmission, (1.0f - wf0) * std::exp(-0.6f)),
              "water transmission = (1-F) * beer-lambert");

        ReflectionSurface waterGrazing = water;
        waterGrazing.viewDir = glm::vec3(1.0f, 0.0f, 0.0f);  // cos = 0
        ReflectionResult wg = m->evaluate(waterGrazing);
        check(near(wg.fresnel, 1.0f) && near(wg.transmission, 0.0f),
              "water grazing: full reflection, zero transmission");

        // clear coat surface path
        ReflectionSurface coat = s;
        coat.clearCoat = true;
        ReflectionResult cr = m->evaluate(coat);
        check(near(cr.fresnel, 0.04f + 0.9216f * 0.04f),
              "clear coat surface fresnel combines layers");

        // f0 = 0 falls back to the config dielectric default
        ReflectionSurface nodie = s;
        nodie.f0 = 0.0f;
        ReflectionResult nd = m->evaluate(nodie);
        check(near(nd.fresnel, 0.04f), "f0 0 uses default dielectric F0");
    }

    // ---- 8. blend: mix(probe, screen, screenWeight) * fresnel ----
    {
        std::string error;
        auto m = create_reflection_model(error);

        ReflectionSurface s;
        s.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        s.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        s.roughness = 0.1f;
        s.f0 = 0.04f;
        const float sw = 1.0f - 0.1f / 0.45f;  // 0.7778
        const glm::vec3 probe(0.2f, 0.2f, 0.2f);
        const glm::vec3 screen(1.0f, 1.0f, 1.0f);
        const glm::vec3 out = m->blend(s, probe, screen);
        const float expected = (0.2f + sw * 0.8f) * 0.04f;
        check(near(out.x, expected) && near(out.y, expected) &&
                  near(out.z, expected),
              "blend = mix(probe, screen, screenWeight) * fresnel");

        // rough: probe-only source
        ReflectionSurface rough;
        rough.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        rough.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        rough.roughness = 0.9f;
        rough.f0 = 0.04f;
        const glm::vec3 outR = m->blend(rough, probe, screen);
        check(near(outR.x, 0.2f * 0.04f), "rough blend uses probe source");

        // water: screen-sharp times water fresnel
        ReflectionSurface water;
        water.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        water.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        water.water = true;
        const glm::vec3 outW = m->blend(water, probe, screen);
        const float wf0 = ((1.333f - 1.0f) / (1.333f + 1.0f)) *
                          ((1.333f - 1.0f) / (1.333f + 1.0f));
        check(near(outW.x, 1.0f * wf0), "water blend = screen * waterFresnel");
    }

    // ---- 9. determinism (bit-exact) ----
    {
        std::string error;
        auto m = create_reflection_model(error);
        ReflectionSurface s;
        s.normal = glm::vec3(0.3f, 0.9f, 0.1f);
        s.normal = glm::normalize(s.normal);
        s.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        s.roughness = 0.42f;
        s.metallic = 0.5f;
        s.f0 = 0.6f;
        s.clearCoat = true;
        s.coatF0 = 0.05f;
        s.water = false;
        s.thickness = 2.0f;

        const ReflectionResult a = m->evaluate(s);
        const ReflectionResult b = m->evaluate(s);
        check(std::memcmp(&a, &b, sizeof(ReflectionResult)) == 0,
              "evaluate reproduces bit-exact results");
        const glm::vec3 c1 = m->blend(s, glm::vec3(0.1f, 0.2f, 0.3f),
                                      glm::vec3(0.9f, 0.8f, 0.7f));
        const glm::vec3 c2 = m->blend(s, glm::vec3(0.1f, 0.2f, 0.3f),
                                      glm::vec3(0.9f, 0.8f, 0.7f));
        check(std::memcmp(&c1, &c2, sizeof(glm::vec3)) == 0,
              "blend reproduces bit-exact results");
    }

    if (g_failures == 0) {
        std::printf("[reflection-model] ALL PASSED\n");
        return 0;
    }
    std::printf("[reflection-model] %d FAILURE(S)\n", g_failures);
    return 1;
}

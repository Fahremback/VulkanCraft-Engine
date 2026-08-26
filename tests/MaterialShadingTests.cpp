// MaterialShadingTests.cpp — Agente 1 (task_plan A.14): headless gate for the
// PUBLIC material shading contract (IMaterialShading). Proves the
// deterministic pure core: two-sided foliage normal flip, wrapped diffuse,
// thickness-based subsurface transmission, interior ambient decay and the
// shadow-factor integration — no GPU required.

#include "engine/rendering/IMaterialShading.hpp"

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

using Engine::Rendering::IMaterialShading;
using Engine::Rendering::MaterialShadingConfig;
using Engine::Rendering::ShadingSurface;
using Engine::Rendering::ShadingResult;
using Engine::Rendering::create_material_shading;
using Engine::Rendering::create_material_shading_json;

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto m = create_material_shading(error);
        check(m != nullptr, "default material shading created");
        check(error.empty(), "default config diagnostic empty");

        MaterialShadingConfig bad = m->config();
        bad.subsurfaceScatter = 1.5f;
        check(!m->configure(bad, error) && !error.empty(),
              "subsurfaceScatter 1.5 refused");
        bad = m->config();
        bad.subsurfaceTransmissionMax = -0.1f;
        check(!m->configure(bad, error) && !error.empty(),
              "subsurfaceTransmissionMax negative refused");
        bad = m->config();
        bad.interiorFalloffPerMeter = -1.0f;
        check(!m->configure(bad, error) && !error.empty(),
              "interiorFalloffPerMeter negative refused");
        bad = m->config();
        bad.interiorAmbientFloor = 1.0f;
        check(!m->configure(bad, error) && !error.empty(),
              "interiorAmbientFloor 1.0 refused");
        bad = m->config();
        bad.subsurfaceScatter = std::numeric_limits<float>::quiet_NaN();
        check(!m->configure(bad, error) && !error.empty(), "NaN refused");
        check(m->config().subsurfaceScatter == 0.5f,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_material_shading(error);
        MaterialShadingConfig c = a->config();
        c.subsurfaceScatter = 0.2f;
        c.subsurfaceTransmissionMax = 0.9f;
        c.interiorFalloffPerMeter = 0.5f;
        c.interiorAmbientFloor = 0.05f;
        check(a->configure(c, error), "custom config applied");

        auto b = create_material_shading_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().subsurfaceScatter == 0.2f &&
                  b->config().subsurfaceTransmissionMax == 0.9f &&
                  b->config().interiorFalloffPerMeter == 0.5f &&
                  b->config().interiorAmbientFloor == 0.05f,
              "json round-trip bit-exact");

        check(create_material_shading_json(
                  "{ \"version\": 1, \"bogus\": 1 }", error) == nullptr,
              "unknown key refused");
        check(create_material_shading_json(
                  "{ \"version\": 2 }", error) == nullptr,
              "unsupported version refused");
        check(create_material_shading_json(
                  "{ \"version\": 1, \"interiorAmbientFloor\": 1.0 }",
                  error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_material_shading_json(
                  "{ \"subsurfaceScatter\": 0.3 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. two-sided foliage: backfaces flip the shading normal ----
    {
        std::string error;
        auto m = create_material_shading(error);

        ShadingSurface front;
        front.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        front.twoSided = true;
        front.backface = false;
        check(m->shadingNormal(front) == front.normal,
              "frontface keeps the normal");

        ShadingSurface back = front;
        back.backface = true;
        check(m->shadingNormal(back) == -front.normal,
              "backface of two-sided foliage flips the normal");

        ShadingSurface oneSidedBack = back;
        oneSidedBack.twoSided = false;
        check(m->shadingNormal(oneSidedBack) == front.normal,
              "one-sided backface keeps the normal (turns dark)");

        // A backface lit from behind: flipped normal catches the light
        // instead of the surface going black.
        ShadingSurface leaf;
        leaf.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        leaf.twoSided = true;
        leaf.backface = true;
        const glm::vec3 lightFromBehind(0.0f, 0.0f, -1.0f);
        const float lit = m->wrapLight(m->shadingNormal(leaf), lightFromBehind);
        check(near(lit, 1.0f), "flipped backface is fully lit from behind");

        const ShadingResult r = m->evaluate(leaf, lightFromBehind, 0.0f);
        check(near(r.wrapLight, 1.0f) && r.shadingNormal.z < 0.0f,
              "evaluate uses the flipped normal");
    }

    // ---- 4. wrap light: scatter 0 = N.L, scatter 1 = half-Lambert ----
    {
        std::string error;
        auto m = create_material_shading(error);
        const glm::vec3 n(0.0f, 1.0f, 0.0f);

        MaterialShadingConfig c = m->config();
        c.subsurfaceScatter = 0.0f;
        check(m->configure(c, error), "scatter 0 applied");
        check(near(m->wrapLight(n, glm::vec3(0.0f, 1.0f, 0.0f)), 1.0f),
              "scatter 0: facing light = 1");
        check(near(m->wrapLight(n, glm::vec3(1.0f, 0.0f, 0.0f)), 0.0f),
              "scatter 0: perpendicular = 0");
        check(near(m->wrapLight(n, glm::vec3(0.0f, -1.0f, 0.0f)), 0.0f),
              "scatter 0: behind = 0");

        c.subsurfaceScatter = 1.0f;
        check(m->configure(c, error), "scatter 1 applied");
        check(near(m->wrapLight(n, glm::vec3(0.0f, -1.0f, 0.0f)), 0.0f),
              "scatter 1: behind = 0 (half-Lambert clamps)");
        check(near(m->wrapLight(n, glm::vec3(1.0f, 0.0f, 0.0f)), 0.5f),
              "scatter 1: perpendicular = half-Lambert 0.5");

        // mid scatter: (0 + 0.5) / 1.5 = 1/3 for a perpendicular light
        c.subsurfaceScatter = 0.5f;
        check(m->configure(c, error), "scatter 0.5 applied");
        check(near(m->wrapLight(n, glm::vec3(1.0f, 0.0f, 0.0f)), 1.0f / 3.0f),
              "scatter 0.5: perpendicular = 1/3");
        check(m->wrapLight(n, glm::vec3(1.0f, 0.0f, 0.0f)) >
                  m->wrapLight(n, glm::vec3(0.0f, -1.0f, 0.0f)),
              "wrap light monotonic in the dot product");
    }

    // ---- 5. subsurface: thickness-based transmission ----
    {
        std::string error;
        auto m = create_material_shading(error);
        check(near(m->subsurfaceTransmission(0.0f), 0.5f),
              "zero thickness = max transmission");
        check(near(m->subsurfaceTransmission(1.0f), 0.5f * std::exp(-1.0f)),
              "1m thickness = max * exp(-1)");
        check(m->subsurfaceTransmission(0.2f) > m->subsurfaceTransmission(2.0f),
              "transmission decreases with thickness");
        check(near(m->subsurfaceTransmission(-1.0f), m->subsurfaceTransmission(0.0f)),
              "negative thickness clamped to 0");

        MaterialShadingConfig c = m->config();
        c.subsurfaceTransmissionMax = 0.0f;
        check(m->configure(c, error), "opaque applied");
        check(near(m->subsurfaceTransmission(0.0f), 0.0f),
              "opaque material transmits nothing");
    }

    // ---- 6. interior ambient: really dark interiors ----
    {
        std::string error;
        auto m = create_material_shading(error);
        check(near(m->interiorAmbient(0.0f), 1.0f),
              "open space keeps full ambient");
        check(near(m->interiorAmbient(5.0f),
                   0.02f + 0.98f * std::exp(-0.8f * 5.0f)),
              "deep interior decays to floor + (1-floor)*exp(-falloff*depth)");
        check(m->interiorAmbient(0.5f) > m->interiorAmbient(10.0f),
              "ambient decreases with depth into the interior");
        check(m->interiorAmbient(100.0f) >= 0.02f,
              "ambient never drops below the floor (never pitch-black)");
    }

    // ---- 7. shadows: direct diffuse = wrapLight * (1 - shadow) ----
    {
        std::string error;
        auto m = create_material_shading(error);
        ShadingSurface s;
        s.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 light(0.0f, 1.0f, 0.0f);

        const ShadingResult lit = m->evaluate(s, light, 0.0f);
        check(near(lit.directDiffuse, lit.wrapLight),
              "no shadow: direct diffuse = wrap light");
        const ShadingResult shadowed = m->evaluate(s, light, 1.0f);
        check(near(shadowed.directDiffuse, 0.0f),
              "full shadow: direct diffuse = 0");
        const ShadingResult half = m->evaluate(s, light, 0.5f);
        check(near(half.directDiffuse, 0.5f * lit.wrapLight),
              "half shadow: direct diffuse halved");

        // shadow factor is clamped, never amplified
        const ShadingResult over = m->evaluate(s, light, 2.0f);
        check(near(over.directDiffuse, 0.0f), "shadow factor clamped to [0,1]");
    }

    // ---- 8. interiors really dark: shadow + depth compound ----
    {
        std::string error;
        auto m = create_material_shading(error);
        ShadingSurface deep;
        deep.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        deep.interiorDepth = 8.0f;
        const glm::vec3 light(0.0f, 1.0f, 0.0f);
        const ShadingResult r = m->evaluate(deep, light, 1.0f);
        // directDiffuse = 0 (full shadow); ambient ~ floor; total stays low
        check(r.directDiffuse == 0.0f, "deep interior: no direct light");
        check(near(r.interiorAmbient, 0.02f + 0.98f * std::exp(-6.4f)),
              "deep interior: ambient near the floor");
        check(r.total < 0.05f, "deep shadowed interior stays really dark");
        // same surface with a light source nearby (no shadow) is far brighter
        const ShadingResult lit = m->evaluate(deep, light, 0.0f);
        check(lit.total > r.total + 0.5f,
              "unshadowed interior is far brighter than shadowed");
    }

    // ---- 9. determinism (bit-exact) ----
    {
        std::string error;
        auto m = create_material_shading(error);
        ShadingSurface s;
        s.normal = glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
        s.viewDir = glm::vec3(0.0f, 1.0f, 0.0f);
        s.twoSided = true;
        s.backface = true;
        s.thickness = 0.3f;
        s.interiorDepth = 2.0f;
        const glm::vec3 light(0.5f, 0.8f, 0.2f);

        const ShadingResult a = m->evaluate(s, light, 0.3f);
        const ShadingResult b = m->evaluate(s, light, 0.3f);
        check(std::memcmp(&a, &b, sizeof(ShadingResult)) == 0,
              "evaluate reproduces bit-exact results");
    }

    if (g_failures == 0) {
        std::printf("[material-shading] ALL PASSED\n");
        return 0;
    }
    std::printf("[material-shading] %d FAILURE(S)\n", g_failures);
    return 1;
}

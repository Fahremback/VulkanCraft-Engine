// VolumeCloudsTests.cpp — Agente 1 (task_plan A.15): headless gate for the
// PUBLIC volumetric cloud contract (IVolumeClouds). Proves the deterministic
// pure core: seeded FBM density field, Henyey-Greenstein phase and the
// single-scattering raymarch with Beer-Lambert transmittance — no GPU required.

#include "engine/rendering/IVolumeClouds.hpp"

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

using Engine::Rendering::IVolumeClouds;
using Engine::Rendering::VolumeCloudsConfig;
using Engine::Rendering::create_volume_clouds;
using Engine::Rendering::create_volume_clouds_json;

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto c = create_volume_clouds(error);
        check(c != nullptr, "default volume clouds created");
        check(error.empty(), "default config diagnostic empty");

        VolumeCloudsConfig bad = c->config();
        bad.coverage = 1.5f;
        check(!c->configure(bad, error) && !error.empty(), "coverage 1.5 refused");
        bad = c->config();
        bad.densityScale = 0.0f;
        check(!c->configure(bad, error) && !error.empty(), "densityScale 0 refused");
        bad = c->config();
        bad.lightAbsorption = -1.0f;
        check(!c->configure(bad, error) && !error.empty(),
              "lightAbsorption negative refused");
        bad = c->config();
        bad.phaseG = 1.5f;
        check(!c->configure(bad, error) && !error.empty(), "phaseG 1.5 refused");
        bad = c->config();
        bad.seed = 0;
        check(!c->configure(bad, error) && !error.empty(), "seed 0 refused");
        bad = c->config();
        bad.ambientScale = std::numeric_limits<float>::quiet_NaN();
        check(!c->configure(bad, error) && !error.empty(), "NaN refused");
        check(c->config().coverage == 0.5f,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_volume_clouds(error);
        VolumeCloudsConfig c = a->config();
        c.coverage = 0.8f;
        c.densityScale = 2.0f;
        c.detailStrength = 0.5f;
        c.lightAbsorption = 0.7f;
        c.phaseG = -0.3f;
        c.ambientScale = 0.4f;
        c.seed = 7;
        check(a->configure(c, error), "custom config applied");

        auto b = create_volume_clouds_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().coverage == 0.8f && b->config().densityScale == 2.0f &&
                  b->config().detailStrength == 0.5f &&
                  b->config().lightAbsorption == 0.7f &&
                  b->config().phaseG == -0.3f && b->config().ambientScale == 0.4f &&
                  b->config().seed == 7,
              "json round-trip bit-exact");

        check(create_volume_clouds_json("{ \"version\": 1, \"bogus\": 1 }",
                                        error) == nullptr,
              "unknown key refused");
        check(create_volume_clouds_json("{ \"version\": 2 }", error) == nullptr,
              "unsupported version refused");
        check(create_volume_clouds_json(
                  "{ \"version\": 1, \"coverage\": 3.0 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_volume_clouds_json("{ \"coverage\": 0.3 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. density field: deterministic, non-negative, coverage-gated ----
    {
        std::string error;
        auto c = create_volume_clouds(error);

        // clear sky: coverage 0 -> density 0 everywhere
        VolumeCloudsConfig clear = c->config();
        clear.coverage = 0.0f;
        check(c->configure(clear, error), "clear config applied");
        bool any = false;
        for (float y = 0.1f; y <= 0.9f; y += 0.2f) {
            for (float x = 0.0f; x <= 3.0f; x += 1.0f) {
                for (float z = 0.0f; z <= 3.0f; z += 1.0f) {
                    if (c->density(x, y, z) > 0.0f) any = true;
                }
            }
        }
        check(!any, "coverage 0 yields zero density everywhere (clear sky)");

        // full coverage: density peaks in the middle band, non-negative
        VolumeCloudsConfig full = c->config();
        full.coverage = 1.0f;
        check(c->configure(full, error), "full config applied");
        float mid = c->density(1.5f, 0.5f, 1.5f);
        float edge = c->density(1.5f, 0.0f, 1.5f);
        float high = c->density(1.5f, 1.0f, 1.5f);
        check(mid > 0.0f, "full coverage: band center has density");
        check(mid > edge && mid > high,
              "height profile peaks in the middle band");
        check(c->density(-3.0f, 0.5f, -3.0f) >= 0.0f &&
                  c->density(1.5f, 0.5f, 1.5f) >= 0.0f,
              "density never negative");

        // deterministic: same point, same density (bit-exact)
        const float d1 = c->density(0.37f, 0.42f, 0.91f);
        const float d2 = c->density(0.37f, 0.42f, 0.91f);
        check(std::memcmp(&d1, &d2, sizeof(float)) == 0,
              "density is deterministic bit-exact");

        // coverage monotonic: more coverage -> more density at a fixed point
        VolumeCloudsConfig c50 = c->config();
        c50.coverage = 0.5f;
        check(c->configure(c50, error), "coverage 0.5 applied");
        float dLow = c->density(0.37f, 0.42f, 0.91f);
        check(c->configure(full, error), "coverage 1 re-applied");
        float dHigh = c->density(0.37f, 0.42f, 0.91f);
        check(dHigh >= dLow, "density monotonic in coverage");
    }

    // ---- 4. Henyey-Greenstein phase ----
    {
        std::string error;
        auto c = create_volume_clouds(error);

        VolumeCloudsConfig iso = c->config();
        iso.phaseG = 0.0f;
        check(c->configure(iso, error), "isotropic applied");
        check(near(c->phase(0.0f), 1.0f / (4.0f * 3.14159265f)),
              "g=0: isotropic 1/(4*pi)");
        check(near(c->phase(1.0f), c->phase(-1.0f)),
              "g=0: no direction bias");

        VolumeCloudsConfig fwd = c->config();
        fwd.phaseG = 0.7f;
        check(c->configure(fwd, error), "forward applied");
        check(c->phase(1.0f) > c->phase(0.0f) &&
                  c->phase(0.0f) > c->phase(-1.0f),
              "g>0: strong forward peak");

        VolumeCloudsConfig bwd = c->config();
        bwd.phaseG = -0.7f;
        check(c->configure(bwd, error), "backward applied");
        check(c->phase(-1.0f) > c->phase(1.0f),
              "g<0: backward peak");

        const float ratio = c->phase(1.0f) / c->phase(0.0f);  // g = -0.7 fwd value
        VolumeCloudsConfig fwd2 = c->config();
        fwd2.phaseG = 0.9f;
        check(c->configure(fwd2, error), "strong forward applied");
        check(c->phase(1.0f) / c->phase(-1.0f) > 100.0f,
              "g=0.9: phase strongly peaked forward");
        check(ratio > 0.0f, "phase always positive");
    }

    // ---- 5. march: Beer-Lambert + single scattering ----
    {
        std::string error;
        auto c = create_volume_clouds(error);
        const glm::vec3 origin(0.0f, 0.5f, 0.0f);
        const glm::vec3 sunColor(1.0f, 1.0f, 1.0f);

        // clear sky: transmittance 1, no inscatter
        VolumeCloudsConfig clear = c->config();
        clear.coverage = 0.0f;
        check(c->configure(clear, error), "clear config applied");
        float T0 = 0.0f;
        glm::vec3 L0;
        check(c->march(origin, glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 4.0f, 32,
                       glm::vec3(0.0f, 1.0f, 0.0f), sunColor, T0, L0),
              "clear march runs");
        check(near(T0, 1.0f) && L0 == glm::vec3(0.0f),
              "clear sky: full transmittance, zero inscatter");

        // dense overcast: transmittance drops along a longer path
        VolumeCloudsConfig full = c->config();
        full.coverage = 1.0f;
        full.densityScale = 4.0f;
        check(c->configure(full, error), "dense config applied");
        float Tshort = 0.0f, Tlong = 0.0f;
        glm::vec3 Ls, Ll;
        check(c->march(origin, glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 1.0f, 32,
                       glm::vec3(0.0f, 1.0f, 0.0f), sunColor, Tshort, Ls),
              "short march runs");
        check(c->march(origin, glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 6.0f, 32,
                       glm::vec3(0.0f, 1.0f, 0.0f), sunColor, Tlong, Ll),
              "long march runs");
        check(Tshort < 1.0f, "dense cloud extinguishes light");
        check(Tlong < Tshort, "longer path through cloud: less transmittance");
        check(Ls.x > 0.0f, "dense cloud scatters light in");

        // inscatter is phase-dependent on the sun direction
        VolumeCloudsConfig fwd = c->config();
        fwd.phaseG = 0.9f;
        check(c->configure(fwd, error), "forward config applied");
        const glm::vec3 dir(0.0f, 0.0f, 1.0f);
        float Tf = 0.0f, Tb = 0.0f;
        glm::vec3 Lf, Lb;
        check(c->march(origin, dir, 0.0f, 4.0f, 32,
                       glm::vec3(0.0f, 0.0f, 1.0f), sunColor, Tf, Lf),
              "sun aligned march runs");
        check(c->march(origin, dir, 0.0f, 4.0f, 32,
                       glm::vec3(0.0f, 0.0f, -1.0f), sunColor, Tb, Lb),
              "sun anti-aligned march runs");
        check(Lf.x > Lb.x,
              "forward phase: looking toward the sun scatters more light");

        // steps converge: more steps never lose light already accumulated
        float T32 = 0.0f, T128 = 0.0f;
        glm::vec3 L32, L128;
        check(c->march(origin, dir, 0.0f, 4.0f, 32, glm::vec3(0.0f, 1.0f, 0.0f),
                       sunColor, T32, L32),
              "32-step march runs");
        check(c->march(origin, dir, 0.0f, 4.0f, 128,
                       glm::vec3(0.0f, 1.0f, 0.0f), sunColor, T128, L128),
              "128-step march runs");
        check(std::fabs(T128 - T32) < 0.05f,
              "march converges with step count");

        // refusals: invalid params leave outputs untouched
        float Tref = 0.123f;
        glm::vec3 Lref(0.456f, 0.456f, 0.456f);
        check(!c->march(origin, dir, 4.0f, 2.0f, 32,
                        glm::vec3(0.0f, 1.0f, 0.0f), sunColor, Tref, Lref) &&
                  near(Tref, 0.123f) && Lref == glm::vec3(0.456f),
              "tMax <= tMin refused, outputs untouched");
        check(!c->march(origin, dir, 0.0f, 4.0f, 0,
                        glm::vec3(0.0f, 1.0f, 0.0f), sunColor, Tref, Lref),
              "steps 0 refused");
        check(!c->march(origin, dir, 0.0f, 4.0f, 200,
                        glm::vec3(0.0f, 1.0f, 0.0f), sunColor, Tref, Lref),
              "steps > 128 refused");
        check(!c->march(origin, dir, 0.0f, 4.0f, 32,
                        glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
                        Tref, Lref),
              "negative sun color refused");
    }

    // ---- 6. determinism (bit-exact) ----
    {
        std::string error;
        auto c = create_volume_clouds(error);
        float T1 = 0.0f, T2 = 0.0f;
        glm::vec3 L1, L2;
        check(c->march(glm::vec3(0.2f, 0.5f, 0.1f), glm::vec3(0.0f, 0.0f, 1.0f),
                       0.0f, 5.0f, 64, glm::vec3(0.5f, 1.0f, 0.3f),
                       glm::vec3(1.0f, 0.9f, 0.8f), T1, L1),
              "determinism march A runs");
        check(c->march(glm::vec3(0.2f, 0.5f, 0.1f), glm::vec3(0.0f, 0.0f, 1.0f),
                       0.0f, 5.0f, 64, glm::vec3(0.5f, 1.0f, 0.3f),
                       glm::vec3(1.0f, 0.9f, 0.8f), T2, L2),
              "determinism march B runs");
        check(std::memcmp(&T1, &T2, sizeof(float)) == 0 &&
                  std::memcmp(&L1, &L2, sizeof(glm::vec3)) == 0,
              "identical marches reproduce bit-exact outputs");
    }

    if (g_failures == 0) {
        std::printf("[volume-clouds] ALL PASSED\n");
        return 0;
    }
    std::printf("[volume-clouds] %d FAILURE(S)\n", g_failures);
    return 1;
}

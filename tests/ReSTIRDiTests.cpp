// ReSTIRDiTests.cpp — Agente 1 (task_plan A.9): headless gate for the PUBLIC
// ReSTIR Direct-Illumination contract (IReSTIRDI). Proves the deterministic
// pure core: weighted-resampling reservoir updates, temporal and spatial
// reuse, the visibility seam and the unbiased resolve — no GPU required.
//
// MATH UNDER TEST: with the target p_hat = radiance * cos(n, dir), the
// reservoir estimator L = W / M is unbiased for the irradiance sum of the
// lights, and variance shrinks with the effective candidate count M (fresh
// candidates + merged reuse). Perfect importance sampling (pdf proportional to
// radiance) makes L independent of M up to float rounding.

#include "engine/rendering/IReSTIRDI.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

bool near(float a, float b, float eps) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::IReSTIRDI;
using Engine::Rendering::RestirConfig;
using Engine::Rendering::RestirDiFrameResult;
using Engine::Rendering::RestirDiLight;
using Engine::Rendering::RestirDiPixelInput;
using Engine::Rendering::RestirLightSample;
using Engine::Rendering::RestirReservoir;
using Engine::Rendering::create_restir_di;
using Engine::Rendering::create_restir_di_json;

// A deterministic 32x32 pixel grid; every pixel faces +Y and all lights sit
// straight up, so cos(n, dir) == 1 and the ground-truth irradiance is the sum
// of the light radiances.
std::vector<RestirDiPixelInput> gridPixels(std::size_t count) {
    std::vector<RestirDiPixelInput> pixels(count);
    for (std::size_t p = 0; p < count; ++p) {
        pixels[p].position = glm::vec3(0.0f, 0.0f, 0.0f);
        pixels[p].normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return pixels;
}

// Grayscale light: luminance == radiance.
RestirDiLight light(std::uint32_t id, float radiance, float power) {
    RestirDiLight l;
    l.position = glm::vec3(0.0f, 100.0f, 0.0f);
    l.radiance = glm::vec3(radiance);
    l.power = power;
    l.id = id;
    return l;
}

double mean(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (float x : v) s += x;
    return s / static_cast<double>(v.size());
}

double sampleVariance(const std::vector<float>& v, double m) {
    if (v.size() < 2) return 0.0;
    double s = 0.0;
    for (float x : v) {
        const double d = static_cast<double>(x) - m;
        s += d * d;
    }
    return s / static_cast<double>(v.size() - 1);
}

double rmse(const std::vector<float>& v, double truth) {
    double s = 0.0;
    for (float x : v) {
        const double d = static_cast<double>(x) - truth;
        s += d * d;
    }
    return std::sqrt(s / static_cast<double>(v.size()));
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. ABI + default config + all-or-nothing refusal ----
    {
        check(sizeof(RestirReservoir) == 48,
              "reservoir is the 48-byte std430 ABI (RadianceCache::ReservoirGpu)");

        std::string error;
        auto di = create_restir_di(error);
        check(di != nullptr, "default restir di created");
        check(error.empty(), "default config diagnostic empty");
        check(di->config().candidateCount == 8 && di->config().temporalReuse &&
                  di->config().spatialReuse && di->config().seed == 1,
              "defaults: 8 candidates, temporal+spatial, seed 1");

        RestirConfig bad = di->config();
        bad.candidateCount = 0;
        check(!di->configure(bad, error) && !error.empty(),
              "candidateCount 0 refused all-or-nothing");
        bad = di->config();
        bad.spatialReuse = true;
        bad.spatialSamples = 0;
        check(!di->configure(bad, error) && !error.empty(),
              "spatialReuse without samples refused");
        bad = di->config();
        bad.seed = 0;
        check(!di->configure(bad, error) && !error.empty(),
              "seed 0 refused");
        check(di->config().candidateCount == 8,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_restir_di(error);
        RestirConfig c = a->config();
        c.candidateCount = 24;
        c.spatialSamples = 6;
        c.temporalReuse = false;
        c.visibilityReuse = false;
        c.seed = 77;
        check(a->configure(c, error), "custom config applied");

        auto b = create_restir_di_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().candidateCount == 24 && b->config().spatialSamples == 6 &&
                  !b->config().temporalReuse && !b->config().visibilityReuse &&
                  b->config().seed == 77,
              "json round-trip bit-exact");

        check(create_restir_di_json("{ \"version\": 2, \"seed\": 1 }", error) == nullptr,
              "unsupported version refused");
        check(create_restir_di_json(
                  "{ \"version\": 1, \"candidateCount\": 0 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_restir_di_json(
                  "{ \"version\": 1, \"bogus\": 1 }", error) == nullptr,
              "unknown json key refused");
        check(create_restir_di_json("{ \"seed\": 1 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. streaming update: single-sample exact + determinism ----
    {
        std::string error;
        auto di = create_restir_di(error);
        std::vector<RestirLightSample> one;
        RestirLightSample s;
        s.direction = glm::vec3(0.0f, 1.0f, 0.0f);
        s.radiance = glm::vec3(2.0f);
        s.pdf = 1.0f;
        s.lightId = 9u;
        one.push_back(s);

        RestirReservoir r = di->update(one, glm::vec3(0.0f, 1.0f, 0.0f), 5u);
        check(near(di->resolve(r), 2.0f, 1e-6f), "single sample resolves to its radiance");
        check(r.m == 1u && r.lightId == 9u, "single sample accepted");

        RestirReservoir r2 = di->update(one, glm::vec3(0.0f, 1.0f, 0.0f), 5u);
        check(std::memcmp(&r, &r2, sizeof(r)) == 0,
              "update is bit-exact deterministic for identical inputs");
    }

    // ---- 4. streaming update: unbiased with uniform sampling ----
    // Lights 1/2/3 with equal power -> pdf 1/3 each; E[W/M] = 1+2+3 = 6.
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.candidateCount = 64;
        c.temporalReuse = false;
        c.spatialReuse = false;
        c.seed = 42;
        check(di->configure(c, error), "unbiased config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 1.0f));
        lights.push_back(light(2u, 3.0f, 1.0f));

        const std::size_t n = 1024;
        auto pixels = gridPixels(n);
        std::vector<RestirReservoir> none(n);
        RestirDiFrameResult out;
        check(di->diFrame(pixels, lights, none, 0u, {}, out, error),
              "unbiased frame runs");
        const double m = mean(out.radiance);
        check(near(static_cast<float>(m), 6.0f, 0.05f),
              "uniform sampling estimate unbiased (mean ~ 6.0)");
        check(out.effectiveM[0] == 64u, "effective M == candidateCount without reuse");
    }

    // ---- 5. perfect importance sampling: estimate independent of M ----
    // power == radiance -> pdf proportional to radiance -> w == total power
    // for every candidate -> W/M == 6 up to float rounding for ANY M.
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.temporalReuse = false;
        c.spatialReuse = false;
        c.seed = 7;
        check(di->configure(c, error), "perfect-is config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 2.0f));
        lights.push_back(light(2u, 3.0f, 3.0f));

        auto pixels = gridPixels(64);
        std::vector<RestirReservoir> none(64);
        for (std::uint32_t mc : {1u, 3u, 7u, 64u}) {
            c.candidateCount = mc;
            check(di->configure(c, error), "candidate count applied");
            RestirDiFrameResult out;
            check(di->diFrame(pixels, lights, none, 0u, {}, out, error),
                  "perfect-is frame runs");
            bool allExact = true;
            for (float v : out.radiance) {
                if (!near(v, 6.0f, 1e-4f)) allExact = false;
            }
            check(allExact, "perfect importance sampling: estimate ~6 for every M");
        }
    }

    // ---- 6. more candidates -> less variance ----
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.temporalReuse = false;
        c.spatialReuse = false;
        c.seed = 1234;
        check(di->configure(c, error), "variance config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 1.0f));
        lights.push_back(light(2u, 3.0f, 1.0f));

        const std::size_t n = 1024;
        auto pixels = gridPixels(n);
        std::vector<RestirReservoir> none(n);

        c.candidateCount = 2;
        check(di->configure(c, error), "M=2 applied");
        RestirDiFrameResult outLow;
        check(di->diFrame(pixels, lights, none, 0u, {}, outLow, error), "M=2 frame runs");
        const double varLow = sampleVariance(outLow.radiance, mean(outLow.radiance));

        c.candidateCount = 64;
        check(di->configure(c, error), "M=64 applied");
        RestirDiFrameResult outHigh;
        check(di->diFrame(pixels, lights, none, 0u, {}, outHigh, error), "M=64 frame runs");
        const double varHigh = sampleVariance(outHigh.radiance, mean(outHigh.radiance));

        check(varLow > 3.0 * varHigh,
              "more candidates -> strictly lower variance (deterministic)");
    }

    // ---- 7. temporal reuse: doubles effective M, lowers error ----
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.candidateCount = 16;
        c.temporalReuse = false;
        c.spatialReuse = false;
        c.seed = 99;
        check(di->configure(c, error), "temporal setup config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 1.0f));
        lights.push_back(light(2u, 3.0f, 1.0f));

        const std::size_t n = 1024;
        auto pixels = gridPixels(n);

        // Frame 1: fresh only -> reservoirs for frame 2.
        RestirDiFrameResult frame1;
        check(di->diFrame(pixels, lights, std::vector<RestirReservoir>(n), 0u,
                          {}, frame1, error),
              "frame 1 runs");

        // Frame 2A: fresh M=16 at frame index 1 (same total as 2B w/o reuse).
        RestirDiFrameResult frame2a;
        check(di->diFrame(pixels, lights, std::vector<RestirReservoir>(n), 1u,
                          {}, frame2a, error),
              "frame 2A (fresh) runs");

        // Frame 2B: temporal reuse of frame 1 -> effective M = 32.
        c.temporalReuse = true;
        check(di->configure(c, error), "temporal reuse enabled");
        RestirDiFrameResult frame2b;
        check(di->diFrame(pixels, lights, frame1.reservoirs, 1u, {}, frame2b,
                          error),
              "frame 2B (temporal) runs");

        const double rmseFresh = rmse(frame2a.radiance, 6.0);
        const double rmseTemporal = rmse(frame2b.radiance, 6.0);
        check(frame2b.effectiveM[0] == 32u,
              "temporal reuse doubles the effective candidate count");
        check(rmseTemporal < 0.85 * rmseFresh,
              "temporal reuse lowers error at the same fresh candidate count");
    }

    // ---- 8. spatial reuse: grows effective M, lowers error ----
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.candidateCount = 4;
        c.temporalReuse = false;
        c.spatialReuse = false;
        c.seed = 555;
        check(di->configure(c, error), "spatial setup config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 1.0f));
        lights.push_back(light(2u, 3.0f, 1.0f));

        // 16x16 grid so the deterministic neighbor offsets stay in bounds.
        const std::size_t n = 256;
        auto pixels = gridPixels(n);
        std::vector<RestirReservoir> none(n);

        RestirDiFrameResult noSpatial;
        check(di->diFrame(pixels, lights, none, 0u, {}, noSpatial, error),
              "no-spatial frame runs");

        c.spatialReuse = true;
        c.spatialSamples = 4;
        check(di->configure(c, error), "spatial reuse enabled");
        RestirDiFrameResult withSpatial;
        check(di->diFrame(pixels, lights, none, 0u, {}, withSpatial, error),
              "spatial frame runs");

        std::uint32_t accepted = 0;
        std::uint32_t maxM = 0;
        for (std::uint32_t m : withSpatial.effectiveM) {
            if (m > 4u) ++accepted;
            maxM = std::max(maxM, m);
        }
        check(accepted > 0, "spatial reuse merges neighbor reservoirs");
        check(maxM > 4u, "effective M grows with spatial reuse");
        check(rmse(withSpatial.radiance, 6.0) < 0.8 * rmse(noSpatial.radiance, 6.0),
              "spatial reuse lowers error");
    }

    // ---- 9. visibility seam: occluded reuse contributes nothing ----
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.candidateCount = 4;
        c.spatialReuse = false;
        c.temporalReuse = true;
        c.visibilityReuse = true;
        c.seed = 31337;
        check(di->configure(c, error), "visibility config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 1.0f));
        lights.push_back(light(2u, 3.0f, 1.0f));

        // 16x16 grid. Every pixel gets a previous reservoir: columns < 8 with a
        // sample pointing straight up (visible), columns >= 8 with a sample
        // below the horizon (blocked by the seam).
        const std::size_t side = 16;
        const std::size_t n = side * side;
        auto pixels = gridPixels(n);
        std::vector<RestirReservoir> prev(n);
        for (std::size_t p = 0; p < n; ++p) {
            prev[p].sampleRadiance = glm::vec3(3.0f);
            prev[p].weightSum = 12.0f;
            prev[p].m = 4u;  // a previous frame with 4 candidates
            prev[p].flags = 1u;
            prev[p].sampleDirection =
                (p % side < 8u) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                : glm::normalize(glm::vec3(1.0f, 0.1f, 0.0f));
        }

        IReSTIRDI::VisibilityFn horizon =
            [](const glm::vec3&, const glm::vec3& dir, float) {
                return dir.y > 0.5f;
            };

        RestirDiFrameResult out;
        check(di->diFrame(pixels, lights, prev, 1u, horizon, out, error),
              "visibility frame runs");
        bool exact = true;
        for (std::size_t p = 0; p < n; ++p) {
            const std::uint32_t expected = (p % side >= 8u) ? 4u : 8u;
            if (out.effectiveM[p] != expected) exact = false;
        }
        check(exact, "occluded temporal reuse rejected; visible reuse accepted");

        // Same frame without the visibility check: every reuse merges.
        c.visibilityReuse = false;
        check(di->configure(c, error), "visibility disabled");
        RestirDiFrameResult noVis;
        check(di->diFrame(pixels, lights, prev, 1u, {}, noVis, error),
              "no-visibility frame runs");
        bool allMerged = true;
        for (std::uint32_t m : noVis.effectiveM) {
            if (m != 8u) allMerged = false;
        }
        check(allMerged, "without the seam every temporal reuse merges");
    }

    // ---- 10. diFrame refusals (all-or-nothing) ----
    {
        std::string error;
        auto di = create_restir_di(error);
        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));

        RestirDiFrameResult out;
        check(!di->diFrame({}, lights, {}, 0u, {}, out, error) && !error.empty(),
              "empty pixels refused");
        check(!di->diFrame(gridPixels(4), {}, {}, 0u, {}, out, error) &&
                  !error.empty(),
              "empty lights refused");

        std::vector<RestirDiLight> badLight = lights;
        badLight[0].power = 0.0f;
        check(!di->diFrame(gridPixels(4), badLight, {}, 0u, {}, out, error) &&
                  !error.empty(),
              "non-positive light power refused");

        RestirConfig c = di->config();
        c.temporalReuse = true;
        check(di->configure(c, error), "temporal config applied");
        check(!di->diFrame(gridPixels(4), lights,
                           std::vector<RestirReservoir>(3), 0u, {}, out, error) &&
                  !error.empty(),
              "previous reservoir count mismatch refused");
    }

    // ---- 11. diFrame determinism (bit-exact) ----
    {
        std::string error;
        auto di = create_restir_di(error);
        RestirConfig c = di->config();
        c.candidateCount = 8;
        c.temporalReuse = true;
        c.spatialReuse = true;
        c.spatialSamples = 4;
        c.seed = 20260826u;
        check(di->configure(c, error), "determinism config applied");

        std::vector<RestirDiLight> lights;
        lights.push_back(light(0u, 1.0f, 1.0f));
        lights.push_back(light(1u, 2.0f, 2.0f));
        lights.push_back(light(2u, 3.0f, 3.0f));

        const std::size_t n = 256;
        auto pixels = gridPixels(n);
        std::vector<RestirReservoir> prev(n);
        for (std::size_t p = 0; p < n; ++p) {
            prev[p].sampleDirection = glm::vec3(0.0f, 1.0f, 0.0f);
            prev[p].sampleRadiance = glm::vec3(2.0f);
            prev[p].weightSum = 2.0f;
            prev[p].m = 4u;
            prev[p].flags = 1u;
        }

        RestirDiFrameResult a, b;
        check(di->diFrame(pixels, lights, prev, 1u, {}, a, error), "run A");
        check(di->diFrame(pixels, lights, prev, 1u, {}, b, error), "run B");
        bool same = a.radiance.size() == b.radiance.size() &&
                    a.reservoirs.size() == b.reservoirs.size() &&
                    std::memcmp(a.radiance.data(), b.radiance.data(),
                                a.radiance.size() * sizeof(float)) == 0 &&
                    std::memcmp(a.reservoirs.data(), b.reservoirs.data(),
                                a.reservoirs.size() * sizeof(RestirReservoir)) == 0;
        check(same, "diFrame bit-exact deterministic for identical inputs");
    }

    if (g_failures == 0) {
        std::printf("[restir-di] ALL PASSED\n");
        return 0;
    }
    std::printf("[restir-di] %d FAILURE(S)\n", g_failures);
    return 1;
}

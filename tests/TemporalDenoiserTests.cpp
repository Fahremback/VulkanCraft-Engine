// TemporalDenoiserTests.cpp — Agente 1 (task_plan A.11): headless gate for the
// PUBLIC NRD-style temporal denoiser contract (ITemporalDenoiser). Proves the
// deterministic pure core: motion reprojection, depth/normal rejection
// (disocclusion), EMA accumulation with a confidence output — no GPU required.

#include "engine/rendering/ITemporalDenoiser.hpp"

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

using Engine::Rendering::DenoiserConfig;
using Engine::Rendering::DenoiserHistory;
using Engine::Rendering::DenoiserSample;
using Engine::Rendering::ITemporalDenoiser;
using Engine::Rendering::create_temporal_denoiser;
using Engine::Rendering::create_temporal_denoiser_json;

// Deterministic per-pixel hash noise in [-1, 1] (no RNG state, pure function).
float pixelNoise(std::uint32_t p, std::uint32_t seed) {
    std::uint32_t x = p * 0x9E3779B9u ^ (seed * 0x85EBCA77u);
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return (static_cast<float>(x % 2001u) - 1000.0f) / 1000.0f;
}

double meanOf(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (float x : v) s += x;
    return s / static_cast<double>(v.size());
}

double varianceOf(const std::vector<float>& v, double m) {
    if (v.size() < 2) return 0.0;
    double s = 0.0;
    for (float x : v) {
        const double d = static_cast<double>(x) - m;
        s += d * d;
    }
    return s / static_cast<double>(v.size() - 1);
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto dn = create_temporal_denoiser(error);
        check(dn != nullptr, "default temporal denoiser created");
        check(error.empty(), "default config diagnostic empty");

        DenoiserConfig bad = dn->config();
        bad.width = 0;
        check(!dn->configure(bad, error) && !error.empty(), "width 0 refused");
        bad = dn->config();
        bad.height = 0;
        check(!dn->configure(bad, error) && !error.empty(), "height 0 refused");
        bad = dn->config();
        bad.historyWeight = 0.0f;
        check(!dn->configure(bad, error) && !error.empty(),
              "historyWeight 0 refused");
        bad = dn->config();
        bad.depthRejectThreshold = 0.0f;
        check(!dn->configure(bad, error) && !error.empty(),
              "depthRejectThreshold 0 refused");
        bad = dn->config();
        bad.normalRejectDegrees = 0.0f;
        check(!dn->configure(bad, error) && !error.empty(),
              "normalRejectDegrees 0 refused");
        bad = dn->config();
        bad.seed = 0;
        check(!dn->configure(bad, error) && !error.empty(), "seed 0 refused");
        check(dn->config().width == 64,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_temporal_denoiser(error);
        DenoiserConfig c = a->config();
        c.width = 16;
        c.height = 8;
        c.historyWeight = 0.25f;
        c.normalRejectDegrees = 45.0f;
        c.useMotion = false;
        c.seed = 7;
        check(a->configure(c, error), "custom config applied");

        auto b = create_temporal_denoiser_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().width == 16 && b->config().height == 8 &&
                  b->config().historyWeight == 0.25f &&
                  !b->config().useMotion && b->config().seed == 7,
              "json round-trip bit-exact");

        check(create_temporal_denoiser_json("{ \"version\": 2, \"seed\": 1 }",
                                            error) == nullptr,
              "unsupported version refused");
        check(create_temporal_denoiser_json(
                  "{ \"version\": 1, \"width\": 0 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_temporal_denoiser_json(
                  "{ \"version\": 1, \"bogus\": 1 }", error) == nullptr,
              "unknown json key refused");
        check(create_temporal_denoiser_json("{ \"seed\": 1 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. first frame: fresh history -> output == sample, confidence 1 ----
    {
        std::string error;
        auto dn = create_temporal_denoiser(error);
        DenoiserConfig c = dn->config();
        c.width = 4;
        c.height = 4;
        check(dn->configure(c, error), "first-frame config applied");

        const std::uint32_t n = 16;
        std::vector<DenoiserSample> samples(n);
        for (std::uint32_t p = 0; p < n; ++p) {
            samples[p].radiance = glm::vec3(3.0f);
        }
        std::vector<DenoiserHistory> histories(n);
        std::vector<float> conf;
        std::vector<glm::vec3> out;
        check(dn->denoise(samples, histories, conf, out, error),
              "first denoise runs");
        check(near(out[0].r, 3.0f, 1e-6f),
              "first frame outputs the raw sample");
        check(conf[0] == 1.0f && histories[0].frames == 1u,
              "first frame confidence is 1");
    }

    // ---- 4. static scene: EMA reduces variance, mean preserved ----
    {
        std::string error;
        auto dn = create_temporal_denoiser(error);
        DenoiserConfig c = dn->config();
        c.width = 64;
        c.height = 64;
        c.historyWeight = 0.1f;
        check(dn->configure(c, error), "noise config applied");

        const std::uint32_t n = 64u * 64u;
        // Temporal noise: every frame draws a NEW noise value per pixel around
        // the true signal 5.0 (the EMA averages the temporal noise away).
        std::vector<DenoiserSample> samples(n);
        for (std::uint32_t p = 0; p < n; ++p) {
            samples[p].depth = 10.0f;
        }
        std::vector<DenoiserHistory> histories(n);
        std::vector<float> conf;
        std::vector<glm::vec3> out;

        // Input variance: one frame of the noise field.
        std::vector<float> inputRad(n);
        for (std::uint32_t p = 0; p < n; ++p) {
            samples[p].radiance = glm::vec3(5.0f + pixelNoise(p, 11u));
            inputRad[p] = samples[p].radiance.r;
        }
        const double inVar = varianceOf(inputRad, meanOf(inputRad));

        for (int f = 0; f < 64; ++f) {
            for (std::uint32_t p = 0; p < n; ++p) {
                samples[p].radiance =
                    glm::vec3(5.0f + pixelNoise(p, 11u + static_cast<std::uint32_t>(f)));
            }
            check(dn->denoise(samples, histories, conf, out, error),
                  "static frame denoised");
        }

        std::vector<float> outRad(n);
        for (std::uint32_t p = 0; p < n; ++p) outRad[p] = out[p].r;
        const double outVar = varianceOf(outRad, meanOf(outRad));
        check(outVar < 0.1 * inVar,
              "EMA reduces variance by ~alpha/(2-alpha) (0.053x)");
        check(near(static_cast<float>(meanOf(outRad)), 5.0f, 0.05f),
              "denoised mean stays at the true signal");
        check(conf[0] == 64.0f, "confidence grows to the history length (64)");
    }

    // ---- 5. determinism (bit-exact) ----
    {
        std::string error;
        auto a = create_temporal_denoiser(error);
        DenoiserConfig c = a->config();
        c.width = 16;
        c.height = 16;
        check(a->configure(c, error), "determinism config A applied");
        auto b = create_temporal_denoiser(error);
        check(b->configure(c, error), "determinism config B applied");

        const std::uint32_t n = 256;
        std::vector<DenoiserSample> samples(n);
        for (std::uint32_t p = 0; p < n; ++p) {
            samples[p].radiance = glm::vec3(2.0f + pixelNoise(p, 3u));
            samples[p].depth = 5.0f + 0.01f * static_cast<float>(p % 7);
            samples[p].motion = glm::vec2(static_cast<float>(p % 3) - 1.0f, 0.0f);
        }
        std::vector<DenoiserHistory> ha(n), hb(n);
        std::vector<float> ca, cb;
        std::vector<glm::vec3> oa, ob;
        for (int f = 0; f < 10; ++f) {
            check(a->denoise(samples, ha, ca, oa, error), "run A");
            check(b->denoise(samples, hb, cb, ob, error), "run B");
        }
        bool same = std::memcmp(ha.data(), hb.data(), n * sizeof(DenoiserHistory)) == 0 &&
                    std::memcmp(oa.data(), ob.data(), n * sizeof(glm::vec3)) == 0 &&
                    std::memcmp(ca.data(), cb.data(), n * sizeof(float)) == 0;
        check(same, "identical inputs reproduce bit-exact histories/outputs");
    }

    // ---- 6. motion reprojection keeps history across a translation ----
    {
        // 1D strip: content moves +2 px/frame; the correct motion vector
        // reprojects to the right history; without motion the depth gradient
        // mismatch rejects and the history restarts.
        const std::uint32_t width = 16;
        const std::uint32_t n = width;
        auto content = [](int x) {
            DenoiserSample s;
            s.radiance = glm::vec3(1.0f + 0.5f * static_cast<float>(x));
            s.depth = 1.0f + static_cast<float>(x);  // strong gradient
            return s;
        };

        std::string error;
        auto withMotion = create_temporal_denoiser(error);
        DenoiserConfig c = withMotion->config();
        c.width = width;
        c.height = 1;
        // 2-pixel shift on a depth gradient of 1m/px: relative change 2/(p-1)
        // stays above 0.1 for every pixel in the strip.
        c.depthRejectThreshold = 0.1f;
        check(withMotion->configure(c, error), "motion config applied");
        auto noMotion = create_temporal_denoiser(error);
        c.useMotion = false;
        check(noMotion->configure(c, error), "no-motion config applied");

        auto run = [&](ITemporalDenoiser& dn, std::vector<float>& confOut) {
            std::vector<DenoiserSample> f1(n);
            for (std::uint32_t p = 0; p < n; ++p) f1[p] = content(static_cast<int>(p));
            std::vector<DenoiserHistory> hist(n);
            std::vector<float> conf;
            std::vector<glm::vec3> out;
            check(dn.denoise(f1, hist, conf, out, error), "frame 1 runs");

            std::vector<DenoiserSample> f2(n);
            for (std::uint32_t p = 0; p < n; ++p) {
                if (p >= 2u) {
                    f2[p] = content(static_cast<int>(p) - 2);
                    f2[p].motion = glm::vec2(2.0f, 0.0f);
                } else {
                    f2[p] = content(static_cast<int>(p));  // newly exposed
                }
            }
            check(dn.denoise(f2, hist, conf, out, error), "frame 2 runs");
            confOut = conf;
        };

        std::vector<float> confM, confN;
        run(*withMotion, confM);
        run(*noMotion, confN);

        bool preserved = true;
        bool restarted = true;
        for (std::uint32_t p = 2; p < n; ++p) {
            if (confM[p] != 2.0f) preserved = false;
            if (confN[p] != 1.0f) restarted = false;
        }
        check(preserved,
              "motion reprojection preserves the history across the translation");
        check(restarted,
              "without motion the depth mismatch rejects and restarts fresh");
    }

    // ---- 7. disocclusion: rejection refreshes in one frame (no ghosting) ----
    {
        const std::uint32_t n = 16;
        std::string error;
        auto withReject = create_temporal_denoiser(error);
        DenoiserConfig c = withReject->config();
        c.width = 4;
        c.height = 4;
        check(withReject->configure(c, error), "reject config applied");
        auto withoutReject = create_temporal_denoiser(error);
        c.useDepthRejection = false;
        c.useNormalRejection = false;
        check(withoutReject->configure(c, error), "no-reject config applied");

        std::vector<DenoiserSample> samples(n);
        for (std::uint32_t p = 0; p < n; ++p) {
            samples[p].radiance = glm::vec3(5.0f);
            samples[p].depth = 1.0f;
        }
        auto runDisocclusion = [&](ITemporalDenoiser& dn, bool rejectOn,
                                   float& outPixel, float& confPixel) {
            std::vector<DenoiserHistory> hist(n);
            std::vector<float> conf;
            std::vector<glm::vec3> out;
            for (int f = 0; f < 50; ++f) {
                check(dn.denoise(samples, hist, conf, out, error), "history frames");
            }
            // Frame 51: pixel 10 disoccludes (radiance + depth jump).
            std::vector<DenoiserSample> s2 = samples;
            s2[10].radiance = glm::vec3(9.0f);
            s2[10].depth = 5.0f;
            check(dn.denoise(s2, hist, conf, out, error), "disocclusion frame");
            outPixel = out[10].r;
            confPixel = conf[10];
        };

        float outR, confR, outN, confN;
        runDisocclusion(*withReject, true, outR, confR);
        runDisocclusion(*withoutReject, false, outN, confN);

        check(confR == 1.0f && near(outR, 9.0f, 1e-5f),
              "disocclusion with rejection: history restarts, no ghosting");
        check(confN == 51.0f && near(outN, 5.4f, 1e-3f),
              "without rejection the history ghosts (blend 5 -> 9)");
    }

    // ---- 8. depth-only and normal-only rejection ----
    {
        const std::uint32_t n = 9;  // 3x3
        std::string error;

        // Depth jump fires the depth-only rejection.
        auto depthOnly = create_temporal_denoiser(error);
        DenoiserConfig c = depthOnly->config();
        c.width = 3;
        c.height = 3;
        c.useNormalRejection = false;
        check(depthOnly->configure(c, error), "depth-only config applied");

        std::vector<DenoiserSample> s1(n), s2(n);
        for (std::uint32_t p = 0; p < n; ++p) {
            s1[p].radiance = glm::vec3(2.0f);
            s1[p].depth = 1.0f;
        }
        std::vector<DenoiserHistory> hist(n);
        std::vector<float> conf;
        std::vector<glm::vec3> out;
        check(depthOnly->denoise(s1, hist, conf, out, error), "depth frame 1");
        s2 = s1;
        s2[4].depth = 4.0f;  // rel change 3.0 > 0.2
        check(depthOnly->denoise(s2, hist, conf, out, error), "depth frame 2");
        check(conf[4] == 1.0f, "depth jump rejects the history");

        // Normal rotation fires the normal-only rejection.
        auto normalOnly = create_temporal_denoiser(error);
        c = normalOnly->config();
        c.width = 3;
        c.height = 3;
        c.useDepthRejection = false;
        check(normalOnly->configure(c, error), "normal-only config applied");

        std::vector<DenoiserHistory> hist2(n);
        check(normalOnly->denoise(s1, hist2, conf, out, error),
              "normal frame 1");
        std::vector<DenoiserSample> s3 = s1;
        s3[4].normal = glm::vec3(1.0f, 0.0f, 0.0f);  // 90 deg from +Y
        check(normalOnly->denoise(s3, hist2, conf, out, error),
              "normal frame 2");
        check(conf[4] == 1.0f, "90-degree normal rotation rejects the history");
    }

    // ---- 9. refusals + reset_histories ----
    {
        std::string error;
        auto dn = create_temporal_denoiser(error);
        DenoiserConfig c = dn->config();
        c.width = 4;
        c.height = 4;
        check(dn->configure(c, error), "refusal config applied");

        const std::uint32_t n = 16;
        std::vector<DenoiserSample> samples(n);
        std::vector<DenoiserHistory> hist(n);
        std::vector<float> conf;
        std::vector<glm::vec3> out;
        check(!dn->denoise(std::vector<DenoiserSample>(10), hist, conf, out, error) &&
                  !error.empty(),
              "samples/histories size mismatch refused");
        check(dn->denoise(samples, hist, conf, out, error),
              "valid denoise for reset test");
        check(conf[0] == 1.0f, "history valid before reset");
        dn->reset_histories(hist);
        check(dn->denoise(samples, hist, conf, out, error), "denoise after reset");
        check(conf[0] == 1.0f && hist[0].frames == 1u,
              "reset_histories restarts every pixel fresh");
    }

    if (g_failures == 0) {
        std::printf("[temporal-denoiser] ALL PASSED\n");
        return 0;
    }
    std::printf("[temporal-denoiser] %d FAILURE(S)\n", g_failures);
    return 1;
}

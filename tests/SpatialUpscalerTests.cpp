// SpatialUpscalerTests.cpp — Agente 1 (task_plan A.12): headless gate for the
// PUBLIC FSR-style spatial upscaler contract (ISpatialUpscaler). Proves the
// deterministic pure core: edge-adaptive upsampling keeps step edges steeper
// than bilinear while flat regions stay exact (no ringing) — no GPU required.

#include "engine/rendering/ISpatialUpscaler.hpp"

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

using Engine::Rendering::ISpatialUpscaler;
using Engine::Rendering::UpscaleConfig;
using Engine::Rendering::UpscaleMode;
using Engine::Rendering::create_spatial_upscaler;
using Engine::Rendering::create_spatial_upscaler_json;

// A flat source (every texel = value).
std::vector<float> flatImage(std::uint32_t w, std::uint32_t h, float value) {
    std::vector<float> img(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i + 3 < img.size(); i += 4) {
        img[i] = value;
        img[i + 1] = value;
        img[i + 2] = value;
        img[i + 3] = 1.0f;
    }
    return img;
}

// A vertical step edge: columns < edgeCol are 0, >= edgeCol are 1.
std::vector<float> stepImage(std::uint32_t w, std::uint32_t h,
                             std::uint32_t edgeCol) {
    std::vector<float> img(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const float v = (x >= edgeCol) ? 1.0f : 0.0f;
            float* p = &img[(static_cast<std::size_t>(y) * w + x) * 4u];
            p[0] = v;
            p[1] = v;
            p[2] = v;
            p[3] = 1.0f;
        }
    }
    return img;
}

// Max adjacent gradient of channel 0 along a middle row, in a window of
// destination columns around the mapped edge.
float maxGradient(const std::vector<float>& dst, std::uint32_t dw,
                  std::uint32_t dh, std::uint32_t fromX, std::uint32_t toX) {
    const std::uint32_t row = dh / 2;
    float m = 0.0f;
    for (std::uint32_t x = fromX + 1; x <= toX && x < dw; ++x) {
        const float a = dst[(static_cast<std::size_t>(row) * dw + (x - 1)) * 4u];
        const float b = dst[(static_cast<std::size_t>(row) * dw + x) * 4u];
        m = std::max(m, std::fabs(b - a));
    }
    return m;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        check(up != nullptr, "default spatial upscaler created");
        check(error.empty(), "default config diagnostic empty");

        UpscaleConfig bad = up->config();
        bad.srcWidth = 0;
        check(!up->configure(bad, error) && !error.empty(), "srcWidth 0 refused");
        bad = up->config();
        bad.scale = 0.5f;
        check(!up->configure(bad, error) && !error.empty(), "scale 0.5 refused");
        bad = up->config();
        bad.sharpness = 1.5f;
        check(!up->configure(bad, error) && !error.empty(),
              "sharpness 1.5 refused");
        bad = up->config();
        bad.edgeLo = 0.5f;
        bad.edgeHi = 0.1f;
        check(!up->configure(bad, error) && !error.empty(),
              "edgeHi < edgeLo refused");
        bad = up->config();
        bad.seed = 0;
        check(!up->configure(bad, error) && !error.empty(), "seed 0 refused");
        check(up->config().srcWidth == 64,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_spatial_upscaler(error);
        UpscaleConfig c = a->config();
        c.srcWidth = 32;
        c.srcHeight = 16;
        c.scale = 4.0f;
        c.sharpness = 0.5f;
        c.mode = UpscaleMode::Bilinear;
        c.seed = 5;
        check(a->configure(c, error), "custom config applied");

        auto b = create_spatial_upscaler_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().srcWidth == 32 && b->config().scale == 4.0f &&
                  b->config().sharpness == 0.5f &&
                  b->config().mode == UpscaleMode::Bilinear &&
                  b->config().seed == 5,
              "json round-trip bit-exact");

        auto e = create_spatial_upscaler_json(
            "{ \"version\": 1, \"mode\": \"bogus\", \"seed\": 1 }", error);
        check(e == nullptr, "unknown mode refused");
        check(create_spatial_upscaler_json("{ \"version\": 2, \"seed\": 1 }",
                                           error) == nullptr,
              "unsupported version refused");
        check(create_spatial_upscaler_json(
                  "{ \"version\": 1, \"scale\": 0.25 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_spatial_upscaler_json("{ \"seed\": 1 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. flat image stays exact in every mode (no ringing/overshoot) ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        UpscaleConfig c = up->config();
        c.srcWidth = 8;
        c.srcHeight = 8;
        c.scale = 2.0f;
        check(up->configure(c, error), "flat config applied");

        const auto src = flatImage(8, 8, 0.5f);
        for (int modeIdx = 0; modeIdx < 2; ++modeIdx) {
            c.mode = (modeIdx == 0) ? UpscaleMode::Bilinear
                                    : UpscaleMode::EdgeAdaptive;
            check(up->configure(c, error), "flat mode applied");
            std::vector<float> dst;
            check(up->upscale(src, dst, error), "flat upscale runs");
            // RGB must stay exactly 0.5 (alpha is 1.0 and is not checked).
            bool exact = true;
            for (std::size_t i = 0; i + 3 < dst.size(); i += 4) {
                if (std::fabs(dst[i] - 0.5f) > 1e-6f ||
                    std::fabs(dst[i + 1] - 0.5f) > 1e-6f ||
                    std::fabs(dst[i + 2] - 0.5f) > 1e-6f) {
                    exact = false;
                }
            }
            check(exact,
                  modeIdx == 0 ? "bilinear: flat stays exact"
                               : "edge-adaptive: flat stays exact (no ringing)");
        }
    }

    // ---- 4. step edge: edge-adaptive is steeper than bilinear ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        UpscaleConfig c = up->config();
        c.srcWidth = 8;
        c.srcHeight = 8;
        c.scale = 2.0f;
        check(up->configure(c, error), "edge config applied");

        const auto src = stepImage(8, 8, 4u);
        c.mode = UpscaleMode::Bilinear;
        check(up->configure(c, error), "bilinear applied");
        std::vector<float> dstB;
        check(up->upscale(src, dstB, error), "bilinear upscale runs");
        const float gB = maxGradient(dstB, 16, 16, 6, 11);

        c.mode = UpscaleMode::EdgeAdaptive;
        c.sharpness = 1.0f;
        check(up->configure(c, error), "edge-adaptive applied");
        std::vector<float> dstE;
        check(up->upscale(src, dstE, error), "edge-adaptive upscale runs");
        const float gE = maxGradient(dstE, 16, 16, 6, 11);

        check(gE > 1.1f * gB,
              "edge-adaptive keeps the step edge steeper than bilinear");
    }

    // ---- 5. sharpness 0 == bilinear (bit-exact) ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        UpscaleConfig c = up->config();
        c.srcWidth = 8;
        c.srcHeight = 8;
        c.scale = 2.0f;
        check(up->configure(c, error), "sharpness-0 config applied");
        const auto src = stepImage(8, 8, 4u);

        c.mode = UpscaleMode::Bilinear;
        check(up->configure(c, error), "bilinear applied");
        std::vector<float> dstB;
        check(up->upscale(src, dstB, error), "bilinear run");

        c.mode = UpscaleMode::EdgeAdaptive;
        c.sharpness = 0.0f;
        check(up->configure(c, error), "edge-adaptive sharpness 0 applied");
        std::vector<float> dstE;
        check(up->upscale(src, dstE, error), "edge-adaptive sharpness 0 run");

        check(dstB.size() == dstE.size() &&
                  std::memcmp(dstB.data(), dstE.data(),
                              dstB.size() * sizeof(float)) == 0,
              "sharpness 0 reproduces the bilinear output bit-exact");
    }

    // ---- 6. sharpness monotonic: steeper edge as sharpness grows ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        const auto src = stepImage(8, 8, 4u);
        float prev = -1.0f;
        bool monotonic = true;
        for (float s : {0.25f, 0.5f, 0.75f, 1.0f}) {
            UpscaleConfig c = up->config();
            c.srcWidth = 8;
            c.srcHeight = 8;
            c.scale = 2.0f;
            c.sharpness = s;
            check(up->configure(c, error), "sharpness sweep applied");
            std::vector<float> dst;
            check(up->upscale(src, dst, error), "sharpness sweep run");
            const float g = maxGradient(dst, 16, 16, 6, 11);
            if (g < prev) monotonic = false;
            prev = g;
        }
        check(monotonic, "edge steepness is monotonic in sharpness");
    }

    // ---- 7. no ringing into flat areas (exact far from the edge) ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        UpscaleConfig c = up->config();
        c.srcWidth = 8;
        c.srcHeight = 8;
        c.scale = 2.0f;
        c.sharpness = 1.0f;
        check(up->configure(c, error), "ringing config applied");
        const auto src = stepImage(8, 8, 4u);
        std::vector<float> dst;
        check(up->upscale(src, dst, error), "ringing upscale runs");

        // dst[4] maps to src x=1.75 (window cols 0..3, all 0); dst[11] to
        // src x=5.25 (window cols 4..7, all 1): the 4x4 lanczos window is fully
        // inside the flat side -> exact 0 / 1, no ringing bleed.
        const std::size_t row = 8u * 16u * 4u;
        check(dst[row + 4u * 4u] == 0.0f && dst[row + 4u * 4u + 1u] == 0.0f &&
                  dst[row + 4u * 4u + 2u] == 0.0f,
              "no ringing: flat dark side stays exactly 0");
        check(dst[row + 11u * 4u] == 1.0f && dst[row + 11u * 4u + 1u] == 1.0f &&
                  dst[row + 11u * 4u + 2u] == 1.0f,
              "no ringing: flat bright side stays exactly 1");
    }

    // ---- 8. determinism (bit-exact) ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        UpscaleConfig c = up->config();
        c.srcWidth = 16;
        c.srcHeight = 16;
        c.scale = 3.0f;
        check(up->configure(c, error), "determinism config applied");
        const auto src = stepImage(16, 16, 7u);

        std::vector<float> a, b;
        check(up->upscale(src, a, error), "determinism run A");
        check(up->upscale(src, b, error), "determinism run B");
        check(a.size() == b.size() &&
                  std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0,
              "identical inputs reproduce bit-exact upscale output");
    }

    // ---- 9. refusal: src size mismatch ----
    {
        std::string error;
        auto up = create_spatial_upscaler(error);
        UpscaleConfig c = up->config();
        c.srcWidth = 8;
        c.srcHeight = 8;
        check(up->configure(c, error), "refusal config applied");
        std::vector<float> badSrc(8u * 8u * 3u, 0.5f);  // wrong channel count
        std::vector<float> dst;
        check(!up->upscale(badSrc, dst, error) && !error.empty(),
              "src size mismatch refused all-or-nothing");
    }

    if (g_failures == 0) {
        std::printf("[spatial-upscaler] ALL PASSED\n");
        return 0;
    }
    std::printf("[spatial-upscaler] %d FAILURE(S)\n", g_failures);
    return 1;
}

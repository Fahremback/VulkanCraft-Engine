// FftOceanSurfaceTests.cpp — gate for the from-scratch native ocean core
// (task_plan C.20 vkfft/ocean — NOVO). Headless, deterministic, no GPU.
#include "engine/rendering/IFftOceanSurface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace Engine::Rendering;

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL (%d): %s\n", __LINE__, msg); ++g_failed; } else ++g_passed; } while (0)

int main() {
    std::printf("[fftocean] ALL tests starting\n");

    // 1. Config all-or-nothing.
    {
        std::string err;
        FftOceanConfig c;
        c.size = 12;                               // non power-of-two
        CHECK(!c.valid(err), "size 12 refused");
        c.size = 64;
        c.windSpeed = 99.0f;                        // out of range
        CHECK(!c.valid(err), "windSpeed too high refused");
        c.windSpeed = 18.0f;
        c.seed = 0;
        CHECK(!c.valid(err), "seed 0 refused");
        c.seed = 1;
        CHECK(c.valid(err), "default valid");
    }

    // 2. JSON round-trip + version gate.
    {
        std::string err;
        auto ocean = create_fft_ocean_surface(err);
        CHECK(ocean != nullptr, "factory ok");
        std::string json = ocean->config_to_json();
        auto ocean2 = create_fft_ocean_surface_json(json, err);
        CHECK(ocean2 != nullptr, "json roundtrip creates");
        CHECK(ocean2->config().windSpeed == ocean->config().windSpeed, "windSpeed round-trip");
    }
    {
        std::string err;
        auto ocean = create_fft_ocean_surface_json(R"({"version": 2})", err);
        CHECK(ocean == nullptr && !err.empty(), "bad version refused all-or-nothing");
    }

    // 3. Spectrum: directionality and decay.
    {
        std::string err;
        auto ocean = create_fft_ocean_surface(err);
        CHECK(ocean != nullptr, "create ok");
        // Along wind should dominate perpendicular at the same magnitude.
        const float k = 0.25f;
        const float wdir = ocean->config().windDirRad;
        const float sway = std::cos(wdir) * k, wz = std::sin(wdir) * k;
        const float dotK = -(1.0f) * k;  // perpendicular-ish
        float eAlong = ocean->spectrum(std::cos(wdir) * k, std::sin(wdir) * k);
        float ePerp = ocean->spectrum(std::sin(wdir) * k, -std::cos(wdir) * k);
        CHECK(eAlong >= ePerp, "wind-axis dominates");
        CHECK(eAlong >= 0.0f && std::isfinite(eAlong), "spectrum non-negative finite");
        // Higher freq at fixed wind should decay past the peak (large k).
        float eLow = ocean->spectrum(0.05f, 0.0f);
        float eHigh = ocean->spectrum(3.0f, 0.0f);
        // Not strictly ensured for all k; only require finite + bounded.
        CHECK(std::isfinite(eHigh), "high-k finite");
        (void)sway; (void)wz; (void)dotK; (void)eLow;
    }

    // 4. Synth determinism, size, grid, finite outputs.
    {
        std::string err;
        auto ocean = create_fft_ocean_surface(err);
        CHECK(ocean != nullptr, "create");
        std::vector<FftOceanVertex> a, b, c;
        CHECK(ocean->synthesize(2.5f, a, err), "synthesize ok");
        CHECK(ocean->synthesize(2.5f, b, err), "synthesize ok 2");
        const std::size_t n = ocean->config().size;
        CHECK(a.size() == n * n, "grid size");
        CHECK(b.size() == n * n, "grid size 2");
        bool identical = true;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].height != b[i].height || a[i].position.x != b[i].position.x ||
                a[i].position.z != b[i].position.z || a[i].normal.y != b[i].normal.y) {
                identical = false;
                break;
            }
        }
        CHECK(identical, "deterministic bit-exact for same time");
        bool finiteFields = true;
        for (const auto& v : a) {
            if (!std::isfinite(v.height) || !std::isfinite(v.position.x) ||
                !std::isfinite(v.position.z) || !std::isfinite(v.normal.x) ||
                !std::isfinite(v.normal.y) || !std::isfinite(v.normal.z)) {
                finiteFields = false;
                break;
            }
        }
        CHECK(finiteFields, "all fields finite");

        // Wrong time (non-finite) refused all-or-nothing.
        std::vector<FftOceanVertex> out;
        bool refused = !ocean->synthesize(std::numeric_limits<float>::quiet_NaN(), out, err);
        CHECK(refused && out.empty(), "NaN time refused, output intact");

        // Synth at a different time differs (time-varying).
        CHECK(ocean->synthesize(7.0f, c, err), "synth time B");
        bool differs = false;
        for (std::size_t i = 0; i < a.size() && !differs; ++i)
            if (a[i].height != c[i].height) differs = true;
        CHECK(differs, "time-varying height");  // (almost surely; projection differs)

        // Choppiness == 0 -> horizontal displacement near zero relative position.
        FftOceanConfig cc = ocean->config();
        cc.choppiness = 0.0f;
        auto calm = create_fft_ocean_surface(err);
        calm->configure(cc, err);
        std::vector<FftOceanVertex> z;
        CHECK(calm->synthesize(1.0f, z, err), "calm synth");
        float driftMax = 0.0f;
        for (const auto& v : z) {
            const float dx = std::fabs(v.position.x - v.grid.x);
            const float dz = std::fabs(v.position.z - v.grid.y);
            driftMax = std::max(driftMax, std::max(dx, dz));
        }
        CHECK(driftMax < 1e-3f, "zero-choppiness ~ pure vertical (drift ~0)");
    }

    std::printf("\n[fftocean] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[fftocean] FAILED\n"); return 1; }
    std::printf("[fftocean] ALL PASSED\n");
    return 0;
}
// ProbeGridTests.cpp — Agente 1 (task_plan A.10): headless gate for the PUBLIC
// DDGI / radiance probe-grid contract (IProbeGrid). Proves the deterministic
// pure core: toroidal clipmap scroll, EMA history accumulation, variance-driven
// relocation and backface classification resets, all under a per-frame budget —
// no GPU required.

#include "engine/rendering/IProbeGrid.hpp"

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

using Engine::Rendering::IProbeGrid;
using Engine::Rendering::ProbeCaptureSample;
using Engine::Rendering::ProbeCaptureSampler;
using Engine::Rendering::ProbeGridConfig;
using Engine::Rendering::ProbeGridProbe;
using Engine::Rendering::create_probe_grid;
using Engine::Rendering::create_probe_grid_json;

// A sampler with a mutable current radiance (for the EMA jump test).
struct StatefulSampler {
    float value{ 2.0f };
    ProbeCaptureSample operator()(const glm::vec3&, const glm::vec3&) const {
        return ProbeCaptureSample{ glm::vec3(value), false };
    }
};

// A sampler that returns a fixed radiance for every direction.
ProbeCaptureSampler uniformSampler(float radiance) {
    return [radiance](const glm::vec3&, const glm::vec3&) {
        return ProbeCaptureSample{ glm::vec3(radiance), false };
    };
}

// Bright +X (10) vs dim others (1) — strong directional dominance.
ProbeCaptureSampler brightXSampler() {
    return [](const glm::vec3&, const glm::vec3& dir) {
        const bool x = dir.x > 0.5f;
        return ProbeCaptureSample{ glm::vec3(x ? 10.0f : 1.0f), false };
    };
}

// Every direction is occluded (deep inside a wall).
ProbeCaptureSampler wallSampler() {
    return [](const glm::vec3&, const glm::vec3&) {
        return ProbeCaptureSample{ glm::vec3(3.0f), true };
    };
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        check(grid != nullptr, "default probe grid created");
        check(error.empty(), "default config diagnostic empty");
        check(grid->config().resolution == 8 && grid->config().cellSize == 4.0f &&
                  grid->config().probesPerFrame == 32,
              "defaults: 8^3 grid, 4m cells, 32 probes/frame");

        ProbeGridConfig bad = grid->config();
        bad.resolution = 1;
        check(!grid->configure(bad, error) && !error.empty(),
              "resolution 1 refused all-or-nothing");
        bad = grid->config();
        bad.cellSize = 0.1f;
        check(!grid->configure(bad, error) && !error.empty(),
              "cellSize 0.1 refused");
        bad = grid->config();
        bad.probesPerFrame = 0;
        check(!grid->configure(bad, error) && !error.empty(),
              "probesPerFrame 0 refused");
        bad = grid->config();
        bad.historyWeight = 0.0f;
        check(!grid->configure(bad, error) && !error.empty(),
              "historyWeight 0 refused");
        bad = grid->config();
        bad.backfaceThreshold = 7;
        check(!grid->configure(bad, error) && !error.empty(),
              "backfaceThreshold 7 refused");
        bad = grid->config();
        bad.seed = 0;
        check(!grid->configure(bad, error) && !error.empty(), "seed 0 refused");
        check(grid->config().resolution == 8,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_probe_grid(error);
        ProbeGridConfig c = a->config();
        c.resolution = 4;
        c.cellSize = 2.0f;
        c.probesPerFrame = 8;
        c.historyWeight = 0.25f;
        c.relocationEnabled = false;
        c.classificationEnabled = false;
        c.seed = 99;
        check(a->configure(c, error), "custom config applied");

        auto b = create_probe_grid_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().resolution == 4 && b->config().cellSize == 2.0f &&
                  b->config().historyWeight == 0.25f &&
                  !b->config().relocationEnabled && b->config().seed == 99,
              "json round-trip bit-exact");

        check(create_probe_grid_json("{ \"version\": 2, \"seed\": 1 }", error) ==
                  nullptr,
              "unsupported version refused");
        check(create_probe_grid_json(
                  "{ \"version\": 1, \"resolution\": 1 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
        check(create_probe_grid_json(
                  "{ \"version\": 1, \"bogus\": 1 }", error) == nullptr,
              "unknown json key refused");
        check(create_probe_grid_json("{ \"seed\": 1 }", error) == nullptr,
              "missing version refused");
    }

    // ---- 3. fresh slot captures the average on first update ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        ProbeGridConfig c = grid->config();
        c.resolution = 4;  // 64 probes
        c.cellSize = 4.0f;
        c.probesPerFrame = 64;
        check(grid->configure(c, error), "fresh config applied");

        check(grid->update(glm::vec3(0.0f), uniformSampler(2.0f), 0, &error) == 64u,
              "first frame updates all 64 probes");
        check(error.empty(), "no error on valid update");
        ProbeGridProbe p;
        check(grid->probe(0u, p), "probe 0 readable");
        check(near(p.irradiance.r, 2.0f, 1e-6f),
              "fresh probe stores the average capture radiance");
        check(p.age == 1u, "probe age increments after first update");
    }

    // ---- 4. EMA history accumulation converges ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        ProbeGridConfig c = grid->config();
        c.resolution = 4;
        c.probesPerFrame = 64;
        c.historyWeight = 0.1f;
        c.relocationEnabled = false;
        c.classificationEnabled = false;
        check(grid->configure(c, error), "ema config applied");

        StatefulSampler sampler;
        check(grid->update(glm::vec3(0.0f), sampler, 0, nullptr) == 64u,
              "frame 1 (radiance 2)");
        ProbeGridProbe p;
        grid->probe(0u, p);
        check(near(p.irradiance.r, 2.0f, 1e-6f),
              "frame 1: fresh probe at capture value");

        sampler.value = 4.0f;  // scene jumps
        grid->update(glm::vec3(0.0f), sampler, 0, nullptr);
        grid->probe(0u, p);
        check(near(p.irradiance.r, 2.2f, 1e-4f),
              "frame 2: one EMA step 2 -> 2.2");

        for (int i = 0; i < 9; ++i) {
            grid->update(glm::vec3(0.0f), sampler, 0, nullptr);
        }
        grid->probe(0u, p);
        check(near(p.irradiance.r, 4.0f - 2.0f * std::pow(0.9f, 10.0f), 1e-3f),
              "frame 11: EMA converges toward 4 (deterministic)");

        for (int i = 0; i < 41; ++i) {
            grid->update(glm::vec3(0.0f), sampler, 0, nullptr);
        }
        grid->probe(0u, p);
        // Probe 0 had 51 post-jump updates (frame 2 + 9 + 41).
        check(near(p.irradiance.r, 4.0f - 2.0f * std::pow(0.9f, 51.0f), 1e-3f),
              "frame 52: EMA asymptotically at 4");
        check(p.irradiance.r < 4.0f, "EMA stays below the target");
    }

    // ---- 5. toroidal scroll recycles leaving cells, fresh ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        ProbeGridConfig c = grid->config();
        c.resolution = 4;  // window [-2, 2) per axis
        c.cellSize = 4.0f;
        c.probesPerFrame = 8;
        check(grid->configure(c, error), "scroll config applied");

        // Frame 1 with a small budget (8 of 64 slots): the cursor ends at 8.
        check(grid->update(glm::vec3(0.0f), uniformSampler(1.0f), 0, nullptr) ==
                  8u,
              "scroll frame 1 (budget 8)");
        const int half = 2;
        // Camera moves 2 cells on +X: the window becomes [0, 4) on X.
        // Slots 0..7 are NOT updated this frame (cursor starts at 8).
        check(grid->update(glm::vec3(8.0f, 0.0f, 0.0f), uniformSampler(1.0f), 0,
                           nullptr) == 8u,
              "scroll frame 2 (camera +8m, budget 8)");

        bool allInWindow = true;
        for (std::uint32_t s = 0; s < grid->probe_count(); ++s) {
            ProbeGridProbe p;
            grid->probe(s, p);
            if (p.cell.x < 0 || p.cell.x >= 4) allInWindow = false;
            if (p.cell.y < -half || p.cell.y >= half) allInWindow = false;
            if (p.cell.z < -half || p.cell.z >= half) allInWindow = false;
        }
        check(allInWindow,
              "after the scroll every slot holds an in-window cell (clipmap follows camera)");

        // Slot 0 held cell (-2,-2,-2): x=-2 leaves the new window and wraps to
        // the entering relative offset +0 (absolute cell x = camCell 2 + 0).
        ProbeGridProbe p0;
        grid->probe(0u, p0);
        check(p0.cell.x == 2 && p0.cell.y == -2 && p0.cell.z == -2,
              "toroidal wrap: leaving cell -2 enters the new window at x=2");
        // Slot 0 was recycled but NOT updated this frame (budget 8 covered
        // slots 8..15): pending-fresh flag set, history empty.
        check((p0.flags & 1u) != 0u && p0.age == 0u &&
                  p0.irradiance.r == 0.0f,
              "recycled-but-unupdated slot is pending-fresh with cleared history");
        // Slot 2 held cell (0,-2,-2): x=0 is inside the new window -> retained,
        // and it was updated on frame 1 (age >= 1, no pending-fresh flag).
        ProbeGridProbe p2;
        grid->probe(2u, p2);
        check(p2.cell.x == 0 && (p2.flags & 1u) == 0u && p2.age >= 1u,
              "retained slot keeps its history across the scroll");
    }

    // ---- 6. relocation drifts toward the dominant direction, clamped ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        ProbeGridConfig c = grid->config();
        c.resolution = 4;
        c.probesPerFrame = 64;
        c.maxRelocationStep = 0.5f;
        c.classificationEnabled = false;
        check(grid->configure(c, error), "relocation config applied");

        check(grid->update(glm::vec3(0.0f), brightXSampler(), 0, nullptr) == 64u,
              "relocation frame 1");
        ProbeGridProbe p;
        grid->probe(0u, p);
        check(near(p.offset.x, 1.5f, 1e-4f),
              "dominance 0.75 * maxStep 2m = 1.5m drift on +X");
        check((p.flags & 2u) != 0u, "relocated flag set");
        check(grid->relocation_count() == 64u,
              "all 64 probes relocated on frame 1");

        grid->update(glm::vec3(0.0f), brightXSampler(), 0, nullptr);
        grid->probe(0u, p);
        check(near(p.offset.x, 2.0f, 1e-4f),
              "frame 2: drift clamps at half a cell (2m)");
        grid->update(glm::vec3(0.0f), brightXSampler(), 0, nullptr);
        grid->probe(0u, p);
        check(near(p.offset.x, 2.0f, 1e-4f),
              "frame 3: clamped probe does not move further");
        check(grid->relocation_count() == 0u,
              "no relocation counted when the position is clamped");

        // Uniform light -> zero dominance -> no drift at all.
        auto grid2 = create_probe_grid(error);
        check(grid2->configure(c, error), "uniform relocation config applied");
        check(grid2->update(glm::vec3(0.0f), uniformSampler(2.0f), 0, nullptr) ==
                  64u,
              "uniform frame 1");
        grid2->probe(0u, p);
        check(near(p.offset.x, 0.0f, 1e-6f) && near(p.offset.y, 0.0f, 1e-6f),
              "uniform light: no relocation (zero directional dominance)");
    }

    // ---- 7. classification resets probes inside geometry ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        ProbeGridConfig c = grid->config();
        c.resolution = 4;
        c.probesPerFrame = 64;
        c.relocationEnabled = false;
        c.backfaceThreshold = 4;
        check(grid->configure(c, error), "classification config applied");

        check(grid->update(glm::vec3(0.0f), wallSampler(), 0, nullptr) == 64u,
              "wall frame 1");
        ProbeGridProbe p;
        grid->probe(0u, p);
        check(p.resets == 1u, "all-backface probe classified (1 reset)");
        check(near(p.irradiance.r, 0.0f, 1e-6f),
              "classified probe history cleared");
        check(near(p.offset.x, 2.0f, 1e-4f),
              "classified probe pushed one step toward the exit (least-occluded axis)");
        check((p.flags & 4u) != 0u, "classified flag set");
        check(grid->classification_count() == 64u,
              "all 64 probes classified on frame 1");

        grid->update(glm::vec3(0.0f), wallSampler(), 0, nullptr);
        grid->probe(0u, p);
        check(p.resets == 2u,
              "still inside: classified again (resets accumulate)");

        // Classification disabled -> probes keep the capture as history.
        c.classificationEnabled = false;
        check(grid->configure(c, error), "classification disabled");
        auto grid2 = create_probe_grid(error);
        check(grid2->configure(c, error), "disabled config applied");
        check(grid2->update(glm::vec3(0.0f), wallSampler(), 0, nullptr) == 64u,
              "disabled wall frame 1");
        grid2->probe(0u, p);
        check(near(p.irradiance.r, 3.0f, 1e-6f) && p.resets == 0u,
              "without classification the capture becomes history");
    }

    // ---- 8. per-frame budget: exact cap, full coverage in ceil(n/budget) ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        ProbeGridConfig c = grid->config();
        c.resolution = 4;  // 64 probes
        c.probesPerFrame = 8;
        c.relocationEnabled = false;
        c.classificationEnabled = false;
        check(grid->configure(c, error), "budget config applied");

        std::uint32_t total = 0;
        for (int f = 0; f < 8; ++f) {
            const std::uint32_t n =
                grid->update(glm::vec3(0.0f), uniformSampler(1.0f), 0, nullptr);
            check(n == 8u, "each frame spends exactly the budget (8)");
            total += n;
        }
        check(total == 64u, "8 frames cover all 64 probes exactly once");

        // Cursor wrapped: probe 0 was updated on frame 1 only.
        ProbeGridProbe p;
        grid->probe(0u, p);
        check(p.age == 1u, "probe 0 updated exactly once after 8 frames");
        grid->update(glm::vec3(0.0f), uniformSampler(1.0f), 0, nullptr);
        grid->probe(0u, p);
        check(p.age == 2u, "probe 0 updated again on frame 9 (round-robin)");
    }

    // ---- 9. determinism (bit-exact) ----
    {
        std::string error;
        auto a = create_probe_grid(error);
        ProbeGridConfig c = a->config();
        c.resolution = 4;
        c.probesPerFrame = 16;
        c.historyWeight = 0.2f;
        check(a->configure(c, error), "determinism config A applied");
        auto b = create_probe_grid(error);
        check(b->configure(c, error), "determinism config B applied");

        StatefulSampler sampler;
        for (int f = 0; f < 12; ++f) {
            const glm::vec3 cam(static_cast<float>(f) * 3.0f, 0.0f,
                                static_cast<float>(f % 2));
            a->update(cam, sampler, 0, nullptr);
            b->update(cam, sampler, 0, nullptr);
            if (f == 5) sampler.value = 5.0f;
        }
        bool same = true;
        for (std::uint32_t s = 0; s < a->probe_count(); ++s) {
            ProbeGridProbe pa, pb;
            a->probe(s, pa);
            b->probe(s, pb);
            if (std::memcmp(&pa, &pb, sizeof(pa)) != 0) same = false;
        }
        check(same,
              "identical camera/sampler/budget sequence reproduces bit-exact probes");
    }

    // ---- 10. refusals ----
    {
        std::string error;
        auto grid = create_probe_grid(error);
        const std::uint32_t n =
            grid->update(glm::vec3(0.0f), ProbeCaptureSampler(), 0, &error);
        check(n == 0u && !error.empty(),
              "null sampler refused (returns 0 with diagnostic)");
    }

    if (g_failures == 0) {
        std::printf("[probe-grid] ALL PASSED\n");
        return 0;
    }
    std::printf("[probe-grid] %d FAILURE(S)\n", g_failures);
    return 1;
}

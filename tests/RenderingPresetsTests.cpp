// RenderingPresetsTests.cpp — Agente 1 (task_plan A.16): headless gate for the
// PUBLIC quality presets (IRenderingPresets). Proves the data-driven dial:
// every level resolves to a VALID preset, budgets scale monotonically with
// quality, name lookup works, validation is all-or-nothing, determinism.

#include "engine/rendering/IRenderingPresets.hpp"

#include <cstdio>
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

using Engine::Rendering::IRenderingPresets;
using Engine::Rendering::QualityLevel;
using Engine::Rendering::RenderingPreset;
using Engine::Rendering::create_rendering_presets;

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. every level resolves to a valid preset ----
    {
        std::string error;
        auto presets = create_rendering_presets(error);
        check(presets != nullptr && error.empty(), "presets created");
        for (int i = 0; i < static_cast<int>(QualityLevel::Count); ++i) {
            const QualityLevel level = static_cast<QualityLevel>(i);
            check(presets->validate(presets->preset(level), error),
                  std::string("built-in preset ") + presets->name(level) + " is valid");
        }
    }

    // ---- 2. budgets scale monotonically with quality ----
    {
        std::string error;
        auto presets = create_rendering_presets(error);
        const RenderingPreset& low = presets->preset(QualityLevel::Low);
        const RenderingPreset& high = presets->preset(QualityLevel::High);
        const RenderingPreset& cinematic = presets->preset(QualityLevel::Cinematic);

        check(high.gi.probesPerFrame >= low.gi.probesPerFrame,
              "high >= low probes per frame");
        check(high.gi.resolution >= low.gi.resolution, "high >= low resolution");
        check(cinematic.gi.probesPerFrame >= high.gi.probesPerFrame,
              "cinematic >= high probes per frame");
        check(cinematic.capture.cardsPerFrame >= high.capture.cardsPerFrame,
              "cinematic >= high capture budget");
        check(cinematic.diffuseGi.bounces >= high.diffuseGi.bounces,
              "cinematic >= high bounces");
        check(cinematic.trace.maxSteps >= high.trace.maxSteps,
              "cinematic >= high trace steps");
    }

    // ---- 3. data-driven name lookup ----
    {
        std::string error;
        auto presets = create_rendering_presets(error);
        RenderingPreset out;
        check(presets->preset_by_name("high", out), "lookup 'high' (case-insensitive)");
        check(out.gi.probesPerFrame ==
                  presets->preset(QualityLevel::High).gi.probesPerFrame,
              "name lookup matches the enum preset");
        check(presets->preset_by_name("CINEMATIC", out), "lookup 'CINEMATIC'");
        check(!presets->preset_by_name("ultra-mega", out),
              "unknown name refused (out untouched)");

        const std::vector<std::string> names = presets->level_names();
        check(names.size() == 5 && names[0] == "Low" && names[4] == "Cinematic",
              "level names in order");
    }

    // ---- 4. validation is all-or-nothing across all five sub-configs ----
    {
        std::string error;
        auto presets = create_rendering_presets(error);
        RenderingPreset bad = presets->preset(QualityLevel::High);

        bad.gi.cascadeCount = 0;
        check(!presets->validate(bad, error) && !error.empty(),
              "gi.cascadeCount 0 refused");

        bad = presets->preset(QualityLevel::High);
        bad.trace.maxSteps = 0;
        check(!presets->validate(bad, error), "trace.maxSteps 0 refused");

        bad = presets->preset(QualityLevel::High);
        bad.capture.cardsPerFrame = 0;
        check(!presets->validate(bad, error), "capture.cardsPerFrame 0 refused");

        bad = presets->preset(QualityLevel::High);
        bad.diffuseGi.bounces = 9;
        check(!presets->validate(bad, error), "diffuseGi.bounces 9 refused");

        bad = presets->preset(QualityLevel::High);
        bad.reflection.maxScreenRays = 0;
        check(!presets->validate(bad, error), "reflection.maxScreenRays 0 refused");
    }

    // ---- 5. determinism ----
    {
        std::string error;
        auto a = create_rendering_presets(error);
        auto b = create_rendering_presets(error);
        bool identical = true;
        for (int i = 0; i < static_cast<int>(QualityLevel::Count); ++i) {
            const QualityLevel level = static_cast<QualityLevel>(i);
            const RenderingPreset& pa = a->preset(level);
            const RenderingPreset& pb = b->preset(level);
            if (pa.gi.probesPerFrame != pb.gi.probesPerFrame ||
                pa.gi.resolution != pb.gi.resolution ||
                pa.capture.cardsPerFrame != pb.capture.cardsPerFrame ||
                pa.diffuseGi.bounces != pb.diffuseGi.bounces ||
                pa.trace.maxSteps != pb.trace.maxSteps ||
                pa.reflection.maxScreenRays != pb.reflection.maxScreenRays) {
                identical = false;
                break;
            }
        }
        check(identical, "two preset libraries are identical (determinism)");
    }

    if (g_failures == 0) {
        std::printf("[rendering-presets] ALL PASSED\n");
        return 0;
    }
    std::printf("[rendering-presets] %d FAILURE(S)\n", g_failures);
    return 1;
}
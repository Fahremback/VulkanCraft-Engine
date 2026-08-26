// ScreenSpaceTracerTests.cpp — Agente 1 (task_plan A.6): headless gate for the
// PUBLIC screen-space tracer (IScreenSpaceTracer). Proves the screen ray march
// against an injected depth sampler, off-screen fallback, history reprojection,
// disocclusion and determinism — no GPU.

#include "engine/rendering/IScreenSpaceTracer.hpp"

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

bool near(float a, float b, float eps = 1.0e-2f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::DepthSampler;
using Engine::Rendering::IScreenSpaceTracer;
using Engine::Rendering::ScreenTraceConfig;
using Engine::Rendering::ScreenTraceHit;
using Engine::Rendering::create_screen_space_tracer;

// A depth field: a vertical wall at view z = -5 (linear depth 5).
DepthSampler wall_at_depth(float d) {
    return [d](const glm::vec2&) { return d; };
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. configure all-or-nothing ----
    {
        std::string error;
        auto tracer = create_screen_space_tracer(error);
        check(tracer != nullptr && error.empty(), "tracer created");

        ScreenTraceConfig bad = tracer->config();
        bad.maxSteps = 0;
        check(!tracer->configure(bad, error) && !error.empty(), "maxSteps 0 refused");
        bad = tracer->config();
        bad.stepSize = 0.0f;
        check(!tracer->configure(bad, error), "stepSize 0 refused");
        bad = tracer->config();
        bad.depthBias = 0.0f;
        check(!tracer->configure(bad, error), "depthBias 0 refused");
        bad = tracer->config();
        bad.viewportWidth = 0;
        check(!tracer->configure(bad, error), "viewportWidth 0 refused");
    }

    // ---- 2. hit a wall straight ahead ----
    {
        std::string error;
        auto tracer = create_screen_space_tracer(error);
        const DepthSampler wall = wall_at_depth(5.0f);
        // Ray from the camera along -Z hits the wall at t == 5.
        const ScreenTraceHit hit = tracer->trace(
            glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), wall);
        check(hit.hit, "straight ray hits the wall");
        check(!hit.offscreen, "hit ray stays on screen");
        check(near(hit.t, 5.0f, 0.1f), "hit distance == wall depth 5");
        check(near(hit.position.z, -5.0f, 0.1f), "hit position at view z == -5");
    }

    // ---- 3. off-screen fallback ----
    {
        std::string error;
        auto tracer = create_screen_space_tracer(error);
        // A wall at depth 5; a ray pointing sideways (+X) leaves the viewport
        // (u > 1) before reaching the wall.
        const ScreenTraceHit hit = tracer->trace(
            glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, -0.1f),
            wall_at_depth(5.0f));
        check(!hit.hit, "sideways ray does not hit");
        check(hit.offscreen, "sideways ray flags off-screen (fallback trigger)");
    }

    // ---- 4. reprojection: point maps to itself under identity ----
    {
        std::string error;
        auto tracer = create_screen_space_tracer(error);
        const glm::mat4 identity(1.0f);
        // Under identity VP (w == 1), a view point (x, y, -z) maps to NDC
        // (x, y, -z), so (0.5, 0, -5) -> ndc.x = 0.5 -> uv.x = 0.75, uv.y = 0.5.
        const auto r = tracer->reproject(glm::vec3(0.5f, 0.0f, -5.0f),
                                         identity, identity);
        check(r.valid, "reproject valid under identity");
        check(near(r.uv.x, 0.75f, 0.01f), "reprojected UV follows the projection");
        check(near(r.uv.y, 0.5f, 1.0e-3f), "reprojected V is centered");
    }

    // ---- 5. disocclusion ----
    {
        std::string error;
        auto tracer = create_screen_space_tracer(error);
        check(!tracer->disoccluded(5.0f, 5.0f, 0.1f),
              "matching depths are NOT disoccluded");
        check(tracer->disoccluded(5.0f, 8.0f, 0.1f),
              "a large depth jump IS disoccluded (history stale)");
        check(!tracer->disoccluded(5.0f, 5.05f, 0.1f),
              "small depth change within threshold is NOT disoccluded");
    }

    // ---- 6. determinism ----
    {
        std::string error;
        auto a = create_screen_space_tracer(error);
        auto b = create_screen_space_tracer(error);
        const DepthSampler wall = wall_at_depth(5.0f);
        const ScreenTraceHit ha = a->trace(glm::vec3(0.0f),
                                           glm::vec3(0.0f, 0.0f, -1.0f), wall);
        const ScreenTraceHit hb = b->trace(glm::vec3(0.0f),
                                           glm::vec3(0.0f, 0.0f, -1.0f), wall);
        check(ha.hit == hb.hit && ha.offscreen == hb.offscreen &&
                  ha.t == hb.t && ha.uv == hb.uv && ha.position == hb.position &&
                  ha.steps == hb.steps,
              "two tracers reproduce bit-identical hits (determinism)");
    }

    if (g_failures == 0) {
        std::printf("[screen-space-tracer] ALL PASSED\n");
        return 0;
    }
    std::printf("[screen-space-tracer] %d FAILURE(S)\n", g_failures);
    return 1;
}
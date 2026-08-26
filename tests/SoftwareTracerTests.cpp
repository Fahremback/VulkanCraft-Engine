// SoftwareTracerTests.cpp — Agente 1 (task_plan A.7): headless gate for the
// PUBLIC software ray tracer (ISoftwareTracer). Proves sphere tracing against
// synthetic SDFs — hit/miss, analytic normals, shadow-ray occlusion, step
// bounds and determinism — no GPU.

#include "engine/rendering/ISoftwareTracer.hpp"

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

using Engine::Rendering::DistanceField;
using Engine::Rendering::ISoftwareTracer;
using Engine::Rendering::SoftwareTraceConfig;
using Engine::Rendering::SoftwareTraceHit;
using Engine::Rendering::create_software_tracer;

// A sphere of `radius` centered at `center`.
DistanceField sphere(glm::vec3 center, float radius) {
    return [center, radius](const glm::vec3& p) {
        return glm::length(p - center) - radius;
    };
}

// A ground plane at y = 0 (distance = p.y).
DistanceField plane(float y) {
    return [y](const glm::vec3& p) { return p.y - y; };
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. configure all-or-nothing ----
    {
        std::string error;
        auto tracer = create_software_tracer(error);
        check(tracer != nullptr && error.empty(), "tracer created");

        SoftwareTraceConfig bad = tracer->config();
        bad.maxSteps = 0;
        check(!tracer->configure(bad, error) && !error.empty(), "maxSteps 0 refused");
        bad = tracer->config();
        bad.maxDistance = 0.0f;
        check(!tracer->configure(bad, error), "maxDistance 0 refused");
        bad = tracer->config();
        bad.hitEpsilon = 0.0f;
        check(!tracer->configure(bad, error), "hitEpsilon 0 refused");
        bad = tracer->config();
        bad.normalEpsilon = 0.0f;
        check(!tracer->configure(bad, error), "normalEpsilon 0 refused");
    }

    // ---- 2. hit a sphere from outside ----
    {
        std::string error;
        auto tracer = create_software_tracer(error);
        const DistanceField sdf = sphere(glm::vec3(0.0f, 0.0f, 5.0f), 1.0f);
        // Ray from (0,0,0) along +Z: sphere surface at z = 4 (center 5, radius 1).
        const SoftwareTraceHit hit =
            tracer->trace(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), sdf);
        check(hit.hit, "ray hits the sphere");
        check(near(hit.distance, 4.0f, 0.05f), "hit distance == 4 (sphere near side)");
        check(near(hit.position.z, 4.0f, 0.05f), "hit position at z == 4");
        // Normal at the near pole points back toward the origin (-Z).
        check(near(hit.normal.z, -1.0f, 0.05f), "normal points back at the ray origin");
    }

    // ---- 3. miss: ray pointing away ----
    {
        std::string error;
        auto tracer = create_software_tracer(error);
        const DistanceField sdf = sphere(glm::vec3(0.0f, 0.0f, 5.0f), 1.0f);
        const SoftwareTraceHit hit =
            tracer->trace(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), sdf);
        check(!hit.hit, "upward ray misses the sphere");
        check(near(hit.distance, tracer->config().maxDistance),
              "miss reports the max-distance cap");
    }

    // ---- 4. inside the sphere: immediate hit ----
    {
        std::string error;
        auto tracer = create_software_tracer(error);
        const DistanceField sdf = sphere(glm::vec3(0.0f, 0.0f, 5.0f), 1.0f);
        const SoftwareTraceHit hit =
            tracer->trace(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(1.0f, 0.0f, 0.0f), sdf);
        check(hit.hit, "inside the sphere hits immediately");
        check(near(hit.distance, 0.0f, 1.0e-3f), "inside: distance 0");
    }

    // ---- 5. shadow-ray occlusion ----
    {
        std::string error;
        auto tracer = create_software_tracer(error);
        // Sphere centered at z=5 radius 1 spans z in [4, 6].
        const DistanceField sdf = sphere(glm::vec3(0.0f, 0.0f, 5.0f), 1.0f);
        // Light at origin; query point at z=10 — the sphere is between them.
        check(tracer->occluded(glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 0.0f, 10.0f), sdf),
              "a point behind the sphere is occluded from the light");
        // Point on the near side (z=3, before the sphere surface at z=4) sees it.
        check(!tracer->occluded(glm::vec3(0.0f, 0.0f, 0.0f),
                                glm::vec3(0.0f, 0.0f, 3.0f), sdf),
              "a point on the near side is NOT occluded");
    }

    // ---- 6. analytic plane normal ----
    {
        std::string error;
        auto tracer = create_software_tracer(error);
        const DistanceField sdf = plane(0.0f);
        const glm::vec3 n = tracer->surface_normal(glm::vec3(0.0f, 0.0f, 0.0f), sdf);
        check(near(n.x, 0.0f, 1.0e-3f) && near(n.y, 1.0f, 1.0e-3f) &&
                  near(n.z, 0.0f, 1.0e-3f),
              "plane normal is +Y");
    }

    // ---- 7. step bound + determinism ----
    {
        std::string error;
        auto a = create_software_tracer(error);
        auto b = create_software_tracer(error);
        SoftwareTraceConfig config = a->config();
        config.maxSteps = 64;
        a->configure(config, error);
        b->configure(config, error);

        const DistanceField sdf = sphere(glm::vec3(0.0f, 0.0f, 5.0f), 1.0f);
        const SoftwareTraceHit ha =
            a->trace(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), sdf);
        const SoftwareTraceHit hb =
            b->trace(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), sdf);
        check(ha.steps <= 64, "steps are bounded by maxSteps");
        check(ha.hit == hb.hit && ha.distance == hb.distance &&
                  ha.position == hb.position && ha.normal == hb.normal &&
                  ha.steps == hb.steps,
              "two tracers reproduce bit-identical hits (determinism)");
    }

    if (g_failures == 0) {
        std::printf("[software-tracer] ALL PASSED\n");
        return 0;
    }
    std::printf("[software-tracer] %d FAILURE(S)\n", g_failures);
    return 1;
}
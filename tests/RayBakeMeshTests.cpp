// RayBakeMeshTests.cpp — gate for the from-scratch offline ray-bake core
// (task_plan C.4 embree cooker). Headless deterministic AO bake over IRayTracer.
#include "engine/rendering/IRayBakeMesh.hpp"
#include "engine/rendering/IRayTracer.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace vc::rendering;

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL (%d): %s\n", __LINE__, msg); ++g_failed; } else ++g_passed; } while (0)

static void groundPlane(std::vector<RayTracerTriangle>& t) {
    // big horizontal ground quad at y = 0, top-facing (+y normal).
    RayTracerTriangle a = {{-10.f,0.f,-10.f},{10.f,0.f,-10.f},{10.f,0.f,10.f}};
    RayTracerTriangle b = {{-10.f,0.f,-10.f},{10.f,0.f,10.f},{-10.f,0.f,10.f}};
    t.push_back(a); t.push_back(b);
}

static void ceilingAndSkirt(std::vector<RayTracerTriangle>& t) {
    groundPlane(t);
    // A roof 6 units up. The interior sees its UNDER-side, so the front face
    // must point -y (downward): pickings order so (v1-v0)x(v2-v0) = -y so that
    // up-going rays hit the FRONT face (single-sided culling respected).
    RayTracerTriangle c = {{-10.f,6.f,-10.f},{10.f,6.f,-10.f},{10.f,6.f,10.f}}; // front -y
    RayTracerTriangle d = {{-10.f,6.f,-10.f},{10.f,6.f,10.f},{-10.f,6.f,10.f}}; // front -y
    t.push_back(c); t.push_back(d);
}

int main() {
    std::printf("[raybake] ALL tests starting\n");

    // 1. Config.
    std::string err;
    RayBakeConfig c;
    c.samples = 2; CHECK(!c.valid(err), "samples 2 refused");
    c.samples = 16;
    c.maxDistance = -1.0f; CHECK(!c.valid(err), "maxDistance negative refused");
    c.maxDistance = 32.0f;
    c.seed = 0; CHECK(!c.valid(err), "seed 0 refused");
    c.seed = 1;
    CHECK(c.valid(err), "default valid");

    // 2. JSON.
    {
        auto bake = create_ray_bake_mesh(err);
        std::string json = bake->config_to_json();
        auto bake2 = create_ray_bake_mesh(err);
        CHECK(bake2->configure_json(json, err), "json configure ok");
        CHECK(bake2->config().samples == bake->config().samples, "roundtrip");
    }
    {
        auto bake = create_ray_bake_mesh(err);
        CHECK(!bake->configure_json(R"({"version": 9})", err), "bad version refused");
    }

    // 3. Not-built queries refuse.
    {
        auto bake = create_ray_bake_mesh(err);
        std::vector<RayBakeSample> out;
        float o[3] = {0.f, 1.f, 0.f};
        CHECK(!bake->bake(o, 1, out, err), "bake before build refused, output intact");
        CHECK(out.empty(), "output untouched");
    }

    // 4. Open point above a plain ground plane -> open (AO~1).
    {
        std::vector<RayTracerTriangle> t;
        groundPlane(t);
        auto bake = create_ray_bake_mesh(err);
        RayBakeConfig bc = bake->config();
        bc.samples = 64;
        bake->configure(bc, err);
        CHECK(bake->build(t.data(), (std::int32_t)t.size(), err), "build ground");
        float o[3] = {0.f, 30.f, 0.f};   // far above, rays never hit anything <32 (plane at 0 is 30 away)
        std::vector<RayBakeSample> out;
        CHECK(bake->bake(o, 1, out, err), "bake open");
        CHECK(out.size() == 1, "one sample");
        CHECK(out[0].occlusion > 0.98f, "open point near fully open");
    }

    // 5. Enclosed slot (roof 6m above) -> occluded for rays going UP.
    {
        std::vector<RayTracerTriangle> t;
        ceilingAndSkirt(t);
        auto bake = create_ray_bake_mesh(err);
        RayBakeConfig bc = bake->config();
        bc.samples = 128;
        bake->configure(bc, err);
        CHECK(bake->build(t.data(), (std::int32_t)t.size(), err), "build roof");
        float o[3] = {0.f, 3.f, 0.f};   // between ground and roof (6m) -> up-rays occluded
        std::vector<RayBakeSample> out;
        CHECK(bake->bake(o, 1, out, err), "bake enclosed");
        const float upFraction = 128.0f;
        (void)upFraction;
        // Up-facing rays hit the roof: AO should be substantially < open point.
        CHECK(out[0].occlusion < 0.75f, "enclosed slot reduces AO vs open");
    }

    // 6. Determinism.
    {
        std::vector<RayTracerTriangle> t;
        ceilingAndSkirt(t);
        auto bake = create_ray_bake_mesh(err);
        RayBakeConfig bc = bake->config();
        bc.samples = 64;
        bake->configure(bc, err);
        bake->build(t.data(), (std::int32_t)t.size(), err);
        float o[3] = {0.4f, 3.0f, -0.2f};
        std::vector<RayBakeSample> a, b;
        bake->bake(o, 1, a, err);
        bake->bake(o, 1, b, err);
        bool same = a[0].occlusion == b[0].occlusion && a[0].meanDistance == b[0].meanDistance;
        CHECK(same, "deterministic bit-exact");
    }

    // 7. bake_normals: a wall surface (normal +Z) must bake the hemisphere
    //    FACING the wall — the +Y-only bake() would shoot up and miss it.
    {
        std::vector<RayTracerTriangle> t;
        // Vertical wall at z = 0. The probe sits on the -z side, so the front
        // face must point -z (winding with right-hand normal -z); the tracer is
        // single-sided and culls rays that hit the back face.
        RayTracerTriangle a = {{-10.f,0.f,0.f},{10.f,6.f,0.f},{10.f,0.f,0.f}};
        RayTracerTriangle b = {{-10.f,0.f,0.f},{-10.f,6.f,0.f},{10.f,6.f,0.f}};
        t.push_back(a); t.push_back(b);
        auto bake = create_ray_bake_mesh(err);
        RayBakeConfig bc = bake->config();
        bc.samples = 128;
        bake->configure(bc, err);
        CHECK(bake->build(t.data(), (std::int32_t)t.size(), err), "build wall");
        float o[3] = {0.f, 3.f, -2.f};      // 2 units in front of the wall
        float n[3] = {0.f, 0.f, 1.f};       // normal points AT the wall
        std::vector<RayBakeSample> outN, outY;
        CHECK(bake->bake_normals(o, n, 1, outN, err), "bake_normals wall");
        CHECK(bake->bake(o, 1, outY, err), "bake (+Y) wall");
        // With the normal-aware hemisphere the wall occludes most rays;
        // the +Y hemisphere looks up and sees open sky.
        CHECK(outN[0].occlusion < 0.6f, "wall normal hemisphere sees the wall");
        CHECK(outY[0].occlusion > outN[0].occlusion + 0.15f,
              "+Y-only bake overestimates openness on a wall");
    }

    // 8. Build refusal all-or-nothing.
    {
        auto bake = create_ray_bake_mesh(err);
        std::string be;
        CHECK(!bake->build(nullptr, 3, be), "null build refused");
        std::vector<RayTracerTriangle> empty;
        CHECK(!bake->build(empty.data(), 0, be), "0 tri refused");
        // bake_normals refuses null normals too.
        std::vector<RayBakeSample> out;
        float o[3] = {0.f, 1.f, 0.f};
        CHECK(!bake->bake_normals(o, nullptr, 1, out, be), "null normals refused");
    }

    std::printf("\n[raybake] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[raybake] FAILED\n"); return 1; }
    std::printf("[raybake] ALL PASSED\n");
    return 0;
}
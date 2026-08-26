// RayTracerGpuTests — gate do backend HARDWARE (GPU) do IRayTracer
// (Vulkan ray tracing, VK_KHR_ray_tracing_pipeline). A.8: hardware ray
// tracing quando disponível, sem mudar a API pública.
//
// Cena determinística IDÊNTICA à do gate embree (tetraedro + chão, 6 tris) e
// os mesmos 7 rays. Asserções:
//   * hit/miss EXATO vs brute-force Möller–Trumbore single-sided;
//   * primitiveIndex EXATO vs brute-force;
//   * t com tolerância (GPU float paths != CPU);
//   * determinismo cross-build (mesma cena -> mesmos resultados);
//   * occluded == existe closest-hit;
//   * recusas all-or-nothing (null/0 -> build=false, consultas miss);
//   * [tMin, tMax] respeitado;
//   * cross-check vs o backend SOFTWARE (Embree): hit/miss e prim concordam.
//
// Se a GPU não expuser RT, o teste imprime SKIPPED e passa (fallback honesto
// da factory) — na máquina de desenvolvimento (RTX 3060) ele roda de verdade.
#include "engine/rendering/IRayTracer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#include <cstdlib>
#endif

namespace {

struct Vec3 { float x, y, z; };
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 norm(Vec3 a) { float l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }

// Brute-force single-sided — mesma regra empírica do Embree (front = normal).
bool bruteClosest(const std::vector<vc::rendering::RayTracerTriangle>& tris,
                  const vc::rendering::RayTracerRay& ray, float& outT, int& outTri) {
    const Vec3 org{ray.ox, ray.oy, ray.oz};
    const Vec3 dir{ray.dx, ray.dy, ray.dz};
    float bestT = ray.tMax; int bestTri = -1;
    for (size_t i = 0; i < tris.size(); ++i) {
        const auto& t = tris[i];
        const Vec3 a{t.v0[0], t.v0[1], t.v0[2]};
        const Vec3 b{t.v1[0], t.v1[1], t.v1[2]};
        const Vec3 c{t.v2[0], t.v2[1], t.v2[2]};
        Vec3 n = norm(cross(sub(b, a), sub(c, a)));
        if (dot(n, dir) >= 0.0f) continue;               // backface → cull
        Vec3 e1 = sub(b, a), e2 = sub(c, a);
        Vec3 p = cross(dir, e2);
        float det = dot(e1, p);
        if (std::fabs(det) < 1e-9f) continue;
        float invDet = 1.0f / det;
        Vec3 s = sub(org, a);
        float u = dot(s, p) * invDet;
        if (u < 0.0f || u > 1.0f) continue;
        Vec3 q = cross(s, e1);
        float v = dot(dir, q) * invDet;
        if (v < 0.0f || u + v > 1.0f) continue;
        float tt = dot(e2, q) * invDet;
        if (tt > 1e-4f && tt < bestT) { bestT = tt; bestTri = (int)i; }
    }
    if (bestTri < 0) return false;
    outT = bestT; outTri = bestTri;
    return true;
}

int g_failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d — %s\n", __FILE__, __LINE__, msg);       \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

}  // namespace

int main() {
    // Nesta máquina o hook de captura do OBS é uma layer implícita registrada
    // que crasha a cadeia de dispatch do Vulkan; o manifest dela define
    // disable_environment DISABLE_VULKAN_OBS_CAPTURE=1. Setamos o env var no
    // início para o loader nem carregar o hook (antes do vkCreateInstance).
#ifdef _WIN32
    _putenv_s("DISABLE_VULKAN_OBS_CAPTURE", "1");
#endif

    // Cena determinística: tetraedro (4 tris) + chão (2 tris) = 6 triângulos.
    const float verts[8][3] = {
        {0,0,0},{1,0,0},{0,1,0},{0,0,1},
        {-2,0,-2},{2,0,-2},{2,0,2},{-2,0,2}
    };
    const int idx[6][3] = {
        {0,2,1},{0,1,3},{0,3,2},{1,2,3},
        {4,5,6},{4,6,7}
    };
    std::vector<vc::rendering::RayTracerTriangle> tris(6);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 3; ++j) {
            float* dst = j == 0 ? tris[i].v0 : (j == 1 ? tris[i].v1 : tris[i].v2);
            dst[0] = verts[idx[i][j]][0]; dst[1] = verts[idx[i][j]][1]; dst[2] = verts[idx[i][j]][2];
        }

    std::vector<vc::rendering::RayTracerRay> rays = {
        {0.3f,0.3f,0.5f, 0,0,-1, 0.0f,1e30f},                       // z=0 face, t=0.5
        {0.3f,0.5f,0.3f, -1,0,0, 0.0f,1e30f},                       // x=0 face, t=0.3
        {0.5f,0.3f,0.3f, 0,-1,0, 0.0f,1e30f},                       // y=0 face, t=0.3
        {0.2f,0.2f,0.2f, 0.57735027f,0.57735027f,0.57735027f, 0.0f,1e30f}, // slanted face, t≈0.231
        {0.3f,3.0f,0.3f, 0,-1,0, 0.0f,1e30f},                       // slanted face (front), t=2.6
        {5.0f,5.0f,5.0f, 1,0,0, 0.0f,1e30f},                        // miss total
        {0.25f,0.1f,0.25f, 0,1,0, 0.0f,1e30f}                       // de dentro p/ cima → backface → miss
    };

    // Backend hardware — nullptr significa GPU sem RT (fallback honesto).
    auto hw = vc::rendering::create_hw_ray_tracer();
    if (!hw) {
        std::printf("ray_tracer_gpu_tests: SKIPPED (GPU sem ray tracing — "
                    "create_hw_ray_tracer() retornou nullptr)\n");
        return 0;
    }

    // 1) Recusas all-or-nothing.
    {
        CHECK(!hw->build(nullptr, 6), "build com null deve recusar");
        CHECK(!hw->build(tris.data(), 0), "build com 0 triângulos deve recusar");
        auto h = hw->closestHit(rays[0]);
        CHECK(!h.hit, "consulta após build falho deve retornar hit=false");
        CHECK(!hw->occluded(rays[0]), "occlusion após build falho deve retornar false");
    }

    // 2) Build real + closest-hit vs brute-force + determinismo cross-build.
    std::vector<vc::rendering::RayTracerHit> golden;
    for (int buildRun = 0; buildRun < 2; ++buildRun) {
        CHECK(hw->build(tris.data(), (int32_t)tris.size()), "build da cena deve passar");
        std::vector<vc::rendering::RayTracerHit> hits;
        for (const auto& ray : rays) {
            vc::rendering::RayTracerHit h = hw->closestHit(ray);
            hits.push_back(h);
            float tB; int triB;
            bool hitB = bruteClosest(tris, ray, tB, triB);
            if (hitB) {
                CHECK(h.hit, "gpu deve atingir o que o brute atinge");
                // GPU usa float paths diferentes do CPU: tolerância relativa.
                const float tol = 1e-3f * (tB > 1.0f ? tB : 1.0f);
                CHECK(std::fabs(h.t - tB) < tol, "t deve casar com o brute (tolerância GPU)");
                CHECK(h.primitiveIndex == triB, "primitiveIndex deve casar com o brute");
            } else {
                CHECK(!h.hit, "gpu não deve atingir o que o brute não atinge");
            }
            // Occlusion consistente com closest-hit.
            bool occ = hw->occluded(ray);
            CHECK(occ == h.hit, "occluded deve ser true sse existe closest-hit");
        }
        if (buildRun == 0) golden = hits;
        else {
            CHECK(golden.size() == hits.size(), "mesma quantidade de hits");
            for (size_t i = 0; i < golden.size() && i < hits.size(); ++i) {
                CHECK(golden[i].hit == hits[i].hit &&
                          golden[i].primitiveIndex == hits[i].primitiveIndex &&
                          golden[i].t == hits[i].t,
                      "determinismo bit-exato cross-build (mesma cena, mesma GPU)");
            }
        }
    }

    // 3) Intervalo [tMin, tMax] respeitado (mesmo caso do gate embree).
    {
        CHECK(hw->build(tris.data(), (int32_t)tris.size()), "rebuild ok");
        vc::rendering::RayTracerRay shortRay = rays[0];
        shortRay.tMax = 0.05f;                    // hit real em t=0.1 (face slanted)
        auto h = hw->closestHit(shortRay);
        CHECK(!h.hit, "tMax menor que o hit deve cortar");
        vc::rendering::RayTracerRay tight = rays[0];
        tight.tMin = 0.2f;                        // hit real em t=0.1
        auto h2 = hw->closestHit(tight);
        CHECK(!h2.hit, "tMin maior que o hit deve cortar");
    }

    // 4) Cross-check vs o backend SOFTWARE (Embree): mesmos hit/miss e prim.
    {
        auto sw = vc::rendering::create_ray_tracer();
        CHECK(sw != nullptr, "factory software não pode falhar");
        CHECK(sw->build(tris.data(), (int32_t)tris.size()), "build software ok");
        CHECK(hw->build(tris.data(), (int32_t)tris.size()), "build hardware ok");
        for (const auto& ray : rays) {
            vc::rendering::RayTracerHit hHw = hw->closestHit(ray);
            vc::rendering::RayTracerHit hSw = sw->closestHit(ray);
            CHECK(hHw.hit == hSw.hit, "hardware e software devem concordar em hit/miss");
            if (hHw.hit && hSw.hit) {
                CHECK(hHw.primitiveIndex == hSw.primitiveIndex,
                      "hardware e software devem concordar no primitiveIndex");
                const float tol = 1e-3f * (hSw.t > 1.0f ? hSw.t : 1.0f);
                CHECK(std::fabs(hHw.t - hSw.t) < tol,
                      "hardware e software devem concordar em t (tolerância)");
            }
        }
    }

    // 5) Seleção data-driven (A.8): preferHardware=true retorna HW nesta máquina.
    {
        auto pref = vc::rendering::create_ray_tracer_preferred(true);
        CHECK(pref != nullptr, "preferred(true) deve retornar um tracer (HW ou fallback)");
        if (pref) {
            CHECK(pref->build(tris.data(), (int32_t)tris.size()), "preferred build ok");
            vc::rendering::RayTracerHit h = pref->closestHit(rays[0]);
            CHECK(h.hit, "preferred deve resolver o ray 0 (z=0 face)");
        }
    }

    if (g_failures == 0) {
        std::printf("ray_tracer_gpu_tests: ALL PASSED (GPU RT — %zu rays, %zu tris, "
                    "cross-check software ok)\n", rays.size(), tris.size());
        return 0;
    }
    std::printf("ray_tracer_gpu_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}

// EmbreeRayTracerTests — gate headless do IRayTracer (backend Embree 4.4.1
// vendido no vc_sdk_public). Cena determinística (tetraedro + chão), rays
// front-face interiores; comparação vs brute-force Möller–Trumbore com a MESMA
// semântica single-sided (front = lado do normal right-hand; cull se
// dot(normal, dir) >= 0) + determinismo bit-exato + recusas all-or-nothing.
#include "engine/rendering/IRayTracer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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

    // Rays front-face interiores, direções NORMALIZADAS.
    std::vector<vc::rendering::RayTracerRay> rays = {
        {0.3f,0.3f,0.5f, 0,0,-1, 0.0f,1e30f},                       // z=0 face, t=0.5
        {0.3f,0.5f,0.3f, -1,0,0, 0.0f,1e30f},                       // x=0 face, t=0.3
        {0.5f,0.3f,0.3f, 0,-1,0, 0.0f,1e30f},                       // y=0 face, t=0.3
        {0.2f,0.2f,0.2f, 0.57735027f,0.57735027f,0.57735027f, 0.0f,1e30f}, // slanted face, t≈0.231
        {0.3f,3.0f,0.3f, 0,-1,0, 0.0f,1e30f},                       // slanted face (front), t=2.6
        {5.0f,5.0f,5.0f, 1,0,0, 0.0f,1e30f},                        // miss total
        {0.25f,0.1f,0.25f, 0,1,0, 0.0f,1e30f}                       // de dentro p/ cima → backface → miss
    };

    // 1) Recusas all-or-nothing.
    {
        auto tr = vc::rendering::create_ray_tracer();
        CHECK(tr != nullptr, "factory não pode falhar");
        CHECK(!tr->build(nullptr, 6), "build com null deve recusar");
        CHECK(!tr->build(tris.data(), 0), "build com 0 triângulos deve recusar");
        auto h = tr->closestHit(rays[0]);
        CHECK(!h.hit, "consulta após build falho deve retornar hit=false");
        CHECK(!tr->occluded(rays[0]), "occlusion após build falho deve retornar false");
    }

    // 2) Build real + closest-hit vs brute-force + determinismo.
    for (int buildRun = 0; buildRun < 2; ++buildRun) {
        auto tr = vc::rendering::create_ray_tracer();
        CHECK(tr->build(tris.data(), (int32_t)tris.size()), "build da cena deve passar");

        // Snapshot bit-exato do primeiro build para comparar com o segundo.
        static std::vector<vc::rendering::RayTracerHit> golden;
        std::vector<vc::rendering::RayTracerHit> hits;
        for (const auto& ray : rays) {
            vc::rendering::RayTracerHit h = tr->closestHit(ray);
            hits.push_back(h);
            float tB; int triB;
            bool hitB = bruteClosest(tris, ray, tB, triB);
            if (hitB) {
                CHECK(h.hit, "embree deve atingir o que o brute atinge");
                CHECK(std::fabs(h.t - tB) < 1e-4f, "t deve casar com o brute");
                CHECK(h.primitiveIndex == triB, "primitiveIndex deve casar com o brute");
            } else {
                CHECK(!h.hit, "embree não deve atingir o que o brute não atinge");
            }
        }
        if (buildRun == 0) golden = hits;
        else {
            CHECK(golden.size() == hits.size(), "mesma quantidade de hits");
            for (size_t i = 0; i < golden.size() && i < hits.size(); ++i) {
                CHECK(golden[i].hit == hits[i].hit && golden[i].t == hits[i].t &&
                          golden[i].primitiveIndex == hits[i].primitiveIndex,
                      "determinismo bit-exato cross-build");
            }
        }

        // 3) Occlusion consistente com closest-hit (any-hit == existe hit).
        for (const auto& ray : rays) {
            bool occ = tr->occluded(ray);
            vc::rendering::RayTracerHit h = tr->closestHit(ray);
            CHECK(occ == h.hit, "occluded deve ser true sse existe closest-hit");
        }
    }

    // 4) Intervalo [tMin, tMax] respeitado: tMax curto corta o hit.
    {
        auto tr = vc::rendering::create_ray_tracer();
        CHECK(tr->build(tris.data(), (int32_t)tris.size()), "rebuild ok");
        vc::rendering::RayTracerRay shortRay = rays[0];
        shortRay.tMax = 0.2f;                     // hit real em t=0.5
        auto h = tr->closestHit(shortRay);
        CHECK(!h.hit, "tMax menor que o hit deve cortar");
        vc::rendering::RayTracerRay tight = rays[0];
        tight.tMin = 0.6f;                        // hit real em t=0.5
        auto h2 = tr->closestHit(tight);
        CHECK(!h2.hit, "tMin maior que o hit deve cortar");
    }

    if (g_failures == 0) {
        std::printf("embree_ray_tracer_tests: ALL PASSED (%zu rays, %zu tris)\n",
                    rays.size(), tris.size());
        return 0;
    }
    std::printf("embree_ray_tracer_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}

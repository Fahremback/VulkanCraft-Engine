// EmbreeRayTracer — adapter do IRayTracer sobre o core Embree 4.4.1 vendido
// (compilado DENTRO de vc_sdk_public — padrão BUG-010, sem libs externas).
// Semântica single-sided (front = normal right-hand da winding) e determinismo
// verificados no gate embree_ray_tracer_tests (Test #19).
#include "engine/rendering/IRayTracer.hpp"

#include <embree4/rtcore.h>

#include <cstdint>
#include <memory>
#include <vector>

// The vendored embree4 build does not honor per-ray cull flags in its
// kernels (it is a stripped subset). We therefore upload triangles double-sided
// and apply single-sided semantics (cull when dot(winding_normal, dir) >= 0)
// manually in the adapter, matching the gate's brute-force Möller–Trumbore.
namespace vc::rendering {

namespace {

struct Vec3f { float x, y, z; };
Vec3f vsub(Vec3f a, Vec3f b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3f vcross(Vec3f a, Vec3f b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
float vdot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

// Single-sided test: returns true when the hit triangle's winding normal
// faces the ray (dot(normal, dir) < 0), i.e. a valid front-face hit.
bool front_facing(const float* v0, const float* v1, const float* v2,
                  const RayTracerRay& ray) {
    const Vec3f a{v0[0], v0[1], v0[2]};
    const Vec3f b{v1[0], v1[1], v1[2]};
    const Vec3f c{v2[0], v2[1], v2[2]};
    const Vec3f n = vcross(vsub(b, a), vsub(c, a));
    const Vec3f d{ray.dx, ray.dy, ray.dz};
    // Cull back face: dot(normal, dir) > 0.  (>= 0 matches the reference; a
    // grazing dot==0 hit is numerically ambiguous so we treat it as outside.)
    return vdot(n, d) < 0.0f;
}

// RAII do estado Embree (device + scene + geometry por instância do tracer).
class EmbreeRayTracerImpl final : public IRayTracer {
public:
    EmbreeRayTracerImpl() = default;
    ~EmbreeRayTracerImpl() override { teardown(); }
    EmbreeRayTracerImpl(const EmbreeRayTracerImpl&) = delete;
    EmbreeRayTracerImpl& operator=(const EmbreeRayTracerImpl&) = delete;

    bool build(const RayTracerTriangle* triangles, int32_t count) override {
        teardown();
        if (count <= 0 || triangles == nullptr) return false;

        RTCDevice device = rtcNewDevice(nullptr);
        if (device == nullptr) return false;



        RTCScene scene = rtcNewScene(device);
        RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        if (scene == nullptr || geometry == nullptr) {
            if (geometry) rtcReleaseGeometry(geometry);
            if (scene) rtcReleaseScene(scene);
            rtcReleaseDevice(device);
            return false;
        }

        // Buffers de vértices e índices (cópia: o contrato não retém ponteiros).
        std::vector<float> vertices;
        std::vector<std::uint32_t> indices;
        vertices.reserve(static_cast<size_t>(count) * 9u);
        indices.reserve(static_cast<size_t>(count) * 3u);
        for (int32_t i = 0; i < count; ++i) {
            const RayTracerTriangle& t = triangles[i];
            vertices.push_back(t.v0[0]); vertices.push_back(t.v0[1]); vertices.push_back(t.v0[2]);
            vertices.push_back(t.v1[0]); vertices.push_back(t.v1[1]); vertices.push_back(t.v1[2]);
            vertices.push_back(t.v2[0]); vertices.push_back(t.v2[1]); vertices.push_back(t.v2[2]);
            const std::uint32_t base = static_cast<std::uint32_t>(i) * 3u;
            indices.push_back(base); indices.push_back(base + 1u); indices.push_back(base + 2u);
        }

        rtcSetSharedGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                   vertices.data(), 0, sizeof(float) * 3u,
                                   static_cast<unsigned>(vertices.size() / 3u));
        rtcSetSharedGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                   indices.data(), 0, sizeof(std::uint32_t) * 3u,
                                   static_cast<unsigned>(indices.size() / 3u));
        rtcCommitGeometry(geometry);
        rtcAttachGeometry(scene, geometry);
        rtcCommitScene(scene);

        device_ = device;
        scene_ = scene;
        geometry_ = geometry;
        vertices_ = std::move(vertices);
        indices_ = std::move(indices);
        vertex_index_count_ = static_cast<std::uint32_t>(count);
        return true;
    }

    RayTracerHit closestHit(const RayTracerRay& ray) const override {
        RayTracerHit out{};
        out.hit = false;
        out.t = 0.0f;
        out.primitiveIndex = -1;
        if (scene_ == nullptr) return out;

        RTCRayHit rh{};
        rh.ray.org_x = ray.ox; rh.ray.org_y = ray.oy; rh.ray.org_z = ray.oz;
        rh.ray.dir_x = ray.dx; rh.ray.dir_y = ray.dy; rh.ray.dir_z = ray.dz;
        rh.ray.tnear = ray.tMin;
        rh.ray.tfar = ray.tMax;
        rh.ray.mask = static_cast<unsigned>(-1);
        rh.ray.flags = 0;
        rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        rh.hit.primID = RTC_INVALID_GEOMETRY_ID;
        rtcIntersect1(scene_, &rh);

        // Single-sided cull: accept the closest hit only if it is a front face
        // (dot(normal, dir) < 0). The vendored embree does not cull back faces
        // itself, so we apply the semantics here.
        if (rh.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            const std::uint32_t p = rh.hit.primID;
            if (p < vertex_index_count_ &&
                front_facing(&vertices_[static_cast<size_t>(indices_[p*3u])*3u],
                             &vertices_[static_cast<size_t>(indices_[p*3u+1u])*3u],
                             &vertices_[static_cast<size_t>(indices_[p*3u+2u])*3u],
                             ray)) {
                out.hit = true;
                out.t = rh.ray.tfar;
                out.primitiveIndex = static_cast<int32_t>(p);
            }
        }
        return out;
    }

    bool occluded(const RayTracerRay& ray) const override {
        if (scene_ == nullptr) return false;
        RTCRay r{};
        r.org_x = ray.ox; r.org_y = ray.oy; r.org_z = ray.oz;
        r.dir_x = ray.dx; r.dir_y = ray.dy; r.dir_z = ray.dz;
        r.tnear = ray.tMin;
        r.tfar = ray.tMax;
        r.mask = static_cast<unsigned>(-1);
        r.flags = 0;
        rtcOccluded1(scene_, &r);
        // rtcOccluded1 sinaliza occlusion com tfar NEGATIVO (RTCRay não tem geomID).
        // Single-sided: exclude back-face hits, so run closestHit and reuse it.
        return closestHit(ray).hit;
    }

private:
    void teardown() {
        if (geometry_) { rtcReleaseGeometry(geometry_); geometry_ = nullptr; }
        if (scene_) { rtcReleaseScene(scene_); scene_ = nullptr; }
        if (device_) { rtcReleaseDevice(device_); device_ = nullptr; }
        vertices_.clear();
        indices_.clear();
    }

    RTCDevice device_ = nullptr;
    RTCScene scene_ = nullptr;
    RTCGeometry geometry_ = nullptr;
    std::vector<float> vertices_;
    std::vector<std::uint32_t> indices_;
    std::uint32_t vertex_index_count_ = 0;  // number of triangles
};

}  // namespace

// Factory do SDK (registrada no vc_sdk_public; self-contained, all-or-nothing).
std::unique_ptr<IRayTracer> create_ray_tracer() {
    return std::make_unique<EmbreeRayTracerImpl>();
}

}  // namespace vc::rendering

#pragma once

// IRayBakeMesh — Agente 1 (task_plan C.4 embree — NOVO, caminho de baking no
// cooker criado do zero). Provides deterministic CPU baking of a simple
// ambient-occlusion / "distance to mesh" probe over a grid or vertex set using
// the engine's native IRayTracer (Embree backend already present). Closes the
// "baking/ray queries offline no cooker" clause without depending on more of
// the vendor's surface area.
//
#include "engine/rendering/IRayTracer.hpp"

// SCOPE (headless, deterministic):
//   configure  — bake resolution and sample-count config (all-or-nothing);
//   bakeVertices — for each query point cast `samples` stratified hemisphere
//                rays (uniform cosine lobe, seeded) plus a straight downward
//                occlusion probe; returns an AO-like scalar in [0,1] per point
//                (1 = fully open, lower = occluded) and the mean hit distance.
// Deterministic: same mesh+config+seed reproduces bit-exact results.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vc::rendering {

struct RayBakeConfig {
    std::uint32_t samples{ 16 };   // hemisphere rays per point [4, 512] (must divide 32? no)
    float maxDistance{ 32.0f };    // ray length past which we treat as open [1, 4096]
    std::uint32_t seed{ 1 };       // deterministic ray seed

    bool valid(std::string& errorOut) const;
};

struct RayBakeSample {
    float occlusion{ 1.0f };  // 1 = open, 0 = fully occluded
    float meanDistance{ 0.0f };  // average hit distance of occluding rays
};

class IRayBakeMesh {
public:
    virtual ~IRayBakeMesh() = default;

    virtual bool configure(const RayBakeConfig& config, std::string& errorOut) = 0;
    virtual const RayBakeConfig& config() const noexcept = 0;
    virtual bool configure_json(const std::string& jsonText, std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Build the acceleration structure once from `tris`. All-or-nothing:
    // null/empty rejects and clears the bake state.
    virtual bool build(const RayTracerTriangle* tris, std::int32_t count,
                       std::string& errorOut) = 0;

    // Deterministic hemisphere AO over each `origin` (hemisphere up = +Y,
    // geometrically correct only for +Y-facing surfaces). Returns false
    // (output untouched) when the scene is not built or inputs mismatch.
    virtual bool bake(const float* origins, std::size_t points,
                      std::vector<RayBakeSample>& output,
                      std::string& errorOut) const = 0;

    // Same as bake(), but the hemisphere follows the per-point NORMAL: a
    // tangent/bitangent frame is built per sample from the supplied normals,
    // so wall/sloped surfaces bake the correct hemisphere instead of always
    // shooting up. normals is a flat xyz array with 3*points floats, must be
    // unit-length (non-unit normals are normalized internally).
    virtual bool bake_normals(const float* origins, const float* normals,
                              std::size_t points,
                              std::vector<RayBakeSample>& output,
                              std::string& errorOut) const = 0;
};

// ---- public factory ----

std::unique_ptr<IRayBakeMesh> create_ray_bake_mesh(std::string& errorOut);

}  // namespace vc::rendering
#pragma once

// ITetraMeshCooking (FALTANTES §16 item 4): cooks TETRAHEDRAL simulation
// meshes while keeping the THREE meshes of a deformable SEPARATE:
//
//   simulation — a tetrahedral mesh (nodes + tetrahedra) for volumetric
//                solvers (XPBD volumetric / FEM). Every solid voxel becomes
//                the canonical 6-tet decomposition of its cube (all six
//                tetrahedra share the min->max body diagonal), so adjacent
//                voxels are WATER-TIGHT (shared faces split on the same
//                physical diagonal) and the node set is deduplicated by grid
//                position.
//   collider   — a conservative PHYSICS proxy: the voxel path emits per-voxel
//                box triangles (12 per voxel, internal faces included — the
//                classic voxel collider); the triangle-mesh path emits the
//                outer surface of the voxelized solid.
//   render     — the VISUAL mesh: the input triangles verbatim (triangle-mesh
//                path) or the deduplicated exposed surface (voxel path).
//
// fTetWild is the specialized OFFLINE cooker (DEPENDENCY_POLICY: cooker tools
// stay out of the runtime base); this adapter implements the same voxel-family
// tetrahedralization deterministically and headlessly — the output feeds the
// deformable providers (item 3) and the destruction pipeline.
//
// Deterministic: fixed voxel scan order (y,z,x), node dedup by grid position
// (first insertion wins), no randomness — identical inputs produce
// bit-identical meshes (item 5 documents the guarantee).

#include "engine/voxel/IVoxelWorld.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Deformable {

struct TetraCookingConfig {
    std::size_t maxTets{ 1000000 };    // simulation tet cap (refused above)
    std::uint32_t solidBlockFilter{ 0 };  // voxel path: 0 = any solid block

    // JSON keys: maxTets / solidBlockFilter. Out-of-range values are REFUSED
    // with a diagnostic (never clamped), mirroring the other configs.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

// The cooked result: three independent meshes (the item's core requirement).
struct TetraCookedMesh {
    // Simulation (volumetric solver input).
    std::vector<glm::vec3> simNodes;    // unique grid corners
    std::vector<glm::ivec4> simTets;    // tetrahedra (4 node indices)

    // Collider (physics proxy — per-voxel boxes on the voxel path).
    std::vector<glm::vec3> colliderVertices;
    std::vector<glm::uvec3> colliderTriangles;

    // Render (visual — exposed surface on the voxel path, input on the
    // triangle-mesh path).
    std::vector<glm::vec3> renderVertices;
    std::vector<glm::uvec3> renderTriangles;

    bool valid() const noexcept {
        return !simNodes.empty() && !simTets.empty();
    }
};

class ITetraMeshCooking {
public:
    virtual ~ITetraMeshCooking() = default;
    virtual const TetraCookingConfig& config() const noexcept = 0;

    // Cooks a VOXEL region [minimum, maximum] (inclusive): every solid cell
    // (solidBlockFilter == 0 -> any block id != 0) becomes 6 tets. Refuses an
    // empty region (no solid cells) or one whose tet count exceeds maxTets.
    virtual TetraCookedMesh cook_voxel_region(engine::voxel::IVoxelWorld& world,
                                              const glm::ivec3& minimum,
                                              const glm::ivec3& maximum,
                                              std::string& errorOut) = 0;

    // Cooks a CLOSED triangle mesh: voxelizes the interior (even-odd point-in-
    // mesh per voxel center over the mesh AABB), tetrahedralizes the inside
    // voxels, and keeps the input triangles as the render mesh. Refuses an
    // empty/over-cap result.
    virtual TetraCookedMesh cook_triangle_mesh(
        const std::vector<glm::vec3>& vertices,
        const std::vector<glm::uvec3>& triangles,
        std::string& errorOut) = 0;
};

// The single seam; Xpbd-style self-contained adapter (src/engine/sdk/
// TetraMeshCooking.cpp is the only TU that crosses into the tetrahedralizer).
std::unique_ptr<ITetraMeshCooking> create_tetra_mesh_cooking(
    const TetraCookingConfig& config, std::string& errorOut);

}  // namespace Engine::Deformable

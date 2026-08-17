// TetraCookingTests.cpp
//
// Evidence for FALTANTES §16 items 4 + 5: cooking tetrahedral SIMULATION
// meshes while keeping the THREE meshes separate (collider / render /
// simulation), deterministically and headlessly (fTetWild stays a specialized
// offline cooker — the adapter implements the voxel family itself):
//   - voxel region: a 2x2x2 solid box cooks to 8 voxels x 6 tets = 48 tets
//     with 27 deduplicated nodes (water-tight canonical decomposition);
//     the collider is the per-voxel box soup (96 triangles, internal faces
//     kept) and the render is the deduplicated exposed surface (48
//     triangles, 26 vertices) — three genuinely different meshes;
//   - triangle mesh: a closed 2x1x1 box voxelizes to 2 inside voxels -> 12
//     tets; the render is the INPUT triangles verbatim (12) and the collider
//     is the voxelized outer surface (20 triangles) — separation proven with
//     different content;
//   - validation: bad config / empty region / no solid / over-cap / empty
//     mesh / nothing-inside refused;
//   - determinism (item 5): identical cooks -> bit-identical meshes, across
//     instances.
//
// The per-provider determinism guarantees are documented in
// docs/DETERMINISMO_PROVIDERS.md (item 5).

#include <engine/deformable/ITetraMeshCooking.hpp>
#include <engine/voxel/IVoxelWorld.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Engine::Deformable;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

class FlatGenerator final : public engine::voxel::IVoxelGenerator {
public:
    explicit FlatGenerator(int height) : height_(height) {}
    engine::voxel::TerrainPoint sample(float, float) const override {
        engine::voxel::TerrainPoint point;
        point.height = height_;
        point.temperature = 0.5f;
        point.moisture = 0.5f;
        point.slope = 0.0f;
        return point;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }

private:
    int height_;
};

bool boot_world(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
                int budget, int maxBudgetMs = 8000) {
    world.set_chunk_budget(budget);
    const auto start = std::chrono::steady_clock::now();
    while (!world.is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxBudgetMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// A closed 2x1x1 box triangle mesh (12 triangles, 8 vertices).
void make_box_mesh(std::vector<glm::vec3>& vertices,
                   std::vector<glm::uvec3>& triangles) {
    const glm::vec3 v[8] = { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 1, 0 }, { 0, 1, 0 },
                             { 0, 0, 1 }, { 2, 0, 1 }, { 2, 1, 1 }, { 0, 1, 1 } };
    vertices.assign(v, v + 8);
    // 2 triangles per face (outward winding irrelevant to the even-odd test).
    const glm::uvec3 t[12] = {
        { 0, 1, 2 }, { 0, 2, 3 },  // -z
        { 4, 6, 5 }, { 4, 7, 6 },  // +z
        { 0, 4, 5 }, { 0, 5, 1 },  // -y
        { 3, 2, 6 }, { 3, 6, 7 },  // +y
        { 0, 3, 7 }, { 0, 7, 4 },  // -x
        { 1, 5, 6 }, { 1, 6, 2 },  // +x
    };
    triangles.assign(t, t + 12);
}

bool meshes_identical(const TetraCookedMesh& a, const TetraCookedMesh& b) {
    if (a.simNodes.size() != b.simNodes.size() ||
        a.simTets.size() != b.simTets.size() ||
        a.colliderVertices.size() != b.colliderVertices.size() ||
        a.colliderTriangles.size() != b.colliderTriangles.size() ||
        a.renderVertices.size() != b.renderVertices.size() ||
        a.renderTriangles.size() != b.renderTriangles.size())
        return false;
    for (std::size_t i = 0; i < a.simNodes.size(); ++i)
        if (a.simNodes[i] != b.simNodes[i]) return false;
    for (std::size_t i = 0; i < a.simTets.size(); ++i)
        if (a.simTets[i] != b.simTets[i]) return false;
    for (std::size_t i = 0; i < a.colliderVertices.size(); ++i)
        if (a.colliderVertices[i] != b.colliderVertices[i]) return false;
    for (std::size_t i = 0; i < a.colliderTriangles.size(); ++i)
        if (a.colliderTriangles[i] != b.colliderTriangles[i]) return false;
    for (std::size_t i = 0; i < a.renderVertices.size(); ++i)
        if (a.renderVertices[i] != b.renderVertices[i]) return false;
    for (std::size_t i = 0; i < a.renderTriangles.size(); ++i)
        if (a.renderTriangles[i] != b.renderTriangles[i]) return false;
    return true;
}

// 1. Voxel region: 2x2x2 solid box (air gap above the ground) -> 48 tets,
//    27 nodes, per-voxel-box collider (96), exposed-surface render (48/26) —
//    three genuinely separate meshes, water-tight and deterministic.
void test_voxel_region() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(130));
    check(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots (flat 130)");
    // 2x2x2 stone box with an air gap below it (y=132 air, box at y=133..134).
    for (int x = 8; x <= 9; ++x)
        for (int z = 8; z <= 9; ++z)
            for (int y = 133; y <= 134; ++y) world->set_block(x, y, z, 3);

    TetraCookingConfig config;
    std::string err;
    std::unique_ptr<ITetraMeshCooking> cooker =
        create_tetra_mesh_cooking(config, err);
    check(cooker != nullptr, "tetra cooker created");

    std::string error;
    const TetraCookedMesh mesh =
        cooker->cook_voxel_region(*world, glm::ivec3(8, 133, 8),
                                  glm::ivec3(9, 134, 9), error);
    check(mesh.valid() && error.empty(), "voxel region cooked");
    check(mesh.simTets.size() == 48, "8 voxels x 6 tets = 48 tetrahedra");
    check(mesh.simNodes.size() == 27,
          "27 deduplicated nodes (3x3x3 grid corners)");
    check(mesh.colliderTriangles.size() == 96,
          "collider = per-voxel boxes (8 x 12 triangles, internal faces kept)");
    check(mesh.colliderVertices.size() == 64,
          "collider vertices = per-voxel corners (8 voxels x 8, soup not deduped)");
    check(mesh.renderTriangles.size() == 48,
          "render = exposed surface (24 faces x 2)");
    check(mesh.renderVertices.size() == 26,
          "render vertices = 27 surface corners minus the 1 interior corner");

    // Separation: the three meshes are genuinely different.
    check(mesh.simNodes != mesh.renderVertices && mesh.simNodes.size() == 27 &&
              mesh.renderVertices.size() == 26,
          "simulation mesh differs from the render mesh");
    check(mesh.colliderTriangles.size() != mesh.renderTriangles.size(),
          "collider (per-voxel boxes) differs from the render (surface)");

    // Water-tightness: every tet has positive volume (no degenerate/duplicated
    // topology from the shared-diagonal decomposition).
    bool allPositive = true;
    for (const glm::ivec4& tet : mesh.simTets) {
        const glm::vec3& a = mesh.simNodes[tet.x];
        const glm::vec3& b = mesh.simNodes[tet.y];
        const glm::vec3& c = mesh.simNodes[tet.z];
        const glm::vec3& d = mesh.simNodes[tet.w];
        const float volume = glm::dot(b - a, glm::cross(c - a, d - a)) / 6.0f;
        if (!(volume > 1.0e-6f)) allPositive = false;
    }
    check(allPositive, "every tetrahedron has positive volume (no degenerates)");

    // Determinism (item 5): the same cook is bit-identical, and identical
    // across a SECOND cooker instance.
    const TetraCookedMesh again = cooker->cook_voxel_region(
        *world, glm::ivec3(8, 133, 8), glm::ivec3(9, 134, 9), error);
    check(meshes_identical(mesh, again), "voxel cook bit-identical (same instance)");
    std::unique_ptr<ITetraMeshCooking> second =
        create_tetra_mesh_cooking(config, err);
    const TetraCookedMesh cross = second->cook_voxel_region(
        *world, glm::ivec3(8, 133, 8), glm::ivec3(9, 134, 9), error);
    check(meshes_identical(mesh, cross),
          "voxel cook bit-identical across instances (item 5)");
    std::printf("[tetra] voxel region: 48 tets / 27 nodes / collider 96 / render 48+26, water-tight, deterministic OK\n");
}

// 2. Triangle mesh: a closed 2x1x1 box -> 2 inside voxels -> 12 tets; the
//    render keeps the INPUT triangles verbatim, the collider is the
//    voxelized outer surface (20 triangles) — separation with different
//    content.
void test_triangle_mesh() {
    std::vector<glm::vec3> vertices;
    std::vector<glm::uvec3> triangles;
    make_box_mesh(vertices, triangles);

    TetraCookingConfig config;
    std::string err;
    std::unique_ptr<ITetraMeshCooking> cooker =
        create_tetra_mesh_cooking(config, err);
    check(cooker != nullptr, "tetra cooker created");

    std::string error;
    const TetraCookedMesh mesh =
        cooker->cook_triangle_mesh(vertices, triangles, error);
    check(mesh.valid() && error.empty(), "triangle mesh cooked");
    check(mesh.simTets.size() == 12, "2 inside voxels x 6 = 12 tetrahedra");
    check(mesh.simNodes.size() == 12,
          "12 deduplicated nodes (3x2x2 grid corners)");
    check(mesh.renderTriangles.size() == 12 && mesh.renderVertices.size() == 8,
          "render = the INPUT triangles verbatim (12 / 8)");
    check(mesh.colliderTriangles.size() == 20,
          "collider = voxelized outer surface (10 exposed faces x 2)");

    // Separation with DIFFERENT content: render (input box, 12) vs collider
    // (voxel surface, 20) vs simulation (tets, 12 with 12 nodes).
    check(mesh.renderTriangles.size() != mesh.colliderTriangles.size() &&
              mesh.renderVertices.size() != mesh.colliderVertices.size() &&
              mesh.simNodes.size() != mesh.renderVertices.size(),
          "the three meshes carry different content (separation proven)");

    // Determinism (item 5).
    const TetraCookedMesh again = cooker->cook_triangle_mesh(vertices, triangles, error);
    check(meshes_identical(mesh, again),
          "triangle-mesh cook bit-identical (same instance)");
    std::printf("[tetra] triangle mesh: 12 tets / render=input 12 / collider 20, separated + deterministic OK\n");
}

// 3. Validation: bad config / empty region / no solid / over-cap / empty
//    mesh / nothing-inside refused (all-or-nothing, never clamped).
void test_validation() {
    TetraCookingConfig config;
    std::string error;
    check(!config.load_from_json(R"({"maxTets":0})", error),
          "maxTets 0 refused");
    check(!config.load_from_json(R"({"maxTets":20000000})", error),
          "oversized maxTets refused");
    TetraCookingConfig valid;
    check(valid.load_from_json(R"({"maxTets":64,"solidBlockFilter":3})", error) &&
              valid.maxTets == 64 && valid.solidBlockFilter == 3,
          "valid config loads and round-trips");

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(130));
    boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 2);

    TetraCookingConfig cfg;
    std::string err;
    std::unique_ptr<ITetraMeshCooking> cooker = create_tetra_mesh_cooking(cfg, err);
    TetraCookedMesh mesh =
        cooker->cook_voxel_region(*world, glm::ivec3(5, 5, 5), glm::ivec3(4, 5, 5),
                                  error);
    check(!mesh.valid(), "empty region (min > max) refused");
    mesh = cooker->cook_voxel_region(*world, glm::ivec3(5, 200, 5),
                                     glm::ivec3(6, 201, 6), error);
    check(!mesh.valid(), "region with no solid cells refused");
    for (int x = 8; x <= 9; ++x)
        for (int z = 8; z <= 9; ++z)
            for (int y = 133; y <= 134; ++y) world->set_block(x, y, z, 3);
    TetraCookingConfig tiny;
    tiny.maxTets = 6;
    std::unique_ptr<ITetraMeshCooking> capped = create_tetra_mesh_cooking(tiny, err);
    mesh = capped->cook_voxel_region(*world, glm::ivec3(8, 133, 8),
                                     glm::ivec3(9, 134, 9), error);
    check(!mesh.valid(), "tet count over maxTets refused (never clamped)");

    std::vector<glm::vec3> vertices;
    std::vector<glm::uvec3> triangles;
    check(!cooker->cook_triangle_mesh(vertices, triangles, error).valid(),
          "empty triangle mesh refused");
    // Degenerate mesh (3 collinear triangles): nothing inside.
    vertices = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                 glm::vec3(2.0f, 0.0f, 0.0f) };
    triangles = { glm::uvec3(0, 1, 2) };
    check(!cooker->cook_triangle_mesh(vertices, triangles, error).valid(),
          "closed mesh with nothing inside refused");
    std::printf("[tetra] validation OK\n");
}

}  // namespace

int main() {
    test_voxel_region();
    test_triangle_mesh();
    test_validation();
    if (g_failures == 0) {
        std::printf("[tetra] ALL PASSED\n");
        return 0;
    }
    std::printf("[tetra] %d FAILURE(S)\n", g_failures);
    return 1;
}

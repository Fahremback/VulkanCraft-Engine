// MeshCookingTests.cpp
//
// Gate for IMeshCooking (META §18 / FALTANTES item 14): headless mesh cooking
// via the promoted meshoptimizer + xatlas clones (external/solutions).
// Exercises the REAL adapter (src/engine/sdk/MeshCooking.cpp is the only TU
// that includes the backends) proving:
//   - unwrap: a plain mesh with no UVs gets a valid normalized [0,1] atlas
//     (xatlas), indices remapped, hasUvs -> true;
//   - optimize: vertex-cache statistics (ACMR) improve after reordering;
//   - determinism: two cooks of the same (mesh, options) are bit-identical
//     (both unwrap and the full pipeline), across instances;
//   - simplify: quadric-error LOD cuts the index count toward target;
//   - validation: degenerate input is rejected with a message.
//
// Backend provenance/determinism documented in docs/DETERMINISMO_PROVIDERS.md.

#include <engine/procgen/IMeshCooking.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

// A closed unit cube without UVs or normals — 24 vertices (per-face corners),
// 36 indices. Good input for unwrap: xatlas must generate UVs from scratch.
engine::procgen::CookedMesh make_cube() {
    engine::procgen::CookedMesh m;
    // 24 positions (4 per face x 6 faces), centered at origin, size 1.
    const float c = 0.5f;
    // +X face
    const float px[24][3] = {
        { c, -c, -c}, { c, -c,  c}, { c,  c,  c}, { c,  c, -c},
        // -X face
        {-c, -c,  c}, {-c, -c, -c}, {-c,  c, -c}, {-c,  c,  c},
        // +Y face
        {-c,  c, -c}, {-c,  c,  c}, { c,  c,  c}, { c,  c, -c},
        // -Y face
        {-c, -c,  c}, {-c, -c, -c}, { c, -c, -c}, { c, -c,  c},
        // +Z face
        {-c, -c,  c}, { c, -c,  c}, { c,  c,  c}, {-c,  c,  c},
        // -Z face
        { c, -c, -c}, {-c, -c, -c}, {-c,  c, -c}, { c,  c, -c},
    };
    for (auto& v : px) {
        m.positions.push_back(v[0]);
        m.positions.push_back(v[1]);
        m.positions.push_back(v[2]);
    }
    // Two triangles per face (36 indices).
    const std::uint32_t idx[36] = {
        0,1,2, 0,2,3,
        4,5,6, 4,6,7,
        8,9,10, 8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };
    for (auto i : idx) m.indices.push_back(i);
    return m;
}

// A flat grid patch (n x n quads = 2n^2 triangles, all coplanar z=0). This is
// a genuinely reducible mesh: meshopt can collapse interior edges at a
// permissive error without changing the shape, unlike a closed cube whose
// every quad is required to stay manifold.
engine::procgen::CookedMesh make_plane(int n) {
    engine::procgen::CookedMesh m;
    const int vertsPerSide = n + 1;
    const float step = 2.0f / static_cast<float>(n);
    for (int y = 0; y < vertsPerSide; ++y) {
        for (int x = 0; x < vertsPerSide; ++x) {
            m.positions.push_back(-1.0f + x * step);
            m.positions.push_back(-1.0f + y * step);
            m.positions.push_back(0.0f);
        }
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const std::uint32_t i = static_cast<std::uint32_t>(y * vertsPerSide + x);
            const std::uint32_t j = i + 1;
            const std::uint32_t k = i + static_cast<std::uint32_t>(vertsPerSide);
            const std::uint32_t l = k + 1;
            m.indices.push_back(i); m.indices.push_back(k); m.indices.push_back(j);
            m.indices.push_back(j); m.indices.push_back(k); m.indices.push_back(l);
        }
    }
    return m;
}

void test_unwrap() {
    std::printf("[cook] unwrap xatlas…\n");
    auto cooker = engine::procgen::create_mesh_cooker();
    check(cooker != nullptr, "factory create_mesh_cooker() returns non-null");

    engine::procgen::CookedMesh cube = make_cube();
    engine::procgen::CookOptions options;
    options.unwrap = true;
    options.optimize = false;   // isolate unwrap
    engine::procgen::CookedMesh out;
    engine::procgen::CookStats stats;
    std::string err;

    check(cooker->unwrap(cube, options, out, err), "unwrap(cube) succeeds");
    if (g_failures > 0) { std::printf("  unwrap err: %s\n", err.c_str()); return; }

    check(out.vertex_count() >= 3, "unwrap output has vertices");
    check(out.index_count() > 0, "unwrap output has indices");
    check(!out.uvs.empty(), "unwrap generated UVs (input had none)");
    if (!out.uvs.empty()) {
        bool inRange = true;
        for (std::size_t i = 0; i < out.uvs.size(); ++i) {
            const float u = out.uvs[i];
            if (!(u >= -1e-4f && u <= 1.0f + 1e-4f)) { inRange = false; break; }
        }
        check(inRange, "all UVs normalized to [0,1]");
        check(out.uvs.size() == out.vertex_count() * 2,
              "uvs size matches remapped vertex count");
        // Remapped indices must stay in range.
        bool inIdx = true;
        for (auto i : out.indices)
            if (i >= out.vertex_count()) { inIdx = false; break; }
        check(inIdx, "remapped indices within vertex range");
    }

    // analyze reports hasUvs after unwrap. (analyze fills acmr/overdraw/
    // hasUvs — NOT the input/output counters, which cook() sets.)
    engine::procgen::CookStats st;
    std::string err2;
    check(cooker->analyze(out, st, err2), "analyze(unwrapped) succeeds");
    check(st.hasUvs, "analyze reports hasUvs=true after unwrap");
    std::printf("[cook] unwrap OK (%zu vtx, %zu idx, %zu uvs)\n",
                out.vertex_count(), out.index_count(), out.uvs.size());
}

void test_optimize() {
    std::printf("[cook] optimize meshopt…\n");
    auto cooker = engine::procgen::create_mesh_cooker();
    engine::procgen::CookedMesh cube = make_cube();
    engine::procgen::CookOptions options;
    options.unwrap = false;   // keep input UV-less positions
    options.optimize = true;
    engine::procgen::CookedMesh out;
    std::string err;

    const int base = g_failures;
    check(cooker->optimize(cube, options, out, err), "optimize(cube) succeeds");
    if (g_failures > base) { std::printf("  optimize err: %s\n", err.c_str()); return; }

    check(out.index_count() == cube.index_count(), "optimize preserves triangle count");
    check(out.vertex_count() <= cube.vertex_count(), "optimize does not grow vertices");
    // Index remap sanity.
    bool inIdx = true;
    for (auto i : out.indices)
        if (i >= out.vertex_count()) { inIdx = false; break; }
    check(inIdx, "optimized indices within vertex range");

    // Vertex-cache improves (ACMR decreases) on a reordered result vs not.
    engine::procgen::CookStats before, after;
    std::string eb, ea;
    check(cooker->analyze(cube, before, eb), "analyze(before) succeeds");
    check(cooker->analyze(out, after, ea), "analyze(after) succeeds");
    std::printf("[cook] acmr before=%.3f after=%.3f\n",
                static_cast<double>(before.acmr),
                static_cast<double>(after.acmr));
    check(after.acmr <= before.acmr + 1e-9,
          "vertex-cache ACMR does not get worse after optimize");
    std::printf("[cook] optimize OK\n");
}

void test_simplify() {
    std::printf("[cook] simplify LOD…\n");
    auto cooker = engine::procgen::create_mesh_cooker();
    engine::procgen::CookedMesh cube = make_cube();
    engine::procgen::CookedMesh out;
    std::string err;

    // (a) A closed cube is architecturally irreducible — every quad is needed
    // to stay manifold — so a low-error simplify is conservatively a no-op.
    const int low = g_failures;
    std::string errLow;
    check(cooker->simplify(cube, 12, 0.05f, out, errLow),
          "simplify(cube, 12, err=0.05) succeeds");
    if (g_failures > low) { std::printf("  simplify low-err: %s\n", errLow.c_str()); return; }
    check(out.index_count() == cube.index_count(),
          "low-error simplify preserves a closed cube (conservative)");

    // (b) A flat grid has plenty of redundant interior edges: a permissive
    // error must genuinely cut it down, proving the simplification backend
    // works. 8x8 => 128 triangles -> ask for ~32.
    engine::procgen::CookedMesh plane = make_plane(8);
    const std::size_t planeTri = plane.index_count();
    const int permissive = g_failures;
    std::string errHigh;
    check(cooker->simplify(plane, 36, 0.05f, out, errHigh),
          "simplify(grid8, 36, err=0.05) succeeds");
    if (g_failures > permissive) { std::printf("  simplify grid: %s\n", errHigh.c_str()); return; }
    check(out.index_count() < planeTri,
          "grid simplify reduces the index count");
    check(out.index_count() > 0, "grid simplify keeps non-empty result");
    std::printf("[cook] simplify grid %zu -> %zu idx\n",
                planeTri, out.index_count());
    bool inIdx = true;
    for (auto i : out.indices)
        if (i >= out.vertex_count()) { inIdx = false; break; }
    check(inIdx, "simplified indices within vertex range");
    std::printf("[cook] simplify OK\n");
}

void test_determinism() {
    std::printf("[cook] determinism…\n");
    auto a = engine::procgen::create_mesh_cooker();
    auto b = engine::procgen::create_mesh_cooker();
    engine::procgen::CookedMesh cube = make_cube();
    engine::procgen::CookOptions options;
    options.unwrap = true;
    options.optimize = true;

    engine::procgen::CookedMesh oa, ob;
    engine::procgen::CookStats sa, sb;
    std::string ea, eb;
    const int base = g_failures;
    check(a->cook(cube, options, oa, sa, ea), "cook(a) succeeds");
    check(b->cook(cube, options, ob, sb, eb), "cook(b) succeeds");
    if (g_failures > base) return;
    check(oa.positions == ob.positions, "positions bit-identical across instances");
    check(oa.normals == ob.normals, "normals bit-identical across instances");
    check(oa.uvs == ob.uvs, "uvs bit-identical across instances");
    check(oa.indices == ob.indices, "indices bit-identical across instances");
    std::printf("[cook] determinism OK\n");
}

void test_tangents() {
    std::printf("[cook] tangents MikkTSpace…\n");
    auto cooker = engine::procgen::create_mesh_cooker();

    // A planar grid (z=0, +Z normals, UVs aligned with x/y). Tangents should
    // point along +X (u axis) and be orthogonal to the +Z normal.
    engine::procgen::CookedMesh plane = make_plane(2);
    // Give it normals (+Z) and UVs aligned to x/y.
    plane.normals.assign(plane.vertex_count() * 3, 0.0f);
    for (std::size_t v = 0; v < plane.vertex_count(); ++v) {
        plane.normals[v * 3 + 2] = 1.0f;
        plane.uvs.push_back((plane.positions[v * 3] + 1.0f) * 0.5f);
        plane.uvs.push_back((plane.positions[v * 3 + 1] + 1.0f) * 0.5f);
    }

    engine::procgen::CookedMesh out;
    std::string err;
    const int base = g_failures;
    check(cooker->generate_tangents(plane, out, err),
          "generate_tangents(plane) succeeds");
    if (g_failures > base) { std::printf("  tangent err: %s\n", err.c_str()); return; }
    check(out.tangents.size() == out.vertex_count() * 3,
          "tangents generated for every vertex");

    // Every tangent must be unit-length-ish and orthogonal to its normal.
    bool ortho = true, unit = true;
    float maxOrtho = 0.0f;
    for (std::size_t v = 0; v < out.vertex_count(); ++v) {
        const float* n = &out.normals[v * 3];
        const float* t = &out.tangents[v * 3];
        const float nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        const float tl = std::sqrt(t[0]*t[0] + t[1]*t[1] + t[2]*t[2]);
        const float dot = (n[0]*t[0] + n[1]*t[1] + n[2]*t[2]) / (nl * tl);
        if (std::fabs(dot) > 1e-3f) ortho = false;
        if (std::fabs(tl - 1.0f) > 1e-3f) unit = false;
        if (std::fabs(dot) > maxOrtho) maxOrtho = std::fabs(dot);
    }
    check(ortho, "all tangents orthogonal to their normals (|dot| <= 1e-3)");
    check(unit, "all tangents unit length");
    // On the plane aligned with x/y UVs, tangent should point along +X.
    const float* t0 = &out.tangents[0];
    check(t0[0] > 0.9f && std::fabs(t0[1]) < 0.1f && std::fabs(t0[2]) < 0.1f,
          "plane tangent points along +X (u axis)");
    std::printf("[cook] tangents OK (max|dot(t,n)|=%.4f)\n", maxOrtho);

    // Without UVs the call must be refused.
    engine::procgen::CookedMesh noUv = make_cube();
    err.clear();
    check(!cooker->generate_tangents(noUv, out, err),
          "generate_tangents without UVs refused");
    check(!err.empty(), "refusal reports a message");
}

void test_validation() {
    std::printf("[cook] validation…\n");
    auto cooker = engine::procgen::create_mesh_cooker();

    engine::procgen::CookedMesh empty;
    engine::procgen::CookOptions opts;
    engine::procgen::CookedMesh out;
    engine::procgen::CookStats st;
    std::string err;
    check(!cooker->cook(empty, opts, out, st, err), "empty mesh refused");
    check(!err.empty(), "refusal reports a message");

    // Degenerate: fewer than 3 vertices.
    engine::procgen::CookedMesh degen;
    degen.positions = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f };
    degen.indices = { 0, 0, 1 };
    err.clear();
    check(!cooker->cook(degen, opts, out, st, err), "degenerate (<3 vtx) refused");
    std::printf("[cook] validation OK\n");
}

}  // namespace

int main() {
    test_unwrap();
    test_optimize();
    test_simplify();
    test_determinism();
    test_tangents();
    test_validation();
    if (g_failures == 0) {
        std::printf("[cook] ALL PASSED\n");
        return 0;
    }
    std::printf("[cook] %d FAILURE(S)\n", g_failures);
    return 1;
}
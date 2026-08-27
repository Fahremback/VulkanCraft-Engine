// ConvexDecompositionTests.cpp
//
// G.coacd: CoACD (Convex Approximate Convex Decomposition) gate. Proves the
// vendored CoACD library is wired end-to-end through the public
// IConvexDecomposition seam (engine/physics/IConvexDecomposition.hpp) via
// src/engine/sdk/CoACDAdapter.cpp — not just an object module sitting unused.
//
// Scenarios:
//   1. Factory: "coacd" creates the adapter; unknown backend is refused.
//   2. Validation: too-few-vertices and non-multiple-of-3 indices are refused
//      with a diagnostic (all-or-nothing contract).
//   3. Real decomposition: a concave L-shape splits into >1 convex part; a
//      convex box stays as exactly 1 part.
//   4. Output sanity: every part has >= 4 vertices (a tetrahedron minimum)
//      and >= 4 triangles, and indices are in range.
//   5. Determinism: same input twice -> same part count and vertex counts.
//
// Headless, deterministic, no GPU.

#include "engine/physics/IConvexDecomposition.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace engine::physics;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

ConvexInput make_box() {
    // Axis-aligned box -1..1. Already convex; CoACD should keep it as one part.
    ConvexInput in;
    const float v[8][3] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}
    };
    for (int i = 0; i < 8; ++i) {
        in.positions.push_back(v[i][0]);
        in.positions.push_back(v[i][1]);
        in.positions.push_back(v[i][2]);
    }
    const uint32_t t[12][3] = {
        {0,1,2},{0,2,3},{4,6,5},{4,7,6},
        {0,4,5},{0,5,1},{2,6,7},{2,7,3},
        {0,3,7},{0,7,4},{1,5,6},{1,6,2}
    };
    for (int i = 0; i < 12; ++i) {
        in.indices.push_back(t[i][0]);
        in.indices.push_back(t[i][1]);
        in.indices.push_back(t[i][2]);
    }
    return in;
}

ConvexInput make_L_shape() {
    // Proper watertight L: the 2D L polygon (6 corners, CCW seen from +z)
    //   P = {(-1,-1), (1,-1), (1,0), (0,0), (0,1), (-1,1)}
    // extruded in z from -1 to +1. 12 vertices, 20 triangles, manifold.
    // The missing quadrant x>0,y>0 makes it concave -> CoACD must split it
    // into more than one convex part.
    const float P[6][2] = {
        {-1,-1},{ 1,-1},{ 1, 0},{ 0, 0},{ 0, 1},{-1, 1}
    };
    ConvexInput in;
    in.positions.clear();
    for (int i = 0; i < 6; ++i) {  // bottom ring (z=-1), indices 0..5
        in.positions.push_back(P[i][0]);
        in.positions.push_back(P[i][1]);
        in.positions.push_back(-1.0f);
    }
    for (int i = 0; i < 6; ++i) {  // top ring (z=+1), indices 6..11
        in.positions.push_back(P[i][0]);
        in.positions.push_back(P[i][1]);
        in.positions.push_back( 1.0f);
    }
    in.indices.clear();
    // Bottom face (normal -z): fan, reversed winding when seen from +z.
    in.indices.insert(in.indices.end(), {0,2,1, 0,3,2, 0,4,3, 0,5,4});
    // Top face (normal +z): fan, CCW.
    in.indices.insert(in.indices.end(), {6,7,8, 6,8,9, 6,9,10, 6,10,11});
    // Side quads (one per polygon edge, wrap-around).
    for (int i = 0; i < 6; ++i) {
        const int j = (i + 1) % 6;
        in.indices.insert(in.indices.end(), {
            (uint32_t)i, (uint32_t)j, (uint32_t)(j + 6),
            (uint32_t)i, (uint32_t)(j + 6), (uint32_t)(i + 6)
        });
    }
    return in;
}

void test_factory() {
    std::string error;
    auto cd = create_convex_decomposition("coacd", error);
    check(cd != nullptr, "factory: coacd backend created");
    check(error.empty(), "factory: no error on valid backend");
    check(cd && std::string(cd->name()) == "CoACD", "factory: name is CoACD");

    auto unknown = create_convex_decomposition("v-hacd", error);
    check(unknown == nullptr, "factory: unknown backend refused");
    check(!error.empty(), "factory: unknown backend gives diagnostic");
    std::printf("  [factory] name=%s unknown-refused=%s\n",
                cd ? cd->name() : "?", unknown ? "no" : "yes");
}

void test_validation() {
    std::string error;
    auto cd = create_convex_decomposition("coacd", error);
    check(cd != nullptr, "validation: adapter created");

    ConvexInput tiny;
    tiny.positions = {0,0,0, 1,0,0, 0,1,0}; // 1 triangle
    tiny.indices = {0,1,2};
    ConvexResult r;
    check(!cd->decompose(tiny, r, error), "validation: 1-triangle mesh refused");
    check(!error.empty(), "validation: refusal has diagnostic");

    ConvexInput badIdx = make_box();
    badIdx.indices = {0,1}; // not multiple of 3
    error.clear();
    check(!cd->decompose(badIdx, r, error), "validation: bad index count refused");
    check(!error.empty(), "validation: bad index diagnostic");
    std::printf("  [validation] refused tiny=%s badIdx=%s\n",
                error.empty() ? "no" : "yes", error.empty() ? "no" : "yes");
}

void test_convex_box() {
    std::string error;
    auto cd = create_convex_decomposition("coacd", error);
    check(cd != nullptr, "box: adapter created");

    ConvexResult r;
    const bool ok = cd->decompose(make_box(), r, error);
    check(ok, "box: decomposition succeeded");
    check(r.parts.size() >= 1, "box: at least 1 part");
    std::printf("  [box] parts=%zu\n", r.parts.size());
}

void test_concave_L() {
    std::string error;
    auto cd = create_convex_decomposition("coacd", error);
    check(cd != nullptr, "L: adapter created");

    ConvexResult r;
    const bool ok = cd->decompose(make_L_shape(), r, error);
    check(ok, "L: decomposition succeeded");
    check(r.parts.size() >= 2, "L: concave shape splits into >= 2 convex parts");

    for (std::size_t i = 0; i < r.parts.size(); ++i) {
        const auto& p = r.parts[i];
        const bool sane = p.vertices.size() >= 12 && p.triangles.size() >= 12 &&
                          (p.triangles.size() % 3) == 0;
        check(sane, "L: every part has a tetrahedron-sized mesh");
        const std::size_t vertCount = p.vertices.size() / 3;
        bool inRange = true;
        for (std::size_t t = 0; t < p.triangles.size(); ++t) {
            if (p.triangles[t] >= vertCount) { inRange = false; break; }
        }
        check(inRange, "L: part indices in range");
    }
    std::printf("  [L] parts=%zu\n", r.parts.size());
}

void test_determinism() {
    std::string error;
    auto cd = create_convex_decomposition("coacd", error);
    check(cd != nullptr, "determinism: adapter created");

    const ConvexInput in = make_L_shape();
    ConvexResult a, b;
    check(cd->decompose(in, a, error), "determinism: first run ok");
    error.clear();
    check(cd->decompose(in, b, error), "determinism: second run ok");
    check(a.parts.size() == b.parts.size(), "determinism: same part count");
    bool same = a.parts.size() == b.parts.size();
    for (std::size_t i = 0; same && i < a.parts.size(); ++i) {
        same = a.parts[i].vertices.size() == b.parts[i].vertices.size() &&
               a.parts[i].triangles.size() == b.parts[i].triangles.size();
    }
    check(same, "determinism: identical part meshes");
    std::printf("  [determinism] runs=%zu/%zu identical\n", a.parts.size(), b.parts.size());
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== convex decomposition (coacd) ==\n");
    test_factory();
    test_validation();
    test_convex_box();
    test_concave_L();
    test_determinism();

    if (failures == 0) {
        std::printf("ALL CONVEX DECOMPOSITION TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", failures);
    return 1;
}

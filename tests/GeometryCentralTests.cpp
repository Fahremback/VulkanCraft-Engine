// GeometryCentralTests.cpp - Gate for G.geometry-central (vendored repo).
//
// Exercises the REAL vendored library (external/solutions/geometry-central)
// directly — SurfaceMesh construction, manifold/orientation checks, and the
// heat-method geodesic distance solver. This validates the vendored repo
// compiles and works on MSVC 2026. (An engine-side adapter behind
// IMeshGeometryProcessing, if desired, is a separate task.)
//
// Exit codes: 0 = all pass, 1 = any failure. Prints per-assert PASS/FAIL.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

#include "geometrycentral/surface/surface_mesh.h"
#include "geometrycentral/surface/surface_mesh_factories.h"
#include "geometrycentral/surface/vertex_position_geometry.h"
#include "geometrycentral/surface/heat_method_distance.h"
#include "geometrycentral/surface/intrinsic_geometry_interface.h"

using namespace geometrycentral;
using namespace geometrycentral::surface;

static int g_failures = 0;

static void check(bool ok, const std::string& name)
{
    if (ok) { std::printf("PASS: %s\n", name.c_str()); }
    else    { std::printf("FAIL: %s\n", name.c_str()); ++g_failures; }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0)
    {
        std::printf("GeometryCentralTests vendored geometry-central gate v1\n");
        return 0;
    }

    std::printf("Geometry-central gate (vendored repo)\n");

    // Tetrahedron: 4 vertices, 4 triangular faces (outward-oriented).
    std::vector<std::vector<size_t>> polygons = {
        {0, 2, 1},
        {0, 1, 3},
        {1, 2, 3},
        {0, 3, 2},
    };
    std::vector<Vector3> positions = {
        Vector3(0.0, 0.0, 0.0),
        Vector3(1.0, 0.0, 0.0),
        Vector3(0.0, 1.0, 0.0),
        Vector3(0.0, 0.0, 1.0),
    };

    auto result = makeSurfaceMeshAndGeometry(polygons, positions);
    std::unique_ptr<SurfaceMesh>& mesh = std::get<0>(result);
    std::unique_ptr<VertexPositionGeometry>& geom = std::get<1>(result);

    check(mesh->nVertices() == 4, "mesh: 4 vertices");
    check(mesh->nFaces() == 4, "mesh: 4 faces");
    check(mesh->nEdges() == 6, "mesh: 6 edges (tetra)");
    check(mesh->isManifold(), "mesh: is manifold");
    check(mesh->isOriented(), "mesh: is oriented");

    // Closed surface -> no boundary edges.
    bool hasBoundary = false;
    for (Edge e : mesh->edges())
    {
        if (e.isBoundary()) { hasBoundary = true; break; }
    }
    check(!hasBoundary, "mesh: closed (no boundary edges)");

    // Heat method geodesic distance from vertex 0.
    HeatMethodDistanceSolver solver(*geom);
    VertexData<double> dist = solver.computeDistance(mesh->vertex(0));

    double d0 = dist[mesh->vertex(0)];
    check(std::fabs(d0) < 1e-8, "heat: distance at source == 0 (" + std::to_string(d0) + ")");

    bool allFinite = true;
    bool allNonNeg = true;
    for (Vertex v : mesh->vertices())
    {
        double d = dist[v];
        if (!std::isfinite(d)) allFinite = false;
        if (d < 0.0) allNonNeg = false;
    }
    check(allFinite, "heat: all distances finite");
    check(allNonNeg, "heat: all distances non-negative");

    // Vertex 1 is distance 1.0 away (unit edge). Heat method slightly
    // overestimates geodesics; bound it to a plausible range.
    double d1 = dist[mesh->vertex(1)];
    check(d1 > 0.0 && d1 <= 1.5,
        "heat: neighbor distance in plausible range (" + std::to_string(d1) + " in (0, 1.5])");

    // Longest edge in tetra is sqrt(2) ~1.414 (e.g. v1-v3). Distance from v0
    // to v3 cannot exceed the Euclidean straight-line length.
    double d3 = dist[mesh->vertex(3)];
    check(d3 <= std::sqrt(2.0) + 1e-6,
        "heat: far vertex bounded by Euclidean length (" + std::to_string(d3) + " <= " + std::to_string(std::sqrt(2.0)) + ")");

    if (g_failures == 0) { std::printf("ALL GEOMETRY-CENTRAL GATE TESTS PASSED\n"); return 0; }
    std::printf("%d GEOMETRY-CENTRAL GATE TEST(S) FAILED\n", g_failures);
    return 1;
}

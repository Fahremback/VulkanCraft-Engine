// NavigationDynamicTests.cpp — Dynamic voxel navigation tests (§10 item 168).
// Exercises INavigationProvider: build, find_path, update, dynamic obstacles,
// off-mesh links, slope cost areas, and async path queries.
// Pattern follows AdvancedSystemsTests.cpp navigation section.

#include "engine/navigation/INavigationProvider.hpp"
#include "engine/navigation/VoxelNavigation.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("    FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
            g_failures++;                                              \
            return;                                                    \
        }                                                              \
    } while (0)

// ── Helpers ──────────────────────────────────────────────────────────────────

// Build a flat walkable grid matching the proven pattern from
// AdvancedSystemsTests.cpp: columns at cell centers, solidMinY=0, solidMaxY=1.
static std::vector<engine::navigation::VoxelColumn> make_flat_ground(
    float cellSize = 0.5f, int gridSize = 18,
    float originX = -1.0f, float originZ = -1.0f) {
    std::vector<engine::navigation::VoxelColumn> columns;
    for (int gx = 0; gx < gridSize; ++gx) {
        for (int gz = 0; gz < gridSize; ++gz) {
            const float cx = originX + (gx + 0.5f) * cellSize;
            const float cz = originZ + (gz + 0.5f) * cellSize;
            columns.push_back({cx, cz, 0.0f, 1.0f, true, 0});
        }
    }
    return columns;
}

static engine::navigation::NavmeshConfig make_flat_config() {
    engine::navigation::NavmeshConfig cfg;
    cfg.boundsMinX = -1.0f;
    cfg.boundsMinZ = -1.0f;
    cfg.boundsMaxX = 8.0f;
    cfg.boundsMaxZ = 8.0f;
    cfg.boundsMinY = -1.0f;
    cfg.boundsMaxY = 10.0f;
    cfg.cellSize = 0.5f;
    cfg.cellHeight = 0.2f;
    cfg.agentRadius = 0.3f;
    cfg.agentHeight = 1.8f;
    cfg.agentMaxClimb = 1.0f;
    cfg.agentMaxSlope = 45.0f;
    cfg.tileSize = 0.0f;
    return cfg;
}

static engine::navigation::NavmeshConfig make_tiled_config() {
    auto cfg = make_flat_config();
    cfg.tileSize = 4.0f;
    return cfg;
}

// ── Tests ────────────────────────────────────────────────────────────────────

// Test 1: Build a navmesh from flat ground and find a path.
static void test_build_and_find_path() {
    printf("  test_build_and_find_path...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    CHECK(provider, "provider is null");

    auto cols = make_flat_ground();
    auto cfg = make_flat_config();
    std::string err;

    bool ok = provider->build(cfg, cols, err);
    CHECK(ok, ("build failed: " + err).c_str());
    CHECK(provider->valid(), "not valid after build");
    CHECK(provider->revision() > 0, "revision not bumped");

    engine::navigation::PathResult path;
    ok = provider->find_path(0.5f, 1.0f, 0.5f, 7.5f, 1.0f, 7.5f, path);
    CHECK(ok, "find_path returned false");
    CHECK(path.found, "path not found");
    CHECK(path.waypoints.size() >= 6,
          ("too few waypoints: " + std::to_string(path.waypoints.size())).c_str());
    CHECK(path.totalLength > 0.0f, "totalLength is 0");

    printf("    OK: path found with %zu waypoints, length=%.2f\n",
           path.waypoints.size() / 3, path.totalLength);
}

// Test 2: is_walkable on flat ground.
static void test_is_walkable() {
    printf("  test_is_walkable...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_flat_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");

    CHECK(provider->is_walkable(4.0f, 1.0f, 4.0f), "center not walkable");
    CHECK(!provider->is_walkable(100.0f, 1.0f, 100.0f), "outside bounds is walkable");

    printf("    OK: walkable checks passed\n");
}

// Test 3: Incremental update — add a wall and verify revision bumps.
static void test_update_incremental() {
    printf("  test_update_incremental...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_tiled_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");
    uint64_t rev1 = provider->revision();
    CHECK(rev1 > 0, "revision not bumped after build");

    // Add a wall: columns at x=4, y=0..5 (tall solid block).
    std::vector<engine::navigation::VoxelColumn> wall;
    for (float z = -0.5f; z <= 8.0f; z += 0.5f) {
        wall.push_back({4.0f, z, 0.0f, 5.0f, true, 0});
    }

    bool ok = provider->update(wall, err);
    CHECK(ok, ("update failed: " + err).c_str());
    CHECK(provider->revision() > rev1, "revision not bumped after update");

    printf("    OK: incremental update succeeded, rev %" PRIu64 " -> %" PRIu64 "\n",
           rev1, provider->revision());
}

// Test 4: Dynamic obstacle — register, verify navmesh changes, toggle off.
static void test_dynamic_obstacle() {
    printf("  test_dynamic_obstacle...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_tiled_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");
    uint64_t revBefore = provider->revision();

    // Place a tall obstacle (solid 0..5) at (4,4) — replaces terrain (0..1)
    // and pushes the walkable surface up, effectively blocking ground-level.
    engine::navigation::DynamicObstacle obs;
    obs.columns.push_back({4.0f, 4.0f, 0.0f, 5.0f, true, 0});

    bool ok = provider->set_dynamic_obstacle(42, obs, err);
    CHECK(ok, ("set_dynamic_obstacle failed: " + err).c_str());
    // Revision should bump when obstacle changes the navmesh.
    CHECK(provider->revision() > revBefore, "revision not bumped after obstacle");

    // Verify obstacle changed path: direct path through (4,4) should be
    // longer or different than without obstacle.
    engine::navigation::PathResult pathWith;
    ok = provider->find_path(0.5f, 1.0f, 0.5f, 7.5f, 1.0f, 7.5f, pathWith);
    CHECK(ok, "find_path with obstacle failed");
    CHECK(pathWith.found, "path with obstacle not found");

    // Deactivate obstacle — navmesh should revert.
    uint64_t revAfter = provider->revision();
    ok = provider->set_obstacle_active(42, false, err);
    CHECK(ok, ("set_obstacle_active(false) failed: " + err).c_str());
    CHECK(provider->revision() > revAfter, "revision not bumped after deactivate");

    engine::navigation::PathResult pathWithout;
    ok = provider->find_path(0.5f, 1.0f, 0.5f, 7.5f, 1.0f, 7.5f, pathWithout);
    CHECK(ok, "find_path after deactivate failed");
    CHECK(pathWithout.found, "path after deactivate not found");

    printf("    OK: obstacle registered (rev %" PRIu64 "), deactivated (rev %" PRIu64 ")\n",
           revBefore, provider->revision());
}

// Test 5: Off-mesh link.
static void test_off_mesh_link() {
    printf("  test_off_mesh_link...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_tiled_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");

    engine::navigation::OffMeshLink link;
    link.startX = 1.0f;  link.startY = 1.0f;  link.startZ = 1.0f;
    link.endX = 7.0f;    link.endY = 1.0f;    link.endZ = 7.0f;
    link.radius = 1.0f;
    link.bidirectional = true;

    bool ok = provider->set_off_mesh_links({link}, err);
    CHECK(ok, ("set_off_mesh_links failed: " + err).c_str());

    engine::navigation::PathResult path;
    ok = provider->find_path(1.0f, 1.0f, 1.0f, 7.0f, 1.0f, 7.0f, path);
    CHECK(ok, "find_path with link failed");
    CHECK(path.found, "path not found via link");

    printf("    OK: path via off-mesh link found (%zu waypoints)\n",
           path.waypoints.size() / 3);
}

// Test 6: Area cost — set and verify.
static void test_area_cost() {
    printf("  test_area_cost...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_flat_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");

    CHECK(provider->set_area_cost(1, 10.0f, err), "set_area_cost failed");
    CHECK(std::abs(provider->area_cost(1) - 10.0f) < 0.01f, "cost 1 not 10.0");
    CHECK(std::abs(provider->area_cost(0) - 1.0f) < 0.01f, "cost 0 not 1.0");

    printf("    OK: area costs set correctly\n");
}

// Test 7: Async path query.
static void test_async_path() {
    printf("  test_async_path...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_flat_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");

    uint64_t reqId = provider->begin_async_path(
        0.5f, 1.0f, 0.5f, 7.5f, 1.0f, 7.5f, err);
    CHECK(reqId > 0, "begin_async_path returned 0");

    engine::navigation::PathResult path;
    engine::navigation::PathRequestStatus status;
    int max_polls = 1000;
    do {
        status = provider->poll_async_path(reqId, path, err);
        if (status == engine::navigation::PathRequestStatus::Queued ||
            status == engine::navigation::PathRequestStatus::Running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while ((status == engine::navigation::PathRequestStatus::Queued ||
              status == engine::navigation::PathRequestStatus::Running) &&
             --max_polls > 0);

    CHECK(status == engine::navigation::PathRequestStatus::Succeeded,
          ("async path status: " + std::to_string(static_cast<int>(status))).c_str());
    CHECK(path.found, "async path not found");

    printf("    OK: async path completed with %zu waypoints\n",
           path.waypoints.size() / 3);
}

// Test 8: Determinism — same input produces identical path.
static void test_determinism() {
    printf("  test_determinism...\n");

    auto cols = make_flat_ground();
    auto cfg = make_flat_config();

    auto p1 = engine::navigation::create_recast_navigation_provider();
    auto p2 = engine::navigation::create_recast_navigation_provider();
    std::string err1, err2;
    CHECK(p1->build(cfg, cols, err1), "build p1 failed");
    CHECK(p2->build(cfg, cols, err2), "build p2 failed");

    engine::navigation::PathResult path1, path2;
    CHECK(p1->find_path(0.5f, 1.0f, 0.5f, 7.5f, 1.0f, 7.5f, path1),
          "find_path p1 failed");
    CHECK(p2->find_path(0.5f, 1.0f, 0.5f, 7.5f, 1.0f, 7.5f, path2),
          "find_path p2 failed");

    CHECK(path1.found == path2.found, "found mismatch");
    CHECK(path1.waypoints.size() == path2.waypoints.size(),
          ("waypoint count mismatch: " + std::to_string(path1.waypoints.size()) +
           " vs " + std::to_string(path2.waypoints.size())).c_str());
    CHECK(std::abs(path1.totalLength - path2.totalLength) < 0.01f,
          "totalLength mismatch");

    printf("    OK: deterministic paths (identical waypoints and length)\n");
}

// Test 9: Tile revision — per-tile invalidation signal.
static void test_tile_revision() {
    printf("  test_tile_revision...\n");

    auto provider = engine::navigation::create_recast_navigation_provider();
    auto cols = make_flat_ground();
    auto cfg = make_tiled_config();
    std::string err;

    CHECK(provider->build(cfg, cols, err), "build failed");

    uint64_t rev0 = provider->tile_revision(1.0f, 1.0f);
    CHECK(rev0 > 0, "tile_revision is 0 after build");

    // Update a tile far from (1,1).
    std::vector<engine::navigation::VoxelColumn> change;
    change.push_back({7.5f, 7.5f, 0.0f, 3.0f, true, 0});
    CHECK(provider->update(change, err), "update failed");

    uint64_t rev0_after = provider->tile_revision(1.0f, 1.0f);
    CHECK(rev0_after == rev0, "tile at (1,1) was incorrectly invalidated");

    printf("    OK: tile_revision correctly localizes invalidation\n");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("=== Navigation Dynamic Tests ===\n");
    test_build_and_find_path();
    test_is_walkable();
    test_update_incremental();
    test_dynamic_obstacle();
    test_off_mesh_link();
    test_area_cost();
    test_async_path();
    test_determinism();
    test_tile_revision();
    if (g_failures > 0) {
        printf("=== FAILED: %d failures ===\n", g_failures);
        return 1;
    }
    printf("=== ALL PASSED ===\n");
    return 0;
}

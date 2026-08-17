// ConnectivityTests.cpp
//
// Evidence for FALTANTES §16 item 9: incremental voxel connectivity — only
// voxel islands that LOST their connection to the anchored mass become dynamic
// Jolt bodies. The test drives the PUBLIC world surface (IVoxelWorld) +
// PhysicsRuntime + PhysicsStreamingBridge + VoxelConnectivity:
//   - an intact structure (ground + pillar + floating platform) is ONE
//     anchored component -> zero islands;
//   - carving the pillar out of the voxel world detaches the platform -> the
//     bridge spawns a DYNAMIC Jolt body for the island, which FALLS under
//     gravity and rests on the streamed terrain;
//   - incremental: an edit that does NOT disconnect (a side notch) produces no
//     island, and scanned_cells() stays bounded (far less than the world);
//   - determinism: identical inputs -> bit-identical island lists.

#include <engine/voxel/IVoxelWorld.hpp>

#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/PhysicsStreamingBridge.hpp"
#include "engine/physics/VoxelConnectivity.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Engine::Physics;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// Deterministic flat world (mirrors the voxel_sdk_tests generator).
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

constexpr int kGroundTop = 130;   // FlatGenerator(130): ground top face at y=130
constexpr int kPlatformY = 141;   // platform slab row (3x1x3 above the pillar)

// Builds ground-anchored structure inside chunk (0,0): a 3x1x3 floating
// platform supported by a 1x1 pillar column down to the ground. All blocks
// are builtin stone (id 3).
struct Structure {
    glm::ivec3 pillarMin{ 13, kGroundTop + 1, 13 };
    glm::ivec3 pillarMax{ 13, kPlatformY - 1, 13 };
    glm::ivec3 platformMin{ 12, kPlatformY, 12 };
    glm::ivec3 platformMax{ 14, kPlatformY, 14 };
};

void build_structure(engine::voxel::IVoxelWorld& world,
                     const Structure& s) {
    for (int y = s.pillarMin.y; y <= s.pillarMax.y; ++y)
        world.set_block(s.pillarMin.x, y, s.pillarMin.z, 3);
    for (int x = s.platformMin.x; x <= s.platformMax.x; ++x)
        for (int z = s.platformMin.z; z <= s.platformMax.z; ++z)
            world.set_block(x, kPlatformY, z, 3);
}

void carve_box(engine::voxel::IVoxelWorld& world, const glm::ivec3& min,
               const glm::ivec3& max) {
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z)
                world.set_block(x, y, z, 0);  // air
}

// Steps world + physics + bridge together until `predicate` holds.
template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, const glm::vec3& focus, Pred predicate,
            int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 300;  // 300 sim-seconds
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        physics.step(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

std::string island_key(const VoxelIsland& island) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%d,%d,%d|%d,%d,%d|%zu",
                  island.minimum.x, island.minimum.y, island.minimum.z,
                  island.maximum.x, island.maximum.y, island.maximum.z,
                  island.solidCells);
    return std::string(buffer);
}

bool islands_equal(const std::vector<VoxelIsland>& a,
                   const std::vector<VoxelIsland>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (island_key(a[i]) != island_key(b[i])) return false;
    return true;
}

// 1. Intact structure = one anchored component (no island); carving the
//    pillar detaches the platform, the bridge spawns a dynamic body that
//    FALLS under gravity and rests on the streamed terrain.
void test_island_detach_falls() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);  // chunk (0,0) terrain slab (top at y=130)

    const Structure s;
    build_structure(*world, s);
    const auto isSolid = [](std::uint32_t id) { return id != 0u; };

    VoxelConnectivity connectivity;
    ConnectivitySettings settings;
    connectivity.note_edit(s.platformMin, s.platformMax);
    connectivity.note_edit(s.pillarMin, s.pillarMax);

    // Intact: platform reaches the anchored ground through the pillar.
    const std::vector<VoxelIsland> intact =
        connectivity.sync(*world, settings, isSolid);
    check(intact.empty(), "intact structure is one anchored component (0 islands)");
    check(bridge.sync_detached_islands(connectivity, settings, isSolid) == 0,
          "no island body spawned while anchored");
    check(bridge.island_body_count() == 0, "no island bodies tracked");

    // Carve the pillar -> the platform is a detached island.
    carve_box(*world, s.pillarMin, s.pillarMax);
    connectivity.note_edit(s.pillarMin, s.pillarMax);
    const std::vector<VoxelIsland> islands =
        connectivity.sync(*world, settings, isSolid);
    check(islands.size() == 1, "carved pillar detaches exactly one island");
    if (islands.size() == 1) {
        check(islands[0].minimum == glm::ivec3(12, kPlatformY, 12) &&
                  islands[0].maximum == glm::ivec3(14, kPlatformY, 14),
              "island is the 3x1x3 platform bbox");
        check(islands[0].solidCells == 9, "island holds the 9 platform cells");
    }

    const std::size_t spawned =
        bridge.sync_detached_islands(connectivity, settings, isSolid);
    check(spawned == 1 && bridge.island_body_count() == 1,
          "bridge spawned one dynamic island body");
    // Idempotent: re-running the sync must not spawn a second body.
    check(bridge.sync_detached_islands(connectivity, settings, isSolid) == 0,
          "island->body mapping is idempotent");

    // The island body FALLS from y=141.5 and rests on the terrain slab
    // (top y=130 -> resting center ~130.5).
    const BodyHandle islandBody = bridge.island_body_at(0);
    check(islandBody != InvalidBody, "island body handle available");
    bool fell = false;
    settle(*world, physics, bridge, focus, [&]() {
        const RigidBody* rb = physics.body(islandBody);
        if (rb == nullptr) return false;
        fell = fell || rb->position.y < kPlatformY - 5.0f;
        return rb->position.y < 132.0f;
    });
    check(fell, "island body FALLS under gravity (detached island is dynamic)");
    const RigidBody* rest = physics.body(islandBody);
    if (rest != nullptr) {
        check(std::fabs(rest->position.y - (kGroundTop + 0.5f)) < 1.5f,
              "island body rests on the streamed terrain (top y=130)");
    } else {
        check(false, "island body still alive after settle");
    }
    std::printf("[connectivity] island detaches -> dynamic Jolt body falls + rests OK\n");
}

// 2. Incremental: a side notch that does NOT disconnect produces no island;
//    the flood stays bounded (scanned_cells() << world).
void test_incremental_scan() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);

    const Structure s;
    build_structure(*world, s);
    const auto isSolid = [](std::uint32_t id) { return id != 0u; };

    // Notch into the ground beside the pillar: platform stays anchored.
    const glm::ivec3 notchMin{ 12, kGroundTop - 2, 12 };
    const glm::ivec3 notchMax{ 14, kGroundTop - 2, 14 };
    carve_box(*world, notchMin, notchMax);
    VoxelConnectivity connectivity;
    ConnectivitySettings settings;
    connectivity.note_edit(notchMin, notchMax);
    const std::vector<VoxelIsland> noIsland =
        connectivity.sync(*world, settings, isSolid);
    check(noIsland.empty(), "side notch keeps the platform anchored (0 islands)");
    check(connectivity.scanned_cells() < 5000,
          "notch flood stays bounded (incremental, not the whole world)");

    // Now carve the pillar: exactly one island, flood still bounded.
    carve_box(*world, s.pillarMin, s.pillarMax);
    connectivity.note_edit(s.pillarMin, s.pillarMax);
    const std::vector<VoxelIsland> islands =
        connectivity.sync(*world, settings, isSolid);
    check(islands.size() == 1, "pillar carve detaches the platform");
    check(connectivity.scanned_cells() < 5000,
          "pillar flood stays bounded (incremental)");
    std::printf("[connectivity] incremental bounded flood + notch-keeps-anchored OK\n");
}

// 3. Determinism: identical world + edits -> bit-identical island lists.
void test_determinism() {
    const auto run = [](const Structure& s) {
        std::unique_ptr<engine::voxel::IVoxelWorld> world =
            engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
        boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 2);
        build_structure(*world, s);
        const auto isSolid = [](std::uint32_t id) { return id != 0u; };
        carve_box(*world, s.pillarMin, s.pillarMax);
        VoxelConnectivity connectivity;
        ConnectivitySettings settings;
        connectivity.note_edit(s.pillarMin, s.pillarMax);
        const std::vector<VoxelIsland> islands =
            connectivity.sync(*world, settings, isSolid);
        return std::make_pair(islands, connectivity.scanned_cells());
    };

    const auto first = run(Structure{});
    const auto second = run(Structure{});
    check(islands_equal(first.first, second.first),
          "identical inputs -> bit-identical island lists");
    check(first.second == second.second,
          "identical inputs -> identical flood size");
    std::printf("[connectivity] determinism (islands + flood) OK\n");
}

}  // namespace

int main() {
    test_island_detach_falls();
    test_incremental_scan();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[connectivity] ALL PASSED\n");
        return 0;
    }
    std::printf("[connectivity] %d FAILURE(S)\n", g_failures);
    return 1;
}

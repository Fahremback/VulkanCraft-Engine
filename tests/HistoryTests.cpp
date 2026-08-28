// HistoryTests.cpp
//
// Evidence for FALTANTES §16 item 13: reconstruction / historical restoration
// WITHOUT special cases. DestructionHistoryRuntime captures the FULL state of
// a region (every voxel cell + every active debris) and restores it later,
// regardless of how the damage happened:
//   - round-trip: capture a built structure, carve it, restore -> the region
//     is bit-identical to the capture (the SAME mechanism undoes a manual
//     carve and an explosion carve);
//   - debris: a resting debris captured in the region is re-spawned at its
//     captured position after the region was cleared;
//   - end-to-end without special cases: explosion (item 10) carves the
//     support, connectivity (item 9) detaches the island, the island is
//     managed as debris (item 11), falls, sleeps and REVOXELIZES (item 12) —
//     then restore() puts the region back exactly as captured (pillar and
//     platform back, revoxelized cells cleared, debris removed);
//   - determinism: identical scenarios produce identical snapshots/results;
//   - validation: config JSON, empty/oversized regions refused.

#include <engine/voxel/IVoxelWorld.hpp>

#include "engine/gameplay/DebrisRuntime.hpp"
#include "engine/gameplay/DestructionHistoryRuntime.hpp"
#include "engine/gameplay/ExplosionRuntime.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/PhysicsStreamingBridge.hpp"
#include "engine/physics/VoxelConnectivity.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
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
                int budget, int maxSteps = 60 * 180, int maxBudgetMs = 30000) {
    world.set_chunk_budget(budget);
    const auto start = std::chrono::steady_clock::now();
    for (int step = 0; step < maxSteps; ++step) {
        if (world.is_chunk_loaded(0, 0)) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxBudgetMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return world.is_chunk_loaded(0, 0);
}

constexpr int kGroundTop = 130;

template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, Engine::Gameplay::DebrisRuntime& debris,
            const glm::vec3& focus, Pred predicate, int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 300;
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) {
            for (int extra = 0; extra < 120; ++extra) {
                world.update(focus, 1.0f / 60.0f);
                bridge.sync(focus);
                debris.update(focus, 1.0f / 60.0f);
                physics.step(1.0f / 60.0f);
            }
            return true;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        debris.update(focus, 1.0f / 60.0f);
        physics.step(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

BodyDesc debris_desc(const glm::vec3& position, float mass, const ColliderShape& shape) {
    BodyDesc desc;
    desc.motion = MotionType::Dynamic;
    desc.position = position;
    desc.mass = mass;
    desc.collider.shape = shape;
    return desc;
}

// Platform (3x2x3 stone: y=141..142) supported by a 1x1 stone pillar
// (y=131..140). Two layers tall so that when it rests on the intact ground
// its top layer (y=131, air) is the revoxelized footprint.
void build_platform(engine::voxel::IVoxelWorld& world) {
    for (int y = kGroundTop + 1; y <= 140; ++y)
        world.set_block(12, y, 12, 3);
    for (int x = 11; x <= 13; ++x)
        for (int z = 11; z <= 13; ++z) {
            world.set_block(x, 141, z, 3);
            world.set_block(x, 142, z, 3);
        }
}

constexpr glm::ivec3 kRegionMin{ 9, 129, 9 };
constexpr glm::ivec3 kRegionMax{ 15, 143, 15 };

bool region_identical(engine::voxel::IVoxelWorld& world,
                      const Engine::Gameplay::DestructionSnapshot& snapshot) {
    if (!snapshot.valid()) return false;
    std::size_t i = 0;
    for (int y = snapshot.minimum.y; y <= snapshot.maximum.y; ++y) {
        for (int z = snapshot.minimum.z; z <= snapshot.maximum.z; ++z) {
            for (int x = snapshot.minimum.x; x <= snapshot.maximum.x; ++x, ++i) {
                if (i >= snapshot.cells.size()) return false;
                if (world.get_block(x, y, z) != snapshot.cells[i].blockId) return false;
            }
        }
    }
    return i == snapshot.cells.size();
}

// 1. Round-trip: capture a built structure, carve it, restore -> region
//    bit-identical to the capture. The SAME mechanism undoes a manual carve.
void test_round_trip_carve() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");
    build_platform(*world);

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    Engine::Gameplay::DestructionHistoryRuntime history(physics, debris);

    Engine::Gameplay::DestructionSnapshot snapshot;
    std::string error;
    check(history.capture(*world, kRegionMin, kRegionMax, snapshot, error),
          "capture of the built structure");
    check(snapshot.cells.size() > 0, "snapshot holds the region cells");
    std::size_t solid = 0;
    for (const auto& cell : snapshot.cells)
        if (cell.blockId != 0u) ++solid;
    check(solid >= 20, "captured the pillar + platform solid cells");

    // Carve the whole region to air (manual carve — no special source).
    for (const auto& cell : snapshot.cells)
        world->set_block(cell.position.x, cell.position.y, cell.position.z, 0u);
    std::size_t solidAfter = 0;
    for (int y = kRegionMin.y; y <= kRegionMax.y; ++y)
        for (int z = kRegionMin.z; z <= kRegionMax.z; ++z)
            for (int x = kRegionMin.x; x <= kRegionMax.x; ++x)
                if (world->get_block(x, y, z) != 0u) ++solidAfter;
    check(solidAfter == 0, "carve emptied the region");

    const Engine::Gameplay::DestructionRestoreResult result =
        history.restore(*world, snapshot);
    check(result.cellsWritten == solid, "restore rewrote exactly the solid cells");
    check(region_identical(*world, snapshot), "region bit-identical after restore");
    std::printf("[history] round-trip carve restore OK\n");
}

// 2. Debris in the region: a resting debris captured is re-spawned at its
//    captured position after the region was cleared.
void test_debris_in_region() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    const auto shape = BoxShape{ glm::vec3(0.5f, 0.5f, 0.5f) };
    check(debris.spawn(debris_desc(glm::vec3(12.0f, 140.0f, 12.0f), 3.0f, shape), 3u) !=
              InvalidBody,
          "debris spawned in the region");
    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        std::size_t settled = 0;
        for (const auto& e : events)
            if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) ++settled;
        return settled >= 1;
    });

    Engine::Gameplay::DestructionHistoryRuntime history(physics, debris);
    Engine::Gameplay::DestructionSnapshot snapshot;
    std::string error;
    check(history.capture(*world, kRegionMin, kRegionMax, snapshot, error),
          "capture of the region with the resting debris");
    check(snapshot.debris.size() == 1, "captured exactly the one debris");

    // Clear the region's debris (as if the debris was removed/rebuilt).
    check(debris.despawn_in_box(glm::vec3(kRegionMin), glm::vec3(kRegionMax)) == 1,
          "debris despawned from the region");
    check(debris.active_count() == 0, "no active debris left");

    const Engine::Gameplay::DestructionRestoreResult result =
        history.restore(*world, snapshot);
    check(result.debrisSpawned == 1, "captured debris re-spawned");
    check(debris.active_count() == 1, "one active debris after restore");
    const auto restored = debris.debris_in_box(glm::vec3(kRegionMin), glm::vec3(kRegionMax));
    check(restored.size() == 1 && restored[0].id == snapshot.debris[0].id,
          "restored debris keeps its captured identity");
    const glm::vec3 toCaptured = restored[0].position - snapshot.debris[0].position;
    check(glm::dot(toCaptured, toCaptured) < 0.01f,
          "restored debris sits at its captured position");
    std::printf("[history] debris capture/restore OK\n");
}

// 3. End-to-end WITHOUT special cases: explosion (item 10) carves the
//    support, connectivity (item 9) detaches the island, the island becomes
//    debris (item 11), falls, sleeps, REVOXELIZES (item 12) — then the same
//    generic restore() puts the region back exactly as captured: pillar and
//    platform back, revoxelized cells cleared, no debris left.
void test_end_to_end_restore() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");
    build_platform(*world);

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    Engine::Gameplay::DestructionHistoryRuntime history(physics, debris);

    // Capture BEFORE the destruction pipeline.
    Engine::Gameplay::DestructionSnapshot snapshot;
    std::string error;
    check(history.capture(*world, kRegionMin, kRegionMax, snapshot, error),
          "pre-destruction capture");

    // Explosion at the pillar midpoint carves the whole support (item 10).
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 5.5f;
    config.maxPressure = 40.0f;
    config.heatRadius = 0.0f;
    const Engine::Gameplay::ExplosionResult blast = Engine::Gameplay::apply_explosion(
        *world, physics, glm::vec3(12.5f, 135.5f, 12.5f), config);
    check(blast.blocksRemoved > 5, "explosion carved the pillar");

    // Connectivity (item 9): the platform is a detached island.
    VoxelConnectivity connectivity;
    ConnectivitySettings settings;
    connectivity.note_edit(blast.affectedMin, blast.affectedMax);
    connectivity.note_edit(glm::ivec3(11, 141, 11), glm::ivec3(13, 142, 13));
    const auto isSolid = [](std::uint32_t id) { return id != 0u; };
    const std::vector<VoxelIsland> islands =
        connectivity.sync(*world, settings, isSolid);
    check(islands.size() == 1, "connectivity found the detached platform island");

    // The island becomes debris (item 11).
    if (!islands.empty()) {
        const VoxelIsland& island = islands[0];
        const glm::ivec3 size = island.maximum - island.minimum + glm::ivec3(1);
        check(debris.spawn(debris_desc(glm::vec3(island.minimum) + glm::vec3(size) * 0.5f,
                                       static_cast<float>(island.solidCells),
                                       BoxShape{ glm::vec3(size) * 0.5f }),
                           3u) != InvalidBody,
              "island spawned as debris");
    }

    // Falls, rests, sleeps.
    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        for (const auto& e : events)
            if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) return true;
        return false;
    });

    // Revoxelize (item 12): the platform's footprint becomes terrain again.
    Engine::Gameplay::RevoxelizePolicy policy;
    policy.settleDelay = 0.1f;
    const auto blockOf = [](std::uint32_t material) { return material; };
    const Engine::Gameplay::RevoxelizeResult revox =
        debris.revoxelize_sleeping(*world, policy, blockOf);
    check(revox.debrisRevoxelized == 1 && revox.blocksWritten >= 9,
          "debris revoxelized into the world (converged terrain)");

    // The region is NOT the captured state anymore (pillar gone, revoxelized
    // cells added).
    check(!region_identical(*world, snapshot), "region diverged after the pipeline");

    // Restore the pre-destruction snapshot with the SAME generic mechanism.
    // cellsWritten counts only cells that CHANGED (the blast-cratered pillar
    // and the revoxelized layer); untouched cells are already equal. The gate
    // is region_identical below.
    const Engine::Gameplay::DestructionRestoreResult result =
        history.restore(*world, snapshot);
    check(result.cellsWritten > 0,
          "restore rewrote the destroyed cells");
    check(region_identical(*world, snapshot),
          "region bit-identical to the pre-destruction capture after restore");
    check(debris.active_count() == 0,
          "no debris left in the restored region");
    std::printf("[history] end-to-end pipeline -> generic restore OK\n");
}

// 4. Determinism: two identical scenarios produce identical snapshots and
//    identical restore results.
void test_determinism() {
    const auto runScenario = []() {
        std::unique_ptr<engine::voxel::IVoxelWorld> world =
            engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
        const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
        boot_world(*world, focus, 2);
        build_platform(*world);
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        PhysicsStreamingBridge bridge(*world, physics);
        bridge.sync(focus);
        Engine::Gameplay::DebrisRuntime debris(physics, {});
        Engine::Gameplay::DestructionHistoryRuntime history(physics, debris);
        Engine::Gameplay::DestructionSnapshot snapshot;
        std::string error;
        history.capture(*world, kRegionMin, kRegionMax, snapshot, error);
        // Carve half the region.
        for (const auto& cell : snapshot.cells)
            if (cell.position.y <= 137)
                world->set_block(cell.position.x, cell.position.y, cell.position.z, 0u);
        const Engine::Gameplay::DestructionRestoreResult result =
            history.restore(*world, snapshot);
        return std::make_pair(snapshot, result);
    };

    const auto [first, firstResult] = runScenario();
    const auto [second, secondResult] = runScenario();
    check(first.cells.size() == second.cells.size(), "identical snapshot sizes");
    bool cellsEqual = first.cells.size() == second.cells.size();
    if (cellsEqual) {
        for (std::size_t i = 0; i < first.cells.size(); ++i) {
            if (first.cells[i].blockId != second.cells[i].blockId ||
                first.cells[i].position != second.cells[i].position) {
                cellsEqual = false;
                break;
            }
        }
    }
    check(cellsEqual, "identical snapshots (cells bit-equal)");
    check(firstResult.cellsWritten == secondResult.cellsWritten &&
              firstResult.debrisSpawned == secondResult.debrisSpawned &&
              firstResult.debrisDespawned == secondResult.debrisDespawned,
          "identical restore results");
    std::printf("[history] determinism OK\n");
}

// 5. Validation: config JSON, empty/oversized regions refused.
void test_validation() {
    Engine::Gameplay::DestructionHistoryConfig config;
    std::string error;
    check(!config.load_from_json(R"({"maxRegionCells":0})", error),
          "maxRegionCells 0 refused");
    check(!config.load_from_json(R"({"maxRegionCells":-1})", error),
          "negative maxRegionCells refused");
    check(!config.load_from_json(R"({"maxRegionCells":999999999})", error),
          "oversized maxRegionCells refused");
    Engine::Gameplay::DestructionHistoryConfig valid;
    check(valid.load_from_json(R"({"maxRegionCells":64,"enabled":true})", error) &&
              valid.maxRegionCells == 64 && valid.enabled,
          "valid config loads and round-trips");

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    boot_world(*world, focus, 2);
    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    Engine::Gameplay::DestructionHistoryRuntime history(physics, debris);
    Engine::Gameplay::DestructionSnapshot snapshot;
    check(!history.capture(*world, glm::ivec3(3, 3, 3), glm::ivec3(2, 3, 3), snapshot, error),
          "empty region (min > max) refused");
    check(!history.capture(*world, glm::ivec3(0, 0, 0), glm::ivec3(100, 100, 100),
                           snapshot, error),
          "region over maxRegionCells refused");
    std::printf("[history] validation OK\n");
}

}  // namespace

int main() {
    test_round_trip_carve();
    test_debris_in_region();
    test_end_to_end_restore();
    test_determinism();
    test_validation();
    if (g_failures == 0) {
        std::printf("[history] ALL PASSED\n");
        return 0;
    }
    std::printf("[history] %d FAILURE(S)\n", g_failures);
    return 1;
}

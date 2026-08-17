// RevoxelizeTests.cpp
//
// Evidence for FALTANTES §16 item 12: re-aggregate / revoxelize sleeping
// debris per the ASSET policy. The test drives the PUBLIC world surface
// (IVoxelWorld) + PhysicsRuntime + PhysicsStreamingBridge + DebrisRuntime:
//   - a debris that fell and SLEEPS is written back into the voxel world
//     (air cells inside its footprint become solid) and despawned — the
//     destruction converges back to terrain;
//   - the policy filters: settle delay, material filter, missing block
//     mapping (skipped), and per-debris footprint caps;
//   - end-to-end: explosion (item 10) carves a support, connectivity (item 9)
//     detaches the island, the island is managed as debris (item 11), falls,
//     sleeps and REVOXELIZES back into the world;
//   - determinism: identical debris behave identically.

#include <engine/voxel/IVoxelWorld.hpp>

#include "engine/gameplay/DebrisRuntime.hpp"
#include "engine/gameplay/ExplosionRuntime.hpp"
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

constexpr int kGroundTop = 130;

template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, Engine::Gameplay::DebrisRuntime& debris,
            const glm::vec3& focus, Pred predicate, int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 300;
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) {
            // Keep stepping AFTER the Settled event so Jolt actually marks
            // the body sleeping (default sleep-in delay ~0.5 s) and the
            // debris rest timer accumulates past the policy settle delay.
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

// 1. A sleeping debris is revoxelized back into the world (air cells of its
//    footprint become solid) and despawned.
void test_revoxelize_sleeping() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    const Engine::Physics::BodyHandle debris_body = debris.spawn(
        debris_desc(glm::vec3(8.0f, 140.0f, 8.0f), 3.0f,
                    BoxShape{ glm::vec3(0.5f, 1.0f, 0.5f) }), 3u);
    check(debris_body != InvalidBody, "debris spawned");

    // Fall, rest on the terrain slab and SLEEP.
    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        return events.size() >= 1;  // Settled fired once resting
    });
    Engine::Gameplay::RevoxelizePolicy policy;
    policy.settleDelay = 0.1f;
    const auto blockOf = [](std::uint32_t material) { return material; };  // material 3 -> stone 3
    const Engine::Gameplay::RevoxelizeResult result =
        debris.revoxelize_sleeping(*world, policy, blockOf);

    std::printf("  [dbg] blocks=%zu\n", result.blocksWritten);
    // The box settles with sub-cell drift (center ≈ 7.996) so its footprint
    // lands in cells x/z ∈ {7,8} — count the stone cells actually written in
    // that 2x2 window instead of pinning one exact column.
    std::size_t stoneCells = 0;
    for (int x = 7; x <= 8; ++x)
        for (int z = 7; z <= 8; ++z)
            for (int y = kGroundTop + 1; y <= kGroundTop + 2; ++y)
                if (world->get_block(x, y, z) == 3u) ++stoneCells;
    check(result.debrisRevoxelized == 1, "sleeping debris revoxelized");
    check(result.blocksWritten >= 1, "air cells of the footprint turned solid");
    check(stoneCells == result.blocksWritten,
          "every written cell is solid stone (revoxelized)");
    check(debris.active_count() == 0 && debris.pooled_count() == 1,
          "revoxelized debris despawned into the pool");
    std::printf("[revoxelize] sleeping debris written back to the world OK\n");
}

// 2. Policy filters: settle delay, material filter, missing mapping, cap.
void test_policy_filters() {
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
    debris.spawn(debris_desc(glm::vec3(4.0f, 140.0f, 4.0f), 3.0f, shape), 3u);
    debris.spawn(debris_desc(glm::vec3(8.0f, 140.0f, 4.0f), 3.0f, shape), 7u);
    debris.spawn(debris_desc(glm::vec3(12.0f, 140.0f, 4.0f), 3.0f, shape), 3u);
    check(debris.drain_replication_events().size() == 3, "three debris spawned");


    // All settle + sleep.
    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        std::size_t settled = 0;
        for (const auto& e : events)
            if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) ++settled;
        return settled >= 3;
    });

    // No mapping for material 7 -> skipped; the two material-3 debris
    // revoxelize.
    Engine::Gameplay::RevoxelizePolicy policy;
    policy.settleDelay = 0.1f;
    const auto partialMap = [](std::uint32_t material) { return material == 3u ? 3u : 0u; };
    const Engine::Gameplay::RevoxelizeResult first =
        debris.revoxelize_sleeping(*world, policy, partialMap);
    check(first.debrisRevoxelized == 2 && first.debrisSkipped == 1,
          "no-mapping material skipped, mapped debris revoxelized");
    check(debris.active_count() == 1, "only the material-7 debris remains");

    // Material filter: only material 7 now, with a mapping.
    Engine::Gameplay::RevoxelizePolicy filtered;
    filtered.settleDelay = 0.1f;
    filtered.materialFilter = 7u;
    const auto fullMap = [](std::uint32_t material) { return material; };
    const Engine::Gameplay::RevoxelizeResult second =
        debris.revoxelize_sleeping(*world, filtered, fullMap);
    check(second.debrisRevoxelized == 1, "material-filtered debris revoxelized");
    check(debris.active_count() == 0, "all debris revoxelized");

    // settleDelay too high: a fresh debris stays (not eligible).
    const Engine::Physics::BodyHandle deferredBody =
        debris.spawn(debris_desc(glm::vec3(8.0f, 140.0f, 8.0f), 3.0f, shape), 3u);
    Engine::Gameplay::RevoxelizePolicy delayed;
    delayed.settleDelay = 100.0f;
    const Engine::Gameplay::RevoxelizeResult none =
        debris.revoxelize_sleeping(*world, delayed, fullMap);
    check(none.debrisRevoxelized == 0 && debris.active_count() == 1,
          "settle delay too high defers the revoxelization");
    // Remove the deferred debris so the footprint-cap phase below is the
    // only active debris (deterministic per-pass count).
    check(debris.despawn(deferredBody), "deferred debris despawned");

    // Footprint cap: a large debris writes at most maxBlocksPerDebris cells.
    debris.spawn(debris_desc(glm::vec3(12.0f, 140.0f, 8.0f), 9.0f,
                             BoxShape{ glm::vec3(2.0f, 1.0f, 2.0f) }), 3u);
    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        for (const auto& e : events)
            if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) return true;
        return false;
    });
    Engine::Gameplay::RevoxelizePolicy capped;
    capped.settleDelay = 0.1f;
    capped.maxBlocksPerDebris = 8;
    const Engine::Gameplay::RevoxelizeResult cappedResult =
        debris.revoxelize_sleeping(*world, capped, fullMap);
    check(cappedResult.debrisRevoxelized == 1 && cappedResult.blocksWritten == 8,
          "per-debris footprint cap enforced (8 cells written)");
    std::printf("[revoxelize] policy filters (delay/material/mapping/cap) OK\n");
}

// 3. End-to-end convergence: explosion carves the support, connectivity
//    detaches the island, the island is managed as debris, falls, sleeps and
//    REVOXELIZES back into the world.
void test_end_to_end() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    // Platform (3x2x3 stone: y=141..142) supported by a 1x1 stone pillar
    // (y=131..140). The platform is TWO layers tall so that once it rests on
    // the intact ground its TOP layer (y=131, air) is the revoxelized
    // footprint — a 1-layer platform would rest with its footprint on the
    // already-solid ground layer and write nothing.
    for (int y = kGroundTop + 1; y <= 140; ++y)
        world->set_block(12, y, 12, 3);
    for (int x = 11; x <= 13; ++x)
        for (int z = 11; z <= 13; ++z) {
            world->set_block(x, 141, z, 3);
            world->set_block(x, 142, z, 3);
        }

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);

    // Explosion at the pillar midpoint carves the whole support (item 10).
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 5.5f;
    config.maxPressure = 40.0f;
    config.heatRadius = 0.0f;
    const Engine::Gameplay::ExplosionResult blast = Engine::Gameplay::apply_explosion(
        *world, physics, glm::vec3(12.5f, 135.5f, 12.5f), config);
    check(blast.blocksRemoved > 5, "explosion carved the pillar");

    // Connectivity (item 9): the platform is a detached island. Note BOTH
    // the blast's affected region AND the platform's own bounds — the island
    // lives above the carved pillar (y=141..142) and the flood fill only
    // scans dirty regions (mirroring the item-9 test).
    VoxelConnectivity connectivity;
    ConnectivitySettings settings;
    connectivity.note_edit(blast.affectedMin, blast.affectedMax);
    connectivity.note_edit(glm::ivec3(11, 141, 11), glm::ivec3(13, 142, 13));
    const auto isSolid = [](std::uint32_t id) { return id != 0u; };
    const std::vector<VoxelIsland> islands =
        connectivity.sync(*world, settings, isSolid);
    check(islands.size() == 1, "connectivity found the detached platform island");

    // The island becomes debris managed by the DebrisRuntime (item 11) with
    // the island's bbox shape and mass from its cells.
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    BodyHandle islandBody = InvalidBody;
    if (!islands.empty()) {
        const VoxelIsland& island = islands[0];
        const glm::ivec3 size = island.maximum - island.minimum + glm::ivec3(1);
        islandBody = debris.spawn(
            debris_desc(glm::vec3(island.minimum) + glm::vec3(size) * 0.5f,
                        static_cast<float>(island.solidCells),
                        BoxShape{ glm::vec3(size) * 0.5f }), 3u);
    }
    check(islandBody != InvalidBody, "island spawned as debris");

    // Falls, rests, sleeps.
    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        for (const auto& e : events)
            if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) return true;
        return false;
    });

    // Revoxelize: the platform's footprint becomes solid terrain again.
    Engine::Gameplay::RevoxelizePolicy policy;
    policy.settleDelay = 0.1f;
    const auto blockOf = [](std::uint32_t material) { return material; };
    const Engine::Gameplay::RevoxelizeResult result =
        debris.revoxelize_sleeping(*world, policy, blockOf);
    check(result.debrisRevoxelized == 1 && result.blocksWritten >= 9,
          "platform debris revoxelized into a 3x3 block layer");
    std::size_t solidLayer = 0;
    for (int x = 11; x <= 13; ++x)
        for (int z = 11; z <= 13; ++z)
            if (world->get_block(x, kGroundTop + 1, z) == 3) ++solidLayer;
    check(solidLayer >= 3, "the landing layer is solid again (converged terrain)");
    std::printf("[revoxelize] end-to-end: explosion -> island -> debris -> falls -> terrain OK\n");
}

// 4. Determinism: identical debris at mirrored positions behave identically.
void test_determinism() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    const auto shape = BoxShape{ glm::vec3(0.5f, 1.0f, 0.5f) };
    debris.spawn(debris_desc(glm::vec3(6.0f, 140.0f, 6.0f), 3.0f, shape), 3u);
    debris.spawn(debris_desc(glm::vec3(10.0f, 140.0f, 10.0f), 3.0f, shape), 3u);
    check(debris.drain_replication_events().size() == 2, "two debris spawned");

    settle(*world, physics, bridge, debris, focus, [&]() {
        const auto events = debris.drain_replication_events();
        std::size_t settled = 0;
        for (const auto& e : events)
            if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) ++settled;
        return settled >= 2;
    });

    Engine::Gameplay::RevoxelizePolicy policy;
    policy.settleDelay = 0.1f;
    const auto blockOf = [](std::uint32_t material) { return material; };
    const Engine::Gameplay::RevoxelizeResult result =
        debris.revoxelize_sleeping(*world, policy, blockOf);
    check(result.debrisRevoxelized == 2 && result.blocksWritten == 4,
          "both identical debris wrote the same cells");
    // Both boxes settle with the same sub-cell drift, so count stone cells in
    // the mirrored 2x2 windows — identical debris produce identical counts.
    std::size_t left = 0;
    std::size_t right = 0;
    for (int x = 5; x <= 6; ++x)
        for (int z = 5; z <= 6; ++z)
            for (int y = kGroundTop + 1; y <= kGroundTop + 2; ++y) {
                if (world->get_block(x, y, z) == 3u) ++left;
                if (world->get_block(x + 4, y, z + 4) == 3u) ++right;
            }
    check(result.debrisRevoxelized == 2 && result.blocksWritten == 4 && left == 2 && right == 2,
          "both identical debris wrote the same cells");
    check(left == right, "identical footprints produced identical solid cells");
    std::printf("[revoxelize] determinism OK\n");
}

// 5. Policy validation: out-of-range values are refused.
void test_policy_validation() {
    Engine::Gameplay::RevoxelizePolicy policy;
    std::string error;
    check(!policy.load_from_json(R"({"settleDelay":-1})", error), "negative settleDelay refused");
    check(!policy.load_from_json(R"({"maxBlocksPerDebris":0})", error), "maxBlocksPerDebris 0 refused");
    check(!policy.load_from_json(R"({"maxDebrisPerPass":0})", error), "maxDebrisPerPass 0 refused");
    Engine::Gameplay::RevoxelizePolicy valid;
    const bool loaded = valid.load_from_json(
        R"({"settleDelay":2,"materialFilter":7,"maxBlocksPerDebris":16})", error);
    check(loaded && valid.settleDelay == 2.0f && valid.materialFilter == 7u &&
              valid.maxBlocksPerDebris == 16,
          "valid policy loads and round-trips");
    std::printf("[revoxelize] policy validation OK\n");
}

}  // namespace

int main() {
    test_revoxelize_sleeping();
    test_policy_filters();
    test_end_to_end();
    test_determinism();
    test_policy_validation();
    if (g_failures == 0) {
        std::printf("[revoxelize] ALL PASSED\n");
        return 0;
    }
    std::printf("[revoxelize] %d FAILURE(S)\n", g_failures);
    return 1;
}

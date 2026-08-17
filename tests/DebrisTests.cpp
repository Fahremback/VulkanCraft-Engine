// DebrisTests.cpp
//
// Evidence for FALTANTES §16 item 11: production debris with pooling, LOD,
// selective persistence and replication. The test drives PhysicsRuntime +
// PhysicsStreamingBridge (terrain slabs) + DebrisRuntime:
//   - pooling: spawn 8 debris, despawn all, spawn 8 again -> the pool reuses
//     the parked bodies (reused_count == 8, spawned_total stays 8);
//   - LOD: debris beyond the cull radius leave the simulation (active -> 0,
//     culled records kept); when focus returns they re-enter at the same
//     transform;
//   - selective persistence: only RESTING debris within persistRadius with
//     mass >= minPersistMass are snapshotted; restore() rebuilds them with the
//     same identity, position and shape;
//   - replication: lifecycle changes drain as Spawned/Despawned/Settled events;
//   - determinism: identical runs produce identical persistable snapshots.

#include <engine/voxel/IVoxelWorld.hpp>

#include "engine/gameplay/DebrisRuntime.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/PhysicsStreamingBridge.hpp"

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

// Steps world + physics + bridge + debris until `predicate` holds.
template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, Engine::Gameplay::DebrisRuntime& debris,
            const glm::vec3& focus, Pred predicate, int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 300;
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) return true;
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

BodyDesc debris_desc(const glm::vec3& position, float mass) {
    BodyDesc desc;
    desc.motion = MotionType::Dynamic;
    desc.position = position;
    desc.mass = mass;
    desc.collider.shape = BoxShape{ glm::vec3(0.5f) };
    return desc;
}

// 1. Pooling: bodies are parked and REUSED, not recreated.
void test_pooling() {
    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::DebrisConfig config;
    config.poolSize = 16;
    Engine::Gameplay::DebrisRuntime debris(physics, config);

    std::vector<BodyHandle> bodies;
    for (int i = 0; i < 8; ++i)
        bodies.push_back(debris.spawn(debris_desc(glm::vec3(2.0f + i * 1.5f, 140.0f, 2.0f), 1.0f), 3u));
    check(debris.active_count() == 8 && debris.pooled_count() == 0,
          "8 debris spawned fresh");
    check(debris.spawned_total() == 8 && debris.reused_count() == 0,
          "all 8 were created (no reuse yet)");

    for (BodyHandle body : bodies) debris.despawn(body);
    check(debris.active_count() == 0 && debris.pooled_count() == 8,
          "8 debris despawned into the pool (parked)");

    for (int i = 0; i < 8; ++i)
        debris.spawn(debris_desc(glm::vec3(2.0f + i * 1.5f, 140.0f, 2.0f), 1.0f), 3u);
    check(debris.active_count() == 8 && debris.pooled_count() == 0,
          "8 debris re-spawned from the pool");
    check(debris.reused_count() == 8 && debris.spawned_total() == 8,
          "pool reused all 8 bodies (spawned_total unchanged)");
    std::printf("[debris] pooling: parked bodies reused, no new allocations OK\n");
}

// 2. LOD: debris beyond the cull radius leave the simulation; focus return
//    re-enters them at the same transform.
void test_lod_cull_reenter() {
    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::DebrisConfig config;
    config.cullRadius = 96.0f;
    Engine::Gameplay::DebrisRuntime debris(physics, config);

    std::vector<BodyHandle> bodies;
    std::vector<glm::vec3> spawns = { { 4.0f, 140.0f, 4.0f }, { 8.0f, 140.0f, 4.0f },
                                      { 4.0f, 140.0f, 8.0f }, { 8.0f, 140.0f, 8.0f } };
    for (const glm::vec3& p : spawns) bodies.push_back(debris.spawn(debris_desc(p, 1.0f), 3u));
    check(debris.drain_replication_events().size() == 4,
          "spawn drained 4 Spawned events");
    const glm::vec3 focus(8.0f, 140.0f, 8.0f);
    debris.update(focus, 1.0f / 60.0f);
    check(debris.active_count() == 4 && debris.culled_count() == 0,
          "near focus: all 4 debris active");
    // Fresh debris at speed 0 count as resting -> Settled events. Drain them
    // so the later drains are exactly the cull/re-entry events.
    check(debris.drain_replication_events().size() == 4,
          "fresh resting debris emitted Settled events");

    const glm::vec3 farFocus(1000.0f, 140.0f, 1000.0f);
    debris.update(farFocus, 1.0f / 60.0f);
    check(debris.active_count() == 0 && debris.culled_count() == 4 &&
              debris.cull_enter_count() == 4,
          "far focus: all 4 debris culled out of the simulation");
    // Drain the Despawned events of the cull so the re-entry drain below is
    // exactly the Spawned events.
    check(debris.drain_replication_events().size() == 4,
          "cull drained 4 Despawned events");

    debris.update(focus, 1.0f / 60.0f);
    check(debris.active_count() == 4 && debris.culled_count() == 0 &&
              debris.cull_reenter_count() == 4,
          "focus returns: all 4 debris re-entered");
    // The cull DESTROYED the original bodies; re-entry re-armed new ones. The
    // Spawned events after re-entry must carry the same transforms (order
    // preserved by the deterministic culled_ iteration).
    const auto reentered = debris.drain_replication_events();
    bool positionsPreserved = reentered.size() == spawns.size();
    for (std::size_t i = 0; i < reentered.size() && positionsPreserved; ++i) {
        if (glm::distance(reentered[i].position, spawns[i]) > 0.01f)
            positionsPreserved = false;
    }
    check(positionsPreserved, "re-entered debris kept their spawn transforms");
    std::printf("[debris] LOD: cull beyond radius, re-enter on return OK\n");
}

// 3. Selective persistence: only resting + in-radius + above-min-mass debris
//    are snapshotted; restore() rebuilds them with identity and shape.
void test_selective_persistence() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 130, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    Engine::Gameplay::DebrisConfig config;
    config.persistRadius = 8.0f;
    config.minPersistMass = 0.5f;
    Engine::Gameplay::DebrisRuntime debris(physics, config);

    // Heavy resting debris at the focus (persistable).
    const BodyHandle heavy = debris.spawn(debris_desc(glm::vec3(8.0f, 140.0f, 8.0f), 3.0f), 3u);
    // Light dust debris (excluded by minPersistMass).
    debris.spawn(debris_desc(glm::vec3(10.0f, 140.0f, 8.0f), 0.1f), 3u);
    // Heavy but far from the focus (excluded by persistRadius).
    debris.spawn(debris_desc(glm::vec3(15.0f, 140.0f, 15.0f), 3.0f), 3u);

    // Let the near debris fall and REST on the streamed terrain slab.
    settle(*world, physics, bridge, debris, focus, [&]() {
        const RigidBody* rb = physics.body(heavy);
        return rb != nullptr && rb->sleeping;
    });
    debris.update(focus, 1.0f / 60.0f);  // compute resting flags

    // Persistence focus is NEAR the resting debris (the streaming focus is
    // high up at y=200 and would push every resting debris out of the radius).
    const glm::vec3 persistFocus(8.0f, 132.0f, 8.0f);
    const std::vector<Engine::Gameplay::DebrisPersistRecord> snapshot =
        debris.snapshot_persistable(persistFocus);
    check(snapshot.size() == 1, "exactly the heavy resting in-radius debris persisted");
    if (!snapshot.empty()) {
        check(snapshot[0].mass == 3.0f && snapshot[0].materialIndex == 3u,
              "persisted record carries mass + material");
        check(std::holds_alternative<BoxShape>(snapshot[0].shape),
              "persisted record carries the debris shape");
    }

    // Restore into a FRESH runtime (same physics): identity + transform back.
    Engine::Gameplay::DebrisRuntime restored(physics, config);
    check(restored.restore(snapshot) == 1, "snapshot restored into a fresh runtime");
    check(restored.active_count() == 1 && restored.reused_count() == 0,
          "restored debris is active (fresh body)");
    const auto restoredEvents = restored.drain_replication_events();
    check(restoredEvents.size() == 1 &&
              restoredEvents[0].type == Engine::Gameplay::DebrisReplicationEvent::Type::Spawned &&
              restoredEvents[0].id == snapshot[0].id &&
              glm::distance(restoredEvents[0].position, snapshot[0].position) < 0.01f,
          "restored debris keeps identity + transform (via the Spawned event)");
    std::printf("[debris] selective persistence: rest+radius+mass filter, restore round-trip OK\n");
}

// 4. Replication: lifecycle changes drain as Spawned/Despawned/Settled.
void test_replication_events() {
    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::DebrisRuntime debris(physics, {});
    check(debris.drain_replication_events().empty(), "no events before any spawn");

    const BodyHandle a = debris.spawn(debris_desc(glm::vec3(2.0f, 140.0f, 2.0f), 1.0f), 3u);
    const BodyHandle b = debris.spawn(debris_desc(glm::vec3(4.0f, 140.0f, 2.0f), 1.0f), 3u);
    auto spawned = debris.drain_replication_events();
    check(spawned.size() == 2 &&
              spawned[0].type == Engine::Gameplay::DebrisReplicationEvent::Type::Spawned &&
              spawned[1].type == Engine::Gameplay::DebrisReplicationEvent::Type::Spawned,
          "spawns drained as Spawned events (in order)");
    check(spawned[0].id != spawned[1].id && spawned[0].mass == 1.0f,
          "events carry distinct ids + mass");

    check(debris.despawn(a), "despawn a");
    auto despawned = debris.drain_replication_events();
    check(despawned.size() == 1 &&
              despawned[0].type == Engine::Gameplay::DebrisReplicationEvent::Type::Despawned &&
              despawned[0].id == spawned[0].id,
          "despawn drained as a Despawned event with the same id");
    check(debris.despawn(b) && debris.drain_replication_events().size() == 1,
          "second despawn drained");
    std::printf("[debris] replication: spawn/despawn events drained in order OK\n");
}

// 5. Determinism: identical runs -> identical persistable snapshots.
void test_determinism() {
    const auto run = []() {
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::DebrisRuntime debris(physics, {});
        for (int i = 0; i < 3; ++i)
            debris.spawn(debris_desc(glm::vec3(2.0f + i, 140.0f, 2.0f), 1.0f), 3u);
        // Mark resting (speed 0 -> resting) so the snapshot filter is exercised.
        debris.update(glm::vec3(8.0f, 140.0f, 8.0f), 1.0f / 60.0f);
        const auto snapshot = debris.snapshot_persistable(glm::vec3(8.0f, 140.0f, 8.0f));
        std::string key;
        for (const auto& record : snapshot) {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), "%llu|%.2f,%.2f,%.2f|%.2f|%u;",
                          static_cast<unsigned long long>(record.id),
                          static_cast<double>(record.position.x),
                          static_cast<double>(record.position.y),
                          static_cast<double>(record.position.z),
                          static_cast<double>(record.mass), record.materialIndex);
            key += buffer;
        }
        return key;
    };
    check(run() == run(), "identical runs -> identical persistable snapshots");
    std::printf("[debris] determinism OK\n");
}

// 6. Config validation: out-of-range values are refused.
void test_config_validation() {
    Engine::Gameplay::DebrisConfig config;
    std::string error;
    check(!config.load_from_json(R"({"poolSize":0})", error), "poolSize 0 refused");
    check(!config.load_from_json(R"({"poolSize":4096})", error), "poolSize 4096 refused");
    check(!config.load_from_json(R"({"cullRadius":0})", error), "cullRadius 0 refused");
    check(!config.load_from_json(R"({"restSpeed":-1})", error), "negative restSpeed refused");
    Engine::Gameplay::DebrisConfig valid;
    check(valid.load_from_json(R"({"poolSize":8,"cullRadius":128,"persistRadius":32})", error) &&
              valid.poolSize == 8 && valid.cullRadius == 128.0f && valid.persistRadius == 32.0f,
          "valid config loads and round-trips");
    std::printf("[debris] config validation OK\n");
}

}  // namespace

int main() {
    test_pooling();
    test_lod_cull_reenter();
    test_selective_persistence();
    test_replication_events();
    test_determinism();
    test_config_validation();
    if (g_failures == 0) {
        std::printf("[debris] ALL PASSED\n");
        return 0;
    }
    std::printf("[debris] %d FAILURE(S)\n", g_failures);
    return 1;
}

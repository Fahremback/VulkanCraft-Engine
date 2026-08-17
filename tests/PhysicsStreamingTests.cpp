// PhysicsStreamingTests.cpp
//
// Evidence for FALTANTES §16 item 6: physics integrated with streaming,
// sleeping and the authoritative server. The bridge owns the physics bodies
// that mirror the streaming voxel world; this TU proves the contract through
// the PUBLIC world surface (IVoxelWorld) + PhysicsRuntime + the bridge:
//   - a loaded chunk gets a static terrain slab under the world surface;
//   - a dynamic body spawned above streamed terrain falls, rests and SLEEPS;
//   - wake_region (terrain edit) wakes the sleeping body;
//   - a chunk that leaves the loaded set is unloaded: slab destroyed and every
//     dynamic body inside the chunk is despawned (server authority) — no
//     orphan bodies survive an eviction or a focus move.

#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/registry/BlockRegistry.hpp>

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

// Steps world + physics + bridge together until `predicate` holds.
template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, const glm::vec3& focus, Pred predicate,
            int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 180;  // 180 sim-seconds
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

// 1. Streaming -> terrain slab; dynamic body rests and sleeps on it; the
//    terrain-edit wake path (wake_region) wakes it.
void test_streaming_sleep_wake() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 2), "boot world (flat 96, budget 2)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);

    // Chunk (0,0) is loaded -> a static terrain slab at the surface (the boot
    // only guarantees (0,0); neighbors may or may not be loaded yet, so the
    // count is a lower bound).
    bridge.sync(focus);
    check(bridge.terrain_body_count() >= 1,
          "loaded chunk (0,0) gets a terrain slab");
    check(bridge.spawned_terrain_count() >= 1,
          "terrain slab count increments");

    // A dynamic sphere above the terrain falls and rests on the slab. The
    // flat world's top block sits at y=96 (face at y=97); the slab top is
    // placed at the raycast entry, so the resting center is ~97.5 (radius
    // 0.5).
    BodyDesc sphere;
    sphere.position = glm::vec3(8.0f, 120.0f, 8.0f);
    sphere.collider.shape = SphereShape{ 0.5f };
    const BodyHandle ball = bridge.spawn_dynamic(sphere);
    check(ball != InvalidBody, "bridge spawns the dynamic body");

    const bool rested = settle(
        *world, physics, bridge, focus,
        [&]() {
            const RigidBody* rb = bridge.body(ball);
            return rb != nullptr && rb->sleeping;
        });
    check(rested, "body falls, rests on streamed terrain and SLEEPS");

    const RigidBody* rest = bridge.body(ball);
    check(rest != nullptr, "body handle still valid after resting");
    if (rest != nullptr) {
        check(std::fabs(rest->position.y - 97.5f) < 1.0f,
              "resting height matches the streamed surface (face + radius)");
        check(rest->sleeping, "body reports sleeping through the runtime");
    }

    // Terrain edit wakes the sleeping body overlapping the edited region.
    const std::size_t woken = bridge.wake_region(
        glm::vec3(0.0f, 90.0f, 0.0f), glm::vec3(16.0f, 130.0f, 16.0f));
    check(woken == 1, "wake_region wakes exactly the overlapping body");
    const RigidBody* afterWake = bridge.body(ball);
    check(afterWake != nullptr && !afterWake->sleeping,
          "woken body is no longer sleeping");

    std::printf("[streaming] slab under loaded chunk, body rests + sleeps, "
                "wake_region wakes: OK\n");
}

// 2. Server authority: a body spawned into a chunk that is NOT loaded is
//    despawned on the next sync; when the focus moves / budget shrinks and
//    chunks leave the loaded set, their slabs AND contained dynamic bodies are
//    removed — no orphan bodies survive an unload.
void test_streaming_authority_unload() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    check(boot_world(*world, focus, 3), "boot world (budget 3 -> (0,0)+(1,0))");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);

    // Wait until both (0,0) and (1,0) are loaded AND slabbed (boot only
    // guarantees (0,0), so settle on the two-slab state before spawning).
    const bool slabs = settle(
        *world, physics, bridge, focus,
        [&]() { return bridge.terrain_body_count() >= 2; });
    check(slabs, "chunks (0,0) and (1,0) both hold terrain slabs");
    const std::size_t slabsBefore = bridge.terrain_body_count();

    // Body A in chunk (0,0) [world x in 0..16], body B in chunk (1,0)
    // [world x in 16..32]; body C in a NOT-loaded chunk (~(37,37)).
    const auto spawn = [&](float x, float z) {
        BodyDesc sphere;
        sphere.position = glm::vec3(x, 120.0f, z);
        sphere.collider.shape = SphereShape{ 0.5f };
        return bridge.spawn_dynamic(sphere);
    };
    const BodyHandle a = spawn(8.0f, 8.0f);
    const BodyHandle b = spawn(24.0f, 8.0f);
    const BodyHandle c = spawn(600.0f, 600.0f);
    check(a != InvalidBody && b != InvalidBody && c != InvalidBody,
          "three bodies spawned through the bridge");
    check(bridge.dynamic_body_count() == 3, "three tracked dynamic bodies");

    // First sync after the spawns: C's chunk is not loaded -> despawned by
    // authority; A and B rest on slabs in loaded chunks.
    bridge.sync(focus);
    check(bridge.dynamic_body_count() == 2,
          "body in unloaded chunk despawned by authority (no orphans)");
    check(bridge.unloaded_body_count() == 1, "unload counter reflects the despawn");
    check(bridge.terrain_body_count() == slabsBefore,
          "loaded chunks keep their terrain slabs");

    // Move the focus far away with the minimum budget: (0,0) and (1,0) leave
    // the loaded set -> their slabs are destroyed AND the contained bodies are
    // despawned. The new focus region (~(37,37)) loads and gets its own slabs
    // (budget clamps to MIN_CHUNK_BUDGET = 8, so several chunks load there).
    const glm::vec3 farFocus{ 600.0f, 8.0f, 600.0f };
    world->set_chunk_budget(8);
    const bool evicted = settle(
        *world, physics, bridge, farFocus,
        [&]() {
            return bridge.dynamic_body_count() == 0 &&
                   !world->is_chunk_loaded(0, 0) &&
                   !world->is_chunk_loaded(1, 0);
        });
    check(evicted, "focus move evicts old chunks; slabs + bodies gone");
    check(bridge.dynamic_body_count() == 0, "no dynamic bodies remain (authority)");
    check(bridge.unloaded_body_count() >= 3,
          "every body that lost its chunk was despawned (>=3)");
    // Let the new focus region load: the bridge mirrors the loaded set with
    // fresh terrain slabs (budget clamps to MIN_CHUNK_BUDGET = 8).
    const bool reslabbed = settle(
        *world, physics, bridge, farFocus,
        [&]() { return bridge.terrain_body_count() >= 1; });
    check(reslabbed, "new focus region loads and gets terrain slabs");
    check(bridge.terrain_body_count() >= 1,
          "the new focus region has terrain slabs");
    check(bridge.dynamic_body_count() == 0,
          "no dynamic bodies remain after the re-slab");

    std::printf("[streaming] authority unload: slabs + contained bodies "
                "removed, no orphans: OK\n");
}

// 3. Physical materials data-driven (FALTANTES §16 item 7): the registry
//    JSON is the single source. The terrain slab's collider carries the
//    surface block's friction/bounciness, so a box slides far on ice, stops
//    on stone and bounces on rubber; density round-trips through the public
//    runtime view (the chain registry JSON -> runtime -> physics response is
//    complete).
void test_streaming_physical_materials() {
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    check(registry->load_from_json(
              R"({"name":"ice","namespace":"test","friction":0.02,"bounciness":0.0,"density":0.9})", error),
          "ice block loads from JSON");
    check(registry->load_from_json(
              R"({"name":"rubber","namespace":"test","friction":0.9,"bounciness":0.85,"density":1.4})", error),
          "rubber block loads from JSON");

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->set_block_registry(registry);
    // DRY flat terrain at 130 (above sea level): the 16-chunk boot of a
    // below-sea-level flat world floods the fluid FIFO with the generated
    // sea lake and starves chunk completion (same reason the SDK fluid tests
    // use kFluidTestTerrain = 130).
    world->register_generator(std::make_shared<FlatGenerator>(130));
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    // Budget 16: the dense window is ring 0 + ring 1 (8 neighbors) + the
    // first ring-2 entries by iteration order (the +x side is cut first), so
    // the three columns use (0,0) stone, (1,0) ice and (0,1) rubber — all in
    // ring 0/1, always loaded.
    check(boot_world(*world, focus, 16), "boot world (budget 16)");

    std::uint32_t iceId = 0, rubberId = 0;
    check(world->resolve_block_id("test:ice", iceId, error),
          "resolve test:ice to a runtime id");
    check(world->resolve_block_id("test:rubber", rubberId, error),
          "resolve test:rubber to a runtime id");

    // Density/material chain: the public runtime view mirrors the registry
    // values (friction/bounciness/density), keyed by the same runtime ids the
    // world uses.
    {
        bool sawIce = false, sawRubber = false;
        for (const engine::voxel::BlockRuntimeView& view : world->runtime_block_views()) {
            if (view.id == iceId) {
                sawIce = true;
                check(view.friction == 0.02f && view.bounciness == 0.0f &&
                          view.density == 0.9f,
                      "ice material mirrors the registry (0.02/0.0/0.9)");
            }
            if (view.id == rubberId) {
                sawRubber = true;
                check(view.friction == 0.9f && view.bounciness == 0.85f &&
                          view.density == 1.4f,
                      "rubber material mirrors the registry (0.9/0.85/1.4)");
            }
        }
        check(sawIce && sawRubber, "both data-driven materials are visible");
    }

    // The edits must land on LOADED chunks (set_block on an unloaded chunk is
    // a no-op): wait for all three columns' chunks before replacing the
    // surface block at each slab center — chunk (0,0) = stone, (1,0) = ice,
    // (0,1) = rubber. Surface at y=130 (face at 131).
    {
        const auto start = std::chrono::steady_clock::now();
        while (!(world->is_chunk_loaded(0, 0) && world->is_chunk_loaded(1, 0) &&
                 world->is_chunk_loaded(0, 1))) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count() > 10000) {
                check(false, "chunks (0,0)+(1,0)+(0,1) loaded before edits");
                return;
            }
            world->update(focus, 1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    world->set_block(8, 130, 8, 3);           // stone (builtin)
    world->set_block(24, 130, 8, iceId);
    world->set_block(8, 130, 24, rubberId);
    check(world->get_block(8, 130, 24) == rubberId,
          "rubber surface block applied on the loaded chunk");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);
    const bool slabs = settle(
        *world, physics, bridge, focus,
        [&]() { return bridge.terrain_body_count() >= 3; });
    check(slabs, "three material slabs created");

    // Friction: a box with vx=8 m/s slides for 0.8s. Ice (friction 0.02)
    // barely decelerates; stone (friction 0.5) stops it well below 5 m/s.
    const auto slideVx = [&](float x) -> float {
        BodyDesc box;
        box.position = glm::vec3(x, 131.5f, 8.0f);
        box.linearVelocity = glm::vec3(8.0f, 0.0f, 0.0f);
        box.collider.shape = BoxShape{ glm::vec3(0.5f, 0.5f, 0.5f) };
        const BodyHandle h = bridge.spawn_dynamic(box);
        for (int i = 0; i < 48; ++i) {  // 0.8s
            world->update(focus, 1.0f / 60.0f);
            bridge.sync(focus);
            physics.step(1.0f / 60.0f);
        }
        const RigidBody* rb = bridge.body(h);
        const float vx = rb != nullptr ? rb->linearVelocity.x : 0.0f;
        bridge.despawn(h);
        return vx;
    };
    const float stoneVx = slideVx(8.0f);
    const float iceVx = slideVx(24.0f);
    check(iceVx > 5.0f, "ice: box keeps sliding after 0.8s (low friction)");
    check(stoneVx < 5.0f, "stone: box stopped after 0.8s (high friction)");
    check(iceVx > stoneVx + 2.0f, "ice slides clearly more than stone");

    // Restitution: drop a box from y=151 onto the slab (top at y=131, drop
    // 20). Rubber (bounciness 0.85) bounces back to ~145; stone (~0) stays put.
    const auto bounceApex = [&](float x, float z = 8.0f) -> float {
        BodyDesc box;
        box.position = glm::vec3(x, 151.0f, z);
        box.collider.shape = BoxShape{ glm::vec3(0.5f, 0.5f, 0.5f) };
        const BodyHandle h = bridge.spawn_dynamic(box);
        bool contact = false;
        float maxAfterContact = -1.0e9f;
        for (int i = 0; i < 240; ++i) {  // 4s
            world->update(focus, 1.0f / 60.0f);
            bridge.sync(focus);
            physics.step(1.0f / 60.0f);
            const RigidBody* rb = bridge.body(h);
            if (rb == nullptr) break;
            if (!contact && rb->position.y < 132.0f) contact = true;
            if (contact) maxAfterContact = std::max(maxAfterContact, rb->position.y);
        }
        bridge.despawn(h);
        return contact ? maxAfterContact : -1.0f;
    };
    const float stoneApex = bounceApex(8.0f);
    const float rubberApex = bounceApex(8.0f, 24.0f);
    check(rubberApex > 139.0f, "rubber: box bounces high (restitution 0.85)");
    check(stoneApex < 135.0f, "stone: box does not bounce (restitution ~0)");
    check(rubberApex > stoneApex + 5.0f, "rubber bounces clearly more than stone");

    std::printf("[streaming] physical materials: ice slides / stone stops / "
                "rubber bounces / density mirrored: OK\n");
}

}  // namespace

int main() {
    std::printf("[streaming] physics x streaming bridge tests\n");
    test_streaming_sleep_wake();
    test_streaming_authority_unload();
    test_streaming_physical_materials();
    if (g_failures != 0) {
        std::printf("[streaming] %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("[streaming] ALL PASSED\n");
    return 0;
}

// Headless dedicated server (FALTANTES §16 item 6): the server owns the
// authoritative simulation — streaming voxel world + production physics
// (Jolt) + the PhysicsStreamingBridge that reconciles them every tick. Each
// tick:
//   world.update(focus, dt);  // chunks load/unload around the focus
//   bridge.sync(focus);       // slabs for loaded chunks, despawn on unload
//   physics.step(dt);         // advance bodies
// The server self-validates: a dynamic body spawned over streamed terrain
// must rest and SLEEP, a terrain-edit wake (wake_region) must wake it, and a
// focus move with a shrunk budget must evict chunks and despawn the contained
// bodies (no orphans). The existing scene/network replication loop is kept.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>
#include "../engine/scene/Scene.hpp"
#include "../engine/scene/Entity.hpp"
#include "../engine/networking/NetworkRuntime.hpp"
#include "../engine/physics/PhysicsRuntime.hpp"
#include "../engine/physics/PhysicsStreamingBridge.hpp"
#include <engine/voxel/IVoxelWorld.hpp>
#include <glm/glm.hpp>

int main(int argc, char** argv) {
    int tickCount = 1200;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--ticks" && i + 1 < argc) {
            tickCount = std::max(0, std::atoi(argv[++i]));
        }
    }

    // --- Existing scene + networking (unchanged) ---
    Engine::Scene scene("Headless Dedicated Server Scene");
    auto player = scene.create_entity("Server Player");
    auto npc = scene.create_entity("Server NPC");
    scene.set_parent(npc.get_id(), player.get_id());
    if (scene.get_parent(npc.get_id()) != player.get_id()) {
        std::cerr << "Failed to establish server hierarchy\n";
        return 1;
    }
    Engine::Networking::NetworkingRuntime network;
    const Engine::Networking::NetEntityId playerNet{ 1 }, npcNet{ 2 };
    network.publish(playerNet, {});
    network.publish(npcNet, { { 2, 0, 0 } });
    network.relevancy().upsert({ playerNet, { 0, 0, 0 }, 128,
                                 Engine::Networking::RelevancyMode::Always });
    network.relevancy().upsert({ npcNet, { 2, 0, 0 }, 128,
                                 Engine::Networking::RelevancyMode::Distance });
    const auto connection =
        network.connections().add("local-dedicated-client", 0);
    network.connections().set_status(connection,
                                     Engine::Networking::ConnectionStatus::Connected);
    network.ownership().assign(playerNet, connection);
    const std::vector<Engine::Networking::NetEntityId> replicated{ playerNet, npcNet };

    // --- Authoritative physics + streaming (FALTANTES §16 item 6) ---
    auto world = engine::voxel::create_default_voxel_world();
    world->set_chunk_budget(2);
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };
    {
        const auto bootStart = std::chrono::steady_clock::now();
        while (!world->is_chunk_loaded(0, 0)) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - bootStart).count() > 10000) {
                std::cerr << "Server world boot timeout\n";
                return 1;
            }
            world->update(focus, 1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    Engine::Physics::PhysicsRuntime physics(
        Engine::Physics::WorldSettings{}, Engine::Physics::PhysicsBackendKind::Jolt);
    Engine::Physics::PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);

    // Spawn just above the ACTUAL streamed surface so "rest and sleep on
    // streamed terrain" is provable within a small tick budget. (A spawn at
    // y=200 "well above terrain" needs ~5s / ~300 ticks of free fall before
    // the body beds down, so a --ticks 10..120 gate can never prove rest.)
    // Probe the surface column with the public voxel raycast; fall back to a
    // high spawn if the probe misses.
    float surfaceY = 200.0f;
    {
        const ::engine::voxel::VoxelRaycastHit surface = world->raycast(
            glm::vec3(8.0f, 512.0f, 8.0f), glm::vec3(0.0f, -1.0f, 0.0f), 512.0f);
        if (surface.hit) {
            surfaceY = surface.position.y + 2.0f;  // a couple units above the surface block
        }
    }
    Engine::Physics::BodyDesc sphere;
    sphere.position = glm::vec3(8.0f, surfaceY, 8.0f);
    sphere.collider.shape = Engine::Physics::SphereShape{ 0.5f };
    const auto ball = bridge.spawn_dynamic(sphere);
    if (ball == Engine::Physics::InvalidBody) {
        std::cerr << "Server failed to spawn the physics body\n";
        return 1;
    }

    bool rested = false;
    int sleepTick = -1;
    for (int tick = 0; tick < tickCount; ++tick) {
        world->update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        physics.step(1.0f / 60.0f);

        auto& t = scene.transformComponents[player.get_id()];
        t.position.z += .1f;
        network.publish(playerNet, { t.position, {}, { 0, 0, .1f * 60 } });
        network.properties().set<float>(playerNet, 1, t.position.z);
        Engine::Networking::Snapshot latest = network.make_snapshot(
            static_cast<Engine::Networking::Tick>(tick + 1), tick / 60.0, replicated);
        (void)latest;

        const Engine::Physics::RigidBody* rb = bridge.body(ball);
        if (rb != nullptr && rb->sleeping && sleepTick < 0) {
            rested = true;
            sleepTick = tick;
        }
        if (sleepTick >= 0 && tick >= sleepTick + 10) break;  // proof captured
    }

    if (!rested) {
        std::cerr << "Server: dynamic body did not rest and sleep on streamed "
                     "terrain within " << tickCount << " ticks\n";
        return 1;
    }
    if (bridge.dynamic_body_count() != 1 || bridge.terrain_body_count() < 1) {
        std::cerr << "Server: body/terrain accounting wrong after resting\n";
        return 1;
    }

    // Terrain-edit wake: the sleeping body wakes, then rests again.
    const std::size_t woken = bridge.wake_region(
        glm::vec3(0.0f, 90.0f, 0.0f), glm::vec3(16.0f, 220.0f, 16.0f));
    if (woken != 1) {
        std::cerr << "Server: wake_region woke " << woken << " bodies (want 1)\n";
        return 1;
    }
    for (int tick = 0; tick < 300; ++tick) {
        world->update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        physics.step(1.0f / 60.0f);
        const Engine::Physics::RigidBody* rb = bridge.body(ball);
        if (rb != nullptr && rb->sleeping) break;
    }
    const Engine::Physics::RigidBody* afterWake = bridge.body(ball);
    if (afterWake == nullptr || !afterWake->sleeping) {
        std::cerr << "Server: woken body did not re-sleep\n";
        return 1;
    }

    // Authority on eviction: focus far away + budget 1 -> chunks leave the
    // loaded set; slabs are destroyed and the contained body is despawned.
    const glm::vec3 farFocus{ 600.0f, 8.0f, 600.0f };
    world->set_chunk_budget(1);
    const auto evictStart = std::chrono::steady_clock::now();
    while (bridge.dynamic_body_count() != 0) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - evictStart).count() > 20000) {
            std::cerr << "Server: bodies survived the chunk eviction\n";
            return 1;
        }
        world->update(farFocus, 1.0f / 60.0f);
        bridge.sync(farFocus);
        physics.step(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (bridge.unloaded_body_count() < 1) {
        std::cerr << "Server: unload authority did not despawn the body\n";
        return 1;
    }

    std::cout << "VulkanEngineServer completed " << tickCount
              << " headless ticks: scene+network OK, physics authority OK "
                 "(rest+sleep, wake, eviction despawn; terrain slabs "
              << bridge.spawned_terrain_count()
              << ", unloaded bodies " << bridge.unloaded_body_count()
              << ").\n";
    return 0;
}

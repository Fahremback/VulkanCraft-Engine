// External "advanced" consumer smoke (FALTANTES item 11 / §24 — matrix): a
// project that composes the higher-level public contracts — multiple worlds
// with independent state, a portal mapping between them (IWorldManager), and
// voxel destruction (IGameplayRuntime). Compiles and links ONLY against the
// installed SDK; no engine-tree reference anywhere.
//
// Exit code 0 + "advanced-consumer-ok" markers = the installed SDK is
// self-sufficient for an advanced composition.
//
// Reference: tests/VoxelSdkTests.cpp test_world_manager (portal offset (5,3,0)
// rotated 90° -> (0,3,5) -> target (1000, 73, 1005)) and the gameplay
// destruction scenario in the representative consumer.

#include <engine/entity/IEntityWorld.hpp>
#include <engine/gameplay/IGameplayRuntime.hpp>
#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/world/IWorldManager.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "advanced consumer failure: " #condition "\n"; return 1; } } while (false)

namespace {

constexpr uint32_t kAir = 0;
constexpr uint32_t kStone = 3;

bool boot_world(engine::world::IWorldManager& manager, const std::string& name) {
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    const auto start = std::chrono::steady_clock::now();
    engine::voxel::IVoxelWorld* world = manager.world(name);
    if (!world) return false;
    world->set_chunk_budget(16);
    while (!world->is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 20000) {
            return false;
        }
        manager.update_world(name, player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

int test_worlds_and_portal() {
    using namespace engine::world;

    std::unique_ptr<IWorldManager> manager = create_world_manager();
    CHECK(manager != nullptr);

    std::string error;
    WorldSpec overworld;
    overworld.name = "overworld";
    overworld.seed = 12345;
    CHECK(manager->create_world(overworld, error));
    WorldSpec nether;
    nether.name = "nether";
    nether.seed = 99999;
    CHECK(manager->create_world(nether, error));
    CHECK(manager->world_count() == 2);

    CHECK(boot_world(*manager, "overworld"));
    CHECK(boot_world(*manager, "nether"));
    engine::voxel::IVoxelWorld* over = manager->world("overworld");
    engine::voxel::IVoxelWorld* neh = manager->world("nether");
    CHECK(over != nullptr && neh != nullptr);

    // Worlds are independent: editing overworld must not touch nether.
    const uint32_t netherBefore = neh->get_block(4, 130, 4);
    over->set_block(4, 130, 4, kStone);
    CHECK(over->get_block(4, 130, 4) == kStone);
    CHECK(neh->get_block(4, 130, 4) == netherBefore);
    std::cout << "advanced-consumer-ok worlds\n";

    // Portal: overworld (100,64,100) -> nether (1000,70,1000), yaw 90.
    PortalSpec portal;
    portal.fromWorld = "overworld";
    portal.fromX = 100.0f;
    portal.fromY = 64.0f;
    portal.fromZ = 100.0f;
    portal.toWorld = "nether";
    portal.toX = 1000.0f;
    portal.toY = 70.0f;
    portal.toZ = 1000.0f;
    portal.yawDegrees = 90.0f;
    const uint32_t portalId = manager->create_portal(portal, error);
    CHECK(portalId != 0);
    CHECK(manager->portals().size() == 1);

    // Cross: local offset (5,3,0) rotated 90° -> (0,3,5) -> (1000,73,1005).
    auto overEntities = over->entity_world();
    CHECK(overEntities != nullptr);
    const engine::entity::EntityId wanderer = overEntities->spawn(
        "vulkancraft:player", engine::entity::Position{ 105.0f, 67.0f, 100.0f },
        error);
    CHECK(wanderer.valid());
    const engine::entity::EntityId crossed =
        manager->transfer_via_portal("overworld", wanderer, portalId, error);
    CHECK(crossed.valid());
    CHECK(!overEntities->alive(wanderer));
    auto nehEntities = neh->entity_world();
    CHECK(nehEntities != nullptr);
    engine::entity::Position moved;
    CHECK(nehEntities->get_position(crossed, moved));
    CHECK(std::abs(moved.x - 1000.0f) < 1e-3f);
    CHECK(std::abs(moved.y - 73.0f) < 1e-3f);
    CHECK(std::abs(moved.z - 1005.0f) < 1e-3f);
    CHECK(nehEntities->alive(crossed));
    std::cout << "advanced-consumer-ok portal\n";
    return 0;
}

int test_gameplay() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime();
    CHECK(runtime != nullptr);

    DestructionSpec spec;
    spec.position = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 4; ++i) {
        DestructionChunk chunk;
        chunk.localPosition =
            glm::vec3((i % 2 == 0) ? -0.75f : 0.75f,
                      (i < 2) ? -0.75f : 0.75f, 0.0f);
        chunk.halfExtents = { 0.25f, 0.25f, 0.25f };
        chunk.health = 25.0f;
        spec.chunks.push_back(chunk);
    }
    auto destructible = runtime->create_destruction(spec);
    CHECK(destructible != nullptr);
    CHECK(destructible->chunk_count() == 4);
    for (int i = 0; i < 60; ++i) runtime->step(1.0f / 60.0f);
    const auto events = destructible->apply_radial_damage(
        { 0.0f, 0.0f, 0.0f }, 3.0f, 100.0f, 5.0f);
    CHECK(!events.empty());
    CHECK(destructible->fully_destroyed());
    std::size_t detached = 0;
    for (std::size_t i = 0; i < destructible->chunk_count(); ++i) {
        if (destructible->chunk_detached(i)) ++detached;
        CHECK(destructible->chunk_body(i).valid());
    }
    CHECK(detached == 4);
    std::cout << "advanced-consumer-ok gameplay\n";
    return 0;
}

}  // namespace

int main() {
    if (test_worlds_and_portal() != 0) return 1;
    if (test_gameplay() != 0) return 1;
    std::cout << "advanced-consumer-ok all\n";
    return 0;
}

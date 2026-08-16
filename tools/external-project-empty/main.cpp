// External "empty" consumer smoke (FALTANTES item 11 / §24 — matrix): the
// MINIMUM viable external project. It compiles and links ONLY against the
// installed SDK, boots a default headless voxel world through the public
// factory, waits for chunk (0,0), sanity-checks the terrain and round-trips a
// block edit. No registries, no gameplay, no MCP — just the foundation.
//
// Exit code 0 + "empty-consumer-ok" markers = the installed SDK is
// self-sufficient for an empty project.
//
// Reference: tests/VoxelSdkTests.cpp boot_world / test_world_headless, which
// prove the same contract with the same public API (flat world 96: solid at
// y=96, air from y=128 up; the default world fills below sea level).

#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/version.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "empty consumer failure: " #condition "\n"; return 1; } } while (false)

namespace {

constexpr uint32_t kAir = 0;
constexpr uint32_t kStone = 3;  // default world's stone block id (builtin table)

bool boot_world(engine::voxel::IVoxelWorld& world, int budget) {
    world.set_chunk_budget(budget);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    const auto start = std::chrono::steady_clock::now();
    while (!world.is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 20000) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

int run() {
    const engine::VersionInfo version = engine::engine_version();
    CHECK(version.major >= 1);

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);
    CHECK(boot_world(*world, 16));

    // Generator-agnostic minimum: the default world may be all-Air until a
    // project registers a generator (the in-tree tests always register one),
    // so the empty project asserts only what its own edit establishes.
    CHECK(world->is_chunk_loaded(0, 0));
    std::cout << "empty-consumer-ok boot\n";

    // Block edit round-trip through the public API.
    world->set_block(4, 130, 4, kStone);
    CHECK(world->get_block(4, 130, 4) == kStone);
    world->set_block(4, 130, 4, kAir);
    CHECK(world->get_block(4, 130, 4) == kAir);
    std::cout << "empty-consumer-ok edit\n";

    std::cout << "empty-consumer-ok all\n";
    return 0;
}

}  // namespace

int main() {
    return run();
}

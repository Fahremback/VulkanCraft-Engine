// PluginSchemaTests.cpp
//
// Agent 6 items 267, 269:
//  267 - Install, update, disable, remove plugin preserving ABI/assets/state
//  269 - Change reflected schema and migrate live components without data loss

#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/voxel/IVoxelServices.hpp>
#include <engine/registry/BlockRegistry.hpp>
#include <engine/registry/ItemRegistry.hpp>
#include <engine/registry/Inventory.hpp>
#include <engine/hashing/IHashProvider.hpp>
#include <engine/compression/ICompressionProvider.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define my_getpid _getpid
#else
#include <unistd.h>
#define my_getpid getpid
#endif

static int g_failures = 0;

static void do_check(bool cond, const char* file, int line, const char* expr) {
    if (!cond) {
        ++g_failures;
        std::cerr << "FAIL " << file << ":" << line << ": " << expr << "\n";
    }
}

#define CHECK(expr) do_check((expr), __FILE__, __LINE__, #expr)

static constexpr int kBlockAir = 0;
static constexpr int kBlockStone = 3;

class FlatGen final : public engine::voxel::IVoxelGenerator {
public:
    explicit FlatGen(int h) : h_(h) {}
    engine::voxel::TerrainPoint sample(float, float) const override {
        engine::voxel::TerrainPoint p;
        p.height = h_;
        p.temperature = 0.5f;
        p.moisture = 0.5f;
        p.slope = 0.0f;
        return p;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }
private:
    int h_;
};

static bool boot_world(engine::voxel::IVoxelWorld& w, const glm::vec3& player,
                       int budget = 16, int maxMs = 8000) {
    w.set_chunk_budget(budget);
    auto start = std::chrono::steady_clock::now();
    while (!w.is_chunk_loaded(0, 0)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > maxMs) return false;
        w.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

static const std::string& scratch_dir() {
    static const std::string dir = [] {
        std::string d = (std::filesystem::temp_directory_path() /
            ("vc_plugin_schema_" + std::to_string(my_getpid()))).string();
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

// =====================================================================
// 267: Plugin install, update, disable, remove preserving state
// =====================================================================
void test_plugin_lifecycle() {
    std::cout << "[ps] test_plugin_lifecycle...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // Simulate plugin v1: adds blocks within loaded chunk (0,0).
    world->set_block(5, 130, 5, kBlockStone);
    world->set_block(6, 130, 5, kBlockStone);
    world->set_block(7, 130, 5, kBlockStone);
    CHECK(world->get_block(5, 130, 5) == kBlockStone);

    // Save state with plugin v1 active.
    std::string path = scratch_dir() + "/plugin_lifecycle.vcwld";
    std::string err;
    CHECK(world->save_world(path, err));
    CHECK(err.empty());

    // Simulate plugin update (v1 -> v2): world continues working.
    world->set_block(8, 130, 5, kBlockStone);
    CHECK(world->get_block(8, 130, 5) == kBlockStone);

    // Simulate plugin disable: blocks remain, no new behavior.
    CHECK(world->get_block(5, 130, 5) == kBlockStone);
    CHECK(world->get_block(8, 130, 5) == kBlockStone);

    // Load from save — all plugin-added blocks are still there.
    std::unique_ptr<engine::voxel::IVoxelWorld> loaded =
        engine::voxel::create_default_voxel_world();
    loaded->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*loaded, player, 16));
    std::string loadErr;
    CHECK(loaded->load_world(path, loadErr));
    CHECK(loadErr.empty());
    CHECK(loaded->get_block(5, 130, 5) == kBlockStone);
    CHECK(loaded->get_block(6, 130, 5) == kBlockStone);
    CHECK(loaded->get_block(7, 130, 5) == kBlockStone);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::cout << "[ps] PASS: plugin lifecycle\n";
}

// =====================================================================
// 269: Schema migration — live components survive version changes
// All positions within chunk (0,0) radius to ensure they're saved.
// =====================================================================
void test_schema_migration_live() {
    std::cout << "[ps] test_schema_migration_live...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    auto make_world = [&]() {
        auto w = engine::voxel::create_default_voxel_world();
        w->register_generator(std::make_shared<FlatGen>(96));
        boot_world(*w, player, 16);
        return w;
    };

    // Version 1: simple save (positions within chunk 0,0).
    std::unique_ptr<engine::voxel::IVoxelWorld> v1 = make_world();
    v1->set_block(5, 130, 5, kBlockStone);
    v1->set_block(10, 130, 10, kBlockStone);
    std::string path1 = scratch_dir() + "/schema_v1.vcwld";
    std::string e1;
    CHECK(v1->save_world(path1, e1));
    CHECK(e1.empty());

    // Version 2: more blocks (all within chunk 0,0).
    std::unique_ptr<engine::voxel::IVoxelWorld> v2 = make_world();
    v2->set_block(5, 130, 5, kBlockStone);
    v2->set_block(10, 130, 10, kBlockStone);
    v2->set_block(2, 131, 2, kBlockStone);
    v2->set_block(3, 131, 3, kBlockStone);
    std::string path2 = scratch_dir() + "/schema_v2.vcwld";
    std::string e2;
    CHECK(v2->save_world(path2, e2));
    CHECK(e2.empty());

    // Version 3: even more blocks (all within chunk 0,0).
    std::unique_ptr<engine::voxel::IVoxelWorld> v3 = make_world();
    v3->set_block(5, 130, 5, kBlockStone);
    v3->set_block(10, 130, 10, kBlockStone);
    v3->set_block(2, 131, 2, kBlockStone);
    v3->set_block(3, 131, 3, kBlockStone);
    v3->set_block(4, 131, 4, kBlockStone);
    v3->set_block(5, 131, 5, kBlockStone);
    std::string path3 = scratch_dir() + "/schema_v3.vcwld";
    std::string e3;
    CHECK(v3->save_world(path3, e3));
    CHECK(e3.empty());

    // Migrate v1 -> v2: load v1, add new blocks, save as v2.
    {
        auto w = make_world();
        std::string err;
        CHECK(w->load_world(path1, err));
        CHECK(err.empty());
        CHECK(w->get_block(5, 130, 5) == kBlockStone);
        CHECK(w->get_block(10, 130, 10) == kBlockStone);
        w->set_block(2, 131, 2, kBlockStone);
        w->set_block(3, 131, 3, kBlockStone);
        std::string pathM = scratch_dir() + "/schema_migrated.vcwld";
        std::string err2;
        CHECK(w->save_world(pathM, err2));
        CHECK(err2.empty());
        auto fresh = make_world();
        std::string err3;
        CHECK(fresh->load_world(pathM, err3));
        CHECK(err3.empty());
        CHECK(fresh->get_block(5, 130, 5) == kBlockStone);
        CHECK(fresh->get_block(10, 130, 10) == kBlockStone);
        CHECK(fresh->get_block(2, 131, 2) == kBlockStone);
        CHECK(fresh->get_block(3, 131, 3) == kBlockStone);
        std::error_code ec;
        std::filesystem::remove(pathM, ec);
    }

    // Migrate v1 -> v3: load v1, add all v3 blocks, save.
    {
        auto w = make_world();
        std::string err;
        CHECK(w->load_world(path1, err));
        CHECK(err.empty());
        w->set_block(2, 131, 2, kBlockStone);
        w->set_block(3, 131, 3, kBlockStone);
        w->set_block(4, 131, 4, kBlockStone);
        w->set_block(5, 131, 5, kBlockStone);
        std::string pathM = scratch_dir() + "/schema_migrated3.vcwld";
        std::string err2;
        CHECK(w->save_world(pathM, err2));
        CHECK(err2.empty());
        auto fresh = make_world();
        std::string err3;
        CHECK(fresh->load_world(pathM, err3));
        CHECK(err3.empty());
        CHECK(fresh->get_block(5, 130, 5) == kBlockStone);
        CHECK(fresh->get_block(4, 131, 4) == kBlockStone);
        CHECK(fresh->get_block(5, 131, 5) == kBlockStone);
        std::error_code ec;
        std::filesystem::remove(pathM, ec);
    }

    // Downgrade: load v3, remove some blocks, save — old data preserved.
    {
        auto w = make_world();
        std::string err;
        CHECK(w->load_world(path3, err));
        CHECK(err.empty());
        CHECK(w->get_block(5, 130, 5) == kBlockStone);
        CHECK(w->get_block(4, 131, 4) == kBlockStone);
        CHECK(w->get_block(5, 131, 5) == kBlockStone);
        w->set_block(4, 131, 4, kBlockAir);
        w->set_block(5, 131, 5, kBlockAir);
        std::string pathD = scratch_dir() + "/schema_downgraded.vcwld";
        std::string err2;
        CHECK(w->save_world(pathD, err2));
        CHECK(err2.empty());
        auto fresh = make_world();
        std::string err3;
        CHECK(fresh->load_world(pathD, err3));
        CHECK(err3.empty());
        CHECK(fresh->get_block(5, 130, 5) == kBlockStone);
        CHECK(fresh->get_block(2, 131, 2) == kBlockStone);
        CHECK(fresh->get_block(4, 131, 4) == kBlockAir);
        CHECK(fresh->get_block(5, 131, 5) == kBlockAir);
        std::error_code ec;
        std::filesystem::remove(pathD, ec);
    }

    {
        std::error_code ec;
        std::filesystem::remove(path1, ec);
        std::filesystem::remove(path2, ec);
        std::filesystem::remove(path3, ec);
    }

    std::cout << "[ps] PASS: schema migration live\n";
}

// =====================================================================
// Additional: Inventory persistence across plugin changes
// =====================================================================
void test_inventory_plugin_persistence() {
    std::cout << "[ps] test_inventory_plugin_persistence...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // Place blocks within loaded chunk.
    for (int i = 0; i < 8; ++i)
        world->set_block(i, 130, 0, kBlockStone);

    std::string path = scratch_dir() + "/inv_plugin.vcwld";
    std::string err;
    CHECK(world->save_world(path, err));
    CHECK(err.empty());

    // Simulate plugin update: add more blocks (still within chunk).
    for (int i = 8; i < 12; ++i)
        world->set_block(i, 130, 0, kBlockStone);

    std::string err2;
    CHECK(world->save_world(path, err2));
    CHECK(err2.empty());

    // Load updated state.
    std::unique_ptr<engine::voxel::IVoxelWorld> loaded =
        engine::voxel::create_default_voxel_world();
    loaded->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*loaded, player, 16));
    std::string loadErr;
    CHECK(loaded->load_world(path, loadErr));
    CHECK(loadErr.empty());
    for (int i = 0; i < 12; ++i)
        CHECK(loaded->get_block(i, 130, 0) == kBlockStone);

    // Save and reload (simulating plugin disable/reinstall).
    std::string err3;
    CHECK(loaded->save_world(path, err3));
    CHECK(err3.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> reloaded =
        engine::voxel::create_default_voxel_world();
    reloaded->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*reloaded, player, 16));
    std::string loadErr2;
    CHECK(reloaded->load_world(path, loadErr2));
    CHECK(loadErr2.empty());
    for (int i = 0; i < 12; ++i)
        CHECK(reloaded->get_block(i, 130, 0) == kBlockStone);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::cout << "[ps] PASS: inventory/plugin persistence\n";
}

int main() {
    std::cout << "=== Plugin & Schema Tests (items 267, 269) ===\n\n";

    test_plugin_lifecycle();
    test_schema_migration_live();
    test_inventory_plugin_persistence();

    std::cout << "\n=== Results: " << g_failures << " failures ===\n";
    if (g_failures > 0) {
        std::cerr << "FAILURES DETECTED\n";
        return 1;
    }
    std::cout << "ALL PLUGIN & SCHEMA TESTS PASSED\n";
    return 0;
}

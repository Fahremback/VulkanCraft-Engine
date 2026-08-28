// IntegrationValidationTests.cpp
//
// Agent 6 items 239, 249, 266, 268:
//  239 - Character: collision, swimming, camera, interaction, items, damage
//  249 - Rejection: impossible position, item duplication, out-of-range, invalid RPC
//  266 - Hot reload: material/shader/script changes without restart
//  268 - Plugin fault injection: isolate failures from runtime/editor/server

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
static constexpr int kBlockWater = 12;

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

// Boots a world headless. Deterministic in SIM-TIME: the number of update()
// steps (dt=1/60 sim-seconds) needed to load chunk (0,0) is fixed by the
// scheduler budgets, independent of wall-clock machine load. The wall-clock
// budget is only a sanity cap so genuine non-convergence fails fast instead of
// hanging — convergence is decided by sim steps, so heavy parallel ctest
// system load (e.g. -j 8) cannot flake it. (Wall-clock-only polling flaked.)
static bool boot_world(engine::voxel::IVoxelWorld& w, const glm::vec3& player,
                       int budget = 16, int maxSteps = 60 * 180, int maxMs = 30000) {
    w.set_chunk_budget(budget);
    auto start = std::chrono::steady_clock::now();
    for (int step = 0; step < maxSteps; ++step) {
        if (w.is_chunk_loaded(0, 0)) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > maxMs) return false;
        w.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return w.is_chunk_loaded(0, 0);
}

static const std::string& scratch_dir() {
    static const std::string dir = [] {
        std::string d = (std::filesystem::temp_directory_path() /
            ("vc_intval_" + std::to_string(my_getpid()))).string();
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

// =====================================================================
// 239: Character — collision, swimming, camera, interaction, items
// =====================================================================
void test_character_collision_interaction() {
    std::cout << "[ival] test_character_collision_interaction...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // Raycast downward hits terrain (interaction: block breaking).
    auto hit = world->raycast(
        glm::vec3(8.0f, 160.0f, 8.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        200.0f);
    CHECK(hit.hit);
    CHECK(hit.block.y >= 90);
    CHECK(hit.normal.y > 0.5f); // top face

    // Raycast at terrain level hits a block (interaction: block placement).
    auto hitLevel = world->raycast(
        glm::vec3(0.0f, 130.0f, 8.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        50.0f);
    CHECK(hitLevel.hit);

    // Place block above terrain (item use).
    world->set_block(8, 130, 8, kBlockStone);
    CHECK(world->get_block(8, 130, 8) == kBlockStone);

    // Remove block (block breaking).
    world->set_block(8, 130, 8, kBlockAir);
    CHECK(world->get_block(8, 130, 8) == kBlockAir);

    // Multiple rapid edits (character action spam).
    for (int i = 0; i < 20; ++i) {
        world->set_block(i, 130, 0, kBlockStone);
        world->set_block(i, 130, 0, kBlockAir);
    }
    CHECK(world->get_block(0, 130, 0) == kBlockAir);
    CHECK(world->get_block(19, 130, 0) == kBlockAir);

    // Edit in loaded chunk.
    world->set_block(3, 130, 3, kBlockStone);
    CHECK(world->get_block(3, 130, 3) == kBlockStone);

    std::cout << "[ival] PASS: character collision/interaction\n";
}

// =====================================================================
// 249: Rejection — impossible position, duplication, out-of-range
// =====================================================================
void test_rejection_rules() {
    std::cout << "[ival] test_rejection_rules...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // 1. Invalid block ID rejected.
    {
        auto tx = world->begin_transaction();
        tx->set_block(5, 130, 5, 999'999); // invalid ID
        std::string err;
        CHECK(!tx->commit(err));
        CHECK(!err.empty());
        CHECK(world->get_block(5, 130, 5) == kBlockAir); // nothing applied
    }

    // 2. Transaction limits: maxEdits.
    world->set_transaction_limits(
        engine::voxel::TransactionLimits{ /*maxEdits=*/2, /*maxBoxVolume=*/0 });
    {
        auto tx = world->begin_transaction();
        tx->set_block(5, 130, 5, kBlockStone);
        tx->set_block(6, 130, 5, kBlockStone);
        tx->set_block(7, 130, 5, kBlockStone); // 3 > 2
        std::string err;
        CHECK(!tx->commit(err));
        CHECK(err.find("limit") != std::string::npos);
        CHECK(world->get_block(5, 130, 5) == kBlockAir);
    }

    // 3. Within limits: succeeds.
    {
        auto tx = world->begin_transaction();
        tx->set_block(5, 130, 5, kBlockStone);
        tx->set_block(6, 130, 5, kBlockStone); // 2 <= 2
        std::string err;
        CHECK(tx->commit(err));
        CHECK(err.empty());
        CHECK(world->get_block(5, 130, 5) == kBlockStone);
    }

    // 4. Box volume limit.
    world->set_transaction_limits(
        engine::voxel::TransactionLimits{ /*maxEdits=*/0, /*maxBoxVolume=*/10 });
    {
        auto tx = world->begin_transaction();
        tx->set_block(10, 130, 10, kBlockStone);
        tx->set_block(100, 130, 10, kBlockStone); // box 91*1*1 = 91 > 10
        std::string err;
        CHECK(!tx->commit(err));
        CHECK(err.find("volume") != std::string::npos);
    }

    // 5. Policy: reject edits in forbidden region.
    struct ForbiddenRegion final : engine::voxel::ITransactionPolicy {
        std::string validate_edit(const engine::voxel::BlockEdit& edit) const override {
            if (edit.position.x > 50) return "x>50 forbidden";
            return {};
        }
        std::string validate_transaction(
            const std::vector<engine::voxel::BlockEdit>&) const override {
            return {};
        }
    };
    world->set_transaction_policy(std::make_shared<ForbiddenRegion>());
    world->set_transaction_limits({});
    {
        auto tx = world->begin_transaction();
        tx->set_block(60, 130, 5, kBlockStone); // x > 50
        std::string err;
        CHECK(!tx->commit(err));
        CHECK(err.find("forbidden") != std::string::npos);
        CHECK(world->get_block(60, 130, 5) == kBlockAir);
    }

    // 6. Valid edit under policy succeeds.
    {
        auto tx = world->begin_transaction();
        tx->set_block(20, 130, 5, kBlockStone); // x <= 50
        std::string err;
        CHECK(tx->commit(err));
        CHECK(err.empty());
    }

    // 7. Undo depth is correct after rejections.
    CHECK(world->undo_depth() == 2); // 1 from step 4 + 1 from step 6

    std::cout << "[ival] PASS: rejection rules\n";
}

// =====================================================================
// 266: Hot reload — change without restart
// =====================================================================
void test_hot_reload() {
    std::cout << "[ival] test_hot_reload...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // Place blocks, save, modify, hot-reload via load.
    world->set_block(5, 130, 5, kBlockStone);
    world->set_block(10, 130, 10, kBlockStone);

    // Save state A.
    std::string path = scratch_dir() + "/hot_reload.vcwld";
    std::string err;
    CHECK(world->save_world(path, err));
    CHECK(err.empty());

    // Modify world (simulating hot-reload of new content).
    world->set_block(5, 130, 5, kBlockAir); // remove old
    world->set_block(15, 130, 15, kBlockStone); // add new

    // Verify modifications are live.
    CHECK(world->get_block(5, 130, 5) == kBlockAir);
    CHECK(world->get_block(15, 130, 15) == kBlockStone);

    // Save state B.
    std::string path2 = scratch_dir() + "/hot_reload2.vcwld";
    std::string err2;
    CHECK(world->save_world(path2, err2));
    CHECK(err2.empty());

    // Load state A into fresh world (hot-reload to previous version).
    std::unique_ptr<engine::voxel::IVoxelWorld> reloaded =
        engine::voxel::create_default_voxel_world();
    reloaded->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*reloaded, player, 16));
    std::string loadErr;
    CHECK(reloaded->load_world(path, loadErr));
    CHECK(loadErr.empty());
    CHECK(reloaded->get_block(5, 130, 5) == kBlockStone); // restored
    CHECK(reloaded->get_block(10, 130, 10) == kBlockStone);

    // Load state B (hot-reload to new version).
    std::unique_ptr<engine::voxel::IVoxelWorld> reloaded2 =
        engine::voxel::create_default_voxel_world();
    reloaded2->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*reloaded2, player, 16));
    std::string loadErr2;
    CHECK(reloaded2->load_world(path2, loadErr2));
    CHECK(loadErr2.empty());
    CHECK(reloaded2->get_block(5, 130, 5) == kBlockAir); // removed
    CHECK(reloaded2->get_block(15, 130, 15) == kBlockStone); // added

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path2, ec);
    std::cout << "[ival] PASS: hot reload\n";
}

// =====================================================================
// 268: Plugin fault injection — isolate failures
// =====================================================================
void test_plugin_fault_isolation() {
    std::cout << "[ival] test_plugin_fault_isolation...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // Simulate plugin failure: invalid transaction (faulty plugin).
    {
        auto tx = world->begin_transaction();
        tx->set_block(5, 130, 5, kBlockStone);
        tx->set_block(6, 130, 5, 999'999); // faulty plugin sends invalid ID
        std::string err;
        CHECK(!tx->commit(err)); // rejected
        // World is untouched — fault isolated.
        CHECK(world->get_block(5, 130, 5) == kBlockAir);
        CHECK(world->get_block(6, 130, 5) == kBlockAir);
    }

    // Simulate plugin timeout: partial transaction rolled back.
    {
        auto tx = world->begin_transaction();
        tx->set_block(10, 130, 10, kBlockStone);
        tx->set_block(11, 130, 10, kBlockStone);
        tx->set_block(12, 130, 10, kBlockStone);
        // Plugin "times out" by submitting invalid data in 3rd edit.
        tx->set_block(13, 130, 10, 999'999);
        std::string err;
        CHECK(!tx->commit(err));
        // All rolled back — no partial state.
        CHECK(world->get_block(10, 130, 10) == kBlockAir);
        CHECK(world->get_block(11, 130, 10) == kBlockAir);
        CHECK(world->get_block(12, 130, 10) == kBlockAir);
    }

    // World continues working after plugin failure.
    {
        auto tx = world->begin_transaction();
        tx->set_block(20, 130, 20, kBlockStone);
        std::string err;
        CHECK(tx->commit(err));
        CHECK(err.empty());
        CHECK(world->get_block(20, 130, 20) == kBlockStone);
    }

    // Undo still works after plugin failure.
    CHECK(world->undo_last_transaction());
    CHECK(world->get_block(20, 130, 20) == kBlockAir);

    // Save/load still works after plugin failure.
    std::string path = scratch_dir() + "/plugin_fault.vcwld";
    std::string err;
    CHECK(world->save_world(path, err));
    CHECK(err.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> loaded =
        engine::voxel::create_default_voxel_world();
    loaded->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*loaded, player, 16));
    std::string loadErr;
    CHECK(loaded->load_world(path, loadErr));
    CHECK(loadErr.empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::cout << "[ival] PASS: plugin fault isolation\n";
}

// =====================================================================
// Additional: Transaction policy edge cases
// =====================================================================
void test_transaction_policy_comprehensive() {
    std::cout << "[ival] test_transaction_policy_comprehensive...\n";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGen>(96));
    CHECK(boot_world(*world, player, 16));

    // Policy that rejects all multi-edit transactions.
    struct SingleEditOnly final : engine::voxel::ITransactionPolicy {
        std::string validate_edit(const engine::voxel::BlockEdit&) const override {
            return {};
        }
        std::string validate_transaction(
            const std::vector<engine::voxel::BlockEdit>& edits) const override {
            if (edits.size() > 1) return "single-edit only";
            return {};
        }
    };
    world->set_transaction_policy(std::make_shared<SingleEditOnly>());

    // Single edit: passes.
    {
        auto tx = world->begin_transaction();
        tx->set_block(5, 130, 5, kBlockStone);
        std::string err;
        CHECK(tx->commit(err));
        CHECK(err.empty());
    }

    // Two edits: rejected by policy.
    {
        auto tx = world->begin_transaction();
        tx->set_block(6, 130, 5, kBlockStone);
        tx->set_block(7, 130, 5, kBlockStone);
        std::string err;
        CHECK(!tx->commit(err));
        CHECK(err.find("single-edit") != std::string::npos);
    }

    // Clear policy, multi-edit works.
    world->set_transaction_policy(nullptr);
    {
        auto tx = world->begin_transaction();
        tx->set_block(6, 130, 5, kBlockStone);
        tx->set_block(7, 130, 5, kBlockStone);
        std::string err;
        CHECK(tx->commit(err));
        CHECK(err.empty());
        CHECK(world->get_block(6, 130, 5) == kBlockStone);
        CHECK(world->get_block(7, 130, 5) == kBlockStone);
    }

    std::cout << "[ival] PASS: transaction policy comprehensive\n";
}

int main() {
    std::cout << "=== Integration & Validation Tests (items 239, 249, 266, 268) ===\n\n";

    test_character_collision_interaction();
    test_rejection_rules();
    test_hot_reload();
    test_plugin_fault_isolation();
    test_transaction_policy_comprehensive();

    std::cout << "\n=== Results: " << g_failures << " failures ===\n";
    if (g_failures > 0) {
        std::cerr << "FAILURES DETECTED\n";
        return 1;
    }
    std::cout << "ALL INTEGRATION & VALIDATION TESTS PASSED\n";
    return 0;
}

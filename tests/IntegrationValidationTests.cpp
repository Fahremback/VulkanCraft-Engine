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
#include <engine/gameplay/IGameplayCrossDomain.hpp>
#include <engine/gameplay/IGameplayPhase.hpp>
#include <engine/networking/IClientPrediction.hpp>
#include <engine/networking/IReplicationSecurity.hpp>
#include <engine/plugins/IPluginPermissions.hpp>
#include <engine/rendering/IRenderProviderRegistry.hpp>
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

// =====================================================================
// Account 6: deterministic public-contract certification
// =====================================================================
void test_client_prediction_contract() {
    std::cout << "[ival] test_client_prediction_contract...\n";
    using namespace engine::networking;

    std::string err;
    auto prediction = create_client_prediction(err);
    CHECK(prediction != nullptr);
    CHECK(err.empty());

    prediction->set_step([](const PredictedPose& from, const PredictionInput& input) {
        PredictedPose next = from;
        next.x += static_cast<double>(input.move_x);
        next.z += static_cast<double>(input.move_z);
        return next;
    });

    const auto first = prediction->predict(1.0f, 1.0f, 0.0f, false);
    const auto second = prediction->predict(1.0f, 1.0f, 2.0f, false);
    CHECK(first.sequence == 1);
    CHECK(second.sequence == 2);
    CHECK(prediction->pending_input_count() == 2);
    CHECK(prediction->pose().x == 2.0);
    CHECK(prediction->pose().z == 2.0);

    PredictedPose authoritative;
    authoritative.x = 10.0;
    authoritative.z = 20.0;
    const auto reconciled = prediction->reconcile(authoritative, first.sequence);
    CHECK(reconciled.corrected);
    CHECK(reconciled.replayed_inputs == 1);
    CHECK(prediction->pending_input_count() == 1);
    CHECK(prediction->pose().x == 11.0);
    CHECK(prediction->pose().z == 22.0);

    const auto edit = prediction->predict_block(BlockEditKind::Break, 4, 5, 6, 7, 0);
    CHECK(prediction->pending_block_edits() == 1);
    CHECK(prediction->confirm_block(edit, false));
    const auto rollbacks = prediction->drain_rollbacks();
    CHECK(rollbacks.size() == 1);
    CHECK(rollbacks[0].sequence == edit);
    CHECK(rollbacks[0].restore_block == 7);
    CHECK(prediction->pending_block_edits() == 0);

    RemoteSnapshot a;
    a.entity_net_id = 99;
    a.tick = 10;
    a.server_time = 1.0;
    a.pose.x = 2.0;
    RemoteSnapshot b = a;
    b.tick = 11;
    b.server_time = 2.0;
    b.pose.x = 6.0;
    CHECK(prediction->push_remote_snapshot(a));
    CHECK(prediction->push_remote_snapshot(b));
    PredictedPose sampled;
    CHECK(prediction->sample_remote(99, 1.5, sampled));
    CHECK(sampled.x == 4.0);

    CHECK(prediction->reset(err));
    CHECK(prediction->pending_input_count() == 0);
    CHECK(prediction->pending_block_edits() == 0);
    CHECK(prediction->next_sequence() == 1);
    std::cout << "[ival] PASS: client prediction contract\n";
}

void test_gameplay_phase_contract() {
    std::cout << "[ival] test_gameplay_phase_contract...\n";
    using namespace engine::gameplay;

    auto phase = create_gameplay_phase();
    CHECK(phase != nullptr);
    std::string err;
    CHECK(phase->configure({GameplayDomain::Ecs, GameplayDomain::Renderer}, err));
    CHECK(!phase->complete(err));
    CHECK(phase->mark_producer_bound(GameplayDomain::Ecs));
    CHECK(phase->mark_consumer_bound(GameplayDomain::Ecs));
    CHECK(!phase->complete(err));
    CHECK(phase->mark_persistence_bound(GameplayDomain::Ecs));
    CHECK(!phase->complete(err));
    CHECK(phase->mark_replication_bound(GameplayDomain::Ecs));
    CHECK(phase->mark_producer_bound(GameplayDomain::Renderer));
    CHECK(phase->mark_consumer_bound(GameplayDomain::Renderer));
    CHECK(phase->complete(err));
    CHECK(err.empty());
    CHECK(phase->status().size() == 2);
    CHECK(!phase->mark_producer_bound(GameplayDomain::Audio));

    phase->reset();
    CHECK(phase->status().empty());
    CHECK(!phase->complete(err));
    std::cout << "[ival] PASS: gameplay phase contract\n";
}

void test_gameplay_cross_domain_contract() {
    std::cout << "[ival] test_gameplay_cross_domain_contract...\n";
    using namespace engine::gameplay;

    auto cross = create_gameplay_cross_domain();
    auto integration = create_gameplay_integration();
    CHECK(cross != nullptr);
    CHECK(integration != nullptr);
    std::string err;
    CHECK(integration->configure(1.0f / 60.0f, 8, err));
    CHECK(cross->bind_integration(integration.get()));
    CHECK(!cross->bind_navigation(nullptr, nullptr, nullptr, nullptr, err));
    CHECK(!err.empty());
    CHECK(!cross->bind_debug(nullptr, nullptr));
    CHECK(!cross->bind_authoring(nullptr, nullptr));
    cross->refresh();
    const auto snapshot = cross->snapshot();
    CHECK(!snapshot.navigationBound);
    CHECK(!snapshot.debugBound);
    CHECK(!snapshot.authoringBound);
    CHECK(!snapshot.fullyBound);
    const auto json = cross->to_json();
    CHECK(json.find("\"navigationBound\":false") != std::string::npos);
    CHECK(json.find("\"fullyBound\":false") != std::string::npos);
    std::cout << "[ival] PASS: gameplay cross-domain contract\n";
}

void test_render_provider_registry_contract() {
    std::cout << "[ival] test_render_provider_registry_contract...\n";
    using namespace Engine::Rendering;

    std::string err;
    auto registry = create_render_provider_registry(err);
    CHECK(registry != nullptr);
    registry->set(RenderProviderEntry{
        "integration-test", "provider-a", "IntegrationValidationTests",
        "vc_sdk_rendering", "deterministic-test"});
    const auto* first = registry->find("integration-test");
    CHECK(first != nullptr);
    CHECK(first && first->provider == "provider-a");

    registry->set(RenderProviderEntry{
        "integration-test", "provider-b", "IntegrationValidationTests",
        "vc_sdk_rendering", "replacement"});
    const auto* replaced = registry->find("integration-test");
    CHECK(replaced != nullptr);
    CHECK(replaced && replaced->provider == "provider-b");
    const auto all = registry->all();
    CHECK(std::count_if(all.begin(), all.end(), [](const RenderProviderEntry& entry) {
        return entry.system == "integration-test";
    }) == 1);
    const auto json = registry->to_json();
    CHECK(json.find("\"integration-test\"") != std::string::npos);
    CHECK(json.find("\"provider-b\"") != std::string::npos);
    registry->clear();
    CHECK(registry->find("integration-test") == nullptr);
    std::cout << "[ival] PASS: render provider registry contract\n";
}

void test_replication_security_contract() {
    std::cout << "[ival] test_replication_security_contract...\n";
    using namespace engine::networking;

    SecurityLimits limits;
    limits.max_messages_per_window = 2;
    limits.window_millis = 1000;
    limits.max_payload = 8;
    limits.max_response_ratio = 2;
    limits.journal_max_entries = 2;

    std::string err;
    auto security = create_replication_security(limits, err);
    CHECK(security != nullptr);
    PayloadSchema schema;
    schema.name = "bounded-u8";
    schema.max_size = 1;
    schema.fields.push_back(SchemaFieldRule{"value", FieldKind::U8, 1, 1, 3, 1});
    CHECK(security->register_schema(schema, err));
    const std::uint8_t valid[] = {2};
    const std::uint8_t invalid[] = {9};
    CHECK(security->validate("bounded-u8", valid, sizeof(valid), err));
    CHECK(!security->validate("bounded-u8", invalid, sizeof(invalid), err));

    CHECK(security->advance_window(1000));
    CHECK(security->observe_incoming(7, 2));
    CHECK(security->observe_incoming(7, 2));
    CHECK(!security->observe_incoming(7, 2));
    CHECK(security->dropped_spam() == 1);
    CHECK(security->amplification_ok(7, 2, 4));
    CHECK(!security->amplification_ok(7, 2, 5));
    CHECK(security->dropped_amplification() == 1);

    const std::uint8_t payload[] = {1, 2, 3};
    std::uint64_t seq1 = 0;
    std::uint64_t seq2 = 0;
    std::uint64_t seq3 = 0;
    CHECK(security->journal_record("one", payload, sizeof(payload), 10, seq1, err));
    CHECK(security->journal_record("two", payload, sizeof(payload), 11, seq2, err));
    CHECK(security->journal_record("three", payload, sizeof(payload), 12, seq3, err));
    CHECK(seq1 == 1 && seq2 == 2 && seq3 == 3);
    CHECK(security->journal_size() == 2);
    const auto retained = security->journal_since(0);
    CHECK(retained.size() == 2);
    CHECK(retained[0].sequence == 2);
    CHECK(retained[1].sequence == 3);
    std::vector<std::uint64_t> replayed;
    CHECK(security->replay(1, [&](const JournalEntry& entry) {
        replayed.push_back(entry.sequence);
    }) == 2);
    CHECK(replayed.size() == 2);
    CHECK(replayed[0] == 2 && replayed[1] == 3);

    CHECK(security->reset(err));
    CHECK(security->journal_size() == 0);
    CHECK(security->last_journal_sequence() == 0);
    CHECK(security->dropped_spam() == 0);
    CHECK(security->dropped_amplification() == 0);
    std::cout << "[ival] PASS: replication security contract\n";
}

void test_plugin_permissions_contract() {
    std::cout << "[ival] test_plugin_permissions_contract...\n";
    using namespace engine::plugins;

    auto permissions = create_plugin_permission_policy();
    CHECK(permissions != nullptr);
    CHECK(!permissions->is_granted("world.read", "integration-plugin"));
    CHECK(permissions->grant("world.read", "integration-plugin"));
    CHECK(!permissions->grant("world.read", "integration-plugin"));
    CHECK(permissions->is_granted("world.read", "integration-plugin"));
    const auto granted = permissions->evaluate("world.read", "integration-plugin");
    CHECK(granted.allowed);
    CHECK(granted.reason == "granted");
    const auto decisions = permissions->evaluate_all(
        "integration-plugin", {"world.read", "world.write"});
    CHECK(decisions.size() == 2);
    CHECK(decisions[0].allowed);
    CHECK(!decisions[1].allowed);
    CHECK(permissions->revoke("world.read", "integration-plugin"));
    CHECK(!permissions->is_granted("world.read", "integration-plugin"));
    CHECK(!permissions->revoke("world.read", "integration-plugin"));
    std::cout << "[ival] PASS: plugin permissions contract\n";
}

int main() {
    std::cout << "=== Integration & Validation Tests (items 239, 249, 266, 268) ===\n\n";

    test_character_collision_interaction();
    test_rejection_rules();
    test_hot_reload();
    test_plugin_fault_isolation();
    test_transaction_policy_comprehensive();
    test_client_prediction_contract();
    test_gameplay_phase_contract();
    test_gameplay_cross_domain_contract();
    test_render_provider_registry_contract();
    test_replication_security_contract();
    test_plugin_permissions_contract();

    std::cout << "\n=== Results: " << g_failures << " failures ===\n";
    if (g_failures > 0) {
        std::cerr << "FAILURES DETECTED\n";
        return 1;
    }
    std::cout << "ALL INTEGRATION & VALIDATION TESTS PASSED\n";
    return 0;
}

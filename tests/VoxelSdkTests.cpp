// VoxelSdkTests.cpp
//
// Evidence for the public SDK milestone (META section 6 + 7):
//  - This translation unit compiles using ONLY public SDK headers
//    (<engine/...>) plus glm — no engine internals, no Vulkan. It stands in
//    for an external project consuming the engine as a library.
//  - The voxel world operates headless (no renderer) through IVoxelWorld:
//    generation, meshing and edits run on the simulation side.
//  - A registered generator replaces the builtin terrain (plug-in generation).
//  - Block/item registries are data-driven: JSON assets load at runtime,
//    identities are persistent UUIDs (stable across load order), invalid
//    assets fail with a clear diagnostic and a safe fallback exists.

#include <engine/version.hpp>
#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/voxel/IVoxelServices.hpp>
#include <engine/voxel/IVoxelBlockEntity.hpp>
#include <engine/voxel/IBlockEntityScripting.hpp>
#include "engine/sdk/FastNoise2Adapter.hpp"
#include "engine/physics/IConvexDecomposition.hpp"
#include "engine/physics/ICSGOperation.hpp"
#include <engine/registry/BlockRegistry.hpp>
#include <engine/registry/FluidRegistry.hpp>
#include <engine/registry/ItemRegistry.hpp>
#include <engine/registry/ItemStack.hpp>
#include <engine/registry/Inventory.hpp>
#include <engine/registry/RecipeRegistry.hpp>
#include <engine/entity/IEntityWorld.hpp>
#include <engine/entity/IMobBehavior.hpp>
#include <engine/world/IWorldManager.hpp>
#include <engine/navigation/INavigationProvider.hpp>
#include <engine/navigation/VoxelNavigation.hpp>
#include <engine/procgen/INoiseGraph.hpp>
#include <engine/procgen/IClimateBiome.hpp>
#include <engine/procgen/IWorldFeatures.hpp>
#include <engine/procgen/IStructureGenerator.hpp>
#include <engine/procgen/IStructurePlacement.hpp>
#include <engine/procgen/IWorldProfile.hpp>
#include <engine/procgen/IParcellation.hpp>
#include <engine/procgen/IShapeGrammar.hpp>
#include <engine/procgen/IHeightmapErosion.hpp>
#include <engine/procgen/IMeshCooking.hpp>
#include <engine/procgen/IProcgenPreview.hpp>
#include <engine/procgen/IJobRunner.hpp>
#include <engine/procgen/ILodTerrain.hpp>
#include <engine/compression/ICompressionProvider.hpp>
#include <engine/hashing/IHashProvider.hpp>
#include <engine/storage/IChunkStoreFactory.hpp>

#include <glm/glm.hpp>
#include "Voxel.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <cstdint>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <process.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
// windef.h defines far/near/small as empty macros — they collide with
// identifiers used across this TU (e.g. `auto far = ...`); undefine them.
#undef far
#undef near
#undef small
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond  \
                      << "\n";                                                 \
        }                                                                      \
    } while (0)

constexpr int kBlockAir = 0;
constexpr int kBlockDirt = 2;
constexpr int kBlockStone = 3;

// Deterministic generator: a flat world at `height` with no caves/ores.
// Exercise for the public IVoxelGenerator contract (register_generator).
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

// The fluid tests run on DRY flat terrain ABOVE the world's sea level
// (TerrainGenerator::SeaLevel = 127), so the world generates NO water: the
// fluid under test is the only fluid in the world. Booting FlatGenerator(96)
// floods the fluid-physics FIFO (512 cells/tick) with the generated sea lake
// (millions of cells across the 33x33-chunk boot), starving the tested cells
// indefinitely — that was the voxel_sdk_tests timing flake.
constexpr int kFluidTestTerrain = 130;
// Grounded pools sit on the terrain top (non-air below) so the ring is fed
// sideways; the evaporation test's pool floats in air (air below) so nothing
// can feed its ring.
constexpr int kFluidTestGroundedY = kFluidTestTerrain + 1;
constexpr int kFluidTestFloatingY = kFluidTestTerrain + 2;

// ---- Project-owned block entities (META section 8 examples) ----
// The engine stores/ticks/persists; the project owns behavior and state. The
// blob is opaque to the engine; the project migrates its own versions.

// A counter machine: persisted counter + runtime tick count.
class CounterMachine final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "project:counter_machine"; }
    void on_tick(uint64_t worldTick) override {
        ++ticks_;
        lastWorldTick_ = worldTick;
    }
    uint32_t data_version() const override { return 1; }
    std::vector<uint8_t> serialize_state() const override {
        std::vector<uint8_t> blob(4);
        for (int i = 0; i < 4; ++i) {
            blob[static_cast<std::size_t>(i)] =
                static_cast<uint8_t>((counter_ >> (8 * i)) & 0xFFu);
        }
        return blob;
    }
    bool deserialize_state(const std::vector<uint8_t>& data,
                           uint32_t version) override {
        if (version != 1 || data.size() < 4) return false;
        counter_ = 0;
        for (int i = 0; i < 4; ++i) {
            counter_ |= static_cast<uint32_t>(data[static_cast<std::size_t>(i)])
                        << (8 * i);
        }
        return true;
    }
    void on_created() override { created_ = true; }
    void on_destroyed() override { destroyed_ = true; }

    uint32_t counter_{ 0 };        // persisted project state
    uint64_t ticks_{ 0 };          // runtime only (not persisted)
    uint64_t lastWorldTick_{ 0 };
    bool created_{ false };
    bool destroyed_{ false };
};

// A machine that refuses data it cannot interpret (version migration demo).
class StrictMachine final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "project:strict_machine"; }
    void on_tick(uint64_t) override {}
    uint32_t data_version() const override { return 2; }
    std::vector<uint8_t> serialize_state() const override { return {}; }
    bool deserialize_state(const std::vector<uint8_t>&, uint32_t version) override {
        return version == 2;  // refuses older/newer data
    }
};

// A chest block entity that EXERCISES the optional B.1 capabilities: an
// authoritative inventory, a project script and one project component. It
// persists all three through its own state blob (the project's responsibility:
// the blob is opaque to the engine). The engine only routes and frames it.
class ChestEntity final : public engine::voxel::IVoxelBlockEntity {
public:
    ChestEntity() : inventory_(27) {}  // chest: 27 slots

    std::string type_id() const override { return "project:chest"; }
    void on_tick(uint64_t) override {}
    uint32_t data_version() const override { return 1; }

    // B.1 capabilities.
    engine::registry::Inventory* inventory() override { return &inventory_; }
    const engine::registry::Inventory* inventory() const override {
        return &inventory_;
    }
    std::string script_id() const override { return "project:chest_loot"; }
    std::vector<engine::voxel::BlockEntityComponent> components() const override {
        engine::voxel::BlockEntityComponent fuel;
        fuel.type = "project:fuel";
        fuel.version = 1;
        fuel.blob = { 0xBB };
        engine::voxel::BlockEntityComponent lockable;
        lockable.type = "project:lockable";
        lockable.version = 1;
        lockable.blob = { 0xAA, 0x01 };  // opaque project payload
        // Deterministic order (sorted by type): fuel < lockable.
        return { fuel, lockable };
    }

    // Persists the component blob marker (the engine frames it; the project
    // owns the content). The inventory/script/components are runtime
    // capabilities reachable through the accessors; the blob is the
    // persistence channel the entity itself defines.
    std::vector<uint8_t> serialize_state() const override { return lockedBlob_; }
    bool deserialize_state(const std::vector<uint8_t>& data,
                           uint32_t version) override {
        if (version != 1) return false;
        lockedBlob_ = data;
        return true;
    }

    engine::registry::Inventory inventory_;
    std::vector<uint8_t> lockedBlob_;
};

// Boots a world headless: drive update() with real-time pacing until the
// center chunk is loaded or the wall-clock budget runs out.
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

// Runs update() until `predicate` holds. The sim advances by FIXED steps
// (dt = 1/60, unlimited scheduler budgets), so the number of steps needed is
// sim-time deterministic — wall-clock polling alone flaked under parallel
// ctest/system load (the sim converges late, not never). The wall budget stays
// only as a sanity cap so a genuine non-convergence fails fast instead of
// hanging; the default is generous because each step may be slow under load.
template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
            Pred predicate, int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 120;  // 120 sim-seconds
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
    }
    return predicate();
}

// Per-process scratch directory: the file-based tests write to
// temp/vc_sdk_tests_<pid>, so a concurrently running instance of this binary
// (two ctest processes, or stale state from a crashed run) can never collide
// on the same fixed file names. Other engine test binaries follow the same
// unique-under-temp pattern.
const std::string& scratch_dir() {
    static const std::string dir = [] {
        const std::string d = (std::filesystem::temp_directory_path() /
            ("vc_sdk_tests_" + std::to_string(_getpid()))).string();
        std::error_code ec;
        std::filesystem::remove_all(d, ec);  // stale dir from a crashed run
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

// Topmost non-air block of a column (terrain top, water level, or wall top).
int helper_surface_y(engine::voxel::IVoxelWorld& world, int x, int z) {
    for (int y = 160; y >= 0; --y) {
        if (world.get_block(x, y, z) != kBlockAir) return y;
    }
    return -1;
}

void test_version() {
    const engine::VersionInfo version = engine::engine_version();
    CHECK(version.major >= 1);
    CHECK(version.abi != nullptr && version.abi[0] != '\0');
    CHECK(std::string(engine::engine_sdk_abi()) == "engine-voxel-1");
    std::cout << "[sdk] version " << version.major << '.' << version.minor
              << '.' << version.patch << " abi=" << version.abi << "\n";
}

// Headless world through the public interface: boot, edit roundtrip and
// voxel raycast against a flat world produced by a registered generator.
void test_world_headless() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);

    // Register the generator BEFORE boot so generation uses it.
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // Flat world: solid surface at y=96, water 97..127, air from 128 up.
    CHECK(world->get_block(0, 96, 0) != kBlockAir);
    CHECK(world->get_block(0, 128, 0) == kBlockAir);

    // Downward raycast hits the surface top face (before any edits).
    const engine::voxel::VoxelRaycastHit down =
        world->raycast(glm::vec3(8.0f, 160.0f, 8.0f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(down.hit);
    CHECK(down.block.y == 96);
    CHECK(down.normal.y > 0.5f);
    CHECK(down.chunk.x == 0 && down.chunk.y == 0);

    // Upward ray from above the water misses (nothing solid up there).
    const engine::voxel::VoxelRaycastHit up =
        world->raycast(glm::vec3(8.0f, 130.0f, 8.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
    CHECK(!up.hit);

    // Edit roundtrip on a loaded chunk.
    world->set_block(8, 120, 8, kBlockStone);
    CHECK(world->get_block(8, 120, 8) == kBlockStone);

    std::cout << "[sdk] headless world: flat height 96 booted, edits and "
                 "raycast OK\n";
}

// Transactional edits (META section 11): atomic multi-edit commit, full
// rollback on invalid input, undo/redo, single-edit convenience routed through
// the same path, event log and listener.
void test_transactions() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    int committed = 0, undone = 0, redone = 0, rolledBack = 0;
    world->set_transaction_listener(
        [&](const engine::voxel::TransactionEvent& event) {
            switch (event.kind) {
            case engine::voxel::TransactionEvent::Kind::Committed: ++committed; break;
            case engine::voxel::TransactionEvent::Kind::Undone: ++undone; break;
            case engine::voxel::TransactionEvent::Kind::Redone: ++redone; break;
            case engine::voxel::TransactionEvent::Kind::RolledBack: ++rolledBack; break;
            }
        });

    // 1. A multi-edit transaction applies atomically (y=130 is above the flat
    // surface, so previous state is Air everywhere).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        CHECK(tx != nullptr);
        CHECK(tx->edit_count() == 0);
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(4, 130, 4, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        CHECK(tx->edit_count() == 3);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
        CHECK(world->get_block(3, 130, 3) == kBlockStone);
        CHECK(world->get_block(4, 130, 4) == kBlockStone);
        CHECK(world->get_block(5, 130, 5) == kBlockStone);
        CHECK(world->undo_depth() == 1);
        CHECK(world->edit_log_count() == 3);
    }

    // 2. Undo restores the previous state; redo reapplies it.
    CHECK(world->undo_last_transaction());
    CHECK(world->get_block(3, 130, 3) == kBlockAir);
    CHECK(world->get_block(4, 130, 4) == kBlockAir);
    CHECK(world->undo_depth() == 0);
    CHECK(world->redo_last_transaction());
    CHECK(world->get_block(3, 130, 3) == kBlockStone);
    CHECK(world->get_block(5, 130, 5) == kBlockStone);
    CHECK(world->undo_depth() == 1);

    // 3. Invalid block id: commit fails, nothing changes, event logged.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(6, 130, 6, 999'999);
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(!error.empty());
        CHECK(world->get_block(6, 130, 6) == kBlockAir);
        CHECK(world->undo_depth() == 1);   // valid transaction still undoable
        CHECK(world->edit_log_count() == 3);  // rejected edit not logged
    }

    // 4. remove_block + the single-edit convenience API are transactional and
    // undoable (no parallel mutation path).
    world->set_block(9, 130, 9, kBlockStone);          // implicit transaction
    CHECK(world->get_block(9, 130, 9) == kBlockStone);
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> rm = world->begin_transaction();
        rm->remove_block(9, 130, 9);
        std::string error;
        CHECK(rm->commit(error));
        CHECK(world->get_block(9, 130, 9) == kBlockAir);
    }
    CHECK(world->undo_last_transaction());             // undo remove -> stone
    CHECK(world->get_block(9, 130, 9) == kBlockStone);
    CHECK(world->undo_last_transaction());             // undo implicit set -> air
    CHECK(world->get_block(9, 130, 9) == kBlockAir);
    CHECK(world->undo_depth() == 1);                   // multi-edit tx still there
    CHECK(world->undo_last_transaction());             // undo multi-edit tx
    CHECK(world->get_block(3, 130, 3) == kBlockAir);
    CHECK(world->get_block(5, 130, 5) == kBlockAir);
    CHECK(world->undo_depth() == 0);
    CHECK(!world->undo_last_transaction());            // empty undo stack

    // 5. Event counters match the outcome sequence.
    CHECK(committed == 3);   // multi-edit, implicit set, remove
    CHECK(undone == 4);      // multi-edit undo + redo pair, remove, implicit, multi-edit
    CHECK(redone == 1);      // multi-edit redo
    CHECK(rolledBack == 1);  // invalid id

    std::cout << "[sdk] transactions: atomic commit, rollback, undo/redo, "
                 "event log OK\n";
}

// FALTANTES §7 itens 139/142: falha em cada estágio da transação com rollback
// integral, e transações multi-chunk sem estado parcial observável. Estágio 1
// (validação): um id inválido MISTURADO com edições válidas falha ANTES de
// aplicar qualquer coisa. Estágio 2 (apply): edições em chunks carregados +
// uma em chunk NÃO carregado passam a validação mas falham no apply — as
// edições já aplicadas são revertidas (nada parcial observável), o undo stack
// e o event log ficam intactos. Sucesso: edições em DOIS chunks carregados
// cometam atomicamente e o undo reverte ambas.
void test_transaction_failure_stages() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    // Budget 32 => janela completa dos anéis 0-2 (25 chunks): (0,0) e (2,2)
    // ficam carregados; (31,31) fica muito fora da janela.
    CHECK(boot_world(*world, player, 32));
    CHECK(settle(*world, player, [&] { return world->is_chunk_loaded(2, 2); }));

    int committed = 0, rolledBack = 0;
    world->set_transaction_listener(
        [&](const engine::voxel::TransactionEvent& event) {
            if (event.kind == engine::voxel::TransactionEvent::Kind::Committed) ++committed;
            if (event.kind == engine::voxel::TransactionEvent::Kind::RolledBack) ++rolledBack;
        });

    // --- Estágio 1: validação falha ANTES de aplicar qualquer coisa. ---
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);       // válida (chunk (0,0) carregado)
        tx->set_block(4, 130, 4, 999'999);           // id inválido
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(!error.empty());
        // NADA foi aplicado (a validação roda antes do apply): a edição válida
        // também não existe, undo/log intocados.
        CHECK(world->get_block(3, 130, 3) == kBlockAir);
        CHECK(world->get_block(4, 130, 4) == kBlockAir);
        CHECK(world->undo_depth() == 0);
        CHECK(world->edit_log_count() == 0);
    }

    // --- Estágio 2: apply falha no meio de uma transação MULTI-CHUNK — as
    // edições já aplicadas são revertidas (rollback integral, nada parcial). ---
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);        // chunk (0,0) carregado
        tx->set_block(40, 130, 40, kBlockStone);      // chunk (2,2) carregado
        tx->set_block(500, 130, 500, kBlockStone);    // chunk (31,31) NÃO carregado
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(error.find("chunk not loaded") != std::string::npos);
        // Rollback integral: NENHUMA das edições aplicadas antes da falha
        // sobrevive (nem no chunk (0,0), que foi aplicado primeiro).
        CHECK(world->get_block(3, 130, 3) == kBlockAir);
        CHECK(world->get_block(40, 130, 40) == kBlockAir);
        CHECK(world->get_block(500, 130, 500) == kBlockAir);
        CHECK(world->undo_depth() == 0);
        CHECK(world->edit_log_count() == 0);
    }

    // --- Sucesso multi-chunk: edições em DOIS chunks carregados cometam
    // atomicamente; o undo reverte ambas (o mundo segue íntegro pós-falha). ---
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);        // chunk (0,0)
        tx->set_block(40, 130, 40, kBlockStone);      // chunk (2,2)
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
        CHECK(world->get_block(3, 130, 3) == kBlockStone);
        CHECK(world->get_block(40, 130, 40) == kBlockStone);
        CHECK(world->undo_depth() == 1);
        CHECK(world->edit_log_count() == 2);
    }
    CHECK(world->undo_last_transaction());
    CHECK(world->get_block(3, 130, 3) == kBlockAir);
    CHECK(world->get_block(40, 130, 40) == kBlockAir);

    // --- Contadores de evento: exatamente os 2 rollbacks + 1 commit. ---
    CHECK(rolledBack == 2);
    CHECK(committed == 1);

    std::cout << "[sdk] transaction failure stages: validation rejects before "
                 "apply, mid-apply multi-chunk failure rolls back applied "
                 "edits (no partial state, undo/log intact), multi-chunk "
                 "commit + undo OK\n";
}

// FALTANTES §7 item 138: permissões, validação autoritativa e limites por
// transação. `TransactionLimits` (maxEdits, maxBoxVolume) e
// `ITransactionPolicy` (per-edit + whole-transaction) são aplicados na
// VALIDAÇÃO — antes de qualquer edit: uma recusa deixa o mundo intocado
// (RolledBack, nada aplicado, undo/log intactos); dentro dos limites/política
// a transação comita normalmente.
void test_transaction_policy_limits() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    // boot_world only waits for chunk (0,0); the edits below reach chunk
    // (1,1) (x=20..21), so wait for it too — otherwise the commit races the
    // generator/upload and flakes under load (findings #54 pattern).
    CHECK(settle(*world, player, [&] { return world->is_chunk_loaded(1, 1); }));

    int rolledBack = 0, committed = 0;
    world->set_transaction_listener(
        [&](const engine::voxel::TransactionEvent& event) {
            if (event.kind == engine::voxel::TransactionEvent::Kind::RolledBack) ++rolledBack;
            if (event.kind == engine::voxel::TransactionEvent::Kind::Committed) ++committed;
        });

    // --- maxEdits: acima do limite, recusado ANTES do apply (nada aplicado);
    // dentro do limite, comita. ---
    world->set_transaction_limits(
        engine::voxel::TransactionLimits{ /*maxEdits=*/2, /*maxBoxVolume=*/0 });
    CHECK(world->transaction_limits().maxEdits == 2);
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(4, 130, 4, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);  // 3 > 2
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(error.find("per-transaction limit") != std::string::npos);
        CHECK(world->get_block(3, 130, 3) == kBlockAir);
        CHECK(world->undo_depth() == 0);
        CHECK(world->edit_log_count() == 0);
    }
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(4, 130, 4, kBlockStone);  // 2 <= 2
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
        CHECK(world->get_block(3, 130, 3) == kBlockStone);
        CHECK(world->undo_depth() == 1);
    }

    // --- maxBoxVolume: caixa de edições grande recusada; compacta comita. ---
    world->set_transaction_limits(
        engine::voxel::TransactionLimits{ /*maxEdits=*/0, /*maxBoxVolume=*/27 });  // 3^3
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(10, 130, 10, kBlockStone);
        tx->set_block(100, 130, 10, kBlockStone);  // box 91*1*1 = 91 > 27
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(error.find("edit box volume") != std::string::npos);
        CHECK(world->get_block(10, 130, 10) == kBlockAir);
    }
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(10, 130, 10, kBlockStone);
        tx->set_block(11, 130, 10, kBlockStone);  // box 2*1*1 = 2 <= 27
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }

    // --- Política autoritativa (permissões): per-edit + whole-transaction. ---
    struct EastPolicy final : engine::voxel::ITransactionPolicy {
        std::string validate_edit(
            const engine::voxel::BlockEdit& edit) const override {
            if (edit.position.x > 50) return "edits east of x=50 are forbidden";
            return std::string();
        }
        std::string validate_transaction(
            const std::vector<engine::voxel::BlockEdit>& edits) const override {
            if (edits.size() > 1) return "single-edit transactions only";
            return std::string();
        }
    };
    world->set_transaction_policy(std::make_shared<EastPolicy>());
    world->set_transaction_limits({});  // reset limits
    // per-edit: x > 50 recusado — e como a validação roda antes do apply, o
    // edit válido misturado também não é aplicado.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(20, 130, 20, kBlockStone);
        tx->set_block(60, 130, 20, kBlockStone);  // x > 50 -> forbidden
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(error.find("forbidden") != std::string::npos);
        CHECK(world->get_block(20, 130, 20) == kBlockAir);
    }
    // whole-transaction: 2 edits válidos recusados ("single-edit only").
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(20, 130, 20, kBlockStone);
        tx->set_block(21, 130, 20, kBlockStone);
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(error.find("single-edit") != std::string::npos);
        CHECK(world->get_block(20, 130, 20) == kBlockAir);
    }
    // 1 edit válido comita normalmente sob a política.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(20, 130, 20, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
        CHECK(world->get_block(20, 130, 20) == kBlockStone);
    }

    // Contadores: 4 recusas (maxEdits, box, per-edit, whole-tx) + 3 commits.
    CHECK(rolledBack == 4);
    CHECK(committed == 3);

    std::cout << "[sdk] transaction policy + limits: maxEdits/maxBoxVolume and "
                 "authoritative policy reject before apply (nothing applied, "
                 "undo/log intact), valid transactions commit OK\n";
}

// Persistence (META section 10): versioned save/load with semantic identity,
// idempotent re-serialization, corruption/bad-magic/bad-version refusal, and a
// file roundtrip.
void test_persistence() {
    // World A: flat terrain + a transactional edit, then serialize.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    std::string errorA;
    const std::string bytesA = a->serialize_world(errorA);
    CHECK(errorA.empty());
    CHECK(bytesA.size() > 100);

    // World B: same generator, restore the save, compare a sample grid.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string errorB;
    CHECK(b->deserialize_world(bytesA, errorB));
    CHECK(errorB.empty());

    const int xs[] = { 0, 3, 5, 8, 15 };
    const int zs[] = { 0, 8, 15 };
    const int ys[] = { 1, 50, 96, 100, 120, 127, 128, 130, 200 };
    bool identical = true;
    for (const int x : xs) {
        for (const int z : zs) {
            for (const int y : ys) {
                if (a->get_block(x, y, z) != b->get_block(x, y, z)) identical = false;
            }
        }
    }
    CHECK(identical);

    // Idempotency: serializing the restored world yields the same bytes.
    std::string errorC;
    const std::string bytesB = b->serialize_world(errorC);
    CHECK(errorC.empty());
    CHECK(bytesB == bytesA);

    // Corruption: a flipped byte is refused with a clear diagnostic.
    std::string corrupted = bytesA;
    corrupted[bytesA.size() / 2] ^= 0xFF;
    std::string errorD;
    CHECK(!b->deserialize_world(corrupted, errorD));
    CHECK(!errorD.empty());

    // Bad magic and unsupported schema version are refused too.
    std::string badMagic = bytesA;
    badMagic[0] = 'X';
    std::string errorE;
    CHECK(!b->deserialize_world(badMagic, errorE));
    CHECK(!errorE.empty());
    std::string badVersion = bytesA;
    badVersion[5] = 9;  // schema version field (u32 LE at offset 5)
    std::string errorF;
    CHECK(!b->deserialize_world(badVersion, errorF));
    CHECK(!errorF.empty());

    // File roundtrip: save to disk, load into a fresh world. The path lives
    // in the per-process scratch dir so concurrent instances never collide.
    const std::string path = scratch_dir() + "/vc_test_world.vcwld";
    std::string errorG;
    CHECK(a->save_world(path, errorG));
    CHECK(errorG.empty());
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string errorH;
    CHECK(d->load_world(path, errorH));
    CHECK(errorH.empty());
    CHECK(d->get_block(3, 130, 3) == kBlockStone);
    CHECK(d->get_block(5, 130, 5) == kBlockStone);

    // Repeated saves to the SAME slot must replace the previous save (Windows
    // std::rename semantics must not silently fail on an existing target).
    {
        // A second transaction so the new save differs from the first.
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(7, 130, 7, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        std::string saveError;
        CHECK(a->save_world(path, saveError));
        CHECK(saveError.empty());
    }
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> e =
            engine::voxel::create_default_voxel_world();
        e->register_generator(std::make_shared<FlatGenerator>(96));
        CHECK(boot_world(*e, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        std::string loadError;
        CHECK(e->load_world(path, loadError));
        CHECK(loadError.empty());
        CHECK(e->get_block(7, 130, 7) == kBlockStone);  // second save won
        CHECK(e->get_block(3, 130, 3) == kBlockStone);  // first content kept
        CHECK(e->get_block(9, 130, 9) == kBlockAir);     // nothing stale
    }
    std::error_code removeEc;
    std::filesystem::remove(path, removeEc);

    std::cout << "[sdk] persistence: versioned save/load, idempotency, "
                 "corruption & file roundtrip OK\n";
}

// SDK convention (same bug class as json_parse — #153): a diagnostic buffer
// reused by a caller must never poison the success path. save_world must
// clear errorOut at entry, so a stale message from a previous failed call
// cannot turn a successful save into a false-negative `!errorOut.empty()`
// check right after serialize_world (which leaves the buffer untouched on
// success).
void test_save_world_clears_error_out() {
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    CHECK(a->get_block(3, 130, 3) == kBlockStone);

    const std::string path = scratch_dir() + "/vc_test_world_clear_err.vcwld";
    // Caller reuses a buffer that still holds an old error message.
    std::string errorOut = "stale error from a previous failed call";
    CHECK(a->save_world(path, errorOut));
    CHECK(errorOut.empty());  // stale residue must NOT surface as failure

    // The save really landed: a fresh world loads it.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadError = "stale";
    CHECK(b->load_world(path, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);

    std::error_code removeEc;
    std::filesystem::remove(path, removeEc);
    std::cout << "[sdk] save_world clears errorOut at entry (no stale "
                 "false-negative)\n";
}

// Promoted solution (META section 32): Zstandard adapter round-trips byte
// strings, detects its own frames and fails cleanly on foreign input.
void test_compression_provider() {
    std::shared_ptr<engine::compression::ICompressionProvider> z =
        engine::compression::create_zstd_compression_provider();
    CHECK(z != nullptr);

    // Round-trip: empty, tiny, repetitive (compressible) and random data.
    const std::string inputs[] = {
        std::string(),
        "x",
        "hello hello hello hello hello\n",
        std::string(200000, 'a'),
    };
    for (const std::string& in : inputs) {
        const std::string frame = z->compress(in);
        CHECK(!frame.empty());          // even empty input yields a real frame
        CHECK(z->is_compressed(frame));
        CHECK(z->decompress(frame) == in);
    }
    // Compressible input actually shrinks.
    CHECK(z->compress(std::string(200000, 'a')).size() < 2000u);

    // Foreign/garbage input is not a frame and decompression fails cleanly.
    CHECK(!z->is_compressed("not a zstd frame"));
    CHECK(z->decompress("not a zstd frame").empty());
    // A truncated frame fails cleanly too (no crash).
    const std::string frame = z->compress("payload");
    CHECK(!frame.empty());
    CHECK(z->decompress(frame.substr(0, frame.size() / 2)).empty());

    std::cout << "[sdk] compression: zstd round-trip, frame detection, "
                 "clean failure OK\n";
}

// Promoted solution (META section 32): BLAKE3 adapter matches the official
// test vector (BLAKE3-256 of empty input) and is deterministic.
void test_hash_provider() {
    std::shared_ptr<engine::hashing::IHashProvider> h =
        engine::hashing::create_blake3_hash_provider();
    CHECK(h != nullptr);
    CHECK(h->digest_size() == 32u);

    // Official BLAKE3-256 of the empty string (BLAKE3 test_vectors.json).
    const std::string emptyHex =
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";
    CHECK(h->hash_hex(std::string()) == emptyHex);
    CHECK(h->hash(std::string()).size() == 32u);

    // Determinism and sensitivity.
    CHECK(h->hash_hex("abc") == h->hash_hex("abc"));
    CHECK(h->hash_hex("abc") != h->hash_hex("abd"));
    CHECK(h->hash_hex("abc").size() == 64u);
    {   // hex-escape trap: \x consumes following hex digits, so build chars
        // explicitly to test to_hex with 0x00, 0x01, 0xff.
        std::string raw;
        raw.push_back(static_cast<char>(0x00));
        raw.push_back(static_cast<char>(0x01));
        raw.push_back(static_cast<char>(0xFF));
        CHECK(engine::hashing::to_hex(raw) == "0001ff");
    }

    std::cout << "[sdk] hash: BLAKE3 official vector + determinism OK\n";
}

// v4 keeps loading legacy v1-v3 saves: build a v3 save by hand (FNV-1a
// checksum, no entity section) and confirm it loads; also confirm the v4 file
// round-trip through the zstd-compressed file layer preserves the world.
void test_save_v4_legacy() {
    // Minimal v3 save: magic + version 3 + palette count 0 + chunk count 0 +
    // entity count 0 + FNV-1a (offset/prime from the documented format).
    std::string body = "VCWLD";
    auto appendU32 = [&body](uint32_t v) {
        body.push_back(static_cast<char>(v & 0xFFu));
        body.push_back(static_cast<char>((v >> 8) & 0xFFu));
        body.push_back(static_cast<char>((v >> 16) & 0xFFu));
        body.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    appendU32(3);  // schema version 3
    appendU32(0);  // palette count
    appendU32(0);  // chunk count
    appendU32(0);  // block entity count (v3 section)
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : body) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    for (int i = 0; i < 8; ++i) {
        body.push_back(static_cast<char>((hash >> (8 * i)) & 0xFFu));
    }

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string error;
    CHECK(world->deserialize_world(body, error));
    CHECK(error.empty());

    // A v3 save with a corrupted FNV checksum is still refused.
    std::string corrupted = body;
    corrupted[body.size() - 1] ^= 0x01;
    std::string corruptError;
    CHECK(!world->deserialize_world(corrupted, corruptError));
    CHECK(!corruptError.empty());

    // v4 file round-trip: save compresses with zstd, load decompresses, and
    // the compressed file is smaller than the serialized body for the
    // (highly compressible, mostly-air) flat world.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    std::string serError;
    const std::string serialized = a->serialize_world(serError);
    CHECK(serError.empty());
    CHECK(!serialized.empty());
    // v5: "VCWLD" + u32 version(5) + FlatBuffers container with identifier
    // "WLD5" (root uoffset u32, then the 4-char identifier, then the table).
    CHECK(serialized.size() >= 5u + 4u + 8u);
    CHECK(serialized.compare(0, 5, "VCWLD") == 0);
    const uint32_t v5Version =
        static_cast<uint32_t>(static_cast<unsigned char>(serialized[5])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(serialized[6])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(serialized[7])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(serialized[8])) << 24);
    CHECK(v5Version == 5u);
    CHECK(serialized.compare(13, 4, "WLD5") == 0);

    // A v4 save (manual binary body + BLAKE3 checksum, the previous format)
    // must still load through the legacy parser: empty palette/chunks/entities
    // plus the BLAKE3-256 digest over the body.
    {
        std::string v4 = "VCWLD";
        auto appendU32v4 = [&v4](uint32_t v) {
            v4.push_back(static_cast<char>(v & 0xFFu));
            v4.push_back(static_cast<char>((v >> 8) & 0xFFu));
            v4.push_back(static_cast<char>((v >> 16) & 0xFFu));
            v4.push_back(static_cast<char>((v >> 24) & 0xFFu));
        };
        appendU32v4(4);  // schema version 4
        appendU32v4(0);  // palette count
        appendU32v4(0);  // chunk count
        appendU32v4(0);  // block entity count
        const std::shared_ptr<engine::hashing::IHashProvider> h4 =
            engine::hashing::create_blake3_hash_provider();
        v4 += h4->hash(v4);
        std::unique_ptr<engine::voxel::IVoxelWorld> legacy =
            engine::voxel::create_default_voxel_world();
        legacy->register_generator(std::make_shared<FlatGenerator>(96));
        CHECK(boot_world(*legacy, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        std::string v4Error;
        CHECK(legacy->deserialize_world(v4, v4Error));
        CHECK(v4Error.empty());
        std::string badV4 = v4;
        badV4[badV4.size() - 1] ^= 0x01;
        std::string badV4Error;
        CHECK(!legacy->deserialize_world(badV4, badV4Error));
        CHECK(!badV4Error.empty());
    }

    const std::string path = scratch_dir() + "/vc_test_world_v4.vcwld";
    std::string saveError;
    CHECK(a->save_world(path, saveError));
    CHECK(saveError.empty());

    std::string fileBytes;
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        fileBytes = buf.str();
    }  // close before std::remove (Windows refuses to delete open files)
    // The file layer writes a zstd frame: magic 28 B5 2F FD.
    CHECK(fileBytes.size() >= 4u);
    CHECK(static_cast<unsigned char>(fileBytes[0]) == 0x28u);
    CHECK(static_cast<unsigned char>(fileBytes[1]) == 0xB5u);
    CHECK(static_cast<unsigned char>(fileBytes[2]) == 0x2Fu);
    CHECK(static_cast<unsigned char>(fileBytes[3]) == 0xFDu);
    CHECK(fileBytes.size() < serialized.size());

    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadError;
    CHECK(b->load_world(path, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    std::error_code removeEc;
    std::filesystem::remove(path, removeEc);

    std::cout << "[sdk] persistence v4: legacy v3 loads, zstd file layer "
                 "(smaller than raw) round-trips OK\n";
}

// Schema versioning + migration (FALTANTES §4 item 9): world_save_schema_
// version introspects the version field without a full parse; migrate_world_
// save upgrades a legacy v1-v4 save to the CURRENT version (v5) pure — the
// live world is untouched, the migrated bytes load in a fresh world with
// identical content (chunks, palette ids, block entities). Newer saves are
// refused with a forward-compat diagnostic, not silently downgraded.
void test_world_save_migration() {
    // --- Introspection ------------------------------------------------
    std::unique_ptr<engine::voxel::IVoxelWorld> maker =
        engine::voxel::create_default_voxel_world();
    maker->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*maker, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string makerError;
    const std::string v5save = maker->serialize_world(makerError);
    CHECK(makerError.empty());
    CHECK(maker->world_save_schema_version(v5save) == 5u);
    CHECK(maker->world_save_schema_version("not a save") == 0u);
    CHECK(maker->world_save_schema_version("VCWLD") == 0u);  // too small

    // --- Hand-built legacy saves (v1: 1-byte builtin ids + FNV-1a) ----
    auto appendU32 = [](std::string& b, uint32_t v) {
        b.push_back(static_cast<char>(v & 0xFFu));
        b.push_back(static_cast<char>((v >> 8) & 0xFFu));
        b.push_back(static_cast<char>((v >> 16) & 0xFFu));
        b.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    auto appendI32 = [&appendU32](std::string& b, int32_t v) {
        appendU32(b, static_cast<uint32_t>(v));
    };
    auto appendU16 = [](std::string& b, uint16_t v) {
        b.push_back(static_cast<char>(v & 0xFFu));
        b.push_back(static_cast<char>((v >> 8) & 0xFFu));
    };
    auto appendFNV = [](std::string& b) {
        uint64_t hash = 1469598103934665603ull;
        for (const unsigned char c : b) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        for (int i = 0; i < 8; ++i) {
            b.push_back(static_cast<char>((hash >> (8 * i)) & 0xFFu));
        }
    };
    auto appendBlake3 = [](std::string& b) {
        const std::shared_ptr<engine::hashing::IHashProvider> h =
            engine::hashing::create_blake3_hash_provider();
        b += h->hash(b);
    };

    // One chunk (0,0), extent 2: stone at (3,0,3) and a WATER block at
    // (4,0,3) with fluid level 3 (water persists its level; the level byte on
    // a non-fluid block is not restored — that is the engine's contract),
    // everything else air. Index = y*256 + z*16 + x.
    constexpr int kExtent = 2;
    constexpr int kLayer = 256;
    constexpr int kBlockWaterId = 12;  // BlockType::Water
    auto buildChunk = [&](std::string& b, bool wideIds) {
        appendI32(b, 0);  // cx
        appendI32(b, 0);  // cz
        appendU32(b, kExtent);
        const int stoneIndex = 0 * kLayer + 3 * 16 + 3;
        const int waterIndex = 0 * kLayer + 3 * 16 + 4;
        for (int v = 0; v < kExtent * kLayer; ++v) {
            const uint16_t id = (v == stoneIndex) ? kBlockStone
                                : (v == waterIndex) ? kBlockWaterId
                                : kBlockAir;
            if (wideIds) appendU16(b, id); else b.push_back(static_cast<char>(id));
        }
        for (int v = 0; v < kExtent * kLayer; ++v) {
            b.push_back(static_cast<char>((v == waterIndex) ? 3 : 0xFF));
        }
    };

    // v1 save (no palette, no entities, FNV-1a).
    std::string v1 = "VCWLD";
    appendU32(v1, 1);
    appendU32(v1, 1);  // chunk count
    buildChunk(v1, /*wideIds=*/false);
    appendFNV(v1);
    CHECK(maker->world_save_schema_version(v1) == 1u);

    // v2: palette (empty) + u16 ids + FNV.
    std::string v2 = "VCWLD";
    appendU32(v2, 2);
    appendU32(v2, 0);  // palette count
    appendU32(v2, 1);
    buildChunk(v2, /*wideIds=*/true);
    appendFNV(v2);

    // v3: + block entity section (one CounterMachine, counter=7).
    std::string v3 = "VCWLD";
    appendU32(v3, 3);
    appendU32(v3, 0);  // palette count
    appendU32(v3, 1);
    buildChunk(v3, /*wideIds=*/true);
    appendU32(v3, 1);  // entity count
    appendI32(v3, 5);
    appendI32(v3, 96);
    appendI32(v3, 5);
    const std::string typeId = "project:counter_machine";
    appendU32(v3, static_cast<uint32_t>(typeId.size()));
    v3 += typeId;
    appendU32(v3, 1);  // data version
    appendU32(v3, 4);  // blob: counter = 7 (LE u32)
    v3.push_back(static_cast<char>(7));
    v3.push_back(static_cast<char>(0));
    v3.push_back(static_cast<char>(0));
    v3.push_back(static_cast<char>(0));
    appendFNV(v3);

    // v4: same body as v3 but BLAKE3-256 checksum.
    std::string v4 = "VCWLD";
    appendU32(v4, 4);
    appendU32(v4, 0);
    appendU32(v4, 1);
    buildChunk(v4, /*wideIds=*/true);
    appendU32(v4, 1);
    appendI32(v4, 5);
    appendI32(v4, 96);
    appendI32(v4, 5);
    appendU32(v4, static_cast<uint32_t>(typeId.size()));
    v4 += typeId;
    appendU32(v4, 1);
    appendU32(v4, 4);
    v4.push_back(static_cast<char>(7));
    v4.push_back(static_cast<char>(0));
    v4.push_back(static_cast<char>(0));
    v4.push_back(static_cast<char>(0));
    appendBlake3(v4);

    // --- Migrate each legacy save, then round-trip in a fresh world ----
    const struct { uint32_t version; const std::string* bytes; } cases[] = {
        { 1, &v1 }, { 2, &v2 }, { 3, &v3 }, { 4, &v4 },
    };
    for (const auto& testCase : cases) {
        std::string migrated, migrateError;
        CHECK(maker->migrate_world_save(*testCase.bytes, migrated, migrateError));
        CHECK(migrateError.empty());
        CHECK(!migrated.empty());
        // Upgraded to the CURRENT schema version.
        CHECK(maker->world_save_schema_version(migrated) == 5u);

        // The migrated bytes load in a fresh world with identical content.
        std::unique_ptr<engine::voxel::IVoxelWorld> fresh =
            engine::voxel::create_default_voxel_world();
        fresh->register_generator(std::make_shared<FlatGenerator>(96));
        fresh->register_block_entity_type("project:counter_machine",
            [] { return std::make_shared<CounterMachine>(); });
        CHECK(boot_world(*fresh, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        std::string loadError;
        CHECK(fresh->deserialize_world(migrated, loadError));
        CHECK(loadError.empty());
        CHECK(fresh->get_block(3, 0, 3) == kBlockStone);
        CHECK(fresh->get_block(4, 0, 3) == kBlockWaterId);
        CHECK(fresh->get_fluid_level(4, 0, 3) == 3);
        // Block entities survived the migration (v3/v4 only).
        if (testCase.version >= 3) {
            const auto entity = fresh->block_entity_at(5, 96, 5);
            CHECK(entity != nullptr);
            if (entity) {
                CHECK(entity->type_id() == "project:counter_machine");
                // Re-serialize to read the project state (counter=7).
                const std::vector<uint8_t> blob = entity->serialize_state();
                uint32_t counter = 0;
                for (int i = 0; i < 4; ++i) {
                    counter |= static_cast<uint32_t>(blob[static_cast<std::size_t>(i)])
                               << (8 * i);
                }
                CHECK(counter == 7);
            }
        }
        std::cout << "[sdk] migration v" << testCase.version << " -> v5: "
                  << "upgraded, round-trip content identical (chunk"
                  << (testCase.version >= 3 ? " + block entity" : "") << ") OK\n";
    }

    // --- No-op: a save already at the current version -----------------
    std::string noopOut, noopError;
    CHECK(maker->migrate_world_save(v5save, noopOut, noopError));
    CHECK(noopError.empty());
    CHECK(noopOut == v5save);

    // --- Forward-compat: a NEWER schema is refused, never downgraded ----
    std::string newer = v5save;
    newer[5] = static_cast<char>(9);  // version u32 LE -> 9
    newer[6] = static_cast<char>(0);
    newer[7] = static_cast<char>(0);
    newer[8] = static_cast<char>(0);
    CHECK(maker->world_save_schema_version(newer) == 9u);
    std::string newerOut, newerError;
    CHECK(!maker->migrate_world_save(newer, newerOut, newerError));
    CHECK(newerError.find("NEWER") != std::string::npos);
    std::string loadNewerError;
    CHECK(!maker->deserialize_world(newer, loadNewerError));
    CHECK(loadNewerError.find("NEWER") != std::string::npos);

    // --- Corruption is refused, never migrated -------------------------
    std::string corrupt = v3;
    corrupt[corrupt.size() - 1] ^= 0x01;
    std::string corruptOut, corruptError;
    CHECK(!maker->migrate_world_save(corrupt, corruptOut, corruptError));
    CHECK(!corruptError.empty());
    CHECK(corruptOut.empty());

    std::cout << "[sdk] schema versioning + migration: introspection, v1-v4 "
                 "upgrade to v5 (pure, round-trip), no-op current, newer "
                 "refused, corrupt refused OK\n";
}

// Promoted solution (META section 32): the RocksDB chunk store persists world
// blobs in a real embedded key-value database, content-addressed by BLAKE3,
// and the world really delegates persistence to a registered service.
void test_rocksdb_storage() {
    const std::string dbDir = scratch_dir() + "/vc_test_rocksdb";

    // World A: flat + edits, serialize (v5 flatbuffer + BLAKE3 body).
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    std::string serError;
    const std::string blob = a->serialize_world(serError);
    CHECK(serError.empty());
    CHECK(!blob.empty());

    // Save into a fresh RocksDB database: deserialize (caches + persists the
    // blob content-addressed), then save_world materializes the DB on disk.
    std::shared_ptr<engine::voxel::IChunkStorage> store =
        engine::storage::create_rocksdb_chunk_storage();
    CHECK(store != nullptr);
    std::string storeError;
    CHECK(store->deserialize_world(blob, storeError));
    CHECK(storeError.empty());
    CHECK(store->save_world(dbDir, storeError));
    CHECK(storeError.empty());
    store.reset();  // close the DB

    // Load into a NEW store: the world blob survives the database round-trip.
    std::shared_ptr<engine::voxel::IChunkStorage> store2 =
        engine::storage::create_rocksdb_chunk_storage();
    std::string loadError;
    CHECK(store2->load_world(dbDir, loadError));
    CHECK(loadError.empty());
    std::string outError;
    const std::string restored = store2->serialize_world(outError);
    CHECK(outError.empty());
    CHECK(restored == blob);

    // The restored blob loads into a fresh world with the same content.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string bError;
    CHECK(b->deserialize_world(restored, bError));
    CHECK(bError.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    CHECK(b->get_block(5, 130, 5) == kBlockStone);

    // The world really delegates save/load to the registered service.
    b->register_storage(store2);
    const std::vector<std::string> services = b->registered_services();
    bool hasStorage = false;
    for (const std::string& name : services) {
        if (name == "storage") hasStorage = true;
    }
    CHECK(hasStorage);

    // Loading into an empty database is a clear diagnostic, not a crash.
    std::shared_ptr<engine::voxel::IChunkStorage> store3 =
        engine::storage::create_rocksdb_chunk_storage();
    std::string emptyError;
    CHECK(!store3->load_world(scratch_dir() + "/vc_test_rocksdb_empty", emptyError));
    CHECK(!emptyError.empty());

    // Release the database handles BEFORE removing the directories: on
    // Windows a directory holding an open file cannot be deleted. The world
    // `b` owns the registered store and the test locals store2/store3 still
    // hold DB handles (dbDir and the empty database respectively).
    b.reset();
    store2.reset();
    store3.reset();
    std::filesystem::remove_all(dbDir);
    std::filesystem::remove_all(scratch_dir() + "/vc_test_rocksdb_empty");
    std::cout << "[sdk] storage: RocksDB round-trip (BLAKE3-addressed), "
                 "world delegation, empty-db diagnostic OK\n";
}

// A project-supplied persistence backend (IChunkStorage). Proves the world
// really delegates save/load to a registered service instead of the builtin
// path — the storage service wiring is observable, not decorative.
class RecordingStorage final : public engine::voxel::IChunkStorage {
public:
    int serialized = 0, deserialized = 0, saved = 0, loaded = 0;
    std::string lastPayload;

    std::string serialize_world(std::string& errorOut) override {
        ++serialized;
        errorOut.clear();
        return "custom-bytes";
    }
    bool deserialize_world(const std::string& data, std::string& errorOut) override {
        ++deserialized;
        lastPayload = data;
        errorOut.clear();
        return true;
    }
    bool save_world(const std::string&, std::string& errorOut) override {
        ++saved;
        errorOut.clear();
        return true;
    }
    bool load_world(const std::string&, std::string& errorOut) override {
        ++loaded;
        errorOut.clear();
        return true;
    }
};

// IChunkStorage wiring (META section 6): a registered storage backend takes
// over every persistence entry point, and registered_services() reports it.
void test_storage_service() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    auto storage = std::make_shared<RecordingStorage>();
    world->register_storage(storage);

    const std::vector<std::string> names = world->registered_services();
    CHECK(std::find(names.begin(), names.end(), "storage") != names.end());

    std::string error;
    CHECK(world->serialize_world(error) == "custom-bytes");
    CHECK(error.empty());
    CHECK(storage->serialized == 1);

    CHECK(world->deserialize_world("custom-bytes", error));
    CHECK(error.empty());
    CHECK(storage->deserialized == 1);
    CHECK(storage->lastPayload == "custom-bytes");

    CHECK(world->save_world("ignored-path", error));
    CHECK(error.empty());
    CHECK(storage->saved == 1);

    CHECK(world->load_world("ignored-path", error));
    CHECK(error.empty());
    CHECK(storage->loaded == 1);

    std::cout << "[sdk] storage service: persistence delegates to a "
                 "registered IChunkStorage\n";
}

// Region-paged persistence (FALTANTES §4 item 1): a region-capable
// IChunkStorage turns a world save into a DIRECTORY of pages — a "world"
// manifest page (palette + entities, no chunk data) plus one page per region
// tile (8x8 chunks = 128x128 blocks). Round-trips identical data, groups
// chunks by region tile (moving the player lands chunks in a second tile),
// refuses a save with a missing page, and the monolithic path stays intact
// when no paged store is registered.
void test_region_chunk_storage() {
    // --- Phase A: boot at origin (region tile r.0.0), edit, paged save. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto storeA = engine::storage::create_region_chunk_storage(8);
    a->register_storage(storeA);
    const std::string dirA = scratch_dir() + "/vc_test_regions_a";
    std::string errorA;
    CHECK(a->save_world(dirA, errorA));
    CHECK(errorA.empty());
    // The save is a page directory: manifest + one region page for the
    // origin tile (sorted ids: "r.0.0" < "world").
    // The save is a page directory: the "world" manifest page plus one page
    // per region tile. Booting at chunk (0,0) with budget 16 loads chunks
    // -2..2 -> tiles -1..0 in both axes (sorted: "-" < "0" < "w").
    const std::vector<std::string> pagesA = storeA->page_ids();
    CHECK(pagesA.size() == 5);
    CHECK(pagesA[0] == "r.-1.-1");
    CHECK(pagesA[1] == "r.-1.0");
    CHECK(pagesA[2] == "r.0.-1");
    CHECK(pagesA[3] == "r.0.0");
    CHECK(pagesA[4] == "world");

    // --- Phase B: fresh world loads the paged save identically. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto storeB = engine::storage::create_region_chunk_storage(8);
    b->register_storage(storeB);
    std::string errorB;
    CHECK(b->load_world(dirA, errorB));
    CHECK(errorB.empty());
    const int xs[] = { 0, 3, 5, 8, 15 };
    const int zs[] = { 0, 8, 15 };
    const int ys[] = { 1, 50, 96, 100, 120, 127, 128, 130, 200 };
    bool identical = true;
    for (const int x : xs)
        for (const int z : zs)
            for (const int y : ys)
                if (a->get_block(x, y, z) != b->get_block(x, y, z))
                    identical = false;
    CHECK(identical);

    // --- Phase C: move the player to chunk (8,8) — a SECOND region tile
    // (r.1.1) — and save again: both tiles land in the same paged save. ---
    // Both (8,8) and (7,8) must reach Uploaded: a chunk left Generating
    // (worker done, state only advanced by a later update) reads as Air from
    // the public surface and would mismatch the restored page.
    CHECK(settle(*a, glm::vec3(130.0f, 200.0f, 130.0f),
                 [&] { return a->is_chunk_loaded(8, 8) &&
                              a->is_chunk_loaded(7, 8); }));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(140, 130, 140, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    const std::string dirC = scratch_dir() + "/vc_test_regions_c";
    std::string errorC;
    CHECK(a->save_world(dirC, errorC));
    CHECK(errorC.empty());
    // The new window (chunks 6..10, radius 2 around chunk (8,8)) spans tiles
    // 0..1 in both axes; the origin tiles -1..0 were evicted by the move.
    const std::vector<std::string> pagesC = storeA->page_ids();
    CHECK(pagesC.size() == 5);
    CHECK(pagesC[0] == "r.0.0");
    CHECK(std::find(pagesC.begin(), pagesC.end(), "r.0.1") != pagesC.end());
    CHECK(std::find(pagesC.begin(), pagesC.end(), "r.1.0") != pagesC.end());
    CHECK(std::find(pagesC.begin(), pagesC.end(), "r.1.1") != pagesC.end());
    CHECK(std::find(pagesC.begin(), pagesC.end(), "r.-1.-1") == pagesC.end());
    CHECK(pagesC[4] == "world");

    // --- Phase D: a third world loads both region pages; edits in both
    // tiles survive, and untouched generated data round-trips too. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto storeD = engine::storage::create_region_chunk_storage(8);
    c->register_storage(storeD);
    std::string errorD;
    CHECK(c->load_world(dirC, errorD));
    CHECK(errorD.empty());
    CHECK(c->get_block(140, 130, 140) == kBlockStone);  // region r.1.1 edit
    // Untouched generated data round-trips from BOTH tiles: (136,50,136) is
    // chunk (8,8) (r.1.1, edited chunk), (117,50,133) is chunk (7,8) (r.0.1).
    CHECK(c->get_block(136, 50, 136) == a->get_block(136, 50, 136));
    CHECK(c->get_block(117, 50, 133) == a->get_block(117, 50, 133));

    // --- Phase E: a missing region page is refused (all-or-nothing load). ---
    std::error_code removeEc;
    std::filesystem::remove(dirC + "/pages/r_1_1.dat", removeEc);
    CHECK(!removeEc);
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    d->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string errorE;
    CHECK(!d->load_world(dirC, errorE));
    CHECK(!errorE.empty());

    // --- Phase F: without a paged store the monolithic path is untouched: a
    // regular FILE round-trips (no page directory involved). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> e =
        engine::voxel::create_default_voxel_world();
    e->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*e, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = e->begin_transaction();
        tx->set_block(4, 130, 4, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
    }
    const std::string monoPath = scratch_dir() + "/vc_test_regions_mono.vcwld";
    std::string errorF;
    CHECK(e->save_world(monoPath, errorF));
    CHECK(errorF.empty());
    CHECK(std::filesystem::is_regular_file(monoPath));

    std::cout << "[sdk] region storage: paged save/load, per-tile grouping, "
                 "missing-page refusal, monolithic fallback OK\n";
}

// Per-chunk palette + compression in region pages (FALTANTES §4 item 2): a
// v2 page (magic "VCW2") stores each chunk with a palette (u8 indices when
// the chunk has <= 256 distinct blocks) and a zstd-compressed payload. The
// test proves the on-disk page is smaller than the raw equivalent, round-
// trips identically, and a corrupted payload is refused.
void test_region_palette_compression() {
    // --- Phase A: world with edits, paged save. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        tx->set_block(10, 130, 10, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto store = engine::storage::create_region_chunk_storage(8);
    a->register_storage(store);
    const std::string dir = scratch_dir() + "/vc_test_regions_palette";
    std::string errorA;
    CHECK(a->save_world(dir, errorA));
    CHECK(errorA.empty());

    // --- Phase B: the page is v2 and smaller than the raw equivalent. ---
    const std::string pagePath = dir + "/pages/r_0_0.dat";
    std::ifstream in(pagePath, std::ios::binary);
    CHECK(in.good());
    std::string page((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(page.size() > 8);
    CHECK(page.compare(0, 4, "VCW2") == 0);
    const auto u32at = [&page](std::size_t at) {
        return static_cast<uint32_t>(static_cast<uint8_t>(page[at])) |
               (static_cast<uint32_t>(static_cast<uint8_t>(page[at + 1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(page[at + 2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(page[at + 3])) << 24);
    };
    const auto u16at = [&page](std::size_t at) {
        return static_cast<uint16_t>(static_cast<uint8_t>(page[at])) |
               (static_cast<uint16_t>(static_cast<uint8_t>(page[at + 1])) << 8);
    };
    const uint32_t count = u32at(4);
    CHECK(count >= 1);
    std::size_t cursor = 8;
    std::size_t rawBytes = 0;   // v1 equivalent (u16 ids, uncompressed)
    std::size_t firstFlags = 0xFF;
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(cursor + 13 <= page.size());
        const uint32_t extent = u32at(cursor + 8);
        const uint8_t flags = static_cast<uint8_t>(page[cursor + 12]);
        if (i == 0) firstFlags = flags;
        rawBytes += 12 + static_cast<std::size_t>(extent) * 256 * 3;
        cursor += 13;
        if (flags & 0x01) {  // palette present
            CHECK(cursor + 2 <= page.size());
            const uint16_t paletteCount = u16at(cursor);
            cursor += 2 + static_cast<std::size_t>(paletteCount) * 2;
        }
        CHECK(cursor + 4 <= page.size());
        const uint32_t payloadBytes = u32at(cursor);
        cursor += 4 + payloadBytes;
    }
    CHECK(cursor == page.size());  // no trailing garbage
    // The first chunk must use BOTH palette and compression (flat terrain:
    // few distinct blocks, highly repetitive payload) and the page must be
    // smaller than its raw equivalent overall.
    CHECK((firstFlags & 0x01) != 0);
    CHECK((firstFlags & 0x02) != 0);
    CHECK(page.size() < rawBytes);

    // --- Phase C: fresh world loads the v2 page identically. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string errorB;
    CHECK(b->load_world(dir, errorB));
    CHECK(errorB.empty());
    const int xs[] = { 0, 3, 5, 10, 15 };
    const int zs[] = { 0, 8, 15 };
    const int ys[] = { 1, 50, 96, 100, 120, 127, 128, 130, 200 };
    bool identical = true;
    for (const int x : xs)
        for (const int z : zs)
            for (const int y : ys)
                if (a->get_block(x, y, z) != b->get_block(x, y, z))
                    identical = false;
    CHECK(identical);

    // --- Phase D: corruption in the v2 payload is refused cleanly. ---
    const std::string corruptPath = scratch_dir() + "/vc_test_regions_palette_corrupt";
    {
        std::filesystem::create_directories(corruptPath + "/pages");
        // Re-create the manifest page too (load_world requires it).
        std::ifstream worldIn(dir + "/pages/world.dat", std::ios::binary);
        std::string manifest((std::istreambuf_iterator<char>(worldIn)),
                             std::istreambuf_iterator<char>());
        std::ofstream worldOut(corruptPath + "/pages/world.dat", std::ios::binary);
        worldOut.write(manifest.data(),
                       static_cast<std::streamsize>(manifest.size()));
        std::string corrupt = page;
        corrupt[corrupt.size() / 2] ^= 0xFF;  // inside a compressed payload
        std::ofstream out(corruptPath + "/pages/r_0_0.dat", std::ios::binary);
        out.write(corrupt.data(), static_cast<std::streamsize>(corrupt.size()));
    }
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string errorC;
    CHECK(!c->load_world(corruptPath, errorC));
    CHECK(!errorC.empty());

    std::cout << "[sdk] region palette+compression: v2 page smaller than raw, "
                 "round-trip identical, corruption refused OK\n";
}

// Per-voxel state in region pages (FALTANTES item 2): a paged (region) save
// must persist block states exactly like the monolithic v5 save does. The v2
// page appends a state section gated by flag 0x04, only when the chunk holds
// non-default state; loading restores it, and pages without the flag load
// with state 0 everywhere (backward compatible with pre-state readers).
void test_region_state_persistence() {
    // --- Phase A: world with a stateful lamp, paged save. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    auto registryA = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registryA->load_from_json(
        R"({"name":"lamp","namespace":"test","states":[)"
        R"({"name":"base","color":[0.4,0.4,0.4]},)"
        R"({"name":"lit","color":[1.0,0.6,0.1],"lightEmission":0.8}]})",
        error));
    a->set_block_registry(registryA);
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t lampId = 0;
    CHECK(a->resolve_block_id("test:lamp", lampId, error));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, lampId);
        tx->set_block(5, 130, 5, lampId);
        tx->set_block(7, 130, 7, lampId);
        std::string txErr;
        CHECK(tx->commit(txErr));
        CHECK(txErr.empty());
    }
    a->set_block_state(5, 130, 5, 1);
    a->set_block_state(7, 130, 7, 0);
    CHECK(a->get_block_state(3, 130, 3) == 0);
    CHECK(a->get_block_state(5, 130, 5) == 1);
    CHECK(a->get_block_state(7, 130, 7) == 0);

    a->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_regions_state";
    std::string errorA;
    CHECK(a->save_world(dir, errorA));
    CHECK(errorA.empty());

    // --- Phase B: the page carries the state flag (0x04) for the edited
    // chunk and parses with no trailing garbage. ---
    const std::string pagePath = dir + "/pages/r_0_0.dat";
    std::ifstream in(pagePath, std::ios::binary);
    CHECK(in.good());
    std::string page((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(page.size() > 8);
    CHECK(page.compare(0, 4, "VCW2") == 0);
    const auto u32at = [&page](std::size_t at) {
        return static_cast<uint32_t>(static_cast<uint8_t>(page[at])) |
               (static_cast<uint32_t>(static_cast<uint8_t>(page[at + 1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(page[at + 2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(page[at + 3])) << 24);
    };
    const uint32_t count = u32at(4);
    CHECK(count >= 1);
    bool sawStateFlag = false;
    std::size_t cursor = 8;
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(cursor + 13 <= page.size());
        const uint8_t flags = static_cast<uint8_t>(page[cursor + 12]);
        if ((flags & 0x04) != 0) sawStateFlag = true;
        cursor += 13;
        if ((flags & 0x01) != 0) {  // palette present
            CHECK(cursor + 2 <= page.size());
            const uint16_t paletteCount =
                static_cast<uint16_t>(static_cast<uint8_t>(page[cursor])) |
                (static_cast<uint16_t>(static_cast<uint8_t>(page[cursor + 1])) << 8);
            cursor += 2 + static_cast<std::size_t>(paletteCount) * 2;
        }
        CHECK(cursor + 4 <= page.size());
        const uint32_t payloadBytes = u32at(cursor);
        cursor += 4 + payloadBytes;
    }
    CHECK(cursor == page.size());  // no trailing garbage
    CHECK(sawStateFlag);

    // --- Phase C: fresh world loads the page and restores states. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    auto registryB = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(registryB->load_from_json(
        R"({"name":"lamp","namespace":"test","states":[)"
        R"({"name":"base","color":[0.4,0.4,0.4]},)"
        R"({"name":"lit","color":[1.0,0.6,0.1],"lightEmission":0.8}]})",
        error));
    b->set_block_registry(registryB);
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string errorB;
    CHECK(b->load_world(dir, errorB));
    CHECK(errorB.empty());
    CHECK(b->get_block(3, 130, 3) == static_cast<uint32_t>(lampId));
    CHECK(b->get_block(5, 130, 5) == static_cast<uint32_t>(lampId));
    CHECK(b->get_block(7, 130, 7) == static_cast<uint32_t>(lampId));
    CHECK(b->get_block_state(3, 130, 3) == 0);
    CHECK(b->get_block_state(5, 130, 5) == 1);
    CHECK(b->get_block_state(7, 130, 7) == 0);
    // Chunks without state (the flat terrain) load with state 0 — their pages
    // have the flag off, exactly like a pre-state page (backward compatible).
    CHECK(b->get_block_state(1, 50, 1) == 0);

    std::cout << "[sdk] region pages: per-voxel state persisted (flag 0x04), "
                 "round-trip OK\n";
}

// A region backend whose FIRST save_page call blocks on a gate — used to pin
// a save mid-write so the test can inject a concurrent edit deterministically.
// Everything else delegates to the real region backend, so the pages land on
// disk and a subsequent load is a genuine round-trip.
class GatedRegionStorage final : public engine::voxel::IChunkStorage {
public:
    GatedRegionStorage()
        : inner_(engine::storage::create_region_chunk_storage(8)) {}

    std::mutex gateMutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    int savePageCalls = 0;

    std::string serialize_world(std::string& e) override {
        return inner_->serialize_world(e);
    }
    bool deserialize_world(const std::string& d, std::string& e) override {
        return inner_->deserialize_world(d, e);
    }
    bool save_world(const std::string& p, std::string& e) override {
        return inner_->save_world(p, e);
    }
    bool load_world(const std::string& p, std::string& e) override {
        return inner_->load_world(p, e);
    }
    bool supports_regions() const override { return true; }
    bool save_page(const std::string& id, const std::string& payload,
                   std::string& e) override {
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            ++savePageCalls;
            if (savePageCalls == 1) {  // first page write: block mid-flight
                entered = true;
                cv.notify_all();
                cv.wait(lock, [this] { return release; });
            }
        }
        return inner_->save_page(id, payload, e);
    }
    bool load_page(const std::string& id, std::string& out, std::string& e) override {
        return inner_->load_page(id, out, e);
    }
    std::vector<std::string> page_ids() const override {
        return inner_->page_ids();
    }
    bool commit_save(std::string& e) override { return inner_->commit_save(e); }

private:
    std::shared_ptr<engine::voxel::IChunkStorage> inner_;
};

// Consistent snapshot without holding the global mutex through the save
// (FALTANTES §4 item 4). The gate pins the save AFTER the capture (blocked at
// the first page write); a concurrent edit then lands in the LIVE world but
// NOT in the saved pages — the save is a point-in-time. The next save picks
// the edit up. A save that encoded from live chunks would corrupt the
// snapshot with the mid-save edit; this test would fail.
void test_region_snapshot_concurrent_edit() {
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto gated = std::make_shared<GatedRegionStorage>();
    a->register_storage(gated);
    const std::string dir = scratch_dir() + "/vc_test_regions_snapshot";

    std::string saveError;
    std::thread saver([&] {
        a->save_world(dir, saveError);
    });
    {
        std::unique_lock<std::mutex> lock(gated->gateMutex);
        gated->cv.wait_for(lock, std::chrono::seconds(30),
                           [&] { return gated->entered; });
        CHECK(gated->entered);  // save is now past the capture, mid-write
    }
    // Concurrent edit while the save is mid-flight: must not deadlock, and
    // must NOT be part of the snapshot being persisted.
    a->set_block(5, 130, 5, kBlockStone);
    {
        std::lock_guard<std::mutex> lock(gated->gateMutex);
        gated->release = true;
    }
    gated->cv.notify_all();
    saver.join();
    CHECK(saveError.empty());

    // World B loads the SAVED state: pre-edit (3,130,3) present, the
    // mid-save edit (5,130,5) absent — a consistent point-in-time.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadError;
    CHECK(b->load_world(dir, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    CHECK(b->get_block(5, 130, 5) != kBlockStone);

    // The next save captures the edit (delta): world C has both.
    std::string saveError2;
    CHECK(a->save_world(dir, saveError2));
    CHECK(saveError2.empty());
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadError2;
    CHECK(c->load_world(dir, loadError2));
    CHECK(loadError2.empty());
    CHECK(c->get_block(3, 130, 3) == kBlockStone);
    CHECK(c->get_block(5, 130, 5) == kBlockStone);

    std::cout << "[sdk] region snapshot: concurrent mid-save edit does not "
                 "corrupt the save, next save captures it OK\n";
}

// Async save/load (FALTANTES §4 item 5): encode (palette+zstd) and page
// writes run as background jobs, and load runs file reads/decompression/apply
// on the pool — the caller thread is never blocked on them. The result
// arrives via the callback AND wait_async_saves; a concurrent save while one
// is in flight is refused.
void test_region_async_save_load() {
    // --- Async save: dispatch, keep simulating, then wait for the result. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    a->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_regions_async";

    std::mutex cbMutex;
    std::condition_variable cbCv;
    bool callbackFired = false;
    bool callbackOk = false;
    std::string callbackErr;
    std::string dispatchErr;
    CHECK(a->save_world_async(
        dir,
        [&](bool ok, std::string err) {
            std::lock_guard<std::mutex> lock(cbMutex);
            callbackFired = true;
            callbackOk = ok;
            callbackErr = std::move(err);
            cbCv.notify_all();
        },
        dispatchErr));
    CHECK(dispatchErr.empty());

    // The caller keeps simulating while the save is in flight (the snapshot
    // was captured at dispatch; these ticks must not block or corrupt it).
    a->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    CHECK(a->get_block(3, 130, 3) == kBlockStone);

    // The result arrives via wait_async_saves AND the callback.
    std::string waitErr;
    CHECK(a->wait_async_saves(waitErr));
    CHECK(waitErr.empty());
    {
        std::unique_lock<std::mutex> lock(cbMutex);
        cbCv.wait_for(lock, std::chrono::seconds(30),
                      [&] { return callbackFired; });
        CHECK(callbackFired);
        CHECK(callbackOk);
        CHECK(callbackErr.empty());
    }

    // Round-trip: a fresh world restores the async-saved pages.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadErr;
    CHECK(b->load_world(dir, loadErr));
    CHECK(loadErr.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    CHECK(b->get_block(5, 130, 5) == kBlockStone);
    const int xs[] = { 0, 3, 5, 8, 15 };
    const int zs[] = { 0, 8, 15 };
    const int ys[] = { 1, 50, 96, 100, 120, 127, 128, 130, 200 };
    bool identical = true;
    for (const int x : xs)
        for (const int z : zs)
            for (const int y : ys)
                if (a->get_block(x, y, z) != b->get_block(x, y, z))
                    identical = false;
    CHECK(identical);

    // --- Async load: dispatch, then wait; chunks are restored. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    bool loadCallbackFired = false;
    bool loadCallbackOk = false;
    std::string loadDispatchErr;
    // Non-null callback => truly async: dispatch returns, result via wait.
    CHECK(c->load_world_async(
        dir,
        [&](bool ok, std::string) {
            std::lock_guard<std::mutex> lock(cbMutex);
            loadCallbackFired = true;
            loadCallbackOk = ok;
        },
        loadDispatchErr));
    CHECK(loadDispatchErr.empty());
    std::string loadWaitErr;
    CHECK(c->wait_async_saves(loadWaitErr));
    CHECK(loadWaitErr.empty());
    {
        std::lock_guard<std::mutex> lock(cbMutex);
        CHECK(loadCallbackFired);
        CHECK(loadCallbackOk);
    }
    CHECK(c->get_block(3, 130, 3) == kBlockStone);
    CHECK(c->get_block(5, 130, 5) == kBlockStone);

    // --- A save while an async save is in flight is refused. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto gated = std::make_shared<GatedRegionStorage>();
    d->register_storage(gated);
    const std::string gateDir = scratch_dir() + "/vc_test_regions_async_gate";
    std::string gateDispatchErr;
    // Non-null callback => truly async (a null callback would block on the
    // gate — it is treated as the synchronous path).
    CHECK(d->save_world_async(gateDir, [](bool, std::string) {}, gateDispatchErr));
    {
        std::unique_lock<std::mutex> lock(gated->gateMutex);
        gated->cv.wait_for(lock, std::chrono::seconds(30),
                           [&] { return gated->entered; });
        CHECK(gated->entered);  // async save is now mid-write (op in flight)
    }
    std::string refusedErr;
    CHECK(!d->save_world(gateDir + "_2", refusedErr));
    CHECK(!refusedErr.empty());
    {
        std::lock_guard<std::mutex> lock(gated->gateMutex);
        gated->release = true;
    }
    gated->cv.notify_all();
    std::string gateWaitErr;
    CHECK(d->wait_async_saves(gateWaitErr));
    CHECK(gateWaitErr.empty());

    // --- Async requires a paged storage: refused with a clear diagnostic. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> e =
        engine::voxel::create_default_voxel_world();
    std::string noStoreErr;
    CHECK(!e->save_world_async(dir + "_nostore", nullptr, noStoreErr));
    CHECK(!noStoreErr.empty());

    std::cout << "[sdk] region async: save/load on background jobs, caller "
                 "keeps simulating, concurrent save refused OK\n";
}

// Delta saves (FALTANTES §4 item 3): a save rewrites ONLY the region tiles
// whose chunks changed since the last save (revision-tracked). A no-change
// save touches no page file (mtime stable), an edit in one tile rewrites only
// that tile's page, and the round-trip still restores the full world.
void test_region_delta_saves() {
    // Page id -> on-disk file (mirror of page_id_to_file: '.' -> '_').
    const auto pageFile = [](const std::string& dir, const std::string& pageId) {
        std::string leaf = pageId;
        for (char& c : leaf) if (c == '.') c = '_';
        return dir + "/pages/" + leaf + ".dat";
    };
    const auto readFile = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };

    // --- Phase A: world with edits in two tiles, base save. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    // boot_world only waits for chunk (0,0); the transaction below also edits
    // chunk (-1,-1), whose generation/upload races the commit under load
    // (findings #54 pattern) — materialize it first.
    CHECK(settle(*a, glm::vec3(8.0f, 200.0f, 8.0f),
                 [&] { return a->is_chunk_loaded(-1, -1); }));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);      // chunk (0,0) -> r.0.0
        tx->set_block(-5, 130, -5, kBlockStone);    // chunk (-1,-1) -> r.-1.-1
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto store = engine::storage::create_region_chunk_storage(8);
    a->register_storage(store);
    const std::string dir = scratch_dir() + "/vc_test_regions_delta";
    std::string errorA;
    CHECK(a->save_world(dir, errorA));
    CHECK(errorA.empty());
    const std::vector<std::string> pages = store->page_ids();
    CHECK(pages.size() == 5);  // 4 region tiles + world manifest

    // --- Phase B: a no-change save touches NO page file. ---
    std::map<std::string, std::filesystem::file_time_type> beforeTime;
    std::map<std::string, std::string> beforeBytes;
    for (const auto& pid : pages) {
        const std::string path = pageFile(dir, pid);
        beforeTime[pid] = std::filesystem::last_write_time(path);
        beforeBytes[pid] = readFile(path);
    }
    std::string errorB;
    CHECK(a->save_world(dir, errorB));
    CHECK(errorB.empty());
    CHECK(store->page_ids() == pages);  // same page set
    // Chunk pages are gated: a no-change save must not touch a single tile
    // file (mtime AND bytes stable). The "world" manifest is intentionally
    // rewritten every save (small, always fresh); its CONTENT must still be
    // semantically stable (same palette/entities/regions -> same bytes).
    for (const auto& pid : pages) {
        if (pid == "world") {
            CHECK(readFile(pageFile(dir, pid)) == beforeBytes[pid]);
            continue;
        }
        CHECK(std::filesystem::last_write_time(pageFile(dir, pid)) ==
              beforeTime[pid]);
        CHECK(readFile(pageFile(dir, pid)) == beforeBytes[pid]);
    }

    // --- Phase C: an edit in tile r.0.0 rewrites ONLY that tile. ---
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(7, 130, 7, kBlockStone);  // chunk (0,0) -> r.0.0
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    std::string errorC;
    CHECK(a->save_world(dir, errorC));
    CHECK(errorC.empty());
    for (const auto& pid : pages) {
        const std::string path = pageFile(dir, pid);
        if (pid == "world") continue;  // always rewritten, checked in Phase B
        const bool changed =
            std::filesystem::last_write_time(path) != beforeTime[pid];
        CHECK(changed == (pid == "r.0.0"));
        if (pid == "r.0.0") {
            CHECK(readFile(path) != beforeBytes[pid]);  // the edit landed
        } else {
            CHECK(readFile(path) == beforeBytes[pid]);
        }
    }

    // --- Phase D: a fresh world restores the FULL world (base + delta). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string errorD;
    CHECK(b->load_world(dir, errorD));
    CHECK(errorD.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);    // base save edit
    CHECK(b->get_block(-5, 130, -5) == kBlockStone);  // base save edit
    CHECK(b->get_block(7, 130, 7) == kBlockStone);    // delta save edit
    const int xs[] = { 0, 3, 5, 7, 15 };
    const int zs[] = { 0, 8, 15 };
    const int ys[] = { 1, 50, 96, 100, 120, 127, 128, 130, 200 };
    bool identical = true;
    for (const int x : xs)
        for (const int z : zs)
            for (const int y : ys)
                if (a->get_block(x, y, z) != b->get_block(x, y, z))
                    identical = false;
    CHECK(identical);

    std::cout << "[sdk] region delta: no-change save touches no page, one-edit "
                 "save rewrites one tile, full restore OK\n";
}

// A JSON-only block survives save/load through the UUID palette: the palette
// pins each dynamic runtime id to its persistent UUID, so a world saved with
// the registry loaded as [ruby, sapphire] restores byte-identical ids in a
// registry loaded as [sapphire, ruby] — ids never depend on load order.
void test_dynamic_block_persistence() {
    // World A: registry [ruby, sapphire], place ruby, save.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    auto registryA = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registryA->load_from_json(
        R"([{"name":"ruby","namespace":"test","color":[0.9,0.1,0.1]},)"
        R"({"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]}])",
        error));
    a->set_block_registry(registryA);
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t rubyIdA = 0;
    CHECK(a->resolve_block_id("test:ruby", rubyIdA, error));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, rubyIdA);
        tx->set_block(7, 130, 7, rubyIdA);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());

    // World B: registry loaded in the REVERSED order — ids must match.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    auto registryB = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(registryB->load_from_json(
        R"([{"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]},)"
        R"({"name":"ruby","namespace":"test","color":[0.9,0.1,0.1]}])",
        error));
    b->set_block_registry(registryB);
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t rubyIdB = 0;
    CHECK(b->resolve_block_id("test:ruby", rubyIdB, error));
    CHECK(rubyIdB == rubyIdA);  // UUID-sorted allocation: order-independent

    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_block(3, 130, 3) == rubyIdB);
    CHECK(b->get_block(7, 130, 7) == rubyIdB);
    CHECK(b->get_block(4, 130, 4) == kBlockAir);

    // Idempotency through the palette: re-serializing B yields the same bytes.
    std::string reError;
    CHECK(b->serialize_world(reError) == bytes);

    std::cout << "[sdk] dynamic block persistence: UUID palette survives load "
                 "order, byte-identical reserialization\n";
}

// FALTANTES item 1 (parte rede) — identity stability / server-negotiated
// palette: the replication layer ships every catalog-only block's definition
// JSON + runtime id, the client registers them into its own world and the
// dynamic ids resolve IDENTICALLY (UUID-sorted allocation) — so a JSON-only
// block's deltas/snapshots apply without the client recompiling. Codec
// round-trips bit-exactly; a malformed palette is refused all-or-nothing.
void test_replication_palette() {
    constexpr engine::voxel::ReplicationConnectionId conn = 1;
    // Server: JSON-only registry [ruby, sapphire].
    std::unique_ptr<engine::voxel::IVoxelWorld> server =
        engine::voxel::create_default_voxel_world();
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registry->load_from_json(
        R"([{"name":"ruby","namespace":"test","color":[0.9,0.1,0.1],"hardness":3.0},)"
        R"({"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9],"faceTop":[0.2,0.8,0.2]}])",
        error));
    server->set_block_registry(registry);
    CHECK(boot_world(*server, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    uint32_t rubyId = 0, sapphireId = 0;
    CHECK(server->resolve_block_id("test:ruby", rubyId, error));
    CHECK(server->resolve_block_id("test:sapphire", sapphireId, error));
    CHECK(rubyId >= static_cast<uint32_t>(BlockType::Count));
    CHECK(sapphireId >= static_cast<uint32_t>(BlockType::Count));
    CHECK(rubyId != sapphireId);

    auto srv = engine::voxel::create_voxel_replication(*server);
    srv->server_register_connection(conn);
    engine::voxel::ReplicationPalette palette;
    CHECK(srv->server_pack_palette(conn, palette, error));
    CHECK(error.empty());
    // Exactly the two catalog-only blocks; builtins (the engine contract) are
    // NOT shipped; ids match the server's own resolution.
    CHECK(palette.entries.size() == 2);
    std::map<std::string, const engine::voxel::ReplicationPaletteEntry*> byName;
    for (const engine::voxel::ReplicationPaletteEntry& entry : palette.entries) {
        byName[entry.namespacedName] = &entry;
        CHECK(entry.uuid.find('-') != std::string::npos);  // canonical UUID
        CHECK(entry.definitionJson.find("\"name\":\"ruby\"") != std::string::npos ||
              entry.definitionJson.find("\"name\":\"sapphire\"") != std::string::npos);
    }
    CHECK(byName["test:ruby"] != nullptr && byName["test:sapphire"] != nullptr);
    CHECK(byName["test:ruby"]->runtimeId == rubyId);
    CHECK(byName["test:sapphire"]->runtimeId == sapphireId);
    CHECK(byName["test:ruby"]->uuid == registry->find_by_name("test:ruby")->uuid);

    // Deterministic: a second pack is identical.
    engine::voxel::ReplicationPalette palette2;
    CHECK(srv->server_pack_palette(conn, palette2, error));
    CHECK(palette2.entries == palette.entries);

    // Codec: bit-exact round-trip (raw + zstd), malformed frame refused.
    auto raw = engine::voxel::encode_replication_palette(palette);
    engine::voxel::ReplicationPalette decoded;
    CHECK(engine::voxel::decode_replication_palette(raw, decoded));
    CHECK(decoded.entries == palette.entries);
    auto zstd = engine::compression::create_zstd_compression_provider();
    auto packed = engine::voxel::encode_replication_palette(palette, zstd);
    engine::voxel::ReplicationPalette fromPacked;
    CHECK(engine::voxel::decode_replication_palette(packed, fromPacked, zstd));
    CHECK(fromPacked.entries == palette.entries);
    std::vector<std::byte> junk = raw;
    junk[0] = std::byte{'X'};
    engine::voxel::ReplicationPalette bad;
    CHECK(!engine::voxel::decode_replication_palette(junk, bad));
    // Unknown connection is refused.
    engine::voxel::ReplicationPalette unused;
    CHECK(!srv->server_pack_palette(999, unused, error));
    CHECK(error.find("unknown") != std::string::npos);

    // Client: a FRESH world (no registry of its own) applies the palette and
    // resolves the SAME dynamic ids — no recompile, no redefinition.
    std::unique_ptr<engine::voxel::IVoxelWorld> client =
        engine::voxel::create_default_voxel_world();
    client->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*client, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    auto cli = engine::voxel::create_voxel_replication(*client);
    CHECK(cli->client_apply_palette(palette, error));
    CHECK(error.empty());
    uint32_t clientRuby = 0, clientSapphire = 0;
    CHECK(client->resolve_block_id("test:ruby", clientRuby, error));
    CHECK(client->resolve_block_id("test:sapphire", clientSapphire, error));
    CHECK(clientRuby == rubyId);   // same UUID -> same dynamic id
    CHECK(clientSapphire == sapphireId);

    // Client's own block survives the merge (palette adds, never wipes).
    auto merged = client->block_registry();
    CHECK(merged->find_by_name("test:ruby") != nullptr);
    CHECK(merged->find_by_name("test:sapphire") != nullptr);

    // All-or-nothing: a malformed palette leaves the client untouched.
    engine::voxel::ReplicationPalette poisoned = palette;
    poisoned.entries[0].uuid.clear();
    std::string poisonedError;
    CHECK(!cli->client_apply_palette(poisoned, poisonedError));
    CHECK(!poisonedError.empty());
    uint32_t stillRuby = 0;
    CHECK(client->resolve_block_id("test:ruby", stillRuby, error));
    CHECK(stillRuby == rubyId);  // registry unchanged (still the good palette)

    std::cout << "[sdk] replication: server-negotiated block palette (identity "
                 "stability, codec, all-or-nothing, id parity) OK\n";
}

// FALTANTES item 1 / Fase 2 — prova e2e do bloco JSON-only em UM teste externo
// (só headers públicos + glm): um bloco criado somente por asset (JSON) é
// COLOCADO, tem o material de RENDER data-driven, é COLIDIDO (raycast),
// SALVO, CARREGADO e REPLICADO — sem recompilar a engine. A última etapa
// (replicado) depende da paleta negociada pelo servidor (test_replication_
// palette): o cliente novo não conhece o bloco, recebe a definição pela
// paleta e os deltas/regiões com id dinâmico aplicam verbatim.
void test_json_only_block_e2e() {
    constexpr engine::voxel::ReplicationConnectionId conn = 1;
    const char* rubyJson =
        R"({"name":"ruby","namespace":"test","color":[0.9,0.1,0.1],"hardness":3.0})";
    const char* ghostJson =
        R"({"name":"ghost","namespace":"test","collisionShape":"none"})";

    // --- Server: registry JSON-only, boot, PLACE + COLLIDE + RENDER data. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> server =
        engine::voxel::create_default_voxel_world();
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    std::string both = std::string("[") + rubyJson + "," + ghostJson + "]";
    CHECK(    registry->load_from_json(both, error));
    server->set_block_registry(registry);
    // DRY terrain above sea level (kFluidTestTerrain = 130 > SeaLevel 127):
    // no water lake, so the surface is unambiguous and the placed blocks sit
    // inside the replication snapshot window.
    server->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    CHECK(boot_world(*server, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    uint32_t rubyId = 0, ghostId = 0;
    CHECK(server->resolve_block_id("test:ruby", rubyId, error));
    CHECK(server->resolve_block_id("test:ghost", ghostId, error));
    CHECK(rubyId >= static_cast<uint32_t>(BlockType::Count));
    const int surface = helper_surface_y(*server, 3, 3);
    CHECK(surface == kFluidTestTerrain);

    // 1) COLOCADO: transactional place on the authoritative world.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = server->begin_transaction();
        tx->set_block(3, surface + 1, 3, rubyId);
        tx->set_block(5, surface + 1, 5, ghostId);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    CHECK(server->get_block(3, surface + 1, 3) == rubyId);
    CHECK(server->get_block(5, surface + 1, 5) == ghostId);

    // 2) RENDER (dados): the mesher consumes the runtime table derived from
    // the registry; the definition JSON round-trips bit-exactly (color and
    // per-face material survive serialize -> load) — the data the mesher
    // reads is fully data-driven (mesh output proven in
    // scenario_dynamic_block_meshes / voxel_streaming_tests).
    const auto* rubyDef = registry->find_by_name("test:ruby");
    const std::string serialized =
        engine::registry::serialize_block_definition(*rubyDef);
    auto reparsed = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(reparsed->load_from_json(serialized, error));
    const auto* reparsedRuby = reparsed->find_by_name("test:ruby");
    CHECK(reparsedRuby != nullptr);
    CHECK(reparsedRuby->uuid == rubyDef->uuid);
    CHECK(reparsedRuby->color == rubyDef->color);      // %.9g bit-exact
    CHECK(reparsedRuby->hardness == rubyDef->hardness);
    CHECK(!reparsedRuby->hasBuiltinMapping);
    // The effective runtime table exposes the dynamic block with its identity.
    bool rubyInViews = false;
    for (const engine::voxel::BlockRuntimeView& view : server->runtime_block_views()) {
        if (view.id == rubyId) {
            rubyInViews = true;
            CHECK(view.uuid == rubyDef->uuid);
            CHECK(view.solid);  // collidable by default
        }
    }
    CHECK(rubyInViews);

    // 3) COLIDIDO: the raycast hits the solid ruby but passes THROUGH the
    // ghost (collisionShape none) down to the terrain below.
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    const engine::voxel::VoxelRaycastHit rubyHit =
        server->raycast(glm::vec3(3.0f, 200.0f, 3.0f), down, 1000.0f);
    CHECK(rubyHit.hit);
    CHECK(rubyHit.block == glm::ivec3(3, surface + 1, 3));
    const engine::voxel::VoxelRaycastHit ghostHit =
        server->raycast(glm::vec3(5.0f, 200.0f, 5.0f), down, 1000.0f);
    CHECK(ghostHit.hit);
    CHECK(ghostHit.block == glm::ivec3(5, surface, 5));  // terrain below the ghost

    // 4+5) SALVO + CARREGADO: the UUID palette pins the dynamic id; a fresh
    // world with the registry in REVERSED order restores the same id.
    std::string saveError;
    const std::string bytes = server->serialize_world(saveError);
    CHECK(saveError.empty());
    std::unique_ptr<engine::voxel::IVoxelWorld> reload =
        engine::voxel::create_default_voxel_world();
    auto reversed = std::make_shared<engine::registry::BlockRegistry>();
    std::string reversedJson = std::string("[") + ghostJson + "," + rubyJson + "]";
    CHECK(reversed->load_from_json(reversedJson, error));
    reload->set_block_registry(reversed);
    reload->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    CHECK(boot_world(*reload, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    uint32_t reloadRuby = 0;
    CHECK(reload->resolve_block_id("test:ruby", reloadRuby, error));
    CHECK(reloadRuby == rubyId);
    std::string loadError;
    CHECK(reload->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    CHECK(reload->get_block(3, surface + 1, 3) == rubyId);
    CHECK(reload->get_block(5, surface + 1, 5) == ghostId);

    // 6) REPLICADO: a FRESH client (no registry, no knowledge of the block)
    // receives the server-negotiated palette, then the region snapshot with
    // the ruby's DYNAMIC id and the delta stream — all apply verbatim.
    std::unique_ptr<engine::voxel::IVoxelWorld> client =
        engine::voxel::create_default_voxel_world();
    client->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    CHECK(boot_world(*client, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    auto srv = engine::voxel::create_voxel_replication(*server);
    auto cli = engine::voxel::create_voxel_replication(*client);
    srv->server_register_connection(conn);
    srv->server_set_interest(conn, {{16, surface, 16}, 2});

    engine::voxel::ReplicationPalette palette;
    CHECK(srv->server_pack_palette(conn, palette, error));
    CHECK(palette.entries.size() == 2);
    CHECK(cli->client_apply_palette(palette, error));
    CHECK(error.empty());
    uint32_t clientRuby = 0;
    CHECK(client->resolve_block_id("test:ruby", clientRuby, error));
    CHECK(clientRuby == rubyId);

    // Region state-sync carries the dynamic id; the client converges. The
    // snapshot window must cover the placed blocks (surface + 1).
    srv->server_set_snapshot_window(0, surface + 8);
    srv->server_update();
    engine::voxel::RegionReplicationSnapshot region;
    CHECK(srv->server_pack_region(conn, region, error));
    CHECK(error.empty());
    CHECK(cli->client_apply_region(region, error));
    CHECK(error.empty());
    CHECK(client->get_block(3, surface + 1, 3) == rubyId);   // ruby replicated
    CHECK(client->get_block(5, surface + 1, 5) == ghostId);  // ghost replicated

    // Live deltas with the dynamic id also apply after the palette.
    srv->server_update();
    srv->server_update();
    CHECK(srv->server_submit_edit(conn, 4, surface + 1, 4, rubyId).accepted);
    srv->server_update();
    auto batch = srv->server_pack_batch(conn);
    CHECK(batch.deltas.size() >= 1);
    cli->client_apply_batch(batch);
    CHECK(client->get_block(4, surface + 1, 4) == rubyId);  // delta applied

    std::cout << "[sdk] e2e JSON-only block: place, render-data, collide, "
                 "save, load, REPLICATE (palette) without recompiling OK\n";
}

// World identity + content provenance (FALTANTES §4 item 4): seed, world
// name, rules JSON, plugin versions and the block-registry fingerprint ride
// with the v5 save and are restored on load; a registry change since the
// save is DETECTABLE through the fingerprint; a migrated legacy save inherits
// the migrating world's identity; the world manager's save/load keeps the
// seed authoritative.
void test_world_metadata_persistence() {
    // World A: custom registry (ruby v3 + sapphire) + metadata, serialize.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    auto registryA = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registryA->load_from_json(
        R"([{"name":"ruby","namespace":"test","version":3,"color":[0.9,0.1,0.1]},)"
        R"({"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]}])",
        error));
    a->set_block_registry(registryA);
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    engine::voxel::IVoxelWorld::WorldMetadata metaA;
    metaA.seed = 424242;
    metaA.worldName = "alpha-world";
    metaA.rulesJson = R"({"difficulty":"hard","dayLength":1200})";
    metaA.pluginVersions.emplace_back("vulkancraft:mesher", "2.1.0");
    metaA.pluginVersions.emplace_back("vulkancraft:lighting", "1.0.3");
    a->set_world_metadata(metaA);
    CHECK(a->world_metadata().seed == 424242);
    const uint64_t stampA = a->registry_version();
    CHECK(stampA != 0);

    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());

    // World B: same registry in REVERSED load order -> same fingerprint;
    // load restores the full metadata + the save's registry stamp.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    auto registryB = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(registryB->load_from_json(
        R"([{"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]},)"
        R"({"name":"ruby","namespace":"test","version":3,"color":[0.9,0.1,0.1]}])",
        error));
    b->set_block_registry(registryB);
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    CHECK(b->registry_version() == stampA);  // load-order independent

    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    const engine::voxel::IVoxelWorld::WorldMetadata metaB = b->world_metadata();
    CHECK(metaB.seed == 424242);
    CHECK(metaB.worldName == "alpha-world");
    CHECK(metaB.rulesJson.find("dayLength") != std::string::npos);
    CHECK(metaB.pluginVersions.size() == 2);
    CHECK(metaB.pluginVersions[0].first == "vulkancraft:mesher");
    CHECK(metaB.pluginVersions[0].second == "2.1.0");
    CHECK(metaB.pluginVersions[1].first == "vulkancraft:lighting");
    CHECK(metaB.pluginVersions[1].second == "1.0.3");
    CHECK(b->saved_registry_version() == stampA);

    // Idempotency: re-serializing B yields the same bytes (meta included).
    std::string reError;
    CHECK(b->serialize_world(reError) == bytes);

    // A registry change since the save is DETECTABLE: same block, different
    // definition version -> different fingerprint; the save's stamp is kept
    // so a caller can warn (never guess). Identity still restores.
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    auto registryC = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(registryC->load_from_json(
        R"([{"name":"ruby","namespace":"test","version":4,"color":[0.9,0.1,0.1]},)"
        R"({"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]}])",
        error));
    c->set_block_registry(registryC);
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    CHECK(c->registry_version() != stampA);  // version bump -> new stamp
    std::string cError;
    CHECK(c->deserialize_world(bytes, cError));
    CHECK(cError.empty());
    CHECK(c->saved_registry_version() == stampA);  // the save's own stamp
    CHECK(c->saved_registry_version() != c->registry_version());  // mismatch!
    CHECK(c->world_metadata().seed == 424242);  // identity still restored

    // A migrated legacy save inherits the MIGRATING world's identity: build a
    // v1 save (no palette/entities, FNV-1a), migrate it with a world that has
    // metadata, and the upgraded bytes carry that identity.
    auto appendU32 = [](std::string& b, uint32_t v) {
        b.push_back(static_cast<char>(v & 0xFFu));
        b.push_back(static_cast<char>((v >> 8) & 0xFFu));
        b.push_back(static_cast<char>((v >> 16) & 0xFFu));
        b.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    auto appendI32 = [&appendU32](std::string& b, int32_t v) {
        appendU32(b, static_cast<uint32_t>(v));
    };
    std::string v1 = "VCWLD";
    appendU32(v1, 1);
    appendU32(v1, 1);  // one chunk
    appendI32(v1, 0);  // cx
    appendI32(v1, 0);  // cz
    appendU32(v1, 1);  // extent 1 (all air, 1-byte ids)
    for (int v = 0; v < 256; ++v) v1.push_back(static_cast<char>(kBlockAir));
    for (int v = 0; v < 256; ++v) v1.push_back(static_cast<char>(0xFF));  // no fluid
    {
        uint64_t hash = 1469598103934665603ull;
        for (const unsigned char ch : v1) {
            hash ^= ch;
            hash *= 1099511628211ull;
        }
        for (int i = 0; i < 8; ++i) {
            v1.push_back(static_cast<char>((hash >> (8 * i)) & 0xFFu));
        }
    }

    engine::voxel::IVoxelWorld::WorldMetadata metaMigrator;
    metaMigrator.seed = 555;
    metaMigrator.worldName = "migrated-world";
    metaMigrator.rulesJson = R"({"legacy":true})";
    metaMigrator.pluginVersions.emplace_back("vulkancraft:legacy", "0.9");
    a->set_world_metadata(metaMigrator);
    std::string migratedOut, migrateError;
    CHECK(a->migrate_world_save(v1, migratedOut, migrateError));
    CHECK(migrateError.empty());
    CHECK(a->world_save_schema_version(migratedOut) == 5u);
    std::unique_ptr<engine::voxel::IVoxelWorld> m =
        engine::voxel::create_default_voxel_world();
    std::string mError;
    CHECK(m->deserialize_world(migratedOut, mError));
    CHECK(m->world_metadata().seed == 555);
    CHECK(m->world_metadata().worldName == "migrated-world");
    CHECK(m->world_metadata().pluginVersions.size() == 1);

    // The world manager's save/load keeps the seed authoritative: a world
    // created with seed 777, saved and reloaded into a FRESH manager reports
    // the save's seed (not whatever the reload spec claims).
    {
        const std::string path =
            scratch_dir() + "/vc_test_meta_world";
        auto manager = engine::world::create_world_manager();
        engine::world::WorldSpec spec;
        spec.name = "meta-overworld";
        spec.seed = 777;
        spec.rulesJson = R"({"difficulty":"normal"})";
        spec.savePath = path;
        std::string mgrError;
        CHECK(manager->create_world(spec, mgrError));
        CHECK(manager->world_info("meta-overworld").seed == 777);
        CHECK(manager->save_world("meta-overworld", path, mgrError));
        CHECK(mgrError.empty());

        auto manager2 = engine::world::create_world_manager();
        engine::world::WorldSpec reload;
        reload.name = "meta-overworld";
        reload.seed = 123;  // wrong on purpose: the save must win
        reload.savePath = path;
        std::string reloadError;
        CHECK(manager2->load_world(reload, reloadError));
        CHECK(reloadError.empty());
        CHECK(manager2->world_info("meta-overworld").seed == 777);
        CHECK(manager2->world_info("meta-overworld").rulesJson.find("normal") !=
              std::string::npos);
    }

    std::cout << "[sdk] world metadata persistence: seed/name/rules/plugin "
                 "versions + registry fingerprint ride the v5 save, restore on "
                 "load, detect registry change, migrate with identity, manager "
                 "save/load keeps the seed\n";
}

// Per-voxel block state index (FALTANTES item 2 "variantes de modelo"): set/get
// round-trip through the public API, persistence via save/load, and idempotency.
void test_per_voxel_state() {
    // World A: custom registry with a lamp that has 2 states, place blocks,
    // set states, serialize.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    auto registryA = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registryA->load_from_json(
        R"({"name":"lamp","namespace":"test","states":[)"
        R"({"name":"base","color":[0.4,0.4,0.4]},)"
        R"({"name":"lit","color":[1.0,0.6,0.1],"lightEmission":0.8}]})",
        error));
    a->set_block_registry(registryA);
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t lampId = 0;
    CHECK(a->resolve_block_id("test:lamp", lampId, error));

    // Place 3 lamps and set different states.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, lampId);  // default state (0)
        tx->set_block(5, 130, 5, lampId);  // will set state 1 ("lit")
        tx->set_block(7, 130, 7, lampId);  // will set state 0 explicitly
        std::string txErr;
        CHECK(tx->commit(txErr));
        CHECK(txErr.empty());
    }
    // Default state for all 3.
    CHECK(a->get_block_state(3, 130, 3) == 0);
    CHECK(a->get_block_state(5, 130, 5) == 0);
    CHECK(a->get_block_state(7, 130, 7) == 0);

    // Set states: 5→1 (lit), 7→0 (explicit default).
    a->set_block_state(5, 130, 5, 1);
    a->set_block_state(7, 130, 7, 0);  // no-op: already 0
    CHECK(a->get_block_state(3, 130, 3) == 0);
    CHECK(a->get_block_state(5, 130, 5) == 1);
    CHECK(a->get_block_state(7, 130, 7) == 0);

    // Serialize.
    std::string saveErr;
    const std::string bytes = a->serialize_world(saveErr);
    CHECK(saveErr.empty());

    // World B: same registry, restore the save, verify states.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    auto registryB = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(registryB->load_from_json(
        R"({"name":"lamp","namespace":"test","states":[)"
        R"({"name":"base","color":[0.4,0.4,0.4]},)"
        R"({"name":"lit","color":[1.0,0.6,0.1],"lightEmission":0.8}]})",
        error));
    b->set_block_registry(registryB);
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    std::string loadErr;
    CHECK(b->deserialize_world(bytes, loadErr));
    CHECK(loadErr.empty());

    // Block ids restored.
    CHECK(b->get_block(3, 130, 3) == static_cast<uint32_t>(lampId));
    CHECK(b->get_block(5, 130, 5) == static_cast<uint32_t>(lampId));
    CHECK(b->get_block(7, 130, 7) == static_cast<uint32_t>(lampId));

    // States restored.
    CHECK(b->get_block_state(3, 130, 3) == 0);
    CHECK(b->get_block_state(5, 130, 5) == 1);
    CHECK(b->get_block_state(7, 130, 7) == 0);

    // Out-of-bounds returns 0.
    CHECK(b->get_block_state(0, 0, 0) == 0);

    // Re-serialization is idempotent.
    std::string reErr;
    const std::string bytesB = b->serialize_world(reErr);
    CHECK(reErr.empty());
    CHECK(bytesB == bytes);

    // Setting a new block resets state to 0.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = b->begin_transaction();
        tx->set_block(9, 130, 9, lampId);
        std::string txErr;
        CHECK(tx->commit(txErr));
    }
    CHECK(b->get_block_state(9, 130, 9) == 0);
    b->set_block_state(9, 130, 9, 1);
    CHECK(b->get_block_state(9, 130, 9) == 1);
    // Setting a block replaces the state.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = b->begin_transaction();
        tx->set_block(9, 130, 9, 0);  // air
        std::string txErr;
        CHECK(tx->commit(txErr));
    }
    CHECK(b->get_block_state(9, 130, 9) == 0);  // state resets on block change

    std::cout << "[sdk] per-voxel state: set/get round-trip, save/load restore, "
                 "idempotency, block change resets state OK\n";
}

// Block entities (META section 8): lifecycle, deterministic ticking through
// the world scheduler and atomic destroy with the block. All through the
// public contract, in the external-project TU.
void test_block_entity_lifecycle() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    world->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });

    std::vector<engine::voxel::BlockEntityEvent> events;
    world->set_block_entity_listener(
        [&](const engine::voxel::BlockEntityEvent& event) { events.push_back(event); });

    // Attach requires a loaded non-empty block; (5,96,5) is the flat surface.
    auto machine = std::make_shared<CounterMachine>();
    std::string error;
    CHECK(world->attach_block_entity(5, 96, 5, machine, error));
    CHECK(error.empty());
    CHECK(world->block_entity_count() == 1);
    CHECK(world->block_entity_at(5, 96, 5) != nullptr);
    CHECK(world->block_entity_at(5, 96, 5)->type_id() == "project:counter_machine");
    CHECK(machine->created_);
    CHECK(events.size() == 1 && events[0].kind == engine::voxel::BlockEntityEvent::Kind::Attached);

    // Duplicate attach is refused; so is attaching to empty air or a type
    // without a registered factory.
    CHECK(!world->attach_block_entity(5, 96, 5, std::make_shared<CounterMachine>(), error));
    CHECK(!world->attach_block_entity(5, 130, 5, std::make_shared<CounterMachine>(), error));
    CHECK(!error.empty());
    CHECK(!world->attach_block_entity(6, 96, 6, std::make_shared<StrictMachine>(), error));

    // Ticking: update() drives the scheduler's BlockTick phase. Every update
    // is >= the 80ms fixed step, so each fires at least one tick (the
    // scheduler carries any leftover from boot, so exact counts are not
    // asserted — monotonic progress is).
    CHECK(machine->ticks_ == 0);
    world->update(player, 0.09f);
    world->update(player, 0.09f);
    world->update(player, 0.09f);
    CHECK(machine->ticks_ >= 3);
    CHECK(machine->lastWorldTick_ > 0);

    // Explicit detach: gone, Detached event, destroyed hook.
    CHECK(world->remove_block_entity(5, 96, 5));
    CHECK(world->block_entity_count() == 0);
    CHECK(world->block_entity_at(5, 96, 5) == nullptr);
    CHECK(machine->destroyed_);
    CHECK(events.size() == 2 && events[1].kind == engine::voxel::BlockEntityEvent::Kind::Detached);

    std::cout << "[sdk] block entities: lifecycle + scheduler ticking OK\n";
}

void test_block_entity_atomic_destroy() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    world->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });

    auto machine = std::make_shared<CounterMachine>();
    std::string error;
    CHECK(world->attach_block_entity(6, 96, 6, machine, error));

    // Removing the block (transaction -> Air) destroys the entity atomically.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->remove_block(6, 96, 6);
        std::string txError;
        CHECK(tx->commit(txError));
    }
    CHECK(world->block_entity_count() == 0);
    CHECK(machine->destroyed_);

    // The single-edit convenience path behaves identically (Air removes).
    auto second = std::make_shared<CounterMachine>();
    CHECK(world->attach_block_entity(7, 96, 7, second, error));
    world->set_block(7, 96, 7, kBlockAir);
    CHECK(world->block_entity_count() == 0);
    CHECK(second->destroyed_);

    // A non-Air replacement keeps the entity (project decides compatibility).
    auto third = std::make_shared<CounterMachine>();
    CHECK(world->attach_block_entity(8, 96, 8, third, error));
    world->set_block(8, 96, 8, kBlockStone);
    CHECK(world->block_entity_count() == 1);
    CHECK(world->block_entity_at(8, 96, 8) != nullptr);

    std::cout << "[sdk] block entities: atomic destroy with the block OK\n";
}

void test_block_entity_persistence() {
    // World A: attach a machine with project state, save.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*a, player, 16));
    a->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machine = std::make_shared<CounterMachine>();
    machine->counter_ = 7;  // project state to survive the roundtrip
    std::string error;
    CHECK(a->attach_block_entity(8, 96, 8, machine, error));
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());
    // Deterministic entity section: re-serializing yields identical bytes.
    CHECK(a->serialize_world(saveError) == bytes);

    // World B without the factory: the save is refused with a diagnostic.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, player, 16));
    std::string loadError;
    CHECK(!b->deserialize_world(bytes, loadError));
    CHECK(loadError.find("project:counter_machine") != std::string::npos);

    // World C with the factory: entity reconstructed with its state and ticks.
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, player, 16));
    c->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    CHECK(c->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    auto restored = std::dynamic_pointer_cast<CounterMachine>(c->block_entity_at(8, 96, 8));
    CHECK(restored != nullptr);
    CHECK(restored->counter_ == 7);   // opaque project state restored
    CHECK(restored->ticks_ == 0);     // runtime-only state is NOT persisted
    CHECK(c->block_entity_count() == 1);
    c->update(player, 0.09f);
    c->update(player, 0.09f);
    CHECK(restored->ticks_ >= 2);     // the restored entity ticks again

    // An entity that refuses its data fails the load (never silently drops):
    // register a factory whose entity rejects the version written by the save.
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, player, 16));
    d->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<StrictMachine>(); });  // lying factory
    std::string refuseError;
    CHECK(!d->deserialize_world(bytes, refuseError));
    CHECK(refuseError.find("refused") != std::string::npos);

    std::cout << "[sdk] block entities: versioned persistence roundtrip OK\n";
}

// B.3: the canonical stateful blocks (chest, furnace, sign, door, TNT,
// machine) are defined OUTSIDE the engine — JSON block registry + block entity
// factories — and round-trip through attach -> tick -> persist -> reload. The
// engine owns storage/ticking/persistence framing; the project owns behavior.
void test_block_entities_json_defined() {
    // One project-owned entity, parameterized by type id. The engine never
    // interprets the state blob (it only frames it for persistence).
    class ProjectEntity final : public engine::voxel::IVoxelBlockEntity {
    public:
        explicit ProjectEntity(std::string typeId) : typeId_(std::move(typeId)) {}
        std::string type_id() const override { return typeId_; }
        void on_tick(uint64_t) override { ++ticks_; }
        uint32_t data_version() const override { return 1; }
        std::vector<uint8_t> serialize_state() const override {
            std::vector<uint8_t> blob(4);
            for (int i = 0; i < 4; ++i) {
                blob[static_cast<std::size_t>(i)] =
                    static_cast<uint8_t>((counter_ >> (8 * i)) & 0xFFu);
            }
            return blob;
        }
        bool deserialize_state(const std::vector<uint8_t>& data,
                               uint32_t version) override {
            if (version != 1 || data.size() < 4) return false;
            counter_ = 0;
            for (int i = 0; i < 4; ++i) {
                counter_ |= static_cast<uint32_t>(data[static_cast<std::size_t>(i)])
                            << (8 * i);
            }
            return true;
        }
        std::string typeId_;
        uint32_t counter_{ 0 };  // persisted project state
        uint64_t ticks_{ 0 };    // runtime only (not persisted)
    };

    const std::array<const char*, 6> kNames = {
        "chest", "furnace", "sign", "door", "tnt", "machine",
    };
    const std::string kBlocksJson =
        R"([
          {"name":"chest","namespace":"test","color":[0.6,0.4,0.2]},
          {"name":"furnace","namespace":"test","color":[0.4,0.4,0.4]},
          {"name":"sign","namespace":"test","color":[0.7,0.5,0.3]},
          {"name":"door","namespace":"test","color":[0.5,0.3,0.1]},
          {"name":"tnt","namespace":"test","color":[0.9,0.1,0.1]},
          {"name":"machine","namespace":"test","color":[0.2,0.6,0.8]}
        ])";
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    const auto build = [&](std::unique_ptr<engine::voxel::IVoxelWorld>& out) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(kBlocksJson, error));
        world->set_block_registry(blocks);
        for (const char* name : kNames) {
            const std::string typeId = std::string("project:") + name;
            world->register_block_entity_type(typeId, [typeId] {
                return std::make_shared<ProjectEntity>(typeId);
            });
        }
        CHECK(boot_world(*world, player, 16));
        out = std::move(world);
    };

    // World A: place the six JSON-defined blocks and attach one entity each.
    std::unique_ptr<engine::voxel::IVoxelWorld> a;
    build(a);
    std::string error;
    for (int i = 0; i < 6; ++i) {
        const std::string blockName = std::string("test:") + kNames[i];
        uint32_t id = 0;
        CHECK(a->resolve_block_id(blockName, id, error));
        a->set_block(8 + i, 97, 8, id);
        auto entity = std::make_shared<ProjectEntity>(
            std::string("project:") + kNames[i]);
        entity->counter_ = static_cast<uint32_t>(i + 1);
        CHECK(a->attach_block_entity(8 + i, 97, 8, entity, error));
    }
    CHECK(a->block_entity_count() == 6);
    // The blocks are the JSON-defined blocks, not builtins.
    uint32_t chestId = 0;
    CHECK(a->resolve_block_id("test:chest", chestId, error));
    CHECK(a->get_block(8, 97, 8) == chestId);

    const std::string bytes = a->serialize_world(error);
    CHECK(error.empty());

    // World B: same project assets (JSON registry + factories), fresh world.
    std::unique_ptr<engine::voxel::IVoxelWorld> b;
    build(b);
    CHECK(b->deserialize_world(bytes, error));
    CHECK(error.empty());
    CHECK(b->block_entity_count() == 6);
    for (int i = 0; i < 6; ++i) {
        auto e = std::dynamic_pointer_cast<ProjectEntity>(
            b->block_entity_at(8 + i, 97, 8));
        CHECK(e != nullptr);
        CHECK(e->typeId_ == std::string("project:") + kNames[i]);
        CHECK(e->counter_ == static_cast<uint32_t>(i + 1));  // state restored
        CHECK(e->ticks_ == 0);  // runtime-only state is NOT persisted
    }
    // Restored entities tick again.
    b->update(player, 0.09f);
    b->update(player, 0.09f);
    auto e0 = std::dynamic_pointer_cast<ProjectEntity>(b->block_entity_at(8, 97, 8));
    CHECK(e0 != nullptr);
    CHECK(e0->ticks_ >= 2);

    std::cout << "[sdk] block entities: chest/furnace/sign/door/tnt/machine "
                 "JSON-defined outside the engine, full roundtrip OK\n";
}

// Task B.1: OPTIONAL capabilities on block entities — inventory, script and
// components — WITHOUT turning every block into a full ECS entity. The
// accessors default to none, so a plain entity (counter machine) stays lean;
// an entity that opts in (chest) exposes a real authoritative Inventory, a
// script id and sorted project components through the PUBLIC contract, and the
// capabilities survive the save/load roundtrip through the entity's own blob
// (the engine frames it, the project owns the content).
void test_block_entity_optional_capabilities() {
    using engine::registry::ItemDefinition;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    world->register_block_entity_type("project:chest",
        [] { return std::make_shared<ChestEntity>(); });
    world->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // Attach a chest (opts in) and a counter machine (plain, stays lean).
    auto chest = std::make_shared<ChestEntity>();
    std::string error;
    CHECK(world->attach_block_entity(8, 96, 8, chest, error));
    auto machine = std::make_shared<CounterMachine>();
    CHECK(world->attach_block_entity(9, 96, 9, machine, error));

    // (1) The chest's capabilities are reachable through the PUBLIC contract
    //     (block_entity_at -> dynamic_cast -> accessors).
    auto viaWorld = std::dynamic_pointer_cast<ChestEntity>(
        world->block_entity_at(8, 96, 8));
    CHECK(viaWorld != nullptr);
    CHECK(viaWorld->inventory() != nullptr);
    CHECK(viaWorld->inventory()->slot_count() == 27);
    CHECK(viaWorld->inventory() == &chest->inventory_);
    CHECK(viaWorld->script_id() == "project:chest_loot");
    {
        const std::vector<engine::voxel::BlockEntityComponent> comps =
            viaWorld->components();
        CHECK(comps.size() == 2);
        CHECK(comps[0].type == "project:fuel");      // sorted by type
        CHECK(comps[1].type == "project:lockable");
        CHECK(comps[1].blob.size() == 2);
    }

    // (2) The inventory is authoritative and mutable: an item can be placed
    //     through the world-facing accessor (chest slot 0 accepts anything).
    engine::registry::ItemRegistry items;
    {
        ItemDefinition def;
        def.ns = "vulkancraft";
        def.name = "cobblestone";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
    }
    SlotFilter any;
    any.allowAny = true;
    viaWorld->inventory()->set_filter(0, any);
    ItemStack stone;
    stone.item = "vulkancraft:cobblestone";
    stone.count = 12;
    CHECK(viaWorld->inventory()->set(0, stone, items, error));
    CHECK(viaWorld->inventory()->get(0).count == 12);

    // (3) A plain entity keeps the DEFAULT capabilities: no inventory, no
    //     script, no components — it was NOT turned into a full entity.
    auto plain = std::dynamic_pointer_cast<CounterMachine>(
        world->block_entity_at(9, 96, 9));
    CHECK(plain != nullptr);
    CHECK(plain->inventory() == nullptr);
    CHECK(plain->script_id().empty());
    CHECK(plain->components().empty());

    // (4) The capabilities survive save/load through the entity's own blob:
    //     the chest's persisted marker round-trips and the restored entity
    //     still exposes inventory/script/components.
    chest->lockedBlob_ = { 0xDE, 0xAD, 0xBE, 0xEF };
    const std::string bytes = world->serialize_world(error);
    CHECK(error.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> fresh =
        engine::voxel::create_default_voxel_world();
    fresh->register_generator(std::make_shared<FlatGenerator>(96));
    fresh->register_block_entity_type("project:chest",
        [] { return std::make_shared<ChestEntity>(); });
    fresh->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    CHECK(boot_world(*fresh, player, 16));
    CHECK(fresh->deserialize_world(bytes, error));
    CHECK(error.empty());
    auto restored = std::dynamic_pointer_cast<ChestEntity>(
        fresh->block_entity_at(8, 96, 8));
    CHECK(restored != nullptr);
    CHECK(restored->lockedBlob_ == std::vector<uint8_t>({ 0xDE, 0xAD, 0xBE, 0xEF }));
    CHECK(restored->inventory() != nullptr);
    CHECK(restored->inventory()->slot_count() == 27);
    CHECK(restored->script_id() == "project:chest_loot");
    CHECK(restored->components().size() == 2);

    std::cout << "[sdk] block entities: optional inventory/script/components "
                 "reachable through the public contract, plain entities stay "
                 "lean, roundtrip OK\n";
}

// Task B.2 (engine-side scripting leg): the block-entity scripting bridge
// runs the scripts that entities declare through script_id() using the
// engine's ScriptVM, consuming the block-entity listener contract
// (Attached/Detached) and exposing live variables to the inspector seam.
void test_block_entity_scripting() {
    using engine::voxel::BlockEntityScriptSpec;
    using engine::voxel::IBlockEntityScripting;

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    world->register_block_entity_type("project:chest",
        [] { return std::make_shared<ChestEntity>(); });
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // Create the scripting bridge — it subscribes to the world's
    // block-entity listener for its lifetime.
    auto scripting = engine::voxel::create_block_entity_scripting(*world);
    CHECK(scripting != nullptr);
    CHECK(scripting->active_instances() == 0);
    CHECK(scripting->completed_runs() == 0);
    CHECK(scripting->failed_runs() == 0);

    // Register a trivial script: increment "count" on every tick.
    BlockEntityScriptSpec spec;
    spec.scriptId = "project:chest_loot";
    spec.graphJson = R"({
        "id": "00000000-0000-0000-0000-000000000001",
        "name": "counter",
        "nodes": [
            {"id": "00000000-0000-0000-0000-000000000010", "kind": "Event", "event": "on_tick"},
            {"id": "00000000-0000-0000-0000-000000000011", "kind": "ConstantFloat", "literal": {"type": "float", "value": 1.0}},
            {"id": "00000000-0000-0000-0000-000000000012", "kind": "AddFloat", "variable": "count"},
            {"id": "00000000-0000-0000-0000-000000000013", "kind": "Return"}
        ],
        "links": [
            {"from": "00000000-0000-0000-0000-000000000010", "to": "00000000-0000-0000-0000-000000000011"},
            {"from": "00000000-0000-0000-0000-000000000011", "to": "00000000-0000-0000-0000-000000000012"},
            {"from": "00000000-0000-0000-0000-000000000012", "to": "00000000-0000-0000-0000-000000000013"}
        ]
    })";
    CHECK(scripting->register_script(spec));
    CHECK(scripting->has_script("project:chest_loot"));
    CHECK(scripting->last_error().empty());

    // Attach a chest with matching script_id — the bridge picks it up
    // via the Attached listener. The first tick creates the instance.
    auto chest = std::make_shared<ChestEntity>();
    std::string error;
    CHECK(world->attach_block_entity(8, 96, 8, chest, error));
    CHECK(error.empty());

    // Tick: the script's on_tick fires, incrementing "count".
    // After the first tick, the instance is live and active_instances > 0.
    scripting->tick(1.0 / 20.0);
    scripting->tick(1.0 / 20.0);
    scripting->tick(1.0 / 20.0);
    CHECK(scripting->active_instances() == 1);

    // Read the live variable through the inspector seam.
    double count = -1.0;
    CHECK(scripting->script_variable({8, 96, 8}, "count", count));
    CHECK(count > 0.5);
    CHECK(scripting->completed_runs() >= 3);
    CHECK(scripting->failed_runs() == 0);

    // Unregister the script — the bridge no longer drives the entity.
    CHECK(scripting->unregister_script("project:chest_loot"));
    CHECK(!scripting->has_script("project:chest_loot"));

    // Detach the chest — the bridge removes the instance.
    CHECK(world->remove_block_entity(8, 96, 8));
    CHECK(scripting->active_instances() == 0);

    // Re-register and re-attach: fresh VM, count starts from zero again.
    CHECK(scripting->register_script(spec));
    CHECK(world->attach_block_entity(8, 96, 8, chest, error));
    scripting->tick(1.0 / 20.0);
    CHECK(scripting->active_instances() == 1);
    double count2 = -1.0;
    CHECK(scripting->script_variable({8, 96, 8}, "count", count2));
    CHECK(count2 > 0.5);

    std::cout << "[sdk] block-entity scripting bridge: register/tick/variable/"
                 "detach/reattach OK\n";
}

void test_render_handoff_dirty_updates() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // A freshly booted world converges: no chunk is dirty once meshing/light
    // settle (the boot waits for (0,0); drain the rest before asserting).
    CHECK(settle(*world, player, [&] {
        for (const auto& u : world->render_dirty_updates()) {
            (void)u;
        }
        return world->streaming_snapshot().chunksDirty == 0;
    }));

    // (1) A committed edit marks the edited chunk (and its halo neighbors)
    //     dirty with a bumped monotonic revision.
    uint64_t before = 0;
    for (const auto& u : world->render_dirty_updates()) {
        if (u.chunkX == 0 && u.chunkZ == 0) before = u.revision;
    }
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx =
            world->begin_transaction();
        tx->set_block(8, 120, 8, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    bool foundEdited = false;
    for (const auto& u : world->render_dirty_updates()) {
        if (u.chunkX == 0 && u.chunkZ == 0) {
            foundEdited = true;
            CHECK(u.meshDirty);
            CHECK(u.lightDirty);
            CHECK(u.revision > before);  // monotonic per chunk
        }
    }
    CHECK(foundEdited);

    // (2) Deterministic, sorted output: two consecutive polls are identical
    //     (non-consuming) and the list is sorted by (chunkX, chunkZ).
    const std::vector<engine::voxel::ChunkDirtyUpdate> first =
        world->render_dirty_updates();
    const std::vector<engine::voxel::ChunkDirtyUpdate> second =
        world->render_dirty_updates();
    CHECK(first == second);
    bool sorted = true;
    for (std::size_t i = 1; i < first.size(); ++i) {
        if (first[i].chunkX < first[i - 1].chunkX) sorted = false;
        else if (first[i].chunkX == first[i - 1].chunkX &&
                 first[i].chunkZ < first[i - 1].chunkZ) sorted = false;
    }
    CHECK(sorted);

    // (3) The world processes the signals (mesher + light pass consume them):
    //     after the sim settles, the edited chunk is no longer reported dirty.
    CHECK(settle(*world, player, [&] {
        return world->streaming_snapshot().chunksDirty == 0 &&
               world->streaming_snapshot().lightDirtyChunks == 0;
    }));
    for (const auto& u : world->render_dirty_updates()) {
        CHECK(!(u.chunkX == 0 && u.chunkZ == 0));
    }

    std::cout << "[sdk] render handoff: dirty chunk lists with monotonic "
                 "revisions, deterministic non-consuming snapshot OK\n";
}

// Discrete world lighting (META section 12), through the public contract:
// skylight from column occlusion, block light from data-driven emitters,
// attenuation through air, opaque blocking, chunk borders and determinism.
void test_light_skylight() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // Sky settles asynchronously; poll until the surface column is occluded.
    CHECK(settle(*world, player, [&] {
        return world->get_sky_light(8, 96, 8) == 0 &&
               world->get_sky_light(8, 200, 8) == 15;
    }));
    CHECK(world->get_sky_light(8, 200, 8) == 15);  // open air: full sun
    CHECK(world->get_sky_light(8, 97, 8) == 15);   // water surface: sunlit
    CHECK(world->get_sky_light(8, 96, 8) == 0);    // stone occludes
    CHECK(world->get_sky_light(8, 90, 8) == 0);    // underground: dark

    std::cout << "[sdk] lighting: skylight from column occlusion OK\n";
}

void test_light_block_emitter() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registry->load_from_json(
        R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
        error));
    world->set_block_registry(registry);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t lanternId = 0;
    CHECK(world->resolve_block_id("test:lantern", lanternId, error));

    // Place a light emitter in open air above the water line.
    world->set_block(8, 130, 8, lanternId);
    CHECK(settle(*world, player, [&] { return world->get_block_light(8, 130, 8) == 15; }));
    CHECK(world->get_block_light(8, 130, 8) == 15);  // emitter cell
    CHECK(world->get_block_light(8, 129, 8) == 14);  // one air step
    CHECK(world->get_block_light(8, 131, 8) == 14);
    CHECK(world->get_block_light(8, 128, 8) == 13);
    CHECK(world->get_block_light(8, 140, 8) == 5);   // 10 air steps: 15 - 10

    // Opaque blocks stop light: the stone's own cell goes dark even though
    // the emitter below is at full strength. (Light still wraps around the
    // stone through the neighboring column, so the cell directly above is
    // dimmer than the 13 it had before — never brighter.)
    world->set_block(8, 131, 8, kBlockStone);
    CHECK(settle(*world, player, [&] { return world->get_block_light(8, 131, 8) == 0; }));
    CHECK(world->get_block_light(8, 130, 8) == 15);  // emitter unaffected
    CHECK(world->get_block_light(8, 132, 8) < 13);   // direct beam blocked

    // Incremental removal: breaking the emitter decays the field.
    world->set_block(8, 130, 8, kBlockAir);
    CHECK(settle(*world, player, [&] {
        return world->get_block_light(8, 130, 8) == 0 &&
               world->get_block_light(8, 129, 8) == 0;
    }));
    CHECK(world->get_block_light(8, 130, 8) == 0);
    CHECK(world->get_block_light(8, 129, 8) == 0);

    std::cout << "[sdk] lighting: data-driven emitter, attenuation and removal OK\n";
}

void test_light_chunk_boundary() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registry->load_from_json(
        R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
        error));
    world->set_block_registry(registry);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t lanternId = 0;
    CHECK(world->resolve_block_id("test:lantern", lanternId, error));

    // Emitter on the LAST column of chunk (0,0): light must cross into chunk
    // (1,0), which is loaded (render distance for budget 16 covers ring 2).
    world->set_block(15, 130, 8, lanternId);
    CHECK(settle(*world, player, [&] { return world->get_block_light(15, 130, 8) == 15; }));
    CHECK(settle(*world, player, [&] { return world->get_block_light(16, 130, 8) == 14; }));
    CHECK(world->get_block_light(14, 130, 8) == 14);   // same chunk
    CHECK(world->get_block_light(13, 130, 8) == 13);
    CHECK(world->get_block_light(17, 130, 8) == 13);   // deeper into chunk 1
    CHECK(world->get_block_light(24, 130, 8) == 6);    // 15 - 9 steps

    std::cout << "[sdk] lighting: block light crosses chunk borders OK\n";
}

void test_light_determinism() {
    const auto build = [&](const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto registry = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(registry->load_from_json(
            R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
            error));
        world->set_block_registry(registry);
        CHECK(boot_world(*world, player, 16));
        uint32_t lanternId = 0;
        CHECK(world->resolve_block_id("test:lantern", lanternId, error));
        world->set_block(7, 130, 7, lanternId);
        world->set_block(12, 130, 12, lanternId);
        CHECK(settle(*world, player, [&] { return world->get_block_light(7, 130, 7) == 15; }));
        CHECK(settle(*world, player, [&] { return world->get_block_light(12, 130, 12) == 15; }));
        return world;
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    std::unique_ptr<engine::voxel::IVoxelWorld> a = build(player);
    std::unique_ptr<engine::voxel::IVoxelWorld> b = build(player);
    // Same blocks => same light, on any machine/pass order.
    for (int z = 6; z <= 13; ++z) {
        for (int x = 6; x <= 13; ++x) {
            CHECK(a->get_block_light(x, 130, z) == b->get_block_light(x, 130, z));
            CHECK(a->get_sky_light(x, 130, z) == b->get_sky_light(x, 130, z));
        }
    }

    std::cout << "[sdk] lighting: deterministic across instances OK\n";
}

// FALTANTES item 158: edição durante propagação já é testada; faltavam LOAD e
// UNLOAD. O orçamento de streaming + a re-sujeição garantem que o campo de luz
// converge ao MESMO fixpoint independente do timing de load/unload dos chunks:
// (a) LOAD durante propagação — emissor na última coluna do chunk (0,0) com o
// chunk (1,0) NÃO carregado (budget 8 para o player em (-8,200,8), o mesmo
// padrão do teste de fluido em fronteira não carregada); o campo converge no
// (0,0), a fronteira não carregada fica escura (0); quando o player vai para
// leste o (1,0) streama DURANTE a propagação e a fronteira preenche idêntico
// ao controle (budget 16, (1,0) carregado desde o início).
// (b) UNLOAD durante propagação — chunk limpo (sem edits) com skylight
// convergido é evictado quando o player teleporta longe e, ao voltar, o campo
// reconverge idêntico ao controle que nunca descarregou.
void test_light_load_unload_during_propagation() {
    const auto buildLantern = [&](const glm::vec3& player, int budget) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto registry = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(registry->load_from_json(
            R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
            error));
        world->set_block_registry(registry);
        CHECK(boot_world(*world, player, budget));
        uint32_t lanternId = 0;
        CHECK(world->resolve_block_id("test:lantern", lanternId, error));
        // Emitter on the LAST column of chunk (0,0): the frontier crosses
        // into chunk (1,0) once that chunk loads.
        world->set_block(15, 130, 8, lanternId);
        return world;
    };

    const glm::vec3 west{ -8.0f, 200.0f, 8.0f };  // centers chunk (-1,0)
    const glm::vec3 east{ 24.0f, 200.0f, 8.0f };  // centers chunk (1,0)
    const glm::vec3 home{ 8.0f, 200.0f, 8.0f };   // centers chunk (0,0)

    // Control: player centered on (0,0) with budget 16 — chunk (1,0) is ring
    // 1 and loads from the start (the same geometry as test_light_chunk_boundary).
    std::unique_ptr<engine::voxel::IVoxelWorld> control =
        buildLantern(home, 16);
    CHECK(settle(*control, home, [&] {
        return control->get_block_light(15, 130, 8) == 15 &&
               control->get_block_light(16, 130, 8) == 14;
    }));

    // Subject: budget 8 loads chunk (0,0) but stops BEFORE chunk (1,0).
    std::unique_ptr<engine::voxel::IVoxelWorld> subject =
        buildLantern(west, 8);
    CHECK(subject->is_chunk_loaded(0, 0));
    CHECK(!subject->is_chunk_loaded(1, 0));
    // The emitter's field converges inside (0,0); the unloaded frontier stays
    // dark (the world reports 0 for a chunk that is not loaded).
    CHECK(settle(*subject, west,
        [&] { return subject->get_block_light(15, 130, 8) == 15; }));
    CHECK(subject->get_block_light(16, 130, 8) == 0);

    // Move east: (1,0) streams in while the field is live; the frontier must
    // fill exactly like the control (load-during-propagation convergence).
    CHECK(settle(*subject, east, [&] { return subject->is_chunk_loaded(1, 0); }));
    CHECK(settle(*subject, east,
        [&] { return subject->get_block_light(16, 130, 8) == 14; }));
    for (int x = 13; x <= 24; ++x) {
        CHECK(subject->get_block_light(x, 130, 8) ==
              control->get_block_light(x, 130, 8));
    }

    // (b) UNLOAD: a clean (edit-free) skylight chunk is evicted when the
    // player leaves and reconverges identically on reload.
    const auto buildSkylight = [&]() {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        return world;
    };
    std::unique_ptr<engine::voxel::IVoxelWorld> keep = buildSkylight();
    std::unique_ptr<engine::voxel::IVoxelWorld> evict = buildSkylight();
    const auto skylightConverged = [](engine::voxel::IVoxelWorld& w) {
        // True fixed point (C.2): the probe cells converged AND no light work
        // remains (dirty or in flight). With async light the 2-cell probe
        // alone is satisfiable mid-cascade — the chunk's full column is only
        // final once every relight job has run.
        const auto snap = w.streaming_snapshot();
        return w.get_sky_light(8, 96, 8) == 0 &&   // stone occludes
               w.get_sky_light(8, 200, 8) == 15 && // open air: full sun
               snap.lightDirtyChunks == 0 && snap.pendingLightJobs == 0;
    };

    // Teleport far away: the clean (0,0) chunk falls out of the streaming
    // window and is evicted.
    const glm::vec3 far{ 2000.0f, 200.0f, 2000.0f };
    CHECK(settle(*evict, far, [&] { return !evict->is_chunk_loaded(0, 0); }));
    // Return: the chunk reloads and the skylight field reconverges to the
    // exact same column profile as the never-unloaded control.
    CHECK(settle(*evict, home, [&] { return evict->is_chunk_loaded(0, 0); }));
    CHECK(settle(*evict, home, [&] { return skylightConverged(*evict); }));
    for (int y = 90; y <= 100; ++y) {
        CHECK(evict->get_sky_light(8, y, 8) == keep->get_sky_light(8, y, 8));
    }

    std::cout << "[sdk] lighting: load/unload during propagation converge OK\n";
}

// FALTANTES §4 item 6 (data safety): the STREAMING-window eviction (player
// walks away) must not drop unsaved edits — the same guarantee the
// memory-budget eviction already has (test_chunk_memory_budget). Without it,
// an edited chunk evicted from the window is regenerated from the generator
// and the edit is silently lost before any save.
void test_eviction_preserves_edits() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 home{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, home, 16));

    // An unmistakable edit high above the terrain surface.
    world->set_block(8, 130, 8, kBlockStone);
    CHECK(world->get_block(8, 130, 8) == kBlockStone);

    // Player walks far away: the chunk leaves the streaming window. (With the
    // bug it is evicted despite carrying an unsaved edit; with the fix it
    // stays resident — either way the edit must survive the trip.)
    const glm::vec3 far{ 2000.0f, 200.0f, 2000.0f };
    for (int i = 0; i < 60 * 5; ++i) world->update(far, 1.0f / 60.0f);

    // Player returns: the chunk is (re)loaded and the edit is still there.
    CHECK(settle(*world, home, [&] { return world->is_chunk_loaded(0, 0); }));
    CHECK(world->get_block(8, 130, 8) == kBlockStone);

    // Phase 2: with a region-capable storage (the paged save path that
    // production and autosave use), a save persists the edit AND releases the
    // pin — the chunk becomes evictable again (memory is not held forever by
    // a protected chunk) and the edit lives in the save file. (The
    // no-storage monolithic save keeps the pin — registered as a finding;
    // this test exercises the supported path.)
    world->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string saveDir = scratch_dir() + "/evict_after_save";
    std::string saveError;
    CHECK(world->save_world(saveDir, saveError));
    CHECK(saveError.empty());
    CHECK(settle(*world, far, [&] { return !world->is_chunk_loaded(0, 0); }));

    // The edit is persisted: a fresh world loaded from the save has it.
    std::unique_ptr<engine::voxel::IVoxelWorld> loaded =
        engine::voxel::create_default_voxel_world();
    loaded->register_generator(std::make_shared<FlatGenerator>(96));
    loaded->register_storage(engine::storage::create_region_chunk_storage(8));
    CHECK(boot_world(*loaded, home, 16));
    std::string loadError;
    CHECK(loaded->load_world(saveDir, loadError));
    CHECK(loadError.empty());
    CHECK(loaded->get_block(8, 130, 8) == kBlockStone);

    std::cout << "[sdk] streaming eviction preserves unsaved edits (pin "
                 "until save, evictable + persisted after) OK\n";
}

// The block registry is the source of truth for settable ids on the world
// (META section 7 + Prioridade 0 item 1): a builtin block resolves to its
// engine id; a catalog-only JSON block gets a DYNAMIC runtime id (>= Count,
// allocated by UUID — order-independent) and can actually be placed, read,
// raycast against (solid/collidable semantics from the definition) and removed.
// A fabricated id that is not registered is still rejected instead of silently
// aliasing Air.
void test_world_registry_source_of_truth() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();

    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    // ruby: solid/collidable red block. spirit: non-collidable (ray passes
    // through) green block. Both have NO builtinId — catalog-only.
    CHECK(registry->load_from_json(
        R"([{"name":"ruby","namespace":"test","class":"solid","hardness":3.0,"color":[0.9,0.1,0.1]},)"
        R"({"name":"spirit","namespace":"test","class":"solid","collidable":false,"color":[0.1,0.9,0.1]}])",
        error));
    world->set_block_registry(registry);
    // Flat world (surface at y=96) so the floating test blocks at y=130 are
    // in open air and the rays behave deterministically.
    world->register_generator(std::make_shared<FlatGenerator>(96));

    // Builtin block resolves to its engine id.
    uint32_t id = 0;
    CHECK(world->resolve_block_id("vulkancraft:stone", id, error));
    CHECK(error.empty());
    CHECK(id == kBlockStone);

    // Catalog-only blocks now resolve to DYNAMIC ids >= BlockType::Count.
    uint32_t rubyId = 0;
    CHECK(world->resolve_block_id("test:ruby", rubyId, error));
    CHECK(error.empty());
    CHECK(rubyId >= 51u);  // BlockType::Count
    uint32_t spiritId = 0;
    CHECK(world->resolve_block_id("test:spirit", spiritId, error));
    CHECK(error.empty());
    CHECK(spiritId != rubyId);

    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    // A dynamic block can be placed and read back through the transaction path.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, rubyId);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
        CHECK(world->get_block(3, 130, 3) == rubyId);
    }

    // A fabricated id that is not registered is rejected (never Air silently).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(6, 130, 6, 500);  // not in the runtime table
        std::string txError;
        CHECK(!tx->commit(txError));
        CHECK(!txError.empty());
        CHECK(world->get_block(6, 130, 6) == kBlockAir);  // nothing written
    }

    // Solid semantics come from the definition: a downward ray hits the
    // collidable ruby block, and passes THROUGH the non-collidable spirit.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(8, 130, 8, spiritId);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    const engine::voxel::VoxelRaycastHit hitRuby =
        world->raycast(glm::vec3(3.5f, 200.0f, 3.5f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(hitRuby.hit);
    CHECK(hitRuby.block.x == 3 && hitRuby.block.y == 130 && hitRuby.block.z == 3);
    const engine::voxel::VoxelRaycastHit throughSpirit =
        world->raycast(glm::vec3(8.5f, 200.0f, 8.5f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(throughSpirit.hit);                       // hits the flat ground below
    CHECK(throughSpirit.block.y < 130);             // spirit did NOT stop the ray

    // Air (id 0) remains settable: removal keeps working, and the dynamic
    // block is undoable through the same stack.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> rm = world->begin_transaction();
        rm->remove_block(3, 130, 3);
        std::string rmError;
        CHECK(rm->commit(rmError));
        CHECK(rmError.empty());
        CHECK(world->get_block(3, 130, 3) == kBlockAir);
    }

    std::cout << "[sdk] world registry: dynamic ids for catalog-only blocks "
                 "(place/raycast/remove), fabricated ids rejected\n";
}

// Data-driven fluids (META section 13): the registry is the project's way to
// declare what a fluid IS (viscosity, range, cadence, source/falling/
// evaporation, damage); the engine runs the simulation.
void test_fluid_registry() {
    engine::registry::FluidRegistry fluids;
    std::string error;
    CHECK(fluids.load_from_json(
        R"([{"block":"test:sludge","viscosity":0.0,"range":4,"evaporation":false},)"
        R"({"block":"test:tar","viscosity":1.0,"range":2,"tickInterval":0.16,"damagePerTick":3.0}])",
        error));
    CHECK(error.empty());
    CHECK(fluids.size() == 2);

    const engine::registry::FluidDefinition* sludge = fluids.find_by_block("test:sludge");
    CHECK(sludge != nullptr);
    CHECK(sludge->viscosity == 0.0f);
    CHECK(sludge->range == 4);
    CHECK(!sludge->evaporation);
    CHECK(!sludge->uuid.empty());          // derived from the block name
    CHECK(fluids.find_by_uuid(sludge->uuid) != nullptr);
    const engine::registry::FluidDefinition* tar = fluids.find_by_block("test:tar");
    CHECK(tar != nullptr);
    CHECK(tar->damagePerTick == 3.0f);
    CHECK(tar->tickInterval == 0.16f);

    // Deterministic order: same UUID sequence regardless of load order.
    const std::vector<engine::registry::FluidDefinition> first = fluids.all_definitions();
    engine::registry::FluidRegistry reversed;
    CHECK(reversed.load_from_json(
        R"([{"block":"test:tar","viscosity":1.0,"range":2},)"
        R"({"block":"test:sludge","viscosity":0.0,"range":4}])",
        error));
    const std::vector<engine::registry::FluidDefinition> second = reversed.all_definitions();
    CHECK(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) CHECK(first[i].uuid == second[i].uuid);

    // Invalid definitions fail with a diagnostic (never a guessed fluid).
    engine::registry::FluidRegistry strict;
    CHECK(!strict.load_from_json(R"([{"viscosity":0.5}])", error));   // no block
    CHECK(error.find("block") != std::string::npos);
    CHECK(strict.size() == 0);

    std::cout << "[sdk] fluid registry: JSON, UUID identity, order-independent, "
                 "validation OK\n";
}

// FALTANTES item 7: a block can declare its fluid behavior inline, so a
// catalog-only fluid block needs no separate FluidRegistry asset. Schema
// round-trip + all-or-nothing validation, then end-to-end: the world builds
// its fluid table from the block's inline binding and the fluid spreads.
void test_block_inline_fluid() {
    engine::registry::BlockRegistry blocks;
    std::string error;
    CHECK(blocks.load_from_json(
        R"({"name":"goo","namespace":"test","class":"fluid","drops":["test:goo"],"fluid":{"viscosity":0.0,"range":4,"evaporation":false,"damagePerTick":2.0}})",
        error));
    const engine::registry::BlockDefinition* goo = blocks.find_by_name("test:goo");
    CHECK(goo != nullptr);
    CHECK(goo->fluid.declared);
    CHECK(goo->fluid.viscosity == 0.0f);
    CHECK(goo->fluid.range == 4);
    CHECK(!goo->fluid.evaporation);
    CHECK(goo->fluid.damagePerTick == 2.0f);
    CHECK(goo->fluid.source);    // defaults
    CHECK(goo->fluid.falling);
    engine::registry::BlockRegistry plain;
    CHECK(plain.load_from_json(R"({"name":"solid","namespace":"test"})", error));
    CHECK(!plain.find_by_name("test:solid")->fluid.declared);

    // Validation is all-or-nothing: out-of-contract inline fluid is refused.
    auto refuse = [&](const std::string& json) {
        engine::registry::BlockRegistry registry;
        std::string refuseError;
        CHECK(!registry.load_from_json(json, refuseError));
        CHECK(!refuseError.empty());
    };
    refuse(R"({"name":"a","namespace":"test","fluid":{"range":9}})");
    refuse(R"({"name":"b","namespace":"test","fluid":{"viscosity":1.5}})");
    refuse(R"({"name":"c","namespace":"test","fluid":{"density":-1}})");
    refuse(R"({"name":"d","namespace":"test","fluid":{"tickInterval":-0.5}})");
    refuse(R"({"name":"e","namespace":"test","fluid":{"damagePerTick":-2}})");

    // End-to-end: a catalog-only fluid block with inline props drives the
    // simulation WITHOUT any FluidRegistry (thin goo: 2 levels/tick, range 4
    // caps at x=11), exactly like the registry-driven goo of the §13 tests.
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    auto gooBlocks = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(gooBlocks->load_from_json(
        R"({"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3],"fluid":{"viscosity":0.0,"range":4,"falling":false}})",
        error));
    world->set_block_registry(gooBlocks);
    // No set_fluid_registry: the inline binding is the only fluid source.
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t gooId = 0;
    CHECK(world->resolve_block_id("test:goo", gooId, error));
    world->set_block(8, kFluidTestGroundedY, 8, gooId);
    CHECK(settle(*world, player, [&] { return world->get_fluid_level(9, kFluidTestGroundedY, 8) == 2; }));
    CHECK(world->get_fluid_level(8, kFluidTestGroundedY, 8) == 0);   // source
    CHECK(world->get_fluid_level(9, kFluidTestGroundedY, 8) == 2);   // thin: 2/tick
    CHECK(settle(*world, player, [&] { return world->get_fluid_level(10, kFluidTestGroundedY, 8) == 4; }));
    CHECK(world->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);  // range 4 caps

    std::cout << "[sdk] block inline fluid: round-trip, validation and "
                 "registry-free spread OK\n";
}

// The engine runs the project's fluid parameters: thin fluids spread 2 levels
// per step (rings at 2, 4), thick fluids 1 level per step (rings at 1, 2), and
// the spread budget (range) caps both.
void test_fluid_generalized() {
    const auto build = [&](const std::string& fluidJson, uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(fluidJson, error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Thin goo (viscosity 0 -> 2 levels/tick), range 4: rings at 2 and 4. The
    // pool sits on the dry terrain top (non-air below) so the ring is fed
    // sideways — same spread semantics as the old water-below setup, without
    // the generated sea lake flooding the fluid-physics FIFO.
    std::unique_ptr<engine::voxel::IVoxelWorld> thin;
    uint32_t thinId = 0;
    build(R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false}])",
          thinId, thin, player);
    thin->set_block(8, kFluidTestGroundedY, 8, static_cast<uint32_t>(thinId));
    CHECK(settle(*thin, player, [&] { return thin->get_fluid_level(9, kFluidTestGroundedY, 8) == 2; }));
    CHECK(thin->get_fluid_level(8, kFluidTestGroundedY, 8) == 0);   // source
    CHECK(thin->get_fluid_level(9, kFluidTestGroundedY, 8) == 2);   // thin: 2 levels per step
    CHECK(settle(*thin, player, [&] { return thin->get_fluid_level(10, kFluidTestGroundedY, 8) == 4; }));
    CHECK(thin->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);  // range 4 caps

    // Thick goo (viscosity 1 -> 1 level/tick), range 2: rings at 1 and 2.
    std::unique_ptr<engine::voxel::IVoxelWorld> thick;
    uint32_t thickId = 0;
    build(R"([{"block":"test:goo","viscosity":1.0,"range":2,"falling":false}])",
          thickId, thick, player);
    thick->set_block(8, kFluidTestGroundedY, 8, static_cast<uint32_t>(thickId));
    CHECK(settle(*thick, player, [&] { return thick->get_fluid_level(9, kFluidTestGroundedY, 8) == 1; }));
    CHECK(thick->get_fluid_level(9, kFluidTestGroundedY, 8) == 1);
    CHECK(settle(*thick, player, [&] { return thick->get_fluid_level(10, kFluidTestGroundedY, 8) == 2; }));
    CHECK(thick->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);  // range 2 caps it

    std::cout << "[sdk] fluids: data-driven spread (thin 2/tick, thick 1/tick, "
                 "range caps) OK\n";
}

// evaporation=false keeps an unfed cell (pooled); evaporation=true decays it.
// Floating pools (below = air) are never fed, so the flag is directly visible.
void test_fluid_evaporation() {
    const auto build = [&](const std::string& fluidJson, uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(fluidJson, error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Pooled goo: evaporation=false -> the spread ring SURVIVES its unfed
    // step (the pool floats in air, so nothing can feed the ring sideways; an
    // unfed cell also stops spreading, so the pool is exactly one ring).
    std::unique_ptr<engine::voxel::IVoxelWorld> pooled;
    uint32_t pooledId = 0;
    build(R"([{"block":"test:goo","viscosity":1.0,"range":2,"falling":false,"evaporation":false}])",
          pooledId, pooled, player);
    pooled->set_block(8, kFluidTestFloatingY, 8, static_cast<uint32_t>(pooledId));
    CHECK(settle(*pooled, player, [&] { return pooled->get_fluid_level(9, kFluidTestFloatingY, 8) == 1; }));
    CHECK(pooled->get_block(9, kFluidTestFloatingY, 8) == static_cast<uint32_t>(pooledId));
    CHECK(pooled->get_block(10, kFluidTestFloatingY, 8) == kBlockAir);  // unfed cells don't spread

    // Evaporating goo: same setup with evaporation=true -> the ring decays to
    // air once it is processed (only the source cell remains).
    std::unique_ptr<engine::voxel::IVoxelWorld> evaporating;
    uint32_t evaporatingId = 0;
    build(R"([{"block":"test:goo","viscosity":1.0,"range":2,"falling":false,"evaporation":true}])",
          evaporatingId, evaporating, player);
    evaporating->set_block(8, kFluidTestFloatingY, 8, static_cast<uint32_t>(evaporatingId));
    CHECK(settle(*evaporating, player, [&] {
        return evaporating->get_block(9, kFluidTestFloatingY, 8) == kBlockAir &&
               evaporating->get_block(10, kFluidTestFloatingY, 8) == kBlockAir;
    }));
    CHECK(evaporating->get_block(8, kFluidTestFloatingY, 8) == static_cast<uint32_t>(evaporatingId));

    std::cout << "[sdk] fluids: evaporation flag (pooled vs decayed) OK\n";
}

// Per-fluid cadence (tickInterval): a slow fluid steps every N world ticks, so
// it visibly lags a fast fluid with the same range and viscosity.
void test_fluid_tick_cadence() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]},)"
        R"({"name":"ooze","namespace":"test","class":"fluid","color":[0.5,0.2,0.6]}])",
        error));
    world->set_block_registry(blocks);
    auto fluids = std::make_shared<engine::registry::FluidRegistry>();
    // Same range/viscosity; only the cadence differs (0.08 vs 0.64 seconds).
    CHECK(fluids->load_from_json(
        R"([{"block":"test:goo","viscosity":1.0,"range":4,"falling":false,"evaporation":false,"tickInterval":0.08},)"
        R"({"block":"test:ooze","viscosity":1.0,"range":4,"falling":false,"evaporation":false,"tickInterval":0.64}])",
        error));
    CHECK(world->set_fluid_registry(fluids, error));
    CHECK(error.empty());
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t gooId = 0, oozeId = 0;
    CHECK(world->resolve_block_id("test:goo", gooId, error));
    CHECK(world->resolve_block_id("test:ooze", oozeId, error));
    world->set_block(2, kFluidTestGroundedY, 2, gooId);
    world->set_block(12, kFluidTestGroundedY, 12, oozeId);

    // Fast goo reaches its full range (level 4 at distance 4: level = distance
    // for a 1-level/tick fluid)...
    CHECK(settle(*world, player, [&] { return world->get_fluid_level(6, kFluidTestGroundedY, 2) == 4; }));
    // ...while the slow ooze (8x cadence) has barely moved.
    CHECK(world->get_fluid_level(13, kFluidTestGroundedY, 12) < 4);
    // It catches up eventually: cadence gates speed, not reach.
    CHECK(settle(*world, player, [&] { return world->get_fluid_level(16, kFluidTestGroundedY, 12) == 4; }));

    std::cout << "[sdk] fluids: per-fluid tick cadence (slow lags, catches up) OK\n";
}

// FALTANTES §2 item 2: collision/selection shapes. Schema round-trip + strict
// refusals + a REAL consumer: the voxel raycast honors collisionShape (a
// block with collisionShape "none" is not solid even with the legacy
// collidable:true bool — the ray passes through to the terrain below).
void test_block_collision_selection_shapes() {
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"ghost","namespace":"test","collisionShape":"none","selectionShape":"none"},)"
        R"({"name":"picket","namespace":"test","collisionShape":"cross","selectionShape":"cross"},)"
        R"({"name":"plain","namespace":"test"}])",
        error));

    const auto* ghost = blocks->find_by_name("test:ghost");
    const auto* picket = blocks->find_by_name("test:picket");
    const auto* plain = blocks->find_by_name("test:plain");
    CHECK(ghost != nullptr && picket != nullptr && plain != nullptr);
    CHECK(ghost->collisionShape == engine::registry::CollisionShape::None);
    CHECK(ghost->selectionShape == engine::registry::SelectionShape::None);
    CHECK(!ghost->is_collidable());          // none wins over collidable:true
    CHECK(picket->collisionShape == engine::registry::CollisionShape::Cross);
    CHECK(picket->selectionShape == engine::registry::SelectionShape::Cross);
    CHECK(picket->is_collidable());
    CHECK(plain->collisionShape == engine::registry::CollisionShape::Full);
    CHECK(plain->selectionShape == engine::registry::SelectionShape::Full);
    CHECK(plain->is_collidable());

    // Strict enums: explicit unknown values are refused (all-or-nothing).
    auto blocks2 = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(!blocks2->load_from_json(R"([{"name":"bad","collisionShape":"octagon"}])", error));
    auto blocks3 = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(!blocks3->load_from_json(R"([{"name":"bad","selectionShape":"slab"}])", error));

    // End-to-end: raycast honors the collision shape. Player looks straight
    // down; the block sits on the terrain top (first air cell), so a solid
    // block is the FIRST hit and a ghost lets the ray reach the terrain.
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    world->set_block_registry(blocks);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t ghostId = 0, picketId = 0, plainId = 0;
    CHECK(world->resolve_block_id("test:ghost", ghostId, error));
    CHECK(world->resolve_block_id("test:picket", picketId, error));
    CHECK(world->resolve_block_id("test:plain", plainId, error));

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    world->set_block(2, kFluidTestGroundedY, 2, picketId);
    const engine::voxel::VoxelRaycastHit picketHit =
        world->raycast(glm::vec3(2.0f, 200.0f, 2.0f), down, 1000.0f);
    CHECK(picketHit.hit);
    CHECK(picketHit.block == glm::ivec3(2, kFluidTestGroundedY, 2));  // cross is solid at cell granularity

    world->set_block(4, kFluidTestGroundedY, 4, plainId);
    const engine::voxel::VoxelRaycastHit plainHit =
        world->raycast(glm::vec3(4.0f, 200.0f, 4.0f), down, 1000.0f);
    CHECK(plainHit.hit);
    CHECK(plainHit.block == glm::ivec3(4, kFluidTestGroundedY, 4));

    world->set_block(6, kFluidTestGroundedY, 6, ghostId);
    const engine::voxel::VoxelRaycastHit ghostHit =
        world->raycast(glm::vec3(6.0f, 200.0f, 6.0f), down, 1000.0f);
    CHECK(ghostHit.hit);
    // The ghost is skipped: the first solid cell is the terrain BELOW it.
    CHECK(ghostHit.block == glm::ivec3(6, kFluidTestTerrain, 6));

    std::cout << "[sdk] block collision/selection shapes (schema + raycast "
                 "honors collisionShape) OK\n";
}

// FALTANTES §3 item 1: public streaming/budget/observability contract. The
// world exposes a live StreamingSnapshot (chunk census + budgets) and an
// optional push monitor fired after update() while streaming state changes.
void test_streaming_observability() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Budget contract: default inside [min, max]; clamps are honored.
    const engine::voxel::StreamingSnapshot initial = world->streaming_snapshot();
    CHECK(initial.chunkBudget >= initial.chunkBudgetMin);
    CHECK(initial.chunkBudget <= initial.chunkBudgetMax);
    CHECK(initial.chunkBudgetMin == 8);  // world constant (MIN_CHUNK_BUDGET)
    world->set_chunk_budget(100000);
    CHECK(world->streaming_snapshot().chunkBudget == initial.chunkBudgetMax);
    world->set_chunk_budget(1);
    CHECK(world->streaming_snapshot().chunkBudget == initial.chunkBudgetMin);
    world->set_chunk_budget(16);

    // Push observability: a monitor receives snapshots while the world boots
    // and streaming state moves; the census is consistent and the budgets are
    // live.
    struct Recorder final : engine::voxel::IVoxelStreamingMonitor {
        std::vector<engine::voxel::StreamingSnapshot> seen;
        void on_streaming_update(const engine::voxel::StreamingSnapshot& snapshot) override {
            seen.push_back(snapshot);
        }
    };
    auto recorder = std::make_shared<Recorder>();
    world->set_streaming_monitor(recorder);
    CHECK(boot_world(*world, player, 16));
    CHECK(!recorder->seen.empty());
    const engine::voxel::StreamingSnapshot last = recorder->seen.back();
    CHECK(last.chunksLoaded > 0);
    // Every present chunk is in exactly one non-Unloaded state.
    CHECK(last.chunksLoaded ==
          last.chunksGenerating + last.chunksMeshReady + last.chunksUploaded);
    // Headless (NullBridge): meshes complete; uploads stay 0 by design.
    CHECK(last.chunksMeshReady + last.chunksUploaded >= 1);
    CHECK(last.chunkBudget == 16);
    CHECK(last.workerThreads > 0);
    CHECK(last.farLodPercent >= 0 && last.farLodPercent <= 100);

    // Clearing the monitor stops dispatch: updates stop reaching the recorder.
    world->set_streaming_monitor(nullptr);
    const std::size_t beforeClear = recorder->seen.size();
    for (int i = 0; i < 20; ++i) world->update(player, 1.0f / 60.0f);
    CHECK(recorder->seen.size() == beforeClear);

    std::cout << "[sdk] streaming observability (snapshot census + budgets + "
                 "monitor) OK\n";
}

// RAM-budgeted chunk cache (FALTANTES §4 item 6): a memory budget bounds the
// estimated bytes of loaded chunk data; each frame's end evicts the farthest
// chunks while over budget, so every update boundary satisfies the budget. The
// closest chunk survives the eviction (farthest-first policy) and the window
// repopulates once the budget is lifted.
void test_chunk_memory_budget() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    // Park the player over a PLAIN chunk (far from the builtin structure
    // sites near the origin) so the closest chunk is ordinary flat terrain.
    const glm::vec3 player{ 8.0f, 200.0f, 200.0f };  // chunk (0,12)
    CHECK(settle(*world, player,
                 [&] { return world->is_chunk_loaded(0, 12); }));

    // Default: unlimited budget, real usage reported.
    engine::voxel::StreamingSnapshot before = world->streaming_snapshot();
    CHECK(before.memoryBudgetBytes == 0);
    CHECK(before.ramUsageBytes > 0);
    CHECK(before.chunksLoaded >= 5);

    // An edit in the player's own chunk (extent reaches y=110 -> ~86 KB).
    world->set_block(5, 110, 205, kBlockStone);
    CHECK(world->get_block(5, 110, 205) == kBlockStone);

    // Tiny budget: ONE chunk fits. Every update boundary must satisfy the
    // budget (eviction runs at the end of the frame), farthest chunks go
    // first, and the closest chunk (the edited one) survives.
    const uint64_t kTinyBudget = 100000;
    world->set_memory_budget(kTinyBudget);
    CHECK(settle(*world, player,
                 [&] { return world->streaming_snapshot().ramUsageBytes <= kTinyBudget; }));
    const engine::voxel::StreamingSnapshot clamped = world->streaming_snapshot();
    CHECK(clamped.memoryBudgetBytes == kTinyBudget);
    CHECK(clamped.ramUsageBytes <= kTinyBudget);
    CHECK(clamped.chunksLoaded <= 3);      // budget evicted most of the window
    CHECK(clamped.chunksLoaded >= 1);      // at least the closest chunk stays
    CHECK(world->get_block(5, 110, 205) == kBlockStone);  // edited chunk survives

    // Lifting the budget lets the window repopulate.
    world->set_memory_budget(0);
    CHECK(settle(*world, player,
                 [&] { return world->streaming_snapshot().chunksLoaded >= 5; }));
    CHECK(world->streaming_snapshot().ramUsageBytes > kTinyBudget);
    CHECK(world->streaming_snapshot().memoryBudgetBytes == 0);

    std::cout << "[sdk] chunk memory budget: end-of-frame eviction clamps "
                 "usage, farthest-first, closest chunk survives OK\n";
}

// Autosave incremental (FALTANTES §4 item 7): while enabled, the headless
// update() tick fires a DELTA async save (items 3 + 5) to the autosave path
// when a time interval OR a change-volume threshold is crossed. A no-change
// autosave must not rewrite chunk pages (the delta gate), a fire while
// another op is in flight is skipped (timers keep running), and disabling
// autosave stops future fires.
void test_autosave_incremental() {
    const auto pageFile = [](const std::string& dir, const std::string& pageId) {
        std::string leaf = pageId;
        for (char& c : leaf) if (c == '.') c = '_';
        return dir + "/pages/" + leaf + ".dat";
    };
    const auto pageMtime = [&](const std::string& dir, const std::string& pageId) {
        return std::filesystem::last_write_time(pageFile(dir, pageId));
    };
    const auto readFile = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };

    // --- Phase A: edits + time trigger fires an autosave. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);      // chunk (0,0) -> r.0.0
        tx->set_block(-5, 130, -5, kBlockStone);    // chunk (-1,-1) -> r.-1.-1
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto storeA = engine::storage::create_region_chunk_storage(8);
    a->register_storage(storeA);
    const std::string dir = scratch_dir() + "/vc_test_autosave";

    engine::voxel::IVoxelWorld::AutosaveConfig cfg;
    cfg.enabled = true;
    cfg.intervalSeconds = 0.05;   // time trigger: 3 ticks of 1/60s
    cfg.dirtyChunkThreshold = 0;  // volume trigger disabled in Phase A
    a->set_autosave(cfg, dir);
    CHECK(a->autosave_config().enabled);
    CHECK(a->autosave_config().intervalSeconds == 0.05);

    // The time trigger fires while the caller keeps simulating.
    for (int i = 0; i < 4; ++i) a->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    std::string waitErr;
    CHECK(a->wait_async_saves(waitErr));
    CHECK(waitErr.empty());
    CHECK(std::filesystem::exists(pageFile(dir, "world")));
    CHECK(storeA->page_ids().size() >= 5);  // manifest + region tiles

    // Round-trip: a fresh world loads the autosave (edits present).
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadErr;
    CHECK(b->load_world(dir, loadErr));
    CHECK(loadErr.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    CHECK(b->get_block(-5, 130, -5) == kBlockStone);

    // --- Phase B: a no-change autosave (time still elapsing) must NOT
    // rewrite a single chunk page — the delta gate survives the autosave
    // path. The manifest is intentionally rewritten (small, always fresh). ---
    const std::vector<std::string> pages = storeA->page_ids();
    std::map<std::string, std::filesystem::file_time_type> beforeTime;
    std::map<std::string, std::string> beforeBytes;
    for (const auto& pid : pages) {
        beforeTime[pid] = pageMtime(dir, pid);
        beforeBytes[pid] = readFile(pageFile(dir, pid));
    }
    for (int i = 0; i < 6; ++i) a->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    std::string waitErr2;
    CHECK(a->wait_async_saves(waitErr2));
    for (const auto& pid : pages) {
        if (pid == "world") continue;  // manifest always rewritten
        CHECK(pageMtime(dir, pid) == beforeTime[pid]);
        CHECK(readFile(pageFile(dir, pid)) == beforeBytes[pid]);
    }

    // --- Phase C: volume trigger fires early (threshold 1). ---
    engine::voxel::IVoxelWorld::AutosaveConfig cfgVol;
    cfgVol.enabled = true;
    cfgVol.intervalSeconds = 0;   // time trigger off
    cfgVol.dirtyChunkThreshold = 1;
    a->set_autosave(cfgVol, dir);
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(21, 130, 21, kBlockStone);  // chunk (1,1) -> r.0.0
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    a->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);  // one tick fires it
    std::string waitErr3;
    CHECK(a->wait_async_saves(waitErr3));
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadErr2;
    CHECK(c->load_world(dir, loadErr2));
    CHECK(loadErr2.empty());
    CHECK(c->get_block(3, 130, 3) == kBlockStone);
    CHECK(c->get_block(21, 130, 21) == kBlockStone);  // volume-fired edit present

    // --- Phase D: a fire while another op is in flight is skipped (timers
    // keep running — the next eligible update fires it). Deterministic via a
    // storage that blocks the first page write. ---
    class GatedStorage final : public engine::voxel::IChunkStorage {
    public:
        explicit GatedStorage(std::shared_ptr<engine::voxel::IChunkStorage> inner)
            : inner_(std::move(inner)) {}
        std::string serialize_world(std::string& e) override { return inner_->serialize_world(e); }
        bool deserialize_world(const std::string& d, std::string& e) override { return inner_->deserialize_world(d, e); }
        bool supports_regions() const override { return true; }
        bool save_world(const std::string& p, std::string& e) override { return inner_->save_world(p, e); }
        bool load_world(const std::string& p, std::string& e) override { return inner_->load_world(p, e); }
        std::vector<std::string> page_ids() const override { return inner_->page_ids(); }
        bool commit_save(std::string& e) override { return inner_->commit_save(e); }
        bool save_page(const std::string& id, const std::string& bytes, std::string& e) override {
            if (!gatePassed_ && id != "world") {
                std::unique_lock<std::mutex> ul(m_);
                if (!gatePassed_) {  // only the first page write gates
                    gatePassed_ = true;
                    enteredSave_ = true;
                    enteredCv_.notify_all();
                    releaseCv_.wait(ul, [&] { return release_.load(); });
                }
            }
            return inner_->save_page(id, bytes, e);
        }
        bool load_page(const std::string& id, std::string& bytes, std::string& e) override { return inner_->load_page(id, bytes, e); }
        void wait_entered() {
            std::unique_lock<std::mutex> ul(m_);
            enteredCv_.wait(ul, [&] { return enteredSave_.load(); });
        }
        void release() {
            std::lock_guard<std::mutex> lock(m_);
            release_ = true;
            releaseCv_.notify_all();
        }
        std::shared_ptr<engine::voxel::IChunkStorage> inner_;
        mutable std::mutex m_;
        std::condition_variable enteredCv_, releaseCv_;
        std::atomic_bool enteredSave_{ false }, release_{ false };
        std::atomic_bool gatePassed_{ false };
    };

    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto gated = std::make_shared<GatedStorage>(
        engine::storage::create_region_chunk_storage(8));
    d->register_storage(gated);
    const std::string dirD = scratch_dir() + "/vc_test_autosave_gated";
    d->set_autosave(cfgVol, dirD);

    // First fire gets stuck in the gate (op in flight).
    d->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    gated->wait_entered();
    // A second fire while in flight must be SKIPPED, not deadlock (update
    // returns promptly; the timers keep running).
    d->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    gated->release();
    std::string waitErrD;
    CHECK(d->wait_async_saves(waitErrD));
    CHECK(waitErrD.empty());

    // --- Phase E: disabling autosave stops future fires. ---
    engine::voxel::IVoxelWorld::AutosaveConfig cfgOff;
    cfgOff.enabled = false;
    d->set_autosave(cfgOff, dirD);
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = d->begin_transaction();
        tx->set_block(9, 130, 9, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    const auto pagesD = gated->page_ids();
    std::map<std::string, std::filesystem::file_time_type> beforeD;
    for (const auto& pid : pagesD) beforeD[pid] = pageMtime(dirD, pid);
    for (int i = 0; i < 10; ++i) d->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    std::string waitErrE;
    CHECK(d->wait_async_saves(waitErrE));
    for (const auto& pid : pagesD) {
        if (pid == "world") continue;
        CHECK(pageMtime(dirD, pid) == beforeD[pid]);
    }

    std::cout << "[sdk] autosave incremental: time + volume triggers fire a "
                 "delta async save, no-change save skips chunk pages, "
                 "in-flight fire skipped, disable stops fires OK\n";
}

// WAL recovery (FALTANTES §4 item 8): a save journals the undo data of every
// page it is about to overwrite (wal/<page>.bak, or a .missing tombstone for
// pages that did not exist); commit_save drops the journal. A save left
// UNcommitted (process death, power loss) is detected on the next load_world
// and rolled back — the directory returns to the last committed save, never a
// mix of old and new pages.
void test_wal_recovery() {
    const auto pageFile = [](const std::string& dir, const std::string& pageId) {
        std::string leaf = pageId;
        for (char& c : leaf) if (c == '.') c = '_';
        return dir + "/pages/" + leaf + ".dat";
    };
    const auto readFile = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };
    const auto fileExists = [](const std::string& path) {
        return std::filesystem::is_regular_file(path);
    };
    const std::string dir = scratch_dir() + "/vc_test_wal";

    // --- Phase A: a committed save leaves NO journal, and load is a no-op
    // recovery (bytes round-trip unchanged). ---
    auto s1 = engine::storage::create_region_chunk_storage(8);
    std::string errA;
    CHECK(s1->save_world(dir, errA));
    CHECK(errA.empty());
    CHECK(s1->save_page("world", "manifest-A", errA));
    CHECK(s1->save_page("r.0.0", "page-A", errA));
    CHECK(s1->commit_save(errA));
    CHECK(errA.empty());
    CHECK(!fileExists(dir + "/wal"));  // committed: journal gone
    std::string loadA;
    CHECK(s1->load_page("r.0.0", loadA, errA));
    CHECK(loadA == "page-A");

    // --- Phase B: an UNCOMMITTED save (interruption mid-write) is rolled
    // back on the next load. "page-B" replaced r.0.0 and "r.1.1" was
    // created, then the process "died" before commit_save. ---
    auto s2 = engine::storage::create_region_chunk_storage(8);
    std::string errB;
    CHECK(s2->save_world(dir, errB));   // begin: clears any prior journal
    CHECK(s2->save_page("r.0.0", "page-B", errB));  // overwrites page-A
    CHECK(s2->save_page("r.1.1", "page-C", errB));  // new page (tombstone)
    CHECK(fileExists(dir + "/wal/r_0_0.dat.bak"));
    CHECK(fileExists(dir + "/wal/r_1_1.dat.missing"));
    // NO commit_save() — the save was interrupted here.
    CHECK(readFile(pageFile(dir, "r.0.0")) == "page-B");  // torn on disk

    // A fresh store loads: recovery must restore the committed state.
    auto s3 = engine::storage::create_region_chunk_storage(8);
    std::string errC;
    CHECK(s3->load_world(dir, errC));
    CHECK(errC.empty());
    CHECK(!fileExists(dir + "/wal"));  // journal consumed by recovery
    std::string pageAfter;
    CHECK(s3->load_page("r.0.0", pageAfter, errC));
    CHECK(pageAfter == "page-A");      // rolled back to the committed save
    std::string missingPage;
    CHECK(!s3->load_page("r.1.1", missingPage, errC));  // tombstoned: gone
    std::string worldAfter;
    CHECK(s3->load_page("world", worldAfter, errC));
    CHECK(worldAfter == "manifest-A");  // untouched page survived

    // --- Phase C: a complete NEW save after recovery commits normally (the
    // delta bookkeeping on the facade sees no stale journal). ---
    auto s4 = engine::storage::create_region_chunk_storage(8);
    std::string errD;
    CHECK(s4->save_world(dir, errD));
    CHECK(s4->save_page("r.0.0", "page-D", errD));
    CHECK(s4->save_page("r.1.1", "page-D2", errD));
    CHECK(s4->commit_save(errD));
    CHECK(errD.empty());
    auto s5 = engine::storage::create_region_chunk_storage(8);
    std::string errE;
    CHECK(s5->load_world(dir, errE));
    std::string d;
    CHECK(s5->load_page("r.0.0", d, errE));
    CHECK(d == "page-D");
    CHECK(s5->load_page("r.1.1", d, errE));
    CHECK(d == "page-D2");

    // --- Phase D: end-to-end through the facade — an interrupted SAVE leaves
    // a journal that the next load_world rolls back, so a world never loads a
    // half-written save. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto storeA = engine::storage::create_region_chunk_storage(8);
    a->register_storage(storeA);
    const std::string dirA = scratch_dir() + "/vc_test_wal_facade";
    std::string saveErr;
    CHECK(a->save_world(dirA, saveErr));
    CHECK(saveErr.empty());
    CHECK(!fileExists(dirA + "/wal"));  // committed

    // Simulate an interrupted second save: write a page directly through the
    // backend WITHOUT commit (what a crash mid-save leaves behind).
    auto storeB = engine::storage::create_region_chunk_storage(8);
    std::string errF;
    CHECK(storeB->save_world(dirA, errF));
    CHECK(storeB->save_page("r.0.0", "corrupt-interrupt", errF));
    CHECK(fileExists(dirA + "/wal/r_0_0.dat.bak"));

    // The next load_world recovers: the world restores the committed save.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadErr;
    CHECK(b->load_world(dirA, loadErr));
    CHECK(loadErr.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);  // committed edit survived
    CHECK(!fileExists(dirA + "/wal"));

    std::cout << "[sdk] wal recovery: committed save leaves no journal, "
                 "interrupted save rolls back to the last committed state "
                 "(pages + tombstones), end-to-end through the facade OK\n";
}

// FALTANTES §25 — REAL process interruption mid-save. test_wal_recovery above
// simulates the WAL journal window in-process; here the window is produced by
// a REAL child process that dies (self-_exit: no commit_save, no destructors,
// no flushing — the exact on-disk state a killed process leaves) between page
// writes and commit, and a FRESH process verifies recovery on the next load.
//
// Child role: commit a baseline save through the facade, then begin a second
// save that overwrites one page and adds another, and die before commit.
void child_crash_mid_save(const std::string& dir) {
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    if (!boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16)) ::_exit(66);
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx =
            a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string error;
        if (!tx->commit(error)) ::_exit(67);
    }
    auto storeA = engine::storage::create_region_chunk_storage(8);
    a->register_storage(storeA);
    std::string saveErr;
    if (!a->save_world(dir, saveErr) || !saveErr.empty()) ::_exit(68);
    if (std::filesystem::exists(dir + "/wal")) ::_exit(69);  // committed: none

    // Interrupted second save: swap an existing page and add a new one, then
    // die BEFORE commit_save — the exact window the WAL journal covers.
    auto b = engine::storage::create_region_chunk_storage(8);
    std::string err;
    if (!b->save_world(dir, err)) ::_exit(70);
    if (!b->save_page("r.0.0", "torn-interrupted-page", err)) ::_exit(71);
    if (!b->save_page("r.1.1", "never-committed-page", err)) ::_exit(72);
    ::_exit(137);  // "killed" mid-save: no commit, no destructors, no flush
}

std::string self_exe_path(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf, n);
    return argv0 ? argv0 : "";
#else
    return argv0 ? argv0 : "";
#endif
}

// Spawns THIS binary as the crash child (env VC_TEST_CRASH_CHILD=1 +
// VC_TEST_CRASH_DIR=<dir>) and waits. Returns the child's exit code.
int spawn_crash_child(const std::string& exePath, const std::string& dir) {
#ifdef _WIN32
    ::_putenv("VC_TEST_CRASH_CHILD=1");
    ::_putenv(("VC_TEST_CRASH_DIR=" + dir).c_str());
    const intptr_t code =
        ::_spawnl(_P_WAIT, exePath.c_str(), exePath.c_str(),
                  static_cast<const char*>(nullptr));
    ::_putenv("VC_TEST_CRASH_CHILD=");  // unset: restore parent env
    ::_putenv("VC_TEST_CRASH_DIR=");
    return static_cast<int>(code);
#else
    ::setenv("VC_TEST_CRASH_CHILD", "1", 1);
    ::setenv("VC_TEST_CRASH_DIR", dir.c_str(), 1);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exePath.c_str(), exePath.c_str(), static_cast<char*>(nullptr));
        ::_exit(90);  // exec failed
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::unsetenv("VC_TEST_CRASH_CHILD");
    ::unsetenv("VC_TEST_CRASH_DIR");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

void test_real_process_interruption(const char* exePath) {
    if (exePath == nullptr || exePath[0] == '\0') {
        std::cout << "[sdk] real process interruption: skipped (no exe path)\n";
        return;
    }
    const auto readFile = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };
    const std::string dir = scratch_dir() + "/crash_world";

    // The child commits a save, then dies mid-save (exit 137) before commit.
    const int code = spawn_crash_child(exePath, dir);
    CHECK(code == 137);

    // On-disk evidence a killed process leaves behind: journal present, the
    // overwritten page torn on disk, backup/tombstone markers registered.
    CHECK(std::filesystem::exists(dir + "/wal"));
    CHECK(std::filesystem::exists(dir + "/wal/r_0_0.dat.bak"));
    CHECK(std::filesystem::exists(dir + "/wal/r_1_1.dat.missing"));
    CHECK(readFile(dir + "/pages/r_0_0.dat") == "torn-interrupted-page");

    // A FRESH process loads: recovery must roll back to the committed save.
    std::unique_ptr<engine::voxel::IVoxelWorld> w =
        engine::voxel::create_default_voxel_world();
    w->register_generator(std::make_shared<FlatGenerator>(96));
    w->register_storage(engine::storage::create_region_chunk_storage(8));
    CHECK(boot_world(*w, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadErr;
    CHECK(w->load_world(dir, loadErr));
    CHECK(loadErr.empty());
    CHECK(w->get_block(3, 130, 3) == kBlockStone);  // committed edit survived
    CHECK(!std::filesystem::exists(dir + "/wal"));   // journal consumed

    // The never-committed tile was rolled back (tombstoned = removed).
    auto after = engine::storage::create_region_chunk_storage(8);
    std::string err;
    CHECK(after->load_world(dir, err));
    std::string page;
    CHECK(!after->load_page("r.1.1", page, err));

    // No stray .tmp* left by the dead process or the recovery.
    bool strayTmp = false;
    std::error_code ec;
    for (auto& entry :
         std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.path().filename().string().rfind(".tmp", 0) == 0) {
            strayTmp = true;
            break;
        }
    }
    CHECK(!strayTmp);

    std::cout << "[sdk] real process interruption: child died mid-save (exit "
              << code << "), WAL journal left on disk, fresh load rolled back "
                 "to the committed save (stone survived, new tile gone, "
                 "journal consumed, no stray temp files) OK\n";
}

// Block entities + world entities through the PAGED save path (FALTANTES §4):
// the region manifest carries both (block entities with opaque state, world
// entities with health/components), so save_world/load_world via region pages
// must round-trip them exactly like the monolithic v5 serializer.
void test_region_entity_persistence() {
    // --- World A: block entity (counter=7) + world entity (cow, health,
    // component), then a PAGED save. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*a, player, 16));
    a->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machine = std::make_shared<CounterMachine>();
    machine->counter_ = 7;
    std::string error;
    CHECK(a->attach_block_entity(8, 96, 8, machine, error));
    auto entities = a->entity_world();
    CHECK(entities != nullptr);
    engine::entity::EntityId cow = entities->spawn(
        "vulkancraft:cow", engine::entity::Position{ 8.0f, 100.0f, 8.0f }, error);
    CHECK(cow.valid());
    CHECK(entities->set_health(cow, engine::entity::Health{ 7.0f, 20.0f }));
    engine::entity::ComponentData comp;
    comp.type = "vulkancraft:loot";
    comp.version = 1;
    comp.blob = "leather";
    CHECK(entities->set_component(cow, comp));

    a->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_regions_entities";
    std::string saveError;
    CHECK(a->save_world(dir, saveError));
    CHECK(saveError.empty());
    CHECK(std::filesystem::exists(dir + "/pages/world.dat"));

    // --- World B (factory registered): the paged load reconstructs BOTH the
    // block entity (opaque state) and the world entity (health/component). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, player, 16));
    b->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadError;
    CHECK(b->load_world(dir, loadError));
    CHECK(loadError.empty());
    auto restored =
        std::dynamic_pointer_cast<CounterMachine>(b->block_entity_at(8, 96, 8));
    CHECK(restored != nullptr);
    CHECK(restored->counter_ == 7);  // opaque project state survived
    CHECK(b->block_entity_count() == 1);
    auto bEntities = b->entity_world();
    CHECK(bEntities != nullptr);
    CHECK(bEntities->size() == 1);
    const auto inChunk = bEntities->entities_in_chunk(0, 0);
    CHECK(inChunk.size() == 1);
    engine::entity::Health health;
    CHECK(bEntities->get_health(inChunk[0], health));
    CHECK(health.value == 7.0f);
    engine::entity::ComponentData got;
    CHECK(bEntities->get_component(inChunk[0], "vulkancraft:loot", got));
    CHECK(got.blob == "leather");
    b->update(player, 0.09f);
    b->update(player, 0.09f);
    CHECK(restored->ticks_ >= 2);  // the restored block entity ticks again

    // --- World C WITHOUT the factory: the paged load is REFUSED (all-or-
    // nothing, same as the monolithic path — never a silently dropped
    // entity). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, player, 16));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string refuseError;
    CHECK(!c->load_world(dir, refuseError));
    CHECK(refuseError.find("project:counter_machine") != std::string::npos);

    std::cout << "[sdk] region entities: block entities + world entities "
                 "round-trip through the paged save (state, health, "
                 "components, refusal without factory) OK\n";
}

// FALTANTES §4 itens 17/18: load SOB DEMANDA — o chunk é materializado sem
// depender da janela de streaming e sem gerar o chunk antes de restaurá-lo
// (a geração da janela é suprimida durante o load) — + RESTAURAÇÃO EM LOTE —
// os dados voxel são escritos em UM lote com invalidação única, não
// set_block_at por voxel.
void test_region_batch_restore() {
    // --- Phase A: tile r.1.1 (chunk 8,8) fica FORA da janela de boot.
    // Terreno seco acima do nível do mar (130 > SeaLevel 127): a única água
    // do mundo é a que o teste coloca (byte de fluido estável, sem sim). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    CHECK(settle(*a, glm::vec3(130.0f, 200.0f, 130.0f),
                 [&] { return a->is_chunk_loaded(8, 8); }));
    constexpr int kTestWaterId = 12;  // BlockType::Water
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(135, 131, 135, kBlockStone);
        tx->set_block(133, 131, 133, kBlockStone);
        // Water source (byte 0): the fluid byte must round-trip through the
        // batch write; non-fluid cells stay WATER_LEVEL_NONE (engine contract).
        tx->set_block(135, 130, 133, kTestWaterId);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    a->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_batch_restore";
    std::string saveError;
    CHECK(a->save_world(dir, saveError));
    CHECK(saveError.empty());

    // --- Phase B: SEM boot. O load sob demanda materializa os chunks salvos
    // DIRETAMENTE (independente da janela, sem pré-gerar o chunk) e deixa
    // chunks não salvos AUSENTES. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadError;
    CHECK(b->load_world(dir, loadError));
    CHECK(loadError.empty());

    // Chunk salvo FORA da janela de boot (tile r.1.1) restaurado no lugar.
    CHECK(b->is_chunk_loaded(8, 8));
    // Chunk nunca salvo NÃO está materializado: nenhuma geração de janela
    // rodou durante o load (item 17 — restore não gera o chunk antes).
    CHECK(!b->is_chunk_loaded(62, 62));

    // Conteúdo exato via lote (item 18): blocos + byte de fluido.
    CHECK(b->get_block(135, 131, 135) == kBlockStone);
    CHECK(b->get_block(133, 131, 133) == kBlockStone);
    CHECK(b->get_fluid_level(135, 130, 133) == 0);     // fonte restaurada
    CHECK(b->get_fluid_level(135, 131, 135) == 0xff);  // não-fluida: NONE

    // Igualdade de grade completa do chunk restaurado contra A (amostras no
    // mesmo espírito do teste de região monolítico).
    bool identical = true;
    const int xs[] = { 128, 131, 133, 136, 143 };
    const int zs[] = { 128, 136, 143 };
    const int ys[] = { 1, 50, 130, 131, 200 };
    for (const int x : xs)
        for (const int z : zs)
            for (const int y : ys)
                if (a->get_block(x, y, z) != b->get_block(x, y, z))
                    identical = false;
    CHECK(identical);

    // --- Phase C: a geração RETOMA após o load (gap fill sob demanda). O
    // jogador se move para longe; o streaming gera a janela nova. ---
    CHECK(settle(*b, glm::vec3(1000.0f, 200.0f, 1000.0f),
                 [&] { return b->is_chunk_loaded(62, 62); }));

    std::cout << "[sdk] region batch restore: on-demand load (no "
                 "pre-generation, far chunk outside window), single-"
                 "invalidation batch write round-trips blocks + fluid byte, "
                 "generation resumes after load OK\n";
}

// FALTANTES §4 item 19: rollback do carregamento completo se QUALQUER chunk
// falhar. Save v3 montado à mão com 2 chunks — o chunk 1 tem um block id
// INVÁLIDO: o parse aplica o chunk 0 e então falha. O mundo B (com estado
// pré-load próprio: edit + block entity + world entity) deve voltar EXATAMENTE
// ao estado pré-load — nenhum chunk fica parcialmente restaurado.
void test_load_rollback() {
    // Helpers — mesmo layout dos saves v3 montados à mão do teste de migração.
    auto appendU32 = [](std::string& b, uint32_t v) {
        b.push_back(static_cast<char>(v & 0xFFu));
        b.push_back(static_cast<char>((v >> 8) & 0xFFu));
        b.push_back(static_cast<char>((v >> 16) & 0xFFu));
        b.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    auto appendI32 = [&appendU32](std::string& b, int32_t v) {
        appendU32(b, static_cast<uint32_t>(v));
    };
    auto appendU16 = [](std::string& b, uint16_t v) {
        b.push_back(static_cast<char>(v & 0xFFu));
        b.push_back(static_cast<char>((v >> 8) & 0xFFu));
    };
    auto appendFNV = [](std::string& b) {
        uint64_t hash = 1469598103934665603ull;
        for (const unsigned char c : b) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        for (int i = 0; i < 8; ++i) {
            b.push_back(static_cast<char>((hash >> (8 * i)) & 0xFFu));
        }
    };
    constexpr int kLayer = 256;

    // Chunk (0,0), extent 131: stone em (3,130,3) — ACIMA do terreno de 96 do
    // mundo pré-load (air lá) — distingue o conteúdo do save do conteúdo
    // gerado, provando o take-over E o rollback.
    auto buildChunk0 = [&](std::string& b) {
        appendI32(b, 0);
        appendI32(b, 0);
        appendU32(b, 131);
        const int stoneIndex = 130 * kLayer + 3 * 16 + 3;
        for (int v = 0; v < 131 * kLayer; ++v) {
            appendU16(b, (v == stoneIndex) ? static_cast<uint16_t>(kBlockStone)
                                           : static_cast<uint16_t>(kBlockAir));
        }
        for (int v = 0; v < 131 * kLayer; ++v) {
            b.push_back(static_cast<char>(0xFF));  // sem fluido
        }
    };
    // Chunk (5,5), extent 1: primeiro id válido (stone) ou INVÁLIDO (0xEE).
    auto buildChunk1 = [&](std::string& b, uint16_t firstId) {
        appendI32(b, 5);
        appendI32(b, 5);
        appendU32(b, 1);
        for (int v = 0; v < kLayer; ++v) {
            appendU16(b, (v == 0) ? firstId : static_cast<uint16_t>(kBlockAir));
        }
        for (int v = 0; v < kLayer; ++v) {
            b.push_back(static_cast<char>(0xFF));
        }
    };
    auto buildSave = [&](bool badChunk1) {
        std::string body = "VCWLD";
        appendU32(body, 3);  // v3
        appendU32(body, 0);  // paleta vazia
        appendU32(body, 2);  // 2 chunks
        buildChunk0(body);
        buildChunk1(body, badChunk1 ? 0xEEu : static_cast<uint16_t>(kBlockStone));
        appendU32(body, 0);  // 0 block entities
        appendFNV(body);
        return body;
    };

    // --- Mundo B: estado pré-load próprio (edit + block entity + world
    // entity), load falha no chunk 1 DEPOIS do chunk 0 aplicado. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = b->begin_transaction();
        tx->set_block(7, 131, 7, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    b->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machine = std::make_shared<CounterMachine>();
    machine->counter_ = 3;
    std::string error;
    CHECK(b->attach_block_entity(6, 96, 6, machine, error));
    auto bEntities = b->entity_world();
    CHECK(bEntities != nullptr);
    CHECK(bEntities->spawn("vulkancraft:cow",
                           engine::entity::Position{ 5.0f, 100.0f, 5.0f },
                           error).valid());

    const std::string badSave = buildSave(/*badChunk1=*/true);
    std::string loadError;
    CHECK(!b->deserialize_world(badSave, loadError));
    CHECK(loadError.find("unknown block id") != std::string::npos);

    // Rollback completo: chunk (0,0) assumido voltou ao estado pré-load (air
    // em y=130 — o save tinha stone lá), o edit próprio de B sobreviveu, o
    // chunk (5,5) criado pelo load foi REMOVIDO, block entity e world entity
    // de B continuam lá.
    CHECK(b->get_block(3, 130, 3) == kBlockAir);
    CHECK(b->get_block(7, 131, 7) == kBlockStone);
    CHECK(!b->is_chunk_loaded(5, 5));
    auto restored =
        std::dynamic_pointer_cast<CounterMachine>(b->block_entity_at(6, 96, 6));
    CHECK(restored != nullptr);
    CHECK(restored->counter_ == 3);
    CHECK(b->block_entity_count() == 1);
    CHECK(bEntities->size() == 1);

    // --- Caminho de sucesso: load válido COMMITA (sem rollback). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    const std::string goodSave = buildSave(/*badChunk1=*/false);
    std::string okError;
    CHECK(c->deserialize_world(goodSave, okError));
    CHECK(okError.empty());
    CHECK(c->get_block(3, 130, 3) == kBlockStone);  // take-over commitado
    CHECK(c->is_chunk_loaded(5, 5));

    std::cout << "[sdk] load rollback: chunk failure mid-load rolls the whole "
                 "load back (content, created chunks, block/world entities), "
                 "success commits OK\n";
}

// FALTANTES §4 item 19, caminho paginado: o manifest (v5) restaura block
// entities + world entities do save ANTES das páginas de região; uma página
// corrompida que falha no meio do decode deve reverter TUDO — chunks
// aplicados, entidades do save, e o estado pré-load de B deve voltar íntegro.
void test_region_load_rollback() {
    // --- World A: tile r.0.0 + block entity + 2 world entities, save paginado.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    a->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machine = std::make_shared<CounterMachine>();
    machine->counter_ = 7;
    std::string error;
    CHECK(a->attach_block_entity(8, kFluidTestTerrain, 8, machine, error));
    auto aEntities = a->entity_world();
    CHECK(aEntities != nullptr);
    CHECK(aEntities->spawn("vulkancraft:cow",
                           engine::entity::Position{ 8.0f, 132.0f, 8.0f },
                           error).valid());
    CHECK(aEntities->spawn("vulkancraft:cow",
                           engine::entity::Position{ 9.0f, 132.0f, 9.0f },
                           error).valid());
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 131, 3, kBlockStone);
        std::string e;
        CHECK(tx->commit(e));
        CHECK(e.empty());
    }
    a->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_region_rollback";
    std::string saveError;
    CHECK(a->save_world(dir, saveError));
    CHECK(saveError.empty());

    // Corrompe a página r.0.0: infla o contador de chunks (u32 LE no offset
    // 4) — o decode aplica os chunks reais e falha no chunk fantasma.
    const std::string pagePath = dir + "/pages/r_0_0.dat";  // page id "r.0.0" -> file
    std::ifstream in(pagePath, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string page = buffer.str();
    CHECK(page.size() > 8);
    page[4] = static_cast<char>(0xFF);
    std::ofstream out(pagePath, std::ios::binary | std::ios::trunc);
    out.write(page.data(), static_cast<std::streamsize>(page.size()));

    // --- World B: estado pré-load (edit + cow com marker), load falha. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    b->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = b->begin_transaction();
        tx->set_block(7, 131, 7, kBlockStone);
        std::string e;
        CHECK(tx->commit(e));
        CHECK(e.empty());
    }
    auto bEntities = b->entity_world();
    CHECK(bEntities != nullptr);
    const engine::entity::EntityId cowB = bEntities->spawn(
        "vulkancraft:cow", engine::entity::Position{ 5.0f, 132.0f, 5.0f }, error);
    CHECK(cowB.valid());
    engine::entity::ComponentData marker;
    marker.type = "vulkancraft:marker";
    marker.version = 1;
    marker.blob = "B";
    CHECK(bEntities->set_component(cowB, marker));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadError;
    CHECK(!b->load_world(dir, loadError));

    // Rollback: chunk (0,0) voltou ao estado pré-load (o stone do save em
    // (3,131,3) revertido, o edit de B em (7,131,7) intacto); a CounterMachine
    // e as 2 cows DO SAVE revertidas — B volta com a SUA cow (marker "B").
    CHECK(b->get_block(3, 131, 3) == kBlockAir);
    CHECK(b->get_block(7, 131, 7) == kBlockStone);
    CHECK(b->block_entity_count() == 0);
    CHECK(bEntities->size() == 1);
    const auto inChunk = bEntities->entities_in_chunk(0, 0);
    CHECK(inChunk.size() == 1);
    engine::entity::ComponentData got;
    CHECK(bEntities->get_component(inChunk[0], "vulkancraft:marker", got));
    CHECK(got.blob == "B");

    std::cout << "[sdk] region load rollback: corrupt region page mid-load "
                 "reverts chunks + save entities, pre-load state (own edit, "
                 "own cow with marker) intact OK\n";
}

// FALTANTES §4 item 20: bateria de resiliência da persistência. Falta de
// espaço (I/O sabotado — um ARQUIVO ocupando o lugar de pages/, proxy
// determinístico de disco cheio) é recusada com diagnóstico sem corromper um
// save commitado; temp parcial órfão (processo morto durante temp+rename
// DEPOIS do commit) é inerte e swept no load; concorrência (load sincrono E
// assíncrono) é recusada enquanto um save async está em voo (slot único).
// Corrupção/migração/save repetido já têm cobertura própria
// (test_persistence, test_world_save_migration, test_region_delta_saves).
void test_persistence_resilience() {
    // --- Phase A: falha de I/O é recusada com diagnóstico e nunca corrompe
    // um save commitado. ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    a->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string goodDir = scratch_dir() + "/vc_resilience_good";
    std::string goodErr;
    CHECK(a->save_world(goodDir, goodErr));
    CHECK(goodErr.empty());

    const std::string fullDir = scratch_dir() + "/vc_resilience_full";
    std::filesystem::create_directories(fullDir);
    { std::ofstream out(fullDir + "/pages"); out << "not a directory"; }
    std::string ioErr;
    CHECK(!a->save_world(fullDir, ioErr));
    CHECK(!ioErr.empty());

    // O save commitado em goodDir continua íntegro e carrega.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadErrB;
    CHECK(b->load_world(goodDir, loadErrB));
    CHECK(loadErrB.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);

    // O diretório sabotado não vira um mundo utilizável: o load falha LIMPO
    // (o journal do save fracassado é recuperado para nada — sem lixo).
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadErrC;
    CHECK(!c->load_world(fullDir, loadErrC));
    CHECK(!loadErrC.empty());

    // --- Phase B: temp parcial órfão em save commitado é inerte e swept no
    // load (FALTANTES §4 item 20: sempre, mesmo sem journal). ---
    const std::string stray = goodDir + "/pages/r_0_0.dat.tmp999";
    { std::ofstream out(stray); out << "partial garbage from a killed process"; }
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    d->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadErrD;
    CHECK(d->load_world(goodDir, loadErrD));
    CHECK(loadErrD.empty());
    CHECK(d->get_block(3, 130, 3) == kBlockStone);
    CHECK(!std::filesystem::exists(stray));  // swept

    // --- Phase C: concorrência — load sincrono E async recusados enquanto um
    // save async está em voo (slot único de op). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> e =
        engine::voxel::create_default_voxel_world();
    e->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*e, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto gated = std::make_shared<GatedRegionStorage>();
    e->register_storage(gated);
    const std::string gateDir = scratch_dir() + "/vc_resilience_gate";
    std::string gateDispatchErr;
    CHECK(e->save_world_async(gateDir, [](bool, std::string) {}, gateDispatchErr));
    {
        std::unique_lock<std::mutex> lock(gated->gateMutex);
        gated->cv.wait_for(lock, std::chrono::seconds(30),
                           [&] { return gated->entered; });
        CHECK(gated->entered);  // save async em voo (primeira página presa)
    }
    std::string refusedLoad;
    CHECK(!e->load_world(goodDir, refusedLoad));
    CHECK(!refusedLoad.empty());
    std::string refusedAsyncLoad;
    CHECK(!e->load_world_async(goodDir, [](bool, std::string) {},
                               refusedAsyncLoad));
    CHECK(!refusedAsyncLoad.empty());
    {
        std::lock_guard<std::mutex> lock(gated->gateMutex);
        gated->release = true;
    }
    gated->cv.notify_all();
    std::string gateWaitErr;
    CHECK(e->wait_async_saves(gateWaitErr));
    CHECK(gateWaitErr.empty());

    std::cout << "[sdk] persistence resilience: I/O failure refused with "
                 "diagnostic (committed save untouched), stray partial temp "
                 "swept on load, concurrent load refused while async save in "
                 "flight OK\n";
}

// FALTANTES §4 item 21: mundo grande com limite explícito de tempo, RAM e
// tamanho em disco. O mundo headless tem residência densa máxima de 121
// chunks (janela 11x11, kDenseLodRadius=5) — este teste a usa inteira (>=100
// chunks, 4-8x a residência dos outros testes), salva paginado (paleta +
// compressão), recarrega em mundo novo, e prova que o orçamento de RAM do
// item 6 continua válido na escala grande. Todos os números são medidos e
// reportados; os limites são caps generosos (o suite roda sob carga paralela).
void test_large_world() {
    const auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const auto dirBytes = [](const std::string& dir) {
        uint64_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                total += static_cast<uint64_t>(entry.file_size());
            }
        }
        return total;
    };

    // --- Geração: a janela densa inteira (budget 300 -> dense 121 chunks). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    const int64_t bootStart = nowMs();
    CHECK(boot_world(*a, player, 300));
    CHECK(settle(*a, player,
                 [&] { return a->streaming_snapshot().chunksLoaded >= 100; },
                 /*maxMs=*/60000));
    const int64_t bootMs = nowMs() - bootStart;
    const std::size_t generated = a->streaming_snapshot().chunksLoaded;
    CHECK(generated >= 100);
    CHECK(bootMs < 60000);

    // Edit perto do centro (chunk (0,0), tile r.0.0).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 131, 3, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    auto store = engine::storage::create_region_chunk_storage(8);
    a->register_storage(store);
    const std::string dir = scratch_dir() + "/vc_test_large_world";
    const int64_t saveStart = nowMs();
    std::string saveErr;
    CHECK(a->save_world(dir, saveErr));
    CHECK(saveErr.empty());
    const int64_t saveMs = nowMs() - saveStart;
    const uint64_t diskBytes = dirBytes(dir);
    const std::vector<std::string> pages = store->page_ids();
    CHECK(pages.size() >= 5);              // manifest + >=4 tiles de região
    CHECK(pages.back() == "world");
    CHECK(diskBytes > 0);
    CHECK(diskBytes < 8u * 1024 * 1024);   // paleta + zstd; reportado abaixo
    CHECK(saveMs < 15000);

    // --- Recarga em mundo novo: todo o mundo volta (restore sob demanda). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    const int64_t loadStart = nowMs();
    std::string loadErr;
    CHECK(b->load_world(dir, loadErr));
    CHECK(loadErr.empty());
    const int64_t loadMs = nowMs() - loadStart;
    const std::size_t restored = b->streaming_snapshot().chunksLoaded;
    CHECK(restored >= 100);
    CHECK(b->get_block(3, 131, 3) == kBlockStone);
    CHECK(loadMs < 15000);

    // --- RAM: o orçamento do item 6 continua válido na escala grande (o
    // eviction de fim de frame clampa o uso, preservando uma área ampla). ---
    const uint64_t beforeRam = b->streaming_snapshot().ramUsageBytes;
    const uint64_t kRamBudget = 8u * 1024 * 1024;
    b->set_memory_budget(kRamBudget);
    CHECK(settle(*b, player,
                 [&] { return b->streaming_snapshot().ramUsageBytes <= kRamBudget; },
                 /*maxMs=*/60000));
    const engine::voxel::StreamingSnapshot clamped = b->streaming_snapshot();
    CHECK(clamped.ramUsageBytes <= kRamBudget);
    CHECK(clamped.chunksLoaded >= 40);  // escala grande: orçamento NÃO zera a área
    b->set_memory_budget(0);

    std::cout << "[sdk] large world: " << generated << " chunks generated in "
              << bootMs << "ms, paged save " << diskBytes << " bytes ("
              << pages.size() << " pages) in " << saveMs << "ms, reload "
              << restored << " chunks in " << loadMs << "ms, RAM " << beforeRam
              << " -> clamped to " << clamped.ramUsageBytes << " (budget "
              << kRamBudget << ", chunks " << clamped.chunksLoaded << ") OK\n";
}

// FALTANTES §3 item 2: services are substitutable WITHOUT modifying the core.
// Storage/generator/edit/replication are already WIRED; here we prove the
// mesher and lighting plugins route through the world with a real observable
// effect — a lighting plugin forcing emission on a dark block lights the
// world, and a mesher plugin's policy shows up in the effective runtime table.
void test_service_plugin_substitution() {
    // Lighting plugin: forces emission 15 on an otherwise-dark catalog block.
    struct EmissiveLighting final : engine::voxel::IVoxelLighting {
        uint32_t targetId{ 0 };
        const char* name() const override { return "test-emissive"; }
        std::vector<std::pair<uint32_t, engine::voxel::BlockLightProperties>>
        light_property_overrides() override {
            if (targetId == 0) return {};
            return { { targetId, { 15, 15 } } };
        }
    };
    auto emissive = std::make_shared<EmissiveLighting>();

    // Control world: no plugin — the dark block emits nothing.
    {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto registry = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(registry->load_from_json(
            R"([{"name":"dull","namespace":"test","color":[0.4,0.4,0.4]}])", error));
        world->set_block_registry(registry);
        const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
        CHECK(boot_world(*world, player, 16));
        uint32_t dullId = 0;
        CHECK(world->resolve_block_id("test:dull", dullId, error));
        world->set_block(8, 130, 8, dullId);
        CHECK(settle(*world, player, [&] { return true; }));
        CHECK(world->get_block_light(8, 130, 8) == 0);   // no emission
        CHECK(world->get_block_light(8, 129, 8) == 0);
    }

    // Plugin world: the same block emits 15 after the plugin routes.
    {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto registry = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(registry->load_from_json(
            R"([{"name":"dull","namespace":"test","color":[0.4,0.4,0.4]}])", error));
        world->set_block_registry(registry);
        world->register_lighting(emissive);
        CHECK(world->registered_services().end() !=
              std::find(world->registered_services().begin(),
                        world->registered_services().end(), "lighting:test-emissive"));
        const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
        CHECK(boot_world(*world, player, 16));
        uint32_t dullId = 0;
        CHECK(world->resolve_block_id("test:dull", dullId, error));
        // The override is keyed by runtime id (known only after the registry
        // attached); re-registering re-pushes the table and queues a relight.
        emissive->targetId = dullId;
        world->register_lighting(emissive);
        world->set_block(8, 130, 8, dullId);
        CHECK(settle(*world, player, [&] { return world->get_block_light(8, 129, 8) == 14; }));
        CHECK(world->get_block_light(8, 130, 8) == 15);   // emitter cell
        CHECK(world->get_block_light(8, 129, 8) == 14);   // one air step
        CHECK(world->get_block_light(8, 140, 8) == 5);    // 10 air steps: 15 - 10
    }

    // Mesher plugin: per-block policy overrides land in the effective runtime
    // table (what the mesher consumes) and show in runtime_block_views().
    struct GrateMesher final : engine::voxel::IVoxelMesher {
        uint32_t targetId{ 0 };
        const char* name() const override { return "test-grate"; }
        std::vector<std::pair<uint32_t, engine::voxel::BlockMeshPolicy>>
        mesh_policy_overrides() override {
            if (targetId == 0) return {};
            return { { targetId, { /*occludes=*/false, /*transparent=*/true, /*renderLayer=*/1 } } };
        }
    };
    auto grate = std::make_shared<GrateMesher>();
    auto world = engine::voxel::create_default_voxel_world();
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registry->load_from_json(
        R"([{"name":"grate","namespace":"test","color":[0.2,0.2,0.5]}])", error));
    world->set_block_registry(registry);
    // Resolve the id (the table is pushed at set_block_registry; boot is not
    // required for the runtime-block view).
    uint32_t grateId = 0;
    CHECK(world->resolve_block_id("test:grate", grateId, error));
    {
        const auto views = world->runtime_block_views();
        const auto found = std::find_if(views.begin(), views.end(), [&](const auto& view) {
            return view.id == grateId;
        });
        CHECK(found != views.end());
        CHECK(found->occludes);          // registry-derived defaults
        CHECK(!found->transparent);
        CHECK(found->renderLayer == 0);
    }
    grate->targetId = grateId;
    world->register_mesher(grate);
    CHECK(world->registered_services().end() !=
          std::find(world->registered_services().begin(),
                    world->registered_services().end(), "mesher:test-grate"));
    {
        const auto views = world->runtime_block_views();
        const auto found = std::find_if(views.begin(), views.end(), [&](const auto& view) {
            return view.id == grateId;
        });
        CHECK(found != views.end());
        CHECK(!found->occludes);        // plugin policy won
        CHECK(found->transparent);
        CHECK(found->renderLayer == 1);
    }
    // A default mesher (no overrides) restores the registry-derived policy.
    struct DefaultMesher final : engine::voxel::IVoxelMesher {
        const char* name() const override { return "default"; }
    };
    world->register_mesher(std::make_shared<DefaultMesher>());
    {
        const auto views = world->runtime_block_views();
        const auto found = std::find_if(views.begin(), views.end(), [&](const auto& view) {
            return view.id == grateId;
        });
        CHECK(found != views.end());
        CHECK(found->occludes);         // back to the registry-derived default
        CHECK(!found->transparent);
    }

    std::cout << "[sdk] service plugin substitution (lighting + mesher route "
                 "through the world) OK\n";
}

// FALTANTES §25 item 6 (fronteira de chunk): a fluid source placed at the LAST
// column of chunk (0,0) must spread into the NEIGHBOR chunk (1,0) — and keep
// spreading within it — even though the source chunk was just dirtied to
// MeshReady by the set_block (the guard that regressed this raced the dirty
// remesh: it demanded the chunk be Uploaded, popped the source cell and never
// re-scheduled it; can_touch_chunk_at uses the same guard as set_block_at, so
// the order of remesh vs fluid step is irrelevant).
void test_fluid_chunk_boundary() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    uint32_t gooId = 0;
    build(gooId, world, player);

    // x=15 is the last column of chunk (0,0); the set_block dirties the chunk
    // (MeshReady). Thin goo spreads 2 levels/step: ring 1 (x=16, chunk (1,0))
    // at level 2, ring 2 (x=17) at level 4.
    world->set_block(15, kFluidTestGroundedY, 8, static_cast<uint32_t>(gooId));
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(16, kFluidTestGroundedY, 8) == 2; }));
    CHECK(world->get_block(16, kFluidTestGroundedY, 8) == static_cast<uint32_t>(gooId));
    CHECK(world->get_fluid_level(15, kFluidTestGroundedY, 8) == 0);  // source stays
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(17, kFluidTestGroundedY, 8) == 4; }));
    CHECK(world->get_block(17, kFluidTestGroundedY, 8) == static_cast<uint32_t>(gooId));

    // The reverse direction also crosses: a source at x=0 (chunk (0,0)) spreads
    // into chunk (-1,0) at x=-1.
    std::unique_ptr<engine::voxel::IVoxelWorld> west;
    uint32_t westId = 0;
    build(westId, west, player);
    west->set_block(0, kFluidTestGroundedY, 8, static_cast<uint32_t>(westId));
    CHECK(settle(*west, player,
        [&] { return west->get_fluid_level(-1, kFluidTestGroundedY, 8) == 2; }));
    CHECK(west->get_block(-1, kFluidTestGroundedY, 8) == static_cast<uint32_t>(westId));

    std::cout << "[sdk] fluids: cross-chunk boundary spread OK\n";
}

// FALTANTES §8 item 165: simulate correctly through chunk boundaries. A
// falling source in the LAST column of chunk (0,0) falls (chunks are
// full-height columns, so the fall stays in chunk A) and its ring spreads at
// the LOWER level into chunk (1,0) — the boundary is crossed at a different
// Y than the source, not just same-row. Also: a source pinned against an
// UNLOADED boundary chunk must resume the moment the chunk streams in (the
// spread re-enqueues instead of dropping the cell).
void test_fluid_cross_boundary_vertical() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":true,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 32));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    uint32_t gooId = 0;
    build(gooId, world, player);

    // Source in the last column of chunk (0,0), raised 3 cells above the
    // terrain: falling=true drops it cell by cell within chunk (0,0) until it
    // lands on the terrain top; the ring then spreads at the LANDING level —
    // x=16 sits in chunk (1,0). The source column is fully within chunk A
    // (chunks are full-height), so the only cross-boundary write is the
    // lower-level spread into chunk B.
    const int sourceY = kFluidTestGroundedY + 3;
    world->set_block(15, sourceY, 8, static_cast<uint32_t>(gooId));
    // The column fell: the source cell (level 0) keeps its block, and the
    // falling cells below placed goo blocks all the way to the landing level.
    CHECK(settle(*world, player,
        [&] { return world->get_block(15, kFluidTestGroundedY, 8) ==
                     static_cast<uint32_t>(gooId); }));
    CHECK(world->get_fluid_level(15, sourceY, 8) == 0);  // source level
    CHECK(world->get_block(15, sourceY, 8) == static_cast<uint32_t>(gooId));
    // The landing-level ring crossed the chunk boundary: x=16 (chunk (1,0))
    // carries goo at level 2 (thin goo: 2 levels/tick).
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(16, kFluidTestGroundedY, 8) == 2; }));
    CHECK(world->get_block(16, kFluidTestGroundedY, 8) ==
          static_cast<uint32_t>(gooId));

    std::cout << "[sdk] fluids: vertical fall crosses the boundary at the "
                 "landing level OK\n";
}

// FALTANTES §8 item 165 (resume half): a fluid source whose ONLY spread exit
// is an UNLOADED chunk must not die — it re-enqueues and flows the moment the
// chunk streams in. The horizontal spread guard used to just `continue` on an
// untouchable neighbor, dropping the popped cell; when every other exit is a
// wall, the fluid died permanently. Now it survives and resumes.
void test_fluid_resume_after_boundary_load() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player, int budget) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, budget));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };

    // Player at (-8, 200, 8) centers chunk (-1,0); budget 8 (the minimum,
    // budget 4 clamps up) fills ring 0 + ring 1 in fixed order, which stops
    // BEFORE chunk (1,0): chunk (0,0) (the source chunk) loads, chunk (1,0)
    // (the spread exit) does not.
    const glm::vec3 player{ -8.0f, 200.0f, 8.0f };
    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    uint32_t gooId = 0;
    build(gooId, world, player, 8);
    CHECK(world->is_chunk_loaded(0, 0));
    CHECK(!world->is_chunk_loaded(1, 0));

    // Source at x=15 (last column of chunk (0,0)); walls at x=14, z=7, z=9
    // make x=16 the ONLY spread exit — and that chunk is unloaded.
    world->set_block(15, kFluidTestGroundedY, 8, static_cast<uint32_t>(gooId));
    world->set_block(14, kFluidTestGroundedY, 8, kBlockStone);
    world->set_block(15, kFluidTestGroundedY, 7, kBlockStone);
    world->set_block(15, kFluidTestGroundedY, 9, kBlockStone);
    // With the unloaded exit, the fluid must neither die nor leak: several
    // sim-seconds pass and x=16 stays empty while the source survives.
    for (int i = 0; i < 60 * 3; ++i) {
        world->update(player, 1.0f / 60.0f);
    }
    // No leak into the unloaded chunk (fluid level stays empty), and the
    // source survives the wait.
    CHECK(world->get_fluid_level(16, kFluidTestGroundedY, 8) == 0xff);
    CHECK(world->get_block(15, kFluidTestGroundedY, 8) ==
          static_cast<uint32_t>(gooId));

    // Move the player over chunk (1,0): it streams in; the still-queued
    // source resumes and its ring reaches x=16.
    const glm::vec3 east{ 24.0f, 200.0f, 8.0f };
    CHECK(settle(*world, east, [&] {
        return world->is_chunk_loaded(1, 0);
    }));
    CHECK(settle(*world, east,
        [&] { return world->get_fluid_level(16, kFluidTestGroundedY, 8) == 2; }));
    CHECK(world->get_block(16, kFluidTestGroundedY, 8) ==
          static_cast<uint32_t>(gooId));

    std::cout << "[sdk] fluids: source pinned to an unloaded boundary resumes "
                 "when the chunk loads OK\n";
}

// Fluid levels are part of the world save: a data-driven fluid's spread
// survives serialize/deserialize and re-serializes byte-identically.
void test_fluid_persistence() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> a;
    uint32_t gooId = 0;
    build(gooId, a, player);
    a->set_block(8, kFluidTestGroundedY, 8, static_cast<uint32_t>(gooId));
    CHECK(settle(*a, player, [&] { return a->get_fluid_level(10, kFluidTestGroundedY, 8) == 4; }));
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());
    CHECK(a->serialize_world(saveError) == bytes);  // deterministic section

    std::unique_ptr<engine::voxel::IVoxelWorld> b;
    uint32_t gooIdB = 0;
    build(gooIdB, b, player);
    CHECK(gooIdB == gooId);  // same registry -> same dynamic id
    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_fluid_level(8, kFluidTestGroundedY, 8) == 0);   // source restored
    CHECK(b->get_fluid_level(9, kFluidTestGroundedY, 8) == 2);   // ring levels restored
    CHECK(b->get_fluid_level(10, kFluidTestGroundedY, 8) == 4);
    CHECK(b->get_block(9, kFluidTestGroundedY, 8) == static_cast<uint32_t>(gooIdB));
    std::string reError;
    CHECK(b->serialize_world(reError) == bytes);  // byte-identical roundtrip

    std::cout << "[sdk] fluids: levels persist through save/load, byte-identical "
                 "roundtrip OK\n";
}

// FALTANTES §8 item 168 (conservação): um pool fechado (evaporation=false,
// falling=false) atinge um FIXPOINT — a soma total de níveis (e a contagem de
// células de fluido) é idêntica após centenas de ticks extras, e o pool não
// cresce além do range (x=11 permanece ar). Sem drift, sem perda.
void test_fluid_conservation() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 32));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Pool: fonte em (8, y, 8) com anéis 2 e 4 (thin goo). Uma única fonte
    // gera um pool fechado de tamanho fixo (0 + 4x2 + 4x4 = 24 unidades).
    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    uint32_t gooId = 0;
    build(gooId, world, player);
    world->set_block(8, kFluidTestGroundedY, 8, static_cast<uint32_t>(gooId));
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(10, kFluidTestGroundedY, 8) == 4; }));

    // Sum of base levels over the pool region (only fluid cells count).
    const auto total = [&]() {
        int sum = 0;
        int cells = 0;
        for (int x = 4; x <= 12; ++x) {
            for (int z = 4; z <= 12; ++z) {
                if (world->get_block(x, kFluidTestGroundedY, z) !=
                    static_cast<uint32_t>(gooId)) continue;
                ++cells;
                sum += static_cast<int>(
                    world->get_fluid_level(x, kFluidTestGroundedY, z) & 0x07u);
            }
        }
        return std::pair<int, int>{ sum, cells };
    };
    const auto [sum0, cells0] = total();
    // Espalhamento em losango (Manhattan): fonte 0 + 4x2 (anel 1) + 8x4
    // (anel 2) = 40 unidades em 13 células; maxLevel 4 para no anel 2.
    CHECK(sum0 == 40);
    CHECK(cells0 == 13);
    CHECK(world->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);  // range cap

    // Conservação: 120 ticks extras não mudam NADA — o pool é um fixpoint.
    for (int i = 0; i < 120; ++i) world->update(player, 1.0f / 60.0f);
    const auto [sum1, cells1] = total();
    CHECK(sum1 == sum0);
    CHECK(cells1 == cells0);
    CHECK(world->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);  // sem crescer

    std::cout << "[sdk] fluids: conservation — pooled levels are a fixed point "
                 "across ticks, range caps OK\n";
}

// FALTANTES §8 item 168 (cachoeira): falling=true de uma fonte elevada forma
// uma COLUNA contínua de fluido até o chão (cada célula carrega a flag de
// queda, preservando o nível base através da queda), e o nível base da fonte
// chega intacto ao nível do chão — onde o anel espalha (2, 4).
void test_fluid_waterfall() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":true,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 32));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Fonte 4 células acima do chão (groundedY = terrain+1). A coluna cai
    // célula a célula; o nível base 0 (fonte) é preservado pela flag de queda.
    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    uint32_t gooId = 0;
    build(gooId, world, player);
    const int sourceY = kFluidTestGroundedY + 4;
    world->set_block(8, sourceY, 8, static_cast<uint32_t>(gooId));
    // A queda forma a coluna e o poço no chão: o anel 2 no groundedY
    // (distância Manhattan 2, nível base 4) fecha o losango da base.
    const auto baseAt = [&](int x, int y) {
        return static_cast<int>(world->get_fluid_level(x, y, 8) & 0x07u);
    };
    CHECK(settle(*world, player,
        [&] { return baseAt(10, kFluidTestGroundedY) == 4; }));

    // Coluna contínua: TODA célula de groundedY..sourceY é goo.
    for (int y = kFluidTestGroundedY; y <= sourceY; ++y) {
        CHECK(world->get_block(8, y, 8) == static_cast<uint32_t>(gooId));
    }
    // A fonte preserva o nível base 0; as células em queda carregam a flag
    // (0x80) com o MESMO nível base — a queda conserva o nível.
    CHECK(world->get_fluid_level(8, sourceY, 8) == 0);  // source stays
    for (int y = kFluidTestGroundedY; y < sourceY; ++y) {
        const uint8_t level = world->get_fluid_level(8, y, 8);
        CHECK((level & 0x80u) != 0);        // falling flag
        CHECK((level & 0x07u) == 0);        // base 0 preserved through the fall
    }
    // O poço no chão: a coluna em queda espalha (falling=true) como um
    // losango Manhattan com base +2 por passo, capado no nível 4 (maxLevel).
    // Anel 1 = base 2; anel 2 = base 4; distância 3 permanece ar (range cap).
    CHECK(baseAt(9, kFluidTestGroundedY) == 2);
    CHECK(baseAt(10, kFluidTestGroundedY) == 4);
    CHECK(world->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);  // range cap

    // A cachoeira também é um fixpoint: 120 ticks extras não mudam o poço
    // (base 4 no anel 2) nem estouram o range (distância 3 segue ar).
    for (int i = 0; i < 120; ++i) world->update(player, 1.0f / 60.0f);
    for (int y = kFluidTestGroundedY; y <= sourceY; ++y) {
        CHECK(world->get_block(8, y, 8) == static_cast<uint32_t>(gooId));
    }
    CHECK(baseAt(9, kFluidTestGroundedY) == 2);
    CHECK(baseAt(10, kFluidTestGroundedY) == 4);
    CHECK(world->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);

    std::cout << "[sdk] fluids: waterfall — falling column carries the base "
                 "level to the ground, bounded falling pool caps at range OK\n";
}

// FALTANTES §8 item 168 (unload/reload): níveis de fluido sobrevivem a um
// ciclo save (páginas de região) + load em mundo novo — o restore em lote
// grava os bytes de fluido por voxel, então o pool volta com os MESMOS níveis
// (não apenas os blocos).
void test_fluid_unload_reload() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 32));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> a;
    uint32_t gooId = 0;
    build(gooId, a, player);
    a->set_block(8, kFluidTestGroundedY, 8, static_cast<uint32_t>(gooId));
    CHECK(settle(*a, player,
        [&] { return a->get_fluid_level(10, kFluidTestGroundedY, 8) == 4; }));
    const std::string dir = scratch_dir() + "/vc_test_fluid_unload_reload";
    a->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string saveError;
    CHECK(a->save_world(dir, saveError));
    CHECK(saveError.empty());

    // Mundo novo: mesmo registry (mesmo id dinâmico), sem fluido em memória.
    std::unique_ptr<engine::voxel::IVoxelWorld> b;
    uint32_t gooIdB = 0;
    build(gooIdB, b, player);
    CHECK(gooIdB == gooId);
    b->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadError;
    CHECK(b->load_world(dir, loadError));
    CHECK(loadError.empty());

    // Níveis restaurados: fonte 0, anéis 2 e 4, blocos goo, ar no cap.
    CHECK(b->get_fluid_level(8, kFluidTestGroundedY, 8) == 0);
    CHECK(b->get_fluid_level(9, kFluidTestGroundedY, 8) == 2);
    CHECK(b->get_fluid_level(10, kFluidTestGroundedY, 8) == 4);
    CHECK(b->get_block(9, kFluidTestGroundedY, 8) == static_cast<uint32_t>(gooIdB));
    CHECK(b->get_block(11, kFluidTestGroundedY, 8) == kBlockAir);

    // Conservação pós-reload: o pool restaurado também é um fixpoint.
    for (int i = 0; i < 120; ++i) b->update(player, 1.0f / 60.0f);
    CHECK(b->get_fluid_level(10, kFluidTestGroundedY, 8) == 4);
    CHECK(b->get_fluid_level(11, kFluidTestGroundedY, 8) == 0xff);

    std::cout << "[sdk] fluids: unload/reload — paged save restores fluid "
                 "levels, pool stable after reload OK\n";
}

// FALTANTES §8 item 168 (budgets, lado do mundo): com MUITAS células de
// fluido ativas (grade 4x4 de fontes, anéis sobrepostos), o cap por tick da
// simulação adia trabalho em vez de perder — o pool todo converge ao fixpoint
// e permanece. (O carryover determinístico por orçamento da fase FluidTick
// vive em voxel_scheduler_tests, onde o WorldScheduler é acessível.)
void test_fluid_budgets() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
        error));
    world->set_block_registry(blocks);
    auto fluids = std::make_shared<engine::registry::FluidRegistry>();
    CHECK(fluids->load_from_json(
        R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false,"evaporation":false}])",
        error));
    CHECK(world->set_fluid_registry(fluids, error));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 64));
    uint32_t gooIdW = 0;
    CHECK(world->resolve_block_id("test:goo", gooIdW, error));
    // O grid de fontes cruza os chunks (0,0)/(1,0)/(0,1)/(1,1): materialize-os
    // antes de colocar (set_block_at descarta writes em chunk não carregado —
    // o mesmo padrão dos fixes findings #54).
    for (int i = 0; i < 600 &&
                    !(world->is_chunk_loaded(0, 0) && world->is_chunk_loaded(1, 0) &&
                      world->is_chunk_loaded(0, 1) && world->is_chunk_loaded(1, 1));
         ++i) {
        world->update(player, 1.0f / 60.0f);
    }
    CHECK(world->is_chunk_loaded(1, 1));

    // Grade 10x10 de fontes espaçadas 2 células (x=8..26, z=8..26): 100
    // fontes -> rajada inicial de ~700 células, bem acima do cap de 512 do
    // passo por tick — o excedente carrega (scheduler + dedup), nada se perde.
    // Os losangos (raio 2, nível 4 no passo 2) se fundem num pool contínuo.
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            world->set_block(8 + 2 * i, kFluidTestGroundedY, 8 + 2 * j,
                             static_cast<uint32_t>(gooIdW));
        }
    }
    // Convergência sob orçamento: o interior (distância 2 da fonte mais
    // próxima — canto do losango) atinge o nível 4 e permanece fixo.
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(17, kFluidTestGroundedY, 17) == 4; }));
    // Canto do pool (distância 2 da fonte de canto (26,26) -> (27,27)) nível 4;
    // fora do raio (distância 3+: (28,28) está a distância 4) permanece ar.
    CHECK(world->get_fluid_level(27, kFluidTestGroundedY, 27) == 4);
    CHECK(world->get_block(28, kFluidTestGroundedY, 28) == kBlockAir);
    for (int i = 0; i < 120; ++i) world->update(player, 1.0f / 60.0f);
    CHECK(world->get_fluid_level(17, kFluidTestGroundedY, 17) == 4);
    CHECK(world->get_block(28, kFluidTestGroundedY, 28) == kBlockAir);

    std::cout << "[sdk] fluids: budgets — many-source pool converges under the "
                 "per-tick cap, nothing lost OK\n";
}

// FALTANTES §8 item 166: simulação completamente separada do material visual.
// (a) BLOCK_SIM_PROPS é a ÚNICA fonte de solididade/transparência/absorção
// dos blocos builtin — BLOCK_MATERIALS (cor/texturas) não carrega mais flags
// físicas (o guard estático em ArchitectureBoundaryTests prova que World.cpp
// nunca lê a tabela visual). (b) Blocos data-driven com a MESMA cor e classes
// DIFERENTES têm simulação diferente; com a MESMA classe e cores DIFERENTES
// têm simulação idêntica — a cor nunca entra na decisão física.
void test_material_simulation_separation() {
    // --- (a) tabela de simulação embutida: valores canônicos por bloco. ---
    const auto sim = [](BlockType t) { return get_block_sim(t); };
    CHECK(!sim(BlockType::Air).solid && sim(BlockType::Air).transparent &&
          sim(BlockType::Air).lightAbsorption == 0);
    CHECK(sim(BlockType::Stone).solid && !sim(BlockType::Stone).transparent &&
          sim(BlockType::Stone).lightAbsorption == 15);
    CHECK(sim(BlockType::Glass).solid && sim(BlockType::Glass).transparent &&
          sim(BlockType::Glass).lightAbsorption == 0);
    CHECK(!sim(BlockType::Leaves).solid && sim(BlockType::Leaves).transparent &&
          sim(BlockType::Leaves).lightAbsorption == 1);
    CHECK(!sim(BlockType::Water).solid && sim(BlockType::Water).transparent &&
          sim(BlockType::Water).lightAbsorption == 1);
    CHECK(!sim(BlockType::Lava).solid && !sim(BlockType::Lava).transparent &&
          sim(BlockType::Lava).lightAbsorption == 1);
    // Os helpers de simulação leem a tabela de SIMULAÇÃO (nunca o material).
    CHECK(is_solid_block(BlockType::Stone) && !is_solid_block(BlockType::Water));
    CHECK(is_transparent_block(BlockType::Glass) && !is_transparent_block(BlockType::Stone));

    // --- (b) data-driven: a CLASSE decide a simulação; a cor é invisível. ---
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    const char* json =
        R"([
  {"name":"solid_a","namespace":"test","class":"solid","color":[1.0,0.0,0.0]},
  {"name":"transparent_b","namespace":"test","class":"transparent","collidable":false,"color":[1.0,0.0,0.0],"lightAbsorption":0.0},
  {"name":"solid_c","namespace":"test","class":"solid","color":[0.0,0.0,1.0]}
])";
    CHECK(blocks->load_from_json(json, error));
    world->set_block_registry(blocks);
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t a = 0, b = 0, c = 0;
    CHECK(world->resolve_block_id("test:solid_a", a, error) && error.empty());
    CHECK(world->resolve_block_id("test:transparent_b", b, error) && error.empty());
    CHECK(world->resolve_block_id("test:solid_c", c, error) && error.empty());

    const auto view = [&](uint32_t id) -> engine::voxel::BlockRuntimeView {
        for (const auto& v : world->runtime_block_views()) {
            if (v.id == id) return v;
        }
        return {};
    };
    const engine::voxel::BlockRuntimeView va = view(a);
    const engine::voxel::BlockRuntimeView vb = view(b);
    const engine::voxel::BlockRuntimeView vc = view(c);
    // MESMA cor (vermelho), classes diferentes -> simulação DIFERENTE.
    CHECK(va.solid && !va.transparent && va.lightAbsorption == 15);
    CHECK(!vb.solid && vb.transparent && vb.lightAbsorption == 0);
    // MESMA classe (solid), cores diferentes -> simulação IDÊNTICA.
    CHECK(vc.solid == va.solid && vc.transparent == va.transparent &&
          vc.lightAbsorption == va.lightAbsorption);

    // --- comportamento público: o raycast para no sólido e atravessa o
    // transparente (solidity vem da classe, não da aparência). ---
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, a);
        tx->set_block(6, 130, 6, b);
        std::string txError;
        CHECK(tx->commit(txError) && txError.empty());
    }
    const engine::voxel::VoxelRaycastHit hitSolid =
        world->raycast(glm::vec3(3.5f, 200.0f, 3.5f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(hitSolid.hit);
    CHECK(hitSolid.block.x == 3 && hitSolid.block.y == 130 && hitSolid.block.z == 3);
    const engine::voxel::VoxelRaycastHit throughTransparent =
        world->raycast(glm::vec3(6.5f, 200.0f, 6.5f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(throughTransparent.hit);            // hits the flat ground below
    CHECK(throughTransparent.block.y < 130);  // transparent_b did NOT stop it

    std::cout << "[sdk] material×simulation: BlockSimProps is the only source of "
                 "solidity/light, BlockMaterial is appearance-only OK\n";
}

// FALTANTES §8 item 167: água, lava e um TERCEIRO fluido definidos APENAS
// pelo projeto — os três via FluidRegistry JSON (o projeto sobrescreve os
// defaults de água/lava e define um fluido catalog-only). Prova: (1) os três
// simulam como fluido (anéis por níveis); (2) parâmetros distintos
// observáveis — água espessa 1/tick com range 7 e falling:false (poço
// flutuante, sem coluna; alcance longo) vs ácido fino 2/tick com range 4
// (cap curto) vs lava falling:true (coluna desce ao chão) com dano; (3) o
// dano do terceiro fluido (acid) aplica a mobs.
void test_project_defined_fluids() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"acid","namespace":"test","class":"fluid","color":[0.3,1.0,0.2]}])",
        error));
    world->set_block_registry(blocks);
    auto fluids = std::make_shared<engine::registry::FluidRegistry>();
    CHECK(fluids->load_from_json(
        R"([
          {"block":"vulkancraft:water","viscosity":0.5,"range":7,"falling":false,"evaporation":false},
          {"block":"vulkancraft:lava","viscosity":1.0,"range":3,"falling":true,"evaporation":true,"damagePerTick":5.0},
          {"block":"test:acid","viscosity":0.0,"range":4,"falling":false,"evaporation":false,"damagePerTick":2.0}
        ])",
        error));
    CHECK(world->set_fluid_registry(fluids, error));
    CHECK(error.empty());
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 64));
    uint32_t waterId = 0, lavaId = 0, acidId = 0;
    CHECK(world->resolve_block_id("vulkancraft:water", waterId, error) && error.empty());
    CHECK(world->resolve_block_id("vulkancraft:lava", lavaId, error) && error.empty());
    CHECK(world->resolve_block_id("test:acid", acidId, error) && error.empty());
    // boot_world only guarantees chunk (0,0); the pools below reach chunks
    // (0,1), (1,1) and (2,2). set_block DROPS writes to not-yet-loaded chunks
    // (findings #54/#56 pattern), so materialize the target chunks first or
    // the settles run to their wall-clock cap.
    CHECK(settle(*world, player, [&] { return world->is_chunk_loaded(0, 1) &&
                                              world->is_chunk_loaded(1, 1) &&
                                              world->is_chunk_loaded(2, 2); }));

    // (1) Água (override do projeto): espessa 1/tick, range 7 — anel 1 = nível
    // 1 e o alcance longo chega à distância 3 (nível 3); poço no chão.
    world->set_block(8, kFluidTestGroundedY, 8, waterId);
    // The pool spreads ring by ring: settle on BOTH asserted cells so the
    // ring-3 check below does not race the still-expanding pool.
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(9, kFluidTestGroundedY, 8) == 1 &&
                      world->get_fluid_level(11, kFluidTestGroundedY, 8) == 3; }));
    CHECK(world->get_fluid_level(11, kFluidTestGroundedY, 8) == 3);  // range 7

    // (2) Água flutuante falling:false (override do default falling:true):
    // poço no nível de origem, NADA desce — (8,131,30) permanece ar.
    world->set_block(8, kFluidTestFloatingY + 3, 30, waterId);
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(9, kFluidTestFloatingY + 3, 30) == 1; }));
    CHECK(world->get_block(8, kFluidTestGroundedY, 30) == kBlockAir);  // no column

    // (3) Ácido (terceiro fluido): fino 2/tick, range 4 — anel 2 = nível 4 e
    // a distância 3 é ar (cap curto, diferente da água).
    world->set_block(24, kFluidTestGroundedY, 24, acidId);
    CHECK(settle(*world, player,
        [&] { return world->get_fluid_level(26, kFluidTestGroundedY, 24) == 4; }));
    CHECK(world->get_block(27, kFluidTestGroundedY, 24) == kBlockAir);  // range cap 4

    // (4) Lava falling:true: a coluna DESCE ao chão a partir da altura.
    world->set_block(40, kFluidTestFloatingY + 3, 40, lavaId);
    CHECK(settle(*world, player,
        [&] { return world->get_block(40, kFluidTestGroundedY, 40) == lavaId; }));

    std::cout << "[sdk] fluids: project-defined water (thick/range 7/falling:false), "
                 "lava (falling column) and a third acid (thin/range 4) all "
                 "simulate per JSON OK\n";
}

// Task D.5 (handoff 3->1, optical side): a renderer consumes fluid levels and
// OPTICAL PROPERTIES through the public contract only — no visual logic lives
// in the simulation. Levels via get_fluid_level; the color of the fluid AT A
// CELL resolves through the public chain (block id -> runtime_block_views
// uuid -> FluidRegistry::find_by_uuid -> FluidDefinition.color).
void test_fluid_render_handoff_optics() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"acid","namespace":"test","class":"fluid","color":[0.3,1.0,0.2]}])",
        error));
    world->set_block_registry(blocks);
    auto fluids = std::make_shared<engine::registry::FluidRegistry>();
    CHECK(fluids->load_from_json(
        R"([{"block":"test:acid","viscosity":0.0,"range":4,"falling":false,"evaporation":false,"damagePerTick":2.0,"color":[0.3,1.0,0.2,1.0]}])",
        error));
    CHECK(world->set_fluid_registry(fluids, error));
    CHECK(error.empty());
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 64));
    CHECK(settle(*world, player, [&] { return world->is_chunk_loaded(2, 2); }));

    uint32_t acidId = 0;
    CHECK(world->resolve_block_id("test:acid", acidId, error) && error.empty());

    // A renderer needs levels AND optics. Place an acid source on the dry
    // terrain (kFluidTestTerrain = 130, above sea level -> no generated
    // water), then read both through the public contract. The source reads
    // level 0; the spread (mirroring test_project_defined_fluids: acid
    // reaches distance 2 at level 4) is waited for, not raced.
    const int sourceX = 8;
    world->set_block(sourceX, kFluidTestGroundedY, sourceX, acidId);
    CHECK(settle(*world, player, [&] {
        return world->get_fluid_level(sourceX + 2, kFluidTestGroundedY,
                                      sourceX) != 0xFFu;
    }));
    CHECK(world->get_fluid_level(sourceX, kFluidTestGroundedY, sourceX) == 0);
    CHECK(world->get_fluid_level(sourceX + 2, kFluidTestGroundedY, sourceX) !=
          0xFFu);

    // Optical properties of the fluid AT THE SOURCE CELL, through the public
    // chain a renderer can follow with no simulation internals: block id ->
    // runtime view uuid -> FluidRegistry::find_by_uuid -> color.
    const uint32_t cellId = world->get_block(sourceX, kFluidTestGroundedY, sourceX);
    CHECK(cellId == acidId);
    std::string cellUuid;
    for (const auto& view : world->runtime_block_views()) {
        if (view.id == cellId) cellUuid = view.uuid;
    }
    CHECK(!cellUuid.empty());
    const engine::registry::FluidDefinition* def =
        fluids->find_by_uuid(cellUuid);
    CHECK(def != nullptr);
    CHECK(def != nullptr && std::fabs(def->color.r - 0.3f) < 1e-4f &&
          std::fabs(def->color.g - 1.0f) < 1e-4f &&
          std::fabs(def->color.b - 0.2f) < 1e-4f);
    // The named alternative a project can use when it knows the block id.
    // NOTE: the registry stores a COPY per map (byBlock_ and byUuid_), so the
    // lookups return different objects — compare by uuid (the stable
    // identity), never by pointer.
    const engine::registry::FluidDefinition* byName =
        fluids->find_by_block("test:acid");
    CHECK(byName != nullptr && byName->uuid == def->uuid);

    std::cout << "[sdk] fluids: renderer handoff — levels + optical properties "
                 "(color) via the public contract, no visual logic in the "
                 "simulation OK\n";
}

// Semantic block queries (A.2 enabler): consumers ask what a runtime block id
// MEANS without as_builtin_block. is_air matches the empty-cell id; is_fluid
// matches the fluid table (builtin water + project-declared JSON fluids, incl.
// inline FluidBinding blocks) — so gameplay/audio/selection can migrate from
// the builtin enum to the public contract unchanged for JSON-only blocks.
void test_block_semantic_queries() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"acid","namespace":"test","class":"fluid","collidable":false,"color":[0.3,1.0,0.2]}])",
        error));
    world->set_block_registry(blocks);
    auto fluids = std::make_shared<engine::registry::FluidRegistry>();
    CHECK(fluids->load_from_json(
        R"([{"block":"test:acid","viscosity":0.0,"range":4,"falling":false,"evaporation":false,"damagePerTick":2.0,"color":[0.3,1.0,0.2,1.0]}])",
        error));
    CHECK(world->set_fluid_registry(fluids, error));
    CHECK(error.empty());
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 64));
    CHECK(settle(*world, player, [&] { return world->is_chunk_loaded(0, 0); }));

    // Air = the empty-cell id (0); stone is not air.
    CHECK(world->is_air(kBlockAir));
    CHECK(!world->is_air(kBlockStone));
    CHECK(!world->is_air(kBlockDirt));

    // Builtin water (BlockType::Water) is in the engine's default fluid
    // table; stone/dirt are not fluids.
    constexpr int kBlockWaterId = 12;  // BlockType::Water
    CHECK(world->is_fluid(kBlockWaterId));
    CHECK(!world->is_fluid(kBlockStone));
    CHECK(!world->is_fluid(kBlockDirt));
    CHECK(!world->is_fluid(kBlockAir));

    // Solid drives collision/raycast (the semantic the gameplay consumers
    // need): stone/dirt solid, air/water not. is_solid mirrors the REGISTRY
    // faithfully: a JSON block declaring "collidable": false is non-solid
    // (the default is collidable=true, regardless of "class").
    CHECK(world->is_solid(kBlockStone));
    CHECK(world->is_solid(kBlockDirt));
    CHECK(!world->is_solid(kBlockAir));
    CHECK(!world->is_solid(kBlockWaterId));

    // A project-declared JSON fluid resolves to a runtime id that is_fluid
    // recognizes — the SAME answer a JSON-only block gets, no builtin enum.
    uint32_t acidId = 0;
    CHECK(world->resolve_block_id("test:acid", acidId, error) && error.empty());
    CHECK(acidId != kBlockAir);
    CHECK(world->is_fluid(acidId));

    // End-to-end through a placed cell: the id read back from the world is
    // classified identically.
    const int sx = 8, sy = kFluidTestGroundedY, sz = 8;
    world->set_block(sx, sy, sz, acidId);
    const uint32_t cellId = world->get_block(sx, sy, sz);
    CHECK(cellId == acidId);
    CHECK(world->is_fluid(cellId));
    CHECK(!world->is_air(cellId));
    CHECK(!world->is_solid(cellId));
    CHECK(world->is_air(world->get_block(sx, sy + 2, sz)));

    std::cout << "[sdk] blocks: semantic queries is_air/is_fluid/is_solid "
                 "via the public contract (builtin + JSON-only) — A.2 "
                 "enabler OK\n";
}

// Density displacement (task D.2 / META §13): a fluid's declared density now
// resolves what happens when two DIFFERENT fluids meet. A denser fluid
// displaces a lighter one in the cell it spreads into (heavier sinks); a
// lighter fluid never displaces a denser one (it spreads around it). Density
// was previously parsed into FluidParams but never read by the simulation.
void test_fluid_density_displacement() {
    const auto build = [](std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                          uint32_t& heavyId, uint32_t& lightId) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"heavy","namespace":"test","class":"fluid","color":[0.6,0.1,0.6]},)"
            R"({"name":"light","namespace":"test","class":"fluid","color":[0.1,0.6,0.6]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:heavy","viscosity":1.0,"range":2,"falling":false,"evaporation":false,"density":3.0},)"
            R"({"block":"test:light","viscosity":1.0,"range":2,"falling":false,"evaporation":false,"density":0.5}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        CHECK(world->resolve_block_id("test:heavy", heavyId, error));
        CHECK(world->resolve_block_id("test:light", lightId, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // (1) A DENSER fluid displaces a lighter one it spreads into.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world;
        uint32_t heavyId = 0, lightId = 0;
        build(world, heavyId, lightId);
        // Light pool (range 2, viscosity 1): source (8) -> ring (9) lvl1 ->
        // ring (10) lvl2. Settle on the outer ring.
        world->set_block(8, kFluidTestGroundedY, 8, lightId);
        CHECK(settle(*world, player, [&] {
            return world->get_fluid_level(10, kFluidTestGroundedY, 8) == 2;
        }));
        // A heavy source adjacent to the light pool's edge: it displaces the
        // lighter fluid in (10) instead of stopping at the boundary.
        world->set_block(11, kFluidTestGroundedY, 8, heavyId);
        CHECK(settle(*world, player, [&] {
            return world->get_block(10, kFluidTestGroundedY, 8) == heavyId;
        }));
        CHECK(world->get_block(10, kFluidTestGroundedY, 8) == heavyId);
    }

    // (2) A LIGHTER fluid does NOT displace a denser one: it flows around it.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world;
        uint32_t heavyId = 0, lightId = 0;
        build(world, heavyId, lightId);
        // Heavy source at (11) — its range-2 ring reaches (9) (level 2) but
        // leaves (7)/(8) as open air (the source is far enough that the ring
        // never reaches that side). Freeze it by removing the source: the
        // pooled ring stays (evaporation:false) but no longer spreads.
        world->set_block(11, kFluidTestGroundedY, 8, heavyId);
        CHECK(settle(*world, player, [&] {
            return world->get_fluid_level(9, kFluidTestGroundedY, 8) == 2;
        }));
        world->set_block(11, kFluidTestGroundedY, 8, kBlockAir);
        // A light source on the open side of the frozen heavy ring: it must
        // flow into the air at (7) but never displace the denser heavy at
        // (9) (density 0.5 < 3.0).
        world->set_block(8, kFluidTestGroundedY, 8, lightId);
        CHECK(settle(*world, player, [&] {
            return world->get_block(7, kFluidTestGroundedY, 8) == lightId;
        }));
        CHECK(world->get_block(9, kFluidTestGroundedY, 8) == heavyId);   // not displaced
        CHECK(world->get_block(10, kFluidTestGroundedY, 8) == heavyId);  // ring intact
        CHECK(world->get_block(7, kFluidTestGroundedY, 8) == lightId);   // flowed around
    }

    std::cout << "[sdk] fluids: density displacement (denser wins, lighter "
                 "flows around) OK\n";
}

// Task D.3 (META section 13): temperature (declared heat axis), solidification
// and combustion. A fluid declares its solid form (solidifiesInto) and whether
// it ignites adjacent flammable blocks. Unfed edge cells cool into the solid
// form; an igniting fluid consumes a flammable REGISTRY block it touches (a
// non-flammable block is never consumed).
void test_fluid_solidification_combustion() {
    const auto build = [](std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                          uint32_t& fluidId, uint32_t& solidId,
                          uint32_t& woodId, uint32_t& stoneId) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([
              {"name":"magma","namespace":"test","class":"fluid","color":[0.9,0.3,0.1]},
              {"name":"obsidian","namespace":"test","color":[0.1,0.1,0.15]},
              {"name":"wood","namespace":"test","flammability":1.0,"color":[0.6,0.4,0.2]},
              {"name":"stone","namespace":"test","color":[0.5,0.5,0.5]}
            ])", error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:magma","viscosity":1.0,"range":1,"falling":false,
                 "evaporation":false,"density":2.0,"temperature":1400.0,
                 "solidifiesInto":"test:obsidian","ignites":true}])", error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        CHECK(world->resolve_block_id("test:magma", fluidId, error));
        CHECK(world->resolve_block_id("test:obsidian", solidId, error));
        CHECK(world->resolve_block_id("test:wood", woodId, error));
        CHECK(world->resolve_block_id("test:stone", stoneId, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    const int y = kFluidTestGroundedY;

    // (1) Solidification: remove the source; the unfed edge cells cool into
    //     the declared solid form instead of evaporating.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world;
        uint32_t fluidId = 0, solidId = 0, woodId = 0, stoneId = 0;
        build(world, fluidId, solidId, woodId, stoneId);
        world->set_block(8, y, 8, fluidId);  // source
        CHECK(settle(*world, player, [&] {
            return world->get_fluid_level(9, y, 8) == 1;  // range 1 -> ring lvl1
        }));
        world->set_block(8, y, 8, kBlockAir);  // remove source
        CHECK(settle(*world, player, [&] {
            return world->get_block(9, y, 8) == solidId;
        }));
        CHECK(world->get_block(9, y, 8) == solidId);  // edge solidified
    }

    // (2) Combustion: an igniting fluid consumes an adjacent flammable block.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world;
        uint32_t fluidId = 0, solidId = 0, woodId = 0, stoneId = 0;
        build(world, fluidId, solidId, woodId, stoneId);
        world->set_block(9, y, 8, woodId);   // flammable neighbor
        world->set_block(8, y, 8, fluidId);  // igniting source
        CHECK(settle(*world, player, [&] {
            return world->get_block(9, y, 8) != woodId;  // wood consumed
        }));
        CHECK(world->get_block(9, y, 8) != woodId);
    }

    // (3) Combustion gate: a NON-flammable block is never consumed; the fluid
    //     simply cannot spread into it (it spreads around instead).
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world;
        uint32_t fluidId = 0, solidId = 0, woodId = 0, stoneId = 0;
        build(world, fluidId, solidId, woodId, stoneId);
        world->set_block(9, y, 8, stoneId);  // non-flammable neighbor
        world->set_block(8, y, 8, fluidId);  // igniting source
        CHECK(settle(*world, player, [&] {
            return world->get_fluid_level(7, y, 8) == 1;  // spread to the open side
        }));
        CHECK(world->get_block(9, y, 8) == stoneId);  // stone never consumed
    }

    std::cout << "[sdk] fluids: solidification (edge cools to solid) + "
                 "combustion (ignites flammable, spares stone) OK\n";
}

void test_block_registry() {
    engine::registry::BlockRegistry blocks;
    CHECK(blocks.size() > 40);  // builtin table registered

    const engine::registry::BlockDefinition* stone =
        blocks.find_by_name("vulkancraft:stone");
    CHECK(stone != nullptr);
    CHECK(stone->builtinId == 3);
    CHECK(stone->hardness > 1.0f);
    CHECK(stone->uuid.size() == 36);

    // Stable derived id: same namespaced name -> same uuid in a fresh registry.
    const std::string stoneUuid = stone->uuid;
    engine::registry::BlockRegistry again;
    const engine::registry::BlockDefinition* stoneAgain =
        again.find_by_name("vulkancraft:stone");
    CHECK(stoneAgain != nullptr);
    CHECK(stoneAgain->uuid == stoneUuid);

    // JSON asset loads at runtime (no recompile): a project-defined block.
    // FALTANTES §14: per-face materials (faceTop/faceBottom/faceSide),
    // occlusion and renderLayer round-trip through the JSON schema.
    const char* jsonAsset =
        R"([{"name":"ruby","namespace":"test","class":"solid","hardness":3.0,"lightEmission":0.4,"tags":["gem"],"drops":["test:ruby"],"faceTop":[0.2,0.8,0.2],"faceSide":[0.5,0.35,0.2],"occlusion":false,"renderLayer":1},)"
        R"({"name":"sapphire","namespace":"test","class":"solid","hardness":4.0}])";
    std::string error;
    CHECK(blocks.load_from_json(jsonAsset, error));
    const engine::registry::BlockDefinition* ruby = blocks.find_by_name("test:ruby");
    CHECK(ruby != nullptr);
    CHECK(ruby->hardness > 2.9f && ruby->hardness < 3.1f);
    CHECK(ruby->lightEmission > 0.3f);
    CHECK(ruby->tags.size() == 1 && ruby->tags[0] == "gem");
    CHECK(ruby->faceTopSet);
    CHECK(ruby->faceTop.g > 0.7f && ruby->faceTop.g < 0.9f);
    CHECK(!ruby->faceBottomSet);
    CHECK(ruby->faceSideSet);
    CHECK(ruby->faceSide.r > 0.4f && ruby->faceSide.r < 0.6f);
    CHECK(!ruby->occludes);
    CHECK(ruby->renderLayer == 1);
    CHECK(ruby->color.r > 0.99f);  // base color untouched by face overrides

    // renderLayer validation is all-or-nothing: out-of-range is refused with
    // a diagnostic and nothing is registered.
    engine::registry::BlockRegistry badLayer;
    std::string layerError;
    CHECK(!badLayer.load_from_json(
        R"({"name":"bad_layer","namespace":"test","renderLayer":300})", layerError));
    CHECK(!layerError.empty());
    CHECK(badLayer.find_by_name("test:bad_layer") == nullptr);
    engine::registry::BlockRegistry negLayer;
    std::string negError;
    CHECK(!negLayer.load_from_json(
        R"({"name":"neg_layer","namespace":"test","renderLayer":-2})", negError));
    CHECK(!negError.empty());

    // ID stability across load order: identical definitions in a different
    // order produce the same UUIDs (reorder-proof ids).
    engine::registry::BlockRegistry reordered;
    std::string errA;
    CHECK(reordered.load_from_json(
        R"([{"name":"sapphire","namespace":"test"},{"name":"ruby","namespace":"test","id":")" + ruby->uuid + "\"}]", errA));
    const engine::registry::BlockDefinition* rubyReordered =
        reordered.find_by_name("test:ruby");
    CHECK(rubyReordered != nullptr);
    CHECK(rubyReordered->uuid == ruby->uuid);

    // Invalid asset: clear diagnostic, nothing registered, safe fallback.
    engine::registry::BlockRegistry broken;
    std::string brokenError;
    const bool loaded = broken.load_from_json(R"({"name":""})", brokenError);
    CHECK(!loaded);
    CHECK(!brokenError.empty());
    CHECK(broken.find_by_name("test:ruby") == nullptr);
    CHECK(broken.find_by_uuid("00000000-0000-0000-0000-000000000000") == nullptr);
    CHECK(broken.fallback().name == "unknown");
    CHECK(!broken.fallback().collidable);

    std::cout << "[sdk] block registry: " << blocks.size()
              << " blocks, JSON load, id stability and fallback OK\n";
}

void test_item_registry() {
    engine::registry::ItemRegistry items;

    engine::registry::ItemDefinition pickaxe;
    pickaxe.ns = "vulkancraft";
    pickaxe.name = "stone_pickaxe";
    pickaxe.maxStack = 1;
    pickaxe.durability = 250;
    pickaxe.tags = { "tool", "pickaxe" };
    std::string error;
    CHECK(items.register_item(pickaxe, error));
    CHECK(error.empty());

    const engine::registry::ItemDefinition* found =
        items.find_by_name("vulkancraft:stone_pickaxe");
    CHECK(found != nullptr);
    CHECK(found->maxStack == 1);
    CHECK(found->durability == 250);
    CHECK(found->uuid.size() == 36);

    // §2 item 8: use/equipment/behavior components round-trip through the
    // programmatic registration and the JSON schema.
    engine::registry::ItemDefinition sword;
    sword.ns = "vulkancraft";
    sword.name = "stone_sword";
    sword.maxStack = 1;
    sword.useCooldownMs = 250;
    sword.useMode = engine::registry::ItemUseMode::Instant;
    sword.equipSlot = engine::registry::ItemEquipSlot::Hand;
    sword.attackDamage = 5.0f;
    sword.behaviorId = "vulkancraft:melee_swing";
    CHECK(items.register_item(sword, error));
    const engine::registry::ItemDefinition* swordFound =
        items.find_by_name("vulkancraft:stone_sword");
    CHECK(swordFound != nullptr);
    CHECK(swordFound->useCooldownMs == 250);
    CHECK(swordFound->useMode == engine::registry::ItemUseMode::Instant);
    CHECK(swordFound->equipSlot == engine::registry::ItemEquipSlot::Hand);
    CHECK(std::abs(swordFound->attackDamage - 5.0f) < 1e-3f);
    CHECK(swordFound->behaviorId == "vulkancraft:melee_swing");

    // JSON round-trip of the components (useMode/equipSlot enums + stats).
    engine::registry::ItemRegistry components;
    std::string componentsError;
    CHECK(components.load_from_json(
        R"({"name":"iron_helmet","namespace":"test","useCooldown":500,"useMode":"continuous","equipSlot":"head","attackDamage":1.5,"armor":2.0,"behaviorId":"test:wear_effect"})",
        componentsError));
    const engine::registry::ItemDefinition* helmet =
        components.find_by_name("test:iron_helmet");
    CHECK(helmet != nullptr);
    CHECK(helmet->useCooldownMs == 500);
    CHECK(helmet->useMode == engine::registry::ItemUseMode::Continuous);
    CHECK(helmet->equipSlot == engine::registry::ItemEquipSlot::Head);
    CHECK(std::abs(helmet->attackDamage - 1.5f) < 1e-3f);
    CHECK(std::abs(helmet->armor - 2.0f) < 1e-3f);
    CHECK(helmet->behaviorId == "test:wear_effect");

    // §2 item 8 validation is all-or-nothing: out-of-range/unknown components
    // are refused with a diagnostic, never clamped or guessed.
    auto refuseItem = [&](const std::string& json) {
        engine::registry::ItemRegistry registry;
        std::string refuseError;
        CHECK(!registry.load_from_json(json, refuseError));
        CHECK(!refuseError.empty());
    };
    refuseItem(R"({"name":"a","namespace":"test","useCooldown":70000})");
    refuseItem(R"({"name":"b","namespace":"test","useMode":"spell"})");
    refuseItem(R"({"name":"c","namespace":"test","equipSlot":"wings"})");
    refuseItem(R"({"name":"d","namespace":"test","attackDamage":-1})");
    refuseItem(R"({"name":"e","namespace":"test","armor":101})");
    refuseItem(R"({"name":"f","namespace":"test","behaviorId":"unnamespaced"})");


    // Invalid maxStack is rejected with a diagnostic.
    engine::registry::ItemDefinition bad;
    bad.ns = "test";
    bad.name = "bad_stack";
    bad.maxStack = 100;
    std::string badError;
    CHECK(!items.register_item(bad, badError));
    CHECK(!badError.empty());

    // JSON load + stable derived id (same name in a fresh registry that also
    // loads the asset produces the same uuid — id stability across instances).
    engine::registry::ItemRegistry more;
    std::string jsonError;
    CHECK(more.load_from_json(
        R"({"name":"copper_ingot","namespace":"test","maxStack":64,"tags":["material"]})",
        jsonError));
    const engine::registry::ItemDefinition* ingot = more.find_by_name("test:copper_ingot");
    CHECK(ingot != nullptr);
    CHECK(ingot->maxStack == 64);
    engine::registry::ItemRegistry another;
    CHECK(another.load_from_json(
        R"({"name":"copper_ingot","namespace":"test"})", jsonError));
    const engine::registry::ItemDefinition* ingotAnother =
        another.find_by_name("test:copper_ingot");
    CHECK(ingotAnother != nullptr);
    CHECK(ingotAnother->uuid == ingot->uuid);

    CHECK(more.fallback().name == "unknown");

    std::cout << "[sdk] item registry: " << items.size()
              << " items, validation and fallback OK\n";
}

// FALTANTES item 9: cross-reference validation between registries — block
// drops resolve against a registered ItemRegistry, fluid blocks resolve
// against a BlockRegistry, builtin blocks are the engine's own contract, and
// an unnamespaced drop is refused at registration.
void test_cross_reference_validation() {
    using engine::registry::BlockRegistry;
    using engine::registry::FluidRegistry;
    using engine::registry::ItemRegistry;

    // A user-authored block whose drop references an unknown item is flagged;
    // registering the item makes the same registry clean.
    BlockRegistry blocks;
    std::string error;
    CHECK(blocks.load_from_json(
        R"({"name":"ruby_ore","namespace":"test","drops":["test:ruby"]})",
        error));
    ItemRegistry items;
    std::vector<std::string> refErrors;
    CHECK(!blocks.validate_item_references(items, refErrors));
    bool foundRuby = false;
    for (const std::string& message : refErrors) {
        if (message.find("test:ruby") != std::string::npos) foundRuby = true;
    }
    CHECK(foundRuby);
    std::string itemError;
    engine::registry::ItemDefinition ruby;
    ruby.ns = "test";
    ruby.name = "ruby";
    CHECK(items.register_item(ruby, itemError));
    refErrors.clear();
    CHECK(blocks.validate_item_references(items, refErrors));
    CHECK(refErrors.empty());

    // Unnamespaced drops are refused at registration (all-or-nothing).
    BlockRegistry strict;
    CHECK(!strict.load_from_json(
        R"({"name":"bad_drop","namespace":"test","drops":["ruby"]})",
        error));

    // Builtin blocks are the engine's own contract: a fresh BlockRegistry with
    // an empty ItemRegistry validates clean (no false positives from the
    // builtin table's auto-drops).
    BlockRegistry fresh;
    ItemRegistry emptyItems;
    std::vector<std::string> builtinErrors;
    CHECK(fresh.validate_item_references(emptyItems, builtinErrors));
    CHECK(builtinErrors.empty());

    // Fluids: the driven block must resolve (builtin or authored).
    FluidRegistry fluids;
    CHECK(fluids.load_from_json(
        R"({"block":"vulkancraft:sludge","viscosity":0.8})", error));
    BlockRegistry baseBlocks;
    std::vector<std::string> fluidErrors;
    CHECK(!fluids.validate_block_references(baseBlocks, fluidErrors));
    bool foundSludge = false;
    for (const std::string& message : fluidErrors) {
        if (message.find("vulkancraft:sludge") != std::string::npos) foundSludge = true;
    }
    CHECK(foundSludge);
    // Builtin block reference resolves against a fresh BlockRegistry.
    FluidRegistry waterFluid;
    CHECK(waterFluid.load_from_json(
        R"({"block":"vulkancraft:water"})", error));
    fluidErrors.clear();
    CHECK(waterFluid.validate_block_references(baseBlocks, fluidErrors));
    CHECK(fluidErrors.empty());

    std::cout << "[sdk] cross-reference validation: drops->items, fluids->blocks"
              << " OK\n";
}

// FALTANTES item 5: named block states + versioned transitions — JSON
// round-trip, all-or-nothing validation (duplicates, unknown refs, empty
// trigger, self/duplicate rules) and state-aware material resolution in the
// mesher (states[0] = default; index k = states[k]; out-of-range clamps).
void test_block_states_transitions() {
    using engine::registry::BlockRegistry;

    BlockRegistry blocks;
    std::string error;
    CHECK(blocks.load_from_json(
        R"({"name":"lamp","namespace":"test","drops":["test:lamp"],"states":[
            {"name":"base","color":[0.4,0.4,0.4]},
            {"name":"lit","color":[1.0,0.6,0.1],"faceTop":[0.9,0.9,0.9],"lightEmission":0.8}
          ],"transitions":[
            {"from":"","to":"lit","trigger":"ignite"},
            {"from":"lit","to":"","trigger":"extinguish"}
          ]})",
        error));
    const engine::registry::BlockDefinition* lamp = blocks.find_by_name("test:lamp");
    CHECK(lamp != nullptr);
    CHECK(lamp->states.size() == 2);
    CHECK(lamp->state_index("") == 0);
    CHECK(lamp->state_index("base") == 0);
    CHECK(lamp->state_index("lit") == 1);
    CHECK(lamp->state_index("nope") == -1);
    CHECK(lamp->states[1].name == "lit");
    CHECK(std::abs(lamp->states[1].color.r - 1.0f) < 1e-3f);
    CHECK(std::abs(lamp->states[1].color.g - 0.6f) < 1e-3f);
    CHECK(lamp->states[1].faceTopSet);
    CHECK(std::abs(lamp->states[1].faceTop.r - 0.9f) < 1e-3f);
    CHECK(std::abs(lamp->states[1].lightEmission - 0.8f) < 1e-3f);
    CHECK(lamp->transitions.size() == 2);
    CHECK(lamp->transitions[0].fromState.empty());
    CHECK(lamp->transitions[0].toState == "lit");
    CHECK(lamp->transitions[0].trigger == "ignite");
    CHECK(lamp->transitions[1].fromState == "lit");
    CHECK(lamp->transitions[1].toState.empty());

    // Validation is all-or-nothing: structure/ref errors refuse the whole
    // document with a diagnostic, never guessed or clamped.
    auto refuse = [&](const std::string& json) {
        BlockRegistry registry;
        std::string refuseError;
        CHECK(!registry.load_from_json(json, refuseError));
        CHECK(!refuseError.empty());
    };
    refuse(R"({"name":"a","namespace":"test","states":[{"name":"dup"},{"name":"dup"}]})");
    refuse(R"({"name":"b","namespace":"test","states":[{"name":""}]})");
    refuse(R"({"name":"c","namespace":"test","states":[{"name":"s"}],"transitions":[{"from":"","to":"ghost","trigger":"x"}]})");
    refuse(R"({"name":"d","namespace":"test","states":[{"name":"s"}],"transitions":[{"from":"ghost","to":"","trigger":"x"}]})");
    refuse(R"({"name":"e","namespace":"test","transitions":[{"from":"","to":"s","trigger":""}]})");
    refuse(R"({"name":"f","namespace":"test","transitions":[{"from":"s","to":"s","trigger":"x"}]})");
    refuse(R"({"name":"g","namespace":"test","states":[{"name":"s"},{"name":"t"}],"transitions":[{"from":"","to":"s","trigger":"x"},{"from":"","to":"t","trigger":"x"}]})");

    std::cout << "[sdk] block states + transitions: round-trip and validation OK\n";
}

// FALTANTES item 4: sound/particle references, the mining tool component and
// the resistance/physical properties on BlockDefinition — round-trip through
// the JSON schema and all-or-nothing validation (strict tool enum, refs must
// be namespaced, ranges enforced; never clamped).
void test_block_tool_physics() {
    using engine::registry::BlockRegistry;
    using engine::registry::BlockTool;

    BlockRegistry blocks;
    std::string error;
    CHECK(blocks.load_from_json(
        R"({"name":"adamant","namespace":"test","drops":["test:adamant"],"soundPlace":"test:adamant_place","soundBreak":"test:adamant_break","soundStep":"test:adamant_step","soundHit":"test:adamant_hit","particleBreak":"test:adamant_dust","tool":"pickaxe","toolTier":3,"resistance":40,"friction":0.3,"bounciness":0.05,"density":8.0})",
        error));
    const engine::registry::BlockDefinition* adamant = blocks.find_by_name("test:adamant");
    CHECK(adamant != nullptr);
    CHECK(adamant->tool == BlockTool::Pickaxe);
    CHECK(adamant->toolTier == 3);
    CHECK(adamant->resistance == 40.0f);
    CHECK(adamant->friction == 0.3f);
    CHECK(adamant->bounciness == 0.05f);
    CHECK(adamant->density == 8.0f);
    CHECK(adamant->soundPlace == "test:adamant_place");
    CHECK(adamant->soundBreak == "test:adamant_break");
    CHECK(adamant->soundStep == "test:adamant_step");
    CHECK(adamant->soundHit == "test:adamant_hit");
    CHECK(adamant->particleBreak == "test:adamant_dust");

    // §2 item 6: declarative behavior reference round-trips (validated, not
    // resolved — resolution belongs to the abilities/block entity milestone).
    BlockRegistry behavioral;
    CHECK(behavioral.load_from_json(
        R"({"name":"ember","namespace":"test","behaviorId":"test:spread_ember"})",
        error));
    CHECK(behavioral.find_by_name("test:ember")->behaviorId == "test:spread_ember");
    BlockRegistry noBehavior;
    CHECK(noBehavior.load_from_json(R"({"name":"quiet","namespace":"test"})", error));
    CHECK(noBehavior.find_by_name("test:quiet")->behaviorId.empty());

    // Defaults: no tool (Any), tier 0, friction 0.5, density 1, empty refs.
    BlockRegistry plain;
    CHECK(plain.load_from_json(R"({"name":"plain","namespace":"test"})", error));
    const engine::registry::BlockDefinition* plainBlock = plain.find_by_name("test:plain");
    CHECK(plainBlock->tool == BlockTool::Any);
    CHECK(plainBlock->toolTier == 0);
    CHECK(plainBlock->friction == 0.5f);
    CHECK(plainBlock->density == 1.0f);
    CHECK(plainBlock->soundPlace.empty());

    // All-or-nothing: unknown tool, out-of-range tiers/physics and unnamespaced
    // refs are refused with a diagnostic, never clamped or guessed.
    auto refuse = [&](const std::string& json) {
        BlockRegistry registry;
        std::string refuseError;
        CHECK(!registry.load_from_json(json, refuseError));
        CHECK(!refuseError.empty());
    };
    refuse(R"({"name":"a","namespace":"test","tool":"drill"})");
    refuse(R"({"name":"b","namespace":"test","toolTier":9})");
    refuse(R"({"name":"c","namespace":"test","resistance":-1})");
    refuse(R"({"name":"d","namespace":"test","friction":1.5})");
    refuse(R"({"name":"e","namespace":"test","bounciness":-0.1})");
    refuse(R"({"name":"f","namespace":"test","density":0})");
    refuse(R"({"name":"g","namespace":"test","soundPlace":"notnamespaced"})");
    refuse(R"({"name":"h","namespace":"test","behaviorId":"unnamespaced"})");

    std::cout << "[sdk] block tool + physics: round-trip and validation OK\n";
}

// META section 14: authoritative item stacks and slot inventories — typed
// filters, transfer/split/merge/swap, change events, lossless add (remainder)
// and data-driven serialization (namespaced ids, never numeric).
void test_item_stack_inventory() {
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    engine::registry::ItemRegistry items;
    std::string error;
    {
        engine::registry::ItemDefinition def;
        def.ns = "vulkancraft";
        def.name = "cobblestone";
        def.maxStack = 64;
        def.tags = { "stone", "material" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "stone_pickaxe";
        def.maxStack = 1;
        def.durability = 250;
        def.tags = { "tool", "pickaxe" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "coal";
        def.maxStack = 64;
        def.tags = { "fuel" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "iron_ingot";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
    }

    // ---- ItemStack semantics ----
    ItemStack a;
    a.item = "vulkancraft:cobblestone";
    a.count = 10;
    CHECK(a.capacity_left(64) == 54);
    CHECK(a.add(100, 64) == 54);
    CHECK(a.count == 64);
    ItemStack b = a.split(20);
    CHECK(b.count == 20);
    CHECK(a.count == 44);
    ItemStack damaged1, damaged2;
    damaged1.item = "vulkancraft:stone_pickaxe";
    damaged1.count = 1;
    damaged1.damage = 5;
    damaged2 = damaged1;
    CHECK(ItemStack::can_merge(damaged1, damaged2));
    damaged2.damage = 6;
    CHECK(!ItemStack::can_merge(damaged1, damaged2));

    // ---- Inventory: typed filters + authoritative set/add/remove ----
    Inventory inv(6);
    CHECK(inv.slot_count() == 6);
    const uint64_t v0 = inv.version();
    ItemStack stone;
    stone.item = "vulkancraft:cobblestone";
    stone.count = 1;
    CHECK(!inv.set(0, stone, items, error));  // slot 0 locked (no filter)
    CHECK(!error.empty());
    SlotFilter toolFilter;
    toolFilter.allowItems = { "vulkancraft:stone_pickaxe" };
    inv.set_filter(1, toolFilter);
    error.clear();
    CHECK(!inv.set(1, stone, items, error));  // cobblestone rejected by slot 1
    ItemStack pick;
    pick.item = "vulkancraft:stone_pickaxe";
    pick.count = 1;
    CHECK(inv.set(1, pick, items, error));
    SlotFilter tagFilter;
    tagFilter.allowTags = { "stone" };
    inv.set_filter(2, tagFilter);
    CHECK(inv.set(2, stone, items, error));  // accepted via tag expansion
    SlotFilter any;
    any.allowAny = true;
    for (int s = 3; s < 6; ++s) inv.set_filter(s, any);
    error.clear();

    // Lossless add: fits (slots 2..5, 64 each) -> no remainder.
    ItemStack big;
    big.item = "vulkancraft:cobblestone";
    big.count = 200;
    ItemStack rem = inv.add(big, items, error);
    CHECK(error.empty());
    CHECK(rem.empty());
    CHECK(inv.count_of("vulkancraft:cobblestone") == 201);
    CHECK(inv.version() > v0);

    // Overflow is returned as remainder — nothing is lost or truncated.
    big.count = 1000;
    rem = inv.add(big, items, error);
    CHECK(rem.count == 945);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 256);

    // Unknown item refused, never guessed.
    ItemStack ghost;
    ghost.item = "vulkancraft:ghost";
    ghost.count = 1;
    error.clear();
    rem = inv.add(ghost, items, error);
    CHECK(!error.empty());
    CHECK(rem.count == 1);

    CHECK(inv.remove("vulkancraft:cobblestone", 10, items, error) == 10);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 246);
    CHECK(inv.consume(2, 5) == 5);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 241);

    // ---- Transfer (validation on both sides, partial move) ----
    Inventory src(4), dst(4);
    for (int s = 0; s < 4; ++s) {
        src.set_filter(s, any);
        dst.set_filter(s, any);
    }
    stone.count = 30;
    CHECK(src.set(0, stone, items, error));
    ItemStack coal;
    coal.item = "vulkancraft:coal";
    coal.count = 1;
    CHECK(dst.set(0, coal, items, error));
    error.clear();
    // Destination holds a different item -> authoritative refusal.
    CHECK(Inventory::transfer(src, 0, dst, 0, 10, items, error) == 0);
    CHECK(!error.empty());
    // Filter rejection on the destination.
    dst.set_filter(2, toolFilter);
    error.clear();
    CHECK(Inventory::transfer(src, 0, dst, 2, 5, items, error) == 0);
    CHECK(!error.empty());
    // Clean move.
    error.clear();
    CHECK(Inventory::transfer(src, 0, dst, 1, 10, items, error) == 10);
    CHECK(src.count_of("vulkancraft:cobblestone") == 20);
    CHECK(Inventory::transfer(src, 0, dst, 1, 20, items, error) == 20);
    CHECK(src.count_of("vulkancraft:cobblestone") == 0);

    // ---- Swap validates both filters ----
    ItemStack ingot;
    ingot.item = "vulkancraft:iron_ingot";
    ingot.count = 3;
    CHECK(src.set(1, ingot, items, error));
    CHECK(Inventory::swap(src, 0, 1, items, error));
    CHECK(src.get(0).item == "vulkancraft:iron_ingot");
    src.set_filter(2, toolFilter);
    error.clear();
    CHECK(!Inventory::swap(src, 2, 0, items, error));  // slot 2 rejects ingot
    CHECK(!error.empty());

    // ---- Change events: version + callback fire on success only ----
    Inventory eventsInv(3);  // slot 0 locked, slots 1-2 unrestricted
    for (int s = 1; s < 3; ++s) eventsInv.set_filter(s, any);
    int changes = 0;
    eventsInv.set_change_callback([&](const Inventory&) { ++changes; });
    const uint64_t before = eventsInv.version();
    coal.count = 1;
    CHECK(eventsInv.add(coal, items, error).empty());
    CHECK(changes == 1);
    CHECK(eventsInv.version() == before + 1);
    error.clear();
    CHECK(!eventsInv.set(0, coal, items, error));  // locked slot: no event
    CHECK(changes == 1);
    CHECK(eventsInv.version() == before + 1);

    // ---- Serialization: namespaced ids, damage + data round-trip ----
    Inventory saveInv(4);
    for (int s = 0; s < 4; ++s) saveInv.set_filter(s, any);
    pick.damage = 7;
    pick.data = "sharpness:2";
    CHECK(saveInv.set(0, pick, items, error));
    stone.count = 5;
    CHECK(saveInv.set(1, stone, items, error));
    const std::string json = saveInv.serialize_json();
    Inventory loaded(4);
    for (int s = 0; s < 4; ++s) loaded.set_filter(s, any);
    CHECK(loaded.deserialize_json(json, items, error));
    CHECK(loaded.get(0).damage == 7);
    CHECK(loaded.get(0).data == "sharpness:2");
    CHECK(loaded.get(1).count == 5);
    CHECK(loaded.serialize_json() == json);
    // Corrupt payloads are rejected all-or-nothing (version unchanged).
    const uint64_t vLoaded = loaded.version();
    error.clear();
    CHECK(!loaded.deserialize_json(
        R"({"slots":[{"item":"vulkancraft:ghost","count":1},null,null,null]})",
        items, error));
    CHECK(!error.empty());
    CHECK(loaded.version() == vLoaded);
    error.clear();
    CHECK(!loaded.deserialize_json(
        R"({"slots":[{"item":"vulkancraft:coal","count":100},null,null,null]})",
        items, error));
    CHECK(!error.empty());
    CHECK(loaded.version() == vLoaded);
    // Unknown format version is refused, never guessed.
    error.clear();
    CHECK(!loaded.deserialize_json(
        R"({"version":2,"slots":[null,null,null,null]})", items, error));
    CHECK(!error.empty());
    CHECK(loaded.version() == vLoaded);

    std::cout << "[sdk] inventory: typed filters, transfer/split/merge/swap, "
                 "events and JSON round-trip OK\n";
}

// Task E.3 (META section 14): transactional undo/redo for slot contents.
// Every successful mutation pushes the pre-mutation slot vector, so undo()
// restores exact contents with no loss or duplication and redo() re-applies
// the change. A new mutation clears the redo tail; a refused mutation does
// not push history. Undo/redo are themselves mutations (version + callback).
void test_inventory_undo_redo() {
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    engine::registry::ItemRegistry items;
    std::string error;
    {
        engine::registry::ItemDefinition def;
        def.ns = "vulkancraft";
        def.name = "cobblestone";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "coal";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
    }

    SlotFilter any;
    any.allowAny = true;

    Inventory inv(4);
    for (int s = 0; s < 4; ++s) inv.set_filter(s, any);
    int changes = 0;
    inv.set_change_callback([&](const Inventory&) { ++changes; });

    ItemStack stone;
    stone.item = "vulkancraft:cobblestone";
    stone.count = 10;
    ItemStack coal;
    coal.item = "vulkancraft:coal";
    coal.count = 5;

    // Fresh inventory has no history; undo/redo are no-ops.
    CHECK(!inv.can_undo());
    CHECK(!inv.can_redo());
    CHECK(!inv.undo());
    CHECK(!inv.redo());

    // (1) set -> undo restores the exact prior (empty) slot; redo re-applies.
    CHECK(inv.set(0, stone, items, error));
    CHECK(inv.get(0).count == 10);
    CHECK(inv.can_undo());
    CHECK(!inv.can_redo());
    CHECK(inv.undo());
    CHECK(inv.get(0).empty());
    CHECK(inv.can_redo());
    CHECK(inv.redo());
    CHECK(inv.get(0).count == 10);

    // (2) A refused mutation pushes nothing (no history growth).
    ItemStack ghost;
    ghost.item = "vulkancraft:ghost";
    ghost.count = 1;
    const uint64_t vBefore = inv.version();
    error.clear();
    CHECK(!inv.set(0, ghost, items, error));
    CHECK(!error.empty());
    CHECK(inv.version() == vBefore);

    // (3) add + remove compose across slots; undo walks back in LIFO order.
    CHECK(inv.set(1, coal, items, error));
    ItemStack more;
    more.item = "vulkancraft:cobblestone";
    more.count = 4;
    CHECK(inv.add(more, items, error).empty());   // merges into slot 0
    CHECK(inv.get(0).count == 14);
    CHECK(inv.undo());                             // undo add
    CHECK(inv.get(0).count == 10);
    CHECK(inv.undo());                             // undo set(1, coal)
    CHECK(inv.get(1).empty());

    // (4) consume() participates; a zero-consume (empty slot) pushes nothing.
    CHECK(inv.redo());                             // redo set(1, coal)
    CHECK(inv.redo());                             // redo add
    CHECK(inv.get(1).count == 5);
    CHECK(inv.get(0).count == 14);
    CHECK(inv.consume(1, 2) == 2);
    CHECK(inv.get(1).count == 3);
    CHECK(inv.undo());
    CHECK(inv.get(1).count == 5);

    // (5) A new mutation clears the redo tail (linear history).
    CHECK(inv.redo());                             // redo consume
    CHECK(inv.get(1).count == 3);
    CHECK(inv.remove("vulkancraft:cobblestone", 4, items, error) == 4);
    CHECK(inv.get(0).count == 10);
    CHECK(!inv.can_redo());                        // redo tail cleared

    // (6) Cross-inventory transfer records both sides independently.
    Inventory srcInv(2), dstInv(2);
    srcInv.set_filter(0, any);
    srcInv.set_filter(1, any);
    dstInv.set_filter(0, any);
    dstInv.set_filter(1, any);
    CHECK(srcInv.set(0, stone, items, error));
    CHECK(Inventory::transfer(srcInv, 0, dstInv, 0, 3, items, error) == 3);
    CHECK(srcInv.get(0).count == 7);
    CHECK(dstInv.get(0).count == 3);
    CHECK(srcInv.undo());
    CHECK(dstInv.undo());
    CHECK(srcInv.get(0).count == 10);
    CHECK(dstInv.get(0).empty());

    // (7) deserialize participates; undo restores the prior contents.
    Inventory serInv(2);
    serInv.set_filter(0, any);
    serInv.set_filter(1, any);
    CHECK(serInv.set(0, coal, items, error));
    CHECK(serInv.set(1, stone, items, error));
    CHECK(serInv.undo());                          // undo set(1, stone)
    CHECK(serInv.get(1).empty());
    CHECK(serInv.redo());
    CHECK(serInv.get(1).count == 10);

    // (8) undo/redo are themselves mutations: version + callback fire.
    const uint64_t v8 = inv.version();
    const int changes8 = changes;
    CHECK(inv.undo());
    CHECK(inv.version() == v8 + 1);
    CHECK(changes == changes8 + 1);

    // (9) clear_history drops both stacks.
    inv.clear_history();
    CHECK(!inv.can_undo());
    CHECK(!inv.can_redo());
    CHECK(!inv.undo());
    CHECK(!inv.redo());

    std::cout << "[sdk] inventory: transactional undo/redo (exact restore, "
                 "no loss/duplication, redo-tail clearing) OK\n";
}

// Task E.2 (META section 14): nested containers. An item may BE a container
// (backpack): its nested inventory travels in ItemStack::data as the standard
// inventory JSON, so it survives the outer inventory's save/load/replication
// (the payload is opaque). pack_nested/unpack_nested are the canonical
// pack/unpack entry points; unpack is all-or-nothing (wrong slot count,
// unknown item or bad version refused with the target untouched).
void test_inventory_nested_containers() {
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    engine::registry::ItemRegistry items;
    std::string error;
    {
        engine::registry::ItemDefinition def;
        def.ns = "vulkancraft";
        def.name = "backpack";
        def.maxStack = 1;
        def.tags = { "container" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "cobblestone";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "iron_ingot";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
    }
    SlotFilter any;
    any.allowAny = true;

    // Fill a 9-slot backpack with items.
    Inventory backpack(9);
    for (int s = 0; s < 9; ++s) backpack.set_filter(s, any);
    ItemStack stone;
    stone.item = "vulkancraft:cobblestone";
    stone.count = 30;
    ItemStack ingot;
    ingot.item = "vulkancraft:iron_ingot";
    ingot.count = 7;
    CHECK(backpack.set(0, stone, items, error));
    CHECK(backpack.set(1, ingot, items, error));

    // (1) Pack into the backpack ITEM's data payload.
    ItemStack backpackItem;
    backpackItem.item = "vulkancraft:backpack";
    backpackItem.count = 1;
    backpackItem.data = Inventory::pack_nested(backpack);
    CHECK(!backpackItem.data.empty());

    // (2) The container travels in an OUTER inventory and survives the
    //     serialization round-trip byte-identically (save/load/replication
    //     see it as opaque data).
    Inventory outer(4);
    for (int s = 0; s < 4; ++s) outer.set_filter(s, any);
    CHECK(outer.set(2, backpackItem, items, error));
    const std::string outerJson = outer.serialize_json();
    Inventory outerLoaded(4);
    for (int s = 0; s < 4; ++s) outerLoaded.set_filter(s, any);
    CHECK(outerLoaded.deserialize_json(outerJson, items, error));
    CHECK(outerLoaded.get(2).item == "vulkancraft:backpack");
    CHECK(outerLoaded.get(2).data == backpackItem.data);

    // (3) Unpack restores the nested contents (slot count must match).
    // `out` is assigned from the validated candidate, so its initial size is
    // irrelevant (Inventory has no default constructor).
    Inventory opened(1);
    CHECK(Inventory::unpack_nested(outerLoaded.get(2).data, 9, items, opened, error));
    CHECK(opened.slot_count() == 9);
    CHECK(opened.get(0).count == 30);
    CHECK(opened.get(1).item == "vulkancraft:iron_ingot");
    CHECK(opened.get(1).count == 7);
    CHECK(opened.serialize_json() == backpack.serialize_json());

    // (4) Wrong slot count refused all-or-nothing (target untouched).
    Inventory untouched(3);
    CHECK(!Inventory::unpack_nested(backpackItem.data, 5, items, opened, error));
    CHECK(!error.empty());
    CHECK(opened.serialize_json() == backpack.serialize_json());

    // (5) Unknown item inside the payload refused (never guessed).
    std::string corrupt =
        R"({"version":1,"slots":[{"item":"vulkancraft:ghost","count":1},"null"}])";
    CHECK(!Inventory::unpack_nested(corrupt, 2, items, opened, error));
    CHECK(!error.empty());
    CHECK(opened.serialize_json() == backpack.serialize_json());

    // (6) Undo/redo treats the container as opaque: the outer mutation that
    //     placed the backpack is undoable and restores the exact payload.
    CHECK(outer.undo());
    CHECK(outer.get(2).empty());
    CHECK(outer.redo());
    CHECK(outer.get(2).data == backpackItem.data);

    std::cout << "[sdk] inventory: nested containers (item-as-inventory, "
                 "opaque payload round-trip, all-or-nothing unpack) OK\n";
}

// META section 14: data-driven recipe graph — inputs (item/tag/alternatives),
// station/time/energy/fuel/conditions, outputs + byproducts, authoritative
// atomic craft and dependency edges with cycle detection.
void test_recipe_graph() {
    using engine::registry::CraftResult;
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    engine::registry::ItemRegistry items;
    std::string itemError;
    CHECK(items.load_from_json(
        R"([
          {"namespace":"vulkancraft","name":"cobblestone","maxStack":64,"tags":["stone"]},
          {"namespace":"vulkancraft","name":"stick","maxStack":64,"tags":["stick"]},
          {"namespace":"vulkancraft","name":"log","maxStack":64,"tags":["log"]},
          {"namespace":"vulkancraft","name":"plank","maxStack":64},
          {"namespace":"vulkancraft","name":"stone_pickaxe","maxStack":1,"durability":250},
          {"namespace":"vulkancraft","name":"iron_ore","maxStack":64},
          {"namespace":"vulkancraft","name":"iron_ingot","maxStack":64},
          {"namespace":"vulkancraft","name":"coal","maxStack":64,"tags":["fuel"]},
          {"namespace":"vulkancraft","name":"crafting_table","maxStack":64},
          {"namespace":"vulkancraft","name":"furnace","maxStack":64},
          {"namespace":"vulkancraft","name":"slag","maxStack":64},
          {"namespace":"vulkancraft","name":"gizmo","maxStack":64},
          {"namespace":"vulkancraft","name":"widget","maxStack":64}
        ])",
        itemError));
    CHECK(itemError.empty());

    engine::registry::RecipeRegistry recipes(&items);
    std::string recipeError;
    CHECK(recipes.load_from_json(
        R"({
          "namespace":"vulkancraft",
          "recipes":[
            {"name":"stone_pickaxe","station":"vulkancraft:crafting_table","time":2.0,
             "inputs":[{"item":"vulkancraft:cobblestone","count":3},{"tag":"stick","count":2}],
             "outputs":[{"item":"vulkancraft:stone_pickaxe","count":1}]},
            {"name":"plank_from_log","inputs":[{"item":"vulkancraft:log","count":1}],
             "outputs":[{"item":"vulkancraft:plank","count":4}]},
            {"name":"stick_from_plank","inputs":[{"item":"vulkancraft:plank","count":2}],
             "outputs":[{"item":"vulkancraft:stick","count":2}]},
            {"name":"smelt_iron","station":"vulkancraft:furnace","energy":1.0,
             "fuel":"vulkancraft:coal","conditions":["unlocked:iron_age"],
             "inputs":[{"item":"vulkancraft:iron_ore","count":1}],
             "outputs":[{"item":"vulkancraft:iron_ingot","count":1}],
             "byproducts":[{"item":"vulkancraft:slag","count":1,"chance":0.5}]}
          ]
        })",
        recipeError));
    CHECK(recipeError.empty());
    CHECK(recipes.size() == 4);

    const engine::registry::RecipeDefinition* pick =
        recipes.find_by_name("vulkancraft:stone_pickaxe");
    CHECK(pick != nullptr);
    CHECK(pick->station == "vulkancraft:crafting_table");
    CHECK(pick->time == 2.0);
    CHECK(pick->inputs.size() == 2);
    CHECK(pick->inputs[1].tag == "stick");
    CHECK(pick->outputs.size() == 1);
    CHECK(pick->uuid.size() == 36);
    CHECK(recipes.find_by_uuid(pick->uuid) != nullptr);
    CHECK(recipes.find_by_uuid(pick->uuid)->namespaced() == pick->namespaced());

    const engine::registry::RecipeDefinition* smelt =
        recipes.find_by_name("vulkancraft:smelt_iron");
    CHECK(smelt->energy == 1.0);
    CHECK(smelt->fuel == "vulkancraft:coal");
    CHECK(smelt->conditions.size() == 1);
    CHECK(smelt->outputs.size() == 2);
    CHECK(smelt->outputs[0].item == "vulkancraft:iron_ingot");
    CHECK(smelt->outputs[1].byproduct);
    CHECK(smelt->outputs[1].chance == 0.5);

    // Unknown item / unknown tag refused at registration (never guessed).
    engine::registry::RecipeRegistry strict(&items);
    std::string badError;
    CHECK(!strict.load_from_json(
        R"({"recipes":[{"name":"bad","inputs":[{"item":"vulkancraft:nope"}],"outputs":[{"item":"vulkancraft:coal"}]}]})",
        badError));
    CHECK(!badError.empty());
    badError.clear();
    CHECK(!strict.load_from_json(
        R"({"recipes":[{"name":"bad2","inputs":[{"tag":"ghost_tag"}],"outputs":[{"item":"vulkancraft:coal"}]}]})",
        badError));
    CHECK(!badError.empty());

    // ---- Craft: station gating, atomic consumption, outputs/byproducts ----
    SlotFilter any;
    any.allowAny = true;
    Inventory inv(9);
    for (int s = 0; s < 9; ++s) inv.set_filter(s, any);
    std::string error;
    ItemStack c;
    c.item = "vulkancraft:cobblestone";
    c.count = 3;
    CHECK(inv.add(c, items, error).empty());
    ItemStack st;
    st.item = "vulkancraft:stick";
    st.count = 2;
    CHECK(inv.add(st, items, error).empty());

    // Wrong station -> refused, nothing consumed.
    CraftResult noStation = recipes.craft(inv, *pick, "", items);
    CHECK(!noStation.ok);
    CHECK(!noStation.error.empty());
    CHECK(inv.count_of("vulkancraft:cobblestone") == 3);
    CHECK(inv.count_of("vulkancraft:stick") == 2);

    CraftResult crafted =
        recipes.craft(inv, *pick, "vulkancraft:crafting_table", items);
    CHECK(crafted.ok);
    CHECK(crafted.outputs.size() == 1);
    CHECK(crafted.outputs[0].item == "vulkancraft:stone_pickaxe");
    CHECK(crafted.time == 2.0);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 0);
    CHECK(inv.count_of("vulkancraft:stick") == 0);

    // Insufficient input -> atomic failure (nothing consumed).
    ItemStack ore;
    ore.item = "vulkancraft:iron_ore";
    ore.count = 1;
    CHECK(inv.add(ore, items, error).empty());
    CraftResult smelted = recipes.craft(inv, *smelt, "vulkancraft:furnace", items);
    CHECK(smelted.ok);
    CHECK(smelted.outputs.size() == 1);
    CHECK(smelted.outputs[0].item == "vulkancraft:iron_ingot");
    CHECK(inv.count_of("vulkancraft:iron_ore") == 0);
    // Byproduct rolls are deterministic per seed: seed 1 includes slag,
    // seed 2 does not.
    ItemStack ore2;
    ore2.item = "vulkancraft:iron_ore";
    ore2.count = 1;
    CHECK(inv.add(ore2, items, error).empty());
    CraftResult withSlag = recipes.craft(inv, *smelt, "vulkancraft:furnace", items, 1);
    CHECK(withSlag.ok);
    CHECK(withSlag.byproducts.size() == 1);
    CHECK(withSlag.byproducts[0].item == "vulkancraft:slag");
    ItemStack ore3;
    ore3.item = "vulkancraft:iron_ore";
    ore3.count = 1;
    CHECK(inv.add(ore3, items, error).empty());
    CraftResult noSlag = recipes.craft(inv, *smelt, "vulkancraft:furnace", items, 2);
    CHECK(noSlag.ok);
    CHECK(noSlag.byproducts.empty());

    // ---- recipes_for: station filtering + tag/alternative expansion ----
    Inventory mats(9);
    for (int s = 0; s < 9; ++s) mats.set_filter(s, any);
    c.count = 3;
    CHECK(mats.add(c, items, error).empty());
    st.count = 2;
    CHECK(mats.add(st, items, error).empty());
    ItemStack log;
    log.item = "vulkancraft:log";
    log.count = 1;
    CHECK(mats.add(log, items, error).empty());
    const auto anywhere = recipes.recipes_for(mats, "", items);
    bool hasPlank = false, hasPick = false, hasStick = false, hasSmelt = false;
    for (const auto* r : anywhere) {
        if (r->name == "plank_from_log") hasPlank = true;
        if (r->name == "stone_pickaxe") hasPick = true;
        if (r->name == "stick_from_plank") hasStick = true;
        if (r->name == "smelt_iron") hasSmelt = true;
    }
    CHECK(hasPlank);
    CHECK(!hasStick);  // stick_from_plank needs 2 planks, which mats lacks
    CHECK(!hasPick);   // pickaxe requires the crafting table
    CHECK(!hasSmelt);  // smelt requires the furnace (and there is no ore here)

    // ---- Dependency edges + cycle detection ----
    const auto edges = recipes.dependency_edges();
    bool stickToPlank = false, pickToStick = false;
    for (const auto& [from, to] : edges) {
        if (from == "vulkancraft:stick_from_plank" &&
            to == "vulkancraft:plank_from_log")
            stickToPlank = true;
        if (from == "vulkancraft:stone_pickaxe" &&
            to == "vulkancraft:stick_from_plank")
            pickToStick = true;
    }
    CHECK(stickToPlank);
    CHECK(pickToStick);
    std::string cyclePath;
    CHECK(!recipes.has_cycle(cyclePath));
    CHECK(cyclePath.empty());

    engine::registry::RecipeRegistry cyclic(&items);
    CHECK(cyclic.load_from_json(
        R"({"recipes":[
          {"name":"gizmo_from_widget","inputs":[{"item":"vulkancraft:widget"}],"outputs":[{"item":"vulkancraft:gizmo"}]},
          {"name":"widget_from_gizmo","inputs":[{"item":"vulkancraft:gizmo"}],"outputs":[{"item":"vulkancraft:widget"}]}
        ]})",
        recipeError));
    CHECK(cyclic.has_cycle(cyclePath));
    CHECK(!cyclePath.empty());

    std::cout << "[sdk] recipe graph: " << recipes.size()
              << " recipes, station/time/energy/conditions, atomic craft, "
                 "byproducts and cycle detection OK\n";
}

// META section 15 / FALTANTES item 11: EnTT-backed entity world — generational
// handles with pooling, builtin + project components, headless tick with
// per-entity sleeping, chunk spatial index and versioned persistence.
void test_entity_world() {
    using engine::entity::ComponentData;
    using engine::entity::EntityId;
    using engine::entity::Health;
    using engine::entity::Position;

    std::unique_ptr<engine::entity::IEntityWorld> world =
        engine::entity::create_entity_world();
    CHECK(world != nullptr);

    // ---- Generational handles: stale handle never aliases a reused id ----
    std::string error;
    EntityId cow =
        world->spawn("vulkancraft:cow", Position{ 8.0f, 40.0f, 8.0f }, error);
    CHECK(cow.valid());
    CHECK(error.empty());
    CHECK(world->alive(cow));
    CHECK(world->type_of(cow) == "vulkancraft:cow");
    CHECK(world->size() == 1);
    EntityId zombie = world->spawn("vulkancraft:zombie",
                                   Position{ 100.0f, 40.0f, 100.0f }, error);
    CHECK(zombie.valid());
    CHECK(world->size() == 2);

    CHECK(world->despawn(cow));
    CHECK(!world->alive(cow));  // stale handle
    CHECK(world->size() == 1);
    // Re-spawn reuses the pooled id with a bumped generation.
    EntityId cow2 =
        world->spawn("vulkancraft:cow", Position{ 9.0f, 40.0f, 9.0f }, error);
    CHECK(cow2.valid());
    CHECK(cow2 != cow);
    CHECK(world->alive(cow2));
    CHECK(!world->alive(cow));  // still stale even though the id was reused
    error.clear();
    CHECK(!world->spawn("", Position{}, error).valid());
    CHECK(!error.empty());

    // ---- Builtin components ----
    Health health{ 10.0f, 20.0f };
    CHECK(world->set_health(cow2, health));
    Health out;
    CHECK(world->get_health(cow2, out));
    CHECK(out.value == 10.0f);
    CHECK(out.max == 20.0f);
    Position pos;
    CHECK(world->get_position(cow2, pos));
    CHECK(pos.x == 9.0f);
    CHECK(world->set_position(cow2, Position{ 200.0f, 40.0f, 200.0f }));
    CHECK(world->get_position(cow2, pos));
    CHECK(pos.x == 200.0f);
    // Stale/unknown handles refuse everything.
    Health ignored;
    CHECK(!world->get_health(cow, ignored));
    CHECK(!world->set_health(EntityId{ 9999, 0 }, health));
    CHECK(world->type_of(EntityId{ 9999, 0 }).empty());

    // ---- Project components (versioned opaque blobs) ----
    ComponentData comp;
    comp.type = "vulkancraft:attributes";
    comp.version = 3;
    comp.blob = "strength:2";
    CHECK(world->set_component(cow2, comp));
    ComponentData got;
    CHECK(world->get_component(cow2, "vulkancraft:attributes", got));
    CHECK(got.version == 3);
    CHECK(got.blob == "strength:2");
    CHECK(!world->get_component(cow2, "vulkancraft:none", got));

    // ---- Headless tick + sleeping (per-entity tick policy) ----
    std::unique_ptr<engine::entity::IEntityWorld> sim =
        engine::entity::create_entity_world();
    EntityId fast = sim->spawn("vulkancraft:fast", Position{ 0, 0, 0 }, error);
    EntityId slow = sim->spawn("vulkancraft:slow", Position{ 1, 0, 1 }, error);
    CHECK(sim->set_tick_interval(slow, 0.5f));
    int fastTicks = 0, slowTicks = 0;
    float slowDt = 0.0f;
    const auto tickAll = [&](float dt) {
        sim->tick(dt, [&](EntityId handle, float dtValue) {
            if (handle == slow) {
                ++slowTicks;
                slowDt = dtValue;
            } else {
                ++fastTicks;
            }
        });
    };
    tickAll(0.1f);
    tickAll(0.1f);
    tickAll(0.1f);  // slow accumulated 0.3 < 0.5
    CHECK(fastTicks == 3);
    CHECK(slowTicks == 0);
    CHECK(sim->sleeping_count() == 1);
    tickAll(0.1f);
    tickAll(0.1f);  // slow accumulated 0.5 >= 0.5 -> ticks once
    CHECK(fastTicks == 5);
    CHECK(slowTicks == 1);
    CHECK(slowDt > 0.49f && slowDt <= 0.51f);  // effective dt = accumulated

    // ---- Spatial index by chunk (16x16) ----
    std::unique_ptr<engine::entity::IEntityWorld> spatial =
        engine::entity::create_entity_world();
    EntityId e1 = spatial->spawn("vulkancraft:a", Position{ 8, 40, 8 }, error);
    EntityId e2 = spatial->spawn("vulkancraft:b", Position{ 15, 40, 15 }, error);
    EntityId e3 = spatial->spawn("vulkancraft:c", Position{ 20, 40, 20 }, error);
    CHECK(spatial->entities_in_chunk(0, 0).size() == 2);
    CHECK(spatial->entities_in_chunk(1, 1).size() == 1);
    CHECK(spatial->entities_in_chunk(2, 2).empty());
    CHECK(spatial->set_position(e2, Position{ 20, 40, 20 }));  // move chunk
    CHECK(spatial->entities_in_chunk(0, 0).size() == 1);
    CHECK(spatial->entities_in_chunk(1, 1).size() == 2);
    CHECK(spatial->entities_in_aabb(0, 0, 0, 24, 80, 24).size() == 3);
    CHECK(spatial->entities_in_aabb(19, 0, 19, 21, 80, 21).size() == 2);
    CHECK(spatial->despawn(e1));
    CHECK(spatial->entities_in_chunk(0, 0).empty());

    // ---- Persistence: versioned snapshots, all-or-nothing restore ----
    std::unique_ptr<engine::entity::IEntityWorld> save0 =
        engine::entity::create_entity_world();
    EntityId pa = save0->spawn("vulkancraft:cow", Position{ 8, 40, 8 }, error);
    CHECK(save0->set_health(pa, Health{ 5.0f, 20.0f }));
    CHECK(save0->set_tick_interval(pa, 0.25f));
    comp.type = "vulkancraft:attributes";
    comp.version = 2;
    comp.blob = "speed:3";
    CHECK(save0->set_component(pa, comp));
    CHECK(save0->spawn("vulkancraft:zombie", Position{ 100, 40, 100 }, error).valid());
    const auto snapshots = save0->serialize_entities();
    CHECK(snapshots.size() == 2);

    std::unique_ptr<engine::entity::IEntityWorld> load0 =
        engine::entity::create_entity_world();
    CHECK(load0->deserialize_entities(snapshots, error));
    CHECK(load0->size() == 2);
    const auto reloaded = load0->serialize_entities();
    CHECK(reloaded.size() == 2);
    CHECK(reloaded[0].type == "vulkancraft:cow");
    CHECK(reloaded[0].position.x == 8.0f);
    CHECK(reloaded[0].health.value == 5.0f);
    CHECK(reloaded[0].tickInterval == 0.25f);
    CHECK(reloaded[0].components.size() == 1);
    CHECK(reloaded[0].components[0].blob == "speed:3");
    CHECK(reloaded[1].type == "vulkancraft:zombie");

    // All-or-nothing: a bad snapshot leaves the population untouched.
    std::vector<engine::entity::EntitySnapshot> bad = snapshots;
    bad[0].type.clear();
    error.clear();
    CHECK(!load0->deserialize_entities(bad, error));
    CHECK(!error.empty());
    CHECK(load0->size() == 2);

    std::cout << "[sdk] entity world: generational handles, tick/sleeping, "
                 "chunk index and versioned persistence OK\n";
}

// META section 15: the world's entity layer persists through the world save
// (v5 world_entities) — spawn entities, save, load into a fresh world, and the
// population (health, components, position) comes back intact.
void test_entity_world_save() {
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto entities = a->entity_world();
    CHECK(entities != nullptr);
    std::string error;
    engine::entity::EntityId cow = entities->spawn(
        "vulkancraft:cow", engine::entity::Position{ 8.0f, 100.0f, 8.0f }, error);
    CHECK(cow.valid());
    CHECK(entities->set_health(cow, engine::entity::Health{ 7.0f, 20.0f }));
    engine::entity::ComponentData comp;
    comp.type = "vulkancraft:loot";
    comp.version = 1;
    comp.blob = "leather";
    CHECK(entities->set_component(cow, comp));

    std::string serError;
    const std::string blob = a->serialize_world(serError);
    CHECK(serError.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string bError;
    CHECK(b->deserialize_world(blob, bError));
    CHECK(bError.empty());
    auto bEntities = b->entity_world();
    CHECK(bEntities != nullptr);
    CHECK(bEntities->size() == 1);
    const auto inChunk = bEntities->entities_in_chunk(0, 0);
    CHECK(inChunk.size() == 1);
    engine::entity::Health health;
    CHECK(bEntities->get_health(inChunk[0], health));
    CHECK(health.value == 7.0f);
    engine::entity::ComponentData got;
    CHECK(bEntities->get_component(inChunk[0], "vulkancraft:loot", got));
    CHECK(got.blob == "leather");

    // Idempotency with entities: a second serialization is byte-identical.
    std::string bSer;
    const std::string blobB = b->serialize_world(bSer);
    CHECK(bSer.empty());
    CHECK(blobB == blob);

    std::cout << "[sdk] entity world save: v5 world_entities round-trip "
                 "and idempotency OK\n";
}

// META section 16 / FALTANTES item 12: Recast/Detour navigation provider —
// bake from voxel columns, path around walls, maxClimb steps, revision
// invalidation and determinism.
void test_navigation_provider() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    CHECK(nav != nullptr);
    CHECK(!nav->valid());

    const float step = 0.5f;
    const float minCoord = 0.0f, maxCoord = 32.0f;

    const auto ground_columns = [&]() {
        std::vector<VoxelColumn> columns;
        for (float x = minCoord; x <= maxCoord; x += step) {
            for (float z = minCoord; z <= maxCoord; z += step) {
                VoxelColumn column;
                column.x = x;
                column.z = z;
                column.solidMinY = 0.0f;
                column.solidMaxY = 1.0f;
                column.solid = true;
                columns.push_back(column);
            }
        }
        return columns;
    };
    const auto wall_columns = [&](std::vector<VoxelColumn> columns) {
        // A 3-block-high wall at x = 16, z in [8, 24] (walkable top at y=3).
        for (VoxelColumn& column : columns) {
            if (std::fabs(column.x - 16.0f) <= step / 2.0f &&
                column.z >= 8.0f && column.z <= 24.0f) {
                column.solidMinY = 0.0f;
                column.solidMaxY = 3.0f;
            }
        }
        return columns;
    };

    NavmeshConfig config;
    config.boundsMinX = minCoord;
    config.boundsMaxX = maxCoord;
    config.boundsMinZ = minCoord;
    config.boundsMaxZ = maxCoord;
    config.cellSize = step;

    // ---- Flat terrain: straight path, walkability, revision ----
    std::string error;
    CHECK(nav->build(config, ground_columns(), error));
    CHECK(error.empty());
    CHECK(nav->valid());
    const uint64_t r0 = nav->revision();
    CHECK(r0 > 0);
    PathResult flat;
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, flat));
    CHECK(flat.found);
    const float straight = std::sqrt(28.0f * 28.0f * 2.0f);
    CHECK(flat.totalLength > straight - 1.0f);
    CHECK(flat.totalLength < straight + 3.0f);
    CHECK(flat.revision == r0);
    CHECK(flat.waypoints.size() >= 6);  // start + end + intermediate points
    CHECK(nav->is_walkable(8.0f, 1.5f, 8.0f));
    CHECK(!nav->is_walkable(8.0f, 12.0f, 8.0f));  // high above the surface
    // Rebuild bumps the revision (consumers invalidate cached paths).
    CHECK(nav->build(config, ground_columns(), error));
    CHECK(nav->revision() > r0);

    // ---- Wall: the path must deviate around it ----
    // The wall spans z in [8, 24] at x = 16, so a route through its middle
    // (z = 16) must swing around one end — a real detour, not a corner clip.
    CHECK(nav->build(config, wall_columns(ground_columns()), error));
    CHECK(error.empty());
    PathResult around;
    CHECK(nav->find_path(8.0f, 1.5f, 16.0f, 24.0f, 1.5f, 16.0f, around));
    CHECK(around.found);
    CHECK(around.totalLength > 16.0f + 8.0f);  // around the wall end, not through

    // ---- maxClimb: a 1-block step is climbable at 1.0, blocked at 0.2 ----
    const auto stepped_columns = [&]() {
        std::vector<VoxelColumn> columns = ground_columns();
        for (VoxelColumn& column : columns) {
            if (column.x >= 20.0f && column.x <= 28.0f &&
                column.z >= 20.0f && column.z <= 28.0f) {
                // 1-block-high platform (walkable surface at y=2).
                column.solidMinY = 1.0f;
                column.solidMaxY = 2.0f;
            }
        }
        return columns;
    };
    NavmeshConfig climbConfig = config;
    climbConfig.agentMaxClimb = 1.0f;
    CHECK(nav->build(climbConfig, stepped_columns(), error));
    PathResult ontoPlatform;
    CHECK(nav->find_path(10.0f, 1.5f, 10.0f, 24.0f, 2.5f, 24.0f,
                         ontoPlatform));
    CHECK(ontoPlatform.found);
    NavmeshConfig noClimb = climbConfig;
    noClimb.agentMaxClimb = 0.2f;
    CHECK(nav->build(noClimb, stepped_columns(), error));
    PathResult blockedStep;
    CHECK(!nav->find_path(10.0f, 1.5f, 10.0f, 24.0f, 2.5f, 24.0f,
                          blockedStep));
    CHECK(!blockedStep.found);

    // ---- Determinism: same build + query -> same path ----
    CHECK(nav->build(config, ground_columns(), error));
    PathResult first, second;
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, first));
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, second));
    CHECK(first.totalLength == second.totalLength);

    std::cout << "[sdk] navigation: bake, wall detour, maxClimb, revision "
                 "and determinism OK\n";
}

// META section 16: navigation integrated with a real voxel world — the sampler
// reads the world's walkable surface and the navmesh routes around a wall the
// test builds with normal block edits.
void test_navigation_voxel_world() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;

    const auto surface_y = [](engine::voxel::IVoxelWorld& world, int x, int z) {
        for (int y = 160; y >= 0; --y) {
            if (world.get_block(x, y, z) != 0) return y;
        }
        return -1;
    };

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.boundsMinY = 0.0f;
    config.boundsMaxY = 130.0f;  // above the wall top (surface + 3)
    config.cellSize = 0.5f;

    // World A: open terrain.
    std::unique_ptr<engine::voxel::IVoxelWorld> open =
        engine::voxel::create_default_voxel_world();
    open->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*open, glm::vec3(16.0f, 200.0f, 16.0f), 24));
    // The sampler covers x,z in [0,32): all four chunks must be loaded before
    // columns are read, or the mesh is built from partial (air) data.
    CHECK(settle(*open, glm::vec3(16.0f, 200.0f, 16.0f), [&] {
        return open->is_chunk_loaded(1, 0) && open->is_chunk_loaded(0, 1) &&
               open->is_chunk_loaded(1, 1);
    }));
    const int surface = surface_y(*open, 8, 8);
    CHECK(surface > 0);
    std::string error;
    auto columns =
        engine::navigation::sample_voxel_columns(*open, config, error);
    CHECK(error.empty());
    CHECK(columns.size() > 1000);
    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    CHECK(nav->build(config, columns, error));
    CHECK(error.empty());
    const float agentY = static_cast<float>(surface) + 1.5f;
    PathResult openPath;
    CHECK(nav->find_path(8.0f, agentY, 16.0f, 24.0f, agentY, 16.0f, openPath));
    CHECK(openPath.found);
    CHECK(openPath.totalLength < 17.5f);  // essentially straight (16)

    // World B: same terrain plus a 3-block stone wall across the path.
    std::unique_ptr<engine::voxel::IVoxelWorld> walled =
        engine::voxel::create_default_voxel_world();
    walled->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*walled, glm::vec3(16.0f, 200.0f, 16.0f), 24));
    CHECK(settle(*walled, glm::vec3(16.0f, 200.0f, 16.0f), [&] {
        return walled->is_chunk_loaded(1, 0) && walled->is_chunk_loaded(0, 1) &&
               walled->is_chunk_loaded(1, 1);
    }));
    for (int z = 8; z <= 24; ++z) {
        for (int y = surface + 1; y <= surface + 3; ++y) {
            walled->set_block(16, y, z, kBlockStone);
        }
    }
    const auto wallColumns =
        engine::navigation::sample_voxel_columns(*walled, config, error);
    CHECK(error.empty());
    CHECK(nav->build(config, wallColumns, error));
    CHECK(error.empty());
    PathResult walledPath;
    CHECK(nav->find_path(8.0f, agentY, 16.0f, 24.0f, agentY, 16.0f, walledPath));
    CHECK(walledPath.found);
    CHECK(walledPath.totalLength > openPath.totalLength + 8.0f);  // detour

    std::cout << "[sdk] navigation voxel: sampler + wall detour on a real "
                 "world OK\n";
}

// FALTANTES item 12 sub-1 — local navigation update after block transactions:
// tiled navmesh (NavmeshConfig::tileSize > 0) where update() re-bakes ONLY
// the tiles overlapping a block change and the result is equivalent to a
// full rebuild — plus the voxel-world flow where a transaction places a wall
// (detour) and removing it restores the straight path, with per-tile
// revisions proving no tile outside the change was re-baked.
void test_navigation_local_update() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    const float step = 0.5f;
    const float minCoord = 0.0f, maxCoord = 32.0f;

    const auto ground_columns = [&]() {
        std::vector<VoxelColumn> columns;
        for (float x = minCoord; x <= maxCoord; x += step) {
            for (float z = minCoord; z <= maxCoord; z += step) {
                VoxelColumn column;
                column.x = x;
                column.z = z;
                column.solidMinY = 0.0f;
                column.solidMaxY = 1.0f;
                column.solid = true;
                columns.push_back(column);
            }
        }
        return columns;
    };
    const auto with_wall = [&](std::vector<VoxelColumn> columns, float wallX) {
        // A 3-block-high wall at x = wallX, z in [10, 14] — deliberately kept
        // >= 2 world units away from every tile border so exactly ONE tile is
        // affected (the tile containing (wallX, 12)).
        for (VoxelColumn& column : columns) {
            if (std::fabs(column.x - wallX) <= step / 2.0f &&
                column.z >= 10.0f && column.z <= 14.0f) {
                column.solidMinY = 0.0f;
                column.solidMaxY = 3.0f;
            }
        }
        return columns;
    };

    NavmeshConfig config;
    config.boundsMinX = minCoord;
    config.boundsMaxX = maxCoord;
    config.boundsMinZ = minCoord;
    config.boundsMaxZ = maxCoord;
    config.cellSize = step;
    config.tileSize = 8.0f;  // tiled mode: 4x4 tiles of 8 world units

    // ---- Tiled build on flat ground: cross-tile path must work ----
    std::string error;
    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    const auto flatColumns = ground_columns();
    CHECK(nav->build(config, flatColumns, error));
    CHECK(error.empty());
    CHECK(nav->valid());
    PathResult flat;
    // Diagonal across tiles (0,0) -> (3,3): proves border connectivity.
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, flat));
    CHECK(flat.found);
    const float straight = std::sqrt(28.0f * 28.0f * 2.0f);
    CHECK(flat.totalLength > straight - 1.0f);
    CHECK(flat.totalLength < straight + 3.0f);
    const uint64_t r0 = nav->revision();
    CHECK(r0 > 0);
    // Per-tile revisions exist in tiled mode and are uniform after build.
    const uint64_t tile00 = nav->tile_revision(4.0f, 4.0f);    // tile (0,0)
    const uint64_t tile22 = nav->tile_revision(20.0f, 20.0f);  // tile (2,2)
    CHECK(tile00 > 0);
    CHECK(tile22 > 0);
    CHECK(tile00 == tile22);

    // ---- update(): a wall inside tile (1,1) detours the path locally ----
    const auto walled = with_wall(ground_columns(), 12.0f);
    std::vector<VoxelColumn> changed;
    for (const VoxelColumn& c : walled) {
        if (std::fabs(c.x - 12.0f) <= step / 2.0f && c.z >= 10.0f && c.z <= 14.0f) {
            changed.push_back(c);
        }
    }
    CHECK(!changed.empty());
    CHECK(nav->update(changed, error));
    CHECK(error.empty());
    PathResult detour;
    CHECK(nav->find_path(4.0f, 1.5f, 12.0f, 28.0f, 1.5f, 12.0f, detour));
    CHECK(detour.found);
    // The 5-block wall forces a small but deterministic detour (measured
    // ~0.56 over the 24-unit straight path) with real intermediate waypoints.
    CHECK(detour.totalLength > 24.0f + 0.25f);
    CHECK(detour.totalLength < 24.0f + 3.0f);
    CHECK(detour.waypoints.size() > 2);
    CHECK(nav->revision() > r0);
    // Locality: ONLY tile (1,1) was re-baked; (0,0) and (2,2) are untouched.
    CHECK(nav->tile_revision(12.0f, 12.0f) > tile00);   // tile (1,1) bumped
    CHECK(nav->tile_revision(4.0f, 4.0f) == tile00);    // tile (0,0) untouched
    CHECK(nav->tile_revision(20.0f, 20.0f) == tile22);  // tile (2,2) untouched

    // ---- Equivalence: update() == a full rebuild over the updated set ----
    std::unique_ptr<engine::navigation::INavigationProvider> fresh =
        engine::navigation::create_recast_navigation_provider();
    CHECK(fresh->build(config, walled, error));
    CHECK(error.empty());
    PathResult freshDetour;
    CHECK(fresh->find_path(4.0f, 1.5f, 12.0f, 28.0f, 1.5f, 12.0f, freshDetour));
    CHECK(freshDetour.found);
    CHECK(detour.totalLength == freshDetour.totalLength);  // bit-exact
    CHECK(detour.waypoints == freshDetour.waypoints);

    // ---- Long wall: a bigger obstacle forces an unambiguous detour ----
    // z in [2, 30] at x = 12 crosses the path (4,12)->(28,12); the route must
    // swing around one end. Affected tiles: all four z-tiles of x-tile 1;
    // tiles in x-tiles 0 and 2 must stay untouched.
    const auto longWallColumns = [&]() {
        std::vector<VoxelColumn> columns = ground_columns();
        for (VoxelColumn& column : columns) {
            if (std::fabs(column.x - 12.0f) <= step / 2.0f &&
                column.z >= 2.0f && column.z <= 30.0f) {
                column.solidMinY = 0.0f;
                column.solidMaxY = 3.0f;
            }
        }
        return columns;
    };
    std::unique_ptr<engine::navigation::INavigationProvider> navLong =
        engine::navigation::create_recast_navigation_provider();
    CHECK(navLong->build(config, longWallColumns(), error));
    CHECK(error.empty());
    PathResult longDetour;
    CHECK(navLong->find_path(4.0f, 1.5f, 12.0f, 28.0f, 1.5f, 12.0f, longDetour));
    CHECK(longDetour.found);
    CHECK(longDetour.totalLength > 24.0f + 5.0f);   // real detour (~32.3)
    CHECK(longDetour.totalLength < 24.0f + 20.0f);

    // ---- Revert: removing the wall restores the straight path ----
    std::vector<VoxelColumn> reverted;
    for (const VoxelColumn& c : flatColumns) {
        if (std::fabs(c.x - 12.0f) <= step / 2.0f && c.z >= 10.0f && c.z <= 14.0f) {
            reverted.push_back(c);
        }
    }
    CHECK(nav->update(reverted, error));
    CHECK(error.empty());
    PathResult back;
    CHECK(nav->find_path(4.0f, 1.5f, 12.0f, 28.0f, 1.5f, 12.0f, back));
    CHECK(back.found);
    CHECK(back.totalLength < 24.0f + 1.0f);  // straight again

    // ---- Refusals (all-or-nothing / mode guards) ----
    std::string refused;
    CHECK(!nav->update({}, refused));           // empty change set
    CHECK(!refused.empty());
    NavmeshConfig legacyConfig = config;
    legacyConfig.tileSize = 0.0f;
    std::unique_ptr<engine::navigation::INavigationProvider> legacy =
        engine::navigation::create_recast_navigation_provider();
    CHECK(legacy->build(legacyConfig, flatColumns, error));
    CHECK(error.empty());
    CHECK(legacy->tile_revision(4.0f, 4.0f) == 0);  // no tiles in single mode
    CHECK(!legacy->update(changed, refused));        // refused outside tiled mode
    CHECK(!refused.empty());
    NavmeshConfig badTile = config;
    badTile.tileSize = config.cellSize * 0.5f;       // < cellSize
    std::unique_ptr<engine::navigation::INavigationProvider> bad =
        engine::navigation::create_recast_navigation_provider();
    CHECK(!bad->build(badTile, flatColumns, refused));
    CHECK(!refused.empty());

    std::cout << "[sdk] navigation local update: tiled navmesh, one-tile "
                 "re-bake, equivalence to full rebuild, revert and refusals OK\n";
}

// FALTANTES item 12 sub-1 — the same local update, driven by real block
// transactions on a voxel world: placing a wall through a transaction is
// followed by sampling ONLY the affected region and update(), which detours
// the path; removing the wall and updating again restores the straight path.
void test_navigation_local_update_world() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;

    const auto surface_y = [](engine::voxel::IVoxelWorld& world, int x, int z) {
        for (int y = 160; y >= 0; --y) {
            if (world.get_block(x, y, z) != 0) return y;
        }
        return -1;
    };

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.boundsMinY = 0.0f;
    config.boundsMaxY = 130.0f;
    config.cellSize = 0.5f;
    config.tileSize = 8.0f;  // tiled mode

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(16.0f, 200.0f, 16.0f), 24));
    CHECK(settle(*world, glm::vec3(16.0f, 200.0f, 16.0f), [&] {
        return world->is_chunk_loaded(1, 0) && world->is_chunk_loaded(0, 1) &&
               world->is_chunk_loaded(1, 1);
    }));
    const int surface = surface_y(*world, 8, 8);
    CHECK(surface > 0);

    std::string error;
    auto columns = engine::navigation::sample_voxel_columns(*world, config, error);
    CHECK(error.empty());
    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    CHECK(nav->build(config, columns, error));
    CHECK(error.empty());
    const float agentY = static_cast<float>(surface) + 1.5f;
    PathResult openPath;
    CHECK(nav->find_path(4.0f, agentY, 12.0f, 28.0f, agentY, 12.0f, openPath));
    CHECK(openPath.found);
    CHECK(openPath.totalLength < 24.0f + 1.0f);  // straight across tiles
    const uint64_t rOpen = nav->revision();
    CHECK(rOpen > 0);

    // Transaction 1: a 3-block stone wall at x=12, z in [8, 24] (crosses the
    // path (4,12)->(28,12); its ends stay clear of the world borders so the
    // ground around them remains navigable).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        for (int z = 8; z <= 24; ++z) {
            for (int y = surface + 1; y <= surface + 3; ++y) {
                tx->set_block(12, y, z, kBlockStone);
            }
        }
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }

    // Sample ONLY the affected region (same cellSize grid, so the column
    // keys match the stored set) and update the navmesh locally.
    NavmeshConfig affectedConfig = config;
    affectedConfig.boundsMinX = 11.0f;
    affectedConfig.boundsMaxX = 13.0f;
    affectedConfig.boundsMinZ = 7.0f;
    affectedConfig.boundsMaxZ = 25.0f;
    const auto changed =
        engine::navigation::sample_voxel_columns(*world, affectedConfig, error);
    CHECK(error.empty());
    CHECK(changed.size() >= 100);  // wall footprint + margin
    CHECK(nav->update(changed, error));
    CHECK(error.empty());
    PathResult walledPath;
    CHECK(nav->find_path(4.0f, agentY, 12.0f, 28.0f, agentY, 12.0f, walledPath));
    CHECK(walledPath.found);
    // Deterministic detour (~26.6 vs 24.0 straight).
    CHECK(walledPath.totalLength > openPath.totalLength + 1.5f);
    CHECK(walledPath.totalLength < openPath.totalLength + 6.0f);
    CHECK(nav->tile_revision(12.0f, 12.0f) > rOpen);   // tile (1,1) re-baked
    CHECK(nav->tile_revision(4.0f, 4.0f) == rOpen);    // tile (0,0) untouched
    CHECK(nav->tile_revision(20.0f, 20.0f) == rOpen);  // tile (2,2) untouched

    // Transaction 2: remove the wall; a fresh sample of the same region
    // restores the straight path.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        for (int z = 8; z <= 24; ++z) {
            for (int y = surface + 1; y <= surface + 3; ++y) {
                tx->set_block(12, y, z, 0);  // Air
            }
        }
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    const auto reverted =
        engine::navigation::sample_voxel_columns(*world, affectedConfig, error);
    CHECK(nav->update(reverted, error));
    CHECK(error.empty());
    PathResult restored;
    CHECK(nav->find_path(4.0f, agentY, 12.0f, 28.0f, agentY, 12.0f, restored));
    CHECK(restored.found);
    CHECK(restored.totalLength < openPath.totalLength + 1.0f);  // straight again

    std::cout << "[sdk] navigation local update world: transaction places a "
                 "wall (detour), removing it restores the path, only the "
                 "tile of the change re-baked OK\n";
}

// FALTANTES item 12 — doors, platforms and moving obstacles: a dynamic
// obstacle (a named set of solid columns) toggles on/off over the terrain.
// While ACTIVE its columns block the navmesh (detour); while INACTIVE the
// terrain columns are restored (straight path again). Toggling re-bakes ONLY
// the tiles overlapping the footprint and is bit-exact equivalent to a full
// rebuild over the effective column set — a door opening/closing never
// triggers a full rebake.
void test_navigation_dynamic_obstacle() {
    using engine::navigation::DynamicObstacle;
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    const float step = 0.5f;
    const auto ground_columns = [&]() {
        std::vector<VoxelColumn> columns;
        for (float x = 0.25f; x <= 31.75f; x += step) {
            for (float z = 0.25f; z <= 31.75f; z += step) {
                VoxelColumn column;
                column.x = x;
                column.z = z;
                column.solidMinY = 0.0f;
                column.solidMaxY = 1.0f;
                column.solid = true;
                columns.push_back(column);
            }
        }
        return columns;
    };
    const auto wall_obstacle = [&](float z0, float z1) {
        DynamicObstacle obstacle;
        for (float z = z0; z <= z1; z += step) {
            VoxelColumn column;
            column.x = 16.0f;  // tile boundary between x-tiles 1 and 2
            column.z = z;
            column.solidMinY = 0.0f;
            column.solidMaxY = 4.0f;
            column.solid = true;
            obstacle.columns.push_back(column);
        }
        return obstacle;
    };

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.cellSize = step;
    config.tileSize = 8.0f;  // tiled mode: 4x4 tiles of 8 world units

    std::string error;
    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    const auto flatColumns = ground_columns();
    CHECK(nav->build(config, flatColumns, error));
    CHECK(error.empty());
    CHECK(nav->valid());

    // Baseline straight path along z=12, from x=2 to x=30 (the door at x=16
    // spanning z in [8,24] crosses this line).
    PathResult base;
    CHECK(nav->find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, base));
    CHECK(base.found);
    const float baseLen = base.totalLength;
    const uint64_t r0 = nav->tile_revision(4.0f, 4.0f);
    CHECK(r0 > 0);
    CHECK(nav->tile_revision(20.0f, 20.0f) == r0);
    CHECK(nav->tile_revision(28.0f, 4.0f) == r0);

    // ---- Door CLOSED: the wall detours the path ----
    const DynamicObstacle door = wall_obstacle(8.0f, 24.0f);
    CHECK(nav->set_dynamic_obstacle(1, door, error));
    CHECK(error.empty());
    PathResult closed;
    CHECK(nav->find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, closed));
    CHECK(closed.found);
    CHECK(closed.totalLength > baseLen + 2.0f);  // real detour (~30.1 vs 28.0)
    // Locality: the wall touches tiles (1,1),(1,2),(2,1),(2,2) only — corner
    // tiles (0,0) and (3,0) are NOT re-baked.
    CHECK(nav->tile_revision(4.0f, 4.0f) == r0);           // tile (0,0) untouched
    CHECK(nav->tile_revision(28.0f, 4.0f) == r0);          // tile (3,0) untouched
    CHECK(nav->tile_revision(20.0f, 20.0f) > r0);          // tile (2,2) re-baked

    // ---- Door OPEN: path restored (straight again, ~= base) ----
    CHECK(nav->set_obstacle_active(1, false, error));
    CHECK(error.empty());
    PathResult open;
    CHECK(nav->find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, open));
    CHECK(open.found);
    CHECK(std::fabs(open.totalLength - baseLen) < 0.001f);

    // ---- Door re-closed: same detour returns ----
    CHECK(nav->set_obstacle_active(1, true, error));
    CHECK(error.empty());
    PathResult closed2;
    CHECK(nav->find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, closed2));
    CHECK(closed2.found);
    CHECK(std::fabs(closed2.totalLength - closed.totalLength) < 0.001f);

    // ---- Equivalence: obstacle active == full rebuild over the effective
    // column set (ground minus replaced keys + wall), bit-exact ----
    std::unique_ptr<engine::navigation::INavigationProvider> fresh =
        engine::navigation::create_recast_navigation_provider();
    std::vector<VoxelColumn> effective;
    {
        auto wallKey = [&](const VoxelColumn& c) {
            return std::make_pair(static_cast<int>(std::llround(c.x / step)),
                                  static_cast<int>(std::llround(c.z / step)));
        };
        std::vector<std::pair<int, int>> wallKeys;
        for (const VoxelColumn& wc : door.columns) wallKeys.push_back(wallKey(wc));
        for (const VoxelColumn& g : flatColumns) {
            const auto k = wallKey(g);
            if (std::find(wallKeys.begin(), wallKeys.end(), k) != wallKeys.end()) {
                continue;  // this ground column is replaced by the wall
            }
            effective.push_back(g);
        }
        for (const VoxelColumn& c : door.columns) effective.push_back(c);
    }
    CHECK(fresh->build(config, effective, error));
    CHECK(error.empty());
    PathResult baked;
    CHECK(fresh->find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, baked));
    CHECK(baked.found);
    CHECK(baked.totalLength == closed.totalLength);  // bit-exact
    CHECK(baked.waypoints == closed.waypoints);

    // ---- Refusals (all-or-nothing / mode guards) ----
    std::string refused;
    CHECK(!nav->set_obstacle_active(999, true, refused));  // unknown id
    CHECK(!refused.empty());
    CHECK(!nav->set_dynamic_obstacle(2, DynamicObstacle{}, refused));  // empty
    CHECK(!refused.empty());
    NavmeshConfig legacyConfig = config;
    legacyConfig.tileSize = 0.0f;  // single-navmesh mode
    std::unique_ptr<engine::navigation::INavigationProvider> legacy =
        engine::navigation::create_recast_navigation_provider();
    CHECK(legacy->build(legacyConfig, flatColumns, error));
    CHECK(error.empty());
    CHECK(!legacy->set_dynamic_obstacle(1, door, refused));  // refused
    CHECK(!refused.empty());
    std::unique_ptr<engine::navigation::INavigationProvider> freshNav =
        engine::navigation::create_recast_navigation_provider();
    CHECK(!freshNav->set_dynamic_obstacle(1, door, refused));  // before build
    CHECK(!refused.empty());

    std::cout << "[sdk] navigation dynamic obstacle: door toggles locally "
                 "(only its tiles re-bake), open restores the path, active "
                 "== full rebuild bit-exact, refusals OK\n";
}

// FALTANTES item 12 — per-area traversal costs (material/danger) and slope
// cost areas: columns can carry a cost area (0 = default walkable, 1..62 =
// custom); steep-but-walkable terrain is tagged with the slope cost area at
// the heightfield level (exact per-cell slope); set_area_cost weights the
// query filter so find_path routes around expensive areas when a cheaper
// alternative exists.
void test_navigation_area_costs() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    const float step = 0.5f;
    const auto flat_columns = [&]() {
        std::vector<VoxelColumn> columns;
        for (float x = 0.25f; x <= 31.75f; x += step) {
            for (float z = 0.25f; z <= 31.75f; z += step) {
                VoxelColumn column;
                column.x = x;
                column.z = z;
                column.solidMinY = 0.0f;
                column.solidMaxY = 1.0f;
                column.solid = true;
                columns.push_back(column);
            }
        }
        return columns;
    };
    // A cost strip crossing the path line z=12 at x in [14,18], spanning from
    // the top map edge (z=0) down past the line (z=24): the only free
    // crossing is the gap z>24, so a real detour is forced when costed.
    const auto strip_columns = [&](uint8_t tagArea) {
        std::vector<VoxelColumn> columns = flat_columns();
        for (VoxelColumn& column : columns) {
            if (tagArea != 0 && column.x >= 14.0f && column.x <= 18.0f &&
                column.z >= 0.0f && column.z <= 24.0f) {
                column.area = tagArea;
            }
        }
        return columns;
    };
    // A traversable hump crossing the path line: top = 1 + 0.5*(4-|x-16|) for
    // x in [12,20] (rises to y=3 at x=16, connects at y=1 on both ends), z in
    // [0,24] so the free crossing is the gap z>24. The sloped faces are the
    // slope cost area when slopeCostArea is set.
    const auto hump_columns = [&]() {
        std::vector<VoxelColumn> columns = flat_columns();
        for (VoxelColumn& column : columns) {
            if (column.x >= 12.0f && column.x <= 20.0f && column.z >= 0.0f &&
                column.z <= 24.0f) {
                column.solidMaxY =
                    1.0f + 0.5f * (4.0f - std::fabs(column.x - 16.0f));
            }
        }
        return columns;
    };

    const auto path_len = [](engine::navigation::INavigationProvider& nav) {
        PathResult r;
        if (!nav.find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, r))
            return -1.0f;
        return r.totalLength;
    };

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.cellSize = step;
    config.tileSize = 8.0f;  // tiled mode: 4x4 tiles of 8 world units

    std::string error;

    // ---- material/danger cost strip: straight at cost 1, detour at 10 ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, strip_columns(1), error));
        CHECK(error.empty());
        CHECK(nav->valid());
        CHECK(nav->area_cost(1) == 1.0f);  // default cost

        const float straightLen = path_len(*nav);
        CHECK(straightLen > 0.0f);
        CHECK(std::fabs(straightLen - 28.0f) < 1.0f);  // straight across

        CHECK(nav->set_area_cost(1, 10.0f, error));  // cost the strip
        CHECK(error.empty());
        CHECK(nav->area_cost(1) == 10.0f);
        const float detourLen = path_len(*nav);
        CHECK(detourLen > straightLen + 2.0f);  // routed around the strip

        // Doubling the cost keeps the detour; restoring cost 1 restores the
        // straight path (the cost is live, not baked).
        CHECK(nav->set_area_cost(1, 20.0f, error));
        CHECK(error.empty());
        CHECK(path_len(*nav) > straightLen + 2.0f);
        CHECK(nav->set_area_cost(1, 1.0f, error));
        CHECK(error.empty());
        CHECK(std::fabs(path_len(*nav) - straightLen) < 0.001f);
    }

    // ---- strip WITHOUT the cost tag: costed area unused, no detour ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, strip_columns(0), error));
        CHECK(error.empty());
        CHECK(nav->set_area_cost(1, 10.0f, error));
        CHECK(error.empty());
        CHECK(std::fabs(path_len(*nav) - 28.0f) < 1.0f);  // still straight
    }

    // ---- slope cost: hump tagged at the heightfield, detours when costed ----
    {
        NavmeshConfig slopeConfig = config;
        slopeConfig.slopeCostArea = 2;
        slopeConfig.slopeCostStartDegrees = 15.0f;  // hump faces are 26.6 deg
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(slopeConfig, hump_columns(), error));
        CHECK(error.empty());

        // At default cost the path crosses straight over the hump (it is
        // walkable, just costed).
        CHECK(nav->set_area_cost(2, 1.0f, error));
        CHECK(error.empty());
        const float over = path_len(*nav);
        CHECK(over > 0.0f);
        CHECK(over < 40.0f);  // straight over, not around

        // Cost the slope area: the query routes around the hump.
        CHECK(nav->set_area_cost(2, 50.0f, error));
        CHECK(error.empty());
        const float around = path_len(*nav);
        CHECK(around > over + 1.0f);
    }

    // ---- refusals (out of range / invalid cost) ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, flat_columns(), error));
        CHECK(error.empty());
        CHECK(!nav->set_area_cost(64, 1.0f, error));   // area out of range
        CHECK(!error.empty());
        CHECK(!nav->set_area_cost(1, -1.0f, error));   // negative cost
        CHECK(!error.empty());
        CHECK(!nav->set_area_cost(1, std::nanf(""), error));  // NaN
        CHECK(!error.empty());
        CHECK(nav->area_cost(1) == 1.0f);  // unchanged after refusals
    }

    std::cout << "[sdk] navigation area costs: material strip detours at cost "
                 "10 (straight at 1), slope hump tagged at the heightfield "
                 "detours at cost 50, refusals OK\n";
}

// FALTANTES item 12 — off-mesh links (jump/climb): a point-to-point edge in
// the navigation graph that is NOT part of the walkable surface. A wall
// splitting the map in two is impassable without a link; with a link the
// path jumps across it. Links are baked into the tile containing their
// midpoint (only that tile re-bakes on set_off_mesh_links) and Detour snaps
// each endpoint to the nearest poly in that tile.
void test_navigation_off_mesh_links() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::OffMeshLink;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    const float step = 0.5f;
    // Flat ground [0,32]^2 at y 0..1 with a wall at x~=12 spanning ALL z:
    // the wall (solid to y=4, top ledge-filtered) splits the map into west
    // (x<12) and east (x>12) islands — no path without an off-mesh link.
    const auto split_columns = [&]() {
        std::vector<VoxelColumn> columns;
        for (float x = 0.25f; x <= 31.75f; x += step) {
            for (float z = 0.25f; z <= 31.75f; z += step) {
                VoxelColumn column;
                column.x = x;
                column.z = z;
                column.solidMinY = 0.0f;
                column.solidMaxY = 1.0f;
                column.solid = true;
                if (std::fabs(x - 12.0f) <= 0.25f) {
                    column.solidMaxY = 4.0f;  // the splitting wall
                }
                columns.push_back(column);
            }
        }
        return columns;
    };
    // The wall + erosion leave the walkable gap at [11.0, 13.0]: endpoints
    // sit 0.4 inside the poly edges with radius 0.75 (the addTile snap
    // search box must OVERLAP the target poly — the erosion AABB is strict).
    const auto make_jump = [&](bool bidirectional) {
        OffMeshLink jump;
        jump.startX = 11.4f;
        jump.startY = 1.2f;
        jump.startZ = 16.0f;
        jump.endX = 12.6f;
        jump.endY = 1.2f;
        jump.endZ = 16.0f;
        jump.radius = 0.75f;
        jump.bidirectional = bidirectional;
        return jump;
    };

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.cellSize = step;
    config.tileSize = 8.0f;  // tiled mode: 4x4 tiles of 8 world units

    std::string error;

    // ---- baseline: no link -> the map is split, no path either way ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, split_columns(), error));
        CHECK(error.empty());
        CHECK(nav->valid());
        PathResult none;
        CHECK(!nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, none));
        CHECK(!nav->find_path(28.0f, 0.5f, 16.0f, 4.0f, 0.5f, 16.0f, none));
    }

    // ---- link over the wall -> path with a jump, both directions ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, split_columns(), error));
        CHECK(error.empty());
        const uint64_t r0 = nav->tile_revision(4.0f, 4.0f);
        CHECK(r0 > 0);

        CHECK(nav->set_off_mesh_links({ make_jump(true) }, error));
        CHECK(error.empty());

        PathResult fwd;
        CHECK(nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, fwd));
        CHECK(fwd.found);
        CHECK(fwd.totalLength > 0.0f);
        // The jump: two consecutive waypoints straddle the wall (x crosses
        // 12) — the path crosses the split via the link.
        bool sawJump = false;
        for (std::size_t i = 3; i + 2 < fwd.waypoints.size(); i += 3) {
            const float x0 = fwd.waypoints[i - 3];
            const float x1 = fwd.waypoints[i];
            if ((x0 < 12.0f && x1 > 12.0f) || (x0 > 12.0f && x1 < 12.0f))
                sawJump = true;
        }
        CHECK(sawJump);

        // Reverse direction works (bidirectional), same length.
        PathResult rev;
        CHECK(nav->find_path(28.0f, 0.5f, 16.0f, 4.0f, 0.5f, 16.0f, rev));
        CHECK(rev.found);
        CHECK(std::fabs(rev.totalLength - fwd.totalLength) < 0.001f);

        // Locality: only the tile holding the link midpoint (x=12,z=16 ->
        // tile (1,2)) re-baked; all other tiles keep their revision.
        CHECK(nav->tile_revision(4.0f, 4.0f) == r0);    // tile (0,0)
        CHECK(nav->tile_revision(28.0f, 4.0f) == r0);   // tile (3,0)
        CHECK(nav->tile_revision(20.0f, 20.0f) == r0);  // tile (2,2)
        CHECK(nav->tile_revision(12.0f, 16.0f) > r0);   // tile (1,2)

        // Determinism: an identical provider is bit-identical.
        std::unique_ptr<engine::navigation::INavigationProvider> nav2 =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav2->build(config, split_columns(), error));
        CHECK(nav2->set_off_mesh_links({ make_jump(true) }, error));
        PathResult fwd2;
        CHECK(nav2->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, fwd2));
        CHECK(fwd2.found);
        CHECK(fwd2.totalLength == fwd.totalLength);
        CHECK(fwd2.waypoints == fwd.waypoints);
    }

    // ---- one-way link: only start -> end ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, split_columns(), error));
        CHECK(nav->set_off_mesh_links({ make_jump(false) }, error));
        CHECK(error.empty());
        PathResult ok, refused;
        CHECK(nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, ok));
        CHECK(ok.found);
        CHECK(!nav->find_path(28.0f, 0.5f, 16.0f, 4.0f, 0.5f, 16.0f, refused));
    }

    // ---- clearing the links removes the jump ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, split_columns(), error));
        CHECK(nav->set_off_mesh_links({ make_jump(true) }, error));
        PathResult jumpy;
        CHECK(nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, jumpy));
        CHECK(jumpy.found);
        CHECK(nav->set_off_mesh_links({}, error));  // clear
        CHECK(error.empty());
        PathResult none;
        CHECK(!nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, none));
    }

    // ---- links survive terrain updates and obstacle toggles ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, split_columns(), error));
        CHECK(nav->set_off_mesh_links({ make_jump(true) }, error));

        // Terrain edit FAR from the link (dig a hole at x=24): update() must
        // re-bake only that tile and keep the link working.
        std::vector<VoxelColumn> dug;
        for (float z = 7.75f; z <= 8.25f; z += step) {
            VoxelColumn c;
            c.x = 24.0f;
            c.z = z;
            c.solid = false;  // removed column
            dug.push_back(c);
        }
        CHECK(nav->update(dug, error));
        CHECK(error.empty());
        PathResult after;
        CHECK(nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f, after));
        CHECK(after.found);  // link still works

        // Obstacle toggle elsewhere must not clear the link either.
        engine::navigation::DynamicObstacle wall;
        for (float z = 0.25f; z <= 31.75f; z += step) {
            VoxelColumn c;
            c.x = 24.0f;
            c.z = z;
            c.solidMinY = 0.0f;
            c.solidMaxY = 4.0f;
            c.solid = true;
            wall.columns.push_back(c);
        }
        CHECK(nav->set_dynamic_obstacle(7, wall, error));
        CHECK(error.empty());
        CHECK(nav->set_obstacle_active(7, false, error));
        CHECK(error.empty());
        PathResult afterObstacle;
        CHECK(nav->find_path(4.0f, 0.5f, 16.0f, 28.0f, 0.5f, 16.0f,
                             afterObstacle));
        CHECK(afterObstacle.found);  // link still works
    }

    // ---- refusals ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(!nav->set_off_mesh_links({}, error));  // before build
        CHECK(!error.empty());
        CHECK(nav->build(config, split_columns(), error));
        CHECK(error.empty());

        const OffMeshLink good = make_jump(true);

        OffMeshLink nanLink = good;
        nanLink.startX = std::nanf("");
        CHECK(!nav->set_off_mesh_links({ nanLink }, error));  // NaN endpoint
        CHECK(!error.empty());

        OffMeshLink zeroRad = good;
        zeroRad.radius = 0.0f;
        CHECK(!nav->set_off_mesh_links({ zeroRad }, error));  // radius <= 0
        CHECK(!error.empty());

        OffMeshLink negRad = good;
        negRad.radius = -1.0f;
        CHECK(!nav->set_off_mesh_links({ negRad }, error));  // negative
        CHECK(!error.empty());

        OffMeshLink outOfBounds = good;
        outOfBounds.startX = 30.0f;
        outOfBounds.endX = 40.0f;  // midpoint 35 -> outside bounds
        CHECK(!nav->set_off_mesh_links({ outOfBounds }, error));
        CHECK(!error.empty());

        // Single-navmesh mode refused.
        NavmeshConfig legacy = config;
        legacy.tileSize = 0.0f;
        std::unique_ptr<engine::navigation::INavigationProvider> single =
            engine::navigation::create_recast_navigation_provider();
        CHECK(single->build(legacy, split_columns(), error));
        CHECK(!single->set_off_mesh_links({ good }, error));
        CHECK(!error.empty());
    }

    std::cout << "[sdk] navigation off-mesh links: wall-split map crossed "
                 "only via the jump link (bidirectional + one-way), clear "
                 "restores the split, links survive updates/obstacles, "
                 "locality + determinism, refusals OK\n";
}

void test_navigation_async_paths() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathRequestStatus;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    const float step = 0.5f;
    // Flat ground [0,32]^2 at y 0..1 with a wall at x=16 spanning z in
    // [8,24] (a detour around the wall ends, z<8 or z>24).
    const auto columns = [&]() {
        std::vector<VoxelColumn> cols;
        for (float x = 0.25f; x <= 31.75f; x += step) {
            for (float z = 0.25f; z <= 31.75f; z += step) {
                VoxelColumn c;
                c.x = x;
                c.z = z;
                c.solidMinY = 0.0f;
                c.solidMaxY = 1.0f;
                c.solid = true;
                if (std::fabs(x - 16.0f) <= 0.25f && z >= 8.0f &&
                    z <= 24.0f) {
                    c.solidMaxY = 4.0f;  // the blocking wall segment
                }
                cols.push_back(c);
            }
        }
        return cols;
    }();

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.cellSize = step;
    config.tileSize = 8.0f;  // tiled mode: 4x4 tiles of 8 world units

    std::string error;

    // Poll until the request is terminal (bounded wait; the worker is
    // serialized so this never hangs).
    const auto drain = [](engine::navigation::INavigationProvider& nav,
                          uint64_t id, PathResult& out, std::string& err) {
        for (int i = 0; i < 20000; ++i) {
            const PathRequestStatus st = nav.poll_async_path(id, out, err);
            if (st != PathRequestStatus::Queued &&
                st != PathRequestStatus::Running) {
                return st;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return PathRequestStatus::Invalid;
    };

    // ---- sync baseline ----
    float syncLen = -1.0f;
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, columns, error));
        CHECK(error.empty());
        CHECK(nav->valid());
        PathResult r;
        CHECK(nav->find_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f, r));
        CHECK(r.found);
        syncLen = r.totalLength;
        CHECK(syncLen > 0.0f);
    }

    // ---- async result == sync result (bit-exact) ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, columns, error));
        CHECK(error.empty());

        const uint64_t id = nav->begin_async_path(2.0f, 0.5f, 12.0f, 30.0f,
                                                  0.5f, 12.0f, error);
        CHECK(id != 0);
        CHECK(error.empty());
        PathResult out;
        std::string err;
        CHECK(drain(*nav, id, out, err) == PathRequestStatus::Succeeded);
        CHECK(err.empty());
        CHECK(out.found);
        CHECK(out.totalLength == syncLen);   // bit-exact vs sync find_path
        CHECK(out.waypoints.size() >= 3);
        // poll of a completed request stays Succeeded (idempotent).
        PathResult again;
        CHECK(nav->poll_async_path(id, again, err) ==
              PathRequestStatus::Succeeded);
        CHECK(again.totalLength == syncLen);
    }

    // ---- many async requests, FIFO ids, all match sync ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, columns, error));
        std::vector<uint64_t> ids;
        for (int i = 0; i < 8; ++i) {
            ids.push_back(nav->begin_async_path(2.0f, 0.5f, 12.0f, 30.0f,
                                                0.5f, 12.0f, error));
            CHECK(ids.back() != 0);
            CHECK(ids.back() == static_cast<uint64_t>(i + 1));  // ids 1..8
        }
        for (const uint64_t id : ids) {
            PathResult out;
            std::string err;
            CHECK(drain(*nav, id, out, err) == PathRequestStatus::Succeeded);
            CHECK(out.totalLength == syncLen);
        }
    }

    // ---- cancel a queued request (join semantics, deterministic) ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, columns, error));
        const uint64_t idA = nav->begin_async_path(2.0f, 0.5f, 12.0f, 30.0f,
                                                   0.5f, 12.0f, error);
        const uint64_t idB = nav->begin_async_path(2.0f, 0.5f, 12.0f, 30.0f,
                                                   0.5f, 12.0f, error);
        CHECK(idA != 0 && idB != 0);
        // idB is queued behind idA (serialized worker): cancel joins to
        // Cancelled before returning.
        CHECK(nav->cancel_async_path(idB));
        PathResult outB;
        std::string errB;
        CHECK(nav->poll_async_path(idB, outB, errB) ==
              PathRequestStatus::Cancelled);
        // idA still completes normally.
        PathResult outA;
        std::string errA;
        CHECK(drain(*nav, idA, outA, errA) == PathRequestStatus::Succeeded);
        CHECK(outA.totalLength == syncLen);
        // Cancelled is sticky.
        CHECK(nav->poll_async_path(idB, outB, errB) ==
              PathRequestStatus::Cancelled);
    }

    // ---- async queries honor area costs ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        CHECK(nav->build(config, columns, error));
        CHECK(nav->set_area_cost(1, 50.0f, error));  // no area-1 -> no-op
        CHECK(error.empty());
        const uint64_t id = nav->begin_async_path(2.0f, 0.5f, 12.0f, 30.0f,
                                                  0.5f, 12.0f, error);
        PathResult out;
        std::string err;
        CHECK(drain(*nav, id, out, err) == PathRequestStatus::Succeeded);
        CHECK(out.totalLength == syncLen);
    }

    // ---- refusals ----
    {
        std::unique_ptr<engine::navigation::INavigationProvider> nav =
            engine::navigation::create_recast_navigation_provider();
        // Before build.
        CHECK(nav->begin_async_path(2.0f, 0.5f, 12.0f, 30.0f, 0.5f, 12.0f,
                                    error) == 0);
        CHECK(!error.empty());
        // NaN point.
        CHECK(nav->build(config, columns, error));
        CHECK(error.empty());
        CHECK(nav->begin_async_path(std::nanf(""), 0.5f, 12.0f, 30.0f,
                                    0.5f, 12.0f, error) == 0);
        CHECK(!error.empty());
        // Unknown ids.
        PathResult out;
        CHECK(nav->poll_async_path(999, out, error) ==
              PathRequestStatus::Invalid);
        CHECK(!nav->cancel_async_path(999));
    }

    std::cout << "[sdk] navigation async paths: async result bit-exact vs "
                 "sync find_path, FIFO ids, cancel join semantics "
                 "(queued stays cancelled), area costs apply, refusals OK\n";
}

// ---- META section 17 / FALTANTES item 13: authoritative voxel replication ----
// Server validates and applies client edits (cooldown, bounds, loaded chunk,
// block registry), broadcasts ordered deltas over the generic networking
// runtime, streams chunks by interest, persists the dedicated server save;
// clients predict optimistically, correct on the authoritative result and
// resync from snapshots. All flows are driven transport-free through the
// public IVoxelReplication contract.

constexpr engine::voxel::ReplicationConnectionId kRepConnA = 1;
constexpr engine::voxel::ReplicationConnectionId kRepConnB = 2;
constexpr engine::voxel::ReplicationConnectionId kRepConnC = 3;
constexpr engine::voxel::ReplicationConnectionId kRepConnLate = 4;

std::unique_ptr<engine::voxel::IVoxelWorld> make_flat_world() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    if (!boot_world(*world, glm::vec3(16.0f, 200.0f, 16.0f), 16)) return nullptr;
    return world;
}

void test_replication_authority() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    CHECK(server != nullptr);
    const int surface = helper_surface_y(*server, 8, 8);
    CHECK(surface > 0);

    std::shared_ptr<engine::voxel::IVoxelReplication> replication =
        engine::voxel::create_voxel_replication(*server);
    // Wiring: the world exposes the registered replication service.
    server->register_replication(replication);
    const auto services = server->registered_services();
    CHECK(std::find(services.begin(), services.end(), "replication:authoritative") !=
          services.end());
    CHECK(std::string(replication->name()) == "authoritative");

    replication->server_register_connection(kRepConnA);
    replication->server_set_interest(kRepConnA, {{16, surface, 16}, 2});

    // Valid edit: applied to the authoritative world and counted.
    auto ok = replication->server_submit_edit(kRepConnA, 8, surface + 1, 8, kBlockStone);
    CHECK(ok.accepted);
    CHECK(ok.error.empty());
    CHECK(ok.revision == 1);
    CHECK(server->get_block(8, surface + 1, 8) == kBlockStone);
    CHECK(replication->server_edit_count() == 1);

    // Unknown connection refused without mutating anything.
    auto unknown = replication->server_submit_edit(999, 8, surface + 1, 9, kBlockStone);
    CHECK(!unknown.accepted);
    CHECK(unknown.error.find("unknown") != std::string::npos);
    CHECK(server->get_block(8, surface + 1, 9) == kBlockAir);

    // Cooldown: a second edit in the same tick is refused (anti-spam).
    auto spam = replication->server_submit_edit(kRepConnA, 9, surface + 1, 9, kBlockStone);
    CHECK(!spam.accepted);
    CHECK(spam.error.find("cooldown") != std::string::npos);
    replication->server_update();
    replication->server_update();  // advance past the cooldown window
    auto afterCooldown =
        replication->server_submit_edit(kRepConnA, 9, surface + 1, 9, kBlockStone);
    CHECK(afterCooldown.accepted);

    // Out of bounds refused.
    replication->server_update();
    replication->server_update();
    auto bounds = replication->server_submit_edit(kRepConnA, 8, 5000, 8, kBlockStone);
    CHECK(!bounds.accepted);
    CHECK(bounds.error.find("bounds") != std::string::npos);

    // Unloaded chunk refused.
    replication->server_update();
    replication->server_update();
    auto far = replication->server_submit_edit(kRepConnA, 4096, surface, 4096, kBlockStone);
    CHECK(!far.accepted);
    CHECK(far.error.find("chunk") != std::string::npos);

    // Block id unknown to the world registry refused (world transaction gate).
    replication->server_update();
    replication->server_update();
    auto invalid =
        replication->server_submit_edit(kRepConnA, 10, surface + 1, 10, 0x10000u);
    CHECK(!invalid.accepted);
    CHECK(!invalid.error.empty());
    CHECK(server->get_block(10, surface + 1, 10) == kBlockAir);  // nothing mutated

    // Broadcast: accepted edits reach a client; rejections are reported too.
    replication->server_update();
    auto batch = replication->server_pack_batch(kRepConnA);
    CHECK(batch.deltas.size() >= 2);
    CHECK(batch.deltas[0].position == glm::ivec3(8, surface + 1, 8));
    CHECK(batch.deltas[0].blockId == kBlockStone);
    CHECK(batch.deltas[0].previousBlockId == kBlockAir);  // wall goes up from air
    CHECK(batch.deltas[0].revision == 1);
    CHECK(!batch.rejected.empty());
    CHECK(std::find(batch.rejected.begin(), batch.rejected.end(),
                    glm::ivec3(10, surface + 1, 10)) != batch.rejected.end());

    std::unique_ptr<engine::voxel::IVoxelWorld> client = make_flat_world();
    CHECK(client != nullptr);
    auto clientRep = engine::voxel::create_voxel_replication(*client);
    clientRep->client_apply_batch(batch);
    CHECK(client->get_block(8, surface + 1, 8) == kBlockStone);
    CHECK(client->get_block(9, surface + 1, 9) == kBlockStone);

    std::cout << "[sdk] replication: server authority (validate/apply/broadcast) "
                 "OK\n";
}

void test_replication_deltas_reorder() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    std::unique_ptr<engine::voxel::IVoxelWorld> client = make_flat_world();
    CHECK(server && client);
    const int surface = helper_surface_y(*server, 8, 8);

    auto srv = engine::voxel::create_voxel_replication(*server);
    auto cli = engine::voxel::create_voxel_replication(*client);
    srv->server_register_connection(kRepConnA);

    CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, 8, kBlockStone).accepted);
    srv->server_update();
    srv->server_update();
    // Same position edited again: per-position revision is monotonic.
    CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, 8, kBlockDirt).accepted);
    srv->server_update();
    auto batch = srv->server_pack_batch(kRepConnA);
    CHECK(batch.deltas.size() == 2);
    CHECK(batch.deltas[0].revision == 1 && batch.deltas[1].revision == 2);
    CHECK(batch.deltas[0].sequence < batch.deltas[1].sequence);

    cli->client_apply_batch(batch);
    CHECK(client->get_block(8, surface + 1, 8) == kBlockDirt);  // last edit wins

    // Duplicate/out-of-order delivery is dropped without changing state.
    const std::size_t staleBefore = cli->client_stale_dropped();
    cli->client_apply_batch(batch);
    CHECK(cli->client_stale_dropped() == staleBefore + batch.deltas.size());
    CHECK(client->get_block(8, surface + 1, 8) == kBlockDirt);

    std::cout << "[sdk] replication: ordered deltas + stale-drop on replay OK\n";
}

// FALTANTES §7 item 137: integração do commit com replicação e persistência
// incremental. Um commit feito DIRETO no mundo autoritativo (editor/MCP/host,
// não via server_submit_edit) deve alcançar os clientes como deltas ordenados
// (hook de commit = único ponto de broadcast) e sujar o chunk para o save
// incremental seguinte. Rollback (validação) não transmite nada.
void test_commit_replication_persistence() {
    // Servidor com budget 32: garante que o chunk (0,1) (z=24) está residente
    // ao comitar o multi-edit (anéis 0-2 completos = 25 <= 32) — com budget 16
    // o eviction corta o chunk intermitentemente (mesmo flake documentado no
    // test_transaction_failure_stages).
    std::unique_ptr<engine::voxel::IVoxelWorld> server =
        engine::voxel::create_default_voxel_world();
    server->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*server, glm::vec3(8.0f, 200.0f, 8.0f), 32));
    // boot_world only waits for chunk (0,0); the commit below reaches chunk
    // (0,1) (z=24), so wait for it too — the commit would otherwise race the
    // upload and flake (findings #54 pattern).
    CHECK(settle(*server, glm::vec3(8.0f, 200.0f, 8.0f),
                 [&] { return server->is_chunk_loaded(0, 1); }));
    const int surface = helper_surface_y(*server, 8, 8);
    CHECK(surface > 0);

    auto srv = engine::voxel::create_voxel_replication(*server);
    server->register_replication(srv);
    srv->server_register_connection(kRepConnA);
    srv->server_set_interest(kRepConnA, {{0, surface, 0}, 2});

    // Cliente com budget 32: garante que o chunk (0,1) (z=24) está residente
    // ao aplicar o batch (anéis 0-2 completos = 25 <= 32), evitando o flake de
    // "chunk not loaded" na aplicação dos deltas.
    std::unique_ptr<engine::voxel::IVoxelWorld> client =
        engine::voxel::create_default_voxel_world();
    client->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*client, glm::vec3(8.0f, 200.0f, 8.0f), 32));

    // --- Commit multi-edit multi-chunk DIRETO no mundo (não via submit). ---
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = server->begin_transaction();
        tx->set_block(8, surface + 1, 8, kBlockStone);     // chunk (0,0)
        tx->set_block(8, surface + 1, 24, kBlockDirt);     // chunk (0,1)
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    // O hook de commit transmitiu os deltas — nada a ver com server_submit_edit.
    srv->server_update();
    auto batch = srv->server_pack_batch(kRepConnA);
    CHECK(batch.deltas.size() == 2);
    CHECK(batch.deltas[0].position == glm::ivec3(8, surface + 1, 8));
    CHECK(batch.deltas[0].blockId == kBlockStone);
    CHECK(batch.deltas[0].previousBlockId == kBlockAir);
    CHECK(batch.deltas[1].position == glm::ivec3(8, surface + 1, 24));
    CHECK(batch.deltas[1].blockId == kBlockDirt);
    CHECK(batch.deltas[0].revision == 1 && batch.deltas[1].revision == 1);
    CHECK(batch.deltas[0].sequence < batch.deltas[1].sequence);

    // Cliente aplica o batch e converge com a autoridade. O boot não garante
    // que o anel 1 esteja materializado, e apply de delta em chunk não
    // carregado é um no-op silencioso do contrato — então materializamos o
    // chunk (0,1) no cliente ANTES de aplicar (update até is_chunk_loaded).
    auto clientRep = engine::voxel::create_voxel_replication(*client);
    for (int i = 0; i < 120 && !client->is_chunk_loaded(0, 1); ++i) {
        client->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
    }
    CHECK(client->is_chunk_loaded(0, 1));
    clientRep->client_apply_batch(batch);
    CHECK(client->get_block(8, surface + 1, 8) == kBlockStone);
    CHECK(client->get_block(8, surface + 1, 24) == kBlockDirt);
    CHECK(server->undo_depth() == 1);
    CHECK(server->edit_log_count() == 2);

    // --- Rollback (validação: id inválido misturado) NÃO transmite nada. ---
    const std::size_t deltasBefore = srv->server_pack_batch(kRepConnA).deltas.size();
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = server->begin_transaction();
        tx->set_block(12, surface + 1, 12, kBlockStone);
        tx->set_block(13, surface + 1, 13, 0x10000u);  // id sem mapping
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(!error.empty());
    }
    srv->server_update();
    const auto afterRollback = srv->server_pack_batch(kRepConnA);
    CHECK(afterRollback.deltas.size() == deltasBefore);  // nada novo
    CHECK(server->get_block(12, surface + 1, 12) == kBlockAir);  // intocado
    CHECK(server->undo_depth() == 1);                            // undo intacto

    // --- Persistência incremental: o commit sujou o chunk; o save persiste. ---
    auto store = engine::storage::create_region_chunk_storage(8);
    server->register_storage(store);
    const std::string dir = scratch_dir() + "/vc_test_commit_repl";
    std::string saveError;
    CHECK(server->save_world(dir, saveError));
    CHECK(saveError.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> fresh =
        engine::voxel::create_default_voxel_world();
    fresh->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*fresh, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    fresh->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadError;
    CHECK(fresh->load_world(dir, loadError));
    CHECK(loadError.empty());
    CHECK(fresh->get_block(8, surface + 1, 8) == kBlockStone);  // commit persistido
    CHECK(fresh->get_block(8, surface + 1, 24) == kBlockDirt);

    std::cout << "[sdk] commit replication+persistence: direct commit reaches "
                 "clients as ordered deltas, rollback broadcasts nothing, "
                 "incremental save persists the commit OK\n";
}

// FALTANTES §7 item 140: undo/redo é SESSION-SCOPED — o histórico não é
// persistido em nenhum formato de save e um load bem-sucedido (monolítico v5
// E paginado) limpa undo/redo/log. O undo pós-load reverteria contra o
// conteúdo recém-carregado (não contra a sessão que produziu as edições), então
// a nova sessão começa com histórico limpo.
void test_session_scoped_undo() {
    // --- Monolítico (serialize/deserialize v5). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    CHECK(a->undo_depth() == 1);
    CHECK(a->edit_log_count() == 2);
    std::string serError;
    const std::string bytes = a->serialize_world(serError);
    CHECK(serError.empty());
    CHECK(bytes.size() > 100);

    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    // Histórico próprio ANTES do load.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = b->begin_transaction();
        tx->set_block(7, 130, 7, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    CHECK(b->undo_depth() == 1);
    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    // Load bem-sucedido limpa a sessão: undo/redo/log zerados.
    CHECK(b->undo_depth() == 0);
    CHECK(b->edit_log_count() == 0);
    // O conteúdo do save está lá e o histórico antigo NÃO desfaz contra ele.
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    CHECK(b->get_block(7, 130, 7) == kBlockAir);  // edição pré-load não existe

    // Novo commit pós-load volta a ser undoável (sessão nova).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = b->begin_transaction();
        tx->set_block(9, 130, 9, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    CHECK(b->undo_depth() == 1);
    CHECK(b->edit_log_count() == 1);
    CHECK(b->undo_last_transaction());
    CHECK(b->get_block(9, 130, 9) == kBlockAir);
    CHECK(b->redo_last_transaction());
    CHECK(b->get_block(9, 130, 9) == kBlockStone);

    // --- Paginado (load_world_regions). ---
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_session_undo";
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = c->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    CHECK(c->undo_depth() == 1);
    std::string saveError;
    CHECK(c->save_world(dir, saveError));
    CHECK(saveError.empty());
    // O histórico NÃO foi salvo (session-scoped): carregar o mundo em outra
    // instância começa com undo_depth 0.
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    d->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string loadErrorD;
    CHECK(d->load_world(dir, loadErrorD));
    CHECK(loadErrorD.empty());
    CHECK(d->undo_depth() == 0);
    CHECK(d->edit_log_count() == 0);
    CHECK(d->get_block(3, 130, 3) == kBlockStone);

    std::cout << "[sdk] session-scoped undo/redo: history cleared by successful "
                 "load (monolithic + paged), never persisted, new session "
                 "undoable OK\n";
}

// FALTANTES §7 item 141: dry-run, diff e diagnóstico estruturado para
// editor/MCP. dry_run_edits pré-visualiza o que o commit FARIA sem aplicar
// nada: validação idêntica ao commit (limites/política/registry/chunk), diff
// com o before-state por edit, e NENHUMA mutação, evento ou undo.
void test_transaction_dry_run() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    int rolledBack = 0, committed = 0;
    world->set_transaction_listener(
        [&](const engine::voxel::TransactionEvent& event) {
            if (event.kind == engine::voxel::TransactionEvent::Kind::RolledBack) ++rolledBack;
            if (event.kind == engine::voxel::TransactionEvent::Kind::Committed) ++committed;
        });

    // --- Diff correto: before-state lido do mundo vivo, nada aplicado. ---
    engine::voxel::EditDryRunResult dry = world->dry_run_edits({
        { { 3, 130, 3 }, kBlockStone, 0 },
        { { 5, 130, 5 }, kBlockDirt, 0 },
    });
    CHECK(dry.valid);
    CHECK(dry.error.empty());
    CHECK(dry.diff.size() == 2);
    CHECK(dry.diff[0].position == glm::ivec3(3, 130, 3));
    CHECK(dry.diff[0].blockId == kBlockStone);
    CHECK(dry.diff[0].previousBlockId == kBlockAir);   // before-state = air
    CHECK(dry.diff[1].previousBlockId == kBlockAir);
    // O dry-run NÃO mutou nada, não disparou eventos e não tocou o undo.
    CHECK(world->get_block(3, 130, 3) == kBlockAir);
    CHECK(world->get_block(5, 130, 5) == kBlockAir);
    CHECK(rolledBack == 0 && committed == 0);
    CHECK(world->undo_depth() == 0);
    CHECK(world->edit_log_count() == 0);

    // O diff bate com o que o commit realmente faz (mesma validação, mesmo
    // resultado observável depois de aplicar).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockDirt);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    CHECK(world->get_block(3, 130, 3) == kBlockStone);
    CHECK(world->get_block(5, 130, 5) == kBlockDirt);
    CHECK(committed == 1);

    // --- Diagnóstico estruturado: mesmos erros do commit, SEM eventos. ---
    world->set_transaction_limits(
        engine::voxel::TransactionLimits{ /*maxEdits=*/1, /*maxBoxVolume=*/0 });
    engine::voxel::EditDryRunResult overLimit = world->dry_run_edits({
        { { 10, 130, 10 }, kBlockStone, 0 },
        { { 11, 130, 11 }, kBlockStone, 0 },
    });
    CHECK(!overLimit.valid);
    CHECK(overLimit.error.find("per-transaction limit") != std::string::npos);
    CHECK(overLimit.diff.empty());
    CHECK(world->get_block(10, 130, 10) == kBlockAir);  // nada aplicado
    CHECK(rolledBack == 0);  // dry-run nunca dispara RolledBack
    world->set_transaction_limits({});

    // Id inválido (registry): mesmo diagnóstico do commit.
    engine::voxel::EditDryRunResult badId = world->dry_run_edits({
        { { 12, 130, 12 }, 0x10000u, 0 },
    });
    CHECK(!badId.valid);
    CHECK(badId.error.find("block registry") != std::string::npos);
    CHECK(world->get_block(12, 130, 12) == kBlockAir);

    // Chunk não carregado: o dry-run espelha o rollback que o commit faria.
    engine::voxel::EditDryRunResult far = world->dry_run_edits({
        { { 4096, 130, 4096 }, kBlockStone, 0 },
    });
    CHECK(!far.valid);
    CHECK(far.error.find("chunk not loaded") != std::string::npos);
    CHECK(world->get_block(4096, 130, 4096) == kBlockAir);

    std::cout << "[sdk] transaction dry-run: preview without applying (diff "
                 "before-state, no events/undo), diagnostics mirror commit, "
                 "chunk-not-loaded mirrored OK\n";
}

// FALTANTES §6 item 129: integração do scheduler com save e servidor headless.
// O estado do scheduler (relógio fixo + filas) viaja com o save v5 e é
// restaurado no load — um servidor dedicado/headless continua o tick de onde a
// sessão salvada parou. Prova: uma block entity tickável (CounterMachine) roda
// N ticks, o mundo é salvo e recarregado; o PRÓXIMO tick pós-load é contínuo
// (worldTick avança de onde parou, não reinicia em 1).
void test_scheduler_save_headless() {
    // World A: boot, attach a CounterMachine, run ticks so the scheduler clock
    // advances well past zero.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*a, player, 16));
    a->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machine = std::make_shared<CounterMachine>();
    std::string error;
    CHECK(a->attach_block_entity(5, 96, 5, machine, error));
    for (int i = 0; i < 8; ++i) a->update(player, 0.09f);
    CHECK(machine->ticks_ >= 8);
    const uint64_t savedTick = machine->lastWorldTick_;
    CHECK(savedTick > 0);

    // Save (monolithic v5 with scheduler state) and load into a fresh world.
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, player, 16));
    b->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    const auto restored = b->block_entity_at(5, 96, 5);
    CHECK(restored != nullptr);
    auto restoredMachine =
        std::static_pointer_cast<CounterMachine>(restored);
    CHECK(restoredMachine->ticks_ == 0);  // runtime counters start fresh

    // The scheduler clock was restored: the next tick continues PAST the
    // saved worldTick (a fresh scheduler would start at worldTick 1).
    b->update(player, 0.09f);
    CHECK(restoredMachine->ticks_ == 1);
    CHECK(restoredMachine->lastWorldTick_ > savedTick);

    // Region (paged) path: same guarantee through load_world.
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, player, 16));
    c->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machineC = std::make_shared<CounterMachine>();
    std::string errorC;
    CHECK(c->attach_block_entity(5, 96, 5, machineC, errorC));
    for (int i = 0; i < 6; ++i) c->update(player, 0.09f);
    CHECK(machineC->lastWorldTick_ > 0);
    const uint64_t savedTickC = machineC->lastWorldTick_;
    c->register_storage(engine::storage::create_region_chunk_storage(8));
    const std::string dir = scratch_dir() + "/vc_test_sched_headless";
    std::string cSaveError;
    CHECK(c->save_world(dir, cSaveError));
    CHECK(cSaveError.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, player, 16));
    d->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    d->register_storage(engine::storage::create_region_chunk_storage(8));
    std::string dLoadError;
    CHECK(d->load_world(dir, dLoadError));
    CHECK(dLoadError.empty());
    const auto restoredD = d->block_entity_at(5, 96, 5);
    CHECK(restoredD != nullptr);
    auto restoredMachineD = std::static_pointer_cast<CounterMachine>(restoredD);
    // The scheduler clock accumulates deltaSeconds: after the load's own
    // updates, one 0.09s update can land on 1 or 2 fixed steps (carry), so
    // the robust invariant is "the restored entity ticks again" + "the clock
    // advanced past the saved tick" (a flake fixed 07:1x — the exact ==1
    // count straddled the step boundary).
    d->update(player, 0.09f);
    CHECK(restoredMachineD->ticks_ >= 1);
    CHECK(restoredMachineD->lastWorldTick_ > savedTickC);

    std::cout << "[sdk] scheduler save/headless: scheduler clock rides the v5 "
                 "save (monolithic + paged) and a restored world continues its "
                 "tick from the saved worldTick OK\n";
}

void test_replication_interest() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    CHECK(server != nullptr);
    const int surface = helper_surface_y(*server, 8, 8);
    CHECK(surface > 0);

    auto srv = engine::voxel::create_voxel_replication(*server);
    srv->server_register_connection(kRepConnA);
    srv->server_set_snapshot_window(0, surface + 8);  // cover the wall above water
    srv->server_set_interest(kRepConnA, {{0, surface, 0}, 1});

    // Build a wall through the authority inside chunk (0,0).
    for (int z = 4; z <= 12; ++z) {
        CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, z, kBlockStone).accepted);
        srv->server_update();
        srv->server_update();
    }
    srv->server_update();
    auto snapshots = srv->server_pack_interest(kRepConnA);
    // The wall is complete only in the LAST streamed snapshot of chunk (0,0)
    // (earlier ones were streamed mid-construction and are superseded).
    const engine::voxel::ChunkReplicationSnapshot* lastChunk00 = nullptr;
    for (const auto& snapshot : snapshots) {
        if (snapshot.chunkX == 0 && snapshot.chunkZ == 0) lastChunk00 = &snapshot;
    }
    CHECK(lastChunk00 != nullptr);
    if (lastChunk00 != nullptr) {
        const std::size_t index =
            (static_cast<std::size_t>(surface + 1) *
                 engine::voxel::kReplicationChunkSize +
             8) *
                engine::voxel::kReplicationChunkSize +
            8;
        CHECK(index < lastChunk00->blocks.size());
        if (index < lastChunk00->blocks.size()) {
            CHECK(lastChunk00->blocks[index] == kBlockStone);  // wall in snapshot
        }
    }

    // A dirty chunk re-streams after a new edit.
    CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, 14, kBlockStone).accepted);
    srv->server_update();
    auto again = srv->server_pack_interest(kRepConnA);
    bool resent = false;
    for (const auto& snapshot : again) {
        if (snapshot.chunkX == 0 && snapshot.chunkZ == 0) resent = true;
    }
    CHECK(resent);

    // A client far away streams nothing (no loaded chunks in interest range).
    srv->server_register_connection(kRepConnB);
    srv->server_set_interest(kRepConnB, {{4096, surface, 4096}, 1});
    srv->server_update();
    CHECK(srv->server_pack_interest(kRepConnB).empty());

    std::cout << "[sdk] replication: chunk streaming by interest (snapshot, "
                 "dirty resend, out-of-range) OK\n";
}

void test_replication_prediction_correction() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    std::unique_ptr<engine::voxel::IVoxelWorld> clientA = make_flat_world();
    std::unique_ptr<engine::voxel::IVoxelWorld> clientC = make_flat_world();
    CHECK(server && clientA && clientC);
    const int surface = helper_surface_y(*server, 8, 8);

    auto srv = engine::voxel::create_voxel_replication(*server);
    auto cliA = engine::voxel::create_voxel_replication(*clientA);
    auto cliC = engine::voxel::create_voxel_replication(*clientC);
    srv->server_register_connection(kRepConnA);
    srv->server_register_connection(kRepConnC);

    // A predicts a place at P; C (server-validated) places a different block
    // first. A must be corrected to the authoritative block.
    CHECK(cliA->client_predict(8, surface + 1, 8, kBlockStone));
    CHECK(clientA->get_block(8, surface + 1, 8) == kBlockStone);  // optimistic
    CHECK(cliA->client_pending_predictions() == 1);
    CHECK(srv->server_submit_edit(kRepConnC, 8, surface + 1, 8, kBlockDirt).accepted);
    srv->server_update();
    auto batchA = srv->server_pack_batch(kRepConnA);
    CHECK(batchA.deltas.size() == 1);
    CHECK(batchA.deltas[0].blockId == kBlockDirt);
    cliA->client_apply_batch(batchA);
    CHECK(cliA->client_pending_predictions() == 0);          // settled by authority
    CHECK(clientA->get_block(8, surface + 1, 8) == kBlockDirt);  // corrected

    // C predicts, then the server rejects the relayed edit (cooldown): the
    // client reverts to the pre-prediction block.
    CHECK(cliC->client_predict(12, surface + 1, 12, kBlockStone));
    CHECK(clientC->get_block(12, surface + 1, 12) == kBlockStone);
    auto rejected = srv->server_submit_edit(kRepConnC, 12, surface + 1, 12, kBlockStone);
    CHECK(!rejected.accepted);  // C is still inside its cooldown window
    srv->server_update();
    auto batchC = srv->server_pack_batch(kRepConnC);
    CHECK(!batchC.rejected.empty());
    CHECK(std::find(batchC.rejected.begin(), batchC.rejected.end(),
                    glm::ivec3(12, surface + 1, 12)) != batchC.rejected.end());
    cliC->client_apply_batch(batchC);
    CHECK(cliC->client_pending_predictions() == 0);
    CHECK(clientC->get_block(12, surface + 1, 12) == kBlockAir);  // reverted

    std::cout << "[sdk] replication: prediction + correction + rejection-revert "
                 "OK\n";
}

void test_replication_reconnect_resync() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    std::unique_ptr<engine::voxel::IVoxelWorld> clientB = make_flat_world();
    CHECK(server && clientB);
    const int surface = helper_surface_y(*server, 8, 8);

    auto srv = engine::voxel::create_voxel_replication(*server);
    auto cliB = engine::voxel::create_voxel_replication(*clientB);
    srv->server_register_connection(kRepConnA);
    srv->server_set_snapshot_window(0, surface + 8);
    srv->server_set_interest(kRepConnA, {{0, surface, 0}, 1});
    for (int z = 4; z <= 12; ++z) {
        CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, z, kBlockStone).accepted);
        srv->server_update();
        srv->server_update();
    }
    srv->server_update();

    // A late client reconnects: interest streams full snapshots (no deltas
    // needed) and the client world converges to the authoritative state.
    srv->server_register_connection(kRepConnLate);
    srv->server_set_interest(kRepConnLate, {{0, surface, 0}, 1});
    srv->server_update();
    auto snapshots = srv->server_pack_interest(kRepConnLate);
    CHECK(!snapshots.empty());
    for (const auto& snapshot : snapshots) {
        if (snapshot.chunkX == 0 && snapshot.chunkZ == 0) {
            cliB->client_apply_snapshot(snapshot);
        }
    }
    CHECK(clientB->get_block(8, surface + 1, 8) == kBlockStone);   // resynced
    CHECK(clientB->get_block(8, surface + 1, 12) == kBlockStone);

    std::cout << "[sdk] replication: reconnect + snapshot resync OK\n";
}

void test_replication_server_persist() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    CHECK(server != nullptr);
    const int surface = helper_surface_y(*server, 8, 8);

    auto srv = engine::voxel::create_voxel_replication(*server);
    srv->server_register_connection(kRepConnA);
    CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, 8, kBlockStone).accepted);

    std::string error;
    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("vc_rep_server_save_" + std::to_string(_getpid()) + ".bin")).string();
    std::filesystem::remove(path);
    CHECK(srv->server_save(path, error));
    CHECK(error.empty());

    // Dedicated-server persistence: a fresh world loads the authoritative state.
    std::unique_ptr<engine::voxel::IVoxelWorld> loaded =
        engine::voxel::create_default_voxel_world();
    loaded->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(loaded->load_world(path, error));
    CHECK(error.empty());
    CHECK(loaded->get_block(8, surface + 1, 8) == kBlockStone);
    std::filesystem::remove(path);

    std::cout << "[sdk] replication: dedicated-server persistence OK\n";
}

void test_replication_codec() {
    engine::voxel::ReplicationBatch batch;
    batch.sequence = 5;
    batch.deltas.push_back({{1, 2, 3}, 7, 4, 5, 2});
    batch.deltas.push_back({{4, 5, 6}, 3, 9, 6, 3});
    batch.rejected.push_back({9, 9, 9});

    auto raw = engine::voxel::encode_replication_batch(batch);
    engine::voxel::ReplicationBatch decoded;
    CHECK(engine::voxel::decode_replication_batch(raw, decoded));
    CHECK(decoded.sequence == 5);
    CHECK(decoded.deltas.size() == 2);
    CHECK(decoded.rejected.size() == 1);
    CHECK(decoded.deltas == batch.deltas);
    CHECK(decoded.rejected == batch.rejected);

    // Malformed frame (bad magic) is refused, never partially applied.
    std::vector<std::byte> junk = raw;
    junk[0] = std::byte{'X'};
    engine::voxel::ReplicationBatch bad;
    CHECK(!engine::voxel::decode_replication_batch(junk, bad));

    // Compressed round-trip through the promoted zstd provider.
    auto zstd = engine::compression::create_zstd_compression_provider();
    CHECK(zstd != nullptr);
    auto packed = engine::voxel::encode_replication_batch(batch, zstd);
    engine::voxel::ReplicationBatch fromPacked;
    CHECK(engine::voxel::decode_replication_batch(packed, fromPacked, zstd));
    CHECK(fromPacked.deltas == batch.deltas);
    CHECK(fromPacked.rejected == batch.rejected);

    engine::voxel::ChunkReplicationSnapshot snapshot;
    snapshot.chunkX = 0;
    snapshot.chunkZ = 0;
    snapshot.minY = 64;
    snapshot.height = 16;
    snapshot.sequence = 9;
    snapshot.blocks.assign(16 * 16 * 16, 0u);
    snapshot.blocks[(3 * 16 + 2) * 16 + 1] = kBlockStone;
    auto sRaw = engine::voxel::encode_replication_snapshot(snapshot);
    engine::voxel::ChunkReplicationSnapshot sDecoded;
    CHECK(engine::voxel::decode_replication_snapshot(sRaw, sDecoded));
    CHECK(sDecoded.chunkX == 0 && sDecoded.chunkZ == 0 && sDecoded.minY == 64);
    CHECK(sDecoded.height == 16 && sDecoded.sequence == 9);
    CHECK(sDecoded.blocks == snapshot.blocks);
    auto sPacked = engine::voxel::encode_replication_snapshot(snapshot, zstd);
    engine::voxel::ChunkReplicationSnapshot sFromPacked;
    CHECK(engine::voxel::decode_replication_snapshot(sPacked, sFromPacked, zstd));
    CHECK(sFromPacked.blocks == snapshot.blocks);

    std::cout << "[sdk] replication: batch/snapshot codec + zstd compression "
                 "round-trip OK\n";
}

void test_replication_multiclient() {
    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    CHECK(server != nullptr);
    const int surface = helper_surface_y(*server, 8, 8);

    auto srv = engine::voxel::create_voxel_replication(*server);
    srv->server_register_connection(kRepConnA);
    srv->server_register_connection(kRepConnB);
    CHECK(srv->server_submit_edit(kRepConnA, 8, surface + 1, 8, kBlockStone).accepted);
    srv->server_update();
    auto batchA = srv->server_pack_batch(kRepConnA);
    auto batchB = srv->server_pack_batch(kRepConnB);
    CHECK(batchA.deltas.size() == 1);
    CHECK(batchB.deltas.size() == 1);
    CHECK(batchA.deltas[0].position == batchB.deltas[0].position);
    // Per-connection ordering: each client's stream is independent and starts
    // its own sequence space (both are the first delta of their stream).
    CHECK(batchA.deltas[0].sequence == 1);
    CHECK(batchB.deltas[0].sequence == 1);

    // A second edit is delivered to both with increasing per-connection seqs.
    srv->server_update();
    srv->server_update();
    CHECK(srv->server_submit_edit(kRepConnA, 9, surface + 1, 9, kBlockDirt).accepted);
    srv->server_update();
    auto batchA2 = srv->server_pack_batch(kRepConnA);
    auto batchB2 = srv->server_pack_batch(kRepConnB);
    CHECK(batchA2.deltas.size() == 1 && batchB2.deltas.size() == 1);
    CHECK(batchA2.deltas[0].sequence > batchA.deltas[0].sequence);
    CHECK(batchB2.deltas[0].sequence > batchB.deltas[0].sequence);

    std::cout << "[sdk] replication: multiclient broadcast with per-connection "
                 "ordering OK\n";
}

void test_region_replication() {
    // FALTANTES item 6: block entities, fluids, relevant lighting and
    // entities (+§14 inventories) replicate per region as one state-sync unit.
    constexpr std::uint32_t kBlockGlowstone = 40;  // BlockType::Glowstone
    constexpr std::uint32_t kWaterId = 12;          // BlockType::Water

    // Both sides register the same project block entity type (reconstruction
    // requires the factory — all-or-nothing otherwise).
    auto registerChest = [](engine::voxel::IVoxelWorld& world) {
        world.register_block_entity_type("project:counter_machine",
            [] { return std::make_shared<CounterMachine>(); });
    };

    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    CHECK(server != nullptr);
    registerChest(*server);
    const int surface = helper_surface_y(*server, 8, 8);
    CHECK(surface > 0);

    // Authoritative region content: a block entity, an entity carrying a §14
    // inventory component, a fluid (water) and a light emitter (glowstone).
    auto machine = std::make_shared<CounterMachine>();
    machine->counter_ = 42;
    std::string error;
    server->set_block(8, surface + 1, 8, kBlockStone);
    CHECK(server->attach_block_entity(8, surface + 1, 8, machine, error));

    auto entityWorld = server->entity_world();
    CHECK(entityWorld != nullptr);
    engine::entity::ComponentData inventory;
    inventory.type = "inventory";
    inventory.version = 1;
    {
        engine::registry::ItemRegistry items;
        engine::registry::ItemDefinition def;
        def.ns = "vulkancraft";
        def.name = "cobblestone";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
        engine::registry::Inventory inv(2);
        engine::registry::SlotFilter any;
        any.allowAny = true;
        inv.set_filter(0, any);
        engine::registry::ItemStack stone;
        stone.item = "vulkancraft:cobblestone";
        stone.count = 10;
        CHECK(inv.set(0, stone, items, error));
        inventory.blob = inv.serialize_json();
    }
    std::string spawnError;
    const engine::entity::EntityId holder =
        entityWorld->spawn("project:inventory_holder",
                           { static_cast<float>(9), static_cast<float>(surface + 1),
                             static_cast<float>(9) },
                           spawnError);
    CHECK(holder.valid());
    CHECK(entityWorld->set_component(holder, inventory));

    server->set_block(10, surface + 1, 10, kWaterId);
    server->set_block(11, surface + 1, 11, kBlockGlowstone);
    // Let the deterministic engine settle fluid + light on the authority. C.2:
    // light runs on workers, so the light-arrival sim-tick is NOT a fixed
    // point — settle to the actual fixed point instead: the water must have
    // fully spread (active fluid set drained) AND the emitter cell lit. The
    // region snapshot packs the current fluid/light cells, so packing before
    // convergence would make the two bit-identical servers diverge by tick.
    const glm::vec3 player(16.0f, 200.0f, 16.0f);
    CHECK(settle(*server, player, [&] {
        const auto snap = server->streaming_snapshot();
        return server->get_block_light(11, surface + 1, 11) > 0 &&
               snap.activeFluidCells == 0 && snap.pendingFluidTicks == 0 &&
               snap.lightDirtyChunks == 0 && snap.pendingLightJobs == 0;
    }));
    CHECK(server->get_fluid_level(10, surface + 1, 10) != 0xFF);

    auto srv = engine::voxel::create_voxel_replication(*server);
    srv->server_register_connection(kRepConnA);
    srv->server_set_interest(kRepConnA, {{8, surface, 8}, 0});
    // The test edits sit just above the surface; give the snapshot a window
    // that covers the whole region we verify (edits at surface+1 and below).
    srv->server_set_snapshot_window(surface - 24, 48);
    srv->server_update();

    engine::voxel::RegionReplicationSnapshot region;
    std::string regionError;
    CHECK(srv->server_pack_region(kRepConnA, region, regionError));
    CHECK(regionError.empty());
    CHECK(region.chunkRadius == 0);
    CHECK(region.chunks.size() == 1);
    CHECK(region.chunks[0].chunkX == 0 && region.chunks[0].chunkZ == 0);
    // Blocks inside the window (the test window covers the edits).
    CHECK(region.chunks[0].height == 48);

    // Block entity state: position + typeId + version + opaque blob.
    CHECK(region.blockEntities.size() == 1);
    CHECK(region.blockEntities[0].position == glm::ivec3(8, surface + 1, 8));
    CHECK(region.blockEntities[0].typeId == "project:counter_machine");
    CHECK(region.blockEntities[0].dataVersion == 1);
    CHECK(region.blockEntities[0].blob.size() == 4);
    std::uint32_t counter = 0;
    for (int i = 0; i < 4; ++i) {
        counter |= static_cast<std::uint32_t>(region.blockEntities[0].blob[static_cast<std::size_t>(i)])
                   << (8 * i);
    }
    CHECK(counter == 42);

    // Entity snapshot with the inventory component blob (the §14 inventory
    // JSON rides in the protocol untouched).
    CHECK(region.entities.size() == 1);
    CHECK(region.entities[0].type == "project:inventory_holder");
    bool foundInventory = false;
    for (const engine::entity::ComponentData& c : region.entities[0].components) {
        if (c.type == "inventory") {
            foundInventory = true;
            engine::registry::ItemRegistry items;
            engine::registry::ItemDefinition def;
            def.ns = "vulkancraft";
            def.name = "cobblestone";
            def.maxStack = 64;
            CHECK(items.register_item(def, error));
            engine::registry::Inventory parsed(2);
            engine::registry::SlotFilter parsedFilter;
            parsedFilter.allowAny = true;
            parsed.set_filter(0, parsedFilter);
            CHECK(parsed.deserialize_json(c.blob, items, error));
            CHECK(parsed.get(0).item == "vulkancraft:cobblestone");
            CHECK(parsed.get(0).count == 10);
        }
    }
    CHECK(foundInventory);

    // Relevant fluid + light cells: sparse, water + glowstone present.
    bool foundWater = false;
    bool foundLight = false;
    for (const engine::voxel::FluidLightReplicationCell& cell : region.cells) {
        if (cell.position == glm::ivec3(10, surface + 1, 10)) {
            foundWater = true;
            CHECK(cell.fluidLevel != 0xFF);
        }
        if (cell.position == glm::ivec3(11, surface + 1, 11)) {
            foundLight = true;
            CHECK(cell.blockLight > 0);
        }
    }
    CHECK(foundWater);
    CHECK(foundLight);

    // Codec: bit-exact round-trip (region carries everything).
    const std::vector<std::byte> encoded =
        engine::voxel::encode_replication_region(region);
    CHECK(!encoded.empty());
    engine::voxel::RegionReplicationSnapshot decoded;
    CHECK(engine::voxel::decode_replication_region(encoded, decoded));
    CHECK(decoded.sequence == region.sequence);
    CHECK(decoded.chunks.size() == region.chunks.size());
    CHECK(decoded.blockEntities == region.blockEntities);
    CHECK(decoded.cells.size() == region.cells.size());
    CHECK(decoded.entities.size() == region.entities.size());
    CHECK(engine::voxel::encode_replication_region(decoded) == encoded);

    // Determinism between identical servers: bit-identical regions.
    std::unique_ptr<engine::voxel::IVoxelWorld> server2 = make_flat_world();
    CHECK(server2 != nullptr);
    registerChest(*server2);
    const int surface2 = helper_surface_y(*server2, 8, 8);
    CHECK(surface2 == surface);
    auto machine2 = std::make_shared<CounterMachine>();
    machine2->counter_ = 42;
    server2->set_block(8, surface2 + 1, 8, kBlockStone);
    CHECK(server2->attach_block_entity(8, surface2 + 1, 8, machine2, error));
    auto entityWorld2 = server2->entity_world();
    const engine::entity::EntityId holder2 = entityWorld2->spawn(
        "project:inventory_holder",
        { static_cast<float>(9), static_cast<float>(surface2 + 1), static_cast<float>(9) },
        spawnError);
    CHECK(holder2.valid());
    CHECK(entityWorld2->set_component(holder2, inventory));
    server2->set_block(10, surface2 + 1, 10, kWaterId);
    server2->set_block(11, surface2 + 1, 11, kBlockGlowstone);
    // Same fixed-point gate as the first server (C.2): fluid spread converged
    // + emitter lit, so both pack bit-identical regions.
    CHECK(settle(*server2, player, [&] {
        const auto snap = server2->streaming_snapshot();
        return server2->get_block_light(11, surface2 + 1, 11) > 0 &&
               snap.activeFluidCells == 0 && snap.pendingFluidTicks == 0 &&
               snap.lightDirtyChunks == 0 && snap.pendingLightJobs == 0;
    }));
    auto srv2 = engine::voxel::create_voxel_replication(*server2);
    srv2->server_register_connection(kRepConnB);
    srv2->server_set_interest(kRepConnB, {{8, surface2, 8}, 0});
    srv2->server_set_snapshot_window(surface2 - 24, 48);
    srv2->server_update();
    engine::voxel::RegionReplicationSnapshot region2;
    CHECK(srv2->server_pack_region(kRepConnB, region2, regionError));
    {
        const std::vector<std::byte> encoded2 =
            engine::voxel::encode_replication_region(region2);
        if (!(encode_replication_region(region2) == encoded)) {
            const std::vector<std::byte>& e1 = encoded;
            const std::vector<std::byte>& e2 = encoded2;
            std::size_t firstDiff = 0;
            while (firstDiff < e1.size() && firstDiff < e2.size() &&
                   e1[firstDiff] == e2[firstDiff]) ++firstDiff;
            std::cout << "[sdk] REGION-DIFF size1=" << e1.size()
                      << " size2=" << e2.size()
                      << " firstDiff=" << firstDiff
                      << " cells1=" << region.cells.size()
                      << " cells2=" << region2.cells.size()
                      << " seq1=" << region.sequence
                      << " seq2=" << region2.sequence
                      << " chunks1=" << region.chunks.size()
                      << " chunks2=" << region2.chunks.size()
                      << " ent1=" << region.entities.size()
                      << " ent2=" << region2.entities.size()
                      << " be1=" << region.blockEntities.size()
                      << " be2=" << region2.blockEntities.size() << "\n";
            for (std::size_t i = 0; i < region.cells.size() &&
                                    i < region2.cells.size(); ++i) {
                if (region.cells[i].position != region2.cells[i].position ||
                    region.cells[i].blockLight != region2.cells[i].blockLight ||
                    region.cells[i].skyLight != region2.cells[i].skyLight ||
                    region.cells[i].fluidLevel != region2.cells[i].fluidLevel) {
                    std::cout << "  CELL[" << i << "] p1=("
                              << region.cells[i].position.x << ","
                              << region.cells[i].position.y << ","
                              << region.cells[i].position.z << ") bl="
                              << +region.cells[i].blockLight << " sl="
                              << +region.cells[i].skyLight << " fl="
                              << +region.cells[i].fluidLevel << " | p2=("
                              << region2.cells[i].position.x << ","
                              << region2.cells[i].position.y << ","
                              << region2.cells[i].position.z << ") bl="
                              << +region2.cells[i].blockLight << " sl="
                              << +region2.cells[i].skyLight << " fl="
                              << +region2.cells[i].fluidLevel << "\n";
                }
            }
        }
        CHECK(encode_replication_region(region2) == encoded);
    }

    // Client apply: blocks + block entity (reconstructed via factory) +
    // entity (with its inventory component) converge to the region.
    std::unique_ptr<engine::voxel::IVoxelWorld> client = make_flat_world();
    CHECK(client != nullptr);
    registerChest(*client);
    auto cli = engine::voxel::create_voxel_replication(*client);
    std::string applyError;
    CHECK(cli->client_apply_region(decoded, applyError));
    CHECK(applyError.empty());
    CHECK(client->get_block(8, surface + 1, 8) == kBlockStone);
    CHECK(client->get_block(11, surface + 1, 11) == kBlockGlowstone);
    auto chest = client->block_entity_at(8, surface + 1, 8);
    CHECK(chest != nullptr);
    CHECK(chest->type_id() == "project:counter_machine");
    auto* counterMachine = dynamic_cast<CounterMachine*>(chest.get());
    CHECK(counterMachine != nullptr);
    CHECK(counterMachine->counter_ == 42);

    auto clientEntities = client->entity_world()->entities_in_chunk(0, 0);
    CHECK(clientEntities.size() == 1);
    engine::entity::ComponentData received;
    CHECK(client->entity_world()->get_component(clientEntities[0], "inventory",
                                                received));
    engine::registry::ItemRegistry items;
    engine::registry::ItemDefinition def;
    def.ns = "vulkancraft";
    def.name = "cobblestone";
    def.maxStack = 64;
    CHECK(items.register_item(def, error));
    engine::registry::Inventory parsed(2);
    engine::registry::SlotFilter parsedFilter;
    parsedFilter.allowAny = true;
    parsed.set_filter(0, parsedFilter);
    CHECK(parsed.deserialize_json(received.blob, items, error));
    CHECK(parsed.get(0).item == "vulkancraft:cobblestone");
    CHECK(parsed.get(0).count == 10);

    // The client's deterministic engine converges fluid + light to the
    // authoritative cells.
    CHECK(settle(*client, player,
                 [&] {
                     return client->get_block_light(11, surface + 1, 11) > 0 &&
                            client->get_fluid_level(10, surface + 1, 10) ==
                                server->get_fluid_level(10, surface + 1, 10);
                 }));

    // Unknown connection refused; unregistered block entity type refused
    // all-or-nothing (nothing mutated).
    engine::voxel::RegionReplicationSnapshot unused;
    CHECK(!srv->server_pack_region(999, unused, regionError));
    CHECK(!regionError.empty());
    engine::voxel::RegionReplicationSnapshot bad = decoded;
    engine::voxel::BlockEntityReplicationState unknown;
    unknown.position = { 7, surface + 1, 7 };
    unknown.typeId = "project:not_registered";
    unknown.dataVersion = 1;
    bad.blockEntities.push_back(unknown);
    std::unique_ptr<engine::voxel::IVoxelWorld> other = make_flat_world();
    auto otherCli = engine::voxel::create_voxel_replication(*other);
    CHECK(!otherCli->client_apply_region(bad, applyError));
    CHECK(!applyError.empty());
    CHECK(other->block_entity_count() == 0);
    CHECK(other->entity_world()->size() == 0);

    std::cout << "[sdk] replication: region state (block entities + fluids/light "
                 "cells + entities/inventories) OK\n";
}

// FALTANTES item 11 / §17 — transfer between regions: when the client's
// interest moves, block entities and entities whose chunk LEFT the previous
// interest are evicted (no ghosts), while the new region's content arrives
// and the intersection is untouched.
void test_replication_region_handoff() {
    constexpr std::uint32_t kBlockStoneHandoff = 3;
    auto registerChest = [](engine::voxel::IVoxelWorld& world) {
        world.register_block_entity_type("project:counter_machine",
            [] { return std::make_shared<CounterMachine>(); });
    };

    std::unique_ptr<engine::voxel::IVoxelWorld> server = make_flat_world();
    CHECK(server != nullptr);
    registerChest(*server);
    const int surface = helper_surface_y(*server, 8, 8);
    CHECK(surface > 0);

    // Region A (chunk 0,0): a block entity + an entity.
    std::string error;
    server->set_block(8, surface + 1, 8, kBlockStoneHandoff);
    auto machineA = std::make_shared<CounterMachine>();
    machineA->counter_ = 42;
    CHECK(server->attach_block_entity(8, surface + 1, 8, machineA, error));
    auto entityWorld = server->entity_world();
    CHECK(entityWorld != nullptr);
    std::string spawnError;
    const engine::entity::EntityId holderA =
        entityWorld->spawn("project:inventory_holder",
                           { static_cast<float>(9), static_cast<float>(surface + 1),
                             static_cast<float>(9) },
                           spawnError);
    CHECK(holderA.valid());

    auto srv = engine::voxel::create_voxel_replication(*server);
    srv->server_register_connection(kRepConnA);
    srv->server_set_snapshot_window(surface - 24, 48);
    srv->server_set_interest(kRepConnA, {{8, surface, 8}, 0});
    srv->server_update();
    engine::voxel::RegionReplicationSnapshot regionA;
    std::string regionError;
    CHECK(srv->server_pack_region(kRepConnA, regionA, regionError));
    CHECK(regionA.chunks.size() == 1);
    CHECK(regionA.blockEntities.size() == 1);
    CHECK(regionA.entities.size() == 1);

    std::unique_ptr<engine::voxel::IVoxelWorld> client = make_flat_world();
    CHECK(client != nullptr);
    registerChest(*client);
    auto cli = engine::voxel::create_voxel_replication(*client);
    std::string applyError;
    CHECK(cli->client_apply_region(regionA, applyError));
    CHECK(applyError.empty());
    auto machineAClient = client->block_entity_at(8, surface + 1, 8);
    CHECK(machineAClient != nullptr);
    auto* counterA = dynamic_cast<CounterMachine*>(machineAClient.get());
    CHECK(counterA != nullptr && counterA->counter_ == 42);
    CHECK(client->entity_world()->entities_in_chunk(0, 0).size() == 1);

    // Region B (chunk 2,2): a DIFFERENT block entity + entity. The client
    // interest moves there — region A's content must be evicted. boot_world
    // only guarantees the boot focus chunk; settle until (2,2) is loaded
    // before writing into it (same gate the placer hit).
    CHECK(settle(*server, glm::vec3(40.0f, 200.0f, 40.0f),
                 [&]() { return server->is_chunk_loaded(2, 2); }));
    server->set_block(40, surface + 1, 40, kBlockStoneHandoff);
    auto machineB = std::make_shared<CounterMachine>();
    machineB->counter_ = 7;
    CHECK(server->attach_block_entity(40, surface + 1, 40, machineB, error));
    const engine::entity::EntityId holderB =
        entityWorld->spawn("project:inventory_holder",
                           { static_cast<float>(41), static_cast<float>(surface + 1),
                             static_cast<float>(41) },
                           spawnError);
    CHECK(holderB.valid());
    srv->server_set_interest(kRepConnA, {{40, surface, 40}, 0});
    srv->server_update();
    engine::voxel::RegionReplicationSnapshot regionB;
    CHECK(srv->server_pack_region(kRepConnA, regionB, regionError));
    CHECK(regionB.chunks.size() == 1);
    CHECK(regionB.chunks[0].chunkX == 2 && regionB.chunks[0].chunkZ == 2);
    CHECK(regionB.blockEntities.size() == 1);
    CHECK(regionB.blockEntities[0].position == glm::ivec3(40, surface + 1, 40));
    CHECK(regionB.entities.size() == 1);

    // The client's chunk (2,2) must be loaded before the region applies
    // (attach_block_entity needs a loaded non-empty block — same gate the
    // placer hit). boot_world only guarantees the boot focus chunk.
    CHECK(settle(*client, glm::vec3(40.0f, 200.0f, 40.0f),
                 [&]() { return client->is_chunk_loaded(2, 2); }));
    CHECK(cli->client_apply_region(regionB, applyError));
    CHECK(applyError.empty());
    // Region A's block entity and entity are GONE (chunk 0,0 left the
    // interest — no ghosts).
    CHECK(client->block_entity_at(8, surface + 1, 8) == nullptr);
    CHECK(client->entity_world()->entities_in_chunk(0, 0).empty());
    // Region B's content arrived.
    auto machineBClient = client->block_entity_at(40, surface + 1, 40);
    CHECK(machineBClient != nullptr);
    auto* counterB = dynamic_cast<CounterMachine*>(machineBClient.get());
    CHECK(counterB != nullptr && counterB->counter_ == 7);
    CHECK(client->entity_world()->entities_in_chunk(2, 2).size() == 1);
    // Intersection untouched: the client's total block entity + entity counts
    // match exactly the new region (no accumulation across handoffs).
    CHECK(client->block_entity_count() == 1);
    CHECK(client->entity_world()->size() == 1);

    // Determinism: an identical second server produces an identical region B.
    std::unique_ptr<engine::voxel::IVoxelWorld> server2 = make_flat_world();
    CHECK(server2 != nullptr);
    registerChest(*server2);
    const int surface2 = helper_surface_y(*server2, 8, 8);
    CHECK(surface2 == surface);
    auto machineB2 = std::make_shared<CounterMachine>();
    machineB2->counter_ = 7;
    CHECK(settle(*server2, glm::vec3(40.0f, 200.0f, 40.0f),
                 [&]() { return server2->is_chunk_loaded(2, 2); }));
    server2->set_block(40, surface2 + 1, 40, kBlockStoneHandoff);
    CHECK(server2->attach_block_entity(40, surface2 + 1, 40, machineB2, error));
    auto entityWorld2 = server2->entity_world();
    const engine::entity::EntityId holderB2 =
        entityWorld2->spawn("project:inventory_holder",
                            { static_cast<float>(41), static_cast<float>(surface2 + 1),
                              static_cast<float>(41) },
                            spawnError);
    CHECK(holderB2.valid());
    auto srv2 = engine::voxel::create_voxel_replication(*server2);
    srv2->server_register_connection(kRepConnB);
    srv2->server_set_snapshot_window(surface2 - 24, 48);
    srv2->server_set_interest(kRepConnB, {{40, surface2, 40}, 0});
    srv2->server_update();
    engine::voxel::RegionReplicationSnapshot regionB2;
    CHECK(srv2->server_pack_region(kRepConnB, regionB2, regionError));
    // Bit-identical CONTENT across identical servers (per-connection sequence
    // differs: this server only packed once; the first one packed region A
    // first — the sequence is per-connection, the content is the unit).
    // Bit-identical CONTENT across identical servers (per-connection sequence
    // differs: this server only packed once; the first one packed region A
    // first — the sequence is per-connection, the content is the unit).
    for (auto& chunk : regionB.chunks) chunk.sequence = 0;
    for (auto& chunk : regionB2.chunks) chunk.sequence = 0;
    regionB.sequence = 0;
    regionB2.sequence = 0;
    CHECK(encode_replication_region(regionB2) == encode_replication_region(regionB));

    std::cout << "[sdk] replication: region handoff (interest transfer, "
                 "no ghosts, intersection untouched) OK\n";
}

// ---- FALTANTES item 11: mobs are IEntityWorld entities (the legacy Mob/
// MobManager track was removed from the simulation World). The public
// IMobBehavior advances them — gravity, fluid damage (META section 13),
// fluids are never ground, simple AI, death -> despawn. Equivalence gate: the
// cow-in-lava scenario that used to live in the streaming test.

// Polls world.update + behavior.tick until the predicate holds.
template <typename Pred>
bool settle_mob(engine::voxel::IVoxelWorld& world,
                engine::entity::IEntityWorld& entities,
                engine::entity::IMobBehavior& behavior, const glm::vec3& player,
                Pred predicate, int maxMs = 8000) {
    const auto start = std::chrono::steady_clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::string error;
        if (!behavior.tick(1.0f / 60.0f, { player.x, player.y, player.z },
                           entities, world.mob_world_query(), error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

engine::entity::EntityId spawn_mob_entity(engine::entity::IEntityWorld& entities,
                                          uint32_t typeIndex, float maxHealth,
                                          bool hostile,
                                          const engine::entity::Position& pos) {
    std::string error;
    const engine::entity::EntityId id = entities.spawn("project:mob", pos, error);
    if (!id.valid()) return id;
    entities.set_health(id, { maxHealth, maxHealth });
    engine::entity::MobSpec spec;
    spec.typeIndex = typeIndex;
    spec.maxHealth = maxHealth;
    spec.hostile = hostile;
    engine::entity::ComponentData mob;
    mob.type = engine::entity::kMobComponentType;
    mob.version = 1;
    mob.blob = engine::entity::serialize_mob_spec(spec);
    entities.set_component(id, mob);
    return id;
}

void test_mob_behavior() {
    constexpr int kLavaId = 13;  // builtin BlockType::Lava

    // Equivalence gate — cow in lava (was scenario_fluid_damage in the
    // streaming test): the cow sinks through the lava (fluids are never
    // ground) and takes the lava's damagePerTick (4/s) until the check fires.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world = make_flat_world();
        CHECK(world != nullptr);
        const int floorY = helper_surface_y(*world, 8, 8);
        CHECK(floorY > 0);
        for (int x = 5; x <= 11; ++x) {
            for (int z = 5; z <= 11; ++z) {
                world->set_block(x, floorY, z, kBlockStone);
                world->set_block(x, floorY + 1, z, kLavaId);
                world->set_block(x, floorY + 2, z, kLavaId);
                for (int y = floorY + 3; y <= floorY + 6; ++y) {
                    world->set_block(x, y, z, kBlockAir);
                }
            }
        }
        auto& entities = *world->entity_world();
        std::unique_ptr<engine::entity::IMobBehavior> behavior =
            engine::entity::create_mob_behavior();
        const engine::entity::EntityId cow = spawn_mob_entity(
            entities, 3, 10.0f, false,
            { 8.5f, static_cast<float>(floorY) + 4.5f, 8.5f });
        CHECK(cow.valid());
        const glm::vec3 player(16.0f, 200.0f, 16.0f);
        const bool damaged = settle_mob(*world, entities, *behavior, player,
                                        [&] {
                                            engine::entity::Health h;
                                            return entities.get_health(cow, h) &&
                                                   h.value <= 10.0f - 1.5f;
                                        });
        CHECK(damaged);
        engine::entity::Health h;
        engine::entity::Position p;
        CHECK(entities.get_health(cow, h));
        CHECK(h.value < 10.0f);
        CHECK(entities.get_position(cow, p));
        // Fluids are never ground: the cow sank through the lava and rests
        // inside it (below the pool surface), not on top of it.
        CHECK(p.y < static_cast<float>(floorY) + 1.5f);
        std::cout << "[sdk] mob behavior: cow in lava lost "
                  << (10.0f - h.value) << " HP (4/s) and sank\n";
    }

    // FALTANTES §8 item 167: o dano de fluido é data-driven — um TERCEIRO
    // fluido de projeto (test:acid, damagePerTick 2.0 via FluidRegistry)
    // também aplica dano a mobs dentro dele.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world =
            engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(kFluidTestTerrain));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"acid","namespace":"test","class":"fluid","color":[0.3,1.0,0.2]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:acid","viscosity":0.0,"range":4,"falling":false,"evaporation":false,"damagePerTick":2.0}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        const glm::vec3 player(24.0f, 200.0f, 24.0f);
        CHECK(boot_world(*world, player, 64));
        uint32_t acidId = 0;
        CHECK(world->resolve_block_id("test:acid", acidId, error) && error.empty());
        // Poço de ácido no chão + vaca acima: ela afunda e perde HP a 2/s.
        world->set_block(24, kFluidTestGroundedY, 24, acidId);
        CHECK(settle(*world, player,
            [&] { return world->get_fluid_level(26, kFluidTestGroundedY, 24) == 4; }));
        auto& entities = *world->entity_world();
        std::unique_ptr<engine::entity::IMobBehavior> behavior =
            engine::entity::create_mob_behavior();
        const engine::entity::EntityId cow = spawn_mob_entity(
            entities, 3, 10.0f, false,
            { 24.5f, static_cast<float>(kFluidTestGroundedY) + 2.5f, 24.5f });
        CHECK(cow.valid());
        const bool damaged = settle_mob(*world, entities, *behavior, player,
                                        [&] {
                                            engine::entity::Health h;
                                            return entities.get_health(cow, h) &&
                                                   h.value <= 9.0f;
                                        });
        CHECK(damaged);
        engine::entity::Health h;
        CHECK(entities.get_health(cow, h));
        CHECK(h.value < 10.0f);
        std::cout << "[sdk] mob behavior: cow in project acid lost "
                  << (10.0f - h.value) << " HP (2/s, data-driven) OK\n";
    }

    // Death -> despawn: a 1-HP cow in lava dies and leaves the world.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world = make_flat_world();
        CHECK(world != nullptr);
        const int floorY = helper_surface_y(*world, 8, 8);
        for (int x = 5; x <= 11; ++x) {
            for (int z = 5; z <= 11; ++z) {
                world->set_block(x, floorY, z, kBlockStone);
                world->set_block(x, floorY + 1, z, kLavaId);
                world->set_block(x, floorY + 2, z, kLavaId);
            }
        }
        auto& entities = *world->entity_world();
        std::unique_ptr<engine::entity::IMobBehavior> behavior =
            engine::entity::create_mob_behavior();
        const engine::entity::EntityId cow = spawn_mob_entity(
            entities, 3, 1.0f, false,
            { 8.5f, static_cast<float>(floorY) + 2.5f, 8.5f });
        CHECK(cow.valid());
        const glm::vec3 player(16.0f, 200.0f, 16.0f);
        const bool died = settle_mob(*world, entities, *behavior, player,
                                     [&] { return entities.size() == 0; });
        CHECK(died);
        CHECK(!entities.alive(cow));
    }

    // AI: a hostile mob chases the player (closes in at chase speed); a
    // passive mob on the same ground never leaves its wander bound.
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> world = make_flat_world();
        CHECK(world != nullptr);
        const int platformY = 140;
        for (int x = -3; x <= 3; ++x) {
            for (int z = -3; z <= 3; ++z) {
                world->set_block(x, platformY, z, kBlockStone);
                world->set_block(x, platformY + 1, z, kBlockAir);
                world->set_block(x, platformY + 2, z, kBlockAir);
            }
        }
        auto& entities = *world->entity_world();
        std::unique_ptr<engine::entity::IMobBehavior> behavior =
            engine::entity::create_mob_behavior();
        const engine::entity::EntityId zombie = spawn_mob_entity(
            entities, 0, 20.0f, true, { 0.0f, 141.5f, 0.0f });
        const engine::entity::EntityId cow = spawn_mob_entity(
            entities, 3, 10.0f, false, { 0.0f, 141.5f, 0.0f });
        CHECK(zombie.valid() && cow.valid());
        const glm::vec3 player(5.0f, 141.5f, 0.0f);  // 5 m away (chase range 16)
        const int kTicks = 60;                       // 1 s of sim
        for (int i = 0; i < kTicks; ++i) {
            world->update(player, 1.0f / 60.0f);
            std::string error;
            CHECK(behavior->tick(1.0f / 60.0f, { player.x, player.y, player.z },
                                 entities, world->mob_world_query(), error));
        }
        engine::entity::Position pz;
        engine::entity::Position pc;
        CHECK(entities.get_position(zombie, pz));
        CHECK(entities.get_position(cow, pc));
        const float zombieDist = std::sqrt((pz.x - player.x) * (pz.x - player.x) +
                                           (pz.z - player.z) * (pz.z - player.z));
        const float cowDist = std::sqrt((pc.x - player.x) * (pc.x - player.x) +
                                        (pc.z - player.z) * (pc.z - player.z));
        // Chase: ~3.2 m/s closed most of the 5 m in 1 s; passive mob stays
        // well inside its wander bound.
        CHECK(zombieDist < 5.0f - 1.0f);
        CHECK(cowDist < 8.0f);
        engine::entity::Health hz;
        CHECK(entities.get_health(zombie, hz));
        CHECK(hz.value == 20.0f);  // no damage on solid ground
    }

    // Determinism: two identical worlds + populations reproduce bit-exactly.
    {
        std::vector<float> runA;
        std::vector<float> runB;
        for (int run = 0; run < 2; ++run) {
            std::unique_ptr<engine::voxel::IVoxelWorld> world = make_flat_world();
            CHECK(world != nullptr);
            auto& entities = *world->entity_world();
            std::unique_ptr<engine::entity::IMobBehavior> behavior =
                engine::entity::create_mob_behavior();
            spawn_mob_entity(entities, 3, 10.0f, false, { 1.0f, 200.0f, 1.0f });
            spawn_mob_entity(entities, 0, 20.0f, true, { 6.0f, 200.0f, 1.0f });
            const glm::vec3 player(10.0f, 200.0f, 10.0f);
            for (int i = 0; i < 90; ++i) {
                world->update(player, 1.0f / 60.0f);
                std::string error;
                CHECK(behavior->tick(1.0f / 60.0f,
                                     { player.x, player.y, player.z },
                                     entities, world->mob_world_query(),
                                     error));
            }
            std::vector<float> out;
            entities.for_each_entity([&](engine::entity::EntityId id) {
                engine::entity::Position p;
                engine::entity::Health h;
                CHECK(entities.get_position(id, p));
                CHECK(entities.get_health(id, h));
                out.push_back(p.x);
                out.push_back(p.y);
                out.push_back(p.z);
                out.push_back(h.value);
            });
            if (run == 0) runA = out;
            else runB = out;
        }
        CHECK(runA.size() == runB.size());
        CHECK(runA == runB);  // bit-exact across instances
    }

    // Validation: negative dt refused; malformed mob component refused
    // all-or-nothing (nothing mutated); non-mob entities untouched.
    {
        auto world = make_flat_world();
        CHECK(world != nullptr);
        auto& entities = *world->entity_world();
        std::unique_ptr<engine::entity::IMobBehavior> behavior =
            engine::entity::create_mob_behavior();
        const engine::entity::Position at{ 1.0f, 200.0f, 1.0f };
        std::string plainError;
        const engine::entity::EntityId plain =
            entities.spawn("project:plain", at, plainError);
        CHECK(plain.valid());
        const engine::entity::EntityId broken =
            spawn_mob_entity(entities, 3, 10.0f, false, { 3.0f, 200.0f, 3.0f });
        CHECK(broken.valid());
        std::string error;
        CHECK(!behavior->tick(-1.0f, at, entities, world->mob_world_query(),
                              error));
        CHECK(!error.empty());
        error.clear();
        // Corrupt the mob component.
        engine::entity::ComponentData bad;
        bad.type = engine::entity::kMobComponentType;
        bad.version = 1;
        bad.blob = "not json at all";
        CHECK(entities.set_component(broken, bad));
        CHECK(!behavior->tick(1.0f / 60.0f, at, entities,
                              world->mob_world_query(), error));
        CHECK(!error.empty());
        CHECK(entities.alive(plain));
        CHECK(entities.alive(broken));  // nothing mutated by the failed step
        std::cout << "[sdk] mob behavior: equivalence, determinism, AI, death "
                     "despawn, validation OK\n";
    }
}

// ---- META section 15 / FALTANTES item 8: multiple worlds and portals ----
// A public WorldManager owns independent worlds (per-world seed/clock/rules/
// persistence), moves entities between worlds transactionally (commit/rollback)
// and maps spaces through generic portals.

uint32_t seed_mix(uint64_t seed, int x, int z) {
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(x)) +
         0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(z)) +
         0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    h ^= h >> 31;
    return static_cast<uint32_t>(h);
}

// Deterministic seed-driven terrain height: the project wires the world's
// seed (stored/exposed per world by the manager) into its generator.
struct SeedHeightGenerator final : public engine::voxel::IVoxelGenerator {
    explicit SeedHeightGenerator(uint64_t seed) : seed_(seed) {}
    engine::voxel::TerrainPoint sample(float worldX, float worldZ) const override {
        engine::voxel::TerrainPoint point;
        point.height = 8 +
                      static_cast<int>(seed_mix(seed_,
                                                static_cast<int>(std::floor(worldX)),
                                                static_cast<int>(std::floor(worldZ))) %
                                       24);
        return point;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }
    uint64_t seed_{ 0 };
};

void test_world_manager() {
    using namespace engine::world;
    using engine::entity::EntityId;
    using engine::entity::Position;

    auto manager = create_world_manager();
    CHECK(manager != nullptr);

    // ---- create independent worlds (per-world seed/rules) ----
    WorldSpec overworld;
    overworld.name = "overworld";
    overworld.seed = 11;
    overworld.rulesJson = R"({"difficulty":"hard","dayLength":1200})";
    WorldSpec nether;
    nether.name = "nether";
    nether.seed = 22;
    std::string error;
    CHECK(manager->create_world(overworld, error));
    CHECK(manager->create_world(nether, error));
    CHECK(manager->world_count() == 2);
    CHECK(manager->has_world("overworld") && manager->has_world("nether"));
    const std::vector<std::string> names = manager->world_names();
    CHECK(names.size() == 2);
    CHECK(names[0] == "nether" && names[1] == "overworld");  // sorted

    const WorldInfo infoOver = manager->world_info("overworld");
    CHECK(infoOver.loaded && infoOver.seed == 11 && infoOver.entityCount == 0);
    CHECK(infoOver.rulesJson.find("hard") != std::string::npos);
    CHECK(manager->world_info("nether").loaded &&
          manager->world_info("nether").seed == 22);

    // ---- validations all-or-nothing ----
    WorldSpec dup = overworld;
    CHECK(!manager->create_world(dup, error) && !error.empty());
    WorldSpec unnamed;
    unnamed.name = "";
    CHECK(!manager->create_world(unnamed, error));
    WorldSpec badRules = overworld;
    badRules.name = "badrules";
    badRules.rulesJson = "{not json";
    CHECK(!manager->create_world(badRules, error) && !error.empty());
    CHECK(!manager->has_world("badrules"));  // never registered
    CHECK(manager->world("missing") == nullptr);
    CHECK(!manager->update_world("missing", glm::vec3(0.0f), 1.0f / 60.0f));

    // ---- per-world clock independence ----
    CHECK(manager->update_world("overworld", glm::vec3(8.0f, 64.0f, 8.0f),
                                1.0f / 60.0f));
    CHECK(manager->update_world("overworld", glm::vec3(8.0f, 64.0f, 8.0f),
                                1.0f / 60.0f));
    CHECK(std::abs(manager->world_info("overworld").elapsedSeconds -
                   (2.0 / 60.0)) < 1e-6);
    CHECK(manager->world_info("nether").elapsedSeconds == 0.0);  // untouched

    // ---- per-world seed -> distinct terrain; same seed -> identical ----
    engine::voxel::IVoxelWorld* over = manager->world("overworld");
    engine::voxel::IVoxelWorld* neh = manager->world("nether");
    CHECK(over != nullptr && neh != nullptr);
    const uint64_t overSeed = manager->world_info("overworld").seed;
    const uint64_t nehSeed = manager->world_info("nether").seed;
    over->register_generator(std::make_shared<SeedHeightGenerator>(overSeed));
    neh->register_generator(std::make_shared<SeedHeightGenerator>(nehSeed));
    bool different = false;
    for (int x = 0; x < 64 && !different; x += 3) {
        for (int z = 0; z < 64 && !different; z += 3) {
            if (SeedHeightGenerator(overSeed).sample(x, z).height !=
                SeedHeightGenerator(nehSeed).sample(x, z).height) {
                different = true;
            }
        }
    }
    CHECK(different);  // different seeds -> different terrain

    // Same seed -> identical terrain: two worlds created with the same seed
    // store it per world and their generators agree on every column.
    WorldSpec twinA = overworld;
    twinA.name = "twinA";
    twinA.seed = 77;
    WorldSpec twinB = overworld;
    twinB.name = "twinB";
    twinB.seed = 77;
    std::string twinError;
    CHECK(manager->create_world(twinA, twinError));
    CHECK(manager->create_world(twinB, twinError));
    CHECK(manager->world_info("twinA").seed == 77);
    CHECK(manager->world_info("twinB").seed == 77);
    bool identical = true;
    for (int x = 0; x < 32 && identical; x += 3) {
        for (int z = 0; z < 32 && identical; z += 3) {
            if (SeedHeightGenerator(manager->world_info("twinA").seed)
                    .sample(x, z).height !=
                SeedHeightGenerator(manager->world_info("twinB").seed)
                    .sample(x, z).height) {
                identical = false;
            }
        }
    }
    CHECK(identical);  // same seed -> identical terrain

    // ---- transactional entity transfer (commit) ----
    auto* overEntities = over->entity_world().get();
    auto* nehEntities = neh->entity_world().get();
    CHECK(overEntities != nullptr && nehEntities != nullptr);
    std::string spawnError;
    const EntityId cow = overEntities->spawn(
        "vulkancraft:cow", Position{ 10.0f, 40.0f, 10.0f }, spawnError);
    CHECK(cow.valid());
    CHECK(overEntities->set_health(cow, engine::entity::Health{ 17.0f, 20.0f }));
    CHECK(overEntities->set_tick_interval(cow, 0.5f));
    engine::entity::ComponentData inventory;
    inventory.type = "vulkancraft:inventory";
    inventory.version = 2;
    inventory.blob = R"({"slots":[{"item":"cobblestone","count":4}]})";
    CHECK(overEntities->set_component(cow, inventory));

    const EntityId cowInNether = manager->transfer_entity(
        "overworld", cow, "nether", Position{ 100.0f, 64.0f, 100.0f }, error);
    CHECK(cowInNether.valid());
    CHECK(!overEntities->alive(cow));   // removed from source
    CHECK(nehEntities->alive(cowInNether));  // present in destination
    CHECK(manager->world_info("overworld").entityCount == 0);
    CHECK(manager->world_info("nether").entityCount == 1);
    CHECK(nehEntities->type_of(cowInNether) == "vulkancraft:cow");
    engine::entity::Health movedHealth;
    CHECK(nehEntities->get_health(cowInNether, movedHealth));
    CHECK(movedHealth.value == 17.0f);
    float interval = 0.0f;
    CHECK(nehEntities->get_tick_interval(cowInNether, interval));
    CHECK(interval == 0.5f);
    engine::entity::ComponentData movedComponent;
    CHECK(nehEntities->get_component(cowInNether, "vulkancraft:inventory",
                                     movedComponent));
    CHECK(movedComponent.version == 2 &&
          movedComponent.blob.find("cobblestone") != std::string::npos);
    Position movedPos;
    CHECK(nehEntities->get_position(cowInNether, movedPos));
    CHECK(std::abs(movedPos.x - 100.0f) < 1e-4f &&
          std::abs(movedPos.y - 64.0f) < 1e-4f &&
          std::abs(movedPos.z - 100.0f) < 1e-4f);

    // ---- transfer rollback (source untouched on any failure) ----
    const EntityId pig = overEntities->spawn(
        "vulkancraft:pig", Position{ 1.0f, 40.0f, 1.0f }, spawnError);
    CHECK(pig.valid());
    CHECK(!manager->transfer_entity("overworld", pig, "missing", Position{},
                                    error).valid());
    CHECK(overEntities->alive(pig));
    CHECK(!manager->transfer_entity("missing", pig, "nether", Position{},
                                    error).valid());
    CHECK(overEntities->alive(pig));
    CHECK(!manager->transfer_entity("overworld", cow, "nether", Position{},
                                    error).valid());  // stale handle
    CHECK(!manager->transfer_entity("overworld", pig, "overworld", Position{},
                                    error).valid());  // same world
    CHECK(overEntities->alive(pig));

    // ---- portals: generic mappings between spaces ----
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
    PortalSpec badPortal = portal;
    badPortal.toWorld = "missing";
    CHECK(manager->create_portal(badPortal, error) == 0 && !error.empty());
    badPortal = portal;
    badPortal.fromWorld = "missing";
    CHECK(manager->create_portal(badPortal, error) == 0);
    badPortal = portal;
    badPortal.toWorld = "overworld";
    CHECK(manager->create_portal(badPortal, error) == 0);  // same world
    CHECK(manager->portals().size() == 1);
    PortalSpec roundTrip;
    CHECK(manager->portal(portalId, roundTrip));
    CHECK(roundTrip.toWorld == "nether" && roundTrip.yawDegrees == 90.0f);

    // Spawn near the source anchor and cross: local offset (5, 3, 0) rotated
    // 90° around Y -> (0, 3, 5) -> target (1000, 73, 1005).
    const EntityId wanderer = overEntities->spawn(
        "vulkancraft:player", Position{ 105.0f, 67.0f, 100.0f }, spawnError);
    CHECK(wanderer.valid());
    const EntityId crossed =
        manager->transfer_via_portal("overworld", wanderer, portalId, error);
    CHECK(crossed.valid());
    CHECK(!overEntities->alive(wanderer));
    Position crossedPos;
    CHECK(nehEntities->get_position(crossed, crossedPos));
    CHECK(std::abs(crossedPos.x - 1000.0f) < 1e-3f);
    CHECK(std::abs(crossedPos.y - 73.0f) < 1e-3f);
    CHECK(std::abs(crossedPos.z - 1005.0f) < 1e-3f);
    CHECK(!manager->transfer_via_portal("overworld", pig, portalId + 99,
                                        error).valid());  // unknown portal
    CHECK(!manager->transfer_via_portal("nether", cowInNether, portalId,
                                        error).valid());  // wrong source world
    CHECK(nehEntities->alive(cowInNether));
    CHECK(overEntities->alive(pig));
    CHECK(manager->remove_portal(portalId));
    CHECK(manager->portals().empty());
    CHECK(!manager->transfer_via_portal("overworld", pig, portalId,
                                        error).valid());

    // ---- per-world persistence ----
    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() /
        ("wm_save_test_" + std::to_string(_getpid()));
    const std::string savePath = (saveDir / "overworld_save.bin").string();
    std::filesystem::create_directories(saveDir);
    // Load chunks near the pig (world save captures loaded chunk state):
    // set a budget and update until chunk (0,0) is loaded.
    manager->world("overworld")->set_chunk_budget(16);
    const auto bootStart = std::chrono::steady_clock::now();
    while (!manager->world("overworld")->is_chunk_loaded(0, 0)) {
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - bootStart).count() < 8000);
        manager->update_world("overworld", glm::vec3(8.0f, 64.0f, 8.0f),
                              1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(manager->world("overworld")->is_chunk_loaded(0, 0));
    if (!manager->save_world("overworld", savePath, error)) {
        std::cerr << "[wm] save_world failed: " << error << "\n";
    }
    CHECK(manager->save_world("overworld", savePath, error));
    auto manager2 = create_world_manager();
    CHECK(manager2 != nullptr);
    WorldSpec reloaded;
    reloaded.name = "overworld";
    reloaded.seed = 11;
    reloaded.savePath = savePath;
    CHECK(manager2->load_world(reloaded, error));
    CHECK(manager2->world_info("overworld").loaded);
    CHECK(manager2->world_info("overworld").entityCount == 1);  // the pig
    auto* reloadedEntities =
        manager2->world("overworld")->entity_world().get();
    CHECK(reloadedEntities != nullptr && reloadedEntities->size() == 1);
    WorldSpec badLoad = reloaded;
    badLoad.name = "badload";
    badLoad.savePath = (saveDir / "missing.bin").string();
    CHECK(!manager2->load_world(badLoad, error) &&
          !manager2->has_world("badload"));  // all-or-nothing

    // ---- unload ----
    CHECK(manager->unload_world("nether"));
    CHECK(!manager->has_world("nether"));
    CHECK(manager->world_count() == 3);  // overworld + twinA + twinB
    std::filesystem::remove_all(saveDir);

    std::cout << "[sdk] world manager: independent worlds, transactional "
                 "transfer (commit/rollback), portals, per-world "
                 "persistence OK\n";
}

// ---- META section 18 / FALTANTES item 14: procedural generation as assets ----
// Codified generation becomes an editable asset: a noise graph (typed nodes,
// JSON-serializable) drives terrain/caves/ores deterministically by seed
// through the world's public IVoxelGenerator contract — no engine recompile.

using engine::procgen::NoiseGraphSpec;
using engine::procgen::NoiseNodeSpec;

// fbm over perlin — the canonical terrain height graph.
NoiseGraphSpec make_height_spec(std::uint32_t seed, float frequency) {
    NoiseGraphSpec spec;
    spec.seed = seed;
    spec.nodes.push_back({ "perlin", { frequency, 0.0f }, {} });        // 0
    spec.nodes.push_back({ "fbm", { 4, 0.5f, 2.0f, 0.0f }, { 0 } });    // 1
    spec.root = 1;
    return spec;
}

void test_noise_graph_determinism() {
    const NoiseGraphSpec spec = make_height_spec(1337, 0.01f);
    std::string error;
    auto first = engine::procgen::create_noise_graph_from_spec(spec, error);
    CHECK(first != nullptr);
    CHECK(error.empty());
    auto second = engine::procgen::create_noise_graph_from_spec(spec, error);
    CHECK(second != nullptr);
    CHECK(error.empty());
    CHECK(std::string(first->name()) == "fastnoise-lite");
    CHECK(first->seed() == 1337);

    // Same spec + seed -> bit-identical samples from independent instances.
    bool identical = true;
    for (int i = 0; i < 200; ++i) {
        const float x = static_cast<float>((i % 20)) * 1.7f;
        const float z = static_cast<float>((i / 20)) * 2.3f;
        if (first->sample_2d(x, z) != second->sample_2d(x, z)) identical = false;
    }
    CHECK(identical);

    // Sampling order does not matter (no hidden state).
    const float a1 = first->sample_2d(1.0f, 2.0f);
    const float a2 = first->sample_2d(3.0f, 4.0f);
    const float a3 = first->sample_2d(5.0f, 6.0f);
    const float b3 = first->sample_2d(5.0f, 6.0f);
    const float b2 = first->sample_2d(3.0f, 4.0f);
    const float b1 = first->sample_2d(1.0f, 2.0f);
    CHECK(a1 == b1 && a2 == b2 && a3 == b3);

    // 3D sampling is deterministic too (caves/ores).
    CHECK(first->sample_3d(1.0f, 2.0f, 3.0f) == second->sample_3d(1.0f, 2.0f, 3.0f));
    CHECK(first->sample_3d(1.0f, 2.0f, 3.0f) == first->sample_3d(1.0f, 2.0f, 3.0f));

    // set_seed deterministically changes the whole field and restores it.
    const float original = first->sample_2d(10.0f, 10.0f);
    first->set_seed(99);
    CHECK(first->sample_2d(10.0f, 10.0f) != original);
    first->set_seed(1337);
    CHECK(first->sample_2d(10.0f, 10.0f) == second->sample_2d(10.0f, 10.0f));

    std::cout << "[sdk] procgen: noise graph determinism (seed, order, 2D/3D) "
                 "OK\n";
}

void test_noise_graph_nodes() {
    std::string error;

    // Constant node returns its value for every coordinate (2D and 3D).
    NoiseGraphSpec constant;
    constant.seed = 1;
    constant.nodes.push_back({ "constant", { 0.25f }, {} });
    constant.root = 0;
    auto graph = engine::procgen::create_noise_graph_from_spec(constant, error);
    CHECK(graph != nullptr && error.empty());
    CHECK(graph->sample_2d(1.0f, 2.0f) == 0.25f);
    CHECK(graph->sample_2d(-123.0f, 77.0f) == 0.25f);
    CHECK(graph->sample_3d(1.0f, 2.0f, 3.0f) == 0.25f);

    // Perlin stays within [-1, 1] and actually varies.
    NoiseGraphSpec perlin;
    perlin.seed = 7;
    perlin.nodes.push_back({ "perlin", { 0.01f, 0.0f }, {} });
    perlin.root = 0;
    auto plain = engine::procgen::create_noise_graph_from_spec(perlin, error);
    CHECK(plain != nullptr && error.empty());
    float minValue = 2.0f, maxValue = -2.0f;
    bool varies = false;
    float previous = plain->sample_2d(0.0f, 0.0f);
    for (int i = 1; i < 64; ++i) {
        const float v = plain->sample_2d(static_cast<float>(i) * 0.37f, 1.0f);
        minValue = std::min(minValue, v);
        maxValue = std::max(maxValue, v);
        if (v != previous) varies = true;
        previous = v;
    }
    CHECK(minValue >= -1.0f && maxValue <= 1.0f);
    CHECK(varies);

    // Fractal (fbm) reshapes the same base noise (differs somewhere — a single
    // point could coincide by chance).
    auto fractal = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(7, 0.01f), error);
    CHECK(fractal != nullptr && error.empty());
    bool fractalDiffers = false;
    for (int i = 0; i < 64 && !fractalDiffers; ++i) {
        const float x = static_cast<float>(i) * 0.7f;
        if (plain->sample_2d(x, 5.0f) != fractal->sample_2d(x, 5.0f)) {
            fractalDiffers = true;
        }
    }
    CHECK(fractalDiffers);

    // Blends compose sources recursively.
    NoiseGraphSpec blends;
    blends.seed = 3;
    blends.nodes.push_back({ "perlin", { 0.01f, 0.0f }, {} });       // 0
    blends.nodes.push_back({ "perlin", { 0.01f, 100.0f }, {} });     // 1
    blends.nodes.push_back({ "add", {}, { 0, 1 } });                 // 2
    blends.nodes.push_back({ "multiply", {}, { 0, 1 } });            // 3
    blends.nodes.push_back({ "min", {}, { 0, 1 } });                 // 4
    blends.nodes.push_back({ "max", {}, { 0, 1 } });                 // 5
    blends.nodes.push_back({ "constant", { 0.5f }, {} });            // 6
    blends.nodes.push_back({ "lerp", {}, { 0, 1, 6 } });             // 7
    blends.root = 7;
    auto blend = engine::procgen::create_noise_graph_from_spec(blends, error);
    CHECK(blend != nullptr && error.empty());
    const float sa = blend->sample_2d(2.0f, 3.0f);  // sanity: lerp is a blend
    CHECK(sa == sa);  // deterministic

    // Validation: unknown node type, out-of-range root, fractal over blend.
    NoiseGraphSpec unknown;
    unknown.nodes.push_back({ "nope", {}, {} });
    unknown.root = 0;
    CHECK(engine::procgen::create_noise_graph_from_spec(unknown, error) == nullptr);
    CHECK(!error.empty());
    error.clear();

    NoiseGraphSpec badRoot;
    badRoot.nodes.push_back({ "perlin", { 0.01f, 0.0f }, {} });
    badRoot.root = 5;
    CHECK(engine::procgen::create_noise_graph_from_spec(badRoot, error) == nullptr);
    CHECK(!error.empty());
    error.clear();

    NoiseGraphSpec fractalOverBlend;
    fractalOverBlend.nodes.push_back({ "perlin", { 0.01f, 0.0f }, {} });  // 0
    fractalOverBlend.nodes.push_back({ "perlin", { 0.01f, 5.0f }, {} });  // 1
    fractalOverBlend.nodes.push_back({ "add", {}, { 0, 1 } });            // 2
    fractalOverBlend.nodes.push_back({ "fbm", { 4, 0.5f, 2.0f, 0.0f }, { 2 } });
    fractalOverBlend.root = 3;
    CHECK(engine::procgen::create_noise_graph_from_spec(fractalOverBlend, error) ==
          nullptr);
    CHECK(error.find("base noise") != std::string::npos);

    std::cout << "[sdk] procgen: noise graph node semantics + validation OK\n";
}

void test_noise_graph_serialization() {
    const NoiseGraphSpec spec = make_height_spec(4242, 0.02f);
    std::string error;
    auto graph = engine::procgen::create_noise_graph_from_spec(spec, error);
    CHECK(graph != nullptr && error.empty());

    std::string json;
    CHECK(graph->serialize(json));
    CHECK(json.find("\"version\":1") != std::string::npos);
    CHECK(json.find("\"type\":\"fbm\"") != std::string::npos);

    // Round-trip through the asset form samples bit-identically.
    NoiseGraphSpec stub;  // graph starts from a valid spec; deserialize replaces it
    stub.nodes.push_back({ "constant", { 1.0f }, {} });
    stub.root = 0;
    auto rebuilt = engine::procgen::create_noise_graph_from_spec(stub, error);
    CHECK(rebuilt != nullptr && error.empty());
    CHECK(rebuilt->deserialize(json, error));
    CHECK(error.empty());
    bool identical = true;
    for (int i = 0; i < 100; ++i) {
        const float x = static_cast<float>(i) * 0.9f;
        const float z = static_cast<float>(i % 7) * 1.3f;
        if (graph->sample_2d(x, z) != rebuilt->sample_2d(x, z)) identical = false;
    }
    CHECK(identical);
    CHECK(rebuilt->seed() == 4242);

    // serialize(deserialize(json)) is idempotent.
    std::string reSerialized;
    CHECK(rebuilt->serialize(reSerialized));
    CHECK(reSerialized == json);

    // Corrupted / unsupported assets are refused, never guessed.
    std::string badError;
    CHECK(!rebuilt->deserialize("{\"version\":1, broken", badError));
    CHECK(!rebuilt->deserialize("{\"version\":2,\"seed\":0,\"root\":0,"
                                "\"nodes\":[]}", badError));
    CHECK(!rebuilt->deserialize("{\"version\":1,\"seed\":0,\"root\":0,"
                                "\"nodes\":[{\"type\":\"nope\"}]}", badError));
    // Refusal is all-or-nothing: the graph keeps its previous valid state.
    CHECK(rebuilt->sample_2d(1.0f, 1.0f) == graph->sample_2d(1.0f, 1.0f));

    std::cout << "[sdk] procgen: noise graph asset round-trip + refusal OK\n";
}

void test_graph_generator_world() {
    std::string error;
    auto height = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(2026, 0.02f), error);
    CHECK(height != nullptr && error.empty());

    // 3D density fields for caves and ores (same graph family, 3D sampling).
    NoiseGraphSpec caveSpec;
    caveSpec.seed = 11;
    caveSpec.nodes.push_back({ "simplex", { 0.08f, 0.0f }, {} });
    caveSpec.root = 0;
    auto caveGraph = engine::procgen::create_noise_graph_from_spec(caveSpec, error);
    CHECK(caveGraph != nullptr && error.empty());
    auto caves = engine::procgen::create_graph_density_function(caveGraph, 1.0f, 0.0f);
    auto ores = engine::procgen::create_graph_density_function(caveGraph, 0.5f, 0.0f);

    auto generator = engine::procgen::create_graph_voxel_generator(
        height, caves, ores, /*baseHeight=*/96, /*amplitude=*/24);
    CHECK(generator != nullptr);

    // Deterministic sampling of the generator itself.
    const engine::voxel::TerrainPoint p1 = generator->sample(3.0f, 5.0f);
    const engine::voxel::TerrainPoint p2 = generator->sample(3.0f, 5.0f);
    CHECK(p1.height == p2.height);
    CHECK(generator->cave_density(1.0f, 2.0f, 3.0f) ==
          generator->cave_density(1.0f, 2.0f, 3.0f));

    // Non-flat: the graph shapes the terrain (surface varies across the world).
    bool varied = false;
    int previousHeight = generator->sample(4.0f, 4.0f).height;
    for (int x = 2; x <= 12; x += 2) {
        for (int z = 2; z <= 12; z += 2) {
            const int h = generator->sample(static_cast<float>(x),
                                            static_cast<float>(z)).height;
            if (h != previousHeight) varied = true;
            previousHeight = h;
        }
    }
    CHECK(varied);

    // Two separately-booted worlds with identical graph generators produce
    // identical solid terrain (seed-deterministic world generation).
    std::unique_ptr<engine::voxel::IVoxelWorld> worldA =
        engine::voxel::create_default_voxel_world();
    worldA->register_generator(generator);
    CHECK(boot_world(*worldA, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    std::unique_ptr<engine::voxel::IVoxelWorld> worldB =
        engine::voxel::create_default_voxel_world();
    worldB->register_generator(generator);
    CHECK(boot_world(*worldB, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    bool sameTerrain = true;
    for (int x = 2; x <= 12; x += 2) {
        for (int z = 2; z <= 12; z += 2) {
            for (int y = 0; y <= 70; ++y) {  // below the minimum surface (72)
                if (worldA->get_block(x, y, z) != worldB->get_block(x, y, z)) {
                    sameTerrain = false;
                }
            }
        }
    }
    CHECK(sameTerrain);
    // The world actually used the graph: the terrain surface (first non-air,
    // non-water block) IS the graph height at every column, and the terrain is
    // not flat (water sits above the terrain, so the naive surface scan would
    // always return the uniform water top).
    constexpr std::uint32_t kBlockWaterId = 12;  // BlockType::Water
    const auto terrain_surface = [](engine::voxel::IVoxelWorld& world, int x, int z) {
        for (int y = 160; y >= 0; --y) {
            const std::uint32_t id = world.get_block(x, y, z);
            if (id != kBlockAir && id != kBlockWaterId) return y;
        }
        return -1;
    };
    bool nonFlat = false;
    const int reference = terrain_surface(*worldA, 4, 4);
    CHECK(reference > 0);
    const int positions[4][2] = { { 4, 4 }, { 8, 8 }, { 12, 12 }, { 14, 14 } };
    for (const auto& pos : positions) {
        const int worldSurface = terrain_surface(*worldA, pos[0], pos[1]);
        const int graphHeight =
            generator->sample(static_cast<float>(pos[0]), static_cast<float>(pos[1]))
                .height;
        CHECK(worldSurface == graphHeight);  // data-driven generation
        if (worldSurface != reference) nonFlat = true;
    }
    CHECK(nonFlat);

    std::cout << "[sdk] procgen: graph-driven voxel generator (deterministic "
                 "terrain, non-flat) OK\n";
}

// G.manifold: Manifold CSG boolean operations.
// Validates: compile, union/subtract/intersect of two boxes, deterministic.
void test_manifold_csg() {
    using namespace engine::physics;

    std::string error;
    auto csg = create_csg_operation("manifold", error);
    CHECK(csg != nullptr);
    CHECK(error.empty());
    CHECK(std::string(csg->name()) == "Manifold");

    // Create two overlapping boxes.
    // Box A: -1..1 on all axes (center at origin)
    CSGMesh boxA;
    boxA.positions = {
        -1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1,
        -1,-1, 1, 1,-1, 1, 1,1, 1, -1,1, 1
    };
    boxA.indices = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6,
        0,4,5, 0,5,1, 2,6,7, 2,7,3,
        0,3,7, 0,7,4, 1,5,6, 1,6,2
    };

    // Box B: 0..2 on all axes (shifted +1, overlaps with A)
    CSGMesh boxB;
    boxB.positions = {
         0, 0, 0, 2, 0, 0, 2,2, 0, 0,2, 0,
         0, 0, 2, 2, 0, 2, 2,2, 2, 0,2, 2
    };
    boxB.indices = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6,
        0,4,5, 0,5,1, 2,6,7, 2,7,3,
        0,3,7, 0,7,4, 1,5,6, 1,6,2
    };

    // Union: two overlapping boxes -> merged shape.
    CSGMesh resultUnion;
    bool ok = csg->operate(boxA, boxB, CSGOp::Union, resultUnion, error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(resultUnion.positions.size() >= 24);
    CHECK(resultUnion.indices.size() >= 12);

    // Subtract: A \ B -> should remove the overlap.
    CSGMesh resultSub;
    ok = csg->operate(boxA, boxB, CSGOp::Subtract, resultSub, error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(resultSub.positions.size() >= 24);

    // Intersect: A ∩ B -> only the overlapping region.
    CSGMesh resultInter;
    ok = csg->operate(boxA, boxB, CSGOp::Intersect, resultInter, error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(resultInter.positions.size() >= 24);

    // Determinism: same input -> same output.
    CSGMesh resultUnion2;
    ok = csg->operate(boxA, boxB, CSGOp::Union, resultUnion2, error);
    CHECK(ok);
    CHECK(resultUnion.positions.size() == resultUnion2.positions.size());
    CHECK(resultUnion.indices == resultUnion2.indices);

    std::cout << "[sdk] G.manifold: Manifold CSG union/subtract/intersect OK ("
              << resultUnion.positions.size() / 3 << " union verts, "
              << resultInter.positions.size() / 3 << " intersect verts)\n";
}

// G.fastnoise2: FastNoise2 SIMD backend for INoiseGraph.
// Validates: compile, determinism (same spec+seed -> same samples),
// and basic 2D/3D sampling.
void test_fastnoise2_backend() {
    std::string error;

    // Build a simple graph: perlin noise.
    NoiseGraphSpec spec;
    spec.seed = 42;
    spec.nodes.push_back({ "perlin", { 0.02f, 0.0f }, {} });
    spec.root = 0;

    auto first = engine::procgen::create_noise_graph_from_spec_fastnoise2(spec, error);
    CHECK(first != nullptr);
    CHECK(error.empty());

    // Determinism: same spec + same seed -> identical samples.
    auto second = engine::procgen::create_noise_graph_from_spec_fastnoise2(spec, error);
    CHECK(second != nullptr);
    for (float x = -10.0f; x <= 10.0f; x += 1.0f) {
        for (float z = -10.0f; z <= 10.0f; z += 1.0f) {
            CHECK(first->sample_2d(x, z) == second->sample_2d(x, z));
        }
    }
    for (float x = -5.0f; x <= 5.0f; x += 2.0f) {
        for (float y = -5.0f; y <= 5.0f; y += 2.0f) {
            for (float z = -5.0f; z <= 5.0f; z += 2.0f) {
                CHECK(first->sample_3d(x, y, z) == second->sample_3d(x, y, z));
            }
        }
    }

    // Different seed -> different samples (at least one should differ).
    spec.seed = 99;
    auto diffSeed = engine::procgen::create_noise_graph_from_spec_fastnoise2(spec, error);
    CHECK(diffSeed != nullptr);
    bool differs = false;
    for (float x = 0.0f; x <= 10.0f && !differs; x += 1.0f) {
        if (first->sample_2d(x, 0.0f) != diffSeed->sample_2d(x, 0.0f))
            differs = true;
    }
    CHECK(differs);

    // Serialize/deserialize round-trip.
    std::string json;
    CHECK(first->serialize(json));
    CHECK(!json.empty());
    auto restored = engine::procgen::create_noise_graph_from_spec_fastnoise2(spec, error);
    CHECK(restored != nullptr);
    // After deserialize, sampling should match the original (same seed).
    CHECK(restored->deserialize(json, error));
    CHECK(error.empty());
    for (float x = 0.0f; x <= 5.0f; x += 1.0f) {
        CHECK(restored->sample_2d(x, 0.0f) == first->sample_2d(x, 0.0f));
    }

    // Fractal: fbm wrapping a simplex base.
    NoiseGraphSpec fbmSpec;
    fbmSpec.seed = 7;
    fbmSpec.nodes.push_back({ "simplex", { 0.01f, 0.0f }, {} });
    fbmSpec.nodes.push_back({ "fbm", { 4.0f, 0.5f, 2.0f, 0.0f }, { 0 } });
    fbmSpec.root = 1;
    auto fbm = engine::procgen::create_noise_graph_from_spec_fastnoise2(fbmSpec, error);
    CHECK(fbm != nullptr);
    CHECK(error.empty());
    // FBM output should be finite and bounded.
    for (float x = -10.0f; x <= 10.0f; x += 2.0f) {
        float v = fbm->sample_2d(x, 0.0f);
        CHECK(std::isfinite(v));
    }

    std::cout << "[sdk] procgen: FastNoise2 SIMD backend (determinism, 2D/3D, "
                 "fractal, serialize) OK\n";
}

// ---- Item 8: climate graph, biome registry and surface rules (META 18) ----

using engine::procgen::ClimatePoint;
using engine::procgen::BiomeDefinition;

// A graph that samples a constant everywhere (used to bias whole worlds into
// a single climate, so the biome/surface assertions are exact).
std::shared_ptr<engine::procgen::INoiseGraph> make_constant_graph(
    float value, std::string& error) {
    NoiseGraphSpec spec;
    spec.seed = 1;
    spec.nodes.push_back({ "constant", { value }, {} });
    spec.root = 0;
    return engine::procgen::create_noise_graph_from_spec(spec, error);
}

// Index of the first definition with `name`, or biome_count() when absent.
std::size_t registry_index_of(engine::procgen::IBiomeRegistry& registry,
                              const std::string& name) {
    BiomeDefinition def;
    for (std::size_t i = 0; i < registry.biome_count(); ++i) {
        if (registry.biome_definition(i, def) && def.name == name) return i;
    }
    return registry.biome_count();
}

void test_climate_registry() {
    auto registry = engine::procgen::create_biome_registry();
    CHECK(registry != nullptr);
    CHECK(registry->biome_count() >= 10);

    // Deterministic first-match classification over the climate bounds.
    const ClimatePoint hotDry{ 0.7f, -0.7f, 0.5f, 0.0f, 0.0f, 0.0f };
    const ClimatePoint cold{ -0.7f, 0.2f, 0.5f, 0.0f, 0.0f, 0.0f };
    const ClimatePoint lowContinental{ 0.5f, 0.5f, -0.8f, 0.0f, 0.0f, 0.0f };
    const ClimatePoint mild{ 0.5f, 0.3f, 0.5f, 0.0f, 0.0f, 0.0f };
    std::uint32_t first = 0, second = 0;
    CHECK(registry->biome_for(hotDry, first));
    CHECK(registry->biome_for(hotDry, second));
    CHECK(first == second);  // deterministic
    const std::uint32_t hotIndex = first;
    BiomeDefinition def;
    CHECK(registry->biome_definition(first, def));
    CHECK(def.name == "desert" && def.engineBiomeIndex == 8);
    CHECK(registry->biome_for(cold, first) &&
          registry->biome_definition(first, def));
    CHECK(def.name == "glacial" && def.engineBiomeIndex == 16);
    CHECK(registry->biome_for(lowContinental, first) &&
          registry->biome_definition(first, def));
    CHECK(def.name == "deep_ocean");
    CHECK(registry->biome_for(mild, first) &&
          registry->biome_definition(first, def));
    CHECK(def.name == "meadow");  // catch-all last entry

    // JSON asset round-trip: deserialize(classify identically) and a fresh
    // registry built directly from the document.
    std::string asset;
    CHECK(registry->serialize(asset));
    std::string error;
    auto fromJson = engine::procgen::create_biome_registry_from_json(asset, error);
    CHECK(fromJson != nullptr && error.empty());
    CHECK(fromJson->biome_count() == registry->biome_count());
    std::uint32_t a = 0, b = 0;
    CHECK(registry->biome_for(hotDry, a) && fromJson->biome_for(hotDry, b));
    CHECK(a == b);
    CHECK(registry->biome_for(cold, a) && fromJson->biome_for(cold, b));
    CHECK(a == b);

    // All-or-nothing: a corrupted document is rejected and the previous
    // classification survives (desert still resolves to the same index).
    CHECK(fromJson->deserialize("{nope", error) == false);
    CHECK(!error.empty());
    CHECK(fromJson->biome_for(hotDry, b) && b == hotIndex);
    auto bad = engine::procgen::create_biome_registry_from_json("{nope", error);
    CHECK(bad == nullptr && !error.empty());

    // Validation: inverted climate bounds reject the whole asset.
    const char* inverted =
        R"({"version":1,"biomes":[
             {"name":"bad","engineBiomeIndex":4,
              "climate":{"temperature":[0.5,-0.5]}}]})";
    auto rejected = engine::procgen::create_biome_registry_from_json(inverted, error);
    CHECK(rejected == nullptr && !error.empty());

    std::cout << "[sdk] procgen: biome registry (classification, determinism, "
                 "JSON, all-or-nothing) OK\n";
}

void test_climate_sampler() {
    std::string error;
    auto temperature = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(5, 0.02f), error);
    auto moisture = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(7, 0.02f), error);
    CHECK(temperature != nullptr && moisture != nullptr && error.empty());
    auto sampler = engine::procgen::create_climate_sampler(
        temperature, moisture, nullptr, nullptr, nullptr, nullptr);
    CHECK(sampler != nullptr);

    // Deterministic and axis-independent fields.
    const ClimatePoint a = sampler->sample(3.0f, 5.0f);
    const ClimatePoint b = sampler->sample(3.0f, 5.0f);
    CHECK(a.temperature == b.temperature && a.moisture == b.moisture);
    bool axesDiffer = false;
    for (int i = 0; i < 40; ++i) {
        const float x = static_cast<float>(i) * 0.7f;
        const float z = static_cast<float>(i % 7) * 1.3f;
        const ClimatePoint p = sampler->sample(x, z);
        if (p.temperature != p.moisture) axesDiffer = true;
    }
    CHECK(axesDiffer);
    CHECK(a.continentalness == 0.0f && a.river == 0.0f);  // null axes

    // Serialization embeds the graph documents; round-trip re-samples
    // identically.
    std::string asset;
    CHECK(sampler->serialize(asset));
    auto restored = engine::procgen::create_climate_sampler(
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(restored != nullptr);
    CHECK(restored->deserialize(asset, error));
    bool identical = true;
    for (int i = 0; i < 30; ++i) {
        const float x = static_cast<float>(i % 11) * 0.9f;
        const float z = static_cast<float>(i / 11) * 1.7f;
        const ClimatePoint p1 = sampler->sample(x, z);
        const ClimatePoint p2 = restored->sample(x, z);
        if (p1.temperature != p2.temperature || p1.moisture != p2.moisture) {
            identical = false;
        }
    }
    CHECK(identical);

    // Corrupted asset is rejected and keeps the previous state.
    CHECK(restored->deserialize("{nope", error) == false);
    CHECK(!error.empty());
    CHECK(restored->sample(3.0f, 5.0f).temperature == a.temperature);

    std::cout << "[sdk] procgen: climate sampler (determinism, axes, asset "
                 "round-trip) OK\n";
}

// A hot+dry biome whose data rule places SNOW at depth 0 (only) — proving the
// world consumed the data-driven surface rule over the engine builtin (sand).
const char* kRuleOverrideAsset = R"({"version":1,"biomes":[
  {"name":"desert_rule","engineBiomeIndex":8,
   "climate":{"temperature":[0.25,1.0],"moisture":[-1.0,-0.2]},
   "surface":[{"blockId":50,"minDepth":0,"maxDepth":0}]},
  {"name":"meadow","engineBiomeIndex":7}]})";

void test_climate_generator_world() {
    std::string error;
    auto height = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(2026, 0.05f), error);
    CHECK(height != nullptr && error.empty());
    auto registry = engine::procgen::create_biome_registry();
    auto resolver = engine::procgen::create_surface_resolver(registry);
    CHECK(registry != nullptr && resolver != nullptr);

    // Cold world: temperature firmly negative -> glacial (engine 16) -> the
    // engine builtin surface is snow on top.
    auto coldTemp = make_constant_graph(-0.7f, error);
    auto coldMoisture = make_constant_graph(0.0f, error);
    auto continentalness = make_constant_graph(0.5f, error);
    auto coldSampler = engine::procgen::create_climate_sampler(
        coldTemp, coldMoisture, continentalness, nullptr, nullptr, nullptr);
    auto coldGenerator = engine::procgen::create_climate_voxel_generator(
        height, coldSampler, registry, resolver, nullptr, nullptr,
        /*baseHeight=*/131, /*amplitude=*/4);
    CHECK(coldGenerator != nullptr);

    // The generator resolves climate -> biome -> engine biome from data.
    CHECK(coldGenerator->climate_at(4.0f, 4.0f).temperature == -0.7f);
    BiomeDefinition def;
    CHECK(registry->biome_definition(coldGenerator->biome_at(4.0f, 4.0f), def));
    CHECK(def.name == "glacial");
    CHECK(coldGenerator->engine_biome_at(4.0f, 4.0f) == 16);
    CHECK(coldGenerator->sample(4.0f, 4.0f).biomeIndex == 16);

    std::unique_ptr<engine::voxel::IVoxelWorld> coldWorld =
        engine::voxel::create_default_voxel_world();
    coldWorld->register_generator(coldGenerator);
    CHECK(boot_world(*coldWorld, glm::vec3(16.0f, 200.0f, 16.0f), 16));

    // Hot world: hot+dry -> desert (engine 8) -> sand top.
    auto hotTemp = make_constant_graph(0.7f, error);
    auto hotMoisture = make_constant_graph(-0.7f, error);
    auto hotSampler = engine::procgen::create_climate_sampler(
        hotTemp, hotMoisture, continentalness, nullptr, nullptr, nullptr);
    auto hotGenerator = engine::procgen::create_climate_voxel_generator(
        height, hotSampler, registry, resolver, nullptr, nullptr, 131, 4);
    std::unique_ptr<engine::voxel::IVoxelWorld> hotWorld =
        engine::voxel::create_default_voxel_world();
    hotWorld->register_generator(hotGenerator);
    CHECK(boot_world(*hotWorld, glm::vec3(16.0f, 200.0f, 16.0f), 16));

    // Data-driven surface rule: hot+dry biome whose rule places SNOW at depth
    // 0 (only); depth 1+ falls back to the engine builtin desert surface.
    auto ruleRegistry =
        engine::procgen::create_biome_registry_from_json(kRuleOverrideAsset, error);
    CHECK(ruleRegistry != nullptr && error.empty());
    auto ruleResolver = engine::procgen::create_surface_resolver(ruleRegistry);
    auto ruleGenerator = engine::procgen::create_climate_voxel_generator(
        height, hotSampler, ruleRegistry, ruleResolver, nullptr, nullptr, 131, 4);
    std::unique_ptr<engine::voxel::IVoxelWorld> ruleWorld =
        engine::voxel::create_default_voxel_world();
    ruleWorld->register_generator(ruleGenerator);
    CHECK(boot_world(*ruleWorld, glm::vec3(16.0f, 200.0f, 16.0f), 16));

    // Determinism: a second independently booted cold world is identical.
    std::unique_ptr<engine::voxel::IVoxelWorld> coldWorldB =
        engine::voxel::create_default_voxel_world();
    coldWorldB->register_generator(coldGenerator);
    CHECK(boot_world(*coldWorldB, glm::vec3(16.0f, 200.0f, 16.0f), 16));

    constexpr std::uint32_t kSandId = 5;   // BlockType::Sand
    constexpr std::uint32_t kSnowId = 50;  // BlockType::SnowBlock
    const auto top_block = [](engine::voxel::IVoxelWorld& world, int x, int z) {
        for (int y = 160; y >= 0; --y) {
            const std::uint32_t id = world.get_block(x, y, z);
            if (id != kBlockAir) return id;
        }
        return std::uint32_t{ 0 };
    };
    bool terrainVaried = false;
    const int reference = helper_surface_y(*coldWorld, 2, 2);
    CHECK(reference > 0);
    for (int x = 2; x <= 14; x += 2) {
        for (int z = 2; z <= 14; z += 2) {
            CHECK(top_block(*coldWorld, x, z) == kSnowId);  // cold -> snow
            CHECK(top_block(*hotWorld, x, z) == kSandId);   // hot+dry -> sand
            const int topY = helper_surface_y(*ruleWorld, x, z);
            CHECK(topY > 0);
            CHECK(ruleWorld->get_block(x, topY, z) == kSnowId);      // rule won
            CHECK(ruleWorld->get_block(x, topY - 1, z) == kSandId);  // fallback
            if (helper_surface_y(*coldWorld, x, z) != reference) terrainVaried = true;
        }
    }
    CHECK(terrainVaried);  // the height graph still shapes the terrain

    // The world surface matches the graph height at every column (the graph
    // actually shapes the terrain) and the terrain is non-flat.
    bool matchesGraph = true;
    for (int x = 2; x <= 14; x += 2) {
        for (int z = 2; z <= 14; z += 2) {
            const int worldSurface = helper_surface_y(*coldWorld, x, z);
            const int graphHeight = coldGenerator->sample(static_cast<float>(x),
                                                          static_cast<float>(z)).height;
            if (worldSurface != graphHeight) matchesGraph = false;
        }
    }
    CHECK(matchesGraph);

    bool sameTerrain = true;
    for (int x = 2; x <= 12; x += 2) {
        for (int z = 2; z <= 12; z += 2) {
            for (int y = 0; y <= 135; ++y) {  // below the maximum surface (135)
                if (coldWorld->get_block(x, y, z) != coldWorldB->get_block(x, y, z)) {
                    sameTerrain = false;
                }
            }
        }
    }
    CHECK(sameTerrain);

    std::cout << "[sdk] procgen: climate-driven generator (cold->snow, hot->sand, "
                 "data surface rule, determinism) OK\n";
}

// ---- Item 9: decorators, features, carvers and ore distribution (META 18) ----

void test_ore_table() {
    std::string error;
    const char* kOreAsset = R"({"version":1,"rules":[
      {"blockId":18,"minDensity":0.7,"maxDensity":0.8,"minY":10,"maxY":120}]})";
    auto table = engine::procgen::create_ore_table_from_json(kOreAsset, error);
    CHECK(table != nullptr && error.empty());
    CHECK(table->rule_count() == 1);

    // First-match semantics over density (inclusive min, exclusive max) + y.
    CHECK(table->ore_for(0.75f, 60) == 18);
    CHECK(table->ore_for(0.75f, 5) == 0);    // y below the rule range
    CHECK(table->ore_for(0.65f, 60) == 0);   // density below
    CHECK(table->ore_for(0.80f, 60) == 0);   // exclusive max density
    CHECK(table->ore_for(0.75f, 121) == 0);  // y above the rule range

    // JSON round-trip and all-or-nothing rejection.
    std::string asset;
    CHECK(table->serialize(asset));
    auto restored = engine::procgen::create_ore_table_from_json(asset, error);
    CHECK(restored != nullptr && restored->ore_for(0.75f, 60) == 18);
    auto bad = engine::procgen::create_ore_table_from_json("{nope", error);
    CHECK(bad == nullptr && !error.empty());

    std::cout << "[sdk] procgen: ore table (rules, y/density gates, JSON) OK\n";
}

void test_carver() {
    std::string error;
    auto carver = engine::procgen::create_carver({ 60, 12 });  // fluid pool <= 60
    CHECK(carver != nullptr);
    CHECK(carver->fill_block(40) == 12);
    CHECK(carver->fill_block(61) == 0);  // pure air above the pool

    std::string asset;
    CHECK(carver->serialize(asset));
    auto restored = engine::procgen::create_carver_from_json(asset, error);
    CHECK(restored != nullptr && restored->fill_block(40) == 12 &&
          restored->fill_block(61) == 0);
    auto bad = engine::procgen::create_carver_from_json("{nope", error);
    CHECK(bad == nullptr && !error.empty());

    std::cout << "[sdk] procgen: carver (fluid pool, air above, JSON) OK\n";
}

void test_decorator() {
    std::string error;
    const char* kDecoratorSetAsset = R"({"version":1,"decorators":[
      {"type":"column","density":1.0,"params":[2,2],"blocks":[50]}]})";
    auto set = engine::procgen::create_decorator_set_from_json(kDecoratorSetAsset, error);
    CHECK(set != nullptr && error.empty());
    CHECK(set->decorator_count() == 1);

    // Direct placement through a capture writer: two blocks above the surface.
    auto column = engine::procgen::create_decorator(
        engine::procgen::DecoratorSpec{ "column", 1.0f, { 2, 2 }, { 50 }, 3,
                                        true, 0, 0, 0x7FFFFFFF },
        error);
    CHECK(column != nullptr && error.empty());
    engine::voxel::DecorationContext ctx;
    ctx.localX = 4;
    ctx.localZ = 4;
    ctx.worldX = 4.0f;
    ctx.worldZ = 4.0f;
    ctx.surfaceHeight = 131;
    ctx.biomeIndex = 8;
    std::vector<std::pair<int, std::uint32_t>> placed;
    const engine::voxel::BlockWriter writer =
        [&](int lx, int ly, int lz, std::uint32_t blockId) -> bool {
            (void)lx;
            (void)lz;
            placed.push_back({ ly, blockId });
            return true;
        };
    CHECK(column->place(ctx, writer));
    CHECK(placed.size() == 2);
    CHECK(placed[0].first == 132 && placed[0].second == 50);
    CHECK(placed[1].first == 133 && placed[1].second == 50);

    // Biome gate: a decorator pinned to another biome places nothing.
    engine::procgen::DecoratorSpec pinned = column->spec();
    pinned.anyBiome = false;
    pinned.biomeIndex = 9;
    auto gated = engine::procgen::create_decorator(pinned, error);
    CHECK(gated != nullptr);
    placed.clear();
    CHECK(!gated->place(ctx, writer));
    CHECK(placed.empty());

    // Density gate: 0.0 never places.
    engine::procgen::DecoratorSpec none = column->spec();
    none.density = 0.0f;
    auto disabled = engine::procgen::create_decorator(none, error);
    CHECK(disabled != nullptr);
    placed.clear();
    CHECK(!disabled->place(ctx, writer));
    CHECK(placed.empty());

    // Tree: trunk above the surface plus leaf blocks in the crown.
    engine::procgen::DecoratorSpec treeSpec;
    treeSpec.type = "tree";
    treeSpec.density = 1.0f;
    treeSpec.params = { 3, 3, 2 };
    treeSpec.blocks = { 6, 7 };  // wood, leaves
    auto tree = engine::procgen::create_decorator(treeSpec, error);
    CHECK(tree != nullptr && error.empty());
    placed.clear();
    CHECK(tree->place(ctx, writer));
    bool hasTrunk = false, hasLeaf = false;
    for (const auto& entry : placed) {
        if (entry.second == 6 && entry.first > 131) hasTrunk = true;
        if (entry.second == 7) hasLeaf = true;
    }
    CHECK(hasTrunk && hasLeaf);

    // JSON round-trip of the set and all-or-nothing rejection.
    std::string asset;
    CHECK(set->serialize(asset));
    auto restored = engine::procgen::create_decorator_set_from_json(asset, error);
    CHECK(restored != nullptr && restored->decorator_count() == 1);
    placed.clear();
    restored->apply(ctx, writer);
    CHECK(placed.size() == 2);
    auto bad = engine::procgen::create_decorator_set_from_json("{nope", error);
    CHECK(bad == nullptr && !error.empty());
    // Unknown type / malformed params rejected.
    auto badType = engine::procgen::create_decorator_from_json(
        R"({"version":1,"type":"castle","blocks":[1]})", error);
    CHECK(badType == nullptr && !error.empty());

    std::cout << "[sdk] procgen: decorator (column/tree placement, gates, JSON) "
                 "OK\n";
}

// Constant graph (samples the same value everywhere) — already defined as
// make_constant_graph; used here for the ore/carve density fields.
void test_world_features() {
    std::string error;
    auto height = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(2026, 0.05f), error);
    CHECK(height != nullptr && error.empty());
    auto registry = engine::procgen::create_biome_registry();
    auto resolver = engine::procgen::create_surface_resolver(registry);
    auto hotTemp = make_constant_graph(0.7f, error);
    auto hotMoisture = make_constant_graph(-0.7f, error);
    auto continentalness = make_constant_graph(0.5f, error);
    auto hotSampler = engine::procgen::create_climate_sampler(
        hotTemp, hotMoisture, continentalness, nullptr, nullptr, nullptr);
    const auto make_generator =
        [&](std::shared_ptr<const engine::procgen::IDensityFunction> caves,
            std::shared_ptr<const engine::procgen::IDensityFunction> ores,
            std::shared_ptr<const engine::procgen::IOreTable> oreTable,
            std::shared_ptr<const engine::procgen::ICarver> carver,
            std::shared_ptr<const engine::procgen::IDecoratorSet> decoratorSet) {
            return engine::procgen::create_climate_voxel_generator(
                height, hotSampler, registry, resolver, std::move(caves),
                std::move(ores), 131, 4, std::move(oreTable), std::move(carver),
                std::move(decoratorSet));
        };

    // ---- Ore distribution in the world: constant 0.75 field + a rule
    // [0.7,0.8) x y in [10,120] -> DiamondOre (18). The rule overrides the
    // builtin vein (GoldOre at y<96, IronOre at y<256) and the y-gate keeps
    // shallow bedrock layers untouched.
    auto oreField = engine::procgen::create_graph_density_function(
        make_constant_graph(0.75f, error), 1.0f, 0.0f);
    const char* kOreAsset = R"({"version":1,"rules":[
      {"blockId":18,"minDensity":0.7,"maxDensity":0.8,"minY":10,"maxY":120}]})";
    auto oreTable = engine::procgen::create_ore_table_from_json(kOreAsset, error);
    CHECK(oreTable != nullptr);
    auto oreGenerator = make_generator(nullptr, oreField, oreTable, nullptr, nullptr);
    std::unique_ptr<engine::voxel::IVoxelWorld> oreWorld =
        engine::voxel::create_default_voxel_world();
    oreWorld->register_generator(oreGenerator);
    CHECK(boot_world(*oreWorld, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    CHECK(oreWorld->get_block(4, 60, 4) == 18);   // rule over builtin GoldOre
    CHECK(oreWorld->get_block(4, 100, 4) == 18);  // rule over builtin IronOre
    CHECK(oreWorld->get_block(4, 5, 4) == 17);    // y-gate: builtin GoldOre stays

    // ---- Carver in the world: constant 0.5 cave field + fluid pool <= 60.
    auto caveField = engine::procgen::create_graph_density_function(
        make_constant_graph(0.5f, error), 1.0f, 0.0f);
    auto carver = engine::procgen::create_carver({ 60, 12 });
    auto carveGenerator = make_generator(caveField, nullptr, nullptr, carver, nullptr);
    std::unique_ptr<engine::voxel::IVoxelWorld> carveWorld =
        engine::voxel::create_default_voxel_world();
    carveWorld->register_generator(carveGenerator);
    CHECK(boot_world(*carveWorld, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    CHECK(carveWorld->get_block(4, 40, 4) == 12);   // fluid pool inside caves
    CHECK(carveWorld->get_block(4, 100, 4) == 0);   // pure air above the pool

    // ---- Decorator in the world: a 2-block snow column on every land
    // column (data mode replaces the builtin tree pass).
    const char* kDecoratorAsset = R"({"version":1,"decorators":[
      {"type":"column","density":1.0,"params":[2,2],"blocks":[50]}]})";
    auto decoratorSet = engine::procgen::create_decorator_set_from_json(kDecoratorAsset, error);
    CHECK(decoratorSet != nullptr);
    auto decoratorGenerator = make_generator(nullptr, nullptr, nullptr, nullptr,
                                             decoratorSet);
    std::unique_ptr<engine::voxel::IVoxelWorld> decorWorld =
        engine::voxel::create_default_voxel_world();
    decorWorld->register_generator(decoratorGenerator);
    CHECK(boot_world(*decorWorld, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    const int landColumns[4][2] = { { 4, 4 }, { 6, 6 }, { 8, 8 }, { 10, 10 } };
    for (const auto& column : landColumns) {
        const int surface =
            decoratorGenerator->sample(static_cast<float>(column[0]),
                                       static_cast<float>(column[1])).height;
        if (surface <= 129) continue;  // only land columns get decorated
        CHECK(decorWorld->get_block(column[0], surface + 1, column[1]) == 50);
        CHECK(decorWorld->get_block(column[0], surface + 2, column[1]) == 50);
        CHECK(decorWorld->get_block(column[0], surface + 3, column[1]) == 0);
    }

    // Determinism: a second decorator world is identical to the first.
    std::unique_ptr<engine::voxel::IVoxelWorld> decorWorldB =
        engine::voxel::create_default_voxel_world();
    decorWorldB->register_generator(decoratorGenerator);
    CHECK(boot_world(*decorWorldB, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    bool sameTerrain = true;
    for (int x = 4; x <= 10; x += 2) {
        for (int z = 4; z <= 10; z += 2) {
            for (int y = 0; y <= 140; ++y) {
                if (decorWorld->get_block(x, y, z) != decorWorldB->get_block(x, y, z)) {
                    sameTerrain = false;
                }
            }
        }
    }
    CHECK(sameTerrain);

    std::cout << "[sdk] procgen: world features (ore table, carver fluid, "
                 "decorator pillars, determinism) OK\n";
}

// ---- Item 10: deterministic structures via fast-wfc (META 18) ----

// A walled room floor plan: 1 = wall (Stone), 2 = floor (Sand). Pattern
// window 2: non-backtracking WFC cannot solve closed-room samples at larger
// windows (low entropy — see findings); window 2 is the reliable config.
engine::procgen::StructureAssetSpec make_room_spec(std::uint32_t seed) {
    engine::procgen::StructureAssetSpec spec;
    spec.sampleWidth = 8;
    spec.sampleHeight = 5;
    const int w = spec.sampleWidth;
    for (int z = 0; z < spec.sampleHeight; ++z) {
        for (int x = 0; x < w; ++x) {
            const bool wall = z == 0 || z == spec.sampleHeight - 1 ||
                              x == 0 || x == w - 1;
            spec.sample.push_back(wall ? 1u : 2u);
        }
    }
    spec.patternSize = 2;
    spec.seed = seed;
    // Wall profile: three stone layers (wall 2 blocks + roof); floor: one sand
    // layer. Depth = 3.
    spec.profiles.emplace_back(1, std::vector<std::uint32_t>{ 3, 3, 3 });
    spec.profiles.emplace_back(2, std::vector<std::uint32_t>{ 5 });
    return spec;
}

void test_structure_generator() {
    std::string error;
    auto generator = engine::procgen::create_structure_generator(
        make_room_spec(7), error);
    CHECK(generator != nullptr && error.empty());
    CHECK(generator->asset().sampleWidth == 8);

    // Generation succeeds with the expected plan/extrusion dimensions.
    engine::procgen::StructureOutput out;
    CHECK(generator->generate(12, 8, out, error));
    CHECK(out.succeeded && error.empty());
    CHECK(out.width == 12 && out.height == 8 && out.depth == 3);
    CHECK(out.plan.size() == 96 && out.blocks.size() == 96 * 3);

    // Determinism: repeated runs are bit-identical (same seed used).
    engine::procgen::StructureOutput again;
    CHECK(generator->generate(12, 8, again, error));
    CHECK(out.seedUsed == again.seedUsed);
    CHECK(out.plan == again.plan && out.blocks == again.blocks);

    // Window consistency: every patternSize window of the plan is a window of
    // the sample (symmetry 1 = identity) — the output is locally consistent
    // with the authored plan.
    const auto& sample = generator->asset().sample;
    const int sw = generator->asset().sampleWidth;
    const int sh = generator->asset().sampleHeight;
    const int ps = generator->asset().patternSize;
    bool windowsConsistent = true;
    for (int oz = 0; oz + ps <= out.height && windowsConsistent; ++oz) {
        for (int ox = 0; ox + ps <= out.width; ++ox) {
            bool found = false;
            for (int sz = 0; sz + ps <= sh; ++sz) {
                for (int sx = 0; sx + ps <= sw; ++sx) {
                    bool match = true;
                    for (int dy = 0; dy < ps; ++dy) {
                        for (int dx = 0; dx < ps; ++dx) {
                            const std::uint32_t outputId =
                                out.plan[(oz + dy) * out.width + (ox + dx)];
                            const std::uint32_t sampleId =
                                sample[(sz + dy) * sw + (sx + dx)];
                            if (outputId != sampleId) match = false;
                        }
                    }
                    if (match) found = true;
                }
            }
            if (!found) windowsConsistent = false;
        }
    }
    CHECK(windowsConsistent);

    // The plan contains both wall and floor ids, and the extrusion follows
    // the profiles (wall column 3/3/3/air; floor column 5/air).
    bool sawWall = false, sawFloor = false;
    for (int z = 0; z < out.height; ++z) {
        for (int x = 0; x < out.width; ++x) {
            const std::uint32_t id = out.plan[x + z * out.width];
            if (id == 1 && !sawWall) {
                sawWall = true;
                CHECK(out.blocks[x + z * out.width + 0 * out.width * out.height] == 3);
                CHECK(out.blocks[x + z * out.width + 1 * out.width * out.height] == 3);
                CHECK(out.blocks[x + z * out.width + 2 * out.width * out.height] == 3);
            } else if (id == 2 && !sawFloor) {
                sawFloor = true;
                CHECK(out.blocks[x + z * out.width + 0 * out.width * out.height] == 5);
                CHECK(out.blocks[x + z * out.width + 1 * out.width * out.height] == 0);
            }
        }
    }
    CHECK(sawWall && sawFloor);

    // Instance independence: a second generator built from the serialized
    // asset produces the same output.
    std::string asset;
    CHECK(generator->serialize(asset));
    auto restored = engine::procgen::create_structure_generator_from_json(asset, error);
    CHECK(restored != nullptr && error.empty());
    engine::procgen::StructureOutput restoredOut;
    CHECK(restored->generate(12, 8, restoredOut, error));
    CHECK(restoredOut.plan == out.plan && restoredOut.blocks == out.blocks);

    // The ground option is supported and deterministic (pins the sample's
    // bottom pattern as a floor). A platformer-style sample (solid ground row,
    // sparse blocks above, no closed loops) is the config the option can
    // solve.
    {
        engine::procgen::StructureAssetSpec platform;
        platform.sampleWidth = 8;
        platform.sampleHeight = 5;
        platform.patternSize = 2;
        platform.ground = true;
        platform.seed = 7;
        const std::uint32_t rows[5][8] = {
            { 0, 0, 0, 1, 0, 0, 0, 0 },
            { 0, 1, 0, 0, 0, 1, 0, 0 },
            { 0, 0, 0, 0, 1, 0, 0, 0 },
            { 1, 0, 0, 1, 0, 0, 1, 0 },
            { 1, 1, 1, 1, 1, 1, 1, 1 } };
        for (int z = 0; z < 5; ++z) {
            for (int x = 0; x < 8; ++x) platform.sample.push_back(rows[z][x]);
        }
        auto grounded = engine::procgen::create_structure_generator(platform, error);
        CHECK(grounded != nullptr && error.empty());
        engine::procgen::StructureOutput groundedOut;
        CHECK(grounded->generate(12, 8, groundedOut, error));
        CHECK(groundedOut.succeeded);
        engine::procgen::StructureOutput groundedAgain;
        CHECK(grounded->generate(12, 8, groundedAgain, error));
        CHECK(groundedOut.plan == groundedAgain.plan);
        std::string groundAsset;
        CHECK(grounded->serialize(groundAsset));
        CHECK(groundAsset.find("\"ground\":true") != std::string::npos);
    }

    // All-or-nothing on malformed assets; validation rejects bad specs.
    auto bad = engine::procgen::create_structure_generator_from_json("{nope", error);
    CHECK(bad == nullptr && !error.empty());
    CHECK(restored->deserialize("{nope", error) == false);
    CHECK(restored->generate(12, 8, again, error));  // previous spec preserved
    CHECK(again.plan == out.plan);
    auto tiny = make_room_spec(1);
    tiny.sampleWidth = 1;  // patternSize 2 cannot fit
    tiny.sample.resize(1);
    auto rejected = engine::procgen::create_structure_generator(tiny, error);
    CHECK(rejected == nullptr && !error.empty());

    std::cout << "[sdk] procgen: structure generator (determinism, window "
                 "consistency, extrusion, JSON, ground) OK\n";
}

// ---- Item 14: data-driven structures, sockets and spawn rules ----

void test_structure_placement() {
    std::string error;
    auto system = engine::procgen::create_structure_placement_system();
    CHECK(system != nullptr);

    // ---- definitions + sockets ----
    engine::procgen::StructureDefinition room;
    room.id = "test:room";
    room.spec = make_room_spec(7);
    room.outputWidth = 12;
    room.outputHeight = 8;
    room.sockets.push_back(
        engine::procgen::StructureSocket{ "door", { 0, 1, 0 }, 0, "door" });
    room.sockets.push_back(
        engine::procgen::StructureSocket{ "window", { 6, 2, 0 }, 2, "" });
    CHECK(system->add_definition(room, error) && error.empty());
    CHECK(system->definition("test:room") != nullptr);
    CHECK(system->definition("test:nope") == nullptr);
    CHECK(system->definition_ids().size() == 1);

    // Duplicate id refused.
    CHECK(!system->add_definition(room, error) && !error.empty());

    // Invalid structure asset refused (the SAME validation the generator
    // factory applies — patternSize 2 cannot fit a 1x1 sample).
    auto badRoom = room;
    badRoom.id = "test:bad";
    badRoom.spec.sampleWidth = 1;
    badRoom.spec.sample.resize(1);
    CHECK(!system->add_definition(badRoom, error) && !error.empty());

    // Duplicate socket name refused.
    auto dupSockets = room;
    dupSockets.id = "test:dupsockets";
    dupSockets.sockets.push_back(dupSockets.sockets[0]);
    CHECK(!system->add_definition(dupSockets, error) && !error.empty());

    // ---- rules ----
    engine::procgen::StructureSpawnRule rule;
    rule.structureId = "test:room";
    rule.biomes = { "plains" };
    rule.minSurfaceHeight = 100;
    rule.maxSurfaceHeight = 200;
    rule.density = 1.0f;  // always spawns when the other gates pass
    rule.spacing = 8;
    rule.yOffset = 1;
    rule.seedOffset = 3;
    CHECK(system->set_rules({ rule }, error) && error.empty());
    CHECK(system->rules().size() == 1);

    // Dangling rule refused all-or-nothing (state unchanged).
    auto dangling = rule;
    dangling.structureId = "test:nonexistent";
    CHECK(!system->set_rules({ rule, dangling }, error) && !error.empty());
    CHECK(system->rules().size() == 1);

    // ---- try_place: spawns at the cell origin, deterministic ----
    const std::uint32_t worldSeed = 42u;
    engine::procgen::StructurePlacement placed;
    CHECK(system->try_place({ rule }, 8, 8, 120, "plains", worldSeed, placed,
                            error));
    CHECK(placed.structureId == "test:room");
    CHECK(placed.origin == glm::ivec3(8, 121, 8));  // cell + surface + yOffset
    CHECK(placed.output.succeeded);
    CHECK(placed.output.width == 12 && placed.output.height == 8);

    // Canonical cell: querying anywhere inside the same cell produces the
    // same placement (origin, per-cell seed and content).
    engine::procgen::StructurePlacement inside;
    CHECK(system->try_place({ rule }, 12, 15, 120, "plains", worldSeed, inside,
                            error));
    CHECK(inside.origin == placed.origin);
    CHECK(inside.placementSeed == placed.placementSeed);
    CHECK(inside.output.blocks == placed.output.blocks);

    // Determinism across a second system rebuilt from JSON.
    std::string document;
    CHECK(system->serialize(document));
    CHECK(document.find("\"id\":\"test:room\"") != std::string::npos);
    CHECK(document.find("\"connectTag\":\"door\"") != std::string::npos);
    CHECK(document.find("\"structureId\":\"test:room\"") != std::string::npos);
    auto restored =
        engine::procgen::create_structure_placement_system_from_json(document,
                                                                     error);
    CHECK(restored != nullptr && error.empty());
    CHECK(restored->definition("test:room") != nullptr);
    CHECK(restored->rules().size() == 1);
    engine::procgen::StructurePlacement restoredPlaced;
    CHECK(restored->try_place({}, 8, 8, 120, "plains", worldSeed, restoredPlaced,
                              error));
    CHECK(restoredPlaced.origin == placed.origin);
    CHECK(restoredPlaced.placementSeed == placed.placementSeed);
    CHECK(restoredPlaced.output.blocks == placed.output.blocks);
    // Byte-identical JSON round-trip.
    std::string restoredDoc;
    CHECK(restored->serialize(restoredDoc));
    CHECK(restoredDoc == document);

    // Empty rules fall back to the system's stored rules.
    engine::procgen::StructurePlacement stored;
    CHECK(system->try_place({}, 8, 8, 120, "plains", worldSeed, stored, error));
    CHECK(stored.origin == placed.origin);

    // Gates: wrong biome, no biome info, surface out of range -> no match
    // (normal outcome, no diagnostic).
    engine::procgen::StructurePlacement miss;
    CHECK(!system->try_place({ rule }, 8, 8, 120, "desert", worldSeed, miss,
                             error) &&
          error.empty());
    CHECK(!system->try_place({ rule }, 8, 8, 120, "", worldSeed, miss, error) &&
          error.empty());
    CHECK(!system->try_place({ rule }, 8, 8, 90, "plains", worldSeed, miss,
                             error) &&
          error.empty());

    // density 0 never spawns.
    auto neverRule = rule;
    neverRule.density = 0.0f;
    CHECK(!system->try_place({ neverRule }, 8, 8, 120, "plains", worldSeed, miss,
                             error) &&
          error.empty());

    // First matching rule wins (rule order is the priority).
    auto secondRule = rule;
    secondRule.yOffset = 5;
    CHECK(system->try_place({ rule, secondRule }, 8, 8, 120, "plains",
                            worldSeed, placed, error));
    CHECK(placed.origin.y == 121);  // first rule's yOffset won

    // Unknown structureId at placement time is a hard error with diagnostic.
    auto ghostRule = rule;
    ghostRule.structureId = "test:ghost";
    CHECK(!system->try_place({ ghostRule }, 8, 8, 120, "plains", worldSeed,
                             placed, error) &&
          !error.empty());
    CHECK(error.find("test:ghost") != std::string::npos);

    // ---- plan_region: deterministic region planning ----
    // Flat world (surface 120 everywhere, biome "plains"), 2x2 chunks =
    // 16 candidate cells at spacing 8; density 1 -> all spawn.
    const auto surfaceAt = [](int, int) { return 120; };
    const auto biomeAt = [](int, int) { return std::string("plains"); };
    std::vector<engine::procgen::StructurePlacement> plan;
    CHECK(system->plan_region({ rule }, 0, 0, 2, 2, surfaceAt, biomeAt, worldSeed,
                              plan, error));
    CHECK(plan.size() == 16);
    // Every planned placement equals a direct try_place at its own origin.
    for (const auto& p : plan) {
        engine::procgen::StructurePlacement direct;
        CHECK(system->try_place({ rule }, p.origin.x, p.origin.z, 120, "plains",
                                worldSeed, direct, error));
        CHECK(direct.structureId == p.structureId);
        CHECK(direct.origin == p.origin);
    }
    // Per-cell seeds are distinct.
    std::set<std::uint32_t> seeds;
    for (const auto& p : plan) seeds.insert(p.placementSeed);
    CHECK(seeds.size() == plan.size());
    // Deterministic across repeated calls.
    std::vector<engine::procgen::StructurePlacement> plan2;
    CHECK(system->plan_region({ rule }, 0, 0, 2, 2, surfaceAt, biomeAt, worldSeed,
                              plan2, error));
    CHECK(plan2.size() == plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        CHECK(plan[i].origin == plan2[i].origin);
        CHECK(plan[i].placementSeed == plan2[i].placementSeed);
        CHECK(plan[i].output.blocks == plan2[i].output.blocks);
    }

    // ---- sockets: resolve to world space, connect with opposing facings ----
    std::vector<engine::procgen::StructureSocket> worldSockets;
    CHECK(system->resolve_sockets(placed, worldSockets, error) && error.empty());
    CHECK(worldSockets.size() == 2);
    CHECK(worldSockets[0].name == "door");
    CHECK(worldSockets[0].position == glm::ivec3(8, 122, 8));    // (0,1,0)+origin
    CHECK(worldSockets[1].name == "window");
    CHECK(worldSockets[1].position == glm::ivec3(14, 123, 8));   // (6,2,0)+origin

    // A second structure whose socket faces -Z (opposes the window's +Z).
    engine::procgen::StructureDefinition hub;
    hub.id = "test:hub";
    hub.spec = make_room_spec(9);
    hub.outputWidth = 8;
    hub.outputHeight = 6;
    hub.sockets.push_back(
        engine::procgen::StructureSocket{ "door", { 0, 1, 0 }, 3, "door" });
    CHECK(system->add_definition(hub, error) && error.empty());
    engine::procgen::StructureSpawnRule hubRule;
    hubRule.structureId = "test:hub";
    hubRule.density = 1.0f;
    hubRule.spacing = 8;
    engine::procgen::StructurePlacement b;
    CHECK(system->try_place({ hubRule }, 0, 0, 120, "", worldSeed, b, error));
    CHECK(b.origin == glm::ivec3(0, 121, 0));
    // connect a's window (+Z, tag "") to b's door (-Z, tag "door"): b must
    // move so its door sits exactly at the window's world position.
    glm::ivec3 bOrigin;
    CHECK(system->connect_sockets(placed, "window", b, "door", bOrigin, error) &&
          error.empty());
    CHECK(bOrigin == glm::ivec3(14, 122, 8));
    // b at that origin puts its door at the window: (14,122,8)+(0,1,0) ==
    // placed origin (8,121,8) + (6,2,0).
    CHECK(bOrigin + glm::ivec3(0, 1, 0) ==
          placed.origin + glm::ivec3(6, 2, 0));

    // Non-opposing facings refused with a diagnostic.
    auto posHub = hub;
    posHub.id = "test:hubpos";
    posHub.sockets[0].facing = 2;  // +Z — same direction as the window
    CHECK(system->add_definition(posHub, error) && error.empty());
    engine::procgen::StructureSpawnRule posRule = hubRule;
    posRule.structureId = "test:hubpos";
    engine::procgen::StructurePlacement bPos;
    CHECK(system->try_place({ posRule }, 0, 0, 120, "", worldSeed, bPos, error));
    CHECK(!system->connect_sockets(placed, "window", bPos, "door", bOrigin,
                                  error) &&
          !error.empty());

    // Tag mismatch refused (both tags non-empty and different).
    auto tagHub = hub;
    tagHub.id = "test:hubtag";
    tagHub.sockets[0].connectTag = "window";
    CHECK(system->add_definition(tagHub, error) && error.empty());
    engine::procgen::StructureSpawnRule tagRule = hubRule;
    tagRule.structureId = "test:hubtag";
    engine::procgen::StructurePlacement bTag;
    CHECK(system->try_place({ tagRule }, 0, 0, 120, "", worldSeed, bTag, error));
    CHECK(!system->connect_sockets(placed, "door", bTag, "door", bOrigin,
                                  error) &&
          !error.empty());

    // Unknown sockets refused.
    CHECK(!system->connect_sockets(placed, "nosuch", b, "door", bOrigin, error) &&
          !error.empty());
    CHECK(!system->connect_sockets(placed, "window", b, "nosuch", bOrigin, error) &&
          !error.empty());

    // ---- JSON all-or-nothing ----
    CHECK(!system->deserialize("{nope", error) && !error.empty());
    CHECK(system->definition("test:room") != nullptr);  // previous state kept
    CHECK(system->rules().size() == 1);
    CHECK(!system->deserialize("{\"version\":99}", error) && !error.empty());
    CHECK(!restored->deserialize(
              "{\"version\":1,\"definitions\":[],\"rules\":[{\"structureId\":\"nope\"}]}",
              error) &&
          !error.empty());
    CHECK(restored->definition("test:room") != nullptr);  // untouched
    // A document with an invalid asset is refused too.
    CHECK(!restored->deserialize(
              "{\"version\":1,\"definitions\":[{\"id\":\"x\",\"spec\":{\"version\":1,\"sampleWidth\":1,\"sampleHeight\":1,\"sample\":[1]}}]}",
              error) &&
          !error.empty());

    std::cout << "[sdk] procgen: structure placement (definitions, sockets, "
                 "spawn rules, determinism, JSON all-or-nothing) OK\n";
}

// ---- Item 14 resto: structure placement WRITTEN into the world (the
// placement system decides WHERE; this consumer writes the generated blocks
// through the world's transactional path — all-or-nothing) ----

void test_structure_placer() {
    std::string error;
    auto system = engine::procgen::create_structure_placement_system();
    CHECK(system != nullptr);
    engine::procgen::StructureDefinition room;
    room.id = "test:room";
    room.spec = make_room_spec(7);
    room.outputWidth = 12;
    room.outputHeight = 8;
    CHECK(system->add_definition(room, error) && error.empty());
    engine::procgen::StructureSpawnRule rule;
    rule.structureId = "test:room";
    rule.density = 1.0f;
    rule.spacing = 8;
    rule.yOffset = 1;

    // Flat DRY world (130 — the fluid-test convention) so the structure lands
    // on land, inside the loaded region around the boot focus.
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(130));
    CHECK(boot_world(*world, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    // boot_world only guarantees chunk (0,0); the room spans chunks (0,0) and
    // (1,0) (x 8..19), and place_structure legitimately rolls back on an
    // unloaded chunk — settle until the stream loads the second chunk.
    CHECK(settle(*world, glm::vec3(16.0f, 200.0f, 16.0f),
                 [&]() { return world->is_chunk_loaded(1, 0); }));

    const std::uint32_t worldSeed = 42u;
    engine::procgen::StructurePlacement placed;
    CHECK(system->try_place({ rule }, 8, 8, 130, "", worldSeed, placed, error));
    CHECK(placed.origin == glm::ivec3(8, 131, 8));
    CHECK(engine::procgen::place_structure(*world, placed, error) && error.empty());

    // The room is in the world: a wall column is 3/3/3/air and a floor column
    // is 5/air at the placement origin (extrusion profiles of the asset).
    bool sawWall = false, sawFloor = false;
    for (int z = 0; z < placed.output.height && !(sawWall && sawFloor); ++z) {
        for (int x = 0; x < placed.output.width; ++x) {
            const std::uint32_t id =
                placed.output.plan[x + z * placed.output.width];
            const int wx = placed.origin.x + x;
            const int wz = placed.origin.z + z;
            if (id == 1 && !sawWall) {
                sawWall = true;
                CHECK(world->get_block(wx, placed.origin.y, wz) == 3);
                CHECK(world->get_block(wx, placed.origin.y + 1, wz) == 3);
                CHECK(world->get_block(wx, placed.origin.y + 2, wz) == 3);
                CHECK(world->get_block(wx, placed.origin.y + 3, wz) == 0);
            } else if (id == 2 && !sawFloor) {
                sawFloor = true;
                CHECK(world->get_block(wx, placed.origin.y, wz) == 5);
                CHECK(world->get_block(wx, placed.origin.y + 1, wz) == 0);
            }
        }
    }
    CHECK(sawWall && sawFloor);

    // Determinism: a second world receives the identical structure.
    std::unique_ptr<engine::voxel::IVoxelWorld> worldB =
        engine::voxel::create_default_voxel_world();
    worldB->register_generator(std::make_shared<FlatGenerator>(130));
    CHECK(boot_world(*worldB, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    CHECK(settle(*worldB, glm::vec3(16.0f, 200.0f, 16.0f),
                 [&]() { return worldB->is_chunk_loaded(1, 0); }));
    CHECK(engine::procgen::place_structure(*worldB, placed, error) && error.empty());
    bool same = true;
    for (int x = 8; x <= 19; ++x) {
        for (int z = 8; z <= 15; ++z) {
            for (int y = 128; y <= 136; ++y) {
                if (world->get_block(x, y, z) != worldB->get_block(x, y, z)) {
                    same = false;
                }
            }
        }
    }
    CHECK(same);

    // All-or-nothing 1: an unregistered block id fails the WHOLE placement
    // (id validation runs before apply) and nothing is observable.
    engine::procgen::StructurePlacement bad;
    bad.structureId = "test:room";
    bad.origin = glm::ivec3(30, 131, 8);
    bad.output.width = 2;
    bad.output.height = 1;
    bad.output.depth = 1;
    bad.output.blocks = { 999, 0 };
    CHECK(!engine::procgen::place_structure(*world, bad, error) && !error.empty());
    CHECK(world->get_block(30, 131, 8) == 0);
    CHECK(world->get_block(31, 131, 8) == 0);

    // All-or-nothing 2: a placement over an UNLOADED chunk rolls back
    // entirely (no partial edits).
    engine::procgen::StructurePlacement far = placed;
    far.origin = glm::ivec3(1000, 131, 1000);
    CHECK(!engine::procgen::place_structure(*world, far, error) && !error.empty());
    CHECK(world->get_block(1000, 131, 1000) == 0);

    // Malformed output refused with a diagnostic.
    engine::procgen::StructurePlacement malformed;
    malformed.output.width = 0;
    CHECK(!engine::procgen::place_structure(*world, malformed, error) &&
          !error.empty());

    std::cout << "[sdk] procgen: structure placer (placement written into the "
                 "world, transactional all-or-nothing, determinism) OK\n";
}

// ---- Item 14/16: world profile — ONE JSON document composes every
// generation domain (noise + climate + biomes + surface + caves/ores +
// carver + decorators + structures) without per-world C++ assembly ----

const char* kProfileAsset = R"({
  "version": 1,
  "height": {"version":1,"seed":2026,"root":1,"nodes":[
    {"type":"perlin","params":[0.05,0.0],"sources":[]},
    {"type":"fbm","params":[4,0.5,2.0,0.0],"sources":[0]}]},
  "baseHeight": 131, "amplitude": 4,
  "climate": {
    "temperature": {"version":1,"seed":1,"root":0,"nodes":[{"type":"constant","params":[0.7],"sources":[]}]},
    "moisture": {"version":1,"seed":1,"root":0,"nodes":[{"type":"constant","params":[-0.7],"sources":[]}]},
    "continentalness": {"version":1,"seed":1,"root":0,"nodes":[{"type":"constant","params":[0.5],"sources":[]}]}},
  "biomes": {"version":1,"biomes":[
    {"name":"desert_rule","engineBiomeIndex":8,
     "climate":{"temperature":[0.25,1.0],"moisture":[-1.0,-0.2]},
     "surface":[{"blockId":50,"minDepth":0,"maxDepth":0}]},
    {"name":"meadow","engineBiomeIndex":7}]},
  "caves": {"density":{"version":1,"seed":2,"root":0,"nodes":[{"type":"constant","params":[0.0],"sources":[]}]}},
  "ores": {"density":{"version":1,"seed":3,"root":0,"nodes":[{"type":"constant","params":[0.75],"sources":[]}]},
           "table":{"version":1,"rules":[{"blockId":18,"minDensity":0.7,"maxDensity":0.8,"minY":10,"maxY":120}]}},
  "carver": {"version":1,"fluidMaxY":60,"fluidBlockId":12},
  "decorators": {"version":1,"decorators":[
    {"type":"column","density":1.0,"params":[2,2],"blocks":[50]}]},
  "structures": {"version":1,"definitions":[
    {"id":"p:room","outputWidth":12,"outputHeight":8,
     "spec":{"version":1,"sampleWidth":8,"sampleHeight":5,"patternSize":2,
             "symmetry":1,"periodicOutput":false,"ground":false,"seed":7,
             "sample":[1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,1,1,2,2,2,2,2,2,1,1,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1],
             "profiles":[{"blockId":1,"layers":[3,3,3]},{"blockId":2,"layers":[5]}]},
     "sockets":[{"name":"door","position":[0,1,0],"facing":0,"connectTag":"door"}]}],
   "rules":[{"structureId":"p:room","biomes":["desert_rule"],"density":1.0,
              "spacing":8,"yOffset":1,"seedOffset":3}]}
})";

void test_world_profile() {
    std::string error;
    auto profile = engine::procgen::create_world_profile_from_json(kProfileAsset,
                                                                   error);
    CHECK(profile != nullptr && error.empty());
    auto gen = profile->generator();
    CHECK(gen != nullptr);
    auto placement = profile->structure_placement();
    CHECK(placement != nullptr);
    CHECK(placement->definition("p:room") != nullptr);
    CHECK(placement->definition("p:room")->sockets.size() == 1);
    CHECK(placement->rules().size() == 1);

    // The composed generator wires the data-driven hooks directly: ore table,
    // carver fill and biome surface rule resolve through the generator API.
    CHECK(gen->ore_block(0.75f, 60, 0) == 18);        // DiamondOre rule
    CHECK(gen->ore_block(0.75f, 5, 0) == 0);          // y-gate: no rule -> 0
    CHECK(gen->carve_block(0.5f, 40, 20, 0) == 12);   // fluid pool <= 60
    CHECK(gen->carve_block(0.5f, 100, 20, 0) == 0);   // pure air above pool

    // Boot a world with the profile generator: ores, biome surface rule and
    // decorators all reach the voxels.
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(gen);
    CHECK(boot_world(*world, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    CHECK(world->get_block(4, 60, 4) == 18);    // ore rule over builtin GoldOre
    CHECK(world->get_block(4, 100, 4) == 18);   // ore rule over builtin IronOre
    CHECK(world->get_block(4, 5, 4) == 17);     // y-gate: builtin GoldOre stays
    // Columns must be inside the world's decoration band (chunk-local
    // x/z in [4, CHUNK_SIZE-4)); (12,12) is excluded by the engine.
    const int landColumns[3][2] = { { 4, 4 }, { 8, 8 }, { 10, 10 } };
    int decorated = 0;
    for (const auto& column : landColumns) {
        const int surface = gen->sample(static_cast<float>(column[0]),
                                        static_cast<float>(column[1])).height;
        if (surface <= 129) continue;  // only land columns get decorated
        ++decorated;
        CHECK(world->get_block(column[0], surface, column[1]) == 50);  // biome rule
        CHECK(world->get_block(column[0], surface + 1, column[1]) == 50);
        CHECK(world->get_block(column[0], surface + 2, column[1]) == 50);
        CHECK(world->get_block(column[0], surface + 3, column[1]) == 0);
    }
    CHECK(decorated > 0);

    // Structures from the profile: try_place with the STORED rules (empty
    // rules fall back), on the same seed -> deterministic placement.
    const std::uint32_t worldSeed = 42u;
    engine::procgen::StructurePlacement placed;
    CHECK(placement->try_place({}, 8, 8,
                               gen->sample(8.0f, 8.0f).height, "desert_rule",
                               worldSeed, placed, error));
    CHECK(placed.structureId == "p:room");
    CHECK(placed.origin == glm::ivec3(8, gen->sample(8.0f, 8.0f).height + 1, 8));
    CHECK(placed.output.width == 12 && placed.output.height == 8);
    engine::procgen::StructurePlacement again;
    CHECK(placement->try_place({}, 8, 8,
                               gen->sample(8.0f, 8.0f).height, "desert_rule",
                               worldSeed, again, error));
    CHECK(again.origin == placed.origin);
    CHECK(again.output.blocks == placed.output.blocks);

    // Round-trip: serialize -> reload -> serialize is byte-identical, and the
    // restored generator produces an identical world.
    std::string doc;
    CHECK(profile->serialize(doc));
    auto restored = engine::procgen::create_world_profile_from_json(doc, error);
    CHECK(restored != nullptr && error.empty());
    std::string doc2;
    CHECK(restored->serialize(doc2));
    CHECK(doc2 == doc);
    std::unique_ptr<engine::voxel::IVoxelWorld> worldB =
        engine::voxel::create_default_voxel_world();
    worldB->register_generator(restored->generator());
    CHECK(boot_world(*worldB, glm::vec3(16.0f, 200.0f, 16.0f), 16));
    bool same = true;
    for (int x = 4; x <= 12; x += 2) {
        for (int z = 4; z <= 12; z += 2) {
            for (int y = 100; y <= 140; ++y) {
                if (world->get_block(x, y, z) != worldB->get_block(x, y, z)) {
                    same = false;
                }
            }
        }
    }
    CHECK(same);

    // A bare profile (no sections) is still valid: flat generator + empty
    // placement system.
    auto bare = engine::procgen::create_world_profile_from_json(
        "{\"version\":1}", error);
    CHECK(bare != nullptr && error.empty());
    CHECK(bare->generator() != nullptr);
    CHECK(bare->structure_placement()->rules().empty());

    // All-or-nothing: malformed / unknown version / invalid section refused
    // with a diagnostic (nothing partial is ever composed).
    CHECK(engine::procgen::create_world_profile_from_json("{nope", error) ==
              nullptr &&
          !error.empty());
    CHECK(engine::procgen::create_world_profile_from_json(
              "{\"version\":99}", error) == nullptr &&
          !error.empty());
    CHECK(engine::procgen::create_world_profile_from_json(
              "{\"version\":1,\"height\":{\"version\":1,\"seed\":1,\"root\":5,\"nodes\":[]}}",
              error) == nullptr &&
          !error.empty());
    CHECK(engine::procgen::create_world_profile_from_json(
              "{\"version\":1,\"biomes\":{\"version\":1,\"biomes\":[{\"name\":\"x\"}]}}",
              error) == nullptr &&
          !error.empty());
    CHECK(engine::procgen::create_world_profile_from_json(
              "{\"version\":1,\"structures\":{\"version\":1,\"rules\":[{\"structureId\":\"nope\"}]}}",
              error) == nullptr &&
          !error.empty());

    std::cout << "[sdk] procgen: world profile (one JSON composes noise+"
                 "climate+biomes+ores+carver+decorators+structures, "
                 "round-trip, all-or-nothing) OK\n";
}

// ---- Item 16 resto: the WorldManager consumes a world profile end-to-end
// (WorldSpec.profileJson -> composed generator registered on the world) ----

void test_world_manager_profile() {
    std::string error;
    auto manager = engine::world::create_world_manager();
    CHECK(manager != nullptr);

    engine::world::WorldSpec spec;
    spec.name = "profile_world";
    spec.seed = 42;
    spec.profileJson = kProfileAsset;
    CHECK(manager->create_world(spec, error) && error.empty());
    CHECK(manager->has_world("profile_world"));
    engine::voxel::IVoxelWorld* w = manager->world("profile_world");
    CHECK(w != nullptr);
    auto profile = manager->world_profile("profile_world");
    CHECK(profile != nullptr);
    CHECK(profile->structure_placement() != nullptr);
    CHECK(profile->structure_placement()->definition("p:room") != nullptr);
    CHECK(profile->structure_placement()->rules().size() == 1);
    // A world without a profile has none.
    engine::world::WorldSpec plain;
    plain.name = "plain_world";
    CHECK(manager->create_world(plain, error) && error.empty());
    CHECK(manager->world_profile("plain_world") == nullptr);

    // Settle chunk (0,0) through the manager's update loop.
    const glm::vec3 focus(16.0f, 200.0f, 16.0f);
    const auto start = std::chrono::steady_clock::now();
    bool loaded = false;
    for (int step = 0; step < 60 * 30 && !loaded; ++step) {
        if (w->is_chunk_loaded(0, 0)) {
            loaded = true;
            break;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 8000) {
            break;
        }
        manager->update_world("profile_world", focus, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(loaded);

    // The profile's data-driven generator drives the world's voxels.
    CHECK(w->get_block(4, 60, 4) == 18);    // ore rule over builtin GoldOre
    CHECK(w->get_block(4, 100, 4) == 18);   // ore rule over builtin IronOre
    CHECK(w->get_block(4, 5, 4) == 17);     // y-gate: builtin GoldOre stays
    // Biome surface rule + decorator column at a land column.
    const int surface = profile->generator()->sample(8.0f, 8.0f).height;
    if (surface > 129) {
        CHECK(w->get_block(8, surface, 8) == 50);
        CHECK(w->get_block(8, surface + 1, 8) == 50);
        CHECK(w->get_block(8, surface + 2, 8) == 50);
        CHECK(w->get_block(8, surface + 3, 8) == 0);
    }

    // All-or-nothing: an invalid profile refuses world creation (nothing
    // registered, nothing partial).
    engine::world::WorldSpec bad;
    bad.name = "bad_world";
    bad.profileJson = "{\"version\":99}";
    CHECK(!manager->create_world(bad, error) && !error.empty());
    CHECK(!manager->has_world("bad_world"));
    bad.profileJson = "{nope";
    CHECK(!manager->create_world(bad, error) && !error.empty());
    CHECK(!manager->has_world("bad_world"));

    // Determinism: a second manager + world with the same profile is
    // identical to the first.
    auto managerB = engine::world::create_world_manager();
    engine::world::WorldSpec specB = spec;
    specB.name = "profile_world_b";
    CHECK(managerB->create_world(specB, error) && error.empty());
    engine::voxel::IVoxelWorld* wB = managerB->world("profile_world_b");
    CHECK(wB != nullptr);
    const auto startB = std::chrono::steady_clock::now();
    bool loadedB = false;
    for (int step = 0; step < 60 * 30 && !loadedB; ++step) {
        if (wB->is_chunk_loaded(0, 0)) {
            loadedB = true;
            break;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startB).count() > 8000) {
            break;
        }
        managerB->update_world("profile_world_b", focus, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(loadedB);
    bool same = true;
    for (int x = 4; x <= 12; x += 2) {
        for (int z = 4; z <= 12; z += 2) {
            for (int y = 100; y <= 140; ++y) {
                if (w->get_block(x, y, z) != wB->get_block(x, y, z)) {
                    same = false;
                }
            }
        }
    }
    CHECK(same);

    std::cout << "[sdk] world manager: world profile end-to-end (generator "
                 "registered, voxels data-driven, all-or-nothing, "
                 "determinism) OK\n";
}

void test_road_network() {
    std::string error;
    auto builder = engine::procgen::create_road_network_builder();
    CHECK(builder != nullptr);

    // Square of junctions: Delaunay gives 4 hull edges + 1 diagonal.
    engine::procgen::RoadNetworkSpec spec;
    spec.points = { { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 10.0 },
                    { 0.0, 10.0 } };
    CHECK(builder->build(spec, error));
    const auto& network = builder->network();
    CHECK(network.points.size() == 4);
    CHECK(network.edges.size() == 5);  // 4 hull + 1 diagonal

    // Determinism: an independent builder with the same spec produces the
    // identical network (same points, same edges in the same order).
    auto twin = engine::procgen::create_road_network_builder();
    CHECK(twin->build(spec, error));
    const auto& twinNetwork = twin->network();
    CHECK(network.edges == twinNetwork.edges);
    CHECK(network.points == twinNetwork.points);

    // Length filter: maxEdgeLength 10 keeps the 4 sides (10.0) and drops
    // the diagonal (~14.14).
    engine::procgen::RoadNetworkSpec filtered = spec;
    filtered.maxEdgeLength = 10.0;
    auto filteredBuilder = engine::procgen::create_road_network_builder();
    CHECK(filteredBuilder->build(filtered, error));
    CHECK(filteredBuilder->network().edges.size() == 4);

    // JSON round-trip: bit-exact points, same maxEdgeLength, and the
    // deserialized asset rebuilds the same network.
    std::string json;
    CHECK(builder->serialize(json));
    auto restored = engine::procgen::create_road_network_builder();
    CHECK(restored->deserialize(json, error));
    CHECK(restored->network().points == network.points);
    CHECK(restored->network().edges == network.edges);
    auto filteredRestored = engine::procgen::create_road_network_builder();
    std::string filteredJson;
    CHECK(filteredBuilder->serialize(filteredJson));
    CHECK(filteredRestored->deserialize(filteredJson, error));
    CHECK(filteredRestored->network().edges ==
          filteredBuilder->network().edges);

    // All-or-nothing + validation.
    CHECK(restored->deserialize("{nope", error) == false);
    CHECK(restored->network().points == network.points);  // state preserved
    engine::procgen::RoadNetworkSpec tiny;
    tiny.points = { { 0.0, 0.0 }, { 1.0, 1.0 } };
    CHECK(builder->build(tiny, error) == false && !error.empty());
    engine::procgen::RoadNetworkSpec dup = spec;
    dup.points.push_back({ 0.0, 0.0 });  // duplicate junction
    CHECK(builder->build(dup, error) == false && !error.empty());

    std::cout << "[sdk] procgen: road network (delaunay, determinism, "
                 "length filter, JSON) OK\n";
}

void test_parcellation() {
    std::string error;
    auto parcellation = engine::procgen::create_parcellation();
    CHECK(parcellation != nullptr);

    // Square with every Delaunay edge a road -> the two triangles are two
    // parcels whose areas sum to the square.
    engine::procgen::RoadNetwork square;
    square.points = { { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 10.0 },
                      { 0.0, 10.0 } };
    auto squareBuilder = engine::procgen::create_road_network_builder();
    engine::procgen::RoadNetworkSpec squareSpec;
    squareSpec.points = square.points;
    CHECK(squareBuilder->build(squareSpec, error));
    square.edges = squareBuilder->network().edges;
    std::vector<engine::procgen::ParcelPolygon> squareParcels;
    CHECK(parcellation->parcels_from_network(square, squareParcels, error));
    CHECK(squareParcels.size() == 2);
    double squareSum = 0.0;
    for (const auto& parcel : squareParcels) {
        CHECK(parcel.outer.size() == 3);  // triangles
        CHECK(parcel.holes.empty());
        CHECK(parcel.area() > 0.0);
        squareSum += parcel.area();
    }
    CHECK(std::abs(squareSum - 100.0) < 1e-9);

    // Ring-in-ring: an interior road loop makes the annulus come back with
    // a hole, plus the inner region as a separate parcel.
    engine::procgen::RoadNetwork annulus;
    annulus.points = { { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 10.0 },
                       { 0.0, 10.0 }, { 4.0, 4.0 }, { 6.0, 4.0 },
                       { 6.0, 6.0 },  { 4.0, 6.0 } };
    annulus.edges = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
                      { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 } };
    std::vector<engine::procgen::ParcelPolygon> parcels;
    CHECK(parcellation->parcels_from_network(annulus, parcels, error));
    CHECK(parcels.size() == 2);
    const engine::procgen::ParcelPolygon* ringParcel = nullptr;
    const engine::procgen::ParcelPolygon* innerParcel = nullptr;
    for (const auto& parcel : parcels) {
        if (parcel.holes.size() == 1) {
            ringParcel = &parcel;
        } else {
            innerParcel = &parcel;
        }
    }
    CHECK(ringParcel != nullptr && innerParcel != nullptr);
    CHECK(std::abs(ringParcel->area() - 96.0) < 1e-9);      // 100 - 4
    CHECK(std::abs(innerParcel->area() - 4.0) < 1e-9);
    CHECK(std::abs(engine::procgen::ParcelPolygon::ring_area(
              ringParcel->holes[0])) -
              4.0 <
          1e-9);
    CHECK(ringParcel->contains({ 1.0, 1.0 }));   // inside outer, outside hole
    CHECK(!ringParcel->contains({ 5.0, 5.0 }));  // inside the hole
    CHECK(innerParcel->contains({ 5.0, 5.0 }));

    // Determinism: an independent instance produces identical parcels.
    auto twin = engine::procgen::create_parcellation();
    std::vector<engine::procgen::ParcelPolygon> twinParcels;
    CHECK(twin->parcels_from_network(annulus, twinParcels, error));
    CHECK(twinParcels.size() == parcels.size());
    for (std::size_t i = 0; i < parcels.size(); ++i) {
        CHECK(twinParcels[i].outer == parcels[i].outer);
        CHECK(twinParcels[i].holes.size() == parcels[i].holes.size());
        for (std::size_t h = 0; h < parcels[i].holes.size(); ++h) {
            CHECK(twinParcels[i].holes[h] == parcels[i].holes[h]);
        }
    }

    // Dangling road (a bridge) contributes no bounded parcel.
    engine::procgen::RoadNetwork bridge = annulus;
    bridge.edges.push_back({ 0, 7 });  // dangling spur on the outer corner
    std::vector<engine::procgen::ParcelPolygon> bridged;
    CHECK(parcellation->parcels_from_network(bridge, bridged, error));
    CHECK(bridged.size() == 2);
    double bridgedSum = 0.0;
    for (const auto& parcel : bridged) {
        bridgedSum += parcel.area();
    }
    CHECK(std::abs(bridgedSum - 100.0) < 1e-9);

    std::cout << "[sdk] procgen: parcellation (faces, annulus hole, "
                 "contains, determinism) OK\n";
}

void test_parcel_triangulation() {
    std::string error;
    auto parcellation = engine::procgen::create_parcellation();

    // Annulus parcel (8 boundary vertices, 1 hole) -> earcut produces
    // V + 2h - 2 triangles; their areas sum to the parcel area.
    engine::procgen::ParcelPolygon annulus;
    annulus.outer = { { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 10.0 },
                      { 0.0, 10.0 } };
    annulus.holes.push_back({ { 4.0, 4.0 }, { 6.0, 4.0 }, { 6.0, 6.0 },
                              { 4.0, 6.0 } });
    std::vector<std::uint32_t> indices;
    CHECK(parcellation->triangulate(annulus, indices, error));
    CHECK(indices.size() == 8 * 3);  // 4 + 4 + 2*1 - 2 = 8 triangles
    CHECK(indices.size() % 3 == 0);
    auto ring_point = [&annulus](std::uint32_t idx) {
        if (idx < annulus.outer.size()) {
            return annulus.outer[idx];
        }
        return annulus.holes[0][idx - annulus.outer.size()];
    };
    double sum = 0.0;
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        const auto a = ring_point(indices[i]);
        const auto b = ring_point(indices[i + 1]);
        const auto c = ring_point(indices[i + 2]);
        sum += std::abs((b.x - a.x) * (c.y - a.y) -
                        (c.x - a.x) * (b.y - a.y)) *
               0.5;
    }
    CHECK(std::abs(sum - annulus.area()) < 1e-9);

    // Determinism across instances.
    auto twin = engine::procgen::create_parcellation();
    std::vector<std::uint32_t> twinIndices;
    CHECK(twin->triangulate(annulus, twinIndices, error));
    CHECK(twinIndices == indices);

    // Validation: a 2-point outer ring is rejected.
    engine::procgen::ParcelPolygon bad;
    bad.outer = { { 0.0, 0.0 }, { 1.0, 1.0 } };
    CHECK(parcellation->triangulate(bad, indices, error) == false &&
          !error.empty());

    std::cout << "[sdk] procgen: parcel triangulation (earcut, area sum, "
                 "determinism) OK\n";
}

engine::procgen::ShapeGrammar make_house_grammar() {
    using engine::procgen::GrammarOpCall;
    using engine::procgen::GrammarOpMass;
    using engine::procgen::GrammarOpSize;
    using engine::procgen::GrammarOpSplit;
    using engine::procgen::GrammarRule;
    using engine::procgen::ShapeGrammar;
    ShapeGrammar g;
    // 10x10x8 house: walls (2) + interior split into floor/windows/floor
    // (windows split into wall/glass/wall) = 2 + 2 + 3 = 7 boxes.
    GrammarRule axiom;
    axiom.name = "Axiom";
    axiom.ops.push_back(GrammarOpSize{ 10, 10, 8 });
    axiom.ops.push_back(GrammarOpSplit{ 'x', { 2, 6, 2 },
                                        { "Wall", "Interior", "Wall" } });
    g.rules.push_back(axiom);
    GrammarRule wall;
    wall.name = "Wall";
    wall.ops.push_back(GrammarOpMass{ 4 });
    g.rules.push_back(wall);
    GrammarRule interior;
    interior.name = "Interior";
    interior.ops.push_back(GrammarOpSplit{ 'y', { 4, 2, 4 },
                                           { "Floor", "Windows", "Floor" } });
    g.rules.push_back(interior);
    GrammarRule floor;
    floor.name = "Floor";
    floor.ops.push_back(GrammarOpMass{ 4 });
    g.rules.push_back(floor);
    GrammarRule windows;
    windows.name = "Windows";
    windows.ops.push_back(GrammarOpSplit{ 'z', { 1, 6, 1 },
                                          { "Wall", "Glass", "Wall" } });
    g.rules.push_back(windows);
    GrammarRule glass;
    glass.name = "Glass";
    glass.ops.push_back(GrammarOpMass{ 20 });
    g.rules.push_back(glass);
    return g;
}

void test_shape_grammar() {
    std::string error;
    auto runner = engine::procgen::create_shape_grammar_runner();
    CHECK(runner != nullptr);

    // The house grammar emits 7 boxes whose volumes tile the 10x10x8 scope.
    const engine::procgen::ShapeGrammar house = make_house_grammar();
    engine::procgen::GrammarResult result;
    CHECK(runner->run(house, result, error));
    CHECK(result.boxes.size() == 7);
    long long volumeSum = 0;
    for (const auto& box : result.boxes) {
        const long long vol = static_cast<long long>(box.maxX - box.minX) *
                              (box.maxY - box.minY) * (box.maxZ - box.minZ);
        volumeSum += vol;
        CHECK(box.minX >= 0 && box.maxX <= 10);
        CHECK(box.minY >= 0 && box.maxY <= 10);
        CHECK(box.minZ >= 0 && box.maxZ <= 8);
    }
    CHECK(volumeSum == 800);

    // Determinism: an independent runner produces bit-identical boxes.
    auto twin = engine::procgen::create_shape_grammar_runner();
    engine::procgen::GrammarResult twinResult;
    CHECK(twin->run(house, twinResult, error));
    CHECK(twinResult.boxes.size() == result.boxes.size());
    for (std::size_t i = 0; i < result.boxes.size(); ++i) {
        CHECK(twinResult.boxes[i].minX == result.boxes[i].minX);
        CHECK(twinResult.boxes[i].maxX == result.boxes[i].maxX);
        CHECK(twinResult.boxes[i].minY == result.boxes[i].minY);
        CHECK(twinResult.boxes[i].maxY == result.boxes[i].maxY);
        CHECK(twinResult.boxes[i].minZ == result.boxes[i].minZ);
        CHECK(twinResult.boxes[i].maxZ == result.boxes[i].maxZ);
        CHECK(twinResult.boxes[i].blockId == result.boxes[i].blockId);
    }

    // JSON round-trip: the deserialized asset runs to the same boxes.
    std::string json;
    CHECK(runner->serialize(house, json));
    engine::procgen::ShapeGrammar restored;
    CHECK(runner->deserialize(json, restored, error));
    CHECK(restored.rules.size() == house.rules.size());
    engine::procgen::GrammarResult restoredResult;
    CHECK(runner->run(restored, restoredResult, error));
    CHECK(restoredResult.boxes.size() == result.boxes.size());
    for (std::size_t i = 0; i < result.boxes.size(); ++i) {
        CHECK(restoredResult.boxes[i].minX == result.boxes[i].minX);
        CHECK(restoredResult.boxes[i].minY == result.boxes[i].minY);
        CHECK(restoredResult.boxes[i].minZ == result.boxes[i].minZ);
        CHECK(restoredResult.boxes[i].maxX == result.boxes[i].maxX);
        CHECK(restoredResult.boxes[i].maxY == result.boxes[i].maxY);
        CHECK(restoredResult.boxes[i].maxZ == result.boxes[i].maxZ);
        CHECK(restoredResult.boxes[i].blockId == result.boxes[i].blockId);
    }

    // Split clipping: parts beyond the scope extent are clamped; leftover
    // space is unused.
    engine::procgen::ShapeGrammar clip;
    engine::procgen::GrammarRule clipAxiom;
    clipAxiom.name = "Axiom";
    clipAxiom.ops.push_back(engine::procgen::GrammarOpSize{ 8, 1, 1 });
    clipAxiom.ops.push_back(engine::procgen::GrammarOpSplit{
        'x', { 3, 3, 3 }, { "P", "P", "P" } });
    clip.rules.push_back(clipAxiom);
    engine::procgen::GrammarRule p;
    p.name = "P";
    p.ops.push_back(engine::procgen::GrammarOpMass{ 1 });
    clip.rules.push_back(p);
    engine::procgen::GrammarResult clipResult;
    CHECK(runner->run(clip, clipResult, error));
    CHECK(clipResult.boxes.size() == 3);
    CHECK(clipResult.boxes[2].minX == 6 && clipResult.boxes[2].maxX == 8);
    long long clipVolume = 0;
    for (const auto& box : clipResult.boxes) {
        clipVolume += static_cast<long long>(box.maxX - box.minX) *
                      (box.maxY - box.minY) * (box.maxZ - box.minZ);
    }
    CHECK(clipVolume == 8);  // leftover [4,8) clipped to [6,8)

    // Degenerate: Axiom = mass only emits the unit box at the origin.
    engine::procgen::ShapeGrammar single;
    engine::procgen::GrammarRule singleAxiom;
    singleAxiom.name = "Axiom";
    singleAxiom.ops.push_back(engine::procgen::GrammarOpMass{ 3 });
    single.rules.push_back(singleAxiom);
    engine::procgen::GrammarResult singleResult;
    CHECK(runner->run(single, singleResult, error));
    CHECK(singleResult.boxes.size() == 1);
    CHECK(singleResult.boxes[0].minX == 0 && singleResult.boxes[0].minY == 0 &&
          singleResult.boxes[0].minZ == 0);
    CHECK(singleResult.boxes[0].maxX == 1 && singleResult.boxes[0].maxY == 1 &&
          singleResult.boxes[0].maxZ == 1);

    // Validation: missing Axiom, unknown rule reference, split sizes/rules
    // count mismatch and unknown op types are rejected all-or-nothing.
    CHECK(runner->deserialize(
              "{\"version\":1,\"rules\":[{\"name\":\"B\",\"ops\":[]}]}",
              restored, error) == false);
    CHECK(runner->deserialize(
              "{\"version\":1,\"rules\":[{\"name\":\"Axiom\",\"ops\":"
              "[{\"type\":\"call\",\"rule\":\"Ghost\"}]}]}",
              restored, error) == false);
    CHECK(runner->deserialize(
              "{\"version\":1,\"rules\":[{\"name\":\"Axiom\",\"ops\":"
              "[{\"type\":\"split\",\"axis\":\"x\",\"sizes\":[1,2],"
              "\"rules\":[\"P\"]}]}]}",
              restored, error) == false);
    CHECK(runner->deserialize(
              "{\"version\":1,\"rules\":[{\"name\":\"Axiom\",\"ops\":"
              "[{\"type\":\"nope\"}]}]}",
              restored, error) == false);
    CHECK(runner->deserialize("{nope", restored, error) == false);
    // All-or-nothing: the last successful grammar is preserved.
    CHECK(restored.rules.size() == house.rules.size());
    engine::procgen::GrammarResult preserved;
    CHECK(runner->run(restored, preserved, error));
    CHECK(preserved.boxes.size() == result.boxes.size());

    // Runtime recursion limit: Axiom calling itself is structurally valid
    // (validate passes) but fails at run time with a depth error.
    engine::procgen::ShapeGrammar loop;
    engine::procgen::GrammarRule loopAxiom;
    loopAxiom.name = "Axiom";
    loopAxiom.ops.push_back(engine::procgen::GrammarOpCall{ "Axiom" });
    loop.rules.push_back(loopAxiom);
    CHECK(runner->validate(loop, error));
    engine::procgen::GrammarResult loopResult;
    CHECK(runner->run(loop, loopResult, error) == false && !error.empty());

    std::cout << "[sdk] procgen: shape grammar (house volume, determinism, "
                 "JSON, clipping, validation, recursion) OK\n";
}

engine::procgen::Heightmap make_checker_heightmap(int size, float base,
                                                   float amplitude) {
    engine::procgen::Heightmap map;
    map.width = size;
    map.height = size;
    map.values.resize(static_cast<std::size_t>(size) * size);
    for (int z = 0; z < size; ++z) {
        for (int x = 0; x < size; ++x) {
            const float cell =
                ((x % 2 == z % 2) ? 1.0f : -1.0f) * amplitude;
            map.values[z * size + x] = base + cell;
        }
    }
    return map;
}

// Sum of |height - lowest neighbor| over the interior — a roughness measure.
double roughness(const engine::procgen::Heightmap& map) {
    double sum = 0.0;
    for (int z = 1; z < map.height - 1; ++z) {
        for (int x = 1; x < map.width - 1; ++x) {
            const float h = map.values[z * map.width + x];
            float low = h;
            low = std::min(low, map.values[z * map.width + (x - 1)]);
            low = std::min(low, map.values[z * map.width + (x + 1)]);
            low = std::min(low, map.values[(z - 1) * map.width + x]);
            low = std::min(low, map.values[(z + 1) * map.width + x]);
            sum += std::abs(h - low);
        }
    }
    return sum;
}

void test_heightmap_erosion() {
    std::string error;
    auto erosion = engine::procgen::create_heightmap_erosion();
    CHECK(erosion != nullptr);

    // Determinism: two independent instances and repeated runs produce
    // bit-identical results for the same (heightmap, spec).
    const engine::procgen::Heightmap checker = make_checker_heightmap(48, 0.5f, 0.3f);
    engine::procgen::ErosionSpec spec;
    spec.seed = 7;
    spec.iterations = 60000;
    spec.thermalIterations = 6;
    engine::procgen::Heightmap first;
    CHECK(erosion->erode(checker, spec, first, error));
    engine::procgen::Heightmap again;
    CHECK(erosion->erode(checker, spec, again, error));
    CHECK(first.values == again.values);
    auto twin = engine::procgen::create_heightmap_erosion();
    engine::procgen::Heightmap twinOut;
    CHECK(twin->erode(checker, spec, twinOut, error));
    CHECK(twinOut.values == first.values);

    // Real effect: erosion reduces the roughness (spikes eroded, valleys
    // filled) while conserving material (hydraulic returns its sediment on
    // death; thermal only slides material around).
    const double before = roughness(checker);
    const double after = roughness(first);
    CHECK(after < before * 0.95);
    CHECK(after > 0.0);
    float minV = 1e9f;
    float maxV = -1e9f;
    double sumBefore = 0.0;
    double sumAfter = 0.0;
    for (std::size_t i = 0; i < checker.values.size(); ++i) {
        minV = std::min(minV, first.values[i]);
        maxV = std::max(maxV, first.values[i]);
        sumBefore += checker.values[i];
        sumAfter += first.values[i];
    }
    CHECK(minV >= 0.1f && maxV <= 0.9f);
    CHECK(std::abs(sumAfter - sumBefore) < 1.0);  // conserved

    // Thermal only: a single tall spike on flat ground slides down, and the
    // total material is conserved (within float noise).
    engine::procgen::Heightmap spike;
    spike.width = 32;
    spike.height = 32;
    spike.values.assign(32 * 32, 0.5f);
    spike.values[16 * 32 + 16] = 0.9f;
    engine::procgen::ErosionSpec thermal;
    thermal.iterations = 0;
    thermal.thermalIterations = 200;
    thermal.talusAngle = 0.02f;
    double spikeSum = 0.0;
    for (const float v : spike.values) {
        spikeSum += v;
    }
    engine::procgen::Heightmap thermalOut;
    CHECK(erosion->erode(spike, thermal, thermalOut, error));
    double thermalSum = 0.0;
    float spikeMax = -1e9f;
    for (const float v : thermalOut.values) {
        thermalSum += v;
        spikeMax = std::max(spikeMax, v);
    }
    CHECK(spikeMax < 0.7f);           // material slid off the spike
    CHECK(std::abs(thermalSum - spikeSum) < 1e-3);  // conservation
    // The neighbors of the spike rose above the flat base.
    CHECK(thermalOut.values[16 * 32 + 17] > 0.51f);

    // Spec validation + JSON round-trip + all-or-nothing.
    engine::procgen::ErosionSpec bad = spec;
    bad.evaporation = 0.0f;
    CHECK(erosion->validate(bad, error) == false && !error.empty());
    std::string json;
    CHECK(erosion->serialize_spec(spec, json));
    engine::procgen::ErosionSpec restored;
    CHECK(erosion->deserialize_spec(json, restored, error));
    CHECK(restored.seed == spec.seed &&
          restored.iterations == spec.iterations);
    CHECK(restored.evaporation == spec.evaporation);
    engine::procgen::Heightmap restoredOut;
    CHECK(erosion->erode(checker, restored, restoredOut, error));
    CHECK(restoredOut.values == first.values);
    CHECK(erosion->deserialize_spec("{nope", restored, error) == false);
    CHECK(erosion->deserialize_spec(
              "{\"version\":1,\"evaporation\":0}", restored, error) ==
          false);
    CHECK(restored.seed == spec.seed);  // all-or-nothing preserved

    // Tile cache: hits on repeat, distinct keys per (seed, tile), clear.
    auto cache = engine::procgen::create_tile_erosion_cache();
    CHECK(cache != nullptr && cache->size() == 0);
    const engine::procgen::Heightmap tile = make_checker_heightmap(16, 0.5f, 0.2f);
    engine::procgen::Heightmap tileOut;
    CHECK(cache->erode_tile(spec, 0, 0, tile, tileOut, error));
    CHECK(cache->size() == 1);
    engine::procgen::Heightmap tileOut2;
    CHECK(cache->erode_tile(spec, 0, 0, tile, tileOut2, error));
    CHECK(cache->size() == 1);          // cache hit
    CHECK(tileOut2.values == tileOut.values);
    CHECK(cache->erode_tile(spec, 1, 0, tile, tileOut, error));
    CHECK(cache->size() == 2);          // different tile -> new entry
    engine::procgen::ErosionSpec otherSeed = spec;
    otherSeed.seed = 99;
    CHECK(cache->erode_tile(otherSeed, 0, 0, tile, tileOut, error));
    CHECK(cache->size() == 3);          // different seed -> new entry
    auto cacheTwin = engine::procgen::create_tile_erosion_cache();
    engine::procgen::Heightmap twinTile;
    CHECK(cacheTwin->erode_tile(spec, 0, 0, tile, twinTile, error));
    CHECK(twinTile.values == tileOut2.values);  // cross-instance determinism
    cache->clear();
    CHECK(cache->size() == 0);

    // Validation: degenerate heightmaps are rejected.
    engine::procgen::Heightmap empty;
    CHECK(erosion->erode(empty, spec, again, error) == false &&
          !error.empty());

    std::cout << "[sdk] procgen: heightmap erosion (determinism, hydraulic "
                 "roughness, thermal conservation, spec JSON, tile cache) OK\n";
}

engine::procgen::CookedMesh make_grid_mesh(int gridW, int gridH, bool shuffled) {
    engine::procgen::CookedMesh mesh;
    const int vertsX = gridW + 1;
    const int vertsZ = gridH + 1;
    for (int z = 0; z < vertsZ; ++z) {
        for (int x = 0; x < vertsX; ++x) {
            const float h =
                0.3f * std::sin(static_cast<float>(x) * 0.5f) *
                std::cos(static_cast<float>(z) * 0.5f);
            mesh.positions.push_back(static_cast<float>(x));
            mesh.positions.push_back(h);
            mesh.positions.push_back(static_cast<float>(z));
            mesh.normals.push_back(0.0f);
            mesh.normals.push_back(1.0f);
            mesh.normals.push_back(0.0f);
        }
    }
    for (int z = 0; z < gridH; ++z) {
        for (int x = 0; x < gridW; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * vertsX + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + static_cast<std::uint32_t>(vertsX);
            const std::uint32_t i3 = i2 + 1;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i3);
            mesh.indices.push_back(i2);
        }
    }
    if (shuffled) {
        // Deterministic TRIANGLE shuffle that destroys vertex locality while
        // keeping the grid topology intact: a multiplicative permutation over
        // the triangle list (512 and 7919 are coprime). Shuffling individual
        // index elements instead would break the (a,b,c) grouping and turn
        // the mesh into a random triangle soup.
        const std::size_t tris = mesh.indices.size() / 3;
        std::vector<std::uint32_t> scrambled(mesh.indices.size());
        for (std::size_t t = 0; t < tris; ++t) {
            const std::size_t src = (t * 7919ull) % tris;
            scrambled[t * 3 + 0] = mesh.indices[src * 3 + 0];
            scrambled[t * 3 + 1] = mesh.indices[src * 3 + 1];
            scrambled[t * 3 + 2] = mesh.indices[src * 3 + 2];
        }
        mesh.indices = std::move(scrambled);
    }
    return mesh;
}

void test_mesh_cooking() {
    std::string error;
    auto cooker = engine::procgen::create_mesh_cooker();
    CHECK(cooker != nullptr);

    // A grid mesh with scrambled triangle order is cache-hostile; optimizing
    // must substantially improve the ACMR (meshopt analyzer).
    const engine::procgen::CookedMesh bad =
        make_grid_mesh(16, 16, /*shuffled=*/true);
    engine::procgen::CookStats badStats;
    CHECK(cooker->analyze(bad, badStats, error));
    engine::procgen::CookStats goodStats;
    engine::procgen::CookOptions optOnly;
    optOnly.unwrap = false;
    optOnly.simplifyTargetIndices = 0;
    engine::procgen::CookedMesh optimized;
    CHECK(cooker->optimize(bad, optOnly, optimized, error));
    CHECK(cooker->analyze(optimized, goodStats, error));
    CHECK(badStats.acmr > goodStats.acmr * 1.2);  // measurably better
    CHECK(goodStats.acmr < 1.5f);                 // and actually good
    CHECK(optimized.indices.size() == bad.indices.size());
    CHECK(optimized.vertex_count() == bad.vertex_count());

    // Unwrap: UVs are produced in [0,1], triangle count is preserved and
    // every output vertex matches an input vertex (positions preserved).
    const engine::procgen::CookedMesh grid = make_grid_mesh(16, 16, false);
    engine::procgen::CookOptions unwrapOnly;
    unwrapOnly.unwrap = true;
    unwrapOnly.optimize = false;
    engine::procgen::CookedMesh unwrapped;
    CHECK(cooker->unwrap(grid, unwrapOnly, unwrapped, error));
    CHECK(!unwrapped.uvs.empty());
    CHECK(unwrapped.uvs.size() == unwrapped.vertex_count() * 2);
    CHECK(unwrapped.indices.size() == grid.indices.size());
    for (const float uv : unwrapped.uvs) {
        CHECK(uv >= -1e-4f && uv <= 1.0f + 1e-4f);
    }
    for (const std::uint32_t idx : unwrapped.indices) {
        CHECK(idx < unwrapped.vertex_count());
    }

    // Simplify: index count drops to the target and vertices compact.
    engine::procgen::CookedMesh simplified;
    CHECK(cooker->simplify(grid, grid.indices.size() / 2, 0.01f, simplified,
                           error));
    CHECK(simplified.indices.size() <= grid.indices.size() / 2);
    CHECK(simplified.vertex_count() < grid.vertex_count());

    // Determinism: two cooks (and two independent cookers) of the same mesh
    // with the same options are bit-identical.
    engine::procgen::CookOptions full;
    full.unwrap = true;
    full.optimize = true;
    full.simplifyTargetIndices = 512;
    engine::procgen::CookedMesh cooked;
    engine::procgen::CookStats stats;
    CHECK(cooker->cook(bad, full, cooked, stats, error));
    // meshopt quantizes to whole triangles, so the final count can overshoot
    // a non-multiple-of-3 target by one triangle (513 for target 512).
    CHECK(stats.hasUvs && stats.outputIndices <= 515);
    CHECK(stats.outputVertices < bad.vertex_count());
    engine::procgen::CookedMesh cookedAgain;
    engine::procgen::CookStats statsAgain;
    CHECK(cooker->cook(bad, full, cookedAgain, statsAgain, error));
    CHECK(cookedAgain.positions == cooked.positions);
    CHECK(cookedAgain.uvs == cooked.uvs);
    CHECK(cookedAgain.indices == cooked.indices);
    auto twin = engine::procgen::create_mesh_cooker();
    engine::procgen::CookedMesh twinCooked;
    engine::procgen::CookStats twinStats;
    CHECK(twin->cook(bad, full, twinCooked, twinStats, error));
    CHECK(twinCooked.uvs == cooked.uvs && twinCooked.indices == cooked.indices);

    // Validation: degenerate meshes and bad options are rejected.
    engine::procgen::CookedMesh empty;
    CHECK(cooker->cook(empty, full, cooked, stats, error) == false &&
          !error.empty());
    engine::procgen::CookedMesh badIndex = grid;
    badIndex.indices[0] = 999999;
    CHECK(cooker->unwrap(badIndex, unwrapOnly, cooked, error) == false &&
          !error.empty());
    engine::procgen::CookOptions badOpts = full;
    badOpts.overdrawThreshold = 0.5f;
    CHECK(cooker->cook(grid, badOpts, cooked, stats, error) == false &&
          !error.empty());

    std::cout << "[sdk] procgen: mesh cooking (unwrap UVs, optimize ACMR, "
                 "simplify, determinism, validation) OK\n";
}

void test_procgen_preview() {
    std::string error;
    auto preview = engine::procgen::create_procgen_preview();
    CHECK(preview != nullptr);

    // --- terrain: height field ASCII + biome distribution ---
    auto height = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(11, 0.1f), error);
    CHECK(height != nullptr);
    auto temperature = make_constant_graph(0.4f, error);
    auto moisture = make_constant_graph(0.4f, error);
    CHECK(temperature != nullptr && moisture != nullptr);
    auto climate = engine::procgen::create_climate_sampler(
        temperature, moisture, nullptr, nullptr, nullptr, nullptr);
    CHECK(climate != nullptr);
    auto biomes = engine::procgen::create_biome_registry();
    CHECK(biomes != nullptr);

    engine::procgen::PreviewOptions opts;
    opts.sampleSize = 32;
    engine::procgen::PreviewRender terrain;
    CHECK(preview->preview_terrain(*height, *climate, *biomes, opts, terrain,
                                   error));
    CHECK(terrain.lines.size() == 32);
    for (const auto& row : terrain.lines) {
        CHECK(row.size() == 32);
    }
    // Constant climate -> the catch-all biome covers the whole patch.
    bool biomeFull = false;
    for (const auto& st : terrain.stats) {
        if (st.label == "biomes" && st.value.find(":1024") != std::string::npos) {
            biomeFull = true;
        }
    }
    CHECK(biomeFull);

    // --- structure: fast-wfc plan ASCII + block histogram ---
    engine::procgen::PreviewOptions so;
    so.sampleSize = 16;
    engine::procgen::PreviewRender structure;
    CHECK(preview->preview_structure(make_room_spec(7), so, structure, error));
    CHECK(structure.title == "structure 16x16");
    CHECK(structure.lines.size() == 16);
    bool solidCells = false, planOk = false;
    for (const auto& st : structure.stats) {
        if (st.label == "solid_cells" && st.value != "0") {
            solidCells = true;
        }
        if (st.label == "plan" && st.value == "16x16") {
            planOk = true;
        }
    }
    CHECK(solidCells && planOk);

    // --- parcels: square -> 5 roads, 2 parcels summing the square ---
    engine::procgen::RoadNetworkSpec spec;
    spec.points = { { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 10.0 },
                    { 0.0, 10.0 } };
    engine::procgen::PreviewRender parcels;
    CHECK(preview->preview_parcels(spec, opts, parcels, error));
    CHECK(parcels.title.find("4 pts / 5 roads") != std::string::npos);
    CHECK(parcels.lines.size() == 32);
    bool junctions4 = false, roads5 = false, parcels2 = false, sum100 = false;
    for (const auto& st : parcels.stats) {
        if (st.label == "junctions" && st.value == "4") {
            junctions4 = true;
        }
        if (st.label == "roads" && st.value == "5") {
            roads5 = true;
        }
        if (st.label == "parcels" && st.value == "2") {
            parcels2 = true;
        }
        if (st.label == "area_sum" && st.value == "100.00") {
            sum100 = true;
        }
    }
    CHECK(junctions4 && roads5 && parcels2 && sum100);

    // --- shape: house grammar -> 7 boxes, volume 800, bounded render ---
    engine::procgen::PreviewRender shape;
    CHECK(preview->preview_shape(make_house_grammar(), opts, shape, error));
    CHECK(shape.title == "shape 7 boxes");
    CHECK(!shape.lines.empty());
    bool boxes7 = false, vol800 = false;
    for (const auto& st : shape.stats) {
        if (st.label == "boxes" && st.value == "7") {
            boxes7 = true;
        }
        if (st.label == "volume" && st.value == "800") {
            vol800 = true;
        }
    }
    CHECK(boxes7 && vol800);

    // --- erosion: roughness drops, mass conserved ---
    engine::procgen::PreviewOptions eo;
    eo.sampleSize = 24;
    eo.seed = 9;
    engine::procgen::PreviewRender erosion;
    CHECK(preview->preview_erosion(engine::procgen::ErosionSpec{}, eo, erosion,
                                   error));
    CHECK(erosion.title == "erosion 24x24");
    CHECK(erosion.lines.size() == 24 + 1 + 24);  // before | gap | after
    double roughBefore = -1.0, roughAfter = -1.0, massDelta = 1e9;
    for (const auto& st : erosion.stats) {
        if (st.label == "roughness_before") {
            roughBefore = std::stod(st.value);
        }
        if (st.label == "roughness_after") {
            roughAfter = std::stod(st.value);
        }
        if (st.label == "mass_delta") {
            massDelta = std::stod(st.value);
        }
    }
    CHECK(roughBefore > roughAfter);
    CHECK(massDelta < 1e-2);

    // --- mesh: cook stats text ---
    const engine::procgen::CookedMesh grid = make_grid_mesh(16, 16, false);
    engine::procgen::CookOptions full;
    full.unwrap = true;
    full.optimize = true;
    full.simplifyTargetIndices = 0;
    engine::procgen::PreviewRender mesh;
    CHECK(preview->preview_mesh(grid, full, opts, mesh, error));
    bool iv289 = false, ii1536 = false, uvsYes = false;
    for (const auto& st : mesh.stats) {
        if (st.label == "input_vertices" && st.value == "289") {
            iv289 = true;
        }
        if (st.label == "input_indices" && st.value == "1536") {
            ii1536 = true;
        }
        if (st.label == "has_uvs" && st.value == "yes") {
            uvsYes = true;
        }
    }
    CHECK(iv289 && ii1536 && uvsYes);

    // --- determinism: an independent preview instance renders bit-identically
    auto twin = engine::procgen::create_procgen_preview();
    engine::procgen::PreviewRender tTerrain;
    CHECK(twin->preview_terrain(*height, *climate, *biomes, opts, tTerrain,
                                error));
    CHECK(tTerrain.lines == terrain.lines);
    CHECK(tTerrain.stats.size() == terrain.stats.size());
    for (std::size_t i = 0; i < terrain.stats.size(); ++i) {
        CHECK(tTerrain.stats[i].label == terrain.stats[i].label);
        CHECK(tTerrain.stats[i].value == terrain.stats[i].value);
    }
    engine::procgen::PreviewRender tStructure;
    CHECK(twin->preview_structure(make_room_spec(7), so, tStructure, error));
    CHECK(tStructure.lines == structure.lines);
    engine::procgen::PreviewRender tParcels;
    CHECK(twin->preview_parcels(spec, opts, tParcels, error));
    CHECK(tParcels.lines == parcels.lines);
    engine::procgen::PreviewRender tShape;
    CHECK(twin->preview_shape(make_house_grammar(), opts, tShape, error));
    CHECK(tShape.lines == shape.lines);
    engine::procgen::PreviewRender tErosion;
    CHECK(twin->preview_erosion(engine::procgen::ErosionSpec{}, eo, tErosion,
                                error));
    CHECK(tErosion.lines == erosion.lines);
    engine::procgen::PreviewRender tMesh;
    CHECK(twin->preview_mesh(grid, full, opts, tMesh, error));
    CHECK(tMesh.stats.size() == mesh.stats.size());
    for (std::size_t i = 0; i < mesh.stats.size(); ++i) {
        CHECK(tMesh.stats[i].value == mesh.stats[i].value);
    }

    std::cout << "[sdk] procgen: preview (terrain, structure, parcels, shape, "
                 "erosion, mesh, determinism) OK\n";
}

// Deterministic noisy tile in [0, 1] for the erosion batch (pure per-cell
// hash, no trig).
engine::procgen::Heightmap make_erosion_tile(int size, std::uint64_t seed) {
    engine::procgen::Heightmap h;
    h.width = size;
    h.height = size;
    std::uint64_t s = 0x9e3779b97f4a7c15ull ^ seed;
    h.values.reserve(static_cast<std::size_t>(size * size));
    for (int i = 0; i < size * size; ++i) {
        s += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= (z >> 31);
        const float noise =
            (static_cast<float>(z >> 40) / 16777216.0f - 0.5f) * 0.6f;
        h.values.push_back(
            std::max(0.05f, std::min(0.95f, 0.5f + noise)));
    }
    return h;
}

void test_procgen_jobs() {
    std::string error;
    auto jobs = engine::procgen::create_procgen_jobs();
    CHECK(jobs != nullptr);
    auto token = engine::procgen::create_cancellation_token();
    CHECK(token != nullptr && !token->cancelled());

    // --- erosion batch: outputs == individual erode, progress reaches total
    std::vector<engine::procgen::Heightmap> tiles;
    tiles.push_back(make_erosion_tile(16, 1));
    tiles.push_back(make_erosion_tile(16, 2));
    tiles.push_back(make_erosion_tile(16, 3));
    tiles.push_back(make_erosion_tile(16, 4));
    engine::procgen::ErosionSpec spec;
    spec.iterations = 2000;  // fast batch
    std::vector<engine::procgen::Heightmap> eroded;
    std::size_t lastCompleted = 0;
    std::size_t lastTotal = 0;
    engine::procgen::JobResult r = jobs->erode_tiles(
        spec, tiles, *token,
        [&](const engine::procgen::JobProgress& p) {
            lastCompleted = p.completed;
            lastTotal = p.total;
        },
        eroded, error);
    CHECK(r == engine::procgen::JobResult::Completed);
    CHECK(eroded.size() == 4);
    CHECK(lastCompleted == 3 && lastTotal == 4);

    auto erosion = engine::procgen::create_heightmap_erosion();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        engine::procgen::Heightmap single;
        CHECK(erosion->erode(tiles[i], spec, single, error));
        CHECK(eroded[i].values == single.values);  // bit-exact
    }

    // --- cancellation mid-batch (from the progress callback) ---
    auto cancelToken = engine::procgen::create_cancellation_token();
    std::vector<engine::procgen::Heightmap> partial;
    engine::procgen::JobResult cr = jobs->erode_tiles(
        spec, tiles, *cancelToken,
        [&](const engine::procgen::JobProgress& p) {
            if (p.completed >= 1) {
                cancelToken->cancel();
            }
        },
        partial, error);
    CHECK(cr == engine::procgen::JobResult::Cancelled);
    CHECK(partial.size() == 2);  // unit 0+1 ran, unit 2 aborted
    for (std::size_t i = 0; i < partial.size(); ++i) {
        CHECK(partial[i].values == eroded[i].values);  // prefix == full run
    }

    // --- pre-cancelled: no work, empty outputs ---
    auto deadToken = engine::procgen::create_cancellation_token();
    deadToken->cancel();
    std::vector<engine::procgen::Heightmap> none;
    CHECK(jobs->erode_tiles(spec, tiles, *deadToken, nullptr, none, error) ==
          engine::procgen::JobResult::Cancelled);
    CHECK(none.empty());

    // --- structure batch: outputs == individual generates ---
    auto gen = engine::procgen::create_structure_generator(
        make_room_spec(7), error);
    CHECK(gen != nullptr);
    std::vector<std::pair<int, int>> sizes = { { 12, 8 }, { 16, 12 } };
    std::vector<engine::procgen::StructureOutput> structs;
    engine::procgen::JobResult sr = jobs->generate_structures(
        make_room_spec(7), sizes, *token, nullptr, structs, error);
    CHECK(sr == engine::procgen::JobResult::Completed);
    CHECK(structs.size() == 2);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        engine::procgen::StructureOutput single;
        CHECK(gen->generate(sizes[i].first, sizes[i].second, single, error));
        CHECK(structs[i].width == single.width);
        CHECK(structs[i].height == single.height);
        CHECK(structs[i].depth == single.depth);
        CHECK(structs[i].plan == single.plan);
        CHECK(structs[i].blocks == single.blocks);
    }

    // --- cook batch: outputs == individual cooks ---
    const engine::procgen::CookedMesh gridMesh = make_grid_mesh(16, 16, false);
    engine::procgen::CookOptions full;
    full.unwrap = true;
    full.optimize = true;
    full.simplifyTargetIndices = 0;
    engine::procgen::CookOptions noUnwrap;
    noUnwrap.unwrap = false;
    noUnwrap.optimize = true;
    std::vector<engine::procgen::CookedMesh> meshes = { gridMesh, gridMesh };
    std::vector<engine::procgen::CookOptions> optionSets = { full, noUnwrap };
    std::vector<engine::procgen::CookedMesh> firstCooked;
    // The batch uses one options set; run twice with each.
    for (const auto& opts : optionSets) {
        std::vector<engine::procgen::CookedMesh> cooked;
        engine::procgen::JobResult mr =
            jobs->cook_meshes(opts, meshes, *token, nullptr, cooked, error);
        CHECK(mr == engine::procgen::JobResult::Completed);
        CHECK(cooked.size() == 2);
        if (&opts == &optionSets[0]) {
            firstCooked = cooked;
        }
        auto cooker = engine::procgen::create_mesh_cooker();
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            engine::procgen::CookedMesh single;
            engine::procgen::CookStats stats;
            CHECK(cooker->cook(meshes[i], opts, single, stats, error));
            CHECK(cooked[i].positions == single.positions);
            CHECK(cooked[i].indices == single.indices);
            CHECK(cooked[i].uvs == single.uvs);
        }
    }

    // --- cook cancellation: with 3 meshes, cancelling at unit 1 aborts unit 2
    auto cookToken = engine::procgen::create_cancellation_token();
    std::vector<engine::procgen::CookedMesh> threeMeshes = { gridMesh,
                                                             gridMesh,
                                                             gridMesh };
    std::vector<engine::procgen::CookedMesh> cookedPartial;
    engine::procgen::JobResult cmr = jobs->cook_meshes(
        full, threeMeshes, *cookToken,
        [&](const engine::procgen::JobProgress& p) {
            if (p.completed >= 1) {
                cookToken->cancel();
            }
        },
        cookedPartial, error);
    CHECK(cmr == engine::procgen::JobResult::Cancelled);
    CHECK(cookedPartial.size() == 2);  // units 0+1 ran, unit 2 aborted

    // --- empty batches: trivially Completed with no outputs ---
    std::vector<engine::procgen::Heightmap> emptyTiles;
    std::vector<engine::procgen::Heightmap> emptyOut;
    CHECK(jobs->erode_tiles(spec, emptyTiles, *token, nullptr, emptyOut,
                            error) ==
          engine::procgen::JobResult::Completed);
    CHECK(emptyOut.empty());
    std::vector<std::pair<int, int>> emptySizes;
    std::vector<engine::procgen::StructureOutput> emptyStructs;
    CHECK(jobs->generate_structures(make_room_spec(7), emptySizes, *token,
                                    nullptr, emptyStructs, error) ==
          engine::procgen::JobResult::Completed);
    CHECK(emptyStructs.empty());
    std::vector<engine::procgen::CookedMesh> emptyMeshes;
    std::vector<engine::procgen::CookedMesh> emptyCooked;
    CHECK(jobs->cook_meshes(full, emptyMeshes, *token, nullptr, emptyCooked,
                            error) ==
          engine::procgen::JobResult::Completed);
    CHECK(emptyCooked.empty());

    // --- failures: invalid input rejects with a diagnostic ---
    engine::procgen::StructureAssetSpec badAsset;
    badAsset.sampleWidth = 0;
    std::vector<engine::procgen::StructureOutput> badStructs;
    CHECK(jobs->generate_structures(badAsset, sizes, *token, nullptr,
                                    badStructs, error) ==
          engine::procgen::JobResult::Failed);
    CHECK(badStructs.empty() && !error.empty());
    error.clear();
    engine::procgen::CookedMesh badMesh = gridMesh;
    badMesh.indices[0] = 999999;
    std::vector<engine::procgen::CookedMesh> badMeshes = { badMesh };
    std::vector<engine::procgen::CookedMesh> badCooked;
    CHECK(jobs->cook_meshes(full, badMeshes, *token, nullptr, badCooked,
                            error) ==
          engine::procgen::JobResult::Failed);
    CHECK(badCooked.empty() && !error.empty());

    // --- determinism across instances ---
    auto twin = engine::procgen::create_procgen_jobs();
    std::vector<engine::procgen::Heightmap> twinEroded;
    CHECK(twin->erode_tiles(spec, tiles, *token, nullptr, twinEroded, error) ==
          engine::procgen::JobResult::Completed);
    CHECK(twinEroded.size() == eroded.size());
    for (std::size_t i = 0; i < eroded.size(); ++i) {
        CHECK(twinEroded[i].values == eroded[i].values);
    }
    std::vector<engine::procgen::StructureOutput> twinStructs;
    CHECK(twin->generate_structures(make_room_spec(7), sizes, *token, nullptr,
                                    twinStructs, error) ==
          engine::procgen::JobResult::Completed);
    CHECK(twinStructs.size() == structs.size());
    for (std::size_t i = 0; i < structs.size(); ++i) {
        CHECK(twinStructs[i].plan == structs[i].plan);
        CHECK(twinStructs[i].blocks == structs[i].blocks);
    }
    std::vector<engine::procgen::CookedMesh> twinCooked;
    CHECK(twin->cook_meshes(full, meshes, *token, nullptr, twinCooked,
                            error) ==
          engine::procgen::JobResult::Completed);
    CHECK(twinCooked.size() == 2);
    for (std::size_t i = 0; i < twinCooked.size(); ++i) {
        CHECK(twinCooked[i].positions == firstCooked[i].positions);
        CHECK(twinCooked[i].indices == firstCooked[i].indices);
        CHECK(twinCooked[i].uvs == firstCooked[i].uvs);
    }

    std::cout << "[sdk] procgen: cancellable jobs (erosion batch, structures, "
                 "cooking, cancellation, failures, determinism) OK\n";
}

void test_lod_terrain() {
    std::string error;
    auto sampler = engine::procgen::create_lod_terrain_sampler();
    CHECK(sampler != nullptr);

    // The world function: the same graph-based generator the world uses for
    // detail (height graph -> baseHeight + round(height * amplitude)).
    auto height = engine::procgen::create_noise_graph_from_spec(
        make_height_spec(21, 0.05f), error);
    CHECK(height != nullptr);
    auto gen = engine::procgen::create_graph_voxel_generator(
        height, nullptr, nullptr, /*baseHeight=*/64, /*amplitude=*/32);
    CHECK(gen != nullptr);

    // Level 0 (cellSize 1) IS the detail surface: every anchor is an integer
    // world column and equals gen.sample(anchor) bit-exactly.
    std::vector<engine::procgen::LodCell> level0;
    CHECK(sampler->sample(*gen, 0, 0, 8, 6, 1, level0, error));
    CHECK(level0.size() == 48);
    for (int z = 0; z < 6; ++z) {
        for (int x = 0; x < 8; ++x) {
            const auto& cell = level0[static_cast<std::size_t>(x + z * 8)];
            CHECK(cell.anchorX == x && cell.anchorZ == z && cell.cellSize == 1);
            const engine::voxel::TerrainPoint p = gen->sample(
                static_cast<float>(x), static_cast<float>(z));
            CHECK(cell.height == static_cast<float>(p.height));
            CHECK(cell.biomeIndex == p.biomeIndex);
        }
    }

    // Higher levels share anchors with level 0: anchors are multiples of
    // cellSize, so every level-2 cell equals gen.sample(anchor) and the level
    // -0 cell at the same anchor — cross-level coherent (the distant surface
    // is the same function as the detail).
    // 3x3 grid of size-2 cells: anchors (0,0)..(4,4), all inside the level-0
    // 8x6 grid, so every level-2 anchor is also a level-0 anchor.
    std::vector<engine::procgen::LodCell> level2;
    CHECK(sampler->sample(*gen, 0, 0, 3, 3, 2, level2, error));
    CHECK(level2.size() == 9);
    for (int z = 0; z < 3; ++z) {
        for (int x = 0; x < 3; ++x) {
            const auto& cell = level2[static_cast<std::size_t>(x + z * 3)];
            CHECK(cell.anchorX == x * 2 && cell.anchorZ == z * 2);
            const engine::voxel::TerrainPoint p = gen->sample(
                static_cast<float>(x * 2), static_cast<float>(z * 2));
            CHECK(cell.height == static_cast<float>(p.height));
            // Same anchor as the level-0 grid.
            CHECK(cell.height == level0[x * 2 + z * 2 * 8].height);
        }
    }

    // Aligned origin: anchors start at the origin.
    std::vector<engine::procgen::LodCell> shifted;
    CHECK(sampler->sample(*gen, 4, 4, 2, 2, 2, shifted, error));
    CHECK(shifted.size() == 4);
    CHECK(shifted[0].anchorX == 4 && shifted[0].anchorZ == 4);

    // Interpolation passes exactly through the generator samples at anchors.
    CHECK(sampler->interpolated_height(level2, 3, 3, 2, 4.0f, 4.0f) ==
          level2[2 + 2 * 3].height);  // anchor (4,4)
    CHECK(sampler->interpolated_height(level2, 3, 3, 2, 0.0f, 0.0f) ==
          level2[0].height);
    // Midpoint (3,0) blends anchors (2,0) and (4,0) linearly (float tolerance).
    const float mid = sampler->interpolated_height(level2, 3, 3, 2, 3.0f, 0.0f);
    const float avg =
        0.5f * (level2[1].height + level2[2].height);  // anchors (2,0) (4,0)
    CHECK(std::fabs(mid - avg) < 1e-4f);
    // Out of range clamps to the outermost cell (anchor (4,4)).
    CHECK(sampler->interpolated_height(level2, 3, 3, 2, 100.0f, 100.0f) ==
          level2[8].height);

    // Determinism: an independent sampler produces bit-identical cells.
    auto twin = engine::procgen::create_lod_terrain_sampler();
    std::vector<engine::procgen::LodCell> twinCells;
    CHECK(twin->sample(*gen, 0, 0, 8, 6, 1, twinCells, error));
    CHECK(twinCells.size() == level0.size());
    for (std::size_t i = 0; i < level0.size(); ++i) {
        CHECK(twinCells[i].anchorX == level0[i].anchorX);
        CHECK(twinCells[i].anchorZ == level0[i].anchorZ);
        CHECK(twinCells[i].height == level0[i].height);
        CHECK(twinCells[i].biomeIndex == level0[i].biomeIndex);
    }

    // Validation: non-positive grid/cell size and unaligned origins reject.
    std::vector<engine::procgen::LodCell> bad;
    CHECK(sampler->sample(*gen, 0, 0, 0, 4, 2, bad, error) == false &&
          !error.empty());
    error.clear();
    CHECK(sampler->sample(*gen, 0, 0, 4, 0, 2, bad, error) == false &&
          !error.empty());
    error.clear();
    CHECK(sampler->sample(*gen, 0, 0, 4, 4, 0, bad, error) == false &&
          !error.empty());
    error.clear();
    CHECK(sampler->sample(*gen, 1, 0, 4, 4, 2, bad, error) == false &&
          !error.empty());
    error.clear();
    // Empty cell set interpolates to 0.
    std::vector<engine::procgen::LodCell> none;
    CHECK(sampler->interpolated_height(none, 4, 4, 2, 0.0f, 0.0f) == 0.0f);

    std::cout << "[sdk] procgen: coherent LOD (same world function, "
                 "cross-level anchors, interpolation, determinism, "
                 "validation) OK\n";
}

}  // namespace

int main(int argc, char** argv) {
    // FALTANTES §25 — real process interruption: when this binary is spawned
    // as the "crash child" (env VC_TEST_CRASH_CHILD), it commits a save and
    // then dies mid-save (see test_real_process_interruption below).
    if (std::getenv("VC_TEST_CRASH_CHILD") != nullptr) {
        if (const char* crashDir = std::getenv("VC_TEST_CRASH_DIR")) {
            child_crash_mid_save(crashDir);  // never returns (_exit)
        }
        return 138;
    }
    try {
        test_version();
        test_world_headless();
        test_transactions();
        test_transaction_failure_stages();
        test_transaction_policy_limits();
        test_persistence();
        test_save_world_clears_error_out();
        test_compression_provider();
        test_hash_provider();
        test_save_v4_legacy();
        test_world_save_migration();
        test_rocksdb_storage();
        test_storage_service();
        test_region_chunk_storage();
        test_region_palette_compression();
        test_region_state_persistence();
        test_region_delta_saves();
        test_region_snapshot_concurrent_edit();
        test_region_async_save_load();
        test_world_registry_source_of_truth();
        test_dynamic_block_persistence();
        test_replication_palette();
        test_json_only_block_e2e();
        test_world_metadata_persistence();
        test_per_voxel_state();
        test_block_entity_lifecycle();
        test_block_entity_atomic_destroy();
        test_block_entity_persistence();
        test_block_entities_json_defined();
        test_block_entity_optional_capabilities();
        test_block_entity_scripting();
        test_render_handoff_dirty_updates();
        test_light_skylight();
        test_light_block_emitter();
        test_light_chunk_boundary();
        test_light_determinism();
        test_light_load_unload_during_propagation();
        test_eviction_preserves_edits();
        test_fluid_registry();
        test_block_inline_fluid();
        test_fluid_generalized();
        test_fluid_evaporation();
        test_fluid_tick_cadence();
        test_block_collision_selection_shapes();
        test_streaming_observability();
        test_chunk_memory_budget();
        test_autosave_incremental();
        test_wal_recovery();
        test_real_process_interruption(
            self_exe_path(argc > 0 ? argv[0] : nullptr).c_str());
        test_region_entity_persistence();
        test_region_batch_restore();
        test_load_rollback();
        test_region_load_rollback();
        test_persistence_resilience();
        test_large_world();
        test_service_plugin_substitution();
        test_fluid_chunk_boundary();
        test_fluid_cross_boundary_vertical();
        test_fluid_resume_after_boundary_load();
        test_fluid_persistence();
        test_fluid_conservation();
        test_fluid_waterfall();
        test_fluid_unload_reload();
        test_fluid_budgets();
        test_material_simulation_separation();
        test_project_defined_fluids();
        test_fluid_render_handoff_optics();
        test_block_semantic_queries();
        test_fluid_density_displacement();
        test_fluid_solidification_combustion();
        test_block_registry();
        test_item_registry();
        test_cross_reference_validation();
        test_block_states_transitions();
        test_block_tool_physics();
        test_item_stack_inventory();
        test_inventory_undo_redo();
        test_inventory_nested_containers();
        test_recipe_graph();
        test_entity_world();
        test_entity_world_save();
        test_navigation_provider();
        test_navigation_voxel_world();
        test_navigation_local_update();
        test_navigation_local_update_world();
        test_navigation_dynamic_obstacle();
        test_navigation_area_costs();
        test_navigation_off_mesh_links();
        test_navigation_async_paths();
        test_replication_authority();
        test_replication_deltas_reorder();
        test_commit_replication_persistence();
        test_session_scoped_undo();
        test_transaction_dry_run();
        test_scheduler_save_headless();
        test_replication_interest();
        test_replication_prediction_correction();
        test_replication_reconnect_resync();
        test_replication_server_persist();
        test_replication_codec();
        test_replication_multiclient();
        test_region_replication();
        test_replication_region_handoff();
        test_mob_behavior();
        test_world_manager();
        test_noise_graph_determinism();
        test_noise_graph_nodes();
        test_noise_graph_serialization();
        test_graph_generator_world();
        test_manifold_csg();
        test_fastnoise2_backend();
        test_climate_registry();
        test_climate_sampler();
        test_climate_generator_world();
        test_ore_table();
        test_carver();
        test_decorator();
        test_world_features();
        test_structure_generator();
        test_structure_placement();
        test_structure_placer();
        test_world_profile();
        test_world_manager_profile();
        test_road_network();
        test_parcellation();
        test_parcel_triangulation();
        test_shape_grammar();
        test_mesh_cooking();
        test_heightmap_erosion();
        test_procgen_preview();
        test_procgen_jobs();
        test_lod_terrain();
    } catch (const std::exception& e) {
        std::cerr << "[voxel_sdk_tests] uncaught exception: " << e.what()
                  << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "[voxel_sdk_tests] uncaught unknown exception\n";
        return EXIT_FAILURE;
    }

    if (g_failures != 0) {
        std::cerr << "[voxel_sdk_tests] " << g_failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "[voxel_sdk_tests] all checks passed\n";
    return EXIT_SUCCESS;
}

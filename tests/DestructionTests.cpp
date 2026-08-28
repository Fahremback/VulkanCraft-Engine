// DestructionTests.cpp
//
// Evidence for FALTANTES §16 item 14: budgets + telemetry for large-scale
// destruction. ONE data-driven budget (DestructionBudget) bounds every
// pipeline stage per step and telemetry (DestructionTelemetry) makes the
// whole destruction step observable — done work AND budget spills:
//   - validation: out-of-range caps are REFUSED (never clamped), mirroring
//     the other gameplay configs;
//   - apply_to: the budget maps into the leaf configs that already carry caps
//     (ExplosionConfig, RevoxelizePolicy, DestructionHistoryConfig) — one
//     source of truth, no drift;
//   - telemetry: step counters reset per step while cumulative totals persist;
//     spills flag exceeded() per stage; deterministic across instances;
//   - explosion caps (the entry point of large-scale destruction had NO cap):
//     a huge blast with maxCarvedCells/maxBurnedCells/maxImpulsedBodies stops
//     the stage at the cap and counts the skipped candidates; 0 = unlimited
//     keeps the pre-budget behavior (parity);
//   - pipeline integration: the budget gates connectivity island promotion
//     and debris spawn bursts (the two stages with no leaf config), caps
//     revoxelization per pass and history restore regions, and the telemetry
//     report mirrors the leaf results 1:1;
//   - determinism: identical capped scenarios produce identical counters.

#include <engine/registry/BlockRegistry.hpp>
#include <engine/voxel/IVoxelWorld.hpp>

#include "engine/gameplay/DebrisRuntime.hpp"
#include "engine/gameplay/DestructionBudget.hpp"
#include "engine/gameplay/DestructionHistoryRuntime.hpp"
#include "engine/gameplay/DestructionTelemetry.hpp"
#include "engine/gameplay/ExplosionRuntime.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/PhysicsStreamingBridge.hpp"
#include "engine/physics/VoxelConnectivity.hpp"

#include <glm/glm.hpp>

#include <chrono>
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
                int budget, int maxSteps = 60 * 180, int maxBudgetMs = 30000) {
    world.set_chunk_budget(budget);
    const auto start = std::chrono::steady_clock::now();
    for (int step = 0; step < maxSteps; ++step) {
        if (world.is_chunk_loaded(0, 0)) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxBudgetMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return world.is_chunk_loaded(0, 0);
}

constexpr int kGroundTop = 130;

struct TestWorld {
    std::shared_ptr<engine::registry::BlockRegistry> registry;
    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    std::uint32_t woodId{ 0 };
    bool ok{ false };
};

TestWorld make_test_world() {
    TestWorld out;
    std::string error;
    out.registry = std::make_shared<engine::registry::BlockRegistry>();
    out.ok = out.registry->load_from_json(
        R"({"name":"wood","namespace":"test","resistance":0.0,"flammability":1.0,"density":0.6})", error);
    out.world = engine::voxel::create_default_voxel_world();
    out.world->set_block_registry(out.registry);
    out.world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    out.ok = out.ok && boot_world(*out.world, glm::vec3(8.0f, 200.0f, 8.0f), 2) &&
             out.world->resolve_block_id("test:wood", out.woodId, error);
    return out;
}

template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, Engine::Gameplay::DebrisRuntime& debris,
            const glm::vec3& focus, Pred predicate, int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 300;
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) {
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

BodyDesc debris_desc(const glm::vec3& position, float mass) {
    BodyDesc desc;
    desc.motion = MotionType::Dynamic;
    desc.position = position;
    desc.mass = mass;
    desc.collider.shape = BoxShape{ glm::vec3(0.5f) };
    return desc;
}

// 1. Validation: out-of-range caps are REFUSED with a diagnostic (never
//    clamped), mirroring the other gameplay configs.
void test_budget_validation() {
    Engine::Gameplay::DestructionBudget budget;
    std::string error;
    check(!budget.load_from_json(R"({"maxCarvedCells":2000000})", error),
          "maxCarvedCells above the ceiling refused");
    check(!budget.load_from_json(R"({"maxBurnedCells":-1})", error),
          "negative maxBurnedCells refused");
    check(!budget.load_from_json(R"({"maxRevoxelizedDebris":0})", error),
          "maxRevoxelizedDebris 0 refused (would bypass the leaf bound)");
    check(!budget.load_from_json(R"({"maxRestoredCells":0})", error),
          "maxRestoredCells 0 refused");
    check(!budget.load_from_json(R"({"maxDetachedIslands":5000})", error),
          "maxDetachedIslands above the ceiling refused");
    Engine::Gameplay::DestructionBudget valid;
    check(valid.load_from_json(
              R"({"enabled":true,"maxCarvedCells":64,"maxBurnedCells":16,)"
              R"("maxImpulsedBodies":8,"maxDetachedIslands":4,"maxSpawnedDebris":2,)"
              R"("maxRevoxelizedDebris":3,"maxRevoxelizedBlocks":7,"maxRestoredCells":128})", error) &&
              valid.maxCarvedCells == 64 && valid.maxBurnedCells == 16 &&
              valid.maxImpulsedBodies == 8 && valid.maxDetachedIslands == 4 &&
              valid.maxSpawnedDebris == 2 && valid.maxRevoxelizedDebris == 3 &&
              valid.maxRevoxelizedBlocks == 7 && valid.maxRestoredCells == 128,
          "valid budget loads and round-trips every cap");
    Engine::Gameplay::DestructionBudget disabled;
    check(disabled.load_from_json(R"({"enabled":false})", error) && !disabled.enabled,
          "enabled=false accepted (budget disabled)");
    std::printf("[budget] validation OK\n");
}

// 2. apply_to: the budget maps into the leaf configs that already carry caps —
//    ONE source of truth for the whole pipeline, no drift.
void test_budget_applies_to_leaves() {
    Engine::Gameplay::DestructionBudget budget;
    budget.maxCarvedCells = 1000;
    budget.maxBurnedCells = 500;
    budget.maxImpulsedBodies = 64;
    budget.maxRevoxelizedDebris = 8;
    budget.maxRevoxelizedBlocks = 16;
    budget.maxRestoredCells = 2048;

    Engine::Gameplay::ExplosionConfig explosion;
    Engine::Gameplay::RevoxelizePolicy revox;
    Engine::Gameplay::DestructionHistoryConfig history;
    budget.apply_to(explosion);
    budget.apply_to(revox);
    budget.apply_to(history);
    check(explosion.maxCarvedCells == 1000 && explosion.maxBurnedCells == 500 &&
              explosion.maxImpulsedBodies == 64,
          "budget flows into the explosion config caps");
    check(revox.maxDebrisPerPass == 8 && revox.maxBlocksPerDebris == 16,
          "budget flows into the revoxelize policy caps");
    check(history.maxRegionCells == 2048,
          "budget flows into the history restore cap");

    // Disabled budget: leaves keep their own config (no-ops).
    Engine::Gameplay::DestructionBudget off;
    off.enabled = false;
    Engine::Gameplay::ExplosionConfig untouched;
    Engine::Gameplay::RevoxelizePolicy untouchedPolicy;
    Engine::Gameplay::DestructionHistoryConfig untouchedHistory;
    off.apply_to(untouched);
    off.apply_to(untouchedPolicy);
    off.apply_to(untouchedHistory);
    check(untouched.maxCarvedCells == 0 && untouched.maxBurnedCells == 0 &&
              untouchedPolicy.maxDebrisPerPass == 16 &&
              untouchedHistory.maxRegionCells == 4096,
          "disabled budget leaves the leaf configs untouched");
    std::printf("[budget] apply_to leaves OK\n");
}

// 3. Telemetry: step counters reset per step while cumulative totals persist;
//    spills flag exceeded() per stage; deterministic across instances.
void test_telemetry_basics() {
    Engine::Gameplay::DestructionTelemetry telemetry;
    telemetry.note_carved(12);
    telemetry.note_burned(3);
    telemetry.note_islands(2);
    telemetry.note_spawned(2);
    telemetry.note_revoxelized(1);
    telemetry.note_blocks(18);
    telemetry.note_restored(45);
    telemetry.note_written(45);
    check(telemetry.step().carvedCells == 12 && telemetry.step().burnedCells == 3 &&
              telemetry.step().detachedIslands == 2 && telemetry.step().spawnedDebris == 2 &&
              telemetry.step().revoxelizedDebris == 1 && telemetry.step().revoxelizedBlocks == 18 &&
              telemetry.step().restoredCells == 45 && telemetry.step().cellsWritten == 45,
          "step counters mirror the notes");

    telemetry.begin_step();
    check(telemetry.step().carvedCells == 0 && telemetry.step().spawnedDebris == 0,
          "begin_step resets the step counters");
    check(telemetry.cumulative().carvedCells == 12 &&
              telemetry.cumulative().revoxelizedBlocks == 18,
          "cumulative totals persist across steps");

    telemetry.note_spill(Engine::Gameplay::DestructionStage::Islands, 3);
    telemetry.note_spill(Engine::Gameplay::DestructionStage::Carve, 9);
    check(telemetry.step().spilledIslands == 3 && telemetry.step().spilledCarved == 9,
          "spills recorded in the step counters");
    check(telemetry.cumulative().spilledIslands == 3,
          "spills accumulate cumulatively");
    check(telemetry.exceeded(Engine::Gameplay::DestructionStage::Islands) &&
              telemetry.exceeded(Engine::Gameplay::DestructionStage::Carve) &&
              !telemetry.exceeded(Engine::Gameplay::DestructionStage::Burn) &&
              !telemetry.exceeded(Engine::Gameplay::DestructionStage::Restore),
          "exceeded() flags exactly the spilled stages");

    // Determinism: identical note sequences -> identical reports.
    const auto run = []() {
        Engine::Gameplay::DestructionTelemetry t;
        t.note_carved(4);
        t.note_burned(1);
        t.note_spill(Engine::Gameplay::DestructionStage::Spawn, 2);
        t.note_restored(9);
        return t.step();
    };
    const Engine::Gameplay::DestructionCounters a = run();
    const Engine::Gameplay::DestructionCounters b = run();
    check(a.carvedCells == b.carvedCells && a.spilledDebris == b.spilledDebris &&
              a.restoredCells == b.restoredCells,
          "identical note sequences -> identical step reports");
    std::printf("[telemetry] step/cumulative/spills/determinism OK\n");
}

// 4. Explosion caps (the entry point of large-scale destruction): the stage
//    stops at its per-call cap and counts the skipped candidates; 0 stays
//    unlimited (parity with the pre-budget behavior).
void test_explosion_budget_caps() {
    // Carve cap: a big blast over the flat stone ground has thousands of
    // candidates; maxCarvedCells=16 stops the carve at exactly 16.
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 6.0f;
        config.maxPressure = 40.0f;
        config.heatRadius = 0.0f;
        config.maxCarvedCells = 16;
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        check(result.blocksRemoved == 16,
              "carve cap stops the blast at exactly maxCarvedCells");
        check(result.carvedSkipped > 0,
              "the remaining carve candidates are counted as skipped (spill)");
    }
    // Burn cap: 6 flammable wood cells in the heat ring, maxBurnedCells=2.
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        t.world->set_block(8, kGroundTop, 5, t.woodId);
        t.world->set_block(8, kGroundTop, 11, t.woodId);
        t.world->set_block(5, kGroundTop, 8, t.woodId);
        t.world->set_block(11, kGroundTop, 8, t.woodId);
        t.world->set_block(6, kGroundTop, 6, t.woodId);
        t.world->set_block(10, kGroundTop, 10, t.woodId);
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 1.0f;      // carve zone d<1 (tiny)
        config.heatRadius = 5.0f;       // burn zone d in [1,5) (the wood)
        config.heatDamage = 1.0f;
        config.ignitionThreshold = 1.0f;
        config.maxPressure = 40.0f;
        config.maxBurnedCells = 2;
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        check(result.blocksIgnited == 2,
              "burn cap stops the fireball at exactly maxBurnedCells");
        check(result.burnedSkipped == 4,
              "the remaining flammable cells are counted as skipped");
    }
    // Impulse cap: 6 dynamic bodies in the blast sphere, maxImpulsedBodies=2.
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        PhysicsStreamingBridge bridge(*t.world, physics);
        bridge.sync(glm::vec3(8.0f, 200.0f, 8.0f));
        const glm::vec3 spots[6] = {
            { 8.0f, 133.0f, 8.0f }, { 9.0f, 133.0f, 8.0f }, { 8.0f, 133.0f, 9.0f },
            { 9.0f, 133.0f, 9.0f }, { 7.0f, 133.0f, 7.0f }, { 7.0f, 133.0f, 9.0f },
        };
        for (const glm::vec3& spot : spots) bridge.spawn_dynamic(debris_desc(spot, 2.0f));
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 4.0f;
        config.maxPressure = 40.0f;
        config.heatRadius = 0.0f;
        config.impulseScale = 2.0f;
        config.maxImpulsedBodies = 2;
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        check(result.bodiesImpulsed == 2,
              "impulse cap stops the wave at exactly maxImpulsedBodies");
        check(result.impulseSkipped == 4,
              "the remaining dynamic bodies are counted as skipped");
    }
    // Unlimited (0) parity: no caps -> the full blast carves (pre-budget
    // behavior preserved).
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 6.0f;
        config.maxPressure = 40.0f;
        config.heatRadius = 0.0f;
        config.maxCarvedCells = 0;  // unlimited
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        check(result.blocksRemoved > 16 && result.carvedSkipped == 0,
              "cap 0 = unlimited: full carve, zero spills");
    }
    std::printf("[budget] explosion caps (carve/burn/impulse, unlimited parity) OK\n");
}

// 5. Pipeline integration: the budget gates the two stages with no leaf
//    config (islands promotion, debris spawn bursts), maps into the leaf caps
//    (revoxelization, history restore), and the telemetry report mirrors the
//    leaf results 1:1.
void test_pipeline_budget_integration() {
    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };

    // 5a. Islands gate: two pillars carved -> two detached islands; the budget
    //     promotes only maxDetachedIslands (spill for the rest).
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        // Two identical pillar+platform structures INSIDE the loaded chunk
        // (0,0) — a structure at x=20 would sit in chunk (1,1), which the
        // budget-2 boot never loads (findings #65: ring r<=1 only).
        const int pillars[2][2] = { { 4, 4 }, { 12, 12 } };
        for (int i = 0; i < 2; ++i) {
            const int px = pillars[i][0], pz = pillars[i][1];
            for (int y = kGroundTop + 1; y <= 140; ++y) t.world->set_block(px, y, pz, 3);
            for (int x = px - 1; x <= px + 1; ++x)
                for (int z = pz - 1; z <= pz + 1; ++z) t.world->set_block(x, 141, z, 3);
        }
        // Carve both pillars (manual carve — the pipeline source is generic).
        for (int i = 0; i < 2; ++i) {
            const int px = pillars[i][0], pz = pillars[i][1];
            for (int y = kGroundTop + 1; y <= 140; ++y) t.world->set_block(px, y, pz, 0);
        }
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        VoxelConnectivity connectivity;
        ConnectivitySettings settings;
        for (int i = 0; i < 2; ++i) {
            const int px = pillars[i][0], pz = pillars[i][1];
            connectivity.note_edit(glm::ivec3(px, kGroundTop + 1, pz),
                                   glm::ivec3(px, 140, pz));
            connectivity.note_edit(glm::ivec3(px - 1, 141, pz - 1),
                                   glm::ivec3(px + 1, 141, pz + 1));
        }
        const auto isSolid = [](std::uint32_t id) { return id != 0u; };
        const std::vector<VoxelIsland> islands =
            connectivity.sync(*t.world, settings, isSolid);
        check(islands.size() == 2, "connectivity found both detached platforms");

        Engine::Gameplay::DestructionBudget budget;
        budget.maxDetachedIslands = 1;
        Engine::Gameplay::DestructionTelemetry telemetry;
        std::size_t promoted = 0;
        const std::size_t limit = budget.max_islands();
        for (const VoxelIsland& island : islands) {
            if (promoted >= limit) {
                telemetry.note_spill(Engine::Gameplay::DestructionStage::Islands,
                                     islands.size() - promoted);
                break;
            }
            telemetry.note_islands(1);
            ++promoted;
            (void)island;  // spawn bounded (not needed for the gate proof)
        }
        check(promoted == 1 && telemetry.step().detachedIslands == 1,
              "island promotion bounded by maxDetachedIslands");
        check(telemetry.step().spilledIslands == 1 &&
                  telemetry.exceeded(Engine::Gameplay::DestructionStage::Islands),
              "the un-promoted island is recorded as a spill");
    }

    // 5b. Revoxelize cap: budget.apply_to(policy) -> maxDebrisPerPass=1; two
    //     sleeping debris -> only one revoxelized per pass.
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        PhysicsStreamingBridge bridge(*t.world, physics);
        bridge.sync(focus);
        Engine::Gameplay::DebrisRuntime debris(physics, {});
        check(debris.spawn(debris_desc(glm::vec3(6.0f, 140.0f, 6.0f), 3.0f), 3u) != InvalidBody,
              "debris A spawned");
        check(debris.spawn(debris_desc(glm::vec3(10.0f, 140.0f, 10.0f), 3.0f), 3u) != InvalidBody,
              "debris B spawned");
        settle(*t.world, physics, bridge, debris, focus, [&]() {
            const auto events = debris.drain_replication_events();
            std::size_t settled = 0;
            for (const auto& e : events)
                if (e.type == Engine::Gameplay::DebrisReplicationEvent::Type::Settled) ++settled;
            return settled >= 2;
        });

        Engine::Gameplay::DestructionBudget budget;
        budget.maxRevoxelizedDebris = 1;
        Engine::Gameplay::RevoxelizePolicy policy;
        policy.settleDelay = 0.0f;  // eligible as soon as resting
        budget.apply_to(policy);
        check(policy.maxDebrisPerPass == 1, "budget capped the revoxelize pass");
        const auto blockOf = [](std::uint32_t material) { return material; };
        const Engine::Gameplay::RevoxelizeResult result =
            debris.revoxelize_sleeping(*t.world, policy, blockOf);
        check(result.debrisRevoxelized == 1,
              "revoxelization bounded by maxRevoxelizedDebris per pass");
        check(debris.active_count() == 1,
              "the second sleeping debris stays active (next pass)");
    }

    // 5c. History restore cap: budget.apply_to(history) -> maxRegionCells=8;
    //     a region over the cap is refused.
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::DebrisRuntime debris(physics, {});
        Engine::Gameplay::DestructionBudget budget;
        budget.maxRestoredCells = 8;
        Engine::Gameplay::DestructionHistoryConfig config;
        budget.apply_to(config);
        check(config.maxRegionCells == 8, "budget capped the restore region");
        Engine::Gameplay::DestructionHistoryRuntime history(physics, debris, config);
        Engine::Gameplay::DestructionSnapshot snapshot;
        std::string error;
        check(!history.capture(*t.world, glm::ivec3(3, 3, 3), glm::ivec3(5, 5, 5),
                               snapshot, error),
              "capture region over the budget cap refused (never clamped)");
        Engine::Gameplay::DestructionHistoryRuntime unrestricted(physics, debris, {});
        check(unrestricted.capture(*t.world, glm::ivec3(3, 3, 3), glm::ivec3(5, 5, 5),
                                   snapshot, error),
              "the same region fits the default (uncapped) config");
    }

    // 5d. Debris spawn gate: the pipeline's spawn burst is bounded by
    //     max_debris() (the stage has no leaf config — the caller gates).
    {
        Engine::Gameplay::DestructionBudget budget;
        budget.maxSpawnedDebris = 2;
        Engine::Gameplay::DestructionTelemetry telemetry;
        std::size_t spawned = 0;
        const std::size_t limit = budget.max_debris();
        for (int i = 0; i < 4; ++i) {
            if (spawned >= limit) {
                telemetry.note_spill(Engine::Gameplay::DestructionStage::Spawn, 4 - spawned);
                break;
            }
            telemetry.note_spawned(1);
            ++spawned;
        }
        check(spawned == 2 && telemetry.step().spawnedDebris == 2 &&
                  telemetry.step().spilledDebris == 2 &&
                  telemetry.exceeded(Engine::Gameplay::DestructionStage::Spawn),
              "debris spawn burst bounded by maxSpawnedDebris (spill recorded)");
    }

    // 5e. Telemetry coherence: a capped explosion's result feeds the notes
    //     1:1 — the report mirrors the leaf result exactly.
    {
        TestWorld t = make_test_world();
        check(t.ok, "test world boots");
        // Wood in the heat ring (d in [6,7)): OUTSIDE the blast (blastRadius
        // 6), inside the heat (heatRadius 7) — so the fireball burns them
        // while the blast carves the stone ground below (cap 16).
        t.world->set_block(8, kGroundTop, 2, t.woodId);
        t.world->set_block(8, kGroundTop, 14, t.woodId);
        t.world->set_block(2, kGroundTop, 8, t.woodId);
        t.world->set_block(14, kGroundTop, 8, t.woodId);
        t.world->set_block(5, kGroundTop, 2, t.woodId);
        t.world->set_block(11, kGroundTop, 14, t.woodId);
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 6.0f;
        config.heatRadius = 7.0f;
        config.heatDamage = 1.0f;
        config.ignitionThreshold = 1.0f;
        config.maxPressure = 40.0f;
        config.maxCarvedCells = 16;
        config.maxBurnedCells = 2;
        config.maxImpulsedBodies = 2;
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        Engine::Gameplay::DestructionTelemetry telemetry;
        telemetry.note_carved(result.blocksRemoved);
        telemetry.note_burned(result.blocksIgnited);
        telemetry.note_impulsed(result.bodiesImpulsed);
        telemetry.note_spill(Engine::Gameplay::DestructionStage::Carve, result.carvedSkipped);
        telemetry.note_spill(Engine::Gameplay::DestructionStage::Burn, result.burnedSkipped);
        telemetry.note_spill(Engine::Gameplay::DestructionStage::Impulse, result.impulseSkipped);
        check(telemetry.step().carvedCells == result.blocksRemoved &&
                  telemetry.step().burnedCells == result.blocksIgnited &&
                  telemetry.step().impulsedBodies == result.bodiesImpulsed &&
                  telemetry.step().spilledCarved == result.carvedSkipped &&
                  telemetry.step().spilledBurned == result.burnedSkipped &&
                  telemetry.step().spilledImpulsed == result.impulseSkipped,
              "telemetry report mirrors the capped explosion result 1:1");
    }
    std::printf("[budget] pipeline integration (islands/spawn gates, leaf caps, coherence) OK\n");
}

// 6. Determinism: identical capped scenarios produce identical counters
//    (budget + telemetry + capped explosion), bit-exact across instances.
void test_determinism() {
    const auto run = []() {
        TestWorld t = make_test_world();
        // Wood in the heat ring (d=6.02, heat 7, blast 5): burned, not carved.
        t.world->set_block(8, kGroundTop, 2, t.woodId);
        t.world->set_block(8, kGroundTop, 14, t.woodId);
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 5.0f;
        config.heatRadius = 7.0f;
        config.heatDamage = 1.0f;
        config.ignitionThreshold = 1.0f;
        config.maxPressure = 40.0f;
        config.maxCarvedCells = 12;
        config.maxBurnedCells = 1;
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        Engine::Gameplay::DestructionTelemetry telemetry;
        telemetry.note_carved(result.blocksRemoved);
        telemetry.note_burned(result.blocksIgnited);
        telemetry.note_spill(Engine::Gameplay::DestructionStage::Carve, result.carvedSkipped);
        telemetry.note_spill(Engine::Gameplay::DestructionStage::Burn, result.burnedSkipped);
        return telemetry.step();
    };
    const Engine::Gameplay::DestructionCounters a = run();
    const Engine::Gameplay::DestructionCounters b = run();
    check(a.carvedCells == b.carvedCells && a.burnedCells == b.burnedCells &&
              a.spilledCarved == b.spilledCarved && a.spilledBurned == b.spilledBurned,
          "identical capped scenarios -> bit-identical telemetry counters");
    std::printf("[budget] determinism OK\n");
}

}  // namespace

int main() {
    test_budget_validation();
    test_budget_applies_to_leaves();
    test_telemetry_basics();
    test_explosion_budget_caps();
    test_pipeline_budget_integration();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[destruction] ALL PASSED\n");
        return 0;
    }
    std::printf("[destruction] %d FAILURE(S)\n", g_failures);
    return 1;
}

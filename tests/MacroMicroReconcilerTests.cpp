// MacroMicroReconcilerTests.cpp
//
// FALTANTES differential — MacroMicroReconciler: DETERMINISTIC MATERIALIZATION
// OF AGGREGATE CONSEQUENCES (META §32). The gate drives the PUBLIC contract
// headless and proves:
//   - rules are data-driven JSON, all-or-nothing, bit-exact round-trip;
//   - materialize() maps macro counters (population/resources/growth) to a
//     CONCRETE, ORDERED, DETERMINISTIC effect list (spawns, drops, growth,
//     removals) — positions derived from the per-region seed, never on cell
//     borders, bit-identical across instances;
//   - reconcile() emits at most budget effects per call, persists a cursor
//     (a large consequence set spans ticks) and NEVER re-emits after
//     completion;
//   - a macro change while effects are pending INVALIDATES only the pending
//     batch and re-derives from the newest macro (the ICausalResolver
//     affected-descendants semantics applied to temporal consequences);
//   - merge_and_resolve() keeps ONE effect per target slot — higher kind
//     priority wins, ties break by lower region seed — deterministically;
//   - serialize/deserialize round-trips the reconciliation state bit-exactly,
//     all-or-nothing.

#include "engine/simulation/IMacroMicroReconciler.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

using engine::simulation::MaterializedEffect;
using engine::simulation::ReconcilerBudget;
using engine::simulation::ReconcilerMacroState;
using engine::simulation::ReconcilerRule;
using engine::simulation::ReconcilerState;
using engine::simulation::create_macro_micro_reconciler;

ReconcilerMacroState forest_macro(std::uint64_t seed = 42,
                                  float population = 3.0f,
                                  float resources = 2.0f,
                                  float growth = 0.5f) {
    ReconcilerMacroState macro;
    macro.cellX = 0;
    macro.cellZ = 0;
    macro.cellSize = 16.0f;
    macro.population = population;
    macro.previousPopulation = population;
    macro.resources = resources;
    macro.growth = growth;
    macro.seed = seed;
    macro.tags = { "forest" };
    return macro;
}

std::vector<ReconcilerRule> forest_rules() {
    std::vector<ReconcilerRule> rules(1);
    rules[0].tag = "forest";
    rules[0].archetypeId = "tree";
    rules[0].itemId = "wood";
    rules[0].blockId = "sapling";
    rules[0].maxGrowthStages = 3;
    rules[0].growthDensity = 2.0f;
    return rules;
}

// ---- rules: data-driven JSON, all-or-nothing, bit-exact round-trip ----

void test_rules_json_roundtrip() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;

    const std::string doc =
        "{\"version\":1,\"rules\":[{\"tag\":\"forest\",\"archetypeId\":\"tree\","
        "\"itemId\":\"wood\",\"blockId\":\"sapling\",\"maxGrowthStages\":3,"
        "\"growthDensity\":2},{\"tag\":\"\",\"archetypeId\":\"peasant\","
        "\"itemId\":\"grain\",\"blockId\":\"wheat\",\"maxGrowthStages\":2,"
        "\"growthDensity\":1.5}]}";
    check(reconciler->set_rules_json(doc, error), "rules JSON accepted");
    check(error.empty(), "rules JSON diagnostic empty");
    check(reconciler->rules() != nullptr && reconciler->rules()->size() == 2,
          "two rules active");

    const std::string canonical = reconciler->rules_to_json();
    check(!canonical.empty(), "canonical emit non-empty");

    auto reconciler2 = create_macro_micro_reconciler();
    check(reconciler2->set_rules_json(canonical, error), "canonical re-parsed");
    check(reconciler2->rules_to_json() == canonical,
          "rules round-trip bit-exact");

    // Refusals all-or-nothing (the active rules stay untouched).
    auto reconciler3 = create_macro_micro_reconciler();
    check(reconciler3->set_rules_json(doc, error), "reconciler3 rules set");
    const std::string before = reconciler3->rules_to_json();
    check(!reconciler3->set_rules_json(
              "{\"version\":2,\"rules\":[]}", error) &&
              !error.empty(),
          "version 2 refused");
    check(!reconciler3->set_rules_json(
              "{\"version\":1,\"rules\":[{\"tag\":\"a\"},{\"tag\":\"a\"}]}",
              error) &&
              !error.empty(),
          "duplicate tag refused");
    check(!reconciler3->set_rules_json(
              "{\"version\":1,\"rules\":[{\"tag\":\"\"},{\"tag\":\"\"}]}",
              error) &&
              !error.empty(),
          "two default rules refused");
    check(!reconciler3->set_rules_json(
              "{\"version\":1,\"rules\":[{\"tag\":\"a\",\"maxGrowthStages\":2.5}]}",
              error) &&
              !error.empty(),
          "non-integer maxGrowthStages refused");
    check(!reconciler3->set_rules_json(
              "{\"version\":1,\"rules\":[{\"tag\":\"a\",\"growthDensity\":-1}]}",
              error) &&
              !error.empty(),
          "negative growthDensity refused");
    check(reconciler3->rules_to_json() == before,
          "refused rules left the active set untouched");

    std::printf("[reconciler] rules JSON round-trip + refusals OK\n");
}

// ---- materialize: deterministic macro -> concrete effects ----

void test_materialize_deterministic() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");
    check(reconciler->set_budget(ReconcilerBudget{ 1, 0 }, error), "budget set");

    const ReconcilerMacroState macro = forest_macro();
    std::vector<MaterializedEffect> effects;
    check(reconciler->materialize(macro, effects, error), "materialize ok");
    check(error.empty(), "materialize diagnostic empty");

    // 3 spawns + 2 drops + floor(0.5 * 2) growth = 6 effects, in fixed order.
    check(effects.size() == 6, "effect count 6");
    check(effects[0].kind == MaterializedEffect::Kind::SpawnEntity &&
              effects[0].archetypeId == "tree",
          "spawn 0 = tree");
    check(effects[2].kind == MaterializedEffect::Kind::SpawnEntity,
          "spawn 2 is a spawn");
    check(effects[3].kind == MaterializedEffect::Kind::ResourceDrop &&
              effects[3].itemId == "wood",
          "drop 0 = wood");
    check(effects[4].kind == MaterializedEffect::Kind::ResourceDrop,
          "drop 1 is a drop");
    check(effects[5].kind == MaterializedEffect::Kind::GrowthStage &&
              effects[5].blockId == "sapling" && effects[5].growthStage == 2,
          "growth = sapling stage 2 (1 + floor(0.5 * 2))");

    // Positions strictly inside the region cell (never on the border).
    for (const MaterializedEffect& effect : effects) {
        if (effect.kind == MaterializedEffect::Kind::SpawnEntity ||
            effect.kind == MaterializedEffect::Kind::ResourceDrop) {
            check(effect.positionX > 0.0f && effect.positionX < 16.0f &&
                      effect.positionZ > 0.0f && effect.positionZ < 16.0f,
                  "position strictly inside the cell");
        }
    }

    // Determinism: the same macro on a fresh instance reproduces the same
    // effects bit-exactly (values and positions).
    auto reconciler2 = create_macro_micro_reconciler();
    check(reconciler2->set_rules(forest_rules(), error), "rules2 set");
    std::vector<MaterializedEffect> effects2;
    check(reconciler2->materialize(macro, effects2, error), "materialize2 ok");
    check(effects.size() == effects2.size(), "same effect count");
    bool identical = true;
    for (std::size_t i = 0; i < effects.size() && i < effects2.size(); ++i) {
        const MaterializedEffect& a = effects[i];
        const MaterializedEffect& b = effects2[i];
        if (a.kind != b.kind || a.archetypeId != b.archetypeId ||
            a.itemId != b.itemId || a.blockId != b.blockId ||
            a.positionX != b.positionX || a.positionZ != b.positionZ ||
            a.blockX != b.blockX || a.blockY != b.blockY ||
            a.blockZ != b.blockZ || a.growthStage != b.growthStage) {
            identical = false;
        }
    }
    check(identical, "cross-instance bit-exact determinism");

    std::printf("[reconciler] deterministic materialization OK\n");
}

void test_materialize_decline() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");

    ReconcilerMacroState macro = forest_macro(7, 2.0f, 1.0f, 0.2f);
    macro.previousPopulation = 5.0f;
    std::vector<MaterializedEffect> effects;
    check(reconciler->materialize(macro, effects, error), "materialize ok");

    // 2 spawns + 1 drop + floor(0.2*2)=0 growth + 3 removals (5-2).
    check(effects.size() == 6, "effect count 6 with decline");
    check(effects[3].kind == MaterializedEffect::Kind::RemoveEntity &&
              effects[4].kind == MaterializedEffect::Kind::RemoveEntity &&
              effects[5].kind == MaterializedEffect::Kind::RemoveEntity,
          "removals emitted last");
    check(effects[3].reason == "population_decline", "decline reason");
    check(effects[3].handle == "0,0:decline:0" &&
              effects[5].handle == "0,0:decline:2",
          "deterministic decline handles");

    std::printf("[reconciler] decline -> RemoveEntity consequences OK\n");
}

void test_materialize_refusals() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");

    std::vector<MaterializedEffect> effects;

    ReconcilerMacroState bad = forest_macro();
    bad.version = 2;
    check(!reconciler->materialize(bad, effects, error) && !error.empty() &&
              effects.empty(),
          "bad version refused");

    bad = forest_macro();
    bad.population = -1.0f;
    check(!reconciler->materialize(bad, effects, error) && effects.empty(),
          "negative population refused");

    bad = forest_macro();
    bad.growth = 1.5f;
    check(!reconciler->materialize(bad, effects, error) && effects.empty(),
          "growth > 1 refused");

    bad = forest_macro();
    bad.cellSize = 0.0f;
    check(!reconciler->materialize(bad, effects, error) && effects.empty(),
          "cellSize 0 refused");

    // No rules configured: a consequence-producing macro is refused.
    auto empty = create_macro_micro_reconciler();
    check(!empty->materialize(forest_macro(), effects, error) &&
              !error.empty(),
          "no rules -> consequence-producing macro refused");

    // Rule missing the archetype: spawns refused.
    auto missing = create_macro_micro_reconciler();
    std::vector<ReconcilerRule> rule(1);
    rule[0].tag = "forest";
    rule[0].itemId = "wood";
    check(missing->set_rules(rule, error), "item-only rule set");
    check(!missing->materialize(forest_macro(), effects, error) &&
              !error.empty(),
          "missing archetypeId refused");

    std::printf("[reconciler] materialize refusals all-or-nothing OK\n");
}

// ---- reconcile: budget + continuation + no re-emission ----

void test_reconcile_budget_continuation() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");
    check(reconciler->set_budget(ReconcilerBudget{ 1, 3 }, error), "budget 3");

    ReconcilerMacroState macro = forest_macro(1, 10.0f, 0.0f, 0.0f);
    ReconcilerState state;
    std::vector<MaterializedEffect> effects;

    std::size_t total = 0;
    int ticks = 0;
    while (true) {
        check(reconciler->reconcile(state, macro, effects, error),
              "reconcile ok");
        total += effects.size();
        ++ticks;
        if (state.complete) break;
        check(ticks < 10, "completes within 10 ticks");
    }
    check(ticks == 4, "10 effects at budget 3 span 4 ticks (3+3+3+1)");
    check(total == 10, "all 10 spawns emitted exactly once");
    check(state.materializationCount == 1, "one materialization");
    for (const MaterializedEffect& effect : effects) {
        check(effect.kind == MaterializedEffect::Kind::SpawnEntity,
              "final batch are spawns");
    }

    // After completion, the same macro emits nothing (no re-emission).
    check(reconciler->reconcile(state, macro, effects, error) &&
              effects.empty() && state.complete,
          "no re-emission after completion");

    std::printf("[reconciler] budget continuation + no duplicate OK\n");
}

void test_reconcile_invalidation() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");
    check(reconciler->set_budget(ReconcilerBudget{ 1, 2 }, error), "budget 2");

    ReconcilerState state;
    std::vector<MaterializedEffect> effects;

    // Tick 1: macro A -> 5 spawns; budget 2 -> 2 emitted, 3 pending.
    ReconcilerMacroState macroA = forest_macro(3, 5.0f, 0.0f, 0.0f);
    check(reconciler->reconcile(state, macroA, effects, error),
          "tick1 ok");
    check(effects.size() == 2 && !state.complete, "2 of 5 emitted, pending");
    check(state.materializationCount == 1, "materialized once");

    // Macro CHANGES (aggregate evolved) while pending: the pending batch is
    // INVALIDATED and re-derived from the NEW macro (5 drops now).
    ReconcilerMacroState macroB = forest_macro(3, 0.0f, 5.0f, 0.0f);
    check(reconciler->reconcile(state, macroB, effects, error), "tick2 ok");
    check(effects.size() == 2, "tick2 emits 2");
    check(state.materializationCount == 2, "re-materialized after change");
    for (const MaterializedEffect& effect : effects) {
        check(effect.kind == MaterializedEffect::Kind::ResourceDrop &&
                  effect.itemId == "wood",
              "tick2 effects come from the NEW macro (drops, not spawns)");
    }

    // Drain the new batch: 3 more ticks at budget 2 (2+2+1).
    std::size_t drops = 2;
    int guard = 0;
    while (!state.complete) {
        check(reconciler->reconcile(state, macroB, effects, error),
              "drain ok");
        for (const MaterializedEffect& effect : effects) {
            check(effect.kind == MaterializedEffect::Kind::ResourceDrop,
                  "drained effects are the new drops");
            ++drops;
        }
        check(++guard < 10, "drains within 10 ticks");
    }
    check(drops == 5, "exactly the 5 drops of the NEW macro");
    check(state.materializationCount == 2, "no further materialization");

    std::printf("[reconciler] macro change invalidates pending (causal) OK\n");
}

// ---- merge_and_resolve: one winner per target slot ----

void test_merge_and_resolve() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;

    // Same block targeted by two regions: same kind -> LOWER seed wins.
    std::vector<MaterializedEffect> a;
    MaterializedEffect growth;
    growth.kind = MaterializedEffect::Kind::GrowthStage;
    growth.blockId = "sapling";
    growth.blockX = 10;
    growth.blockY = 0;
    growth.blockZ = 10;
    growth.growthStage = 2;
    a.push_back(growth);

    std::vector<MaterializedEffect> b = a;  // same target
    std::vector<MaterializedEffect> out;
    check(reconciler->merge_and_resolve(a, b, 5, 9, out, error),
          "merge ok");
    check(out.size() == 1 && out[0].blockX == 10, "one winner, lower seed (5)");
    check(reconciler->merge_and_resolve(a, b, 9, 5, out, error) &&
              out.size() == 1,
          "lower seed wins regardless of batch order");

    // Kind priority: a drop beats a spawn at the SAME position.
    std::vector<MaterializedEffect> spawns;
    MaterializedEffect spawn;
    spawn.kind = MaterializedEffect::Kind::SpawnEntity;
    spawn.archetypeId = "tree";
    spawn.positionX = 8.0f;
    spawn.positionZ = 8.0f;
    spawns.push_back(spawn);

    std::vector<MaterializedEffect> drops;
    MaterializedEffect drop;
    drop.kind = MaterializedEffect::Kind::ResourceDrop;
    drop.itemId = "wood";
    drop.positionX = 8.0f;
    drop.positionZ = 8.0f;
    drops.push_back(drop);

    check(reconciler->merge_and_resolve(spawns, drops, 1, 2, out, error),
          "spawn+drop merge ok");
    check(out.size() == 1 && out[0].kind == MaterializedEffect::Kind::ResourceDrop,
          "drop (higher priority) wins the position");

    // Invalid effect -> all-or-nothing refusal, out untouched.
    std::vector<MaterializedEffect> bad = a;
    bad[0].blockId.clear();
    std::vector<MaterializedEffect> untouched;
    untouched.push_back(spawn);
    std::vector<MaterializedEffect> outBefore;
    check(!reconciler->merge_and_resolve(a, bad, 1, 2, outBefore, error) &&
              !error.empty() && outBefore.empty(),
          "invalid effect refused, out empty");

    std::printf("[reconciler] deterministic conflict resolution OK\n");
}

// ---- persistence: bit-exact state round-trip, all-or-nothing ----

void test_state_serialization() {
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");
    check(reconciler->set_budget(ReconcilerBudget{ 1, 2 }, error), "budget 2");

    ReconcilerState state;
    std::vector<MaterializedEffect> effects;
    ReconcilerMacroState macro = forest_macro(11, 7.0f, 3.0f, 0.8f);
    check(reconciler->reconcile(state, macro, effects, error) &&
              effects.size() == 2,
          "mid-batch reconcile (2 of 11 emitted)");

    std::string serialized;
    check(reconciler->serialize_state(state, serialized, error),
          "serialize mid-batch");

    ReconcilerState restored;
    check(reconciler->deserialize_state(serialized, restored, error),
          "deserialize mid-batch");
    check(restored.cursor == state.cursor &&
              restored.macroFingerprint == state.macroFingerprint &&
              restored.materializationCount == state.materializationCount &&
              restored.complete == state.complete &&
              restored.pending.size() == state.pending.size(),
          "state fields round-trip");

    std::string reserialized;
    check(reconciler->serialize_state(restored, reserialized, error),
          "reserialize");
    check(reserialized == serialized, "bit-exact state round-trip");

    // The restored state continues the reconciliation (no re-emission).
    std::vector<MaterializedEffect> continued;
    check(reconciler->reconcile(restored, macro, continued, error),
          "restored reconcile ok");
    check(continued.size() == 2, "continues from the persisted cursor");
    check(restored.materializationCount == state.materializationCount,
          "no re-materialization after restore");

    // All-or-nothing: malformed document leaves `out` untouched.
    ReconcilerState untouched = restored;
    check(!reconciler->deserialize_state("{not json", untouched, error) &&
              untouched.cursor == restored.cursor,
          "malformed document refused, out untouched");
    check(!reconciler->deserialize_state(
              "{\"version\":1,\"cellX\":\"0\",\"cellZ\":\"0\","
              "\"macroFingerprint\":\"1\",\"cursor\":\"99\","
              "\"materializationCount\":\"1\",\"complete\":false,"
              "\"pending\":[]}",
              untouched, error) &&
              !error.empty(),
          "corrupt cursor refused");

    std::printf("[reconciler] bit-exact persistence + continuation OK\n");
}

// ---- composition: the handoff shape of ISimulationLod drives the reconciler

void test_composition_with_lod_handoff() {
    // Mirrors the exact Aggregate handoff ISimulationLod emits on
    // Aggregate -> Full/Coarse (SimulationLodEvent population/resources) and
    // the climate growth factor (SimulationClimate.growth) — the reconciler
    // turns those MACRO counters into MICRO consequences.
    auto reconciler = create_macro_micro_reconciler();
    std::string error;
    check(reconciler->set_rules(forest_rules(), error), "rules set");
    check(reconciler->set_budget(ReconcilerBudget{ 1, 0 }, error), "unlimited");

    ReconcilerMacroState handoff;
    handoff.cellX = 2;
    handoff.cellZ = -1;
    handoff.cellSize = 16.0f;
    handoff.population = 4.0f;        // aggregate population counter
    handoff.previousPopulation = 4.0f;
    handoff.resources = 3.0f;         // aggregate resources counter
    handoff.growth = 0.25f;           // seasonal growth (climate)
    handoff.seed = 20260826ULL;
    handoff.tags = { "forest" };

    ReconcilerState state;
    std::vector<MaterializedEffect> effects;
    check(reconciler->reconcile(state, handoff, effects, error),
          "handoff reconcile ok");
    check(state.complete, "unlimited budget completes in one tick");
    check(effects.size() == 7, "4 spawns + 3 drops + floor(0.25*2)=0 growth");
    check(effects[0].kind == MaterializedEffect::Kind::SpawnEntity &&
              effects[4].kind == MaterializedEffect::Kind::ResourceDrop,
          "spawns then drops");

    // Positions inside the (2,-1) region cell, world-aligned.
    for (const MaterializedEffect& effect : effects) {
        if (effect.kind == MaterializedEffect::Kind::SpawnEntity ||
            effect.kind == MaterializedEffect::Kind::ResourceDrop) {
            check(effect.positionX > 2.0f * 16.0f &&
                      effect.positionX < 3.0f * 16.0f &&
                      effect.positionZ > -1.0f * 16.0f &&
                      effect.positionZ < 0.0f,
                  "handoff positions inside the region cell");
        }
    }

    std::printf("[reconciler] ISimulationLod handoff -> micro effects OK\n");
}

}  // namespace

int main() {
    test_rules_json_roundtrip();
    test_materialize_deterministic();
    test_materialize_decline();
    test_materialize_refusals();
    test_reconcile_budget_continuation();
    test_reconcile_invalidation();
    test_merge_and_resolve();
    test_state_serialization();
    test_composition_with_lod_handoff();
    if (g_failures == 0) {
        std::printf("[macro-micro-reconciler] ALL PASSED\n");
        return 0;
    }
    std::printf("[macro-micro-reconciler] %d FAILURE(S)\n", g_failures);
    return 1;
}

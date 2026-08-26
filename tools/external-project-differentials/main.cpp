// External "differentials" consumer smoke (FALTANTES item 11 / S24 -- matrix;
// AGENT-5-DOCS-20260825, claim 02:05, 2026-08-26): proves the META S32
// content-pipeline differentials through their PUBLIC contracts ONLY —
// TimelineGraph, CausalResolver, MacroMicroReconciler, WorldDirector,
// EpisodeCompiler, HeadlessSimulationFarm and SemanticEngineAPI
// (findings #132/#134/#138/#140/#142/#145). Compiles and links ONLY against
// the installed SDK (find_package(vulkan_craft_sdk CONFIG) + <prefix>/include);
// no engine-tree reference anywhere.
//
// Exit code 0 + "differentials-consumer-ok" markers = the installed SDK is
// self-sufficient for the differential contracts (the "self-contained std"
// claim, verified from OUTSIDE the engine tree).

#include <engine/compiler/IEpisodeCompiler.hpp>
#include <engine/director/IWorldDirector.hpp>
#include <engine/semantic/ISemanticApi.hpp>
#include <engine/simfarm/ISimulationFarm.hpp>
#include <engine/simulation/IMacroMicroReconciler.hpp>
#include <engine/timeline/ICausalResolver.hpp>
#include <engine/timeline/ITimelineGraph.hpp>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "differentials consumer failure: " #condition "\n"; return 1; } } while (false)

// --- 1. TimelineGraph (#132): root, fork, write, effective payload, causal ---
int test_timeline() {
    using namespace engine::timeline;
    std::string error;
    auto graph = create_timeline_graph();
    CHECK(graph != nullptr);

    const std::vector<std::byte> rootPayload = {
        std::byte{ 'r' }, std::byte{ 'o' }, std::byte{ 'o' }, std::byte{ 't' } };
    const TimelineNodeId root = graph->create_root(rootPayload, error);
    CHECK(root != 0 && error.empty());

    const TimelineNodeId forkA = graph->fork(root, error);
    CHECK(forkA != 0 && error.empty());
    const TimelineNodeId forkB = graph->fork(root, error);
    CHECK(forkB != 0 && error.empty());

    // A write on forkA must NOT mutate forkB (copy-on-write).
    const std::vector<std::byte> aPayload = {
        std::byte{ 'A' }, std::byte{ '1' } };
    const TimelineNodeId a1 = graph->write(forkA, aPayload, error);
    CHECK(a1 != 0 && error.empty());

    std::vector<std::byte> effectiveB;
    CHECK(graph->effective_payload(forkB, effectiveB));
    CHECK(effectiveB == rootPayload);  // forkB still sees the root state

    std::vector<std::byte> effectiveA;
    CHECK(graph->effective_payload(a1, effectiveA));
    CHECK(effectiveA == aPayload);

    CHECK(graph->is_ancestor(root, a1));
    CHECK(!graph->is_ancestor(a1, root));
    CHECK(graph->common_ancestor(a1, forkB) == root);
    CHECK(graph->node_count() == 4);  // root + forkA + forkB + a1
    std::cout << "differentials-consumer-ok timeline\n";
    return 0;
}

// --- 2. CausalResolver (#132): leaves, derived, dirtiness, topo resolve ---
int test_causal() {
    using namespace engine::timeline;
    std::string error;
    auto resolver = create_causal_resolver();
    CHECK(resolver != nullptr);

    const CausalNodeId a = resolver->add_leaf("a", 2, error);
    const CausalNodeId b = resolver->add_leaf("b", 3, error);
    CHECK(a != 0 && b != 0 && error.empty());

    const CausalNodeId sum = resolver->add_derived(
        "sum", { a, b },
        [](const std::vector<std::int64_t>& inputs) {
            return inputs[0] + inputs[1];
        },
        error);
    CHECK(sum != 0 && error.empty());

    CHECK(resolver->resolve() == 1);  // sum recomputed once
    CausalNodeState sumState;
    CHECK(resolver->state(sum, sumState));
    CHECK(sumState.value == 5);

    CHECK(resolver->set_leaf_value(a, 10, error) && error.empty());
    const auto affected = resolver->affected_descendants(a);
    CHECK(affected.size() == 1 && affected[0] == sum);
    CHECK(resolver->resolve() == 1);
    CHECK(resolver->state(sum, sumState));
    CHECK(sumState.value == 13);
    std::cout << "differentials-consumer-ok causal\n";
    return 0;
}

// --- 3. MacroMicroReconciler (#134): rules, materialize, reconcile ---
int test_reconciler() {
    using namespace engine::simulation;
    std::string error;
    auto reconciler = create_macro_micro_reconciler();
    CHECK(reconciler != nullptr);

    ReconcilerRule rule;
    rule.tag = "forest";
    rule.archetypeId = "vc:deer";
    rule.itemId = "vc:wood";
    rule.blockId = "vc:sapling";
    rule.maxGrowthStages = 3;
    rule.growthDensity = 2.0f;
    CHECK(reconciler->set_rules({ rule }, error) && error.empty());

    ReconcilerBudget budget;
    budget.maxEffectsPerTick = 100;
    CHECK(reconciler->set_budget(budget, error) && error.empty());

    ReconcilerMacroState macro;
    macro.cellX = 0;
    macro.cellZ = 0;
    macro.cellSize = 16.0f;
    macro.population = 5.0f;
    macro.previousPopulation = 2.0f;  // growth: 3 population units
    macro.resources = 4.0f;
    macro.growth = 1.0f;
    macro.seed = 42;
    macro.tags = { "forest" };

    std::vector<MaterializedEffect> effects;
    CHECK(reconciler->materialize(macro, effects, error) && error.empty());
    CHECK(!effects.empty());

    ReconcilerState state;
    std::vector<MaterializedEffect> emitted;
    CHECK(reconciler->reconcile(state, macro, emitted, error) && error.empty());
    CHECK(state.complete);
    std::cout << "differentials-consumer-ok reconciler\n";
    return 0;
}

// --- 4. WorldDirector (#138): spec, eligibility, utility, selection ---
int test_director() {
    using namespace engine::director;
    std::string error;
    auto director = create_world_director();
    CHECK(director != nullptr);

    DirectorSpec spec;
    spec.version = 1;
    spec.maxPerTick = 1;
    spec.candidates.push_back({"raid"});
    CHECK(director->set_spec(spec, error) && error.empty());

    WorldEventCandidate candidate;
    candidate.id = "raid";
    candidate.baseUtility = 0.8f;
    candidate.weight = 1.0f;
    DirectorWorldState world;
    world.tick = 100;
    EventSelectionState sel;
    sel.id = "raid";
    std::string reason;
    bool eligible = false;
    CHECK(director->eligible(candidate, world, sel, reason));
    eligible = (reason == "eligible");
    CHECK(eligible);
    std::cout << "differentials-consumer-ok director\n";
    return 0;
}

// --- 5. SemanticEngineAPI (#145): schema, canonicalization, refusal ---
int test_semantic() {
    using namespace engine::semantic;
    std::string error;
    auto api = create_semantic_api();
    CHECK(api != nullptr);

    const std::string schema =
        "{\"name\":\"material\",\"version\":1,\"fields\":["
        "{\"name\":\"name\",\"type\":\"string\",\"required\":true},"
        "{\"name\":\"friction\",\"type\":\"double\",\"default\":0.6,\"min\":0,\"max\":1}]}";
    CHECK(api->register_kind_json(schema, error) && error.empty());

    std::string canonical;
    CHECK(api->validate("material", "{\"name\":\"Ice\",\"friction\":0.4}",
                        canonical, error));
    CHECK(canonical.find("\"friction\":0.4") != std::string::npos);

    std::string refused;
    CHECK(!api->validate("material", "{\"name\":\"X\",\"unknownKey\":1}",
                         refused, error) && !error.empty());
    CHECK(refused.empty());  // all-or-nothing: no partial canonical form
    std::cout << "differentials-consumer-ok semantic\n";
    return 0;
}

// --- 6. EpisodeCompiler (#140) + HeadlessSimulationFarm (#142) ---
int test_episode_farm() {
    using namespace engine::compiler;
    using namespace engine::simfarm;
    std::string error;

    auto compiler = create_episode_compiler();
    CHECK(compiler != nullptr);
    CHECK(compiler->register_validator(
              "mission",
              [](const std::string& json, std::string& e) {
                  if (json.empty() || json.front() != '{' ||
                      json.back() != '}') {
                      e = "not a json object";
                      return false;
                  }
                  return true;
              },
              error));
    CHECK(compiler->register_simulator(
              "mission",
              [](const std::string& json, int steps, std::string& trace,
                 std::string&) {
                  trace = "sim:" + std::to_string(steps) + ":" +
                          std::to_string(json.size());
                  return true;
              },
              error));
    CHECK(compiler->register_tester(
              "mission",
              [](const std::string&, std::string&) { return true; }, error));

    EpisodeManifest manifest;
    manifest.title = "ExternalDifferentials";
    EpisodeEntry entry;
    entry.kind = "mission";
    entry.name = "m1";
    entry.json = "{\"objective\":\"reach_the_peak\"}";
    manifest.entries.push_back(std::move(entry));

    EpisodePackage package;
    CHECK(compiler->compile(manifest, package, error) && error.empty());
    CHECK(compiler->verify(package, error));
    CHECK(!package.manifestJson.empty() && !package.payload.empty());
    CHECK(package.signature.size() == 32);  // default digest: 32 hex chars

    auto farm = create_simulation_farm();
    CHECK(farm != nullptr);
    CHECK(farm->set_driver(
              [](const std::string&, std::uint64_t seed,
                 const std::string& profile, std::uint64_t step,
                 std::string&) -> std::string {
                  return "run:" + profile + ":" + std::to_string(seed) + ":" +
                         std::to_string(step);
              },
              error));

    FarmConfig config;
    config.maxStepsPerTrial = 8;
    config.maxStallSteps = 2;
    config.seedStart = 0;
    config.seedEnd = 2;
    config.profiles = { "m1" };
    FarmReport report;
    CHECK(farm->run(config, report, error) && error.empty());
    CHECK(report.trialsRun == 2);
    CHECK(report.softlocks == 0 && report.repeats == 0);
    std::cout << "differentials-consumer-ok episode-farm\n";
    return 0;
}

int main() {
    if (test_timeline() != 0) return 1;
    if (test_causal() != 0) return 1;
    if (test_reconciler() != 0) return 1;
    if (test_director() != 0) return 1;
    if (test_semantic() != 0) return 1;
    if (test_episode_farm() != 0) return 1;
    std::cout << "differentials-consumer-ok all\n";
    return 0;
}

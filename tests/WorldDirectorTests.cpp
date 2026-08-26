// WorldDirectorTests.cpp
//
// FALTANTES differential — WorldDirector: EVENT SELECTION BY RULES, UTILITY,
// COHERENCE AND DIVERSITY (META §32). The gate drives the PUBLIC contract
// headless and proves:
//   - the spec is data-driven JSON, all-or-nothing, bit-exact round-trip;
//   - eligibility gates (requiresAll / excludesAny / cooldown / concurrency /
//     daily limit / disabled) with deterministic reasons;
//   - utility scoring (baseUtility * weight * urgency - diversity penalty) is
//     deterministic and drives the selection;
//   - DIVERSITY: a recently-fired candidate pays the penalty, so the
//     selection spreads across candidates and recovers after a full window;
//   - COHERENCE: the world-state tags gate eligibility (peace/war flips the
//     eligible set);
//   - select() advances the caller-owned selection states deterministically
//     (lastFireTick / fireCount / firesThisDay / dayOfLastFire /
//     selectedCount) and persists bit-exactly (cooldown survives a
//     save/load cycle);
//   - all-or-nothing refusals (no spec, bad world, duplicate/missing
//     selection ids, chosen event without a state) mutate nothing;
//   - cross-instance determinism: same inputs -> identical decisions and
//     identical state deltas.

#include "engine/director/IWorldDirector.hpp"

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

using engine::director::DirectorSelection;
using engine::director::DirectorSpec;
using engine::director::DirectorWorldState;
using engine::director::EventSelectionState;
using engine::director::WorldEventCandidate;
using engine::director::create_world_director;

// A spec with one candidate per gate.
DirectorSpec gate_spec() {
    DirectorSpec spec;
    spec.dayLengthTicks = 100;

    WorldEventCandidate raid;
    raid.id = "raid";
    raid.requiresAll = { "war" };
    spec.candidates.push_back(raid);

    WorldEventCandidate storm;
    storm.id = "storm";
    storm.excludesAny = { "peace" };
    spec.candidates.push_back(storm);

    WorldEventCandidate guard;
    guard.id = "guard";
    guard.cooldownTicks = 100;
    spec.candidates.push_back(guard);

    WorldEventCandidate gift;
    gift.id = "gift";
    gift.maxPerDay = 1;
    spec.candidates.push_back(gift);

    WorldEventCandidate army;
    army.id = "army";
    army.maxConcurrent = 1;
    spec.candidates.push_back(army);

    WorldEventCandidate off;
    off.id = "off";
    off.weight = 0.0f;
    spec.candidates.push_back(off);

    return spec;
}

// ---- spec: data-driven JSON, all-or-nothing, bit-exact round-trip ----

void test_spec_json_roundtrip() {
    auto director = create_world_director();
    std::string error;

    const std::string doc =
        "{\"version\":1,\"maxPerTick\":2,\"diversityPenalty\":0.3,"
        "\"recencyWindow\":500,\"dayLengthTicks\":120,"
        "\"candidates\":[{\"id\":\"raid\",\"requiresAll\":[\"war\"],"
        "\"excludesAny\":[\"peace\"],\"baseUtility\":0.9,\"weight\":1.5,"
        "\"cooldownTicks\":100,\"maxConcurrent\":2,\"maxPerDay\":3,"
        "\"category\":\"combat\"},{\"id\":\"storm\",\"baseUtility\":0.4,"
        "\"weight\":1,\"cooldownTicks\":0,\"maxConcurrent\":1,"
        "\"maxPerDay\":0,\"category\":\"weather\"}]}";
    check(director->set_spec_json(doc, error), "spec JSON accepted");
    check(error.empty(), "spec JSON diagnostic empty");
    check(director->spec() != nullptr && director->spec()->candidates.size() == 2,
          "two candidates active");

    const std::string canonical = director->spec_to_json();
    check(!canonical.empty(), "canonical emit non-empty");

    auto director2 = create_world_director();
    check(director2->set_spec_json(canonical, error), "canonical re-parsed");
    check(director2->spec_to_json() == canonical,
          "spec round-trip bit-exact");

    // Refusals all-or-nothing (the active spec stays untouched).
    auto director3 = create_world_director();
    check(director3->set_spec_json(doc, error), "director3 spec set");
    const std::string before = director3->spec_to_json();
    check(!director3->set_spec_json(
              "{\"version\":2,\"candidates\":[]}", error) &&
              !error.empty(),
          "version 2 refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"candidates\":[{\"id\":\"a\"},{\"id\":\"a\"}]}",
              error) &&
              !error.empty(),
          "duplicate id refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"candidates\":[{\"id\":\"\","
              "\"baseUtility\":0.5}]}",
              error) &&
              !error.empty(),
          "empty id refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"candidates\":[{\"id\":\"a\","
              "\"baseUtility\":1.5}]}",
              error) &&
              !error.empty(),
          "baseUtility > 1 refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"candidates\":[{\"id\":\"a\",\"weight\":-1}]}",
              error) &&
              !error.empty(),
          "negative weight refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"candidates\":[{\"id\":\"a\","
              "\"maxConcurrent\":0}]}",
              error) &&
              !error.empty(),
          "maxConcurrent 0 refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"recencyWindow\":0,\"candidates\":[]}", error) &&
              !error.empty(),
          "recencyWindow 0 refused");
    check(!director3->set_spec_json(
              "{\"version\":1,\"dayLengthTicks\":0,\"candidates\":[]}", error) &&
              !error.empty(),
          "dayLengthTicks 0 refused");
    check(director3->spec_to_json() == before,
          "refused specs left the active spec untouched");

    std::printf("[director] spec JSON round-trip + refusals OK\n");
}

// ---- eligibility gates ----

void test_eligibility_gates() {
    auto director = create_world_director();
    std::string error;
    check(director->set_spec(gate_spec(), error), "gate spec set");

    DirectorWorldState world;
    world.tags = { "peace" };
    std::vector<EventSelectionState> selections;
    for (const WorldEventCandidate& candidate : director->spec()->candidates) {
        EventSelectionState state;
        state.id = candidate.id;
        selections.push_back(state);
    }
    const auto& stateOf = [&selections](const std::string& id)
        -> const EventSelectionState& {
        for (const EventSelectionState& state : selections) {
            if (state.id == id) return state;
        }
        return selections[0];
    };

    std::string reason;
    check(director->eligible(director->spec()->candidates[0], world,
                             stateOf("raid"), reason) == false &&
              reason == "missing_tags",
          "raid gated by missing war tag");
    check(director->eligible(director->spec()->candidates[1], world,
                             stateOf("storm"), reason) == false &&
              reason == "excluded_tag",
          "storm gated by excluded peace tag");

    world.tags = { "war" };
    check(director->eligible(director->spec()->candidates[0], world,
                             stateOf("raid"), reason) &&
              reason == "eligible",
          "raid eligible under war");

    // Cooldown.
    for (EventSelectionState& state : selections) {
        if (state.id == "guard") {
            state.lastFireTick = 0;
            state.fireCount = 1;
        }
    }
    world.tick = 50;
    check(director->eligible(director->spec()->candidates[2], world,
                             stateOf("guard"), reason) == false &&
              reason == "cooldown",
          "guard in cooldown at tick 50");
    world.tick = 100;
    check(director->eligible(director->spec()->candidates[2], world,
                             stateOf("guard"), reason) &&
              reason == "eligible",
          "guard eligible after cooldown");

    // Concurrency cap.
    for (EventSelectionState& state : selections) {
        if (state.id == "army") state.activeCount = 1;
    }
    check(director->eligible(director->spec()->candidates[4], world,
                             stateOf("army"), reason) == false &&
              reason == "concurrency_limit",
          "army at concurrency cap");

    // Daily limit (dayLengthTicks 100; day 1 = ticks 100..199).
    for (EventSelectionState& state : selections) {
        if (state.id == "gift") {
            state.dayOfLastFire = 1;
            state.firesThisDay = 1;
        }
    }
    world.tick = 150;
    check(director->eligible(director->spec()->candidates[3], world,
                             stateOf("gift"), reason) == false &&
              reason == "daily_limit",
          "gift at daily limit");
    world.tick = 250;  // day 2
    check(director->eligible(director->spec()->candidates[3], world,
                             stateOf("gift"), reason) &&
              reason == "eligible",
          "gift eligible next day");

    // Disabled.
    check(director->eligible(director->spec()->candidates[5], world,
                             stateOf("off"), reason) == false &&
              reason == "disabled",
          "zero-weight candidate disabled");

    std::printf("[director] eligibility gates + reasons OK\n");
}

// ---- utility + deterministic selection ----

void test_utility_and_selection() {
    auto director = create_world_director();
    std::string error;

    DirectorSpec spec;
    WorldEventCandidate a;
    a.id = "alpha";
    a.baseUtility = 0.9f;
    WorldEventCandidate b;
    b.id = "beta";
    b.baseUtility = 0.3f;
    spec.candidates = { a, b };
    spec.maxPerTick = 1;
    check(director->set_spec(spec, error), "spec set");

    DirectorWorldState world;
    std::vector<EventSelectionState> selections = { { "alpha" }, { "beta" } };
    std::vector<DirectorSelection> out;

    check(director->select(world, selections, out, error), "select ok");
    check(out.size() == 1 && out[0].eventId == "alpha",
          "alpha (higher utility) selected");
    check(out[0].utility == 0.9f, "alpha utility 0.9 (bit-exact)");
    check(out[0].reason == "eligible", "selection reason eligible");
    for (const EventSelectionState& state : selections) {
        if (state.id == "alpha") {
            check(state.fireCount == 1 && state.lastFireTick == 0 &&
                      state.selectedCount == 1 && state.firesThisDay == 1 &&
                      state.dayOfLastFire == 0,
                  "alpha state advanced deterministically");
        }
    }

    // Determinism: identical inputs on a fresh instance -> identical out.
    auto director2 = create_world_director();
    check(director2->set_spec(spec, error), "spec2 set");
    DirectorWorldState world2;
    std::vector<EventSelectionState> selections2 = { { "alpha" }, { "beta" } };
    std::vector<DirectorSelection> out2;
    check(director2->select(world2, selections2, out2, error), "select2 ok");
    check(out2.size() == out.size() &&
              out2[0].eventId == out[0].eventId &&
              out2[0].utility == out[0].utility &&
              out2[0].reason == out[0].reason,
          "cross-instance deterministic decision");
    check(selections2[0].fireCount == selections[0].fireCount &&
              selections2[0].lastFireTick == selections[0].lastFireTick,
          "cross-instance deterministic state delta");

    std::printf("[director] utility-driven deterministic selection OK\n");
}

// ---- diversity: recency penalty spreads the selection ----

void test_diversity_penalty() {
    auto director = create_world_director();
    std::string error;

    DirectorSpec spec;
    WorldEventCandidate a;
    a.id = "alpha";
    a.baseUtility = 0.5f;
    WorldEventCandidate b;
    b.id = "beta";
    b.baseUtility = 0.5f;
    spec.candidates = { a, b };
    spec.maxPerTick = 1;
    spec.recencyWindow = 100;
    spec.diversityPenalty = 0.25f;
    check(director->set_spec(spec, error), "spec set");

    DirectorWorldState world;
    std::vector<EventSelectionState> selections = { { "alpha" }, { "beta" } };
    std::vector<DirectorSelection> out;

    // Tick 0: tie -> id ASC -> alpha.
    check(director->select(world, selections, out, error) &&
              out.size() == 1 && out[0].eventId == "alpha",
          "first selection alpha (tie, id order)");

    // Tick 10: alpha just fired (urgency 0.1 -> pays the penalty); beta is
    // fresh (urgency 1). Beta wins.
    world.tick = 10;
    check(director->select(world, selections, out, error) &&
              out.size() == 1 && out[0].eventId == "beta",
          "diversity: beta wins while alpha is recent");
    check(out[0].utility == 0.5f,
          "beta utility 0.5 (fresh, no penalty)");

    // After a full window (tick 200): alpha fully urgent again -> tie ->
    // id ASC -> alpha.
    world.tick = 200;
    check(director->select(world, selections, out, error) &&
              out.size() == 1 && out[0].eventId == "alpha",
          "alpha recovers after the recency window");

    std::printf("[director] diversity penalty + recovery OK\n");
}

// ---- coherence: world tags flip the eligible set ----

void test_coherence_world_tags() {
    auto director = create_world_director();
    std::string error;

    DirectorSpec spec;
    WorldEventCandidate raid;
    raid.id = "raid";
    raid.requiresAll = { "war" };
    WorldEventCandidate storm;
    storm.id = "storm";
    storm.excludesAny = { "peace" };
    spec.candidates = { raid, storm };
    spec.maxPerTick = 0;  // select ALL eligible
    check(director->set_spec(spec, error), "spec set");

    DirectorWorldState world;
    world.tags = { "peace" };
    std::vector<EventSelectionState> selections = { { "raid" }, { "storm" } };
    std::vector<DirectorSelection> out;

    // Peace: raid misses war, storm is excluded by peace -> NOTHING fires.
    check(director->select(world, selections, out, error), "select (peace)");
    check(out.empty(), "no coherent event under peace");

    // War: both become eligible (maxPerTick 0 -> both selected).
    world.tags = { "war" };
    check(director->select(world, selections, out, error), "select (war)");
    check(out.size() == 2 && out[0].eventId == "raid" &&
              out[1].eventId == "storm",
          "both events selected coherently under war");

    std::printf("[director] coherence via world tags OK\n");
}

// ---- persistence: cooldown survives save/load ----

void test_selection_persistence() {
    auto director = create_world_director();
    std::string error;

    DirectorSpec spec;
    WorldEventCandidate guard;
    guard.id = "guard";
    guard.cooldownTicks = 100;
    spec.candidates = { guard };
    check(director->set_spec(spec, error), "spec set");

    DirectorWorldState world;
    std::vector<EventSelectionState> selections = { { "guard" } };
    std::vector<DirectorSelection> out;

    check(director->select(world, selections, out, error) &&
              out.size() == 1 && out[0].eventId == "guard",
          "guard selected at tick 0");

    std::string serialized;
    check(director->serialize_selections(selections, serialized, error),
          "serialize");

    std::vector<EventSelectionState> restored;
    check(director->deserialize_selections(serialized, restored, error),
          "deserialize");
    check(restored.size() == 1 && restored[0].id == "guard" &&
              restored[0].fireCount == 1 && restored[0].lastFireTick == 0,
          "selection state round-trips");

    std::string reserialized;
    check(director->serialize_selections(restored, reserialized, error),
          "reserialize");
    check(reserialized == serialized, "bit-exact round-trip");

    // The restored cooldown is honored: tick 50 -> nothing, tick 100 -> again.
    world.tick = 50;
    check(director->select(world, restored, out, error) && out.empty(),
          "cooldown survives restore (tick 50)");
    world.tick = 100;
    check(director->select(world, restored, out, error) &&
              out.size() == 1 && out[0].eventId == "guard",
          "eligible again after cooldown (tick 100)");

    // All-or-nothing: malformed document leaves out untouched.
    std::vector<EventSelectionState> untouched = restored;
    check(!director->deserialize_selections("{not json", untouched, error) &&
              untouched.size() == 1,
          "malformed document refused, out untouched");

    std::printf("[director] bit-exact persistence + cooldown OK\n");
}

// ---- all-or-nothing refusals ----

void test_refusals() {
    auto director = create_world_director();
    std::string error;
    DirectorWorldState world;
    std::vector<EventSelectionState> selections = { { "a" } };
    std::vector<DirectorSelection> out;

    check(!director->select(world, selections, out, error) && !error.empty() &&
              out.empty(),
          "select without spec refused");

    DirectorSpec spec;
    WorldEventCandidate a;
    a.id = "a";
    spec.candidates = { a };
    check(director->set_spec(spec, error), "spec set");

    world.version = 2;
    check(!director->select(world, selections, out, error) && !error.empty(),
          "world version 2 refused");
    world.version = 1;

    selections = { { "a" }, { "a" } };
    check(!director->select(world, selections, out, error) && !error.empty(),
          "duplicate selection id refused");

    selections = { { "unknown" } };
    check(!director->select(world, selections, out, error) && !error.empty(),
          "selection id absent from spec refused");

    // A chosen event WITHOUT a selection state refuses all-or-nothing.
    selections.clear();
    std::vector<EventSelectionState> stateBefore = selections;
    check(!director->select(world, selections, out, error) &&
              !error.empty() && out.empty(),
          "chosen event without a state refused");
    check(selections.empty(), "nothing mutated on refusal");

    std::printf("[director] all-or-nothing refusals OK\n");
}

}  // namespace

int main() {
    test_spec_json_roundtrip();
    test_eligibility_gates();
    test_utility_and_selection();
    test_diversity_penalty();
    test_coherence_world_tags();
    test_selection_persistence();
    test_refusals();
    if (g_failures == 0) {
        std::printf("[world-director] ALL PASSED\n");
        return 0;
    }
    std::printf("[world-director] %d FAILURE(S)\n", g_failures);
    return 1;
}

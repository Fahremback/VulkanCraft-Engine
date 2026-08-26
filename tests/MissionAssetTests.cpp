// MissionAssetTests.cpp
//
// FALTANTES item 23 — "criar scripts, abilities, animações, missões e
// diálogos": missions as data-driven JSON assets (objectives, dialogue graph,
// unlock conditions, rewards) applied through the public IMissionWorld seam.
// The runtime is deterministic with caller-owned progress (MissionState) and
// reports every decision as MissionEvents; the state serializes bit-exactly.
//
// Proves, headless and text-only:
//   - definition validation all-or-nothing + JSON round-trip bit-exact;
//   - unlock conditions (can_accept/accept);
//   - objective progress (collect via count_of, reach via position),
//     gating conditions, ObjectiveCompleted and MissionCompleted;
//   - dialogue graph navigation (offered choices filtered by conditions,
//     advance to next node / end, invalid index refused);
//   - completion + reward application (item/xp/flag), repeatable reset;
//   - state serialization bit-exact + all-or-nothing deserialization;
//   - determinism across fresh instances.

#include "engine/gameplay/IMissionAsset.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
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

// ---- deterministic mock world (the public seam) ------------------------------

struct MockMissionWorld : public engine::gameplay::IMissionWorld {
    std::map<std::string, float> counts;
    std::map<std::string, bool> flags;
    std::map<std::string, float> attributes;
    float px{ 0.0f };
    float pz{ 0.0f };
    bool hasPosition{ true };
    std::string rewardItem;
    int rewardCount{ 0 };
    int rewardXp{ 0 };
    std::vector<std::string> setFlags;

    float count_of(const std::string& key) const override {
        const auto it = counts.find(key);
        return it == counts.end() ? 0.0f : it->second;
    }
    bool flag(const std::string& key) const override {
        const auto it = flags.find(key);
        return it != flags.end() && it->second;
    }
    float attribute(const std::string& key) const override {
        const auto it = attributes.find(key);
        return it == attributes.end() ? 0.0f : it->second;
    }
    bool position(float& x, float& z) const override {
        if (!hasPosition) return false;
        x = px;
        z = pz;
        return true;
    }
    bool apply_reward(const std::string& itemId, int count, int xp) override {
        rewardItem = itemId;
        rewardCount = count;
        rewardXp = xp;
        return true;
    }
    bool set_flag(const std::string& key) override {
        setFlags.push_back(key);
        flags[key] = true;
        return true;
    }
};

// ---- reference mission --------------------------------------------------------

engine::gameplay::MissionDefinition make_mission() {
    using namespace engine::gameplay;
    MissionDefinition definition;
    definition.name = "First Steps";
    // Canonical UUID (with hyphens) so the JSON round-trip is bit-exact — a
    // non-canonical id would be re-derived on load (findings #84 lesson).
    definition.id = "11111111-2222-3333-4444-555555555555";
    MissionObjective collect;
    collect.id = "collect_stone";
    collect.kind = MissionObjectiveKind::Collect;
    collect.target = "stone";
    collect.count = 3;
    MissionObjective reach;
    reach.id = "reach_hill";
    reach.kind = MissionObjectiveKind::Reach;
    reach.count = 1;
    reach.x = 100.0f;
    reach.z = -50.0f;
    reach.radius = 5.0f;
    definition.objectives = { collect, reach };
    DialogueChoice what;
    what.text = "What should I do?";
    what.next = "intro";
    DialogueChoice skip;
    skip.text = "Skip";
    skip.next = "";
    MissionCondition seenIntro;
    seenIntro.kind = MissionConditionKind::Flag;
    seenIntro.key = "seen_intro";
    seenIntro.flagValue = true;
    skip.conditions = { seenIntro };
    DialogueNode start;
    start.id = "start";
    start.speaker = "Mentor";
    start.text = "Welcome!";
    start.choices = { what, skip };
    DialogueNode intro;
    intro.id = "intro";
    intro.speaker = "Mentor";
    intro.text = "Collect 3 stone.";
    definition.dialogue = { start, intro };
    MissionCondition levelGate;
    levelGate.kind = MissionConditionKind::Attribute;
    levelGate.key = "level";
    levelGate.op = ">=";
    levelGate.value = 2.0f;
    definition.unlockConditions = { levelGate };
    definition.reward.itemId = "coin";
    definition.reward.count = 5;
    definition.reward.xp = 100;
    definition.reward.setFlag = "quest1_done";
    return definition;
}

// ---- definition validation ----------------------------------------------------

void test_definition_validation() {
    std::string error;
    const engine::gameplay::MissionDefinition base = make_mission();
    check(base.validate(error), "reference mission validates");

    auto bad = base;
    bad.name = "";
    check(!bad.validate(error), "empty name refused");
    bad = base;
    bad.version = 2;
    check(!bad.validate(error), "version 2 refused");
    bad = base;
    bad.objectives.clear();
    check(!bad.validate(error), "empty objectives refused");
    bad = base;
    bad.objectives[0].id = bad.objectives[1].id;
    check(!bad.validate(error), "duplicate objective id refused");
    bad = base;
    bad.objectives[0].target = "";
    check(!bad.validate(error), "collect objective without target refused");
    bad = base;
    bad.objectives[0].count = 0;
    check(!bad.validate(error), "objective count 0 refused");
    bad = base;
    bad.objectives[1].radius = -1.0f;
    check(!bad.validate(error), "negative reach radius refused");
    bad = base;
    bad.objectives[0].conditions.push_back(
        engine::gameplay::MissionCondition{engine::gameplay::MissionConditionKind::Counter,
                                           "stone", "??", 1.0f, true});
    check(!bad.validate(error), "invalid condition op refused");
    bad = base;
    engine::gameplay::MissionCondition unknownObjective;
    unknownObjective.kind = engine::gameplay::MissionConditionKind::ObjectiveDone;
    unknownObjective.key = "nope";
    bad.unlockConditions.push_back(unknownObjective);
    check(!bad.validate(error), "objectiveDone with unknown objective refused");
    bad = base;
    bad.dialogue = {engine::gameplay::DialogueNode{"intro", "Mentor", "hi", {}}};
    check(!bad.validate(error), "dialogue without a 'start' node refused");
    bad = base;
    bad.dialogue[1].id = "start";
    check(!bad.validate(error), "duplicate dialogue node id refused");
    bad = base;
    bad.dialogue[0].choices[0].next = "missing_node";
    check(!bad.validate(error), "choice next referencing an unknown node refused");
    bad = base;
    bad.dialogue[0].choices[0].text = "";
    check(!bad.validate(error), "empty choice text refused");
    bad = base;
    bad.reward.count = -1;
    check(!bad.validate(error), "negative reward count refused");
    bad = base;
    bad.reward.xp = -1;
    check(!bad.validate(error), "negative reward xp refused");
}

// ---- JSON round-trip ----------------------------------------------------------

void test_json_round_trip() {
    const engine::gameplay::MissionDefinition definition = make_mission();
    const std::string json = definition.to_json();
    engine::gameplay::MissionDefinition loaded;
    std::string error;
    check(loaded.load_from_json(json, error), "mission JSON loads");
    check(loaded.to_json() == json, "mission JSON round-trips bit-exact");
    check(loaded.name == "First Steps" && loaded.objectives.size() == 2 &&
              loaded.dialogue.size() == 2 && loaded.unlockConditions.size() == 1,
          "mission fields survive the round-trip");
    check(loaded.objectives[0].target == "stone" && loaded.objectives[0].count == 3 &&
              loaded.objectives[1].kind == engine::gameplay::MissionObjectiveKind::Reach,
          "objective fields survive the round-trip");
    check(loaded.dialogue[0].id == "start" && loaded.dialogue[0].choices.size() == 2 &&
              loaded.dialogue[0].choices[0].next == "intro",
          "dialogue fields survive the round-trip");
    check(loaded.reward.itemId == "coin" && loaded.reward.count == 5 &&
              loaded.reward.xp == 100 && loaded.reward.setFlag == "quest1_done",
          "reward fields survive the round-trip");
    // All-or-nothing: malformed documents are refused.
    check(!loaded.load_from_json("not json", error), "malformed mission JSON refused");
    check(!loaded.load_from_json("{\"name\":\"X\",\"objectives\":[]}", error),
          "mission with empty objectives refused at load");
    // id derived from the name when omitted.
    check(loaded.load_from_json(
              "{\"name\":\"NoId\",\"objectives\":[{\"id\":\"a\",\"kind\":\"collect\","
              "\"target\":\"stone\",\"count\":1}]}",
              error),
          "mission without explicit id loads");
    check(!loaded.id.empty() && loaded.id != "missions:First Steps",
          "id derived from the name");
}

// ---- unlock / accept ----------------------------------------------------------

void test_accept_and_unlock() {
    using namespace engine::gameplay;
    auto runtime = create_mission_runtime();
    const MissionDefinition definition = make_mission();
    MockMissionWorld world;
    MissionState state;
    std::string error;

    world.attributes["level"] = 1.0f;
    check(!runtime->can_accept(definition, state, world, error),
          "level 1 cannot accept (needs level >= 2)");
    std::vector<MissionEvent> events;
    check(!runtime->accept(definition, state, world, events, error),
          "accept refused when unlock conditions fail");
    check(!state.accepted, "state stays unaccepted after refusal");

    world.attributes["level"] = 2.0f;
    check(runtime->can_accept(definition, state, world, error),
          "level 2 can accept");
    check(runtime->accept(definition, state, world, events, error),
          "accept succeeds");
    check(state.accepted && state.missionId == "11111111-2222-3333-4444-555555555555",
          "state accepted with the mission id");
    check(state.dialogueNode == "start", "dialogue starts at 'start'");
    check(events.size() >= 2 && events[0].kind == MissionEvent::Kind::Accepted &&
              events[1].kind == MissionEvent::Kind::DialogueShown &&
              events[1].nodeId == "start",
          "accept emits Accepted + DialogueShown(start)");
    events.clear();
    check(!runtime->accept(definition, state, world, events, error),
          "double accept refused");
}

// ---- objective progress -------------------------------------------------------

void test_objective_progress() {
    using namespace engine::gameplay;
    auto runtime = create_mission_runtime();
    const MissionDefinition definition = make_mission();
    MockMissionWorld world;
    world.attributes["level"] = 2.0f;
    MissionState state;
    std::string error;
    std::vector<MissionEvent> events;
    check(runtime->accept(definition, state, world, events, error), "mission accepted");

    // Progress the collect objective from the world counter.
    world.counts["stone"] = 2.0f;
    events.clear();
    check(runtime->update(definition, state, world, events, error), "update ok");
    check(state.objectiveProgress["collect_stone"] == 2.0f,
          "collect progress mirrors the world counter");
    bool stoneDone = false;
    bool missionDone = false;
    for (const auto& event : events) {
        if (event.kind == MissionEvent::Kind::ObjectiveCompleted &&
            event.objectiveId == "collect_stone") {
            stoneDone = true;
        }
        if (event.kind == MissionEvent::Kind::MissionCompleted) missionDone = true;
    }
    check(!stoneDone && !missionDone, "objective not complete yet");

    // Reach the hill -> reach objective done; both done -> MissionCompleted.
    world.px = 101.0f;
    world.pz = -50.0f;
    events.clear();
    check(runtime->update(definition, state, world, events, error), "update ok");
    for (const auto& event : events) {
        if (event.kind == MissionEvent::Kind::ObjectiveCompleted &&
            event.objectiveId == "reach_hill") {
            stoneDone = true;
        }
        if (event.kind == MissionEvent::Kind::MissionCompleted) missionDone = true;
    }
    check(stoneDone, "reach objective completed");
    check(state.objectiveProgress["reach_hill"] == 1.0f, "reach progress set to target");
    check(!missionDone, "mission not completed while collect is still pending");

    world.counts["stone"] = 3.0f;
    events.clear();
    check(runtime->update(definition, state, world, events, error), "update ok");
    missionDone = false;
    bool stoneComplete = false;
    for (const auto& event : events) {
        if (event.kind == MissionEvent::Kind::ObjectiveCompleted &&
            event.objectiveId == "collect_stone") {
            stoneComplete = true;
        }
        if (event.kind == MissionEvent::Kind::MissionCompleted) missionDone = true;
    }
    check(stoneComplete, "collect objective completed");
    check(missionDone, "MissionCompleted fired when all objectives are done");
    check(state.objectiveProgress["collect_stone"] == 3.0f,
          "collect progress clamps at the target");
}

// ---- objective gating conditions ----------------------------------------------

void test_objective_gating() {
    using namespace engine::gameplay;
    MissionDefinition definition = make_mission();
    definition.objectives[0].conditions.push_back(
        MissionCondition{MissionConditionKind::Flag, "stones_unlocked", ">=", 0.0f, true});
    auto runtime = create_mission_runtime();
    MockMissionWorld world;
    world.attributes["level"] = 2.0f;
    world.counts["stone"] = 10.0f;  // plenty, but the objective is gated
    MissionState state;
    std::string error;
    std::vector<MissionEvent> events;
    check(runtime->accept(definition, state, world, events, error), "mission accepted");
    events.clear();
    check(runtime->update(definition, state, world, events, error), "update ok");
    check(state.objectiveProgress["collect_stone"] == 0.0f,
          "gated objective does not progress while its condition fails");
    world.flags["stones_unlocked"] = true;
    events.clear();
    check(runtime->update(definition, state, world, events, error), "update ok");
    check(state.objectiveProgress["collect_stone"] == 3.0f,
          "gated objective progresses once its condition passes");
}

// ---- dialogue navigation ------------------------------------------------------

void test_dialogue_advance() {
    using namespace engine::gameplay;
    auto runtime = create_mission_runtime();
    const MissionDefinition definition = make_mission();
    MockMissionWorld world;
    world.attributes["level"] = 2.0f;
    MissionState state;
    std::string error;
    std::vector<MissionEvent> events;
    check(runtime->accept(definition, state, world, events, error), "mission accepted");

    // The "Skip" choice requires the seen_intro flag — not set, so only the
    // "What should I do?" choice is offered.
    check(runtime->advance_dialogue(definition, state, 0, world, events, error),
          "advance to the offered choice");
    check(state.dialogueNode == "intro", "dialogue moved to 'intro'");
    check(events.size() == 1 && events[0].kind == MissionEvent::Kind::DialogueShown &&
              events[0].nodeId == "intro",
          "advance emits DialogueShown(intro)");
    events.clear();
    // Only one choice was offered — index 1 is out of the offered list.
    check(!runtime->advance_dialogue(definition, state, 1, world, events, error),
          "index of a condition-failing choice refused");
    // "intro" is terminal (no choices) — nothing offered.
    check(!runtime->advance_dialogue(definition, state, 0, world, events, error),
          "terminal node refuses advance");

    // With the flag set, the Skip choice is offered and ends the dialogue.
    MissionState state2;
    world.flags["seen_intro"] = true;
    check(runtime->accept(definition, state2, world, events, error), "mission 2 accepted");
    events.clear();
    check(runtime->advance_dialogue(definition, state2, 1, world, events, error),
          "skip choice offered once its condition passes");
    check(state2.dialogueNode.empty(), "skip ends the dialogue");
    check(events.size() == 1 && events[0].nodeId.empty(),
          "ending emits DialogueShown with an empty node");
}

// ---- completion / reward ------------------------------------------------------

void test_complete_and_reward() {
    using namespace engine::gameplay;
    auto runtime = create_mission_runtime();
    MissionDefinition definition = make_mission();
    MockMissionWorld world;
    world.attributes["level"] = 2.0f;
    world.counts["stone"] = 3.0f;
    world.px = 100.0f;
    world.pz = -50.0f;
    MissionState state;
    std::string error;
    std::vector<MissionEvent> events;
    check(runtime->accept(definition, state, world, events, error), "mission accepted");

    // complete() before all objectives are done is refused.
    state.objectiveProgress["collect_stone"] = 1.0f;
    check(!runtime->complete(definition, state, world, events, error),
          "complete refused while objectives are pending");
    state.objectiveProgress["collect_stone"] = 3.0f;
    state.objectiveProgress["reach_hill"] = 1.0f;
    check(runtime->complete(definition, state, world, events, error),
          "complete succeeds once all objectives are done");
    check(state.completed, "state marked completed");
    check(world.rewardItem == "coin" && world.rewardCount == 5 && world.rewardXp == 100,
          "reward item/xp applied through the world");
    check(world.flags["quest1_done"] && world.setFlags.size() == 1,
          "completion flag set through the world");
    bool completedEvent = false;
    bool rewardEvent = false;
    for (const auto& event : events) {
        if (event.kind == MissionEvent::Kind::MissionCompleted) completedEvent = true;
        if (event.kind == MissionEvent::Kind::RewardApplied && event.itemId == "coin") {
            rewardEvent = true;
        }
    }
    check(completedEvent && rewardEvent, "complete emits MissionCompleted + RewardApplied");
    events.clear();
    check(!runtime->complete(definition, state, world, events, error),
          "second complete refused (non-repeatable)");
    check(!runtime->accept(definition, state, world, events, error),
          "accept refused after a non-repeatable completion");

    // Repeatable mission: completion resets and can be accepted again.
    definition.repeatable = true;
    MissionState repeatState;
    check(runtime->accept(definition, repeatState, world, events, error), "repeatable accepted");
    for (const auto& objective : definition.objectives) {
        repeatState.objectiveProgress[objective.id] = static_cast<float>(objective.count);
    }
    check(runtime->complete(definition, repeatState, world, events, error),
          "repeatable mission completes");
    check(!repeatState.completed && !repeatState.accepted,
          "repeatable mission resets for another run");
    check(runtime->accept(definition, repeatState, world, events, error),
          "repeatable mission can be accepted again");
}

// ---- state serialization ------------------------------------------------------

void test_state_serialization() {
    using namespace engine::gameplay;
    auto runtime = create_mission_runtime();
    const MissionDefinition definition = make_mission();
    MockMissionWorld world;
    world.attributes["level"] = 2.0f;
    world.counts["stone"] = 2.0f;
    MissionState state;
    std::string error;
    std::vector<MissionEvent> events;
    check(runtime->accept(definition, state, world, events, error), "mission accepted");
    events.clear();
    check(runtime->update(definition, state, world, events, error), "update ok");
    state.counters["extra"] = 3.5f;
    state.flags["hint_shown"] = true;
    std::string json;
    check(runtime->serialize_state(state, json, error), "state serialized");
    MissionState loaded;
    check(runtime->deserialize_state(json, loaded, error), "state deserialized");
    std::string rejson;
    check(runtime->serialize_state(loaded, rejson, error), "loaded state serialized");
    check(rejson == json, "state round-trips bit-exact");
    check(loaded.accepted && loaded.objectiveProgress["collect_stone"] == 2.0f &&
              loaded.counters["extra"] == 3.5f && loaded.flags["hint_shown"] &&
              loaded.dialogueNode == "start",
          "state fields survive the round-trip");
    // All-or-nothing: malformed documents leave the target untouched.
    MissionState untouched;
    untouched.missionId = "keep";
    check(!runtime->deserialize_state("not json", untouched, error),
          "malformed state refused");
    check(untouched.missionId == "keep", "target untouched on refusal");
    check(!runtime->deserialize_state("{\"accepted\":true}", untouched, error),
          "accepted state without missionId refused");
}

// ---- determinism --------------------------------------------------------------

void test_determinism() {
    using namespace engine::gameplay;
    auto runtimeA = create_mission_runtime();
    auto runtimeB = create_mission_runtime();
    const MissionDefinition definition = make_mission();
    MockMissionWorld worldA;
    MockMissionWorld worldB;
    worldA.attributes["level"] = 2.0f;
    worldB.attributes["level"] = 2.0f;
    MissionState stateA;
    MissionState stateB;
    std::string error;
    std::vector<MissionEvent> eventsA;
    std::vector<MissionEvent> eventsB;
    check(runtimeA->accept(definition, stateA, worldA, eventsA, error), "A accepted");
    check(runtimeB->accept(definition, stateB, worldB, eventsB, error), "B accepted");
    // A deterministic sequence of world changes and updates.
    const std::vector<float> stoneLevels = {0.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f};
    const std::vector<std::pair<float, float>> positions = {
        {0.0f, 0.0f}, {0.0f, 0.0f}, {100.0f, -50.0f}, {100.0f, -50.0f}, {100.0f, -50.0f}, {100.0f, -50.0f}};
    std::string streamA;
    std::string streamB;
    for (std::size_t i = 0; i < stoneLevels.size(); ++i) {
        worldA.counts["stone"] = stoneLevels[i];
        worldB.counts["stone"] = stoneLevels[i];
        worldA.px = positions[i].first;
        worldA.pz = positions[i].second;
        worldB.px = positions[i].first;
        worldB.pz = positions[i].second;
        eventsA.clear();
        eventsB.clear();
        check(runtimeA->update(definition, stateA, worldA, eventsA, error), "A update");
        check(runtimeB->update(definition, stateB, worldB, eventsB, error), "B update");
        for (const auto& event : eventsA) {
            streamA += std::to_string(static_cast<int>(event.kind)) + "," +
                       event.objectiveId + "," + event.nodeId + ";";
        }
        for (const auto& event : eventsB) {
            streamB += std::to_string(static_cast<int>(event.kind)) + "," +
                       event.objectiveId + "," + event.nodeId + ";";
        }
    }
    check(streamA == streamB, "bit-identical event streams across instances");
    std::string jsonA;
    std::string jsonB;
    check(runtimeA->serialize_state(stateA, jsonA, error), "A serialized");
    check(runtimeB->serialize_state(stateB, jsonB, error), "B serialized");
    check(jsonA == jsonB, "bit-identical final states across instances");
}

}  // namespace

int main() {
    test_definition_validation();
    test_json_round_trip();
    test_accept_and_unlock();
    test_objective_progress();
    test_objective_gating();
    test_dialogue_advance();
    test_complete_and_reward();
    test_state_serialization();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[mission-asset] ALL PASSED\n");
        return 0;
    }
    std::printf("[mission-asset] %d FAILURE(S)\n", g_failures);
    return 1;
}

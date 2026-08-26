#pragma once

// IMissionAsset (FALTANTES item 23 — "criar scripts, abilities, animações,
// missões e diálogos"): the PUBLIC mission/dialogue contract. A mission is a
// data-driven asset (versioned JSON, all-or-nothing, bit-exact round-trip —
// the AbilityDefinition/VehicleAsset pattern) with OBJECTIVES (reach/collect/
// kill/interact, all AND-ed), a DIALOGUE GRAPH (nodes with speaker/text and
// condition-gated choices), UNLOCK CONDITIONS (flags/counters/attributes),
// a REWARD (item + xp + flag) and repeatability.
//
// The runtime applies the definition through the minimal IMissionWorld seam
// (counters, flags, attributes, position, reward application) — no core code
// needs to change to add a mission. Progress is caller-owned and explicit
// (MissionState: objective progress, runtime counters/flags, the current
// dialogue node); every decision is reported as MissionEvents the project
// applies (dialogue shown, objective completed, mission completed, reward
// applied). Deterministic: the same (definition, state, world) sequence
// reproduces bit-exactly, and the state serializes bit-exactly (%.9g) for
// saves and replication.
//
// Dialogue semantics: the entry node is "start". A node's choices are shown
// only when ALL of the choice's conditions pass (the runtime filters). An
// empty choices list ends the dialogue (a terminal node). advance_dialogue
// moves to the choice's `next` ("" = end). The dialogue is optional (a
// mission may have none).
//
// This header is self-contained (std + glm). load_from_json / to_json /
// validate of the definition and serialize_state / deserialize_state of the
// state are implemented by the SDK adapter (src/engine/sdk/MissionAsset.cpp).

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

// ---- conditions --------------------------------------------------------------

// Condition kinds, evaluated against the world seam and/or the mission state.
// All conditions in a list must pass (AND).
enum class MissionConditionKind : std::uint8_t {
    Flag,           // world.flag(key) == flagValue
    Counter,        // world.count_of(key) <op> value
    ObjectiveDone,  // state objective `key` reached its target
    Attribute,      // world.attribute(key) <op> value
};

struct MissionCondition {
    MissionConditionKind kind{ MissionConditionKind::Flag };
    // Flag/Counter/Attribute: the world key. ObjectiveDone: the objective id.
    std::string key;
    // Comparison for Counter/Attribute: "==" | "!=" | ">=" | "<=" | ">" | "<".
    std::string op{ ">=" };
    // Threshold for Counter/Attribute.
    float value{ 0.0f };
    // Expected flag value for Flag.
    bool flagValue{ true };
};

// ---- objectives --------------------------------------------------------------

enum class MissionObjectiveKind : std::uint8_t {
    Reach,     // player within `radius` of (x, z)
    Collect,   // world.count_of(target) >= count (items)
    Kill,      // world.count_of(target) >= count (entities)
    Interact,  // world.count_of(target) >= count (interactions)
};

struct MissionObjective {
    // Unique within the mission ("collect_stone", "reach_hill").
    std::string id;
    MissionObjectiveKind kind{ MissionObjectiveKind::Collect };
    // Item/entity/block id for Collect/Kill/Interact ("" for Reach).
    std::string target;
    // Required progress. 1 for Reach/Interact.
    int count{ 1 };
    // Reach target point and radius (radius >= 0; 0 = exact cell).
    float x{ 0.0f };
    float z{ 0.0f };
    float radius{ 0.0f };
    // Optional extra gates on this objective's progress (AND).
    std::vector<MissionCondition> conditions;
};

// ---- dialogue ----------------------------------------------------------------

struct DialogueChoice {
    std::string text;   // choice label shown to the player
    std::string next;   // next node id ("" = end the dialogue)
    // All must pass for the choice to be offered (AND).
    std::vector<MissionCondition> conditions;
};

struct DialogueNode {
    std::string id;      // unique within the mission; "start" = entry node
    std::string speaker; // speaker name
    std::string text;    // line text
    // Choices offered (empty = terminal node, the dialogue ends). The runtime
    // only offers choices whose conditions pass.
    std::vector<DialogueChoice> choices;
};

// ---- reward ------------------------------------------------------------------

struct MissionReward {
    std::string itemId;  // "" = no item
    int count{ 0 };      // >= 0
    int xp{ 0 };         // >= 0
    std::string setFlag; // flag set on the world at completion ("" = none)
};

// ---- the definition (pure data) ----------------------------------------------

struct MissionDefinition {
    // Stable project id; derived from "missions:<name>" when omitted.
    std::string id;
    std::string name;
    int version{ 1 };

    // ALL objectives must reach their target for the mission to complete.
    std::vector<MissionObjective> objectives;
    // Dialogue graph; "start" is the entry node (may be empty = no dialogue).
    std::vector<DialogueNode> dialogue;
    // ALL must pass (world seam) for the mission to be accepted.
    std::vector<MissionCondition> unlockConditions;
    MissionReward reward;
    bool repeatable{ false };

    // All-or-nothing: refuses malformed documents with a diagnostic (never
    // clamps or partially applies). Implemented by the SDK adapter.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    // Bit-exact: to_json() round-trips every field (%.9g float emission).
    std::string to_json() const;
    bool validate(std::string& errorOut) const;
};

// ---- world seam --------------------------------------------------------------

// The minimal world a mission runtime needs. The project implements this over
// its own world (entities/items/attributes); the runtime never couples to a
// concrete world type. Every method is deterministic.
class IMissionWorld {
public:
    virtual ~IMissionWorld() = default;

    // Progress source for Collect/Kill/Interact (e.g. "stone" -> collected).
    // Returns 0 for unknown keys.
    virtual float count_of(const std::string& key) const = 0;
    // World flags (e.g. story flags). Unknown keys are false.
    virtual bool flag(const std::string& key) const = 0;
    // Player/world attributes (e.g. "level"). Unknown keys are 0.
    virtual float attribute(const std::string& key) const = 0;
    // Player position (Reach objectives). Returns false when unavailable.
    virtual bool position(float& x, float& z) const = 0;
    // Applies the reward item + xp. Returns false when refused.
    virtual bool apply_reward(const std::string& itemId, int count, int xp) = 0;
    // Sets a world flag. Returns false when refused.
    virtual bool set_flag(const std::string& key) = 0;
};

// ---- runtime state -----------------------------------------------------------

// Caller-owned mission progress (explicit, never hidden in the adapter).
struct MissionState {
    std::string missionId;
    bool accepted{ false };
    bool completed{ false };
    // Objective id -> progress (0..target). Rebuilt from the world each
    // update for Collect/Kill/Interact; set directly by Reach.
    std::map<std::string, float> objectiveProgress;
    // Runtime counters/flags the project may advance (choice conditions can
    // gate on them). Persisted bit-exactly.
    std::map<std::string, float> counters;
    std::map<std::string, bool> flags;
    // Current dialogue node id ("" = no dialogue active).
    std::string dialogueNode;
};

// Decisions the runtime reports (the project applies them).
struct MissionEvent {
    enum class Kind : std::uint8_t {
        Accepted,            // the mission started (entry dialogue shown)
        ObjectiveCompleted,  // an objective reached its target
        DialogueShown,       // a dialogue node was shown
        MissionCompleted,    // all objectives done (complete() applies the reward)
        RewardApplied,       // the reward was applied
    };
    Kind kind{ Kind::Accepted };
    std::string objectiveId;  // ObjectiveCompleted
    std::string nodeId;       // DialogueShown
    std::string itemId;       // RewardApplied
    int count{ 0 };           // RewardApplied item count
    int xp{ 0 };              // RewardApplied xp
};

// ---- runtime -----------------------------------------------------------------

class IMissionRuntime {
public:
    virtual ~IMissionRuntime() = default;

    // True when ALL unlock conditions pass for this mission against the
    // current world/state (all-or-nothing: any failure -> false).
    virtual bool can_accept(const MissionDefinition& definition,
                            const MissionState& state, IMissionWorld& world,
                            std::string& errorOut) const = 0;

    // Starts the mission: sets accepted, begins the dialogue at "start" (when
    // the mission has one; emits DialogueShown). Refuses when already accepted
    // (or completed and not repeatable) or when unlock conditions fail.
    virtual bool accept(const MissionDefinition& definition, MissionState& state,
                        IMissionWorld& world, std::vector<MissionEvent>& events,
                        std::string& errorOut) = 0;

    // Advances objective progress from the world (Collect/Kill/Interact read
    // count_of; Reach reads position) and fires ObjectiveCompleted for
    // objectives that crossed their target. When ALL objectives are done the
    // mission is complete (emits MissionCompleted; complete() applies the
    // reward). Deterministic. Refuses an unaccepted/completed mission.
    virtual bool update(const MissionDefinition& definition, MissionState& state,
                        IMissionWorld& world, std::vector<MissionEvent>& events,
                        std::string& errorOut) = 0;

    // Moves the dialogue from the current node to the selected choice's
    // `next`. The choice index is among the OFFERED (condition-passing)
    // choices (conditions evaluate against `world` + the state); an index
    // outside the offered list is refused. Emits DialogueShown for the
    // target node ("" = dialogue ended).
    virtual bool advance_dialogue(const MissionDefinition& definition,
                                  MissionState& state, std::size_t choiceIndex,
                                  IMissionWorld& world,
                                  std::vector<MissionEvent>& events,
                                  std::string& errorOut) = 0;

    // Completes a mission whose objectives are ALL done: applies the reward
    // (world.apply_reward + world.set_flag) and marks it completed (unless
    // repeatable, in which case the state resets for another run). Refuses an
    // incomplete mission, an unaccepted mission, or a completed non-repeatable
    // mission. Emits MissionCompleted + RewardApplied.
    virtual bool complete(const MissionDefinition& definition, MissionState& state,
                          IMissionWorld& world, std::vector<MissionEvent>& events,
                          std::string& errorOut) = 0;

    // Bit-exact state persistence (%.9g). deserialize is all-or-nothing.
    virtual bool serialize_state(const MissionState& state, std::string& out,
                                 std::string& errorOut) const = 0;
    virtual bool deserialize_state(const std::string& data, MissionState& out,
                                   std::string& errorOut) const = 0;
};

// The only implementation (src/engine/sdk/MissionAsset.cpp).
std::unique_ptr<IMissionRuntime> create_mission_runtime();

}  // namespace gameplay
}  // namespace engine

// MissionAsset.cpp — the only TU implementing the public mission/dialogue
// contract (FALTANTES item 23 — "missões e diálogos"). MissionDefinition is
// PURE DATA (versioned JSON, all-or-nothing, bit-exact %.9g round-trip — the
// AbilityDefinition/VehicleAsset pattern); the runtime applies it through the
// IMissionWorld seam (counters, flags, attributes, position, reward
// application) and reports decisions as MissionEvents. Progress is
// caller-owned and explicit (MissionState); the runtime is deterministic —
// the same (definition, state, world) sequence reproduces bit-exactly, and
// the state serializes bit-exactly for saves and replication.
//
// Numeric validation uses BIT-LEVEL finite checks: the project compiles with
// /fp:fast (findings #79), which folds std::isfinite(NaN) to true.

#include "engine/gameplay/IMissionAsset.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

namespace engine {
namespace gameplay {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

const char* condition_kind_name(MissionConditionKind kind) {
    switch (kind) {
        case MissionConditionKind::Flag: return "flag";
        case MissionConditionKind::Counter: return "counter";
        case MissionConditionKind::ObjectiveDone: return "objectiveDone";
        case MissionConditionKind::Attribute: break;
    }
    return "attribute";
}

bool parse_condition_kind(const std::string& name, MissionConditionKind& out) {
    if (name == "flag") { out = MissionConditionKind::Flag; return true; }
    if (name == "counter") { out = MissionConditionKind::Counter; return true; }
    if (name == "objectiveDone") { out = MissionConditionKind::ObjectiveDone; return true; }
    if (name == "attribute") { out = MissionConditionKind::Attribute; return true; }
    return false;
}

const char* objective_kind_name(MissionObjectiveKind kind) {
    switch (kind) {
        case MissionObjectiveKind::Reach: return "reach";
        case MissionObjectiveKind::Collect: return "collect";
        case MissionObjectiveKind::Kill: return "kill";
        case MissionObjectiveKind::Interact: break;
    }
    return "interact";
}

bool parse_objective_kind(const std::string& name, MissionObjectiveKind& out) {
    if (name == "reach") { out = MissionObjectiveKind::Reach; return true; }
    if (name == "collect") { out = MissionObjectiveKind::Collect; return true; }
    if (name == "kill") { out = MissionObjectiveKind::Kill; return true; }
    if (name == "interact") { out = MissionObjectiveKind::Interact; return true; }
    return false;
}

const std::set<std::string>& valid_ops() {
    static const std::set<std::string> ops = {"==", "!=", ">=", "<=", ">", "<"};
    return ops;
}

// ---- validation --------------------------------------------------------------

bool validate_condition(const MissionCondition& condition, const MissionDefinition& definition,
                        std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "mission asset condition: " + message;
        return false;
    };
    if (condition.key.empty()) return fail("key must not be empty");
    if (condition.kind == MissionConditionKind::Counter ||
        condition.kind == MissionConditionKind::Attribute) {
        if (!valid_ops().count(condition.op)) {
            return fail("op must be ==|!=|>=|<=|>|< (got '" + condition.op + "')");
        }
        if (!finite_float(condition.value)) return fail("value must be finite");
    }
    if (condition.kind == MissionConditionKind::ObjectiveDone) {
        bool found = false;
        for (const MissionObjective& objective : definition.objectives) {
            if (objective.id == condition.key) { found = true; break; }
        }
        if (!found) return fail("objectiveDone references unknown objective '" + condition.key + "'");
    }
    return true;
}

bool validate_definition(const MissionDefinition& definition, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "mission asset: " + message;
        return false;
    };
    if (definition.name.empty()) return fail("name must not be empty");
    if (definition.version != 1) return fail("unsupported version");
    if (definition.objectives.empty()) return fail("at least one objective is required");
    std::set<std::string> objectiveIds;
    for (const MissionObjective& objective : definition.objectives) {
        if (objective.id.empty()) return fail("objective id must not be empty");
        if (!objectiveIds.insert(objective.id).second) {
            return fail("duplicate objective id '" + objective.id + "'");
        }
        if (objective.kind != MissionObjectiveKind::Reach && objective.target.empty()) {
            return fail("objective '" + objective.id + "' needs a target");
        }
        if (objective.count < 1) return fail("objective '" + objective.id + "' count must be >= 1");
        if (!finite_float(objective.x) || !finite_float(objective.z)) {
            return fail("objective '" + objective.id + "' position must be finite");
        }
        if (!finite_float(objective.radius) || objective.radius < 0.0f) {
            return fail("objective '" + objective.id + "' radius must be finite and >= 0");
        }
        for (const MissionCondition& condition : objective.conditions) {
            if (!validate_condition(condition, definition, errorOut)) return false;
        }
    }
    std::set<std::string> nodeIds;
    for (const DialogueNode& node : definition.dialogue) {
        if (node.id.empty()) return fail("dialogue node id must not be empty");
        if (!nodeIds.insert(node.id).second) {
            return fail("duplicate dialogue node id '" + node.id + "'");
        }
    }
    if (!definition.dialogue.empty() && !nodeIds.count("start")) {
        return fail("dialogue must declare a 'start' node");
    }
    for (const DialogueNode& node : definition.dialogue) {
        for (const DialogueChoice& choice : node.choices) {
            if (choice.text.empty()) return fail("dialogue choice text must not be empty");
            if (!choice.next.empty() && !nodeIds.count(choice.next)) {
                return fail("dialogue choice 'next' references unknown node '" + choice.next + "'");
            }
            for (const MissionCondition& condition : choice.conditions) {
                if (!validate_condition(condition, definition, errorOut)) return false;
            }
        }
    }
    for (const MissionCondition& condition : definition.unlockConditions) {
        if (!validate_condition(condition, definition, errorOut)) return false;
    }
    if (definition.reward.count < 0) return fail("reward count must be >= 0");
    if (definition.reward.xp < 0) return fail("reward xp must be >= 0");
    return true;
}

bool validate_state(const MissionState& state, std::string& errorOut) {
    if (state.missionId.empty() && (state.accepted || state.completed)) {
        errorOut = "mission state: accepted/completed state needs a missionId";
        return false;
    }
    for (const auto& entry : state.objectiveProgress) {
        if (!finite_float(entry.second)) {
            errorOut = "mission state: non-finite objective progress";
            return false;
        }
    }
    for (const auto& entry : state.counters) {
        if (!finite_float(entry.second)) {
            errorOut = "mission state: non-finite counter";
            return false;
        }
    }
    return true;
}

// ---- emitters ----------------------------------------------------------------

std::string emit_condition(const MissionCondition& condition) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"kind\":\"" << condition_kind_name(condition.kind)
        << "\",\"key\":\"" << json_escape(condition.key)
        << "\",\"op\":\"" << json_escape(condition.op)
        << "\",\"value\":" << condition.value
        << ",\"flagValue\":" << (condition.flagValue ? "true" : "false") << '}';
    return out.str();
}

std::string emit_conditions(const std::vector<MissionCondition>& conditions) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < conditions.size(); ++i) {
        if (i != 0) out << ',';
        out << emit_condition(conditions[i]);
    }
    out << ']';
    return out.str();
}

std::string emit_objective(const MissionObjective& objective) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"id\":\"" << json_escape(objective.id)
        << "\",\"kind\":\"" << objective_kind_name(objective.kind)
        << "\",\"target\":\"" << json_escape(objective.target)
        << "\",\"count\":" << objective.count
        << ",\"x\":" << objective.x
        << ",\"z\":" << objective.z
        << ",\"radius\":" << objective.radius
        << ",\"conditions\":" << emit_conditions(objective.conditions) << '}';
    return out.str();
}

std::string emit_choice(const DialogueChoice& choice) {
    std::ostringstream out;
    out << "{\"text\":\"" << json_escape(choice.text)
        << "\",\"next\":\"" << json_escape(choice.next)
        << "\",\"conditions\":" << emit_conditions(choice.conditions) << '}';
    return out.str();
}

std::string emit_node(const DialogueNode& node) {
    std::ostringstream out;
    out << "{\"id\":\"" << json_escape(node.id)
        << "\",\"speaker\":\"" << json_escape(node.speaker)
        << "\",\"text\":\"" << json_escape(node.text)
        << "\",\"choices\":[";
    for (std::size_t i = 0; i < node.choices.size(); ++i) {
        if (i != 0) out << ',';
        out << emit_choice(node.choices[i]);
    }
    out << "]}";
    return out.str();
}

std::string emit_reward(const MissionReward& reward) {
    std::ostringstream out;
    out << "{\"itemId\":\"" << json_escape(reward.itemId)
        << "\",\"count\":" << reward.count
        << ",\"xp\":" << reward.xp
        << ",\"setFlag\":\"" << json_escape(reward.setFlag) << "\"}";
    return out.str();
}

// ---- readers ----------------------------------------------------------------

bool read_condition(const sdk::JsonValue& value, MissionCondition& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "mission asset: each condition must be an object";
        return false;
    }
    MissionCondition condition;
    const std::string kind = sdk::json_string(value, "kind", "");
    if (!parse_condition_kind(kind, condition.kind)) {
        errorOut = "mission asset: unknown condition kind '" + kind + "'";
        return false;
    }
    condition.key = sdk::json_string(value, "key", "");
    condition.op = sdk::json_string(value, "op", ">=");
    condition.value = static_cast<float>(sdk::json_number(value, "value", 0.0));
    condition.flagValue = sdk::json_bool(value, "flagValue", true);
    out = std::move(condition);
    return true;
}

bool read_conditions(const sdk::JsonValue& value, std::vector<MissionCondition>& out,
                     std::string& errorOut) {
    if (value.kind != sdk::JsonValue::Kind::Array) {
        errorOut = "mission asset: conditions must be an array";
        return false;
    }
    out.reserve(value.array.size());
    for (const sdk::JsonValue& item : value.array) {
        MissionCondition condition;
        if (!read_condition(item, condition, errorOut)) return false;
        out.push_back(std::move(condition));
    }
    return true;
}

bool read_objective(const sdk::JsonValue& value, MissionObjective& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "mission asset: each objective must be an object";
        return false;
    }
    MissionObjective objective;
    objective.id = sdk::json_string(value, "id", "");
    const std::string kind = sdk::json_string(value, "kind", "collect");
    if (!parse_objective_kind(kind, objective.kind)) {
        errorOut = "mission asset: unknown objective kind '" + kind + "'";
        return false;
    }
    objective.target = sdk::json_string(value, "target", "");
    objective.count = static_cast<int>(sdk::json_number(value, "count", 1.0));
    objective.x = static_cast<float>(sdk::json_number(value, "x", 0.0));
    objective.z = static_cast<float>(sdk::json_number(value, "z", 0.0));
    objective.radius = static_cast<float>(sdk::json_number(value, "radius", 0.0));
    const sdk::JsonValue* conditionsValue = value.field("conditions");
    if (conditionsValue != nullptr && !read_conditions(*conditionsValue, objective.conditions, errorOut)) {
        return false;
    }
    out = std::move(objective);
    return true;
}

bool read_choice(const sdk::JsonValue& value, DialogueChoice& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "mission asset: each dialogue choice must be an object";
        return false;
    }
    DialogueChoice choice;
    choice.text = sdk::json_string(value, "text", "");
    choice.next = sdk::json_string(value, "next", "");
    const sdk::JsonValue* conditionsValue = value.field("conditions");
    if (conditionsValue != nullptr && !read_conditions(*conditionsValue, choice.conditions, errorOut)) {
        return false;
    }
    out = std::move(choice);
    return true;
}

bool read_node(const sdk::JsonValue& value, DialogueNode& out, std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "mission asset: each dialogue node must be an object";
        return false;
    }
    DialogueNode node;
    node.id = sdk::json_string(value, "id", "");
    node.speaker = sdk::json_string(value, "speaker", "");
    node.text = sdk::json_string(value, "text", "");
    const sdk::JsonValue* choicesValue = value.field("choices");
    if (choicesValue != nullptr) {
        if (choicesValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "mission asset: dialogue choices must be an array";
            return false;
        }
        node.choices.reserve(choicesValue->array.size());
        for (const sdk::JsonValue& item : choicesValue->array) {
            DialogueChoice choice;
            if (!read_choice(item, choice, errorOut)) return false;
            node.choices.push_back(std::move(choice));
        }
    }
    out = std::move(node);
    return true;
}

bool read_reward(const sdk::JsonValue& value, MissionReward& out) {
    MissionReward reward;
    reward.itemId = sdk::json_string(value, "itemId", "");
    reward.count = static_cast<int>(sdk::json_number(value, "count", 0.0));
    reward.xp = static_cast<int>(sdk::json_number(value, "xp", 0.0));
    reward.setFlag = sdk::json_string(value, "setFlag", "");
    out = std::move(reward);
    return true;
}

// ---- condition evaluation ----------------------------------------------------

bool compare_float(float a, const std::string& op, float b) {
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == "<") return a < b;
    return false;
}

bool eval_condition(const MissionCondition& condition, const MissionDefinition& definition,
                    const MissionState& state, IMissionWorld& world) {
    switch (condition.kind) {
        case MissionConditionKind::Flag:
            return world.flag(condition.key) == condition.flagValue;
        case MissionConditionKind::Counter:
            return compare_float(world.count_of(condition.key), condition.op, condition.value);
        case MissionConditionKind::Attribute:
            return compare_float(world.attribute(condition.key), condition.op, condition.value);
        case MissionConditionKind::ObjectiveDone: {
            for (const MissionObjective& objective : definition.objectives) {
                if (objective.id == condition.key) {
                    const auto it = state.objectiveProgress.find(objective.id);
                    return it != state.objectiveProgress.end() &&
                           it->second >= static_cast<float>(objective.count);
                }
            }
            return false;
        }
    }
    return false;
}

bool eval_conditions(const std::vector<MissionCondition>& conditions,
                     const MissionDefinition& definition, const MissionState& state,
                     IMissionWorld& world) {
    for (const MissionCondition& condition : conditions) {
        if (!eval_condition(condition, definition, state, world)) return false;
    }
    return true;
}

const DialogueNode* find_node(const MissionDefinition& definition, const std::string& id) {
    for (const DialogueNode& node : definition.dialogue) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

// ---- the runtime --------------------------------------------------------------

class MissionRuntime final : public IMissionRuntime {
public:
    bool can_accept(const MissionDefinition& definition, const MissionState& state,
                    IMissionWorld& world, std::string& errorOut) const override {
        if (!definition.validate(errorOut)) return false;
        return eval_conditions(definition.unlockConditions, definition, state, world);
    }

    bool accept(const MissionDefinition& definition, MissionState& state, IMissionWorld& world,
                std::vector<MissionEvent>& events, std::string& errorOut) override {
        if (!validate_definition(definition, errorOut)) return false;
        if (!validate_state(state, errorOut)) return false;
        if (state.accepted) {
            errorOut = "mission: already accepted";
            return false;
        }
        if (state.completed && !definition.repeatable) {
            errorOut = "mission: already completed and not repeatable";
            return false;
        }
        if (!eval_conditions(definition.unlockConditions, definition, state, world)) {
            errorOut = "mission: unlock conditions not met";
            return false;
        }
        events.clear();
        state.missionId = definition.id;
        state.accepted = true;
        state.completed = false;
        state.objectiveProgress.clear();
        for (const MissionObjective& objective : definition.objectives) {
            state.objectiveProgress[objective.id] = 0.0f;
        }
        state.dialogueNode = "";
        MissionEvent accepted;
        accepted.kind = MissionEvent::Kind::Accepted;
        events.push_back(std::move(accepted));
        if (!definition.dialogue.empty()) {
            state.dialogueNode = "start";
            MissionEvent shown;
            shown.kind = MissionEvent::Kind::DialogueShown;
            shown.nodeId = "start";
            events.push_back(std::move(shown));
        }
        return true;
    }

    bool update(const MissionDefinition& definition, MissionState& state, IMissionWorld& world,
                std::vector<MissionEvent>& events, std::string& errorOut) override {
        if (!validate_definition(definition, errorOut)) return false;
        if (!validate_state(state, errorOut)) return false;
        if (!state.accepted) {
            errorOut = "mission: not accepted";
            return false;
        }
        if (state.completed && !definition.repeatable) {
            errorOut = "mission: already completed";
            return false;
        }
        events.clear();
        bool allDone = true;
        for (const MissionObjective& objective : definition.objectives) {
            const auto progressIt = state.objectiveProgress.find(objective.id);
            const float current = progressIt != state.objectiveProgress.end()
                                      ? progressIt->second
                                      : 0.0f;
            if (current >= static_cast<float>(objective.count)) continue;
            const bool gated = !eval_conditions(objective.conditions, definition, state, world);
            float progress = 0.0f;
            if (!gated) {
                if (objective.kind == MissionObjectiveKind::Reach) {
                    float px = 0.0f, pz = 0.0f;
                    if (world.position(px, pz)) {
                        const float dx = px - objective.x;
                        const float dz = pz - objective.z;
                        const float distance = std::sqrt(dx * dx + dz * dz);
                        if (distance <= objective.radius) progress = static_cast<float>(objective.count);
                    }
                } else {
                    progress = world.count_of(objective.target);
                }
                progress = std::min(progress, static_cast<float>(objective.count));
            }
            state.objectiveProgress[objective.id] = progress;
            if (progress >= static_cast<float>(objective.count)) {
                MissionEvent completed;
                completed.kind = MissionEvent::Kind::ObjectiveCompleted;
                completed.objectiveId = objective.id;
                events.push_back(std::move(completed));
            }
        }
        for (const MissionObjective& objective : definition.objectives) {
            const auto it = state.objectiveProgress.find(objective.id);
            const float progress = it != state.objectiveProgress.end() ? it->second : 0.0f;
            if (progress < static_cast<float>(objective.count)) {
                allDone = false;
                break;
            }
        }
        if (allDone) {
            MissionEvent completed;
            completed.kind = MissionEvent::Kind::MissionCompleted;
            events.push_back(std::move(completed));
        }
        return true;
    }

    bool advance_dialogue(const MissionDefinition& definition, MissionState& state,
                          std::size_t choiceIndex, IMissionWorld& world,
                          std::vector<MissionEvent>& events,
                          std::string& errorOut) override {
        if (!validate_definition(definition, errorOut)) return false;
        if (!validate_state(state, errorOut)) return false;
        if (!state.accepted) {
            errorOut = "mission: not accepted";
            return false;
        }
        if (state.dialogueNode.empty()) {
            errorOut = "mission: no dialogue active";
            return false;
        }
        const DialogueNode* node = find_node(definition, state.dialogueNode);
        if (node == nullptr) {
            errorOut = "mission: current dialogue node not in the definition";
            return false;
        }
        // Offer only the choices whose conditions pass.
        std::vector<std::size_t> offered;
        for (std::size_t i = 0; i < node->choices.size(); ++i) {
            if (eval_conditions(node->choices[i].conditions, definition, state, world)) {
                offered.push_back(i);
            }
        }
        if (choiceIndex >= offered.size()) {
            errorOut = "mission: choice index out of the offered choices";
            return false;
        }
        events.clear();
        const DialogueChoice& choice = node->choices[offered[choiceIndex]];
        if (choice.next.empty()) {
            state.dialogueNode = "";
            MissionEvent shown;
            shown.kind = MissionEvent::Kind::DialogueShown;
            shown.nodeId = "";
            events.push_back(std::move(shown));
        } else {
            state.dialogueNode = choice.next;
            MissionEvent shown;
            shown.kind = MissionEvent::Kind::DialogueShown;
            shown.nodeId = choice.next;
            events.push_back(std::move(shown));
        }
        return true;
    }

    bool complete(const MissionDefinition& definition, MissionState& state, IMissionWorld& world,
                  std::vector<MissionEvent>& events, std::string& errorOut) override {
        if (!validate_definition(definition, errorOut)) return false;
        if (!validate_state(state, errorOut)) return false;
        if (!state.accepted) {
            errorOut = "mission: not accepted";
            return false;
        }
        if (state.completed && !definition.repeatable) {
            errorOut = "mission: already completed";
            return false;
        }
        for (const MissionObjective& objective : definition.objectives) {
            const auto it = state.objectiveProgress.find(objective.id);
            const float progress = it != state.objectiveProgress.end() ? it->second : 0.0f;
            if (progress < static_cast<float>(objective.count)) {
                errorOut = "mission: objectives not all complete";
                return false;
            }
        }
        events.clear();
        if (!world.apply_reward(definition.reward.itemId, definition.reward.count,
                                definition.reward.xp)) {
            errorOut = "mission: reward refused by the world";
            return false;
        }
        if (!definition.reward.setFlag.empty() && !world.set_flag(definition.reward.setFlag)) {
            errorOut = "mission: flag refused by the world";
            return false;
        }
        MissionEvent completed;
        completed.kind = MissionEvent::Kind::MissionCompleted;
        events.push_back(std::move(completed));
        MissionEvent rewarded;
        rewarded.kind = MissionEvent::Kind::RewardApplied;
        rewarded.itemId = definition.reward.itemId;
        rewarded.count = definition.reward.count;
        rewarded.xp = definition.reward.xp;
        events.push_back(std::move(rewarded));
        if (definition.repeatable) {
            // Reset for another run (the reward was already applied).
            state.accepted = false;
            state.completed = false;
            state.objectiveProgress.clear();
            state.dialogueNode = "";
        } else {
            state.completed = true;
            state.dialogueNode = "";
        }
        return true;
    }

    bool serialize_state(const MissionState& state, std::string& out,
                         std::string& errorOut) const override {
        if (!validate_state(state, errorOut)) return false;
        std::ostringstream stream;
        stream << std::setprecision(9);
        stream << "{\"missionId\":\"" << json_escape(state.missionId)
               << "\",\"accepted\":" << (state.accepted ? "true" : "false")
               << ",\"completed\":" << (state.completed ? "true" : "false")
               << ",\"objectiveProgress\":{";
        bool first = true;
        for (const auto& entry : state.objectiveProgress) {
            if (!first) stream << ',';
            first = false;
            stream << '"' << json_escape(entry.first) << "\":" << entry.second;
        }
        stream << "},\"counters\":{";
        first = true;
        for (const auto& entry : state.counters) {
            if (!first) stream << ',';
            first = false;
            stream << '"' << json_escape(entry.first) << "\":" << entry.second;
        }
        stream << "},\"flags\":{";
        first = true;
        for (const auto& entry : state.flags) {
            if (!first) stream << ',';
            first = false;
            stream << '"' << json_escape(entry.first) << "\":"
                   << (entry.second ? "true" : "false");
        }
        stream << "},\"dialogueNode\":\"" << json_escape(state.dialogueNode) << "\"}";
        out = stream.str();
        return true;
    }

    bool deserialize_state(const std::string& data, MissionState& out,
                           std::string& errorOut) const override {
        sdk::JsonValue root;
        if (!sdk::json_parse(data, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "mission state: root must be an object";
            return false;
        }
        MissionState state;
        state.missionId = sdk::json_string(root, "missionId", "");
        state.accepted = sdk::json_bool(root, "accepted", false);
        state.completed = sdk::json_bool(root, "completed", false);
        state.dialogueNode = sdk::json_string(root, "dialogueNode", "");
        const sdk::JsonValue* progressValue = root.field("objectiveProgress");
        if (progressValue != nullptr) {
            if (!progressValue->is_object()) {
                errorOut = "mission state: objectiveProgress must be an object";
                return false;
            }
            for (const auto& entry : progressValue->object) {
                state.objectiveProgress[entry.first] =
                    static_cast<float>(entry.second.number);
            }
        }
        const sdk::JsonValue* countersValue = root.field("counters");
        if (countersValue != nullptr) {
            if (!countersValue->is_object()) {
                errorOut = "mission state: counters must be an object";
                return false;
            }
            for (const auto& entry : countersValue->object) {
                state.counters[entry.first] = static_cast<float>(entry.second.number);
            }
        }
        const sdk::JsonValue* flagsValue = root.field("flags");
        if (flagsValue != nullptr) {
            if (!flagsValue->is_object()) {
                errorOut = "mission state: flags must be an object";
                return false;
            }
            for (const auto& entry : flagsValue->object) {
                state.flags[entry.first] = entry.second.boolean;
            }
        }
        if (!validate_state(state, errorOut)) return false;
        out = std::move(state);
        return true;
    }

private:
};

}  // namespace

// ---- definition contract methods (SDK) ----------------------------------------

bool MissionDefinition::validate(std::string& errorOut) const {
    return validate_definition(*this, errorOut);
}

bool MissionDefinition::load_from_json(const std::string& jsonText, std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "mission asset: root must be an object";
        return false;
    }
    MissionDefinition parsed;
    parsed.name = sdk::json_string(root, "name", "");
    parsed.version = static_cast<int>(sdk::json_number(root, "version", 1));
    const std::string id = sdk::json_string(root, "id", "");
    parsed.id = sdk::uuid_or_derived(id, "missions:" + parsed.name);
    const sdk::JsonValue* objectivesValue = root.field("objectives");
    if (objectivesValue == nullptr || objectivesValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "mission asset: objectives must be an array";
        return false;
    }
    parsed.objectives.reserve(objectivesValue->array.size());
    for (const sdk::JsonValue& item : objectivesValue->array) {
        MissionObjective objective;
        if (!read_objective(item, objective, errorOut)) return false;
        parsed.objectives.push_back(std::move(objective));
    }
    const sdk::JsonValue* dialogueValue = root.field("dialogue");
    if (dialogueValue != nullptr) {
        if (dialogueValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "mission asset: dialogue must be an array";
            return false;
        }
        parsed.dialogue.reserve(dialogueValue->array.size());
        for (const sdk::JsonValue& item : dialogueValue->array) {
            DialogueNode node;
            if (!read_node(item, node, errorOut)) return false;
            parsed.dialogue.push_back(std::move(node));
        }
    }
    const sdk::JsonValue* unlockValue = root.field("unlockConditions");
    if (unlockValue != nullptr && !read_conditions(*unlockValue, parsed.unlockConditions, errorOut)) {
        return false;
    }
    const sdk::JsonValue* rewardValue = root.field("reward");
    if (rewardValue != nullptr) {
        if (!rewardValue->is_object()) {
            errorOut = "mission asset: reward must be an object";
            return false;
        }
        if (!read_reward(*rewardValue, parsed.reward)) return false;
    }
    parsed.repeatable = sdk::json_bool(root, "repeatable", false);
    if (!validate_definition(parsed, errorOut)) return false;
    *this = std::move(parsed);
    return true;
}

std::string MissionDefinition::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"name\":\"" << json_escape(name)
        << "\",\"version\":" << version;
    if (!id.empty()) {
        out << ",\"id\":\"" << json_escape(id) << '"';
    }
    out << ",\"objectives\":[";
    for (std::size_t i = 0; i < objectives.size(); ++i) {
        if (i != 0) out << ',';
        out << emit_objective(objectives[i]);
    }
    out << "],\"dialogue\":[";
    for (std::size_t i = 0; i < dialogue.size(); ++i) {
        if (i != 0) out << ',';
        out << emit_node(dialogue[i]);
    }
    out << "],\"unlockConditions\":" << emit_conditions(unlockConditions)
        << ",\"reward\":" << emit_reward(reward)
        << ",\"repeatable\":" << (repeatable ? "true" : "false") << '}';
    return out.str();
}

}  // namespace gameplay
}  // namespace engine

// ---- factory -----------------------------------------------------------------

namespace engine {
namespace gameplay {

std::unique_ptr<IMissionRuntime> create_mission_runtime() {
    return std::make_unique<MissionRuntime>();
}

}  // namespace gameplay
}  // namespace engine

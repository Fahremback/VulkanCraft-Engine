// WorldDirector.cpp — SDK adapter for the public IWorldDirector contract
// (FALTANTES differential: event selection by rules, utility, coherence and
// diversity — META §32). Single TU, pure, deterministic, all-or-nothing
// refusals. Parses JSON through the shared RegistryJson helpers; emission is
// a local deterministic emitter (std::map ordering, %.9g floats — the same
// convention as WorldProfile/AbilitySystem/MacroMicroReconciler).
#include "engine/director/IWorldDirector.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace engine {
namespace director {
namespace {

std::string float_str(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return std::string(buffer);
}

std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct JsonValue {
    enum class Kind { Null, Str, Num, Bool, Arr, Obj } kind{ Kind::Null };
    std::string str;
    double num{ 0.0 };
    bool boolean{ false };
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    static JsonValue number(double value) {
        JsonValue v;
        v.kind = Kind::Num;
        v.num = value;
        return v;
    }
    static JsonValue text(const std::string& value) {
        JsonValue v;
        v.kind = Kind::Str;
        v.str = value;
        return v;
    }
    static JsonValue bool_value(bool value) {
        JsonValue v;
        v.kind = Kind::Bool;
        v.boolean = value;
        return v;
    }
    static JsonValue json_array(std::vector<JsonValue> values) {
        JsonValue v;
        v.kind = Kind::Arr;
        v.arr = std::move(values);
        return v;
    }
    static JsonValue json_object(std::map<std::string, JsonValue> fields) {
        JsonValue v;
        v.kind = Kind::Obj;
        v.obj = std::move(fields);
        return v;
    }
};

std::string emit_json(const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::Null:
            return "null";
        case JsonValue::Kind::Bool:
            return value.boolean ? "true" : "false";
        case JsonValue::Kind::Num: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.9g", value.num);
            return std::string(buffer);
        }
        case JsonValue::Kind::Str:
            return "\"" + escape_json(value.str) + "\"";
        case JsonValue::Kind::Arr: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.arr.size(); ++i) {
                if (i != 0) out += ",";
                out += emit_json(value.arr[i]);
            }
            out += "]";
            return out;
        }
        case JsonValue::Kind::Obj: {
            std::string out = "{";
            bool first = true;
            for (const auto& entry : value.obj) {
                if (!first) out += ",";
                first = false;
                out += "\"" + escape_json(entry.first) + "\":" +
                       emit_json(entry.second);
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

std::string uint64_str(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

bool parse_uint64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

bool read_uint64_field(const engine::sdk::JsonValue& doc,
                       const std::string& key, std::uint64_t& out,
                       std::uint64_t defaultValue, std::string& errorOut) {
    const auto* field = doc.field(key);
    if (field == nullptr) {
        out = defaultValue;
        return true;
    }
    if (field->kind == engine::sdk::JsonValue::Kind::Number &&
        field->number >= 0.0) {
        out = static_cast<std::uint64_t>(field->number);
        return true;
    }
    if (field->is_string() && parse_uint64(field->string, out)) {
        return true;
    }
    errorOut = "field '" + key + "' must be a non-negative integer";
    return false;
}

bool read_double_field(const engine::sdk::JsonValue& doc,
                       const std::string& key, float& out, float defaultValue,
                       std::string& errorOut) {
    const auto* field = doc.field(key);
    if (field == nullptr) {
        out = defaultValue;
        return true;
    }
    if (field->kind != engine::sdk::JsonValue::Kind::Number ||
        !std::isfinite(field->number)) {
        errorOut = "field '" + key + "' must be a finite number";
        return false;
    }
    out = static_cast<float>(field->number);
    return true;
}

bool read_int_field(const engine::sdk::JsonValue& doc, const std::string& key,
                    int& out, int defaultValue, std::string& errorOut) {
    const auto* field = doc.field(key);
    if (field == nullptr) {
        out = defaultValue;
        return true;
    }
    if (field->kind != engine::sdk::JsonValue::Kind::Number ||
        !std::isfinite(field->number) ||
        field->number != std::floor(field->number)) {
        errorOut = "field '" + key + "' must be an integer";
        return false;
    }
    out = static_cast<int>(field->number);
    return true;
}

bool has_tag(const std::vector<std::string>& tags, const std::string& tag) {
    for (const std::string& existing : tags) {
        if (existing == tag) return true;
    }
    return false;
}

bool valid_spec(const DirectorSpec& spec, std::string& errorOut) {
    if (spec.version != 1) {
        errorOut = "unsupported spec version";
        return false;
    }
    if (spec.maxPerTick < 0) {
        errorOut = "maxPerTick must be >= 0";
        return false;
    }
    if (!std::isfinite(spec.diversityPenalty) || spec.diversityPenalty < 0.0f ||
        spec.diversityPenalty > 1.0f) {
        errorOut = "diversityPenalty must be finite and in [0, 1]";
        return false;
    }
    if (spec.recencyWindow < 1) {
        errorOut = "recencyWindow must be >= 1";
        return false;
    }
    if (spec.dayLengthTicks < 1) {
        errorOut = "dayLengthTicks must be >= 1";
        return false;
    }
    std::set<std::string> ids;
    for (const WorldEventCandidate& candidate : spec.candidates) {
        if (candidate.id.empty()) {
            errorOut = "candidate id must be non-empty";
            return false;
        }
        if (!ids.insert(candidate.id).second) {
            errorOut = "duplicate candidate id: " + candidate.id;
            return false;
        }
        if (!std::isfinite(candidate.baseUtility) ||
            candidate.baseUtility < 0.0f || candidate.baseUtility > 1.0f) {
            errorOut = "baseUtility must be finite and in [0, 1]";
            return false;
        }
        if (!std::isfinite(candidate.weight) || candidate.weight < 0.0f) {
            errorOut = "weight must be finite and >= 0";
            return false;
        }
        if (candidate.maxConcurrent < 1) {
            errorOut = "maxConcurrent must be >= 1";
            return false;
        }
    }
    return true;
}

class WorldDirector final : public IWorldDirector {
public:
    bool set_spec(const DirectorSpec& spec, std::string& errorOut) override {
        if (!valid_spec(spec, errorOut)) return false;
        spec_ = spec;
        specSet_ = true;
        return true;
    }

    bool set_spec_json(const std::string& jsonText,
                       std::string& errorOut) override {
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(jsonText, doc, errorOut)) {
            errorOut = "malformed spec document: " + errorOut;
            return false;
        }
        DirectorSpec spec;
        const auto* versionField = doc.field("version");
        if (versionField == nullptr ||
            versionField->kind != engine::sdk::JsonValue::Kind::Number ||
            versionField->number != 1.0) {
            errorOut = "spec document version must be 1";
            return false;
        }
        if (!read_int_field(doc, "maxPerTick", spec.maxPerTick, 1, errorOut) ||
            !read_double_field(doc, "diversityPenalty",
                               spec.diversityPenalty, 0.25, errorOut) ||
            !read_uint64_field(doc, "recencyWindow", spec.recencyWindow, 1000,
                               errorOut) ||
            !read_uint64_field(doc, "dayLengthTicks", spec.dayLengthTicks, 2400,
                               errorOut)) {
            return false;
        }
        const auto* candidatesField = doc.field("candidates");
        if (candidatesField == nullptr ||
            candidatesField->kind != engine::sdk::JsonValue::Kind::Array) {
            errorOut = "spec document must have a candidates array";
            return false;
        }
        for (const engine::sdk::JsonValue& candidateDoc :
             candidatesField->array) {
            if (!candidateDoc.is_object()) {
                errorOut = "each candidate must be an object";
                return false;
            }
            WorldEventCandidate candidate;
            candidate.id = engine::sdk::json_string(candidateDoc, "id", "");
            candidate.requiresAll =
                engine::sdk::json_string_array(candidateDoc, "requiresAll");
            candidate.excludesAny =
                engine::sdk::json_string_array(candidateDoc, "excludesAny");
            if (!read_double_field(candidateDoc, "baseUtility",
                                   candidate.baseUtility, 0.5, errorOut) ||
                !read_double_field(candidateDoc, "weight", candidate.weight, 1.0,
                                   errorOut) ||
                !read_uint64_field(candidateDoc, "cooldownTicks",
                                   candidate.cooldownTicks, 0, errorOut) ||
                !read_uint64_field(candidateDoc, "maxConcurrent",
                                   candidate.maxConcurrent, 1, errorOut) ||
                !read_uint64_field(candidateDoc, "maxPerDay",
                                   candidate.maxPerDay, 0, errorOut)) {
                return false;
            }
            candidate.category =
                engine::sdk::json_string(candidateDoc, "category", "");
            spec.candidates.push_back(candidate);
        }
        // All-or-nothing: only a fully validated spec becomes active.
        if (!set_spec(spec, errorOut)) return false;
        return true;
    }

    std::string spec_to_json() const override {
        std::vector<JsonValue> candidates;
        for (const WorldEventCandidate& candidate : spec_.candidates) {
            std::map<std::string, JsonValue> fields;
            fields["id"] = JsonValue::text(candidate.id);
            std::vector<JsonValue> requiresAll;
            for (const std::string& tag : candidate.requiresAll) {
                requiresAll.push_back(JsonValue::text(tag));
            }
            fields["requiresAll"] = JsonValue::json_array(std::move(requiresAll));
            std::vector<JsonValue> excludesAny;
            for (const std::string& tag : candidate.excludesAny) {
                excludesAny.push_back(JsonValue::text(tag));
            }
            fields["excludesAny"] = JsonValue::json_array(std::move(excludesAny));
            fields["baseUtility"] = JsonValue::number(candidate.baseUtility);
            fields["weight"] = JsonValue::number(candidate.weight);
            fields["cooldownTicks"] =
                JsonValue::text(uint64_str(candidate.cooldownTicks));
            fields["maxConcurrent"] =
                JsonValue::text(uint64_str(candidate.maxConcurrent));
            fields["maxPerDay"] = JsonValue::text(uint64_str(candidate.maxPerDay));
            fields["category"] = JsonValue::text(candidate.category);
            candidates.push_back(JsonValue::json_object(std::move(fields)));
        }
        std::map<std::string, JsonValue> doc;
        doc["version"] = JsonValue::number(1);
        doc["maxPerTick"] = JsonValue::number(spec_.maxPerTick);
        doc["diversityPenalty"] = JsonValue::number(spec_.diversityPenalty);
        doc["recencyWindow"] = JsonValue::text(uint64_str(spec_.recencyWindow));
        doc["dayLengthTicks"] =
            JsonValue::text(uint64_str(spec_.dayLengthTicks));
        doc["candidates"] = JsonValue::json_array(std::move(candidates));
        return emit_json(JsonValue::json_object(std::move(doc)));
    }

    const DirectorSpec* spec() const override {
        return specSet_ ? &spec_ : nullptr;
    }

    bool eligible(const WorldEventCandidate& candidate,
                  const DirectorWorldState& world,
                  const EventSelectionState& selection,
                  std::string& reasonOut) const override {
        if (candidate.weight == 0.0f) {
            reasonOut = "disabled";
            return false;
        }
        for (const std::string& tag : candidate.requiresAll) {
            if (!has_tag(world.tags, tag)) {
                reasonOut = "missing_tags";
                return false;
            }
        }
        for (const std::string& tag : candidate.excludesAny) {
            if (has_tag(world.tags, tag)) {
                reasonOut = "excluded_tag";
                return false;
            }
        }
        if (selection.fireCount > 0 &&
            world.tick - selection.lastFireTick < candidate.cooldownTicks) {
            reasonOut = "cooldown";
            return false;
        }
        if (selection.activeCount >= candidate.maxConcurrent) {
            reasonOut = "concurrency_limit";
            return false;
        }
        if (candidate.maxPerDay > 0) {
            const std::uint64_t day = world.tick / spec_.dayLengthTicks;
            const std::uint64_t firesToday =
                selection.dayOfLastFire == day ? selection.firesThisDay : 0;
            if (firesToday >= candidate.maxPerDay) {
                reasonOut = "daily_limit";
                return false;
            }
        }
        reasonOut = "eligible";
        return true;
    }

    bool candidate_utility(const WorldEventCandidate& candidate,
                           const DirectorWorldState& world,
                           const EventSelectionState& selection,
                           float& utilityOut,
                           std::string& errorOut) const override {
        if (!specSet_) {
            errorOut = "no spec configured";
            return false;
        }
        if (candidate.id != selection.id) {
            errorOut = "candidate and selection ids mismatch";
            return false;
        }
        const bool known = std::any_of(
            spec_.candidates.begin(), spec_.candidates.end(),
            [&candidate](const WorldEventCandidate& entry) {
                return entry.id == candidate.id;
            });
        if (!known) {
            errorOut = "candidate id absent from the spec";
            return false;
        }
        if (candidate.weight == 0.0f) {
            utilityOut = 0.0f;
            return true;
        }
        const float urgency = urgency_impl(candidate, world, selection);
        const float recencyFactor = 1.0f - urgency;
        utilityOut = candidate.baseUtility * candidate.weight *
                         (0.5f + 0.5f * urgency) -
                     spec_.diversityPenalty * recencyFactor;
        return true;
    }

    bool select(DirectorWorldState& world,
                std::vector<EventSelectionState>& selections,
                std::vector<DirectorSelection>& out,
                std::string& errorOut) override {
        out.clear();
        if (!specSet_) {
            errorOut = "no spec configured";
            return false;
        }
        if (world.version != 1) {
            errorOut = "unsupported world version";
            return false;
        }

        // Validate the selection list (all-or-nothing) and index it.
        std::map<std::string, std::size_t> indexById;
        for (std::size_t i = 0; i < selections.size(); ++i) {
            if (indexById.count(selections[i].id) != 0) {
                errorOut = "duplicate selection id: " + selections[i].id;
                return false;
            }
            const bool known = std::any_of(
                spec_.candidates.begin(), spec_.candidates.end(),
                [&selections, i](const WorldEventCandidate& entry) {
                    return entry.id == selections[i].id;
                });
            if (!known) {
                errorOut = "selection id absent from the spec: " +
                           selections[i].id;
                return false;
            }
            indexById[selections[i].id] = i;
        }

        // Score every eligible candidate (pure; nothing mutated yet).
        struct Scored {
            const WorldEventCandidate* candidate;
            float utility;
        };
        std::vector<Scored> scored;
        EventSelectionState implicitState;  // zero state for unregistered ids
        for (const WorldEventCandidate& candidate : spec_.candidates) {
            implicitState.id = candidate.id;
            const auto found = indexById.find(candidate.id);
            const EventSelectionState& state =
                found != indexById.end() ? selections[found->second]
                                         : implicitState;
            std::string reason;
            if (!eligible(candidate, world, state, reason)) continue;
            float utility = 0.0f;
            if (!candidate_utility(candidate, world, state, utility,
                                   errorOut)) {
                return false;
            }
            scored.push_back({ &candidate, utility });
        }
        // Deterministic order: utility DESC, id ASC.
        std::sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) {
                      if (a.utility != b.utility) return a.utility > b.utility;
                      return a.candidate->id < b.candidate->id;
                  });

        // The chosen set must all have selection states (all-or-nothing —
        // validate before mutating).
        const std::size_t chosen =
            spec_.maxPerTick > 0
                ? std::min<std::size_t>(
                      static_cast<std::size_t>(spec_.maxPerTick),
                      scored.size())
                : scored.size();
        for (std::size_t i = 0; i < chosen; ++i) {
            if (indexById.count(scored[i].candidate->id) == 0) {
                errorOut = "missing selection state for " +
                           scored[i].candidate->id;
                return false;
            }
        }

        // Commit: advance the chosen states deterministically.
        const std::uint64_t day = world.tick / spec_.dayLengthTicks;
        for (std::size_t i = 0; i < chosen; ++i) {
            EventSelectionState& state =
                selections[indexById[scored[i].candidate->id]];
            state.lastFireTick = world.tick;
            ++state.fireCount;
            ++state.selectedCount;
            if (state.dayOfLastFire != day) {
                state.dayOfLastFire = day;
                state.firesThisDay = 1;
            } else {
                ++state.firesThisDay;
            }
            DirectorSelection decision;
            decision.eventId = scored[i].candidate->id;
            decision.utility = scored[i].utility;
            decision.reason = "eligible";
            out.push_back(decision);
        }
        return true;
    }

    bool serialize_selections(
        const std::vector<EventSelectionState>& selections, std::string& out,
        std::string& errorOut) const override {
        std::vector<JsonValue> items;
        for (const EventSelectionState& state : selections) {
            std::map<std::string, JsonValue> fields;
            fields["id"] = JsonValue::text(state.id);
            fields["lastFireTick"] = JsonValue::text(uint64_str(state.lastFireTick));
            fields["fireCount"] = JsonValue::text(uint64_str(state.fireCount));
            fields["selectedCount"] =
                JsonValue::text(uint64_str(state.selectedCount));
            fields["firesThisDay"] =
                JsonValue::text(uint64_str(state.firesThisDay));
            fields["dayOfLastFire"] =
                JsonValue::text(uint64_str(state.dayOfLastFire));
            fields["activeCount"] =
                JsonValue::text(uint64_str(state.activeCount));
            items.push_back(JsonValue::json_object(std::move(fields)));
        }
        std::map<std::string, JsonValue> doc;
        doc["version"] = JsonValue::number(1);
        doc["selections"] = JsonValue::json_array(std::move(items));
        out = emit_json(JsonValue::json_object(std::move(doc)));
        return true;
    }

    bool deserialize_selections(
        const std::string& data, std::vector<EventSelectionState>& out,
        std::string& errorOut) const override {
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(data, doc, errorOut)) {
            errorOut = "malformed selections document: " + errorOut;
            return false;
        }
        const auto* versionField = doc.field("version");
        if (versionField == nullptr ||
            versionField->kind != engine::sdk::JsonValue::Kind::Number ||
            versionField->number != 1.0) {
            errorOut = "selections document version must be 1";
            return false;
        }
        const auto* selectionsField = doc.field("selections");
        if (selectionsField == nullptr ||
            selectionsField->kind != engine::sdk::JsonValue::Kind::Array) {
            errorOut = "selections document must have a selections array";
            return false;
        }
        std::vector<EventSelectionState> parsed;
        std::set<std::string> seen;
        for (const engine::sdk::JsonValue& item : selectionsField->array) {
            if (!item.is_object()) {
                errorOut = "each selection must be an object";
                return false;
            }
            EventSelectionState state;
            state.id = engine::sdk::json_string(item, "id", "");
            if (state.id.empty()) {
                errorOut = "selection id must be non-empty";
                return false;
            }
            if (!seen.insert(state.id).second) {
                errorOut = "duplicate selection id: " + state.id;
                return false;
            }
            if (!read_uint64_field(item, "lastFireTick", state.lastFireTick, 0,
                                   errorOut) ||
                !read_uint64_field(item, "fireCount", state.fireCount, 0,
                                   errorOut) ||
                !read_uint64_field(item, "selectedCount", state.selectedCount, 0,
                                   errorOut) ||
                !read_uint64_field(item, "firesThisDay", state.firesThisDay, 0,
                                   errorOut) ||
                !read_uint64_field(item, "dayOfLastFire", state.dayOfLastFire, 0,
                                   errorOut) ||
                !read_uint64_field(item, "activeCount", state.activeCount, 0,
                                   errorOut)) {
                return false;
            }
            parsed.push_back(state);
        }
        // All-or-nothing: only a fully validated document replaces `out`.
        out = std::move(parsed);
        return true;
    }

private:
    // The recency fraction of a candidate: 1 when it never fired or has not
    // fired for a full window, 0 when it fired THIS tick. Deterministic.
    float urgency_impl(const WorldEventCandidate& candidate,
                       const DirectorWorldState& world,
                       const EventSelectionState& selection) const {
        if (selection.fireCount == 0) return 1.0f;
        const std::uint64_t elapsed =
            world.tick > selection.lastFireTick
                ? world.tick - selection.lastFireTick
                : 0;
        if (elapsed >= spec_.recencyWindow) return 1.0f;
        return static_cast<float>(elapsed) /
               static_cast<float>(spec_.recencyWindow);
    }

    DirectorSpec spec_;
    bool specSet_{ false };
};

}  // namespace

std::unique_ptr<IWorldDirector> create_world_director() {
    return std::unique_ptr<IWorldDirector>(new WorldDirector());
}

}  // namespace director
}  // namespace engine

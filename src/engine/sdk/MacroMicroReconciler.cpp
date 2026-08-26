// MacroMicroReconciler.cpp — SDK adapter for the public IMacroMicroReconciler
// contract (FALTANTES differential: deterministic materialization of
// aggregate consequences — META §32). Single TU, pure, deterministic, all-or-
// nothing refusals. Parses JSON through the shared RegistryJson helpers
// (src/engine/sdk/RegistryJson.cpp, compiled alongside this TU); emission is
// a local deterministic emitter (std::map ordering, %.9g floats — the same
// convention as WorldProfile/AbilitySystem).
#include "engine/simulation/IMacroMicroReconciler.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace engine {
namespace simulation {
namespace {

// Deterministic 64-bit mix (splitmix64 finalizer). The ONLY source of
// pseudo-randomness in this adapter — every derived position/handle is a
// pure function of (seed, index) through this function, so the same input
// reproduces the same consequences bit-exactly on every platform.
std::uint64_t mix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::uint32_t float_to_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

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

// Minimal deterministic JSON emitter: object maps use std::map ordering
// (sorted keys); floats emit as %.9g; integers > 2^53 are emitted as strings
// so the uint64 fields round-trip bit-exactly through the double-based
// RegistryJson parser.
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
                out += "\"" + escape_json(entry.first) + "\":" + emit_json(entry.second);
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

int kind_priority(MaterializedEffect::Kind kind) {
    switch (kind) {
        case MaterializedEffect::Kind::RemoveEntity: return 4;
        case MaterializedEffect::Kind::GrowthStage: return 3;
        case MaterializedEffect::Kind::ResourceDrop: return 2;
        case MaterializedEffect::Kind::SpawnEntity: return 1;
    }
    return 0;
}

const char* kind_name(MaterializedEffect::Kind kind) {
    switch (kind) {
        case MaterializedEffect::Kind::SpawnEntity: return "spawn";
        case MaterializedEffect::Kind::ResourceDrop: return "drop";
        case MaterializedEffect::Kind::GrowthStage: return "growth";
        case MaterializedEffect::Kind::RemoveEntity: return "remove";
    }
    return "spawn";
}

bool kind_from_name(const std::string& name, MaterializedEffect::Kind& out) {
    if (name == "spawn") out = MaterializedEffect::Kind::SpawnEntity;
    else if (name == "drop") out = MaterializedEffect::Kind::ResourceDrop;
    else if (name == "growth") out = MaterializedEffect::Kind::GrowthStage;
    else if (name == "remove") out = MaterializedEffect::Kind::RemoveEntity;
    else return false;
    return true;
}

bool valid_macro(const ReconcilerMacroState& macro, std::string& errorOut) {
    if (macro.version != 1) {
        errorOut = "unsupported macro version";
        return false;
    }
    if (!(macro.cellSize > 0.0f) || !std::isfinite(macro.cellSize)) {
        errorOut = "cellSize must be finite and > 0";
        return false;
    }
    const float* counters[] = { &macro.population, &macro.previousPopulation,
                                &macro.resources };
    for (const float* counter : counters) {
        if (!std::isfinite(*counter) || *counter < 0.0f) {
            errorOut = "counters must be finite and >= 0";
            return false;
        }
    }
    if (!std::isfinite(macro.growth) || macro.growth < 0.0f ||
        macro.growth > 1.0f) {
        errorOut = "growth must be finite and in [0, 1]";
        return false;
    }
    return true;
}

std::string effect_target_key(const MaterializedEffect& effect) {
    switch (effect.kind) {
        case MaterializedEffect::Kind::SpawnEntity:
        case MaterializedEffect::Kind::ResourceDrop: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "pos:%08x:%08x",
                          float_to_bits(effect.positionX),
                          float_to_bits(effect.positionZ));
            return std::string(buffer);
        }
        case MaterializedEffect::Kind::GrowthStage: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "block:%d:%d:%d",
                          effect.blockX, effect.blockY, effect.blockZ);
            return std::string(buffer);
        }
        case MaterializedEffect::Kind::RemoveEntity:
            return "handle:" + effect.handle;
    }
    return "";
}

bool effects_equal(const MaterializedEffect& a, const MaterializedEffect& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case MaterializedEffect::Kind::SpawnEntity:
            return a.archetypeId == b.archetypeId &&
                   a.positionX == b.positionX && a.positionZ == b.positionZ;
        case MaterializedEffect::Kind::ResourceDrop:
            return a.itemId == b.itemId && a.positionX == b.positionX &&
                   a.positionZ == b.positionZ;
        case MaterializedEffect::Kind::GrowthStage:
            return a.blockId == b.blockId && a.blockX == b.blockX &&
                   a.blockY == b.blockY && a.blockZ == b.blockZ &&
                   a.growthStage == b.growthStage;
        case MaterializedEffect::Kind::RemoveEntity:
            return a.handle == b.handle && a.reason == b.reason;
    }
    return false;
}

bool valid_effect(const MaterializedEffect& effect) {
    switch (effect.kind) {
        case MaterializedEffect::Kind::SpawnEntity:
            return !effect.archetypeId.empty();
        case MaterializedEffect::Kind::ResourceDrop:
            return !effect.itemId.empty();
        case MaterializedEffect::Kind::GrowthStage:
            return !effect.blockId.empty() && effect.growthStage >= 1;
        case MaterializedEffect::Kind::RemoveEntity:
            return !effect.handle.empty();
    }
    return false;
}

class MacroMicroReconciler final : public IMacroMicroReconciler {
public:
    bool set_rules(const std::vector<ReconcilerRule>& rules,
                   std::string& errorOut) override {
        std::set<std::string> tags;
        bool hasDefault = false;
        for (const ReconcilerRule& rule : rules) {
            if (rule.maxGrowthStages < 1) {
                errorOut = "maxGrowthStages must be >= 1";
                return false;
            }
            if (!std::isfinite(rule.growthDensity) || rule.growthDensity < 0.0f) {
                errorOut = "growthDensity must be finite and >= 0";
                return false;
            }
            if (rule.tag.empty()) {
                if (hasDefault) {
                    errorOut = "only one default rule (empty tag) is allowed";
                    return false;
                }
                hasDefault = true;
            } else if (!tags.insert(rule.tag).second) {
                errorOut = "duplicate rule tag: " + rule.tag;
                return false;
            }
        }
        rules_ = rules;
        return true;
    }

    bool set_rules_json(const std::string& jsonText,
                        std::string& errorOut) override {
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(jsonText, doc, errorOut)) {
            errorOut = "malformed rules document: " + errorOut;
            return false;
        }
        const auto* versionField = doc.field("version");
        const auto* rulesField = doc.field("rules");
        if (versionField == nullptr ||
            versionField->kind != engine::sdk::JsonValue::Kind::Number ||
            versionField->number != 1.0) {
            errorOut = "rules document version must be 1";
            return false;
        }
        if (rulesField == nullptr ||
            rulesField->kind != engine::sdk::JsonValue::Kind::Array) {
            errorOut = "rules document must have a rules array";
            return false;
        }
        std::vector<ReconcilerRule> parsed;
        for (const engine::sdk::JsonValue& ruleDoc : rulesField->array) {
            if (!ruleDoc.is_object()) {
                errorOut = "each rule must be an object";
                return false;
            }
            ReconcilerRule rule;
            rule.tag = engine::sdk::json_string(ruleDoc, "tag", "");
            rule.archetypeId = engine::sdk::json_string(ruleDoc, "archetypeId", "");
            rule.itemId = engine::sdk::json_string(ruleDoc, "itemId", "");
            rule.blockId = engine::sdk::json_string(ruleDoc, "blockId", "");
            const double stages = engine::sdk::json_number(ruleDoc, "maxGrowthStages", 3.0);
            if (!std::isfinite(stages) || stages != std::floor(stages)) {
                errorOut = "maxGrowthStages must be an integer";
                return false;
            }
            rule.maxGrowthStages = static_cast<int>(stages);
            rule.growthDensity =
                static_cast<float>(engine::sdk::json_number(ruleDoc, "growthDensity", 2.0));
            parsed.push_back(rule);
        }
        // All-or-nothing: only the fully validated list becomes active.
        if (!set_rules(parsed, errorOut)) return false;
        return true;
    }

    std::string rules_to_json() const override {
        std::vector<JsonValue> ruleArray;
        for (const ReconcilerRule& rule : rules_) {
            std::map<std::string, JsonValue> fields;
            fields["tag"] = JsonValue::text(rule.tag);
            fields["archetypeId"] = JsonValue::text(rule.archetypeId);
            fields["itemId"] = JsonValue::text(rule.itemId);
            fields["blockId"] = JsonValue::text(rule.blockId);
            fields["maxGrowthStages"] = JsonValue::number(rule.maxGrowthStages);
            fields["growthDensity"] = JsonValue::number(rule.growthDensity);
            ruleArray.push_back(JsonValue::json_object(std::move(fields)));
        }
        std::map<std::string, JsonValue> doc;
        doc["version"] = JsonValue::number(1);
        doc["rules"] = JsonValue::json_array(std::move(ruleArray));
        return emit_json(JsonValue::json_object(std::move(doc)));
    }

    bool set_budget(const ReconcilerBudget& budget,
                    std::string& errorOut) override {
        if (budget.version != 1) {
            errorOut = "unsupported budget version";
            return false;
        }
        if (budget.maxEffectsPerTick < 0) {
            errorOut = "maxEffectsPerTick must be >= 0";
            return false;
        }
        budget_ = budget;
        budgetSet_ = true;
        return true;
    }

    const std::vector<ReconcilerRule>* rules() const override { return &rules_; }
    const ReconcilerBudget* budget() const override {
        return budgetSet_ ? &budget_ : nullptr;
    }

    bool materialize(const ReconcilerMacroState& macro,
                     std::vector<MaterializedEffect>& effectsOut,
                     std::string& errorOut) const override {
        effectsOut.clear();
        if (!valid_macro(macro, errorOut)) return false;

        const ReconcilerRule* rule = rule_for(macro);
        const std::int64_t spawnCount =
            static_cast<std::int64_t>(std::floor(macro.population));
        const std::int64_t dropCount =
            static_cast<std::int64_t>(std::floor(macro.resources));
        const std::int64_t prevCount =
            static_cast<std::int64_t>(std::floor(macro.previousPopulation));
        const float density = rule != nullptr ? rule->growthDensity : 0.0f;
        const std::int64_t growthCount =
            static_cast<std::int64_t>(std::floor(macro.growth * density));
        const std::int64_t removalCount =
            prevCount > spawnCount ? prevCount - spawnCount : 0;

        if ((spawnCount > 0 || dropCount > 0 || growthCount > 0) &&
            rule == nullptr) {
            errorOut = "no rules configured for the region";
            return false;
        }
        if (spawnCount > 0 && rule->archetypeId.empty()) {
            errorOut = "the matching rule provides no archetypeId";
            return false;
        }
        if (dropCount > 0 && rule->itemId.empty()) {
            errorOut = "the matching rule provides no itemId";
            return false;
        }
        if (growthCount > 0 && rule->blockId.empty()) {
            errorOut = "the matching rule provides no blockId";
            return false;
        }

        std::uint64_t index = 0;
        for (std::int64_t i = 0; i < spawnCount; ++i, ++index) {
            MaterializedEffect effect;
            effect.kind = MaterializedEffect::Kind::SpawnEntity;
            effect.archetypeId = rule->archetypeId;
            cell_position(macro, index, effect.positionX, effect.positionZ);
            effectsOut.push_back(effect);
        }
        for (std::int64_t i = 0; i < dropCount; ++i, ++index) {
            MaterializedEffect effect;
            effect.kind = MaterializedEffect::Kind::ResourceDrop;
            effect.itemId = rule->itemId;
            cell_position(macro, index, effect.positionX, effect.positionZ);
            effectsOut.push_back(effect);
        }
        const int stage = growth_stage(macro, *rule);
        const std::uint64_t growthBase = mix64(macro.seed ^ 0x5EED);
        const std::uint64_t cellWidth =
            std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
                                           std::floor(macro.cellSize)));
        for (std::int64_t i = 0; i < growthCount; ++i, ++index) {
            MaterializedEffect effect;
            effect.kind = MaterializedEffect::Kind::GrowthStage;
            effect.blockId = rule->blockId;
            const std::uint64_t h = mix64(growthBase + static_cast<std::uint64_t>(i));
            effect.blockX = static_cast<int>(
                std::floor(macro.cellX * macro.cellSize) +
                static_cast<std::int64_t>(h % cellWidth));
            effect.blockY = 0;
            effect.blockZ = static_cast<int>(
                std::floor(macro.cellZ * macro.cellSize) +
                static_cast<std::int64_t>((h >> 32) % cellWidth));
            effect.growthStage = stage;
            effectsOut.push_back(effect);
        }
        for (std::int64_t i = 0; i < removalCount; ++i, ++index) {
            MaterializedEffect effect;
            effect.kind = MaterializedEffect::Kind::RemoveEntity;
            effect.handle = uint64_str(static_cast<std::uint64_t>(macro.cellX)) +
                            "," +
                            uint64_str(static_cast<std::uint64_t>(macro.cellZ)) +
                            ":decline:" + uint64_str(static_cast<std::uint64_t>(i));
            effect.reason = "population_decline";
            effectsOut.push_back(effect);
        }
        return true;
    }

    bool reconcile(ReconcilerState& state, const ReconcilerMacroState& macro,
                   std::vector<MaterializedEffect>& effectsOut,
                   std::string& errorOut) override {
        effectsOut.clear();
        if (state.version != 1) {
            errorOut = "unsupported state version";
            return false;
        }
        if (!budgetSet_) {
            errorOut = "no budget configured";
            return false;
        }
        if (!valid_macro(macro, errorOut)) return false;

        const std::uint64_t fingerprint = macro_fingerprint(macro);
        const bool rematerialize =
            (state.pending.empty() && !state.complete) ||
            state.macroFingerprint != fingerprint;

        std::vector<MaterializedEffect> batch;
        if (rematerialize) {
            // All-or-nothing: refuse BEFORE touching `state` when the new
            // macro cannot be materialized (e.g. a needed id is missing).
            if (!materialize(macro, batch, errorOut)) return false;
        } else {
            batch = state.pending;
        }

        std::size_t cursor = rematerialize ? 0 : state.cursor;
        if (cursor > batch.size()) {
            errorOut = "corrupt reconciliation cursor";
            return false;
        }

        const std::size_t budget = budget_.maxEffectsPerTick > 0
                                       ? static_cast<std::size_t>(
                                             budget_.maxEffectsPerTick)
                                       : batch.size();
        std::size_t emitted = 0;
        while (cursor < batch.size() && emitted < budget) {
            effectsOut.push_back(batch[cursor]);
            ++cursor;
            ++emitted;
        }

        // Commit.
        if (rematerialize) {
            state.pending = std::move(batch);
            ++state.materializationCount;
        }
        state.cursor = cursor;
        state.macroFingerprint = fingerprint;
        state.complete = cursor >= state.pending.size();
        return true;
    }

    bool merge_and_resolve(const std::vector<MaterializedEffect>& a,
                           const std::vector<MaterializedEffect>& b,
                           std::uint64_t seedA, std::uint64_t seedB,
                           std::vector<MaterializedEffect>& out,
                           std::string& errorOut) const override {
        out.clear();
        for (const MaterializedEffect& effect : a) {
            if (!valid_effect(effect)) {
                errorOut = "invalid effect in batch A";
                return false;
            }
        }
        for (const MaterializedEffect& effect : b) {
            if (!valid_effect(effect)) {
                errorOut = "invalid effect in batch B";
                return false;
            }
        }
        // First pass: one winner per target slot.
        std::map<std::string, std::pair<MaterializedEffect, std::uint64_t>>
            winners;
        const auto insertWinner = [&winners](const MaterializedEffect& effect,
                                             std::uint64_t seed) {
            const std::string key = effect_target_key(effect);
            const auto found = winners.find(key);
            if (found == winners.end()) {
                winners.emplace(key, std::make_pair(effect, seed));
                return;
            }
            const MaterializedEffect& current = found->second.first;
            const int currentPriority = kind_priority(current.kind);
            const int newPriority = kind_priority(effect.kind);
            if (newPriority > currentPriority ||
                (newPriority == currentPriority && seed < found->second.second)) {
                found->second = std::make_pair(effect, seed);
            }
        };
        for (const MaterializedEffect& effect : a) insertWinner(effect, seedA);
        for (const MaterializedEffect& effect : b) insertWinner(effect, seedB);
        // Second pass: input order, winners in place. A slot is consumed by
        // its FIRST kept effect — value-identical duplicates from the losing
        // batch must not also survive (one winner per target slot).
        std::set<std::string> consumed;
        const auto keepWinner = [&winners, &consumed](const MaterializedEffect& effect) {
            const std::string key = effect_target_key(effect);
            const auto found = winners.find(key);
            if (found == winners.end() ||
                !effects_equal(found->second.first, effect)) {
                return false;
            }
            if (consumed.count(key) != 0) return false;
            consumed.insert(key);
            return true;
        };
        for (const MaterializedEffect& effect : a) {
            if (keepWinner(effect)) out.push_back(effect);
        }
        for (const MaterializedEffect& effect : b) {
            if (keepWinner(effect)) out.push_back(effect);
        }
        return true;
    }

    bool serialize_state(const ReconcilerState& state, std::string& out,
                         std::string& errorOut) const override {
        if (state.version != 1) {
            errorOut = "unsupported state version";
            return false;
        }
        std::vector<JsonValue> pending;
        for (const MaterializedEffect& effect : state.pending) {
            pending.push_back(effect_json(effect));
        }
        std::map<std::string, JsonValue> doc;
        doc["version"] = JsonValue::number(1);
        doc["cellX"] = JsonValue::text(uint64_str(static_cast<std::uint64_t>(state.cellX)));
        doc["cellZ"] = JsonValue::text(uint64_str(static_cast<std::uint64_t>(state.cellZ)));
        doc["macroFingerprint"] = JsonValue::text(uint64_str(state.macroFingerprint));
        doc["cursor"] = JsonValue::text(uint64_str(static_cast<std::uint64_t>(state.cursor)));
        doc["materializationCount"] =
            JsonValue::text(uint64_str(state.materializationCount));
        doc["complete"] = JsonValue::bool_value(state.complete);
        doc["pending"] = JsonValue::json_array(std::move(pending));
        out = emit_json(JsonValue::json_object(std::move(doc)));
        return true;
    }

    bool deserialize_state(const std::string& data, ReconcilerState& out,
                           std::string& errorOut) const override {
        engine::sdk::JsonValue doc;
        if (!engine::sdk::json_parse(data, doc, errorOut)) {
            errorOut = "malformed state document: " + errorOut;
            return false;
        }
        ReconcilerState parsed;
        std::uint64_t version = 0;
        if (!read_uint64_field(doc, "version", version, 1, errorOut)) {
            errorOut = "state version must be 1";
            return false;
        }
        if (version != 1) {
            errorOut = "unsupported state version";
            return false;
        }
        std::uint64_t cellX = 0, cellZ = 0, cursor = 0, count = 0;
        if (!read_uint64_field(doc, "cellX", cellX, 0, errorOut) ||
            !read_uint64_field(doc, "cellZ", cellZ, 0, errorOut) ||
            !read_uint64_field(doc, "macroFingerprint", parsed.macroFingerprint, 0,
                               errorOut) ||
            !read_uint64_field(doc, "cursor", cursor, 0, errorOut) ||
            !read_uint64_field(doc, "materializationCount", count, 0, errorOut)) {
            return false;
        }
        parsed.cellX = static_cast<std::int64_t>(cellX);
        parsed.cellZ = static_cast<std::int64_t>(cellZ);
        parsed.cursor = static_cast<std::size_t>(cursor);
        parsed.materializationCount = count;
        const auto* completeField = doc.field("complete");
        if (completeField == nullptr ||
            completeField->kind != engine::sdk::JsonValue::Kind::Bool) {
            errorOut = "state document must have a complete boolean";
            return false;
        }
        parsed.complete = completeField->boolean;
        const auto* pendingField = doc.field("pending");
        if (pendingField == nullptr ||
            pendingField->kind != engine::sdk::JsonValue::Kind::Array) {
            errorOut = "state document must have a pending array";
            return false;
        }
        for (const engine::sdk::JsonValue& effectDoc : pendingField->array) {
            MaterializedEffect effect;
            if (!effect_from_json(effectDoc, effect, errorOut)) return false;
            parsed.pending.push_back(effect);
        }
        if (parsed.cursor > parsed.pending.size()) {
            errorOut = "corrupt cursor in state document";
            return false;
        }
        // All-or-nothing: only a fully validated document replaces `out`.
        out = parsed;
        return true;
    }

private:
    // The matching rule: the first rule whose tag is present in the region's
    // tags; a default rule (empty tag) matches any region without an explicit
    // rule. nullptr when no rule matches.
    const ReconcilerRule* rule_for(const ReconcilerMacroState& macro) const {
        const ReconcilerRule* fallback = nullptr;
        for (const ReconcilerRule& rule : rules_) {
            if (rule.tag.empty()) {
                if (fallback == nullptr) fallback = &rule;
                continue;
            }
            for (const std::string& tag : macro.tags) {
                if (tag == rule.tag) return &rule;
            }
        }
        return fallback;
    }

    static int growth_stage(const ReconcilerMacroState& macro,
                            const ReconcilerRule& rule) {
        const int maxStage = std::max(1, rule.maxGrowthStages);
        const int offset = static_cast<int>(
            std::floor(macro.growth * static_cast<float>(maxStage - 1)));
        return std::max(1, std::min(maxStage, 1 + offset));
    }

    static void cell_position(const ReconcilerMacroState& macro,
                              std::uint64_t index, float& outX, float& outZ) {
        const std::uint64_t h = mix64(mix64(macro.seed) + index);
        const float u = static_cast<float>(h % 1000u) / 1000.0f;
        const float v = static_cast<float>((h >> 32) % 1000u) / 1000.0f;
        outX = static_cast<float>(macro.cellX) * macro.cellSize +
               (0.15f + 0.7f * u) * macro.cellSize;
        outZ = static_cast<float>(macro.cellZ) * macro.cellSize +
               (0.15f + 0.7f * v) * macro.cellSize;
    }

    static std::uint64_t macro_fingerprint(const ReconcilerMacroState& macro) {
        std::uint64_t h = macro.seed;
        h = mix64(h ^ float_to_bits(macro.population));
        h = mix64(h ^ float_to_bits(macro.previousPopulation));
        h = mix64(h ^ float_to_bits(macro.resources));
        h = mix64(h ^ float_to_bits(macro.growth));
        for (const std::string& tag : macro.tags) {
            for (const char c : tag) {
                h = mix64(h ^ static_cast<std::uint64_t>(
                                 static_cast<unsigned char>(c)));
            }
        }
        return h;
    }

    static JsonValue effect_json(const MaterializedEffect& effect) {
        std::map<std::string, JsonValue> fields;
        fields["kind"] = JsonValue::text(kind_name(effect.kind));
        switch (effect.kind) {
            case MaterializedEffect::Kind::SpawnEntity:
                fields["archetypeId"] = JsonValue::text(effect.archetypeId);
                fields["positionX"] = JsonValue::number(effect.positionX);
                fields["positionZ"] = JsonValue::number(effect.positionZ);
                break;
            case MaterializedEffect::Kind::ResourceDrop:
                fields["itemId"] = JsonValue::text(effect.itemId);
                fields["positionX"] = JsonValue::number(effect.positionX);
                fields["positionZ"] = JsonValue::number(effect.positionZ);
                break;
            case MaterializedEffect::Kind::GrowthStage:
                fields["blockId"] = JsonValue::text(effect.blockId);
                fields["blockX"] = JsonValue::number(effect.blockX);
                fields["blockY"] = JsonValue::number(effect.blockY);
                fields["blockZ"] = JsonValue::number(effect.blockZ);
                fields["growthStage"] = JsonValue::number(effect.growthStage);
                break;
            case MaterializedEffect::Kind::RemoveEntity:
                fields["handle"] = JsonValue::text(effect.handle);
                fields["reason"] = JsonValue::text(effect.reason);
                break;
        }
        return JsonValue::json_object(std::move(fields));
    }

    static bool read_uint64_field(const engine::sdk::JsonValue& doc,
                                  const std::string& key, std::uint64_t& out,
                                  std::uint64_t defaultValue,
                                  std::string& errorOut) {
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
        if (field->is_string() &&
            parse_uint64(field->string, out)) {
            return true;
        }
        errorOut = "field '" + key + "' must be a non-negative integer";
        return false;
    }

    static bool effect_from_json(const engine::sdk::JsonValue& doc,
                                 MaterializedEffect& out,
                                 std::string& errorOut) {
        if (!doc.is_object()) {
            errorOut = "each pending effect must be an object";
            return false;
        }
        const std::string kindName = engine::sdk::json_string(doc, "kind", "");
        if (!kind_from_name(kindName, out.kind)) {
            errorOut = "unknown effect kind: " + kindName;
            return false;
        }
        switch (out.kind) {
            case MaterializedEffect::Kind::SpawnEntity:
                out.archetypeId = engine::sdk::json_string(doc, "archetypeId", "");
                out.positionX = static_cast<float>(
                    engine::sdk::json_number(doc, "positionX", 0.0));
                out.positionZ = static_cast<float>(
                    engine::sdk::json_number(doc, "positionZ", 0.0));
                break;
            case MaterializedEffect::Kind::ResourceDrop:
                out.itemId = engine::sdk::json_string(doc, "itemId", "");
                out.positionX = static_cast<float>(
                    engine::sdk::json_number(doc, "positionX", 0.0));
                out.positionZ = static_cast<float>(
                    engine::sdk::json_number(doc, "positionZ", 0.0));
                break;
            case MaterializedEffect::Kind::GrowthStage:
                out.blockId = engine::sdk::json_string(doc, "blockId", "");
                if (!read_int_field(doc, "blockX", out.blockX, 0, errorOut) ||
                    !read_int_field(doc, "blockY", out.blockY, 0, errorOut) ||
                    !read_int_field(doc, "blockZ", out.blockZ, 0, errorOut) ||
                    !read_int_field(doc, "growthStage", out.growthStage, 1,
                                    errorOut)) {
                    return false;
                }
                break;
            case MaterializedEffect::Kind::RemoveEntity:
                out.handle = engine::sdk::json_string(doc, "handle", "");
                out.reason = engine::sdk::json_string(doc, "reason", "");
                break;
        }
        if (!valid_effect(out)) {
            errorOut = "pending effect is incomplete";
            return false;
        }
        return true;
    }

    static bool read_int_field(const engine::sdk::JsonValue& doc,
                               const std::string& key, int& out,
                               int defaultValue, std::string& errorOut) {
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

    std::vector<ReconcilerRule> rules_;
    ReconcilerBudget budget_;
    bool budgetSet_{ false };
};

}  // namespace

std::unique_ptr<IMacroMicroReconciler> create_macro_micro_reconciler() {
    return std::unique_ptr<IMacroMicroReconciler>(
        new MacroMicroReconciler());
}

}  // namespace simulation
}  // namespace engine

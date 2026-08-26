// AbilitySystem.cpp — the only TU implementing the public ability/powers
// contract (FALTANTES §19). AbilityDefinition is PURE DATA (versioned JSON,
// all-or-nothing, bit-exact %.9g round-trip — the VehicleAsset pattern); the
// runtime applies it through the IAbilityWorld seam (voxel block writes,
// physics bodies, attributes/cost/health) and reports presentation hooks as
// AbilityEvents. Sustained effects (Telekinesis hold, Flight thrust, Periodic
// ticks) are advanced deterministically by update(); casts can be cancelled /
// interrupted; the whole runtime state (cooldowns + active casts) serializes
// bit-exactly for persistence and network authority/prediction.
//
// The world is passed PER CALL (the MobBehavior::tick pattern), so the runtime
// is a pure function of (definition, world state, cast sequence) — the same
// sequence on the same world reproduces bit-exactly, and the same system
// instance can drive several worlds.
//
// Numeric validation uses BIT-LEVEL finite checks: the project compiles with
// /fp:fast (findings #79), which folds std::isfinite(NaN) to true.

#include "engine/gameplay/IAbilitySystem.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
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

bool finite_vec3(const glm::vec3& value) {
    return finite_float(value.x) && finite_float(value.y) && finite_float(value.z);
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

// ---- emitters ---------------------------------------------------------------

const char* condition_kind_name(AbilityConditionKind kind) {
    switch (kind) {
        case AbilityConditionKind::OwnerTag: return "ownerTag";
        case AbilityConditionKind::TargetTag: return "targetTag";
        case AbilityConditionKind::OwnerAttribute: return "ownerAttribute";
        case AbilityConditionKind::TargetAttribute: return "targetAttribute";
        case AbilityConditionKind::Distance: break;
    }
    return "distance";
}

const char* target_mode_name(AbilityTargetMode mode) {
    switch (mode) {
        case AbilityTargetMode::Self: return "self";
        case AbilityTargetMode::Direction: return "direction";
        case AbilityTargetMode::Point: return "point";
        case AbilityTargetMode::Body: break;
    }
    return "body";
}

const char* effect_type_name(AbilityEffectType type) {
    switch (type) {
        case AbilityEffectType::Damage: return "damage";
        case AbilityEffectType::Heal: return "heal";
        case AbilityEffectType::Impulse: return "impulse";
        case AbilityEffectType::Telekinesis: return "telekinesis";
        case AbilityEffectType::Flight: return "flight";
        case AbilityEffectType::BlockEdit: return "blockEdit";
        case AbilityEffectType::Periodic: break;
    }
    return "periodic";
}

std::string emit_ivec3(const glm::ivec3& v) {
    std::ostringstream out;
    out << "[" << v.x << "," << v.y << "," << v.z << "]";
    return out.str();
}

std::string emit_vec3(const glm::vec3& v) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "[" << v.x << "," << v.y << "," << v.z << "]";
    return out.str();
}

std::string emit_condition(const AbilityCondition& condition) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"kind\":\"" << condition_kind_name(condition.kind)
        << "\",\"tag\":\"" << json_escape(condition.tag)
        << "\",\"attribute\":\"" << json_escape(condition.attribute)
        << "\",\"minValue\":" << condition.minValue
        << ",\"maxDistance\":" << condition.maxDistance << '}';
    return out.str();
}

std::string emit_targeting(const AbilityTargeting& targeting) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"mode\":\"" << target_mode_name(targeting.mode)
        << "\",\"range\":" << targeting.range
        << ",\"radius\":" << targeting.radius << '}';
    return out.str();
}

std::string emit_effect(const AbilityEffect& effect) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"type\":\"" << effect_type_name(effect.type) << '"';
    switch (effect.type) {
        case AbilityEffectType::Damage:
        case AbilityEffectType::Heal:
            out << ",\"amount\":" << effect.amount;
            break;
        case AbilityEffectType::Impulse:
            out << ",\"force\":" << effect.force;
            break;
        case AbilityEffectType::Telekinesis:
            out << ",\"holdOffset\":[" << effect.holdOffsetX << ","
                << effect.holdOffsetY << "," << effect.holdOffsetZ << "]"
                << ",\"grabForce\":" << effect.grabForce
                << ",\"durationSeconds\":" << effect.durationSeconds;
            break;
        case AbilityEffectType::Flight:
            out << ",\"thrust\":" << effect.thrust
                << ",\"durationSeconds\":" << effect.durationSeconds;
            break;
        case AbilityEffectType::BlockEdit:
            out << ",\"min\":" << emit_ivec3(effect.min)
                << ",\"max\":" << emit_ivec3(effect.max)
                << ",\"blockId\":" << effect.blockId
                << ",\"relative\":" << (effect.relative ? "true" : "false");
            break;
        case AbilityEffectType::Periodic:
            out << ",\"intervalSeconds\":" << effect.intervalSeconds
                << ",\"ticks\":" << effect.ticks;
            if (effect.subEffect != nullptr) {
                out << ",\"subEffect\":" << emit_effect(*effect.subEffect);
            }
            break;
    }
    if (!effect.castAnimation.empty()) {
        out << ",\"castAnimation\":\"" << json_escape(effect.castAnimation) << '"';
    }
    if (!effect.particleEffect.empty()) {
        out << ",\"particleEffect\":\"" << json_escape(effect.particleEffect) << '"';
    }
    if (!effect.soundEffect.empty()) {
        out << ",\"soundEffect\":\"" << json_escape(effect.soundEffect) << '"';
    }
    out << '}';
    return out.str();
}

// ---- readers ----------------------------------------------------------------

bool read_vec3(const sdk::JsonValue& object, const std::string& key,
               glm::vec3& out) {
    const sdk::JsonValue* value = object.field(key);
    if (value == nullptr || value->kind != sdk::JsonValue::Kind::Array ||
        value->array.size() != 3) {
        return false;
    }
    for (const sdk::JsonValue& component : value->array) {
        if (component.kind != sdk::JsonValue::Kind::Number) return false;
    }
    out = {static_cast<float>(value->array[0].number),
           static_cast<float>(value->array[1].number),
           static_cast<float>(value->array[2].number)};
    return finite_vec3(out);
}

bool read_ivec3(const sdk::JsonValue& object, const std::string& key,
                glm::ivec3& out) {
    const sdk::JsonValue* value = object.field(key);
    if (value == nullptr || value->kind != sdk::JsonValue::Kind::Array ||
        value->array.size() != 3) {
        return false;
    }
    for (const sdk::JsonValue& component : value->array) {
        if (component.kind != sdk::JsonValue::Kind::Number) return false;
    }
    out = {static_cast<int>(value->array[0].number),
           static_cast<int>(value->array[1].number),
           static_cast<int>(value->array[2].number)};
    return true;
}

bool parse_effect(const sdk::JsonValue& value, AbilityEffect& out,
                  std::string& errorOut);

bool parse_effect(const sdk::JsonValue& value, AbilityEffect& out,
                  std::string& errorOut) {
    if (!value.is_object()) {
        errorOut = "ability effect: each effect must be an object";
        return false;
    }
    const std::string type = sdk::json_string(value, "type", "");
    AbilityEffect effect;
    if (type == "damage") {
        effect.type = AbilityEffectType::Damage;
        effect.amount = static_cast<float>(sdk::json_number(value, "amount", 0.0));
    } else if (type == "heal") {
        effect.type = AbilityEffectType::Heal;
        effect.amount = static_cast<float>(sdk::json_number(value, "amount", 0.0));
    } else if (type == "impulse") {
        effect.type = AbilityEffectType::Impulse;
        effect.force = static_cast<float>(sdk::json_number(value, "force", 0.0));
    } else if (type == "telekinesis") {
        effect.type = AbilityEffectType::Telekinesis;
        glm::vec3 hold{0.0f, 1.5f, 0.0f};
        if (!read_vec3(value, "holdOffset", hold)) {
            errorOut = "ability effect: telekinesis holdOffset must be a finite [x,y,z] array";
            return false;
        }
        effect.holdOffsetX = hold.x;
        effect.holdOffsetY = hold.y;
        effect.holdOffsetZ = hold.z;
        effect.grabForce = static_cast<float>(sdk::json_number(value, "grabForce", 240.0));
        effect.durationSeconds = static_cast<float>(sdk::json_number(value, "durationSeconds", 0.0));
    } else if (type == "flight") {
        effect.type = AbilityEffectType::Flight;
        effect.thrust = static_cast<float>(sdk::json_number(value, "thrust", 320.0));
        effect.durationSeconds = static_cast<float>(sdk::json_number(value, "durationSeconds", 0.0));
    } else if (type == "blockEdit") {
        effect.type = AbilityEffectType::BlockEdit;
        if (!read_ivec3(value, "min", effect.min) ||
            !read_ivec3(value, "max", effect.max)) {
            errorOut = "ability effect: blockEdit min/max must be [x,y,z] integer arrays";
            return false;
        }
        effect.blockId = static_cast<std::uint32_t>(
            std::lround(sdk::json_number(value, "blockId", 1.0)));
        effect.relative = sdk::json_bool(value, "relative", true);
    } else if (type == "periodic") {
        effect.type = AbilityEffectType::Periodic;
        effect.intervalSeconds = static_cast<float>(
            sdk::json_number(value, "intervalSeconds", 0.5));
        effect.ticks = static_cast<int>(std::lround(
            sdk::json_number(value, "ticks", 4.0)));
        const sdk::JsonValue* sub = value.field("subEffect");
        if (sub != nullptr) {
            AbilityEffect subEffect;
            if (!parse_effect(*sub, subEffect, errorOut)) return false;
            effect.subEffect =
                std::make_shared<AbilityEffect>(std::move(subEffect));
        }
    } else {
        errorOut = "ability effect: unknown type '" + type + "'";
        return false;
    }
    effect.castAnimation = sdk::json_string(value, "castAnimation", "");
    effect.particleEffect = sdk::json_string(value, "particleEffect", "");
    effect.soundEffect = sdk::json_string(value, "soundEffect", "");
    out = std::move(effect);
    return true;
}

// ---- validation ---------------------------------------------------------------

constexpr std::int64_t kMaxBlockEditVolume = 4096;

bool validate_effect(const AbilityEffect& effect, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "ability effect: " + message;
        return false;
    };
    switch (effect.type) {
        case AbilityEffectType::Damage:
        case AbilityEffectType::Heal:
            if (!finite_float(effect.amount) || effect.amount < 0.0f) {
                return fail("amount must be finite and >= 0");
            }
            break;
        case AbilityEffectType::Impulse:
            if (!finite_float(effect.force) || effect.force < 0.0f) {
                return fail("force must be finite and >= 0");
            }
            break;
        case AbilityEffectType::Telekinesis:
            if (!finite_float(effect.grabForce) || effect.grabForce < 0.0f) {
                return fail("grabForce must be finite and >= 0");
            }
            if (!finite_float(effect.holdOffsetX) ||
                !finite_float(effect.holdOffsetY) ||
                !finite_float(effect.holdOffsetZ) ||
                !finite_float(effect.durationSeconds)) {
                return fail("telekinesis values must be finite");
            }
            if (effect.durationSeconds < 0.0f) {
                return fail("durationSeconds must be >= 0");
            }
            break;
        case AbilityEffectType::Flight:
            if (!finite_float(effect.thrust) || effect.thrust < 0.0f) {
                return fail("thrust must be finite and >= 0");
            }
            if (!finite_float(effect.durationSeconds) ||
                effect.durationSeconds < 0.0f) {
                return fail("durationSeconds must be finite and >= 0");
            }
            break;
        case AbilityEffectType::BlockEdit: {
            const std::int64_t dx = static_cast<std::int64_t>(effect.max.x) -
                                    static_cast<std::int64_t>(effect.min.x) + 1;
            const std::int64_t dy = static_cast<std::int64_t>(effect.max.y) -
                                    static_cast<std::int64_t>(effect.min.y) + 1;
            const std::int64_t dz = static_cast<std::int64_t>(effect.max.z) -
                                    static_cast<std::int64_t>(effect.min.z) + 1;
            if (dx <= 0 || dy <= 0 || dz <= 0) {
                return fail("blockEdit box must have max >= min per axis");
            }
            if (dx * dy * dz > kMaxBlockEditVolume) {
                return fail("blockEdit box exceeds the " +
                            std::to_string(kMaxBlockEditVolume) +
                            " cell volume limit");
            }
            break;
        }
        case AbilityEffectType::Periodic:
            if (!finite_float(effect.intervalSeconds) ||
                effect.intervalSeconds <= 0.0f) {
                return fail("periodic intervalSeconds must be finite and > 0");
            }
            if (effect.ticks < 1) return fail("periodic ticks must be >= 1");
            if (effect.subEffect != nullptr &&
                !validate_effect(*effect.subEffect, errorOut)) {
                return false;
            }
            break;
    }
    return true;
}

bool validate_definition(const AbilityDefinition& definition,
                         std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "ability asset: " + message;
        return false;
    };
    if (definition.name.empty()) return fail("name must not be empty");
    if (definition.version != 1) return fail("unsupported version");
    if (!finite_float(definition.cooldownSeconds) ||
        definition.cooldownSeconds < 0.0f) {
        return fail("cooldownSeconds must be finite and >= 0");
    }
    if (!finite_float(definition.cost.amount) || definition.cost.amount < 0.0f) {
        return fail("cost amount must be finite and >= 0");
    }
    if (!finite_float(definition.targeting.range) ||
        definition.targeting.range < 0.0f) {
        return fail("targeting range must be finite and >= 0");
    }
    if (!finite_float(definition.targeting.radius) ||
        definition.targeting.radius < 0.0f) {
        return fail("targeting radius must be finite and >= 0");
    }
    for (const AbilityAttribute& attribute : definition.attributes) {
        if (attribute.name.empty()) return fail("attribute name must not be empty");
        if (!finite_float(attribute.value)) return fail("attribute value must be finite");
    }
    for (const AbilityCondition& condition : definition.conditions) {
        if ((condition.kind == AbilityConditionKind::OwnerTag ||
             condition.kind == AbilityConditionKind::TargetTag) &&
            condition.tag.empty()) {
            return fail("tag condition requires a tag");
        }
        if ((condition.kind == AbilityConditionKind::OwnerAttribute ||
             condition.kind == AbilityConditionKind::TargetAttribute) &&
            condition.attribute.empty()) {
            return fail("attribute condition requires an attribute name");
        }
        if (condition.kind == AbilityConditionKind::Distance &&
            (!finite_float(condition.maxDistance) ||
             condition.maxDistance < 0.0f)) {
            return fail("distance condition maxDistance must be finite and >= 0");
        }
    }
    if (definition.effects.empty()) return fail("at least one effect is required");
    for (const AbilityEffect& effect : definition.effects) {
        if (!validate_effect(effect, errorOut)) return false;
    }
    return true;
}

std::string emit_definition_body(const AbilityDefinition& definition) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"id\":\"" << json_escape(definition.id)
        << "\",\"name\":\"" << json_escape(definition.name)
        << "\",\"version\":" << definition.version
        << ",\"attributes\":[";
    for (std::size_t i = 0; i < definition.attributes.size(); ++i) {
        if (i) out << ",";
        out << "{\"name\":\"" << json_escape(definition.attributes[i].name)
            << "\",\"value\":" << definition.attributes[i].value << '}';
    }
    out << "],\"tags\":[";
    for (std::size_t i = 0; i < definition.tags.size(); ++i) {
        if (i) out << ",";
        out << '"' << json_escape(definition.tags[i]) << '"';
    }
    out << "],\"cost\":{\"resource\":\"" << json_escape(definition.cost.resource)
        << "\",\"amount\":" << definition.cost.amount << '}'
        << ",\"cooldownSeconds\":" << definition.cooldownSeconds
        << ",\"conditions\":[";
    for (std::size_t i = 0; i < definition.conditions.size(); ++i) {
        if (i) out << ",";
        out << emit_condition(definition.conditions[i]);
    }
    out << "],\"targeting\":" << emit_targeting(definition.targeting)
        << ",\"effects\":[";
    for (std::size_t i = 0; i < definition.effects.size(); ++i) {
        if (i) out << ",";
        out << emit_effect(definition.effects[i]);
    }
    out << "],\"cancelable\":" << (definition.cancelable ? "true" : "false")
        << ",\"interruptible\":" << (definition.interruptible ? "true" : "false")
        << '}';
    return out.str();
}

}  // namespace

bool AbilityDefinition::load_from_json(const std::string& jsonText,
                                       std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "ability asset: root must be an object";
        return false;
    }

    AbilityDefinition parsed;
    parsed.name = sdk::json_string(root, "name", "");
    parsed.version = static_cast<int>(sdk::json_number(root, "version", 1));
    const std::string id = sdk::json_string(root, "id", "");
    parsed.id = sdk::uuid_or_derived(id, "abilities:" + parsed.name);
    parsed.cooldownSeconds = static_cast<float>(
        sdk::json_number(root, "cooldownSeconds", 0.0));
    parsed.cancelable = sdk::json_bool(root, "cancelable", true);
    parsed.interruptible = sdk::json_bool(root, "interruptible", true);

    const sdk::JsonValue* attributesValue = root.field("attributes");
    if (attributesValue != nullptr) {
        if (attributesValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "ability asset: attributes must be an array";
            return false;
        }
        parsed.attributes.reserve(attributesValue->array.size());
        for (const sdk::JsonValue& item : attributesValue->array) {
            if (!item.is_object()) {
                errorOut = "ability asset: each attribute must be an object";
                return false;
            }
            AbilityAttribute attribute;
            attribute.name = sdk::json_string(item, "name", "");
            attribute.value = static_cast<float>(sdk::json_number(item, "value", 0.0));
            parsed.attributes.push_back(attribute);
        }
    }

    parsed.tags = sdk::json_string_array(root, "tags");

    const sdk::JsonValue* costValue = root.field("cost");
    if (costValue != nullptr) {
        if (!costValue->is_object()) {
            errorOut = "ability asset: cost must be an object";
            return false;
        }
        parsed.cost.resource = sdk::json_string(*costValue, "resource", "");
        parsed.cost.amount = static_cast<float>(
            sdk::json_number(*costValue, "amount", 0.0));
    }

    const sdk::JsonValue* conditionsValue = root.field("conditions");
    if (conditionsValue != nullptr) {
        if (conditionsValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "ability asset: conditions must be an array";
            return false;
        }
        parsed.conditions.reserve(conditionsValue->array.size());
        for (const sdk::JsonValue& item : conditionsValue->array) {
            if (!item.is_object()) {
                errorOut = "ability asset: each condition must be an object";
                return false;
            }
            AbilityCondition condition;
            const std::string kind = sdk::json_string(item, "kind", "");
            if (kind == "ownerTag") {
                condition.kind = AbilityConditionKind::OwnerTag;
            } else if (kind == "targetTag") {
                condition.kind = AbilityConditionKind::TargetTag;
            } else if (kind == "ownerAttribute") {
                condition.kind = AbilityConditionKind::OwnerAttribute;
            } else if (kind == "targetAttribute") {
                condition.kind = AbilityConditionKind::TargetAttribute;
            } else if (kind == "distance") {
                condition.kind = AbilityConditionKind::Distance;
            } else {
                errorOut = "ability asset: unknown condition kind '" + kind + "'";
                return false;
            }
            condition.tag = sdk::json_string(item, "tag", "");
            condition.attribute = sdk::json_string(item, "attribute", "");
            condition.minValue = static_cast<float>(
                sdk::json_number(item, "minValue", 0.0));
            condition.maxDistance = static_cast<float>(
                sdk::json_number(item, "maxDistance", 0.0));
            parsed.conditions.push_back(condition);
        }
    }

    const sdk::JsonValue* targetingValue = root.field("targeting");
    if (targetingValue != nullptr) {
        if (!targetingValue->is_object()) {
            errorOut = "ability asset: targeting must be an object";
            return false;
        }
        const std::string mode = sdk::json_string(*targetingValue, "mode", "self");
        if (mode == "self") {
            parsed.targeting.mode = AbilityTargetMode::Self;
        } else if (mode == "direction") {
            parsed.targeting.mode = AbilityTargetMode::Direction;
        } else if (mode == "point") {
            parsed.targeting.mode = AbilityTargetMode::Point;
        } else if (mode == "body") {
            parsed.targeting.mode = AbilityTargetMode::Body;
        } else {
            errorOut = "ability asset: unknown targeting mode '" + mode + "'";
            return false;
        }
        parsed.targeting.range = static_cast<float>(
            sdk::json_number(*targetingValue, "range", 0.0));
        parsed.targeting.radius = static_cast<float>(
            sdk::json_number(*targetingValue, "radius", 0.0));
    }

    const sdk::JsonValue* effectsValue = root.field("effects");
    if (effectsValue == nullptr || effectsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "ability asset: effects must be an array";
        return false;
    }
    parsed.effects.reserve(effectsValue->array.size());
    for (const sdk::JsonValue& item : effectsValue->array) {
        AbilityEffect effect;
        if (!parse_effect(item, effect, errorOut)) return false;
        parsed.effects.push_back(std::move(effect));
    }

    std::string validationError;
    if (!validate_definition(parsed, validationError)) {
        errorOut = validationError;
        return false;
    }
    *this = std::move(parsed);
    return true;
}

std::string AbilityDefinition::to_json() const {
    return emit_definition_body(*this);
}

bool AbilityDefinition::validate(std::string& errorOut) const {
    return validate_definition(*this, errorOut);
}

// ---- runtime ----------------------------------------------------------------

namespace {

struct SustainedEffectState {
    AbilityEffect effect;
    float timer{ 0.0f };
    int ticksDone{ 0 };
};

struct ActiveCast {
    std::size_t castIndex{ 0 };
    std::string abilityId;
    AbilityBodyId caster;
    AbilityTarget target;
    float elapsed{ 0.0f };
    bool cancelled{ false };
    bool interrupted{ false };
    std::vector<SustainedEffectState> sustained;
};

class AbilitySystemImpl final : public IAbilitySystem {
public:
    bool register_ability(const AbilityDefinition& definition,
                          std::string& errorOut) override {
        if (definition.id.empty() || !validate_definition(definition, errorOut)) {
            if (errorOut.empty()) errorOut = "ability: invalid definition";
            return false;
        }
        if (abilities_.find(definition.id) != abilities_.end()) {
            errorOut = "ability: '" + definition.id + "' is already registered";
            return false;
        }
        abilities_[definition.id] = definition;
        // A fresh registration carries no stale cooldown (authority resets it).
        cooldowns_.erase(definition.id);
        return true;
    }

    bool unregister_ability(const std::string& id) override {
        const auto found = abilities_.find(id);
        if (found == abilities_.end()) return false;
        for (const ActiveCast& cast : activeCasts_) {
            if (cast.abilityId == id && !cast.cancelled) return false;
        }
        abilities_.erase(found);
        cooldowns_.erase(id);
        return true;
    }

    const AbilityDefinition* ability(const std::string& id) const override {
        const auto found = abilities_.find(id);
        return found == abilities_.end() ? nullptr : &found->second;
    }

    std::vector<std::string> ability_ids() const override {
        std::vector<std::string> ids;
        ids.reserve(abilities_.size());
        for (const auto& entry : abilities_) ids.push_back(entry.first);
        return ids;
    }

    CastResult cast(const std::string& abilityId, const AbilityBodyId& caster,
                    const AbilityTarget& target,
                    IAbilityWorld& world) override {
        CastResult result;
        const auto found = abilities_.find(abilityId);
        if (found == abilities_.end()) {
            result.error = "ability: unknown ability '" + abilityId + "'";
            return result;
        }
        if (!caster.valid()) {
            result.error = "ability: caster body is invalid";
            return result;
        }
        const AbilityDefinition& definition = found->second;

        // Conditions (AND) — evaluated against the world seam.
        if (!conditions_pass(definition, caster, target, world)) {
            result.error = "ability: conditions not met";
            return result;
        }
        // Cooldown.
        const auto cooldown = cooldowns_.find(abilityId);
        if (cooldown != cooldowns_.end() && cooldown->second > 0.0f) {
            result.error = "ability: on cooldown";
            return result;
        }
        // Cost — spent through the seam; a refusal rejects the whole cast
        // (nothing applies).
        if (!definition.cost.resource.empty() && definition.cost.amount > 0.0f) {
            if (!world.spend_cost(caster, definition.cost.resource,
                                  definition.cost.amount)) {
                result.error = "ability: cost not affordable";
                return result;
            }
        }

        // Committed: cooldown starts now.
        cooldowns_[abilityId] = definition.cooldownSeconds;

        ActiveCast cast;
        cast.castIndex = nextCastIndex_++;
        cast.abilityId = abilityId;
        cast.caster = caster;
        cast.target = target;
        for (const AbilityEffect& effect : definition.effects) {
            apply_effect(effect, caster, target, cast, world);
        }
        // Only SUSTAINED casts stay active (Telekinesis/Flight/Periodic); an
        // instant cast (Damage/Heal/Impulse/BlockEdit) resolves immediately and
        // is not tracked — cancel()/snapshot() only see live casts. castIndex
        // is still unique per accepted cast (even instant ones), so a project
        // never mistakes an instant cast handle for a cancellable one.
        const bool sustained = !cast.sustained.empty();
        if (sustained) {
            activeCasts_.push_back(std::move(cast));
        }

        result.accepted = true;
        result.castIndex = sustained ? activeCasts_.back().castIndex : 0;
        result.effectCount = definition.effects.size();

        AbilityEvent event;
        event.kind = AbilityEvent::Kind::Cast;
        event.abilityId = abilityId;
        event.position = target.point;
        for (const AbilityEffect& effect : definition.effects) {
            if (!effect.castAnimation.empty()) event.animation = effect.castAnimation;
            if (!effect.particleEffect.empty()) event.particle = effect.particleEffect;
            if (!effect.soundEffect.empty()) event.sound = effect.soundEffect;
        }
        emit(event);
        return result;
    }

    void update(float deltaTime, IAbilityWorld& world) override {
        // Bit-level finite check: /fp:fast folds std::isfinite(NaN) to true
        // (findings #79).
        if (!(deltaTime >= 0.0f) || !finite_float(deltaTime)) return;

        for (auto& entry : cooldowns_) {
            entry.second = std::max(0.0f, entry.second - deltaTime);
        }

        std::vector<std::size_t> finished;
        for (ActiveCast& cast : activeCasts_) {
            if (cast.cancelled) continue;
            cast.elapsed += deltaTime;
            for (SustainedEffectState& state : cast.sustained) {
                advance_sustained(cast, state, deltaTime, world);
            }
            const float duration = cast_duration(cast);
            if ((duration > 0.0f && cast.elapsed >= duration) ||
                periodic_done(cast)) {
                finished.push_back(cast.castIndex);
            }
        }
        for (const std::size_t index : finished) {
            finish_cast(index, world, AbilityEvent::Kind::Finished);
        }
    }

    bool cancel(std::size_t castIndex, IAbilityWorld& world,
                std::string& errorOut) override {
        return stop_cast(castIndex, world, AbilityEvent::Kind::Cancelled, false,
                         errorOut);
    }

    bool interrupt(std::size_t castIndex, IAbilityWorld& world,
                   std::string& errorOut) override {
        return stop_cast(castIndex, world, AbilityEvent::Kind::Interrupted, true,
                         errorOut);
    }

    float cooldown_remaining(const std::string& abilityId) const override {
        const auto found = cooldowns_.find(abilityId);
        return found == cooldowns_.end() ? 0.0f : found->second;
    }

    bool on_cooldown(const std::string& abilityId) const override {
        return cooldown_remaining(abilityId) > 0.0f;
    }

    std::size_t active_cast_count() const override {
        std::size_t count = 0;
        for (const ActiveCast& cast : activeCasts_) {
            if (!cast.cancelled) ++count;
        }
        return count;
    }

    ActiveCastInfo active_cast(std::size_t castIndex) const override {
        for (const ActiveCast& cast : activeCasts_) {
            if (cast.castIndex == castIndex) return to_info(cast);
        }
        return ActiveCastInfo{};
    }

    AbilityStateSnapshot snapshot() const override {
        AbilityStateSnapshot snapshot;
        snapshot.nextCastIndex = nextCastIndex_;
        snapshot.cooldowns = cooldowns_;
        for (const ActiveCast& cast : activeCasts_) {
            if (!cast.cancelled) snapshot.activeCasts.push_back(to_info(cast));
        }
        return snapshot;
    }

    bool apply_snapshot(const AbilityStateSnapshot& snapshot,
                        std::string& errorOut) override {
        for (const auto& entry : snapshot.cooldowns) {
            if (abilities_.find(entry.first) == abilities_.end()) {
                errorOut = "ability state: cooldown for unknown ability '" +
                           entry.first + "'";
                return false;
            }
            if (!finite_float(entry.second) || entry.second < 0.0f) {
                errorOut = "ability state: non-finite/negative cooldown";
                return false;
            }
        }
        for (const ActiveCastInfo& info : snapshot.activeCasts) {
            if (abilities_.find(info.abilityId) == abilities_.end()) {
                errorOut = "ability state: active cast of unknown ability '" +
                           info.abilityId + "'";
                return false;
            }
            if (info.castIndex >= snapshot.nextCastIndex) {
                errorOut = "ability state: active cast index out of range";
                return false;
            }
        }
        cooldowns_ = snapshot.cooldowns;
        activeCasts_.clear();
        activeCasts_.reserve(snapshot.activeCasts.size());
        for (const ActiveCastInfo& info : snapshot.activeCasts) {
            ActiveCast cast;
            cast.castIndex = info.castIndex;
            cast.abilityId = info.abilityId;
            cast.caster = info.caster;
            cast.target = info.target;
            cast.elapsed = info.elapsedSeconds;
            rebuild_sustained(cast);
            activeCasts_.push_back(std::move(cast));
        }
        nextCastIndex_ = snapshot.nextCastIndex;
        return true;
    }

    std::string serialize_state(std::string& errorOut) const override {
        return serialize_ability_state(snapshot(), errorOut);
    }

    bool deserialize_state(const std::string& data,
                           std::string& errorOut) override {
        AbilityStateSnapshot snapshot;
        if (!deserialize_ability_state(data, snapshot, errorOut)) return false;
        return apply_snapshot(snapshot, errorOut);
    }

    void set_event_sink(EventSink sink) override { sink_ = std::move(sink); }

private:
    bool conditions_pass(const AbilityDefinition& definition,
                         const AbilityBodyId& caster,
                         const AbilityTarget& target,
                         IAbilityWorld& world) const {
        for (const AbilityCondition& condition : definition.conditions) {
            switch (condition.kind) {
                case AbilityConditionKind::OwnerTag: {
                    const AbilityTagList tags = world.tags(caster);
                    if (std::find(tags.begin(), tags.end(), condition.tag) ==
                        tags.end()) {
                        return false;
                    }
                    break;
                }
                case AbilityConditionKind::TargetTag: {
                    if (!target.body.valid()) return false;
                    const AbilityTagList tags = world.tags(target.body);
                    if (std::find(tags.begin(), tags.end(), condition.tag) ==
                        tags.end()) {
                        return false;
                    }
                    break;
                }
                case AbilityConditionKind::OwnerAttribute: {
                    if (world.attribute(caster, condition.attribute) <
                        condition.minValue) {
                        return false;
                    }
                    break;
                }
                case AbilityConditionKind::TargetAttribute: {
                    if (!target.body.valid()) return false;
                    if (world.attribute(target.body, condition.attribute) <
                        condition.minValue) {
                        return false;
                    }
                    break;
                }
                case AbilityConditionKind::Distance: {
                    AbilityBodyState state;
                    if (!world.body_state(caster, state)) return false;
                    const float dx = target.point.x - state.position.x;
                    const float dy = target.point.y - state.position.y;
                    const float dz = target.point.z - state.position.z;
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) >
                        condition.maxDistance) {
                        return false;
                    }
                    break;
                }
            }
        }
        return true;
    }

    void apply_effect(const AbilityEffect& effect, const AbilityBodyId& caster,
                      const AbilityTarget& target, ActiveCast& cast,
                      IAbilityWorld& world) {
        switch (effect.type) {
            case AbilityEffectType::Damage:
                if (target.body.valid()) world.damage(target.body, effect.amount);
                break;
            case AbilityEffectType::Heal:
                if (target.body.valid()) world.heal(target.body, effect.amount);
                break;
            case AbilityEffectType::Impulse:
                if (target.body.valid()) {
                    world.apply_impulse(target.body, target.direction * effect.force);
                }
                break;
            case AbilityEffectType::BlockEdit: {
                const glm::ivec3 origin{
                    static_cast<int>(std::floor(target.point.x)),
                    static_cast<int>(std::floor(target.point.y)),
                    static_cast<int>(std::floor(target.point.z))};
                for (int y = effect.min.y; y <= effect.max.y; ++y) {
                    for (int z = effect.min.z; z <= effect.max.z; ++z) {
                        for (int x = effect.min.x; x <= effect.max.x; ++x) {
                            const glm::ivec3 cell = effect.relative
                                ? origin + glm::ivec3(x, y, z)
                                : glm::ivec3(x, y, z);
                            world.set_block(cell.x, cell.y, cell.z, effect.blockId);
                        }
                    }
                }
                break;
            }
            case AbilityEffectType::Telekinesis:
            case AbilityEffectType::Flight:
            case AbilityEffectType::Periodic: {
                SustainedEffectState state;
                state.effect = effect;
                cast.sustained.push_back(std::move(state));
                break;
            }
        }
    }

    void advance_sustained(ActiveCast& cast, SustainedEffectState& state,
                           float deltaTime, IAbilityWorld& world) {
        switch (state.effect.type) {            case AbilityEffectType::Telekinesis: {
                if (!cast.target.body.valid() || !cast.caster.valid()) break;
                AbilityBodyState casterState;
                AbilityBodyState targetState;
                if (!world.body_state(cast.caster, casterState) ||
                    !world.body_state(cast.target.body, targetState)) {
                    break;
                }
                const glm::vec3 holdPoint = casterState.position +
                    glm::vec3(state.effect.holdOffsetX, state.effect.holdOffsetY,
                              state.effect.holdOffsetZ);
                const glm::vec3 error = holdPoint - targetState.position;
                // Damped spring toward the hold point (deterministic, no RNG).
                // The damping coefficient is derived from the spring stiffness
                // (near-critical for a unit-mass target) so the held body
                // converges instead of oscillating forever.
                const float damping =
                    2.0f * std::sqrt(std::max(state.effect.grabForce, 0.0f));
                const glm::vec3 force = error * state.effect.grabForce -
                                        targetState.linearVelocity * damping;
                world.add_force(cast.target.body, force);
                emit_tick(cast, state.effect);
                break;
            }
            case AbilityEffectType::Flight: {
                if (!cast.caster.valid()) break;
                world.add_force(cast.caster, glm::vec3(0.0f, state.effect.thrust, 0.0f));
                emit_tick(cast, state.effect);
                break;
            }
            case AbilityEffectType::Periodic: {
                state.timer += deltaTime;
                while (state.timer >= state.effect.intervalSeconds &&
                       state.ticksDone < state.effect.ticks) {
                    state.timer -= state.effect.intervalSeconds;
                    ++state.ticksDone;
                    if (state.effect.subEffect != nullptr) {
                        apply_periodic_sub(*state.effect.subEffect, cast, world);
                    }
                    emit_tick(cast, state.effect);
                }
                break;
            }
            default:
                break;
        }
    }

    void apply_periodic_sub(const AbilityEffect& sub, ActiveCast& cast,
                            IAbilityWorld& world) {
        switch (sub.type) {
            case AbilityEffectType::Damage:
                if (cast.target.body.valid()) world.damage(cast.target.body, sub.amount);
                break;
            case AbilityEffectType::Heal:
                if (cast.target.body.valid()) world.heal(cast.target.body, sub.amount);
                break;
            case AbilityEffectType::Impulse:
                if (cast.target.body.valid()) {
                    world.apply_impulse(cast.target.body,
                                        cast.target.direction * sub.force);
                }
                break;
            case AbilityEffectType::BlockEdit: {
                const glm::ivec3 origin{
                    static_cast<int>(std::floor(cast.target.point.x)),
                    static_cast<int>(std::floor(cast.target.point.y)),
                    static_cast<int>(std::floor(cast.target.point.z))};
                for (int y = sub.min.y; y <= sub.max.y; ++y) {
                    for (int z = sub.min.z; z <= sub.max.z; ++z) {
                        for (int x = sub.min.x; x <= sub.max.x; ++x) {
                            const glm::ivec3 cell = sub.relative
                                ? origin + glm::ivec3(x, y, z)
                                : glm::ivec3(x, y, z);
                            world.set_block(cell.x, cell.y, cell.z, sub.blockId);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    float cast_duration(const ActiveCast& cast) const {
        float duration = 0.0f;
        for (const SustainedEffectState& state : cast.sustained) {
            const AbilityEffect& effect = state.effect;
            if ((effect.type == AbilityEffectType::Telekinesis ||
                 effect.type == AbilityEffectType::Flight) &&
                effect.durationSeconds > 0.0f) {
                duration = std::max(duration, effect.durationSeconds);
            }
        }
        return duration;
    }

    // True when every PERIODIC effect of the cast has delivered all its ticks
    // (the cast then finishes — the periodic damage/heal/impulse schedule ran
    // out). Non-periodic sustained effects (Telekinesis/Flight without a
    // duration) keep the cast alive.
    bool periodic_done(const ActiveCast& cast) const {
        bool hasPeriodic = false;
        bool allDone = true;
        for (const SustainedEffectState& state : cast.sustained) {
            if (state.effect.type != AbilityEffectType::Periodic) continue;
            hasPeriodic = true;
            if (state.ticksDone < state.effect.ticks) allDone = false;
        }
        return hasPeriodic && allDone;
    }

    bool stop_cast(std::size_t castIndex, IAbilityWorld& world,
                   AbilityEvent::Kind kind, bool asInterrupt,
                   std::string& errorOut) {
        for (ActiveCast& cast : activeCasts_) {
            if (cast.castIndex != castIndex) continue;
            if (cast.cancelled) {
                errorOut = "ability: cast already finished";
                return false;
            }
            if (asInterrupt) {
                const AbilityDefinition* definition = ability(cast.abilityId);
                if (definition != nullptr && !definition->interruptible) {
                    errorOut = "ability: '" + cast.abilityId +
                               "' is not interruptible";
                    return false;
                }
            }
            cast.cancelled = true;
            cast.interrupted = asInterrupt;
            emit_event(kind, cast, world);
            return true;
        }
        errorOut = "ability: unknown cast index";
        return false;
    }

    void finish_cast(std::size_t castIndex, IAbilityWorld& world,
                     AbilityEvent::Kind kind) {
        for (ActiveCast& cast : activeCasts_) {
            if (cast.castIndex == castIndex && !cast.cancelled) {
                cast.cancelled = true;
                emit_event(kind, cast, world);
                return;
            }
        }
    }

    void rebuild_sustained(ActiveCast& cast) {
        const AbilityDefinition* definition = ability(cast.abilityId);
        if (definition == nullptr) return;
        cast.sustained.clear();
        for (const AbilityEffect& effect : definition->effects) {
            if (effect.type == AbilityEffectType::Telekinesis ||
                effect.type == AbilityEffectType::Flight ||
                effect.type == AbilityEffectType::Periodic) {
                SustainedEffectState state;
                state.effect = effect;
                cast.sustained.push_back(std::move(state));
            }
        }
    }

    ActiveCastInfo to_info(const ActiveCast& cast) const {
        ActiveCastInfo info;
        info.castIndex = cast.castIndex;
        info.abilityId = cast.abilityId;
        info.caster = cast.caster;
        info.target = cast.target;
        info.elapsedSeconds = cast.elapsed;
        info.durationSeconds = cast_duration(cast);
        info.cancelled = cast.cancelled;
        info.interrupted = cast.interrupted;
        return info;
    }

    void emit(const AbilityEvent& event) {
        if (sink_) sink_(event);
    }

    void emit_tick(const ActiveCast& cast, const AbilityEffect& effect) {
        AbilityEvent event;
        event.kind = AbilityEvent::Kind::EffectTick;
        event.abilityId = cast.abilityId;
        event.animation = effect.castAnimation;
        event.particle = effect.particleEffect;
        event.sound = effect.soundEffect;
        event.position = cast.target.point;
        emit(event);
    }

    void emit_event(AbilityEvent::Kind kind, const ActiveCast& cast,
                    IAbilityWorld& world) {
        AbilityEvent event;
        event.kind = kind;
        event.abilityId = cast.abilityId;
        event.position = cast.target.point;
        for (const SustainedEffectState& state : cast.sustained) {
            if (!state.effect.castAnimation.empty()) {
                event.animation = state.effect.castAnimation;
            }
            if (!state.effect.particleEffect.empty()) {
                event.particle = state.effect.particleEffect;
            }
            if (!state.effect.soundEffect.empty()) {
                event.sound = state.effect.soundEffect;
            }
        }
        emit(event);
    }

    std::map<std::string, AbilityDefinition> abilities_;
    std::map<std::string, float> cooldowns_;
    std::vector<ActiveCast> activeCasts_;
    std::size_t nextCastIndex_{ 1 };
    EventSink sink_;
};

}  // namespace

std::unique_ptr<IAbilitySystem> create_ability_system() {
    return std::make_unique<AbilitySystemImpl>();
}

// ---- state serialization -----------------------------------------------------

std::string serialize_ability_state(const AbilityStateSnapshot& snapshot,
                                    std::string& errorOut) {
    (void)errorOut;
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"nextCastIndex\":" << snapshot.nextCastIndex
        << ",\"cooldowns\":{";
    bool first = true;
    for (const auto& entry : snapshot.cooldowns) {
        if (!first) out << ",";
        first = false;
        out << '"' << json_escape(entry.first) << "\":" << entry.second;
    }
    out << "},\"activeCasts\":[";
    for (std::size_t i = 0; i < snapshot.activeCasts.size(); ++i) {
        if (i) out << ",";
        const ActiveCastInfo& info = snapshot.activeCasts[i];
        out << "{\"castIndex\":" << info.castIndex
            << ",\"abilityId\":\"" << json_escape(info.abilityId)
            << "\",\"caster\":" << info.caster.id
            << ",\"target\":{\"mode\":\"" << target_mode_name(info.target.mode)
            << "\",\"point\":" << emit_vec3(info.target.point)
            << ",\"direction\":" << emit_vec3(info.target.direction)
            << ",\"body\":" << info.target.body.id << '}'
            << ",\"elapsedSeconds\":" << info.elapsedSeconds
            << ",\"durationSeconds\":" << info.durationSeconds
            << ",\"cancelled\":" << (info.cancelled ? "true" : "false")
            << ",\"interrupted\":" << (info.interrupted ? "true" : "false")
            << '}';
    }
    out << "]}";
    return out.str();
}

bool deserialize_ability_state(const std::string& data,
                               AbilityStateSnapshot& out,
                               std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(data, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "ability state: root must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(root, "version", 1));
    if (version != 1) {
        errorOut = "ability state: unsupported version";
        return false;
    }
    AbilityStateSnapshot parsed;
    parsed.nextCastIndex = static_cast<std::size_t>(
        std::lround(sdk::json_number(root, "nextCastIndex", 1.0)));

    const sdk::JsonValue* cooldownsValue = root.field("cooldowns");
    if (cooldownsValue != nullptr) {
        if (!cooldownsValue->is_object()) {
            errorOut = "ability state: cooldowns must be an object";
            return false;
        }
        for (const auto& entry : cooldownsValue->object) {
            if (entry.second.kind != sdk::JsonValue::Kind::Number ||
                !finite_float(static_cast<float>(entry.second.number)) ||
                entry.second.number < 0.0) {
                errorOut = "ability state: invalid cooldown for '" + entry.first + "'";
                return false;
            }
            parsed.cooldowns[entry.first] = static_cast<float>(entry.second.number);
        }
    }

    const sdk::JsonValue* castsValue = root.field("activeCasts");
    if (castsValue != nullptr) {
        if (castsValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "ability state: activeCasts must be an array";
            return false;
        }
        parsed.activeCasts.reserve(castsValue->array.size());
        for (const sdk::JsonValue& item : castsValue->array) {
            if (!item.is_object()) {
                errorOut = "ability state: each active cast must be an object";
                return false;
            }
            ActiveCastInfo info;
            info.castIndex = static_cast<std::size_t>(
                std::lround(sdk::json_number(item, "castIndex", 0.0)));
            info.abilityId = sdk::json_string(item, "abilityId", "");
            info.caster.id = static_cast<std::uint32_t>(
                std::lround(sdk::json_number(item, "caster", 0.0)));
            const sdk::JsonValue* targetValue = item.field("target");
            if (targetValue != nullptr && targetValue->is_object()) {
                const std::string mode = sdk::json_string(*targetValue, "mode", "self");
                if (mode == "self") {
                    info.target.mode = AbilityTargetMode::Self;
                } else if (mode == "direction") {
                    info.target.mode = AbilityTargetMode::Direction;
                } else if (mode == "point") {
                    info.target.mode = AbilityTargetMode::Point;
                } else if (mode == "body") {
                    info.target.mode = AbilityTargetMode::Body;
                } else {
                    errorOut = "ability state: unknown target mode '" + mode + "'";
                    return false;
                }
                glm::vec3 point{0.0f};
                if (!read_vec3(*targetValue, "point", point)) {
                    errorOut = "ability state: target point must be a finite [x,y,z] array";
                    return false;
                }
                info.target.point = point;
                glm::vec3 direction{0.0f, 0.0f, -1.0f};
                if (!read_vec3(*targetValue, "direction", direction)) {
                    errorOut = "ability state: target direction must be a finite [x,y,z] array";
                    return false;
                }
                info.target.direction = direction;
                info.target.body.id = static_cast<std::uint32_t>(
                    std::lround(sdk::json_number(*targetValue, "body", 0.0)));
            }
            info.elapsedSeconds = static_cast<float>(
                sdk::json_number(item, "elapsedSeconds", 0.0));
            info.durationSeconds = static_cast<float>(
                sdk::json_number(item, "durationSeconds", 0.0));
            info.cancelled = sdk::json_bool(item, "cancelled", false);
            info.interrupted = sdk::json_bool(item, "interrupted", false);
            parsed.activeCasts.push_back(std::move(info));
        }
    }
    out = std::move(parsed);
    return true;
}

}  // namespace gameplay
}  // namespace engine

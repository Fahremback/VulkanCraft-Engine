// AbilityEffects.cpp — the only TU implementing the public ability effects
// contract (Agente 4 §5 item 59 CORE): validated effect specs emitted as
// public gameplay events. The event payload is a fixed deterministic byte
// layout: [kind(u8)][idLen(u8)][id bytes][magnitude(f32)][radius(f32)]
// [targetX/Y/Z(f32)][stacks(u32)][eventKindLen(u8)][eventKind bytes]
// [blockIdLen(u8)][blockId bytes][statusIdLen(u8)][statusId bytes].

#include "engine/gameplay/IAbilityEffects.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace engine {
namespace gameplay {

const char* ability_effect_kind_name(AbilityEffectKind kind) {
    switch (kind) {
        case AbilityEffectKind::ForceImpulse: return "force_impulse";
        case AbilityEffectKind::Field: return "field";
        case AbilityEffectKind::Teleport: return "teleport";
        case AbilityEffectKind::CreateBlock: return "create_block";
        case AbilityEffectKind::DestroyBlock: return "destroy_block";
        case AbilityEffectKind::Status: return "status";
        case AbilityEffectKind::Generic: return "generic";
    }
    return "generic";
}

namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool finite_vec3(const glm::vec3& v) {
    return finite_float(v.x) && finite_float(v.y) && finite_float(v.z);
}

std::uint16_t kind_code(AbilityEffectKind kind) {
    return static_cast<std::uint16_t>(kind) + 1;  // 1..7
}

bool check_spec(const AbilityEffectSpec& spec, std::string& errorOut) {
    if (spec.id.empty()) {
        errorOut = "ability effects: spec id must be non-empty";
        return false;
    }
    if (!finite_float(spec.magnitude) || spec.magnitude < 0.0f) {
        errorOut = "ability effects: spec '" + spec.id + "' magnitude must be >= 0";
        return false;
    }
    if (spec.kind == AbilityEffectKind::Field) {
        if (!finite_float(spec.radius) || spec.radius <= 0.0f) {
            errorOut = "ability effects: field '" + spec.id + "' needs radius > 0";
            return false;
        }
    }
    if (!finite_vec3(spec.target)) {
        errorOut = "ability effects: spec '" + spec.id + "' target must be finite";
        return false;
    }
    if (spec.kind == AbilityEffectKind::CreateBlock && spec.blockId.empty()) {
        errorOut = "ability effects: create_block '" + spec.id + "' needs blockId";
        return false;
    }
    if (spec.kind == AbilityEffectKind::Status) {
        if (spec.statusId.empty()) {
            errorOut = "ability effects: status '" + spec.id + "' needs statusId";
            return false;
        }
        if (spec.statusStacks == 0) {
            errorOut = "ability effects: status '" + spec.id + "' needs stacks >= 1";
            return false;
        }
    }
    if (spec.kind == AbilityEffectKind::Generic && spec.eventKind.empty()) {
        errorOut = "ability effects: generic '" + spec.id + "' needs eventKind";
        return false;
    }
    return true;
}

void append_string(std::vector<std::uint8_t>& out, const std::string& text) {
    out.push_back(static_cast<std::uint8_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void append_f32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
}

std::vector<std::uint8_t> serialize(const AbilityEffectSpec& spec) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(spec.kind));
    append_string(out, spec.id);
    append_f32(out, spec.magnitude);
    append_f32(out, spec.radius);
    append_f32(out, spec.target.x);
    append_f32(out, spec.target.y);
    append_f32(out, spec.target.z);
    append_u32(out, spec.statusStacks);
    append_string(out, spec.eventKind);
    append_string(out, spec.blockId);
    append_string(out, spec.statusId);
    return out;
}

class AbilityEffects final : public IAbilityEffects {
public:
    AbilityEffects() = default;

    bool configure(const std::vector<AbilityEffectSpec>& specs,
                   std::string& errorOut) override {
        std::map<std::string, AbilityEffectSpec> parsed;
        for (const AbilityEffectSpec& spec : specs) {
            if (!check_spec(spec, errorOut)) return false;
            if (parsed.count(spec.id) != 0) {
                errorOut = "ability effects: duplicate spec id '" + spec.id + "'";
                return false;
            }
            parsed[spec.id] = spec;
        }
        specs_ = std::move(parsed);
        return true;
    }

    bool emit(IGameplayEvents& events, const std::string& effectId,
              std::uint64_t tick, std::string& errorOut) override {
        const auto found = specs_.find(effectId);
        if (found == specs_.end()) {
            errorOut = "ability effects: unknown effect '" + effectId + "'";
            return false;
        }
        const AbilityEffectSpec& spec = found->second;
        events.publish(kind_code(spec.kind), tick, serialize(spec));
        return true;
    }

    const AbilityEffectSpec* spec(const std::string& id) const override {
        const auto found = specs_.find(id);
        return found == specs_.end() ? nullptr : &found->second;
    }

    std::vector<std::string> ids() const override {
        std::vector<std::string> out;
        out.reserve(specs_.size());
        for (const auto& entry : specs_) out.push_back(entry.first);
        return out;
    }

    std::size_t count() const override { return specs_.size(); }
    void clear() override { specs_.clear(); }

private:
    std::map<std::string, AbilityEffectSpec> specs_;
};

}  // namespace

std::unique_ptr<IAbilityEffects> create_ability_effects() {
    return std::make_unique<AbilityEffects>();
}

}  // namespace gameplay
}  // namespace engine

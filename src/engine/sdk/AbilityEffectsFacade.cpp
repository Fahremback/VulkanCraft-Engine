// AbilityEffectsFacade.cpp — adapter do contrato público IAbilityEffects
// (engine::gameplay). O autor define efeitos (kind + parâmetros); configure()
// valida a lista ALL-OR-NOTHING; emit() valida o efeito e PUBLICA um evento
// tipado no IGameplayEvents (payload = spec serializado de forma
// determinística), que o cenário (física/voxel/renderer/áudio) consome.
// Puro, self-contained (std), determinístico.
//
// Histórico: a interface foi declarada e consumida por consumidores externos
// (tools/gameplay-consumer, tools/editor-gameplay-consumer), mas NÃO possuía
// implementação no namespace engine::gameplay — o external-consumer-gate
// falhava no link do SDK instalado (LNK2019 create_ability_effects). Esta
// implementação fecha o contrato.

#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

namespace {

// Codifica o kind do efeito em um id estável 1..7 para o evento público.
std::uint16_t ability_effect_kind_code(AbilityEffectKind kind) {
    switch (kind) {
        case AbilityEffectKind::ForceImpulse: return 1;
        case AbilityEffectKind::Field:        return 2;
        case AbilityEffectKind::Teleport:     return 3;
        case AbilityEffectKind::CreateBlock:  return 4;
        case AbilityEffectKind::DestroyBlock: return 5;
        case AbilityEffectKind::Status:       return 6;
        case AbilityEffectKind::Generic:      return 7;
    }
    return 0;
}

// Verificação de finitude bit-level (a engine compila com /fp:fast — ver
// AbilitySystem.cpp). NaN/Inf são rejeitados como não-finitos.
bool finite_float(float value) {
    return std::abs(value) <= std::numeric_limits<float>::max();
}

// Serialização determinística do spec para os bytes opacos do evento.
// Layout versionado: [u8 ver=1][u8 kind][var id\0][f32 mag][f32 radius]
// [3xf32 target][var blockId\0][var statusId\0][u32 stacks][var eventKind\0].
std::vector<std::uint8_t> serialize_spec(const AbilityEffectSpec& spec) {
    std::vector<std::uint8_t> out;
    auto push_u8 = [&out](std::uint8_t v) { out.push_back(v); };
    auto push_f32 = [&out](float v) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(v), "float is 32-bit");
        std::memcpy(&bits, &v, sizeof(bits));
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
    };
    auto push_u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    };
    auto push_str = [&out](const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    };

    push_u8(1);                                                        // version
    push_u8(static_cast<std::uint8_t>(spec.kind));
    push_str(spec.id);
    push_f32(spec.magnitude);
    push_f32(spec.radius);
    push_f32(spec.target.x);
    push_f32(spec.target.y);
    push_f32(spec.target.z);
    push_str(spec.blockId);
    push_str(spec.statusId);
    push_u32(spec.statusStacks);
    push_str(spec.eventKind);
    return out;
}

}  // namespace

class AbilityEffectsImpl final : public IAbilityEffects {
public:
    bool configure(const std::vector<AbilityEffectSpec>& specs,
                   std::string& errorOut) override {
        std::vector<AbilityEffectSpec> parsed;
        parsed.reserve(specs.size());
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const AbilityEffectSpec& s = specs[i];
            if (s.id.empty()) {
                errorOut = "ability_effects: id vazio no efeito " + std::to_string(i);
                return false;
            }
            const bool duplicate = std::any_of(
                parsed.begin(), parsed.end(),
                [&s](const AbilityEffectSpec& p) { return p.id == s.id; });
            if (duplicate) {
                errorOut = "ability_effects: id duplicado '" + s.id + "'";
                return false;
            }
            if (s.magnitude < 0.0f || !finite_float(s.magnitude)) {
                errorOut = "ability_effects: magnitude inválida no efeito '" + s.id + "'";
                return false;
            }
            if (s.kind == AbilityEffectKind::Field && (s.radius <= 0.0f || !finite_float(s.radius))) {
                errorOut = "ability_effects: radius inválido no Field '" + s.id + "'";
                return false;
            }
            if (s.kind == AbilityEffectKind::Teleport &&
                (!finite_float(s.target.x) || !finite_float(s.target.y) || !finite_float(s.target.z))) {
                errorOut = "ability_effects: target não-finita no Teleport '" + s.id + "'";
                return false;
            }
            if (s.kind == AbilityEffectKind::CreateBlock && s.blockId.empty()) {
                errorOut = "ability_effects: blockId vazio no CreateBlock '" + s.id + "'";
                return false;
            }
            if (s.kind == AbilityEffectKind::Status && s.statusId.empty()) {
                errorOut = "ability_effects: statusId vazio no Status '" + s.id + "'";
                return false;
            }
            if (s.kind == AbilityEffectKind::Status && s.statusStacks < 1) {
                errorOut = "ability_effects: statusStacks >= 1 no Status '" + s.id + "'";
                return false;
            }
            if (s.kind == AbilityEffectKind::Generic && s.eventKind.empty()) {
                errorOut = "ability_effects: eventKind vazio no Generic '" + s.id + "'";
                return false;
            }
            parsed.push_back(s);
        }
        // Stable, deterministic ascending order by id.
        std::sort(parsed.begin(), parsed.end(),
                  [](const AbilityEffectSpec& a, const AbilityEffectSpec& b) {
                      return a.id < b.id;
                  });
        specs_ = std::move(parsed);
        return true;
    }

    bool emit(IGameplayEvents& events, const std::string& effectId,
              std::uint64_t tick, std::string& errorOut) override {
        auto found = std::lower_bound(
            specs_.begin(), specs_.end(), effectId,
            [](const AbilityEffectSpec& a, const std::string& id) { return a.id < id; });
        if (found == specs_.end() || found->id != effectId) {
            errorOut = "ability_effects: efeito desconhecido '" + effectId + "'";
            return false;
        }
        const std::uint16_t code = ability_effect_kind_code(found->kind);
        events.publish(code, tick, serialize_spec(*found));
        return true;
    }

    const AbilityEffectSpec* spec(const std::string& id) const override {
        auto found = std::lower_bound(
            specs_.begin(), specs_.end(), id,
            [](const AbilityEffectSpec& a, const std::string& key) { return a.id < key; });
        if (found == specs_.end() || found->id != id) return nullptr;
        return &(*found);
    }

    std::vector<std::string> ids() const override {
        std::vector<std::string> out;
        out.reserve(specs_.size());
        for (const auto& s : specs_) out.push_back(s.id);
        return out;
    }

    std::size_t count() const override { return specs_.size(); }

    void clear() override { specs_.clear(); }

private:
    std::vector<AbilityEffectSpec> specs_;  // ordenado por id (ascendente)
};

std::unique_ptr<IAbilityEffects> create_ability_effects() {
    return std::make_unique<AbilityEffectsImpl>();
}

}  // namespace gameplay
}  // namespace engine
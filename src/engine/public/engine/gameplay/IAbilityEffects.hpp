// IAbilityEffects — specs de EFEITOS de abilities validadas, emitidas como
// EVENTOS públicos. Componente CORE do §5 item 59 ("integrar poderes/
// abilities ao cenário: força, campo, teleporte, criação/destruição, status
// e eventos"): o autor define efeitos (kind + parâmetros); `emit` valida e
// PUBLICA no IGameplayEvents (#253) — o cenário (física/voxel) consome o
// evento e aplica (aplicação real = integração; a EMISSÃO é o contrato).
// O payload do evento serializa o spec (id + parâmetros, bytes opacos).
// Puro e determinístico; sem depender do cenário.

#pragma once

#include "engine/gameplay/IGameplayEvents.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

enum class AbilityEffectKind : std::uint8_t {
    ForceImpulse,   // impulso de força (magnitude + direção target)
    Field,          // campo de área (magnitude + radius)
    Teleport,       // teleporte (target)
    CreateBlock,    // criação de bloco (blockId + target)
    DestroyBlock,   // destruição de bloco (target)
    Status,         // status (statusId + stacks — IEffectStacks)
    Generic         // evento genérico (eventKind)
};

const char* ability_effect_kind_name(AbilityEffectKind kind);

struct AbilityEffectSpec {
    std::string id;                    // único
    AbilityEffectKind kind{ AbilityEffectKind::Generic };
    float magnitude{ 1.0f };           // força/campo (>= 0)
    float radius{ 1.0f };              // campo (> 0 quando Field)
    glm::vec3 target{ 0.0f };          // teleporte/posição (finita)
    std::string blockId;               // CreateBlock (não-vazio)
    std::string statusId;              // Status (não-vazio)
    std::uint32_t statusStacks{ 1 };   // Status (>= 1)
    std::string eventKind;             // Generic (não-vazio)
};

class IAbilityEffects {
public:
    virtual ~IAbilityEffects() = default;

    // All-or-nothing: id vazio/duplicado ou parâmetros inválidos por kind
    // (magnitude < 0, radius <= 0 no Field, blockId vazio no CreateBlock,
    // statusId vazio no Status, eventKind vazio no Generic, target não-finita)
    // → rejeita a lista inteira.
    virtual bool configure(const std::vector<AbilityEffectSpec>& specs,
                           std::string& errorOut) = 0;

    // Emite o efeito: publica no IGameplayEvents um evento com kind =
    // ability_effect_kind_code(effect.kind) (1..7) e payload = spec
    // serializado. Efeito desconhecido → false (nada publicado).
    virtual bool emit(IGameplayEvents& events, const std::string& effectId,
                      std::uint64_t tick, std::string& errorOut) = 0;

    virtual const AbilityEffectSpec* spec(const std::string& id) const = 0;
    virtual std::vector<std::string> ids() const = 0;  // ordem crescente
    virtual std::size_t count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IAbilityEffects> create_ability_effects();

}  // namespace gameplay
}  // namespace engine

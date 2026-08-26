// IInteraction — componente genérico de interação data-driven (Agente 4 §1
// item 26 "interação"): cada entidade expõe pontos de interação nomeados com
// raio/prompt/configuração, e o sistema valida disponibilidade por
// distância/estado sem acoplar o jogo ao núcleo. Puro, sem RNG, sem estado
// global; round-trip JSON bit-exact. O chamador (gameplay/input) executa o
// efeito — este contrato decide SE e QUANDO a interação está disponível.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

// Uma interação disponível em uma entidade.
struct InteractionDef {
    std::string name;        // identificador único (ex.: "open", "pickup", "talk")
    std::string prompt;      // texto exibível ao jogador (ex.: "[E] Abrir")
    float radius = 1.0f;     // distância máxima do interator
    bool requiresFacing = false;  // exige que o interator esteja de frente
    float cooldownSeconds = 0.0f; // mínimo entre ativações
};

struct InteractionState {
    std::string name;
    bool available = false;   // dentro do raio (+ facing) e fora do cooldown
    float distance = 0.0f;
    float remainingCooldown = 0.0f;
};

// Registro de interações de uma entidade.
class IInteraction {
public:
    virtual ~IInteraction() = default;

    // Configura a lista de interações (all-or-nothing): nome vazio/duplicado,
    // raio <= 0, cooldown < 0, prompt vazio → rejeita a lista inteira.
    virtual bool configure(const std::vector<InteractionDef>& defs,
                           std::string& errorOut) = 0;

    // Avalia cada interação contra interator (posição) e facing.
    // Interator em (0,0,0); a entidade em entityX/entityZ com orientação
    // entityFacing (radianos); available = distance <= radius (e facing
    // quando requiresFacing). Cooldown conta no relógio próprio (advance).
    virtual std::vector<InteractionState> evaluate(
        float entityX, float entityZ, float entityFacing,
        float interatorX, float interatorZ, float interatorFacing) = 0;

    // Marca uma interação como ativada (aplica o cooldown). Retorna false se
    // o nome não existe ou a interação não está disponível.
    virtual bool activate(const std::string& name) = 0;

    // Avança o relógio de cooldowns (dt >= 0; negativo recusa e retorna false).
    virtual bool advance(float dt) = 0;

    // Serialização JSON versionada all-or-nothing bit-exact.
    virtual bool to_json(std::string& outJson) const = 0;
    virtual bool from_json(const std::string& json, std::string& errorOut) = 0;

    virtual std::size_t count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IInteraction> create_interaction();

}  // namespace gameplay
}  // namespace engine

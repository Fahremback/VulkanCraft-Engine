// IAgentCapabilities — perfil de agente com capacidades de movimento e
// verificação determinística de viabilidade. Unidade CORE dos §4 itens 25
// (off-mesh links e capacidades andar/correr/pular/escalar/nadar/voar) e 28
// (agentes com diferentes tamanhos, alturas, raios, inclinações, degraus).
//
#pragma once
// O contrato NÃO conhece o navmesh: recebe GEOMETRIA LOCAL do trecho (gap,
// degrau, inclinação, profundidade de água, teto) e responde se o agente
// consegue atravessar com a capacidade adequada. O chamador (navmesh,
// controller, IA) alimenta a geometria. Determinístico, self-contained
// (std), sem RNG.
//
// Escada de capacidades (a MENOR que vence é a resposta):
//   Walk  — sem gap, degrau <= maxClimb, inclinação <= maxSlope, seco;
//   Jump  — gap <= jumpDistance E degrau <= jumpHeight E inclinação <=
//           maxSlope (não pousa em rampa íngreme);
//   Climb — parede <= climbHeight (inclinação ignorada);
//   Swim  — água em [0, swimDepth];
//   Fly   — ignora geometria.

#include <cstdint>
#include <memory>
#include <string>

namespace engine::navigation {

enum class MoveCapability {
    Walk,    // superfície plana/leve inclinação
    Run,     // mesma superfície, velocidade maior (não muda viabilidade)
    Jump,    // salto: vence gap horizontal e degrau alto (dentro do limite)
    Climb,   // escalada: superfícies íngremes/acima do slope máximo
    Swim,    // atravessa água (dentro da profundidade máxima)
    Fly,     // ignora terreno (teto arbitrário)
};

struct AgentProfile {
    float radius{ 0.4f };           // raio horizontal (corredores, portas)
    float height{ 1.8f };           // altura (teto, túneis)
    float maxClimb{ 1.0f };         // degrau máximo (walk/jump)
    float maxSlopeDegrees{ 45.0f }; // inclinação máxima andável
    float jumpDistance{ 2.0f };     // gap horizontal máximo de salto
    float jumpHeight{ 1.2f };       // degrau máximo de salto
    float climbHeight{ 3.0f };      // parede máxima escalável (>= jumpHeight)
    float swimDepth{ 2.0f };        // profundidade de água máxima nadável
    bool canJump{ true };
    bool canClimb{ false };
    bool canSwim{ false };
    bool canFly{ false };
};

// Geometria local do trecho a atravessar (entre ponto A e ponto B).
struct TraversalGeometry {
    float horizontalGap{ 0.0f };   // distância horizontal sem chão (gap)
    float stepUp{ 0.0f };          // degrau a subir (positivo = sobe)
    float slopeDegrees{ 0.0f };    // inclinação da superfície
    float waterDepth{ 0.0f };      // profundidade de água no trecho (0 = seco)
    float ceilingClearance{ 0.0f }; // espaço livre acima do chão (altura)
};

struct TraversalResult {
    bool possible{ false };
    MoveCapability capability{ MoveCapability::Walk };  // a melhor capacidade que vence
    const char* reason{ "" };  // diagnóstico curto quando impossível
};

class IAgentCapabilities {
public:
    virtual ~IAgentCapabilities() = default;

    virtual void set_profile(const AgentProfile& profile) = 0;
    virtual AgentProfile profile() const = 0;

    // Verifica se o agente atravessa o trecho descrito. Retorna a melhor
    // capacidade (Walk < Run < Jump < Climb < Swim < Fly) que torna o
    // trecho possível, ou possible=false com reason.
    virtual TraversalResult can_traverse(const TraversalGeometry& geometry) const = 0;
};

std::unique_ptr<IAgentCapabilities> create_agent_capabilities();

}  // namespace engine::navigation

#pragma once

// IHitReaction — modelo determinístico de reação a impacto (knockback,
// stagger, recuperação). Primeiro contrato do §4 item 48 (physical
// animation / hit reactions — unidade CORE; ragdoll e active physics são
// unidades separadas que dependem do motor físico).
//
// O contrato NÃO conhece física: modela o ESTADO da reação — impulso de
// knockback com decaimento exponencial, stagger (janela de vulnerabilidade
// pós-impacto), e recuperação do chão (tempo mínimo deitado + transição
// determinística para de pé). O chamador aplica o impulso resultante onde
// quiser (Jolt, kinematic, etc.).
//
// Self-contained (std only), headless, determinístico. Sem RNG, sem estado
// global.

#include <cstdint>
#include <memory>

namespace engine::gameplay {

enum class HitState {
    Normal,     // de pé, sem reação ativa
    Stagger,    // recuando do impacto (knockback em decaimento)
    Down,       // no chão após knockdown (aguardando recuperação)
    Recovering, // levantando (transição determinística para Normal)
};

struct HitReactionConfig {
    float knockbackInitial{ 0.0f };   // velocidade de knockback no impacto (>= 0)
    float knockbackDecay{ 2.0f };     // decaimento exponencial por segundo (>= 0)
    float staggerDuration{ 0.5f };    // duração do stagger (>= 0)
    float downDuration{ 2.0f };       // tempo mínimo deitado antes de recuperar (>= 0)
    float recoverDuration{ 0.4f };    // duração do levantamento (>= 0)
    float knockdownStrength{ 0.75f }; // strength >= este valor derruba (0..1)
};

// Estado observável da reação num instante.
struct HitReactionState {
    HitState state{ HitState::Normal };
    float knockbackVelocity{ 0.0f };  // velocidade de knockback atual (decaindo)
    float timeInState{ 0.0f };        // tempo acumulado no estado atual
    bool grounded{ true };            // chamador informa se está no chão
};

class IHitReaction {
public:
    virtual ~IHitReaction() = default;

    virtual void set_config(const HitReactionConfig& config) = 0;
    virtual HitReactionConfig config() const = 0;

    // Aplica um impacto: entra em Stagger com knockback = base * strength
    // (0..1, 1 = força total). Se strength >= knockdownStrength, o impacto
    // derruba (vira Down no próximo update, com o limiar de força). Se o
    // personagem já está Down, o impacto reinicia o timer do chão (mas
    // mantém Down). Determinístico.
    virtual void apply_impact(float strength) = 0;

    // Avança o modelo por dt (segundos). `grounded` = personagem apoiado no
    // chão (física real decide; o modelo só transiciona Down→Recovering
    // quando grounded e timeInState >= downDuration). Retorna o estado após
    // o tick.
    virtual HitReactionState update(float dt, bool grounded) = 0;

    virtual HitReactionState state() const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IHitReaction> create_hit_reaction();

}  // namespace engine::gameplay

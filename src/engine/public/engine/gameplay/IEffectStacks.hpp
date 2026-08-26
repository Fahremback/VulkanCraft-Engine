// IEffectStacks — modelo determinístico de efeitos de status ACUMULÁVEIS
// (stacking). Componente do §5 item 64 (Ability System data-driven — o
// IAbilitySystem cobre custo/cooldown/tags/targeting/prediction/effects;
// este contrato adiciona o stacking de efeitos, sem depender do runtime).
//
// Semântica: cada efeito (id) tem uma contagem de stacks. `apply` adiciona
// 1 stack (até maxStacks) e RENOVA a duração total; cada `tick(dt)` decreta
// a duração; quando a duração zera, TODOS os stacks do efeito somem
// (determinístico). `refresh` renova a duração sem adicionar stack.
// Sem RNG, sem estado global; o estado é carregado no próprio adapter.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::gameplay {

// Configuração de um efeito acumulável.
struct EffectStackSpec {
    std::uint16_t effectId{ 0 };
    std::uint32_t maxStacks{ 5 };        // teto de stacks (>= 1)
    float durationSeconds{ 5.0f };       // duração de cada aplicação (>= 0)
    bool refreshOnApply{ true };         // apply renova a duração
};

// Estado observável de um efeito num instante.
struct EffectStackState {
    std::uint16_t effectId{ 0 };
    std::uint32_t stacks{ 0 };
    float remainingSeconds{ 0.0f };  // duração restante (com refresh, a maior)
};

class IEffectStacks {
public:
    virtual ~IEffectStacks() = default;

    // Configura os efeitos conhecidos (substitui a lista). All-or-nothing:
    // id duplicado ou maxStacks == 0 rejeita a lista inteira.
    virtual bool configure(const std::vector<EffectStackSpec>& specs,
                           std::string& errorOut) = 0;

    // Aplica 1 stack do efeito (até maxStacks). `refreshOnApply` renova a
    // duração; sem refresh, aplicações novas NÃO estendem o tempo (o que
    // zera primeiro, sai). Retorna a nova contagem (0 se efeito desconhecido
    // — nunca adiciona estado).
    virtual std::uint32_t apply(std::uint16_t effectId) = 0;

    // Renova a duração do efeito sem adicionar stack (0 = efeito inexistente).
    virtual bool refresh(std::uint16_t effectId) = 0;

    // Remove TODOS os stacks do efeito (0 = efeito inexistente).
    virtual bool clear(std::uint16_t effectId) = 0;

    // Avança o tempo: decreta a duração de cada efeito ativo; duração <= 0
    // remove o efeito INTEIRO. Retorna os ids removidos neste tick (ordem
    // crescente).
    virtual std::vector<std::uint16_t> tick(float dt) = 0;

    // Estado de todos os efeitos ativos (ordem crescente de id).
    virtual std::vector<EffectStackState> states() const = 0;

    virtual std::uint32_t stack_count(std::uint16_t effectId) const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IEffectStacks> create_effect_stacks();

}  // namespace engine::gameplay

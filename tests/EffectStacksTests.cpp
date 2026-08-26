// EffectStacksTests — gate do contrato IEffectStacks (§5 item 64, stacking).
// Prova: apply até maxStacks, refreshOnApply renova, sem refresh o tempo não
// estica, tick decreta e remove o efeito inteiro ao zerar, clear, efeito
// desconhecido não adiciona estado, configure all-or-nothing, ordem dos
// estados, reset.

#include "engine/gameplay/IEffectStacks.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float eps = 1.0e-4f) { return (a > b - eps) && (a < b + eps); }

void test_apply_and_cap() {
    auto stacks = engine::gameplay::create_effect_stacks();
    std::string error;
    engine::gameplay::EffectStackSpec poison;
    poison.effectId = 1;
    poison.maxStacks = 3;
    poison.durationSeconds = 5.0f;
    poison.refreshOnApply = true;
    check(stacks->configure({ poison }, error), "configure 1 efeito");

    check(stacks->apply(1) == 1, "1ª aplicação → 1 stack");
    check(stacks->apply(1) == 2, "2ª aplicação → 2 stacks");
    check(stacks->apply(1) == 3, "3ª aplicação → 3 stacks (cap)");
    check(stacks->apply(1) == 3, "4ª aplicação fica no cap");
    check(stacks->stack_count(1) == 3, "stack_count = 3");

    // Refresh renova a duração (qualquer tick antes de 5s volta para 5s).
    stacks->tick(3.0f);
    check(near(stacks->states()[0].remainingSeconds, 2.0f), "3s de 5s consumidos");
    stacks->refresh(1);
    check(near(stacks->states()[0].remainingSeconds, 5.0f), "refresh renova para 5s");

    // Efeito desconhecido: no-op.
    check(stacks->apply(99) == 0, "efeito desconhecido: 0 stacks");
    check(stacks->stack_count(99) == 0, "desconhecido não adiciona estado");
}

void test_tick_and_removal() {
    auto stacks = engine::gameplay::create_effect_stacks();
    std::string error;
    engine::gameplay::EffectStackSpec burn;
    burn.effectId = 2;
    burn.maxStacks = 2;
    burn.durationSeconds = 4.0f;
    burn.refreshOnApply = false;  // sem refresh: aplicações não esticam
    check(stacks->configure({ burn }, error), "configure burn");

    stacks->apply(2);   // 1 stack, remaining 4
    stacks->apply(2);   // 2 stacks, remaining continua 4 (sem refresh)
    check(stacks->stack_count(2) == 2, "2 stacks sem refresh");
    check(near(stacks->states()[0].remainingSeconds, 4.0f),
          "sem refresh: tempo não estica");

    // tick 3.9: ainda ativo; +0.2: zera e remove o efeito INTEIRO.
    stacks->tick(3.9f);
    check(stacks->stack_count(2) == 2, "ativo antes de zerar");
    const std::vector<std::uint16_t> removed = stacks->tick(0.2f);
    check(removed.size() == 1 && removed[0] == 2, "tick remove o efeito ao zerar");
    check(stacks->stack_count(2) == 0, "todos os stacks somem juntos");

    // clear.
    stacks->apply(2);
    check(stacks->stack_count(2) == 1, "re-aplicado após zerar");
    check(stacks->clear(2), "clear remove");
    check(stacks->stack_count(2) == 0, "0 após clear");
}

void test_configure_and_states() {
    auto stacks = engine::gameplay::create_effect_stacks();
    std::string error;

    // maxStacks == 0 rejeita.
    engine::gameplay::EffectStackSpec bad;
    bad.effectId = 5;
    bad.maxStacks = 0;
    check(!stacks->configure({ bad }, error), "maxStacks 0 rejeitado");

    // Id duplicado rejeita.
    engine::gameplay::EffectStackSpec a;
    a.effectId = 7;
    a.maxStacks = 2;
    engine::gameplay::EffectStackSpec b;
    b.effectId = 7;
    b.maxStacks = 3;
    check(!stacks->configure({ a, b }, error), "id duplicado rejeitado");
    check(stacks->apply(7) == 0, "config inválida: sem efeitos ativos");

    // Ordem dos estados por id crescente.
    engine::gameplay::EffectStackSpec one;
    one.effectId = 1;
    one.maxStacks = 5;
    engine::gameplay::EffectStackSpec three;
    three.effectId = 3;
    three.maxStacks = 5;
    engine::gameplay::EffectStackSpec two;
    two.effectId = 2;
    two.maxStacks = 5;
    check(stacks->configure({ one, three, two }, error), "configure 3 efeitos");
    stacks->apply(3);
    stacks->apply(1);
    stacks->apply(2);
    const std::vector<engine::gameplay::EffectStackState> states = stacks->states();
    check(states.size() == 3 && states[0].effectId == 1 && states[1].effectId == 2 &&
              states[2].effectId == 3,
          "estados ordenados por id crescente");

    // Reset limpa tudo.
    stacks->reset();
    check(stacks->states().empty(), "reset limpa os efeitos");
}

}  // namespace

int main() {
    test_apply_and_cap();
    test_tick_and_removal();
    test_configure_and_states();

    if (failures == 0) {
        std::printf("effect_stacks_tests: all checks passed\n");
        return 0;
    }
    std::printf("effect_stacks_tests: %d failure(s)\n", failures);
    return 1;
}

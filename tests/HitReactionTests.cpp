// HitReactionTests — gate do contrato IHitReaction (§4 item 48, CORE).
// Prova: impacto escala o knockback, stagger decai exponencialmente,
// knockdown quando o impulso vence o limiar, recuperação do chão exige
// grounded + downDuration, impacto prolonga o chão, round-trip de config
// e reset.

#include "engine/gameplay/IHitReaction.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

void test_impact_and_stagger() {
    auto hr = engine::gameplay::create_hit_reaction();
    engine::gameplay::HitReactionConfig config;
    config.knockbackInitial = 10.0f;
    config.knockbackDecay = 2.0f;
    config.staggerDuration = 0.5f;
    hr->set_config(config);

    // Impacto fraco (0.2): knockback = 2, fica em Stagger.
    hr->apply_impact(0.2f);
    engine::gameplay::HitReactionState s = hr->state();
    check(s.state == engine::gameplay::HitState::Stagger, "impacto entra em Stagger");
    check(near(s.knockbackVelocity, 2.0f), "knockback escala com strength");

    // Tick pequeno: decaimento exponencial v = 2 * e^(-2*0.1).
    s = hr->update(0.1f, true);
    check(near(s.knockbackVelocity, 2.0f * std::exp(-0.2f)), "knockback decai exponencial");

    // Após staggerDuration (0.5s no total), volta a Normal.
    hr->update(0.4f, true);
    s = hr->state();
    check(s.state == engine::gameplay::HitState::Normal, "stagger expira → Normal");
    check(near(s.knockbackVelocity, 0.0f), "knockback zera ao sair do stagger");

    // Impacto acima do limiar: strength 1.0 → knockback 10 > 1.5*10? não;
    // usar knockbackInitial alto com threshold relativo... testar knockdown
    // com configuração dedicada no bloco abaixo.
}

void test_knockdown_and_recovery() {
    auto hr = engine::gameplay::create_hit_reaction();
    engine::gameplay::HitReactionConfig config;
    config.knockbackInitial = 10.0f;
    config.knockbackDecay = 0.1f;    // decaimento lento: impulso persiste
    config.staggerDuration = 10.0f;  // não sai do stagger por tempo
    config.downDuration = 2.0f;
    config.recoverDuration = 0.4f;
    config.knockdownStrength = 0.75f; // strength >= 0.75 derruba
    hr->set_config(config);

    // Impacto forte: knockback 10 > limiar (1.5) → knockdown no 1º tick.
    hr->apply_impact(1.0f);
    engine::gameplay::HitReactionState s = hr->update(0.05f, true);
    check(s.state == engine::gameplay::HitState::Down, "impacto forte derruba (Down)");

    // No chão, mas sem tempo suficiente: continua Down.
    s = hr->update(1.0f, true);
    check(s.state == engine::gameplay::HitState::Down, "Down antes do downDuration");

    // No ar (grounded=false): NUNCA recupera.
    s = hr->update(5.0f, false);
    check(s.state == engine::gameplay::HitState::Down, "no ar: não recupera");

    // No chão após downDuration: Recovering.
    s = hr->update(3.0f, true);
    check(s.state == engine::gameplay::HitState::Recovering, "grounded + tempo → Recovering");

    // Após recoverDuration: Normal.
    s = hr->update(0.5f, true);
    check(s.state == engine::gameplay::HitState::Normal, "recuperação completa → Normal");

    // Impacto durante Down prolonga o chão (reinicia o timer).
    hr->apply_impact(1.0f);
    hr->update(0.05f, true);  // vira Down de novo
    hr->update(1.9f, true);   // quase no downDuration
    hr->apply_impact(0.5f);   // impacto prolonga
    s = hr->update(0.2f, true);
    check(s.state == engine::gameplay::HitState::Down, "impacto durante Down prolonga o chão");
}

void test_config_and_reset() {
    auto hr = engine::gameplay::create_hit_reaction();
    engine::gameplay::HitReactionConfig config;
    config.knockbackInitial = 7.0f;
    config.knockbackDecay = 3.0f;
    config.staggerDuration = 0.8f;
    config.downDuration = 3.0f;
    config.recoverDuration = 0.6f;
    hr->set_config(config);

    const engine::gameplay::HitReactionConfig got = hr->config();
    check(near(got.knockbackInitial, 7.0f) && near(got.knockbackDecay, 3.0f) &&
              near(got.staggerDuration, 0.8f) && near(got.downDuration, 3.0f) &&
              near(got.recoverDuration, 0.6f),
          "config round-trip exata");

    hr->apply_impact(1.0f);
    hr->update(0.1f, true);
    check(hr->state().state != engine::gameplay::HitState::Normal, "estado ativo antes do reset");
    hr->reset();
    const engine::gameplay::HitReactionState s = hr->state();
    check(s.state == engine::gameplay::HitState::Normal &&
              near(s.knockbackVelocity, 0.0f) && near(s.timeInState, 0.0f),
          "reset volta a Normal limpo");
}

}  // namespace

int main() {
    test_impact_and_stagger();
    test_knockdown_and_recovery();
    test_config_and_reset();

    if (failures == 0) {
        std::printf("hit_reaction_tests: all checks passed\n");
        return 0;
    }
    std::printf("hit_reaction_tests: %d failure(s)\n", failures);
    return 1;
}

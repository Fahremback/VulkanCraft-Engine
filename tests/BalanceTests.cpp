// BalanceTests — gate do contrato IBalance (§4 item 47, equilíbrio).
// Prova: CoM no centro → Stable; na borda → Edge com correção de tornozelo;
// fora → Unstable com correção de quadril na direção do centróide; suporte
// degenerado (< 3 pontos) → Unstable; round-trip de config.

#include "engine/gameplay/IBalance.hpp"

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

// Quadrado de suporte: (-1,-1) a (1,1).
engine::gameplay::SupportPoint square[4] = {
    { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
};

void test_stable_and_edge() {
    auto bal = engine::gameplay::create_balance();
    engine::gameplay::BalanceConfig config;
    config.edgeMargin = 0.05f;
    config.ankleGain = 8.0f;
    config.hipGain = 3.0f;
    config.maxCorrection = 2.0f;
    bal->set_config(config);

    // CoM no centro: Stable, sem correção.
    engine::gameplay::BalanceResult r = bal->evaluate(0.0f, 0.0f, square, 4);
    check(r.state == engine::gameplay::BalanceState::Stable, "centro → Stable");
    check(near(r.correctionX, 0.0f) && near(r.correctionZ, 0.0f), "centro: sem correção");
    check(r.margin > config.edgeMargin, "centro: margem folgada");

    // CoM perto da borda (0.98, 0): dentro mas margin 0.02 <= 0.05 → Edge.
    // Correção aponta para o centróide (0,0) → negativa em X.
    r = bal->evaluate(0.98f, 0.0f, square, 4);
    check(r.state == engine::gameplay::BalanceState::Edge, "borda → Edge");
    check(r.correctionX < 0.0f && near(r.correctionZ, 0.0f),
          "Edge: correção aponta p/ centróide (negativa em X)");
    check(near(r.correctionX, -config.ankleGain * (config.edgeMargin - 0.02f)),
          "Edge: correção = -ankleGain * erro");
}

void test_unstable() {
    auto bal = engine::gameplay::create_balance();
    engine::gameplay::BalanceConfig config;
    config.edgeMargin = 0.05f;
    config.ankleGain = 8.0f;
    config.hipGain = 3.0f;
    config.maxCorrection = 2.0f;
    bal->set_config(config);

    // CoM fora (2, 0): Unstable; correção aponta para o centróide (0,0).
    // hipGain * 1.0 = 3.0, mas maxCorrection=2 clampa a magnitude a 2.
    engine::gameplay::BalanceResult r = bal->evaluate(2.0f, 0.0f, square, 4);
    check(r.state == engine::gameplay::BalanceState::Unstable, "fora → Unstable");
    check(r.margin < 0.0f, "fora: margem negativa");
    check(r.correctionX < 0.0f && near(r.correctionZ, 0.0f), "correção aponta p/ centróide");
    check(near(r.correctionX, -config.maxCorrection),
          "Unstable: correção limitada por maxCorrection");

    // Fora na diagonal (2, 2): direção = centróide normalizada, magnitude
    // limitada por maxCorrection (2).
    r = bal->evaluate(2.0f, 2.0f, square, 4);
    check(r.correctionX < 0.0f && r.correctionZ < 0.0f, "diagonal: correção (-,-)");
    const float len = std::sqrt(r.correctionX * r.correctionX +
                                r.correctionZ * r.correctionZ);
    check(near(len, config.maxCorrection), "magnitude diagonal limitada por maxCorrection");

    // Muito longe: teto de maxCorrection.
    r = bal->evaluate(10.0f, 0.0f, square, 4);
    check(near(std::fabs(r.correctionX), config.maxCorrection),
          "longe: correção limitada por maxCorrection");

    // Hip gain sem clamp: maxCorrection alto → magnitude = hipGain * |margin|.
    config.maxCorrection = 10.0f;
    bal->set_config(config);
    r = bal->evaluate(2.0f, 0.0f, square, 4);
    check(near(std::fabs(r.correctionX), config.hipGain * 1.0f),
          "Unstable: magnitude = hipGain * distância à borda (sem clamp)");
}

void test_degenerate_and_config() {
    auto bal = engine::gameplay::create_balance();
    engine::gameplay::BalanceConfig config;
    config.edgeMargin = 0.05f;
    config.ankleGain = 8.0f;
    config.hipGain = 3.0f;
    config.maxCorrection = 2.0f;
    bal->set_config(config);

    // Suporte insuficiente: Unstable.
    engine::gameplay::SupportPoint two[2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f } };
    engine::gameplay::BalanceResult r = bal->evaluate(0.5f, 0.0f, two, 2);
    check(r.state == engine::gameplay::BalanceState::Unstable, "< 3 pontos → Unstable");
    r = bal->evaluate(0.5f, 0.0f, nullptr, 0);
    check(r.state == engine::gameplay::BalanceState::Unstable, "null → Unstable");

    // Triângulo: CoM no centro do triângulo é Stable.
    engine::gameplay::SupportPoint tri[3] = {
        { 0.0f, 0.0f }, { 2.0f, 0.0f }, { 0.0f, 2.0f },
    };
    r = bal->evaluate(0.6f, 0.6f, tri, 3);
    check(r.state == engine::gameplay::BalanceState::Stable, "triângulo: centro → Stable");

    // Round-trip de config.
    config.ankleGain = 12.0f;
    config.hipGain = 5.0f;
    config.maxCorrection = 4.0f;
    config.edgeMargin = 0.1f;
    bal->set_config(config);
    const engine::gameplay::BalanceConfig got = bal->config();
    check(near(got.edgeMargin, 0.1f) && near(got.ankleGain, 12.0f) &&
              near(got.hipGain, 5.0f) && near(got.maxCorrection, 4.0f),
          "config round-trip exata");
}

}  // namespace

int main() {
    test_stable_and_edge();
    test_unstable();
    test_degenerate_and_config();

    if (failures == 0) {
        std::printf("balance_tests: all checks passed\n");
        return 0;
    }
    std::printf("balance_tests: %d failure(s)\n", failures);
    return 1;
}

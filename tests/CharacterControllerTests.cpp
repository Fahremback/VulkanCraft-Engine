// CharacterControllerTests — gate do contrato ICharacterController (§4
// item 54, CORE). Prova: step-up até maxStepHeight, rejeição de degrau
// acima do limite, step-down/snap ao chão, queda livre sem amostras,
// projeção de inclinação acima do limite, água (arrasto + flutuação) e
// round-trip de config.

#include "engine/gameplay/ICharacterController.hpp"

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

void test_step_up_and_down() {
    auto cc = engine::gameplay::create_character_controller();
    engine::gameplay::CharacterConfig config;
    config.maxStepHeight = 0.5f;
    config.stepDownDistance = 0.6f;
    config.maxSlopeDegrees = 50.0f;
    cc->set_config(config);

    // Chão plano no mesmo nível: sem step, sem snap.
    engine::gameplay::TerrainSample flat{ 1.0f, 1.0f, 5.0f };
    engine::gameplay::MoveResult r =
        cc->move(0.0f, 5.0f, 0.0f, 1.0f, 1.0f, &flat, 1);
    check(near(r.newY, 5.0f), "chão plano mantém altura");
    check(!r.steppedUp && !r.snappedDown && !r.slopeBlocked, "plano: sem flags");

    // Degrau de 0.3 (<= 0.5): sobe.
    engine::gameplay::TerrainSample step{ 2.0f, 2.0f, 5.3f };
    r = cc->move(0.0f, 5.0f, 0.0f, 2.0f, 2.0f, &step, 1);
    check(r.steppedUp && near(r.newY, 5.3f), "degrau 0.3 sobe (step-up)");

    // Degrau de 1.0 (> 0.5): bloqueado, mantém altura.
    engine::gameplay::TerrainSample wall{ 3.0f, 3.0f, 6.0f };
    r = cc->move(0.0f, 5.0f, 0.0f, 3.0f, 3.0f, &wall, 1);
    check(r.slopeBlocked && near(r.newY, 5.0f), "degrau 1.0 bloqueado");

    // Descida de 0.4 (<= 0.6): snap ao chão.
    engine::gameplay::TerrainSample low{ 4.0f, 4.0f, 4.6f };
    r = cc->move(0.0f, 5.0f, 0.0f, 4.0f, 4.0f, &low, 1);
    check(r.snappedDown && near(r.newY, 4.6f), "descida 0.4 snap ao chão");

    // Descida de 2.0 (> 0.6): desce só o snap máximo (não atravessa vão).
    engine::gameplay::TerrainSample pit{ 5.0f, 5.0f, 3.0f };
    r = cc->move(0.0f, 5.0f, 0.0f, 5.0f, 5.0f, &pit, 1);
    check(r.snappedDown && near(r.newY, 5.0f - 0.6f), "vão fundo: desce só o snap");

    // Sem amostras: queda livre limitada pelo snap.
    r = cc->move(0.0f, 5.0f, 0.0f, 9.0f, 9.0f, nullptr, 0);
    check(r.snappedDown && near(r.newY, 5.0f - 0.6f), "sem chão: cai o snap máximo");
}

void test_slope_and_nearest() {
    auto cc = engine::gameplay::create_character_controller();
    engine::gameplay::CharacterConfig config;
    config.maxStepHeight = 5.0f;   // não bloqueia por degrau
    config.maxSlopeDegrees = 30.0f;
    cc->set_config(config);

    // Rampa 45° (rise=1, dx=1) acima do limite de 30°: projeta em tan(30°).
    engine::gameplay::TerrainSample ramp{ 1.0f, 0.0f, 6.0f };
    engine::gameplay::MoveResult r =
        cc->move(0.0f, 5.0f, 0.0f, 1.0f, 0.0f, &ramp, 1);
    check(r.slopeBlocked, "rampa 45° marcada como bloqueada por inclinação");
    check(near(r.newY, 5.0f + std::tan(30.0f * 3.14159265f / 180.0f)),
          "rampa 45° projetada em tan(30°)");

    // Rampa ~20° (rise=0.36, dx=1) dentro do limite: sobe o terreno real.
    engine::gameplay::TerrainSample gentle{ 1.0f, 0.0f, 5.36f };
    r = cc->move(0.0f, 5.0f, 0.0f, 1.0f, 0.0f, &gentle, 1);
    check(!r.slopeBlocked && near(r.newY, 5.36f), "rampa 20° sobe o terreno real");

    // Amostra mais próxima vence (2 amostras, alvo perto da segunda).
    engine::gameplay::TerrainSample a{ 0.0f, 0.0f, 4.0f };
    engine::gameplay::TerrainSample b{ 8.0f, 8.0f, 4.7f };
    engine::gameplay::TerrainSample samples[2] = { a, b };
    r = cc->move(0.0f, 4.0f, 0.0f, 7.9f, 7.9f, samples, 2);
    check(r.steppedUp && near(r.newY, 4.7f), "amostra mais próxima do alvo vence");
}

void test_water() {
    auto cc = engine::gameplay::create_character_controller();
    engine::gameplay::CharacterConfig config;
    config.waterSurfaceY = 0.0f;
    config.waterDrag = 2.0f;
    config.waterBuoyancy = 3.0f;
    cc->set_config(config);

    // Fora d'água (depth <= 0): velocidade intacta.
    check(near(cc->water_velocity(-5.0f, 0.0f, 0.1f), -5.0f), "fora d'água: intacta");

    // Dentro d'água: arrasto exponencial + flutuação por profundidade.
    // depth=2, dt=0.5: v = -5*e^(-1) + 3*2*0.5 = -1.839 + 3 = 1.161.
    const float v = cc->water_velocity(-5.0f, 2.0f, 0.5f);
    check(near(v, -5.0f * std::exp(-1.0f) + 3.0f * 2.0f * 0.5f),
          "água: arrasto exponencial + flutuação linear");

    // Profundidade maior → flutuação maior (monotônica).
    const float deeper = cc->water_velocity(-5.0f, 4.0f, 0.5f);
    check(deeper > v, "profundidade maior flutua mais");
}

void test_config_round_trip() {
    auto cc = engine::gameplay::create_character_controller();
    engine::gameplay::CharacterConfig config;
    config.maxStepHeight = 0.9f;
    config.stepDownDistance = 1.2f;
    config.maxSlopeDegrees = 45.0f;
    config.waterSurfaceY = -3.0f;
    config.waterDrag = 5.0f;
    config.waterBuoyancy = 7.0f;
    cc->set_config(config);

    const engine::gameplay::CharacterConfig got = cc->config();
    check(near(got.maxStepHeight, 0.9f) && near(got.stepDownDistance, 1.2f) &&
              near(got.maxSlopeDegrees, 45.0f) && near(got.waterSurfaceY, -3.0f) &&
              near(got.waterDrag, 5.0f) && near(got.waterBuoyancy, 7.0f),
          "config round-trip exata");
}

}  // namespace

int main() {
    test_step_up_and_down();
    test_slope_and_nearest();
    test_water();
    test_config_round_trip();

    if (failures == 0) {
        std::printf("character_controller_tests: all checks passed\n");
        return 0;
    }
    std::printf("character_controller_tests: %d failure(s)\n", failures);
    return 1;
}

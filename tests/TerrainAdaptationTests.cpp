// TerrainAdaptationTests — gate do contrato público de adaptação procedural
// ao terreno (agente 4 §4 item 2, unidade final). Prova a amostragem
// bilinear da grade, o alinhamento pés/root pela regra do menor ajuste
// (d = min_f(H_f + off_f − (R + oy_f))) e o round-trip JSON bit-exact.

#include "engine/animation/ITerrainAdaptation.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

using engine::animation::FootConfig;
using engine::animation::FootGroundResult;
using engine::animation::ITerrainAdaptation;
using engine::animation::TerrainAdaptationResult;
using engine::animation::create_terrain_adaptation;

// Grade 2x2: (0,0)=0, (1,0)=0, (0,1)=0, (1,1)=0 → plano H=0.
void setup_flat(auto& ta, std::string& err) {
    check(ta->set_heightmap("flat", 0.0, 0.0, 1.0, {0, 0, 0, 0}, 2, 2, err) &&
              err.empty(),
          "heightmap plano");
}

// Grade 2x2 inclinada: h00=0, h10=1, h01=0, h11=1.
void setup_ramp(auto& ta, std::string& err) {
    check(ta->set_heightmap("ramp", 0.0, 0.0, 1.0, {0, 1, 0, 1}, 2, 2, err) &&
              err.empty(),
          "heightmap rampa");
}

void test_heightmap() {
    auto ta = create_terrain_adaptation();
    std::string err;
    setup_flat(ta, err);

    check(approx(ta->height_at("flat", 0.0, 0.0, err), 0.0, 1e-12),
          "altura em (0,0) = 0");
    check(approx(ta->height_at("flat", 5.0, -3.0, err), 0.0, 1e-12),
          "clamp: fora da grade → borda");

    auto ta2 = create_terrain_adaptation();
    setup_ramp(ta2, err);
    check(approx(ta2->height_at("ramp", 0.0, 0.0, err), 0.0, 1e-12),
          "rampa (0,0) = 0");
    check(approx(ta2->height_at("ramp", 1.0, 0.0, err), 1.0, 1e-12),
          "rampa (1,0) = 1");
    check(approx(ta2->height_at("ramp", 0.5, 0.0, err), 0.5, 1e-12),
          "rampa (0.5,0) = 0.5 (bilinear)");
    check(approx(ta2->height_at("ramp", 0.5, 0.5, err), 0.5, 1e-12),
          "rampa centro = 0.5 (bilinear 2D)");

    check(!ta->set_heightmap("bad", 0.0, 0.0, 1.0, {0, 0, 0}, 2, 2, err),
          "tamanho errado rejeitado");
    check(!ta->set_heightmap("bad", 0.0, 0.0, 0.0, {0, 0, 0, 0}, 2, 2, err),
          "cell <= 0 rejeitado");
    check(approx(ta->height_at("ghost", 0.0, 0.0, err), 0.0) &&
              err.find("unknown terrain") != std::string::npos,
          "terreno desconhecido → erro");
}

void test_adapt() {
    auto ta = create_terrain_adaptation();
    std::string err;
    setup_flat(ta, err);

    check(ta->configure("root", {{"foot_l", {0, 0, 0.3}, 0.0},
                                 {"foot_r", {0.3, 0.0, 0.0}}},
                        err) &&
              err.empty(),
          "configure 2 pés");

    // Plano H=0, root R=0, pés com oy=0 → d=0, pés delta 0.
    const TerrainAdaptationResult r0 = ta->adapt("flat", 0.0, 0.0, 0.0, err);
    check(err.empty() && approx(r0.root_y, 0.0, 1e-12) && r0.feet.size() == 2,
          "plano: root 0, 2 pés");
    if (r0.feet.size() == 2) {
        check(approx(r0.feet[0].delta_y, 0.0, 1e-12) &&
                  approx(r0.feet[1].delta_y, 0.0, 1e-12),
              "plano: pés sem delta");
    }

    // Terreno 0.3 abaixo: grade 2x2 toda em -0.3.
    check(ta->set_heightmap("low", 0.0, 0.0, 1.0, {-0.3, -0.3, -0.3, -0.3},
                            2, 2, err) &&
              err.empty(),
          "heightmap low");
    const TerrainAdaptationResult r1 = ta->adapt("low", 0.0, 0.0, 0.0, err);
    check(err.empty() && approx(r1.root_y, -0.3, 1e-12),
          "terreno baixo: root desce 0.3");
    check(r1.feet.size() == 2 && approx(r1.feet[0].delta_y, 0.0, 1e-12) &&
              approx(r1.feet[1].delta_y, 0.0, 1e-12),
          "terreno baixo: pés delta 0 (root já pousou)");

    // Degrau: célula (0,0)=0, (1,0)=0.4 → foot_l (0,0.3) H=0; foot_r
    // (0.3,0) H=0.12 (bilinear).
    check(ta->set_heightmap("step", 0.0, 0.0, 1.0,
                            {0.0, 0.4, 0.0, 0.4}, 2, 2, err) &&
              err.empty(),
          "heightmap step");
    const TerrainAdaptationResult r2 = ta->adapt("step", 0.0, 0.0, 0.0, err);
    check(err.empty() && approx(r2.root_y, 0.0, 1e-9),
          "degrau: root fica em 0 (pé esquerdo no chão)");
    if (r2.feet.size() == 2) {
        check(approx(r2.feet[0].delta_y, 0.0, 1e-9),
              "degrau: foot_l delta 0");
        check(approx(r2.feet[1].delta_y, 0.12, 1e-9),
              "degrau: foot_r erguido 0.12 (bilinear 0.3·0.4)");
    }

    // Erros: sem configure.
    auto ta3 = create_terrain_adaptation();
    setup_flat(ta3, err);
    const TerrainAdaptationResult r3 = ta3->adapt("flat", 0.0, 0.0, 0.0, err);
    check(r3.feet.empty() && err.find("configure") != std::string::npos,
          "adapt sem configure → erro");
    // configure inválida.
    check(!ta3->configure("", {{"foot_l", {0, 0, 0}}}, err),
          "root vazio rejeitado");
    check(!ta3->configure("root", {{"foot_l", {0, 0, 0}},
                                   {"foot_l", {0, 0, 0}}},
                          err),
          "osso de pé duplicado rejeitado");
}

void test_state() {
    auto ta = create_terrain_adaptation();
    std::string err;
    setup_flat(ta, err);
    check(ta->configure("root", {{"foot_l", {0, 0, 0.3}}, {"foot_r", {0.3, 0, 0}}},
                        err) &&
              err.empty(),
          "configure");

    const std::string s1 = ta->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto ta2 = create_terrain_adaptation();
    check(ta2->deserialize_state(s1, err) && err.empty(), "deserialize ok");
    check(ta2->serialize_state() == s1, "round-trip bit-exact");
    // Terreno plano H=0 com root em 0.5 → pés descem o root até o chão.
    const TerrainAdaptationResult r = ta2->adapt("flat", 1.0, 0.5, 2.0, err);
    check(err.empty() && approx(r.root_y, 0.0, 1e-12),
          "adapt pós-restore (root desce até o chão no plano)");

    check(!ta2->deserialize_state("{\"terrain\":{},\"config\":{\"root\":1}}",
                                  err),
          "restore malformado rejeitado");
    check(ta2->serialize_state() == s1, "estado intacto após falha");
}

}  // namespace

int main() {
    test_heightmap();
    test_adapt();
    test_state();

    if (g_failures == 0) {
        std::cout << "terrain_adaptation_tests: all checks passed\n";
        return 0;
    }
    std::cout << "terrain_adaptation_tests: " << g_failures << " failure(s)\n";
    return 1;
}

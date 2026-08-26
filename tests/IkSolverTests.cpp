// IkSolverTests — gate do contrato público de IK analítico (agente 4 §4
// item 2, unidade "IK de mãos/pés/olhar"). Prova o triângulo exato do
// 2-bone (alcançável, esticado, lado da dobra, erros degenerados) e o
// look-at (shortest-arc, anti-paralelo, correção de roll, erros).

#include "engine/animation/IIkSolver.hpp"

#include <cmath>
#include <iostream>
#include <string>

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

bool approx_v(const engine::animation::AnimVec3& a,
              const engine::animation::AnimVec3& b, double eps = 1e-9) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

double vlen(const engine::animation::AnimVec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

using engine::animation::AnimQuat;
using engine::animation::AnimVec3;
using engine::animation::IIkSolver;
using engine::animation::TwoBoneResult;
using engine::animation::create_ik_solver;

void test_two_bone_reachable() {
    auto ik = create_ik_solver();
    std::string err;

    // L1=L2=1, alvo (1.5,0,0) → d=1.5 (alcançável).
    const TwoBoneResult r =
        ik->solve_two_bone({0, 0, 0}, {1.5, 0, 0}, 1.0, 1.0, {0, 1, 0}, err);
    check(err.empty(), "2-bone alcançável sem erro");
    check(!r.stretched, "não esticado");
    check(approx(r.elbow_angle, std::acos(0.75), 1e-9),
          "elbow_angle = acos(0.75)");
    check(approx(r.joint_angle, std::acos(-0.125), 1e-9),
          "joint_angle = acos(-0.125)");
    check(approx_v(r.elbow_pos, {0.75, 0.6614378278, 0.0}, 1e-9),
          "elbow_pos = (0.75, sin(acos(0.75)), 0)");
    check(approx_v(r.effector, {1.5, 0, 0}, 1e-9),
          "effector alcança o alvo");

    // Simetria: mesma geometria, comprimentos invertidos → mesmo resultado.
    const TwoBoneResult r2 =
        ik->solve_two_bone({0, 0, 0}, {1.5, 0, 0}, 1.0, 1.0, {0, -1, 0}, err);
    check(err.empty() && approx_v(r2.elbow_pos, {0.75, -0.6614378278, 0.0}),
          "bend_dir (0,-1,0) → dobra espelhada");
}

void test_two_bone_stretch_and_errors() {
    auto ik = create_ik_solver();
    std::string err;

    // Alvo além do alcance → esticado, linha reta.
    const TwoBoneResult s =
        ik->solve_two_bone({0, 0, 0}, {3, 0, 0}, 1.0, 1.0, {0, 1, 0}, err);
    check(err.empty() && s.stretched, "alvo além do alcance → esticado");
    check(approx(s.elbow_angle, 0.0, 1e-9) &&
              approx(s.joint_angle, 3.141592653589793, 1e-9),
          "esticado: ombro 0°, cotovelo 180°");
    check(approx_v(s.elbow_pos, {1, 0, 0}, 1e-9) &&
              approx_v(s.effector, {2, 0, 0}, 1e-9),
          "esticado: elbow (1,0,0), effector (2,0,0)");

    // Alvo dentro do alcance mínimo (d=0.5): sem erro, effector ≈ alvo.
    const TwoBoneResult t =
        ik->solve_two_bone({0, 0, 0}, {0.5, 0, 0}, 1.0, 1.0, {0, 1, 0}, err);
    check(err.empty() && !t.stretched, "alvo próximo ok");
    check(approx_v(t.effector, {0.5, 0, 0}, 1e-9),
          "effector alcança alvo próximo");

    // Erros honestos.
    check(!ik->solve_two_bone({0, 0, 0}, {1, 0, 0}, 0.0, 1.0, {0, 1, 0}, err)
               .stretched &&
              err.find("> 0") != std::string::npos,
          "comprimento zero → erro");
    check(!ik->solve_two_bone({0, 0, 0}, {0, 0, 0}, 1.0, 1.0, {0, 1, 0}, err)
               .stretched &&
              err.find("coincides") != std::string::npos,
          "alvo na origem → erro");
    check(!ik->solve_two_bone({0, 0, 0}, {1, 0, 0}, 1.0, 1.0, {0, 0, 0}, err)
               .stretched &&
              err.find("bend_dir") != std::string::npos,
          "bend_dir nulo → erro");
}

void test_aim() {
    auto ik = create_ik_solver();
    std::string err;

    // +Z → +X com up +Y: 90° em torno de +Y; up preservado (sem roll).
    const AnimQuat q1 =
        ik->solve_aim({0, 0, 1}, {1, 0, 0}, {0, 1, 0}, err);
    check(err.empty(), "aim +Z→+X sem erro");
    check(approx(q1.y, 0.707106781, 1e-6) && approx(q1.w, 0.707106781, 1e-6),
          "quat = 90° em torno de +Y");
    check(approx_v(q1.rotate({0, 0, 1}), {1, 0, 0}, 1e-9),
          "rotate(axis) ≈ target");
    check(approx_v(q1.rotate({0, 1, 0}), {0, 1, 0}, 1e-9),
          "up preservado (sem roll)");

    // +Z → +Y com up +X.
    const AnimQuat q2 =
        ik->solve_aim({0, 0, 1}, {0, 1, 0}, {1, 0, 0}, err);
    check(err.empty(), "aim +Z→+Y ok");
    check(approx_v(q2.rotate({0, 0, 1}), {0, 1, 0}, 1e-9),
          "rotate(axis) ≈ target (+Z→+Y)");

    // Alinhado → identidade.
    const AnimQuat qi =
        ik->solve_aim({0, 0, 1}, {0, 0, 1}, {0, 1, 0}, err);
    check(err.empty() && approx(qi.w, 1.0, 1e-12) &&
              approx(qi.x, 0.0, 1e-12) && approx(qi.y, 0.0, 1e-12) &&
              approx(qi.z, 0.0, 1e-12),
          "target alinhado → identidade");

    // Anti-paralelo: +Z → −Z → 180° (qualquer perpendicular válida).
    const AnimQuat qa =
        ik->solve_aim({0, 0, 1}, {0, 0, -1}, {0, 1, 0}, err);
    check(err.empty(), "aim anti-paralelo sem erro");
    check(approx_v(qa.rotate({0, 0, 1}), {0, 0, -1}, 1e-9),
          "rotate(axis) ≈ −target (anti-paralelo)");

    // Roll: target +Y, up 45° entre X e Y → após a correção, o up fica
    // perpendicular ao target (componente Y zerada).
    const AnimQuat qr = ik->solve_aim(
        {0, 0, 1}, {0, 1, 0}, {0.707106781, 0.707106781, 0}, err);
    check(err.empty(), "aim com roll ok");
    check(approx_v(qr.rotate({0, 0, 1}), {0, 1, 0}, 1e-9),
          "rotate(axis) ≈ target (com roll)");
    const AnimVec3 rolled = qr.rotate({0.707106781, 0.707106781, 0});
    check(approx(rolled.y, 0.0, 1e-6),
          "roll corrigido: up perpendicular ao target");

    // Erro: vetor nulo → identidade default + erro honesto.
    const AnimQuat qz =
        ik->solve_aim({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, err);
    check(approx(qz.w, 1.0, 1e-12) &&
              err.find("non-zero") != std::string::npos,
          "axis nulo → erro");
}

void test_state() {
    auto ik = create_ik_solver();
    std::string err;
    check(ik->serialize_state() == "{}", "estado = {} (sem estado)");
    check(ik->deserialize_state("{}", err) && err.empty(),
          "deserialize {} ok");
    check(!ik->deserialize_state("[]", err), "não-objeto rejeitado");
}

}  // namespace

int main() {
    test_two_bone_reachable();
    test_two_bone_stretch_and_errors();
    test_aim();
    test_state();

    if (g_failures == 0) {
        std::cout << "ik_solver_tests: all checks passed\n";
        return 0;
    }
    std::cout << "ik_solver_tests: " << g_failures << " failure(s)\n";
    return 1;
}

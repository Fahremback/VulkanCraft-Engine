// InertializerTests — gate do contrato público de inertialização (agente 4
// §4 item 46, componente "inertialization"). Prova o resíduo (current ⊖
// target), a continuidade no primeiro tick (sem snap), o decaimento
// monotônico com envelope (1 + t/T)·e^(−t/T), o settled em t ≥ 4T, os erros
// all-or-nothing e o round-trip JSON.

#include "engine/animation/IInertializer.hpp"

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

bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::BonePose;
using engine::animation::IInertializer;
using engine::animation::InertializerResult;
using engine::animation::create_inertializer;

// Pose com um osso `hips` em posição dada.
std::vector<BonePose> pose_hips(const AnimVec3& pos) {
    return {{"hips", {pos, AnimQuat{}, {1, 1, 1}}}};
}

std::vector<BonePose> pose_thigh_rot(const AnimQuat& rot) {
    return {{"thigh", {{0, 1, 0}, rot, {1, 1, 1}}}};
}

const BonePose* find_bone(const std::vector<BonePose>& v,
                          const std::string& bone) {
    for (const BonePose& p : v) {
        if (p.bone == bone) return &p;
    }
    return nullptr;
}

void test_continuity_and_decay() {
    auto in = create_inertializer();
    std::string err;
    in->set_decay_time(0.25, err);

    // Descontinuidade: pose sai de hips (1,0,0) para o alvo (0,0,0).
    check(in->reset(pose_hips({1, 0, 0}), pose_hips({0, 0, 0}), err) &&
              err.empty(),
          "reset ok");
    check(in->is_active(), "ativo após reset");

    // Tick 0: d = envelope(0) = 1 → saída = pose atual (SEM snap).
    const InertializerResult r0 = in->tick(pose_hips({0, 0, 0}), 0.0, err);
    check(err.empty() && r0.pose.size() == 1 &&
              approx(r0.pose[0].local.position.x, 1.0, 1e-9),
          "tick 0: saída = pose atual (continuidade, sem snap)");

    // t=0.1: d = (1+0.4)·e^(−0.4) = 1.4·0.67032 = 0.93845.
    const InertializerResult r1 = in->tick(pose_hips({0, 0, 0}), 0.1, err);
    check(err.empty() && approx(r1.pose[0].local.position.x, 0.938450, 1e-4),
          "t=0.1: d = 0.93845 (decaimento suave)");

    // t=0.2: d = (1+0.8)·e^(−0.8) = 1.8·0.44933 = 0.80879.
    const InertializerResult r2 = in->tick(pose_hips({0, 0, 0}), 0.1, err);
    check(err.empty() && approx(r2.pose[0].local.position.x, 0.808793, 1e-4),
          "t=0.2: d = 0.80879 (monotônico)");
    check(r2.pose[0].local.position.x < r1.pose[0].local.position.x,
          "decaimento monotônico");

    // Atinge t=1.0 (4T) → settled; depois mais ticks até t=2.0 → residual
    // desprezível (d(2.0) = 9·e⁻⁸ ≈ 0.00302).
    bool settledSeen = false;
    double last = r2.pose[0].local.position.x;
    for (int i = 0; i < 18; ++i) {
        const InertializerResult r = in->tick(pose_hips({0, 0, 0}), 0.1, err);
        check(err.empty(), "tick sem erro");
        if (r.settled) settledSeen = true;
        last = r.pose[0].local.position.x;
    }
    check(settledSeen, "settled em t ≥ 4T");
    check(last < 0.01, "resíduo desprezível em t=2.0");
}

void test_rotation_residual() {
    auto in = create_inertializer();
    std::string err;
    in->set_decay_time(0.25, err);
    const AnimQuat yaw90{0, 0.7071067811865476, 0, 0.7071067811865476};

    check(in->reset(pose_thigh_rot(yaw90), pose_thigh_rot(AnimQuat{}), err) &&
              err.empty(),
          "reset rotacional");
    const InertializerResult r0 =
        in->tick(pose_thigh_rot(AnimQuat{}), 0.0, err);
    check(err.empty() && r0.pose.size() == 1 &&
              std::fabs(r0.pose[0].local.rotation.y - 0.7071067811865476) <
                  1e-6,
          "tick 0: rotação continua em 90° (sem snap)");

    // Avança até t=2.0 (20 ticks de 0.1) → settled e convergência.
    bool settled = false;
    double lastY = 1.0;
    const BonePose* thigh = nullptr;
    for (int i = 0; i < 20; ++i) {
        const InertializerResult r =
            in->tick(pose_thigh_rot(AnimQuat{}), 0.1, err);
        check(err.empty(), "tick rotacional sem erro");
        if (r.settled) settled = true;
        thigh = find_bone(r.pose, "thigh");
        if (thigh != nullptr) lastY = thigh->local.rotation.y;
    }
    check(settled, "settled após 4T");
    // Copia o valor para fora do escopo do loop: `thigh` aponta para o
    // `InertializerResult` local ao loop (destruído a cada iteração);
    // ler o ponteiro depois é use-after-scope. Guardamos o último y.
    check(std::fabs(lastY) < 0.01, "converge à identidade (y ≈ 0)");
}

void test_clear_and_errors() {
    auto in = create_inertializer();
    std::string err;
    in->set_decay_time(0.25, err);
    check(in->reset(pose_hips({1, 0, 0}), pose_hips({0, 0, 0}), err) &&
              err.empty(),
          "reset ok");
    in->clear();
    check(!in->is_active(), "inativo após clear");
    const InertializerResult r = in->tick(pose_hips({5, 0, 0}), 0.1, err);
    check(err.empty() && r.settled &&
              approx(r.pose[0].local.position.x, 5.0, 1e-12),
          "tick sem resíduo devolve o alvo intacto");

    check(!in->reset(pose_hips({1, 0, 0}), {}, err),
          "reset com tamanhos diferentes → erro");
    std::vector<BonePose> other = {{"other", {{0, 0, 0}, AnimQuat{}, {1,1,1}}}};
    check(!in->reset(pose_hips({1, 0, 0}), other, err) &&
              err.find("missing from target") != std::string::npos,
          "reset com osso faltando no alvo → erro");

    in->set_decay_time(0.0, err);
    check(!err.empty(), "decay time 0 → erro");
    err.clear();
    in->set_decay_time(-1.0, err);
    check(!err.empty(), "decay time negativo → erro");
}

void test_state() {
    auto in = create_inertializer();
    std::string err;
    in->set_decay_time(0.25, err);
    check(in->reset(pose_hips({1, 0, 0}), pose_hips({0, 0, 0}), err) &&
              err.empty(),
          "reset ok");
    const InertializerResult a1 = in->tick(pose_hips({0, 0, 0}), 0.1, err);
    check(err.empty() && approx(a1.pose[0].local.position.x, 0.938450, 1e-4),
          "tick a1 (t=0.1)");

    const std::string s1 = in->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto in2 = create_inertializer();
    check(in2->deserialize_state(s1, err) && err.empty(), "deserialize ok");
    check(in2->serialize_state() == s1, "round-trip bit-exact");

    // O PRÓXIMO tick do original e do restaurado devem ser idênticos.
    const InertializerResult a2 = in->tick(pose_hips({0, 0, 0}), 0.1, err);
    const InertializerResult b = in2->tick(pose_hips({0, 0, 0}), 0.1, err);
    check(err.empty() && std::fabs(a2.pose[0].local.position.x -
                                   b.pose[0].local.position.x) < 1e-6,
          "próximo tick idêntico pós-restore");

    const std::string s2 = in2->serialize_state();  // estado pós-tick
    check(!in2->deserialize_state("{\"decay\":0,\"time\":0,\"residual\":[]}",
                                  err),
          "restore com decay 0 rejeitado");
    check(in2->serialize_state() == s2, "estado intacto após falha");
}

}  // namespace

int main() {
    test_continuity_and_decay();
    test_rotation_residual();
    test_clear_and_errors();
    test_state();

    if (g_failures == 0) {
        std::cout << "inertializer_tests: all checks passed\n";
        return 0;
    }
    std::cout << "inertializer_tests: " << g_failures << " failure(s)\n";
    return 1;
}

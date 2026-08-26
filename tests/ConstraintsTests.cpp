// ConstraintsTests — gate do contrato público de constraints de articulação
// (agente 4 §4 item 2, unidade "constraints"). Prova o registro all-or-
// nothing, o clamp determinístico de Euler XYZ (eixos independentes, ±inf =
// sem limite, osso fora do limite intacto) e o round-trip JSON bit-exact.

#include "engine/animation/IConstraints.hpp"

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

bool approx_q(const engine::animation::AnimQuat& a,
              const engine::animation::AnimQuat& b, double eps = 1e-9) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps && std::fabs(a.w - b.w) <= eps;
}

using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::BonePose;
using engine::animation::IConstraints;
using engine::animation::JointLimit;
using engine::animation::create_constraints;

constexpr double kPi = 3.14159265358979323846;

// Pose com: thigh yaw 90° (flexão além do limite), head roll 90°, hips 45°.
std::vector<BonePose> make_pose() {
    const double s45 = 0.7071067811865476;
    return {
        {"hips", {{0, 0, 0}, {0, 0.382683432, 0, 0.923879533}, {1, 1, 1}}},
        {"thigh", {{0, 1, 0}, {0, s45, 0, s45}, {1, 1, 1}}},
        {"head", {{0, 1.8, 0}, {s45, 0, 0, s45}, {1, 1, 1}}},
    };
}

void test_add_constraint() {
    auto c = create_constraints();
    std::string err;

    check(c->add_constraint("hinge", {{"thigh", -0.5, 0.5}}, err) &&
              err.empty(),
          "constraint válida aceita");
    check(c->has_constraint("hinge"), "has_constraint hinge");
    check(c->add_constraint("full", {{"thigh", -kPi, kPi}}, err) &&
              err.empty(),
          "limites completos aceitos");

    check(!c->add_constraint("", {{"thigh", -1, 1}}, err),
          "id vazio rejeitado");
    check(!c->add_constraint("hinge", {{"thigh", -1, 1}}, err),
          "id duplicado rejeitado");
    check(!c->add_constraint("x", {{"", -1, 1}}, err),
          "osso vazio rejeitado");
    check(!c->add_constraint("x", {{"thigh", -1, 1}, {"thigh", -1, 1}}, err),
          "osso duplicado rejeitado");
    check(!c->add_constraint("x", {{"thigh", 1, -1}}, err),
          "min > max rejeitado");
}

void test_apply() {
    auto c = create_constraints();
    std::string err;
    // Hinge: y ∈ [−0.5, 0.5] (thigh) — sem limites em x/z.
    check(c->add_constraint("hinge", {{"thigh", -1e300, 1e300, -0.5, 0.5,
                                       -1e300, 1e300}},
                            err) &&
              err.empty(),
          "hinge add");
    // Roll-only: x ∈ [−0.1, 0.1] (head).
    check(c->add_constraint("roll", {{"head", -0.1, 0.1}}, err) && err.empty(),
          "roll add");
    // Sem limites (full range) no hips.
    check(c->add_constraint("free", {{"hips", -kPi, kPi, -kPi, kPi, -kPi,
                                      kPi}},
                            err) &&
              err.empty(),
          "free add");

    const std::vector<BonePose> pose = make_pose();

    // Hinge: thigh yaw 90° (1.5708) → clampado em 0.5; hips intacto.
    const std::vector<BonePose> h = c->apply_constraint("hinge", pose, err);
    check(err.empty() && h.size() == 3, "hinge apply ok");
    if (h.size() == 3) {
        check(h[1].bone == "thigh" && approx(h[1].local.rotation.y, 0.247403959, 1e-6),
              "thigh yaw clampado em 0.5 (sin(0.25) = 0.2474)");
        check(approx_q(h[0].local.rotation, pose[0].local.rotation),
              "hips intacto (sem limite no hinge)");
    }

    // Roll: head roll 90° (1.5708) → clampado em 0.1.
    const std::vector<BonePose> r = c->apply_constraint("roll", pose, err);
    check(err.empty() && r.size() == 3 &&
              approx(r[2].local.rotation.x, 0.049979169, 1e-6),
          "head roll clampado em 0.1 (sin(0.05) = 0.04998)");

    // Free: hips yaw 45° dentro do range → inalterado.
    const std::vector<BonePose> f = c->apply_constraint("free", pose, err);
    check(err.empty() &&
              approx_q(f[0].local.rotation, pose[0].local.rotation),
          "hips dentro do range → inalterado");

    // Constraint desconhecida / osso ausente na pose → erro all-or-nothing.
    check(c->apply_constraint("ghost", pose, err).empty() &&
              err.find("unknown constraint") != std::string::npos,
          "constraint desconhecida → erro");
    std::vector<BonePose> partial = {pose[0]};  // só hips
    check(c->apply_constraint("hinge", partial, err).empty() &&
              err.find("unknown bone") != std::string::npos,
          "osso do limite ausente na pose → erro");
}

void test_state() {
    auto c = create_constraints();
    std::string err;
    check(c->add_constraint("hinge", {{"thigh", -1e300, 1e300, -0.5, 0.5,
                                       -1e300, 1e300}},
                            err) &&
              err.empty(),
          "hinge add");

    const std::string s1 = c->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto c2 = create_constraints();
    check(c2->deserialize_state(s1, err) && err.empty(), "deserialize ok");
    check(c2->serialize_state() == s1, "round-trip bit-exact");

    const std::vector<BonePose> pose = make_pose();
    const std::vector<BonePose> h2 = c2->apply_constraint("hinge", pose, err);
    check(err.empty() && h2.size() == 3 &&
              approx(h2[1].local.rotation.y, 0.247403959, 1e-6),
          "apply pós-restore");

    check(!c2->deserialize_state(
              "{\"x\":[{\"bone\":\"thigh\",\"min_x\":1,\"max_x\":-1,"
              "\"min_y\":0,\"max_y\":0,\"min_z\":0,\"max_z\":0}]}",
              err),
          "restore min>max rejeitado");
    check(!c2->deserialize_state("{\"x\":[1,2]}", err),
          "restore não-objeto rejeitado");
    check(c2->serialize_state() == s1, "estado intacto após falha");
}

}  // namespace

int main() {
    test_add_constraint();
    test_apply();
    test_state();

    if (g_failures == 0) {
        std::cout << "constraints_tests: all checks passed\n";
        return 0;
    }
    std::cout << "constraints_tests: " << g_failures << " failure(s)\n";
    return 1;
}

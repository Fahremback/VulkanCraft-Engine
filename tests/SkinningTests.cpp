// SkinningTests — gate do contrato público de skinning CPU (agente 4 §4
// item 49). Prova skin = world · invBind (translação, escala, rotação pura),
// a deformação por 4 influências com pesos normalizados, os erros
// all-or-nothing e o estado {}.

#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/ISkinning.hpp"

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

bool approx_v(const engine::animation::AnimVec3& a,
              const engine::animation::AnimVec3& b, double eps = 1e-9) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::BonePose;
using engine::animation::IAnimCore;
using engine::animation::ISkinning;
using engine::animation::SkinMatrix;
using engine::animation::SkinVertex;
using engine::animation::SkeletonSpec;
using engine::animation::create_anim_core;
using engine::animation::create_skinning;

// hips (raiz, bind origem) + thigh (bind (0,1,0)).
SkeletonSpec make_human() {
    SkeletonSpec sk;
    sk.id = "human";
    sk.bones = {
        {"hips", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", 0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    return sk;
}

// Um osso com bind ESCALADO 2x.
SkeletonSpec make_scaled() {
    SkeletonSpec sk;
    sk.id = "scaled";
    sk.bones = {
        {"b", -1, {{0, 0, 0}, AnimQuat{}, {2, 2, 2}}},
    };
    return sk;
}

// Um osso com bind ROTACIONADO 90° (yaw).
SkeletonSpec make_rotated() {
    SkeletonSpec sk;
    sk.id = "rotated";
    sk.bones = {
        {"b", -1, {{0, 0, 0}, {0, 0.7071067811865476, 0, 0.7071067811865476},
                   {1, 1, 1}}},
    };
    return sk;
}

struct Fixture {
    std::unique_ptr<IAnimCore> core;
    std::unique_ptr<ISkinning> skin;
};

Fixture make_fixture() {
    Fixture fx;
    fx.core = create_anim_core();
    std::string err;
    check(fx.core->add_skeleton(make_human(), err) && err.empty(),
          "skeleton human");
    check(fx.core->add_skeleton(make_scaled(), err) && err.empty(),
          "skeleton scaled");
    check(fx.core->add_skeleton(make_rotated(), err) && err.empty(),
          "skeleton rotated");
    fx.skin = create_skinning(*fx.core);
    return fx;
}

void test_identity_and_translate() {
    Fixture fx = make_fixture();
    std::string err;

    // Pose = bind → skin identidade.
    const std::vector<BonePose> bindPose = fx.core->bind_pose("human", err);
    check(err.empty(), "bind_pose ok");
    const std::vector<SkinMatrix> m0 =
        fx.skin->skin_matrices("human", bindPose, err);
    check(err.empty() && m0.size() == 2, "skin_matrices bind → 2");
    if (m0.size() == 2) {
        const AnimVec3 v = m0[0].apply({1, 2, 3});
        check(approx_v(v, {1, 2, 3}), "pose = bind → skin identidade");
        const AnimVec3 vt = m0[1].apply({0, 1, 0});
        check(approx_v(vt, {0, 1, 0}), "thigh bind → identidade");
    }

    // Pose: SÓ hips move +2 (thigh local fica no bind) → skin de ambos =
    // translate (2,0,0) (local_to_world COMPÕE a hierarquia).
    std::vector<BonePose> pose = bindPose;
    pose[0].local.position = {2, 0, 0};
    const std::vector<SkinMatrix> m1 = fx.skin->skin_matrices("human", pose, err);
    check(err.empty() && m1.size() == 2, "skin_matrices translate ok");
    const AnimVec3 v1 = m1[1].apply({0, 1, 0});
    check(approx_v(v1, {2, 1, 0}), "vértice do thigh segue o mundo (delta (2,0,0))");
    const AnimVec3 vh = m1[0].apply({1, 0, 0});
    check(approx_v(vh, {3, 0, 0}), "vértice do hips também +2");
}

void test_scale_and_rotation() {
    Fixture fx = make_fixture();
    std::string err;

    // Bind escala 2, pose escala 1 → skin escala 0.5.
    const std::vector<BonePose> bindS = fx.core->bind_pose("scaled", err);
    std::vector<BonePose> poseS = bindS;
    poseS[0].local.scale = {1, 1, 1};
    const std::vector<SkinMatrix> ms =
        fx.skin->skin_matrices("scaled", poseS, err);
    check(err.empty() && ms.size() == 1, "skin escala ok");
    const AnimVec3 vs = ms[0].apply({2, 0, 0});
    check(approx_v(vs, {1, 0, 0}), "escala 2→1 → vértice 0.5x");

    // Bind rot 90° yaw, pose identidade → skin = bind⁻¹ (−90° yaw).
    const std::vector<BonePose> bindR = fx.core->bind_pose("rotated", err);
    std::vector<BonePose> poseR = bindR;
    poseR[0].local.rotation = AnimQuat{};
    const std::vector<SkinMatrix> mr =
        fx.skin->skin_matrices("rotated", poseR, err);
    check(err.empty() && mr.size() == 1, "skin rotação ok");
    const AnimVec3 vr = mr[0].apply({1, 0, 0});
    check(approx_v(vr, {0, 0, 1}, 1e-9),
          "rot bind 90° com pose identidade → desfaz (1,0,0)→(0,0,1)");
}

void test_blend() {
    Fixture fx = make_fixture();
    std::string err;
    // Pose: hips (2,0,0), thigh local (−2,1,0) → world (0,1,0) = bind →
    // skin[0] = T(2,0,0), skin[1] = I (ossos com skins DIFERENTES).
    const std::vector<BonePose> bindPose = fx.core->bind_pose("human", err);
    std::vector<BonePose> pose = bindPose;
    pose[0].local.position = {2, 0, 0};
    pose[1].local.position = {-2, 1, 0};
    const std::vector<SkinMatrix> m = fx.skin->skin_matrices("human", pose, err);
    check(err.empty() && m.size() == 2, "skin blend ok");

    SkinVertex v;
    v.position = {1, 0, 0};
    v.bone0 = 0;
    v.bone1 = 1;
    v.weight0 = 0.5;
    v.weight1 = 0.5;
    const AnimVec3 p1 = fx.skin->apply_skin(m, v, err);
    check(err.empty() && approx_v(p1, {2, 0, 0}),
          "blend 0.5/0.5: (3,0,0)·0.5 + (1,0,0)·0.5 = (2,0,0)");

    // Pesos NÃO normalizados (2,2) → mesma posição.
    v.weight0 = 2.0;
    v.weight1 = 2.0;
    const AnimVec3 p2 = fx.skin->apply_skin(m, v, err);
    check(err.empty() && approx_v(p2, {2, 0, 0}),
          "pesos (2,2) normalizados → mesma posição");

    // Conveniência skin_vertices.
    SkinVertex a;
    a.position = {1, 0, 0};
    a.bone0 = 1;
    a.weight0 = 1.0;
    const std::vector<AnimVec3> all =
        fx.skin->skin_vertices("human", pose, {v, a}, err);
    check(err.empty() && all.size() == 2 &&
              approx_v(all[0], {2, 0, 0}) && approx_v(all[1], {1, 0, 0}),
          "skin_vertices (2 vértices)");
}

void test_errors_and_state() {
    Fixture fx = make_fixture();
    std::string err;
    const std::vector<BonePose> bindPose = fx.core->bind_pose("human", err);

    check(fx.skin->skin_matrices("ghost", bindPose, err).empty() &&
              err.find("unknown skeleton") != std::string::npos,
          "skeleton desconhecida → erro");

    std::vector<BonePose> wrongOrder = {bindPose[1], bindPose[0]};
    check(fx.skin->skin_matrices("human", wrongOrder, err).empty() &&
              err.find("order") != std::string::npos,
          "pose fora de ordem → erro");

    std::vector<BonePose> partial = {bindPose[0]};
    check(fx.skin->skin_matrices("human", partial, err).empty() &&
              err.find("every bone") != std::string::npos,
          "pose incompleta → erro");

    const std::vector<SkinMatrix> m = fx.skin->skin_matrices("human", bindPose, err);
    SkinVertex v;
    v.position = {0, 0, 0};
    v.bone0 = 7;
    v.weight0 = 1.0;
    check(approx_v(fx.skin->apply_skin(m, v, err), {0, 0, 0}) &&
              err.find("out of range") != std::string::npos,
          "índice de osso fora do range → erro");
    v.bone0 = 0;
    v.weight0 = 0.0;
    check(approx_v(fx.skin->apply_skin(m, v, err), {0, 0, 0}) &&
              err.find("no influence") != std::string::npos,
          "peso total zero → erro");

    check(fx.skin->serialize_state() == "{}", "estado = {}");
    check(fx.skin->deserialize_state("{}", err) && err.empty(),
          "deserialize {} ok");
    check(!fx.skin->deserialize_state("[]", err), "não-objeto rejeitado");
}

}  // namespace

int main() {
    test_identity_and_translate();
    test_scale_and_rotation();
    test_blend();
    test_errors_and_state();

    if (g_failures == 0) {
        std::cout << "skinning_tests: all checks passed\n";
        return 0;
    }
    std::cout << "skinning_tests: " << g_failures << " failure(s)\n";
    return 1;
}

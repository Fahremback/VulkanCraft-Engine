// RetargetingTests — gate do contrato público de retargeting de animação
// (agente 4 §4 item 2, unidade "retargeting"). Prova o mapeamento fonte→alvo
// all-or-nothing (skeletons/ossos/escala/duplicatas), a reamostragem com
// escala de posição e bind p/ ossos sem mapeamento, a recusa de clip de
// outra skeleton e o round-trip JSON bit-exact.

#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/IRetargeting.hpp"

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
using engine::animation::ClipSpec;
using engine::animation::IAnimCore;
using engine::animation::IRetargeting;
using engine::animation::RetargetMapping;
using engine::animation::SkeletonSpec;
using engine::animation::create_anim_core;
using engine::animation::create_retargeting;

// Fonte: hips (raiz) + thigh — bind (0,0,0)/(0,1,0); clip walk translada
// hips 0→(1,0,0) em 1s.
SkeletonSpec make_source() {
    SkeletonSpec sk;
    sk.id = "human_s";
    sk.bones = {
        {"hips", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", 0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    return sk;
}

// Alvo: pelvis (raiz) + leg + head — bind (0,0,0)/(0,2,0)/(0,2.2,0).
SkeletonSpec make_target() {
    SkeletonSpec sk;
    sk.id = "human_t";
    sk.bones = {
        {"pelvis", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"leg", 0, {{0, 2, 0}, AnimQuat{}, {1, 1, 1}}},
        {"head", 1, {{0, 2.2, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    return sk;
}

ClipSpec make_walk_s() {
    ClipSpec clip;
    clip.id = "walk_s";
    clip.skeleton = "human_s";
    clip.duration = 1.0;
    clip.tracks = {
        {"hips",
         {{0.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{1, 0, 0}, AnimQuat{}, {1, 1, 1}}}}},
        {"thigh",
         {{0.0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}}}},
    };
    return clip;
}

struct Fixture {
    std::unique_ptr<IAnimCore> core;
    std::unique_ptr<IRetargeting> retarget;
};

Fixture make_fixture() {
    Fixture fx;
    fx.core = create_anim_core();
    std::string err;
    check(fx.core->add_skeleton(make_source(), err) && err.empty(),
          "skeleton fonte aceita");
    check(fx.core->add_skeleton(make_target(), err) && err.empty(),
          "skeleton alvo aceita");
    check(fx.core->add_clip(make_walk_s(), err) && err.empty(),
          "clip walk_s aceito");
    fx.retarget = create_retargeting(*fx.core);
    return fx;
}

void test_add_retarget() {
    Fixture fx = make_fixture();
    std::string err;

    check(fx.retarget->add_retarget(
              "s2t", "human_s", "human_t",
              {{"hips", "pelvis", 2.0}, {"thigh", "leg", 2.0}}, err) &&
              err.empty(),
          "retarget válido aceito");
    check(fx.retarget->has_retarget("s2t"), "has_retarget s2t");
    check(fx.retarget->retarget_ids().size() == 1,
          "retarget_ids = 1");

    check(!fx.retarget->add_retarget("s2t", "human_s", "human_t",
                                     {{"hips", "pelvis", 1.0}}, err) &&
              err.find("duplicate retarget") != std::string::npos,
          "id duplicado rejeitado");
    check(!fx.retarget->add_retarget("x", "ghost", "human_t",
                                     {{"hips", "pelvis", 1.0}}, err),
          "skeleton fonte desconhecida rejeitada");
    check(!fx.retarget->add_retarget("x", "human_s", "ghost",
                                     {{"hips", "pelvis", 1.0}}, err),
          "skeleton alvo desconhecida rejeitada");
    check(!fx.retarget->add_retarget("x", "human_s", "human_t",
                                     {{"spine", "pelvis", 1.0}}, err),
          "osso fonte desconhecido rejeitado");
    check(!fx.retarget->add_retarget("x", "human_s", "human_t",
                                     {{"hips", "toe", 1.0}}, err),
          "osso alvo desconhecido rejeitado");
    check(!fx.retarget->add_retarget("x", "human_s", "human_t",
                                     {{"hips", "pelvis", 1.0},
                                      {"thigh", "pelvis", 1.0}},
                                     err),
          "osso alvo duplicado rejeitado");
    check(!fx.retarget->add_retarget("x", "human_s", "human_t",
                                     {{"hips", "pelvis", 0.0}}, err),
          "scale 0 rejeitado");
    check(fx.retarget->retarget_ids().size() == 1,
          "estado intacto após recusas");
}

void test_retarget_pose() {
    Fixture fx = make_fixture();
    std::string err;
    check(fx.retarget->add_retarget(
              "s2t", "human_s", "human_t",
              {{"hips", "pelvis", 2.0}, {"thigh", "leg", 2.0}}, err) &&
              err.empty(),
          "retarget s2t");

    // t=1: pelvis (1·2,0,0); leg = thigh local (0,1,0)·2 = (0,2,0);
    // head SEM mapeamento → bind (0,2.2,0). Ordem = declaração da alvo.
    const std::vector<BonePose> pose =
        fx.retarget->retarget_pose("s2t", "walk_s", 1.0, err);
    check(err.empty() && pose.size() == 3, "retarget_pose → 3 ossos");
    if (pose.size() == 3) {
        check(pose[0].bone == "pelvis" &&
                  approx_v(pose[0].local.position, {2, 0, 0}, 1e-9),
              "pelvis = hips (1,0,0)·2");
        check(pose[1].bone == "leg" &&
                  approx_v(pose[1].local.position, {0, 2, 0}, 1e-9),
              "leg = thigh local ·2");
        check(pose[2].bone == "head" &&
                  approx_v(pose[2].local.position, {0, 2.2, 0}, 1e-9),
              "head sem mapeamento = bind (0,2.2,0)");
    }

    // t=0: hips (0,0,0) → pelvis (0,0,0).
    const std::vector<BonePose> p0 =
        fx.retarget->retarget_pose("s2t", "walk_s", 0.0, err);
    check(err.empty() && p0.size() == 3 &&
              approx_v(p0[0].local.position, {0, 0, 0}, 1e-9),
          "t=0: pelvis (0,0,0)");

    // Scale 1.0: mapeamento direto sem escala.
    check(fx.retarget->add_retarget("s2t1", "human_s", "human_t",
                                    {{"hips", "pelvis", 1.0}}, err) &&
              err.empty(),
          "retarget scale 1");
    const std::vector<BonePose> p1 =
        fx.retarget->retarget_pose("s2t1", "walk_s", 1.0, err);
    check(err.empty() && p1.size() == 3 &&
              approx_v(p1[0].local.position, {1, 0, 0}, 1e-9),
          "scale 1: pelvis (1,0,0)");

    // Erros honestos.
    check(fx.retarget->retarget_pose("ghost", "walk_s", 0.5, err).empty() &&
              err.find("unknown retarget") != std::string::npos,
          "retarget desconhecido → erro");
    check(fx.retarget->retarget_pose("s2t", "ghost", 0.5, err).empty() &&
              err.find("unknown clip") != std::string::npos,
          "clip desconhecido → erro");
}

void test_clip_wrong_skeleton() {
    Fixture fx = make_fixture();
    std::string err;
    // Clip registrado na skeleton ALVO (nome de osso inexistente na fonte).
    ClipSpec clip;
    clip.id = "walk_t";
    clip.skeleton = "human_t";
    clip.duration = 1.0;
    clip.tracks = {
        {"pelvis",
         {{0.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{1, 0, 0}, AnimQuat{}, {1, 1, 1}}}}},
    };
    check(fx.core->add_clip(clip, err) && err.empty(), "clip walk_t aceito");
    check(fx.retarget->add_retarget("s2t", "human_s", "human_t",
                                    {{"hips", "pelvis", 1.0}}, err) &&
              err.empty(),
          "retarget s2t");
    const std::vector<BonePose> pose =
        fx.retarget->retarget_pose("s2t", "walk_t", 0.5, err);
    check(pose.empty() &&
              err.find("not registered on source skeleton") !=
                  std::string::npos,
          "clip de outra skeleton → erro honesto");
}

void test_state() {
    Fixture fx = make_fixture();
    std::string err;
    check(fx.retarget->add_retarget(
              "s2t", "human_s", "human_t",
              {{"hips", "pelvis", 2.0}, {"thigh", "leg", 2.0}}, err) &&
              err.empty(),
          "add s2t");

    const std::string s1 = fx.retarget->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto r2 = create_retargeting(*fx.core);
    check(r2->deserialize_state(s1, err) && err.empty(), "deserialize ok");
    check(r2->serialize_state() == s1, "round-trip bit-exact");
    const std::vector<BonePose> pose =
        r2->retarget_pose("s2t", "walk_s", 1.0, err);
    check(err.empty() && pose.size() == 3 &&
              approx_v(pose[0].local.position, {2, 0, 0}, 1e-9),
          "retarget_pose após restore");

    check(!r2->deserialize_state(
              "{\"x\":{\"source\":\"ghost\",\"target\":\"human_t\","
              "\"mappings\":[]}}",
              err),
          "restore com skeleton desconhecida rejeitado");
    // Falha NÃO corrompe o estado anterior.
    check(r2->serialize_state() == s1, "estado intacto após falha");
}

}  // namespace

int main() {
    test_add_retarget();
    test_retarget_pose();
    test_clip_wrong_skeleton();
    test_state();

    if (g_failures == 0) {
        std::cout << "retargeting_tests: all checks passed\n";
        return 0;
    }
    std::cout << "retargeting_tests: " << g_failures << " failure(s)\n";
    return 1;
}

// AnimCoreTests — gate do contrato público do núcleo de animação (agente 4 §4
// item 1). Prova que skeleton/clip/blend tree são determinísticos,
// all-or-nothing no registro, bit-exact no round-trip JSON, e que a
// amostragem (lerp/slerp) e a composição local→world se comportam como
// documentado.

#include "engine/animation/IAnimCore.hpp"

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
using engine::animation::BlendSpec;
using engine::animation::Bone;
using engine::animation::ClipSpec;
using engine::animation::IAnimCore;
using engine::animation::SkeletonSpec;
using engine::animation::create_anim_core;

// hips (raiz) + thigh (filho) — bind: hips na origem, thigh a (0,1,0).
SkeletonSpec make_skeleton() {
    SkeletonSpec sk;
    sk.id = "human";
    sk.bones = {
        {"hips", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", 0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    return sk;
}

// Walk: hips translada 0→(1,0,0) em 1s; thigh rotaciona 0°→90° (eixo Y).
ClipSpec make_walk() {
    ClipSpec clip;
    clip.id = "walk";
    clip.skeleton = "human";
    clip.duration = 1.0;
    clip.tracks = {
        {"hips",
         {{0.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{1, 0, 0}, AnimQuat{}, {1, 1, 1}}}}},
        {"thigh",
         {{0.0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{0, 1, 0}, {0, 0.707106781, 0, 0.707106781}, {1, 1, 1}}}}},
    };
    return clip;
}

ClipSpec make_run() {
    ClipSpec clip;
    clip.id = "run";
    clip.skeleton = "human";
    clip.duration = 0.5;
    clip.tracks = {
        {"hips",
         {{0.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
          {0.5, {{3, 0, 0}, AnimQuat{}, {1, 1, 1}}}}},
    };
    return clip;
}

void test_skeleton_validate() {
    SkeletonSpec s = make_skeleton();
    std::string err;
    check(s.validate(err) && err.empty(), "skeleton válida aceita");

    SkeletonSpec bad = s;
    bad.id = "";
    check(!bad.validate(err) && !err.empty(), "id vazio recusa");

    bad = s;
    bad.bones[1].id = "hips";  // duplicado
    check(!bad.validate(err) && !err.empty(), "bone duplicado recusa");

    bad = s;
    bad.bones[0].parent = 1;  // ciclo hips→thigh→hips
    check(!bad.validate(err) && !err.empty(), "ciclo de parentesco recusa");

    bad = s;
    bad.bones[1].parent = 5;  // fora do range
    check(!bad.validate(err) && !err.empty(), "parent fora do range recusa");
}

void test_clip_validate() {
    ClipSpec c = make_walk();
    std::string err;
    check(c.validate(err) && err.empty(), "clip válida aceita");

    ClipSpec bad = c;
    bad.duration = 0.0;
    check(!bad.validate(err) && !err.empty(), "duration 0 recusa");

    bad = c;
    bad.tracks[0].bone = "thigh";  // duplicado
    check(!bad.validate(err) && !err.empty(), "trilha duplicada recusa");

    bad = c;
    bad.tracks[0].keys[1].t = 0.0;  // igual ao anterior → não estritamente crescente
    check(!bad.validate(err) && !err.empty(), "tempos não crescentes recusam");

    bad = c;
    bad.tracks[0].keys[1].t = 2.0;  // além da duration
    check(!bad.validate(err) && !err.empty(), "tempo além da duration recusa");
}

void test_spec_roundtrip() {
    const SkeletonSpec sk = make_skeleton();
    std::string err;
    SkeletonSpec sk2;
    check(sk2.load_from_json(sk.to_json(), err) && err.empty(), "skeleton round-trip");
    check(sk2.to_json() == sk.to_json(), "skeleton bit-exact");

    const ClipSpec cl = make_walk();
    ClipSpec cl2;
    check(cl2.load_from_json(cl.to_json(), err) && err.empty(), "clip round-trip");
    check(cl2.to_json() == cl.to_json(), "clip bit-exact");

    BlendSpec b;
    b.id = "b1";
    b.clip_a = "walk";
    b.clip_b = "run";
    BlendSpec b2;
    check(b2.load_from_json(b.to_json(), err) && err.empty(), "blend round-trip");
    check(b2.to_json() == b.to_json(), "blend bit-exact");

    SkeletonSpec keep = sk2;
    check(!sk2.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(sk2.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
}

void test_add_all_or_nothing() {
    auto core = create_anim_core();
    std::string err;
    check(core->add_skeleton(make_skeleton(), err), "add skeleton");

    ClipSpec orphan = make_walk();
    orphan.skeleton = "nope";
    check(!core->add_clip(orphan, err) && !err.empty(),
          "clip com skeleton desconhecida recusa");

    check(core->add_clip(make_walk(), err), "add walk");
    check(core->add_clip(make_run(), err), "add run");
    check(core->has_clip("walk") && core->has_clip("run"), "clips registradas");

    BlendSpec b;
    b.id = "b1";
    b.clip_a = "walk";
    b.clip_b = "nope";
    check(!core->add_blend(b, err) && !err.empty(), "blend com clip desconhecida recusa");
    b.clip_b = "walk";  // mesma skeleton — ok
    b.clip_a = "walk";
    b.clip_b = "run";
    check(core->add_blend(b, err), "add blend walk/run");

    BlendSpec cross;
    cross.id = "x";
    cross.clip_a = "walk";
    cross.clip_b = "walk";
    check(core->add_blend(cross, err), "blend de clip consigo mesma (mesma skeleton)");
    (void)cross;

    check(core->skeleton_ids().size() == 1 && core->clip_ids().size() == 2 &&
              core->blend_ids().size() == 2,
          "registros: 1 skeleton / 2 clips / 2 blends");
}

void test_sample_clip() {
    auto core = create_anim_core();
    std::string err;
    core->add_skeleton(make_skeleton(), err);
    core->add_clip(make_walk(), err);

    const auto p0 = core->sample_clip("walk", 0.0, err);
    check(p0.size() == 2 && p0[0].bone == "hips" && p0[1].bone == "thigh",
          "pose cobre os ossos na ordem da skeleton");
    check(approx_v(p0[0].local.position, {0, 0, 0}), "t=0: hips na origem");

    const auto p05 = core->sample_clip("walk", 0.5, err);
    check(approx_v(p05[0].local.position, {0.5, 0, 0}),
          "t=0.5: hips translada no meio (lerp)");
    // slerp(identidade, 90°, 0.5) = 45° → {0, sin(22.5°), 0, cos(22.5°)}
    const double s22 = std::sin(22.5 * 3.14159265358979323846 / 180.0);
    const double c22 = std::cos(22.5 * 3.14159265358979323846 / 180.0);
    check(approx(p05[1].local.rotation.y, s22, 1e-7) &&
              approx(p05[1].local.rotation.w, c22, 1e-7),
          "t=0.5: rotação do thigh = 45° (slerp)");

    const auto p1 = core->sample_clip("walk", 1.0, err);
    check(approx_v(p1[0].local.position, {1, 0, 0}), "t=1: hips em (1,0,0)");
    check(approx(p1[1].local.rotation.y, 0.707106781) &&
              approx(p1[1].local.rotation.w, 0.707106781),
          "t=1: rotação do thigh = 90°");
    const auto p2 = core->sample_clip("walk", 5.0, err);  // clamp
    check(approx_v(p2[0].local.position, {1, 0, 0}), "t>duration clampado");
}

void test_sample_blend() {
    auto core = create_anim_core();
    std::string err;
    core->add_skeleton(make_skeleton(), err);
    core->add_clip(make_walk(), err);
    core->add_clip(make_run(), err);
    BlendSpec b;
    b.id = "b1";
    b.clip_a = "walk";
    b.clip_b = "run";
    b.param_min = 0.0;
    b.param_max = 1.0;
    core->add_blend(b, err);

    const auto at_min = core->sample_blend("b1", 0.0, 1.0, err);
    check(approx_v(at_min[0].local.position, {1, 0, 0}),
          "param=min → clip A (walk t=1 → (1,0,0))");
    const auto at_max = core->sample_blend("b1", 1.0, 1.0, err);
    check(approx_v(at_max[0].local.position, {3, 0, 0}),
          "param=max → clip B (run t=1 → (3,0,0))");
    const auto mid = core->sample_blend("b1", 0.5, 1.0, err);
    check(approx_v(mid[0].local.position, {2, 0, 0}),
          "param=0.5 → média ((1+3)/2)");
    const auto clamp = core->sample_blend("b1", 5.0, 1.0, err);
    check(approx_v(clamp[0].local.position, {3, 0, 0}), "param clampado em [0,1]");
}

void test_local_to_world() {
    auto core = create_anim_core();
    std::string err;
    core->add_skeleton(make_skeleton(), err);
    core->add_clip(make_walk(), err);

    const auto pose = core->sample_clip("walk", 1.0, err);  // hips (1,0,0), thigh 90°
    const auto world = core->local_to_world("human", pose, err);
    check(world.size() == 2, "world cobre os 2 ossos");
    check(approx_v(world[0].world.position, {1, 0, 0}),
          "hips mundo = local (raiz)");
    // thigh mundo: rotação do pai (identidade) ∘ (0,1,0) + (1,0,0) = (1,1,0)
    check(approx_v(world[1].world.position, {1, 1, 0}),
          "thigh mundo = (1,1,0)");
    check(approx(world[1].world.rotation.y, 0.707106781) &&
              approx(world[1].world.rotation.w, 0.707106781),
          "thigh rotação mundo = 90°");
}

void test_state_roundtrip() {
    auto a = create_anim_core();
    std::string err;
    a->add_skeleton(make_skeleton(), err);
    a->add_clip(make_walk(), err);
    a->add_clip(make_run(), err);
    BlendSpec b;
    b.id = "b1";
    b.clip_a = "walk";
    b.clip_b = "run";
    a->add_blend(b, err);

    const std::string state = a->serialize_state();
    auto c = create_anim_core();
    check(c->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(c->serialize_state() == state, "state round-trip bit-exact");

    const auto pa = a->sample_clip("walk", 0.5, err);
    const auto pb = c->sample_clip("walk", 0.5, err);
    check(pa.size() == pb.size() &&
              approx_v(pa[0].local.position, pb[0].local.position) &&
              pa[1].local.rotation.y == pb[1].local.rotation.y,
          "amostragem idêntica após restauração");

    auto d = create_anim_core();
    const std::string before = d->serialize_state();
    check(!d->deserialize_state("{\"skeletons\":[],\"clips\":[],\"blends\":["
                                "{\"version\":1,\"id\":\"x\",\"clip_a\":\"nope\","
                                "\"clip_b\":\"nope\"}]}",
                                err) &&
              !err.empty(),
          "blend com clip desconhecida recusa (all-or-nothing)");
    check(d->serialize_state() == before, "recusa não muta (all-or-nothing)");
}

void test_determinism() {
    auto a = create_anim_core();
    auto b = create_anim_core();
    std::string err;
    a->add_skeleton(make_skeleton(), err);
    b->add_skeleton(make_skeleton(), err);
    a->add_clip(make_walk(), err);
    b->add_clip(make_walk(), err);

    const auto pa = a->sample_clip("walk", 0.333, err);
    const auto pb = b->sample_clip("walk", 0.333, err);
    check(pa.size() == pb.size(), "determinismo: mesmo tamanho");
    for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
        check(pa[i].local.position.x == pb[i].local.position.x &&
                  pa[i].local.rotation.y == pb[i].local.rotation.y &&
                  pa[i].local.rotation.w == pb[i].local.rotation.w,
              "determinismo: valores bit-exatos");
    }
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado bit-exato");
}

}  // namespace

int main() {
    test_skeleton_validate();
    test_clip_validate();
    test_spec_roundtrip();
    test_add_all_or_nothing();
    test_sample_clip();
    test_sample_blend();
    test_local_to_world();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "anim_core_tests: all checks passed\n";
    } else {
        std::cout << "anim_core_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}

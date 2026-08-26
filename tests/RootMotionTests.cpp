// RootMotionTests — gate do contrato público de root motion (agente 4 §4 item
// 1). Prova que o delta do osso raiz de um clip é determinístico, bit-exact e
// se comporta como documentado (intervalos parciais e reversos incluídos).

#include "engine/animation/IRootMotion.hpp"

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

using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::Bone;
using engine::animation::ClipSpec;
using engine::animation::IAnimCore;
using engine::animation::IRootMotion;
using engine::animation::RootMotionSample;
using engine::animation::SkeletonSpec;
using engine::animation::create_anim_core;
using engine::animation::create_root_motion;

// hips (raiz) translada 0→(1,0,0) em 1s; thigh gira 0°→90° (eixo Y).
void build_world(std::unique_ptr<IAnimCore>& core, std::string& err) {
    SkeletonSpec sk;
    sk.id = "human";
    sk.bones = {
        {"hips", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", 0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    core->add_skeleton(sk, err);
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
    core->add_clip(clip, err);
}

void test_full_interval() {
    auto core = create_anim_core();
    std::string err;
    build_world(core, err);
    auto rm = create_root_motion();

    const RootMotionSample s = rm->compute(*core, "walk", "hips", 0.0, 1.0, err);
    check(err.empty(), "compute ok");
    check(approx(s.position_delta.x, 1.0) && approx(s.position_delta.y, 0.0) &&
              approx(s.position_delta.z, 0.0),
          "delta completo = (1,0,0)");
    check(approx(s.distance, 1.0), "distância = 1");
    check(approx(s.horizontal_distance, 1.0), "distância horizontal = 1");
    check(s.rotation_delta.w == 1.0, "rotação do hips não muda → identidade");
}

void test_partial_interval() {
    auto core = create_anim_core();
    std::string err;
    build_world(core, err);
    auto rm = create_root_motion();

    const RootMotionSample s = rm->compute(*core, "walk", "hips", 0.25, 0.75, err);
    check(approx(s.position_delta.x, 0.5), "intervalo parcial: delta = 0.5");
    check(approx(s.distance, 0.5), "distância parcial = 0.5");
}

void test_reverse_interval() {
    auto core = create_anim_core();
    std::string err;
    build_world(core, err);
    auto rm = create_root_motion();

    const RootMotionSample s = rm->compute(*core, "walk", "hips", 1.0, 0.0, err);
    check(approx(s.position_delta.x, -1.0), "intervalo reverso: delta = -1");
    check(approx(s.distance, 1.0), "distância é sempre >= 0");
}

void test_refusals() {
    auto core = create_anim_core();
    std::string err;
    build_world(core, err);
    auto rm = create_root_motion();

    const RootMotionSample s1 =
        rm->compute(*core, "nope", "hips", 0.0, 1.0, err);
    check(!err.empty() && s1.distance == 0.0, "clip desconhecida recusa");

    err.clear();
    const RootMotionSample s2 =
        rm->compute(*core, "walk", "spine", 0.0, 1.0, err);
    check(!err.empty() && s2.distance == 0.0, "osso desconhecido recusa");

    err.clear();
    const RootMotionSample s3 =
        rm->compute(*core, "walk", "", 0.0, 1.0, err);
    check(!err.empty() && s3.distance == 0.0, "osso vazio recusa");
}

void test_state_roundtrip() {
    auto rm = create_root_motion();
    std::string err;
    const std::string state = rm->serialize_state();
    check(state == "{}", "estado canônico vazio");
    check(rm->deserialize_state(state, err) && err.empty(), "deserialize aceita");
    check(rm->serialize_state() == "{}", "round-trip bit-exact");
    check(!rm->deserialize_state("[1,2]", err) && !err.empty(),
          "não-objeto recusa");
}

void test_determinism() {
    auto core_a = create_anim_core();
    auto core_b = create_anim_core();
    std::string err;
    build_world(core_a, err);
    build_world(core_b, err);
    auto rma = create_root_motion();
    auto rmb = create_root_motion();

    const RootMotionSample sa =
        rma->compute(*core_a, "walk", "hips", 0.1, 0.9, err);
    err.clear();
    const RootMotionSample sb =
        rmb->compute(*core_b, "walk", "hips", 0.1, 0.9, err);
    check(sa.position_delta.x == sb.position_delta.x &&
              sa.position_delta.y == sb.position_delta.y &&
              sa.position_delta.z == sb.position_delta.z &&
              sa.distance == sb.distance &&
              sa.horizontal_distance == sb.horizontal_distance,
          "determinismo: deltas bit-exatos cross-instance");
}

}  // namespace

int main() {
    test_full_interval();
    test_partial_interval();
    test_reverse_interval();
    test_refusals();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "root_motion_tests: all checks passed\n";
    } else {
        std::cout << "root_motion_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}

// AnimAdditiveTests — gate do contrato público de animação aditiva (agente 4
// §4 item 1, unidade "additive"). Prova que o delta é a pose do clip em t
// RELATIVA à referência (posição subtraída, rotação composta com o inverso,
// escala dividida), que t=ref = identidade, e que a camada aplica por
// correspondência de osso com all-or-nothing em osso desconhecido.

#include "engine/animation/IAnimAdditive.hpp"
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

bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

bool approx_q(const engine::animation::AnimQuat& a,
              const engine::animation::AnimQuat& b, double eps = 1e-6) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) &&
           approx(a.z, b.z, eps) && approx(a.w, b.w, eps);
}

using engine::animation::AdditiveDelta;
using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::BonePose;
using engine::animation::ClipSpec;
using engine::animation::IAnimAdditive;
using engine::animation::IAnimCore;
using engine::animation::SkeletonSpec;
using engine::animation::create_anim_additive;
using engine::animation::create_anim_core;

SkeletonSpec make_skeleton() {
    SkeletonSpec sk;
    sk.id = "human";
    sk.bones = {
        {"hips", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", 0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    return sk;
}

// Walk: hips translada 0→(1,0,0) em 1s; thigh estática no bind.
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
          {1.0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}}}},
    };
    return clip;
}

// Wave: hips estática; thigh rotaciona 0°→90° (eixo Y) em 1s.
ClipSpec make_wave() {
    ClipSpec clip;
    clip.id = "wave";
    clip.skeleton = "human";
    clip.duration = 1.0;
    clip.tracks = {
        {"hips",
         {{0.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}}}},
        {"thigh",
         {{0.0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
          {1.0, {{0, 1, 0}, {0, 0.707106781, 0, 0.707106781}, {1, 1, 1}}}}},
    };
    return clip;
}

struct Fixture {
    std::unique_ptr<IAnimCore> core;
    std::unique_ptr<IAnimAdditive> add;
};

Fixture make_fixture() {
    Fixture fx;
    fx.core = create_anim_core();
    std::string err;
    check(fx.core->add_skeleton(make_skeleton(), err) && err.empty(),
          "skeleton aceita");
    check(fx.core->add_clip(make_walk(), err) && err.empty(), "walk aceito");
    check(fx.core->add_clip(make_wave(), err) && err.empty(), "wave aceito");
    fx.add = create_anim_additive(*fx.core);
    return fx;
}

const AdditiveDelta* find_delta(const std::vector<AdditiveDelta>& v,
                                const std::string& bone) {
    for (const AdditiveDelta& d : v) {
        if (d.bone == bone) return &d;
    }
    return nullptr;
}

void test_sample_additive() {
    Fixture fx = make_fixture();
    std::string err;

    // Wave t=1 vs ref 0: thigh = 90° yaw; hips = identidade.
    const std::vector<AdditiveDelta> d1 =
        fx.add->sample_additive("wave", 1.0, 0.0, err);
    check(err.empty() && d1.size() == 2, "sample wave (1,0) → 2 deltas");
    if (d1.size() == 2) {
        const AdditiveDelta* thigh = find_delta(d1, "thigh");
        check(thigh != nullptr, "delta do thigh presente");
        if (thigh != nullptr) {
            check(approx_q(thigh->local.rotation,
                           AnimQuat{0, 0.707106781, 0, 0.707106781}),
                  "delta thigh = 90° yaw");
            check(approx(thigh->local.position.y, 0.0, 1e-9),
                  "delta thigh posição = 0 (só rotação)");
        }
        const AdditiveDelta* hips = find_delta(d1, "hips");
        check(hips != nullptr, "delta do hips presente");
        if (hips != nullptr) {
            check(approx(hips->local.position.x, 0.0, 1e-9) &&
                      approx(hips->local.position.y, 0.0, 1e-9),
                  "delta hips posição = 0");
            check(approx_q(hips->local.rotation, AnimQuat{}),
                  "delta hips rotação = identidade");
        }
    }

    // Wave t=0.5: thigh = 45° yaw (slerp).
    const std::vector<AdditiveDelta> d2 =
        fx.add->sample_additive("wave", 0.5, 0.0, err);
    check(err.empty() && d2.size() == 2, "sample wave (0.5,0) ok");
    const AdditiveDelta* thigh2 = find_delta(d2, "thigh");
    check(thigh2 != nullptr, "delta thigh (0.5) presente");
    if (thigh2 != nullptr) {
        check(approx_q(thigh2->local.rotation,
                       AnimQuat{0, 0.382683432, 0, 0.923879533}),
              "delta thigh (0.5) = 45° yaw");
    }

    // Walk t=1 vs ref 0: hips translada (1,0,0); rotação identidade.
    const std::vector<AdditiveDelta> d3 =
        fx.add->sample_additive("walk", 1.0, 0.0, err);
    check(err.empty(), "sample walk ok");
    const AdditiveDelta* hips3 = find_delta(d3, "hips");
    check(hips3 != nullptr && approx(hips3->local.position.x, 1.0, 1e-9),
          "delta walk hips = (1,0,0)");

    // t == ref → deltas identidade (todos).
    const std::vector<AdditiveDelta> d4 =
        fx.add->sample_additive("wave", 0.5, 0.5, err);
    check(err.empty() && d4.size() == 2, "sample t==ref ok");
    if (d4.size() == 2) {
        const AdditiveDelta* thigh4 = find_delta(d4, "thigh");
        check(thigh4 != nullptr && approx_q(thigh4->local.rotation, AnimQuat{}),
              "delta t==ref = identidade (rotação)");
    }

    // Erro honesto p/ clip desconhecido.
    const std::vector<AdditiveDelta> d5 =
        fx.add->sample_additive("jump", 0.0, 0.0, err);
    check(d5.empty() && err.find("unknown clip") != std::string::npos,
          "clip desconhecido → erro");
}

void test_layer_additive() {
    Fixture fx = make_fixture();
    std::string err;

    // Base = walk em t=0.5 (hips (0.5,0,0), thigh bind); deltas = wave t=1.
    const std::vector<BonePose> base = fx.core->sample_clip("walk", 0.5, err);
    check(err.empty() && base.size() == 2, "base walk (0.5) ok");
    const std::vector<AdditiveDelta> deltas =
        fx.add->sample_additive("wave", 1.0, 0.0, err);
    check(err.empty(), "deltas wave ok");

    const std::vector<BonePose> layered =
        fx.add->layer_additive(base, deltas, err);
    check(err.empty() && layered.size() == 2, "layer ok");
    for (const BonePose& p : layered) {
        if (p.bone == "hips") {
            check(approx(p.local.position.x, 0.5, 1e-9) &&
                      approx(p.local.position.y, 0.0, 1e-9),
                  "hips mantém translação da base (delta pos 0)");
        } else if (p.bone == "thigh") {
            check(approx_q(p.local.rotation,
                           AnimQuat{0, 0.707106781, 0, 0.707106781}),
                  "thigh = base(identidade) · delta 90°");
            check(approx(p.local.position.y, 1.0, 1e-9),
                  "thigh posição mantém bind (delta pos 0)");
        }
    }

    // Layer com osso desconhecido → erro all-or-nothing (nada aplicado).
    std::vector<AdditiveDelta> bad = deltas;
    bad.push_back({"spine", {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}});
    const std::vector<BonePose> rejected =
        fx.add->layer_additive(base, bad, err);
    check(rejected.empty() &&
              err.find("unknown bone") != std::string::npos,
          "osso desconhecido → erro, nada aplicado");

    // Layer com apenas UM delta: o outro osso permanece intacto.
    std::vector<AdditiveDelta> partial;
    if (const AdditiveDelta* thigh = find_delta(deltas, "thigh")) {
        partial.push_back(*thigh);
    }
    const std::vector<BonePose> partialLayered =
        fx.add->layer_additive(base, partial, err);
    check(err.empty(), "layer parcial ok");
    for (const BonePose& p : partialLayered) {
        if (p.bone == "hips") {
            check(approx(p.local.position.x, 0.5, 1e-9),
                  "hips intacto (sem delta)");
        } else if (p.bone == "thigh") {
            check(approx_q(p.local.rotation,
                           AnimQuat{0, 0.707106781, 0, 0.707106781}),
                  "thigh rotacionado pelo delta parcial");
        }
    }
}

void test_state() {
    Fixture fx = make_fixture();
    std::string err;
    check(fx.add->serialize_state() == "{}", "estado = {} (sem estado)");
    check(fx.add->deserialize_state("{}", err) && err.empty(),
          "deserialize {} ok");
    check(!fx.add->deserialize_state("[]", err),
          "deserialize não-objeto rejeitado");
}

}  // namespace

int main() {
    test_sample_additive();
    test_layer_additive();
    test_state();

    if (g_failures == 0) {
        std::cout << "anim_additive_tests: all checks passed\n";
        return 0;
    }
    std::cout << "anim_additive_tests: " << g_failures << " failure(s)\n";
    return 1;
}

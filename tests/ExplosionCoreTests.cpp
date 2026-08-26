// ExplosionCoreTests — gate do contrato público de explosão CORE (agente 4
// §5 item 58, componente blast determinístico). Prova o falloff
// (1 − d/radius)^power com clamp, a validação all-or-nothing do spec e os
// fragmentos determinísticos por seed (direção unitária, speed no range,
// massa uniforme, mesmo seed → bit-exact).
//
// NOTA: renomeado de ExplosionTests.cpp → ExplosionCoreTests.cpp após um
// clobber acidental (o arquivo ExplosionTests.cpp pertence ao AGENT-2 —
// teste de integração do ExplosionRuntime; reconstruído separadamente).

#include "engine/physics/IExplosion.hpp"

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

double vlen(const engine::animation::AnimVec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

using engine::animation::AnimVec3;
using engine::physics::ExplosionFragment;
using engine::physics::ExplosionSample;
using engine::physics::ExplosionSpec;
using engine::physics::IExplosion;
using engine::physics::create_explosion;

void test_configure() {
    auto e = create_explosion();
    std::string err;
    ExplosionSpec s;

    check(e->configure(s, err) && err.empty(), "spec default aceito");
    check(approx(e->spec().radius, 5.0, 1e-12), "spec() reflete o default");

    s.radius = 0.0;
    check(!e->configure(s, err) && err.find("radius") != std::string::npos,
          "radius 0 → erro");
    s.radius = 5.0;
    s.impulse = -1.0;
    check(!e->configure(s, err), "impulse negativo → erro");
    s.impulse = 1000.0;
    s.falloff_power = -1.0;
    check(!e->configure(s, err), "falloff_power negativo → erro");
    s.falloff_power = 2.0;
    s.fragments = 0;
    check(!e->configure(s, err), "fragments 0 → erro");
    s.fragments = 16;
    s.fragment_speed = -5.0;
    check(!e->configure(s, err), "fragment_speed negativo → erro");
    s.fragment_speed = 20.0;
    check(e->configure(s, err) && err.empty(), "spec válido re-aceito");
}

void test_sample() {
    auto e = create_explosion();
    std::string err;
    ExplosionSpec s;
    s.radius = 10.0;
    s.impulse = 100.0;
    s.heat = 50.0;
    s.damage = 25.0;
    s.falloff_power = 2.0;
    check(e->configure(s, err) && err.empty(), "configure");

    const ExplosionSample c = e->sample_at(0.0, err);
    check(err.empty() && approx(c.falloff, 1.0, 1e-12) &&
              approx(c.impulse, 100.0, 1e-9) && approx(c.heat, 50.0, 1e-9) &&
              approx(c.damage, 25.0, 1e-9),
          "epicentro: falloff 1, valores cheios");

    const ExplosionSample mid = e->sample_at(5.0, err);
    check(err.empty() && approx(mid.falloff, 0.25, 1e-9) &&
              approx(mid.impulse, 25.0, 1e-9),
          "d = raio/2, power 2 → falloff 0.25, impulso 25");

    const ExplosionSample edge = e->sample_at(10.0, err);
    check(err.empty() && approx(edge.falloff, 0.0, 1e-12) &&
              approx(edge.impulse, 0.0, 1e-12),
          "d = raio → zero");

    const ExplosionSample out = e->sample_at(20.0, err);
    check(err.empty() && approx(out.impulse, 0.0, 1e-12),
          "d > raio → zero");

    check(approx(e->sample_at(-1.0, err).falloff, 0.0) &&
              err.find(">= 0") != std::string::npos,
          "d negativo → erro");

    // Power 1 (linear): meio → 0.5.
    ExplosionSpec lin = s;
    lin.falloff_power = 1.0;
    check(e->configure(lin, err) && err.empty(), "configure linear");
    const ExplosionSample lm = e->sample_at(5.0, err);
    check(err.empty() && approx(lm.falloff, 0.5, 1e-9),
          "power 1 → falloff linear 0.5 no meio");
}

void test_fragments() {
    auto e = create_explosion();
    std::string err;
    ExplosionSpec s;
    s.fragments = 8;
    s.fragment_speed = 20.0;
    check(e->configure(s, err) && err.empty(), "configure");

    const std::vector<ExplosionFragment> f1 = e->fragments(42, err);
    const std::vector<ExplosionFragment> f2 = e->fragments(42, err);
    const std::vector<ExplosionFragment> f3 = e->fragments(7, err);
    check(err.empty() && f1.size() == 8, "8 fragmentos");
    check(f1.size() == f2.size(), "mesmo seed → mesmo nº");
    bool identical = true;
    for (std::size_t i = 0; i < f1.size() && identical; ++i) {
        identical = approx(f1[i].direction.x, f2[i].direction.x, 1e-12) &&
                    approx(f1[i].direction.y, f2[i].direction.y, 1e-12) &&
                    approx(f1[i].direction.z, f2[i].direction.z, 1e-12) &&
                    approx(f1[i].speed, f2[i].speed, 1e-12);
    }
    check(identical, "mesmo seed → fragmentos bit-exact");

    bool differs = false;
    for (std::size_t i = 0; i < f1.size() && !differs; ++i) {
        differs = !(approx(f1[i].direction.x, f3[i].direction.x, 1e-12) &&
                    approx(f1[i].direction.y, f3[i].direction.y, 1e-12) &&
                    approx(f1[i].direction.z, f3[i].direction.z, 1e-12));
    }
    check(differs, "seed diferente → direções diferentes");

    double massSum = 0.0;
    for (const ExplosionFragment& f : f1) {
        check(approx(vlen(f.direction), 1.0, 1e-9),
              "direção unitária");
        check(f.speed >= 10.0 - 1e-9 && f.speed <= 20.0 + 1e-9,
              "speed em [0.5·20, 20]");
        massSum += f.mass;
    }
    check(approx(massSum, 1.0, 1e-12), "massa total = 1 (uniforme)");
}

void test_state() {
    auto e = create_explosion();
    std::string err;
    ExplosionSpec s;
    s.radius = 12.0;
    s.fragments = 4;
    check(e->configure(s, err) && err.empty(), "configure");

    const std::string s1 = e->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto e2 = create_explosion();
    check(e2->deserialize_state(s1, err) && err.empty(), "deserialize ok");
    check(e2->serialize_state() == s1, "round-trip bit-exact");
    check(approx(e2->spec().radius, 12.0, 1e-12), "spec restaurado");
    const ExplosionSample c = e2->sample_at(0.0, err);
    check(err.empty() && approx(c.falloff, 1.0, 1e-12),
          "sample pós-restore");

    check(!e2->deserialize_state(
              "{\"radius\":0,\"impulse\":0,\"heat\":0,\"damage\":0,"
              "\"falloff_power\":2,\"fragments\":4,\"fragment_speed\":20}",
              err),
          "restore com radius 0 rejeitado");
    check(e2->serialize_state() == s1, "estado intacto após falha");
}

}  // namespace

int main() {
    test_configure();
    test_sample();
    test_fragments();
    test_state();

    if (g_failures == 0) {
        std::cout << "explosion_core_tests: all checks passed\n";
        return 0;
    }
    std::cout << "explosion_core_tests: " << g_failures << " failure(s)\n";
    return 1;
}

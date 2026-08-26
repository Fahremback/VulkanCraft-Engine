// AnimEventsTests — gate do contrato público de eventos de animação (agente 4
// §4 item 1, unidade "events"). Prova que o registro é all-or-nothing
// (clip/duração validados), o polling é meio-aberto determinístico (t0, t1],
// a ordem canônica é (tempo, inserção) e o round-trip JSON é bit-exact.

#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/IAnimEvents.hpp"

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

using engine::animation::AnimEvent;
using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::ClipSpec;
using engine::animation::IAnimCore;
using engine::animation::IAnimEvents;
using engine::animation::SkeletonSpec;
using engine::animation::create_anim_core;
using engine::animation::create_anim_events;

SkeletonSpec make_skeleton() {
    SkeletonSpec sk;
    sk.id = "human";
    sk.bones = {
        {"hips", -1, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", 0, {{0, 1, 0}, AnimQuat{}, {1, 1, 1}}},
    };
    return sk;
}

ClipSpec make_clip(const std::string& id, double duration) {
    ClipSpec clip;
    clip.id = id;
    clip.skeleton = "human";
    clip.duration = duration;
    clip.tracks = {
        {"hips",
         {{0.0, {{0, 0, 0}, AnimQuat{}, {1, 1, 1}}},
          {duration, {{1, 0, 0}, AnimQuat{}, {1, 1, 1}}}}},
    };
    return clip;
}

// Retorna um par (core, events) já populado com os clips walk(1s)/run(0.5s).
struct Fixture {
    std::unique_ptr<IAnimCore> core;
    std::unique_ptr<IAnimEvents> events;
};

Fixture make_fixture() {
    Fixture fx;
    fx.core = create_anim_core();
    std::string err;
    check(fx.core->add_skeleton(make_skeleton(), err) && err.empty(),
          "skeleton aceita");
    check(fx.core->add_clip(make_clip("walk", 1.0), err) && err.empty(),
          "clip walk aceito");
    check(fx.core->add_clip(make_clip("run", 0.5), err) && err.empty(),
          "clip run aceito");
    fx.events = create_anim_events(*fx.core);
    return fx;
}

void test_add_event_validation() {
    Fixture fx = make_fixture();
    std::string err;

    check(fx.events->add_event({"walk", 0.25, "footstep"}, err) && err.empty(),
          "evento válido aceito");
    check(fx.events->add_event({"walk", 0.5, "land"}, err) && err.empty(),
          "segundo evento aceito");
    check(fx.events->add_event({"run", 0.0, "start"}, err) && err.empty(),
          "evento em t=0 aceito");
    check(fx.events->add_event({"walk", 1.0, "end"}, err) && err.empty(),
          "evento em t=duration aceito");

    check(!fx.events->add_event({"jump", 0.1, "x"}, err) &&
              err.find("unknown clip") != std::string::npos,
          "clip desconhecido rejeitado");
    check(!fx.events->add_event({"walk", 1.5, "x"}, err) &&
              err.find("exceeds") != std::string::npos,
          "tempo além da duração rejeitado");
    check(!fx.events->add_event({"walk", -0.1, "x"}, err),
          "tempo negativo rejeitado");
    check(!fx.events->add_event({"walk", 0.5, ""}, err),
          "nome vazio rejeitado");
    check(!fx.events->add_event({"walk", 0.25, "footstep"}, err) &&
              err.find("duplicate") != std::string::npos,
          "duplicata exata rejeitada");
}

void test_order_and_poll() {
    Fixture fx = make_fixture();
    std::string err;

    // Adiciona fora de ordem — a query deve ordenar por (tempo, inserção).
    check(fx.events->add_event({"walk", 0.75, "impact"}, err) && err.empty(),
          "add 0.75");
    check(fx.events->add_event({"walk", 0.25, "footstep"}, err) && err.empty(),
          "add 0.25");
    check(fx.events->add_event({"walk", 0.5, "land"}, err) && err.empty(),
          "add 0.5");
    check(fx.events->add_event({"walk", 0.5, "second"}, err) && err.empty(),
          "add 0.5 (segunda, mesma hora)");

    const std::vector<AnimEvent> all = fx.events->events_for("walk", err);
    check(err.empty() && all.size() == 4, "events_for retorna 4");
    if (all.size() == 4) {
        check(all[0].time == 0.25 && all[0].name == "footstep",
              "ordem: 0.25 primeiro");
        check(all[1].time == 0.5 && all[1].name == "land",
              "ordem: 0.5 land (inserção)");
        check(all[2].time == 0.5 && all[2].name == "second",
              "ordem: 0.5 second depois de land");
        check(all[3].time == 0.75 && all[3].name == "impact",
              "ordem: 0.75 por último");
    }

    // Polling meio-aberto (t0, t1].
    const std::vector<AnimEvent> p1 = fx.events->poll("walk", 0.0, 0.3, err);
    check(err.empty() && p1.size() == 1 && p1[0].name == "footstep",
          "poll (0,0.3] → footstep");
    const std::vector<AnimEvent> p2 = fx.events->poll("walk", 0.25, 0.5, err);
    check(err.empty() && p2.size() == 2 && p2[0].name == "land" &&
              p2[1].name == "second",
          "poll (0.25,0.5] → land+second (0.25 NÃO re-dispara, 0.5 dispara)");
    const std::vector<AnimEvent> p3 = fx.events->poll("walk", 0.5, 0.75, err);
    check(err.empty() && p3.size() == 1 && p3[0].name == "impact",
          "poll (0.5,0.75] → impact");
    const std::vector<AnimEvent> p4 = fx.events->poll("walk", 0.0, 1.0, err);
    check(err.empty() && p4.size() == 4, "poll (0,1] → todos");
    const std::vector<AnimEvent> p5 = fx.events->poll("run", 0.0, 1.0, err);
    check(err.empty() && p5.empty(), "poll de clip sem eventos → vazio");

    check(!fx.events->poll("jump", 0.0, 1.0, err).empty() == false &&
              err.find("unknown clip") != std::string::npos,
          "poll de clip desconhecido → erro");
}

void test_remove() {
    Fixture fx = make_fixture();
    std::string err;
    check(fx.events->add_event({"walk", 0.25, "footstep"}, err) && err.empty(),
          "add footstep");
    check(fx.events->remove_event("walk", 0.25, "footstep"),
          "remove footstep");
    check(fx.events->events_for("walk", err).empty(),
          "events_for vazio após remover");
    check(!fx.events->remove_event("walk", 0.25, "footstep"),
          "remove inexistente → false");
}

void test_round_trip() {
    Fixture fx = make_fixture();
    std::string err;
    check(fx.events->add_event({"walk", 0.25, "footstep"}, err) && err.empty(),
          "add footstep");
    check(fx.events->add_event({"walk", 0.75, "impact"}, err) && err.empty(),
          "add impact");
    check(fx.events->add_event({"run", 0.5, "finish"}, err) && err.empty(),
          "add finish");

    const std::string s1 = fx.events->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    // Instância nova sobre o MESMO core → restaurar e re-serializar.
    Fixture fx2 = make_fixture();
    check(fx2.events->deserialize_state(s1, err) && err.empty(),
          "deserialize ok");
    check(fx2.events->serialize_state() == s1,
          "round-trip bit-exact");
    const std::vector<AnimEvent> restored =
        fx2.events->poll("walk", 0.0, 0.5, err);
    check(err.empty() && restored.size() == 1 &&
              restored[0].name == "footstep",
          "poll após restore");
    const std::vector<AnimEvent> r2 = fx2.events->events_for("run", err);
    check(err.empty() && r2.size() == 1 && r2[0].name == "finish",
          "events_for após restore");

    // Rejeições all-or-nothing na restauração.
    check(!fx2.events->deserialize_state(
              "[{\"clip\":\"jump\",\"time\":0.1,\"name\":\"x\"}]", err),
          "restore com clip desconhecido rejeitado");
    check(!fx2.events->deserialize_state(
              "[{\"clip\":\"walk\",\"time\":9.0,\"name\":\"x\"}]", err),
          "restore com tempo além da duração rejeitado");
    check(!fx2.events->deserialize_state(
              "[{\"clip\":\"walk\",\"time\":0.25,\"name\":\"a\"},"
              "{\"clip\":\"walk\",\"time\":0.25,\"name\":\"a\"}]",
              err),
          "restore com duplicata rejeitado");
    check(!fx2.events->deserialize_state("{\"not\":\"array\"}", err),
          "restore não-array rejeitado");
    // Falha NÃO corrompe o estado anterior.
    check(fx2.events->serialize_state() == s1,
          "estado intacto após restauração falha");
}

}  // namespace

int main() {
    test_add_event_validation();
    test_order_and_poll();
    test_remove();
    test_round_trip();

    if (g_failures == 0) {
        std::cout << "anim_events_tests: all checks passed\n";
        return 0;
    }
    std::cout << "anim_events_tests: " << g_failures << " failure(s)\n";
    return 1;
}

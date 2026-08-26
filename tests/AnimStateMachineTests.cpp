// AnimStateMachineTests — gate do contrato público de animation state machine
// (agente 4 §4 item 1). Prova que as transições por evento/condição/timer, o
// tempo com speed, o loop e a persistência são determinísticos, all-or-nothing
// e bit-exact.

#include "engine/animation/IAnimStateMachine.hpp"

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

using engine::animation::AnimState;
using engine::animation::AnimStateMachineSpec;
using engine::animation::AnimTransition;
using engine::animation::IAnimStateMachine;
using engine::animation::create_anim_state_machine;

// idle --(enemy_seen)--> chase --(low_hp)--> flee --(2s)--> dead(terminal)
AnimStateMachineSpec make_spec() {
    AnimStateMachineSpec spec;
    spec.id = "vulkancraft:guard";
    spec.initial = "idle";
    spec.states = {
        {"idle", "vulkancraft:anim_idle", 1.0, true},
        {"chase", "vulkancraft:anim_run", 1.5, true},
        {"flee", "vulkancraft:anim_run", 2.0, false},
        {"dead", "vulkancraft:anim_dead", 1.0, false},
    };
    spec.transitions = {
        {"idle", "chase", "enemy_seen", "", 0.0, 0.2},
        {"chase", "flee", "", "low_hp", 0.0, 0.2},
        {"flee", "dead", "", "", 2.0, 0.0},
    };
    return spec;
}

void test_validate() {
    AnimStateMachineSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    AnimStateMachineSpec bad = s;
    bad.states.clear();
    check(!bad.validate(err) && !err.empty(), "sem estados recusa");

    bad = s;
    bad.states[1].id = "idle";  // duplicado
    check(!bad.validate(err) && !err.empty(), "id duplicado recusa");

    bad = s;
    bad.initial = "nope";
    check(!bad.validate(err) && !err.empty(), "initial desconhecido recusa");

    bad = s;
    bad.states[0].clip = "";
    check(!bad.validate(err) && !err.empty(), "clip vazio recusa");

    bad = s;
    bad.states[1].speed = -1.0;
    check(!bad.validate(err) && !err.empty(), "speed negativo recusa");

    bad = s;
    bad.transitions[0].on_condition = "x";  // agora tem 2 gatilhos
    check(!bad.validate(err) && !err.empty(), "dois gatilhos recusam");

    bad = s;
    bad.transitions[2].after_seconds = 0.0;  // nenhum gatilho ativo
    check(!bad.validate(err) && !err.empty(), "sem gatilho recusa");
}

void test_spec_roundtrip() {
    const AnimStateMachineSpec spec = make_spec();
    const std::string json = spec.to_json();
    AnimStateMachineSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    AnimStateMachineSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json(
              "{\"version\":2,\"id\":\"x\",\"initial\":\"i\",\"states\":[]}",
              err) &&
              !err.empty(),
          "versão desconhecida recusa");
}

void test_start_and_clip() {
    auto m = create_anim_state_machine();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->start(err), "start");
    check(m->state() == "idle", "estado inicial = idle");
    check(m->clip() == "vulkancraft:anim_idle", "clip do estado inicial");
    check(m->is_looping(), "idle loopa");
    check(!m->done(), "idle não é terminal");
    check(m->time_in_state() == 0.0, "tempo inicial 0");
}

void test_event_transition() {
    auto m = create_anim_state_machine();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->start(err), "start");
    check(m->send_event("enemy_seen", err), "send_event enemy_seen");
    check(m->state() == "chase", "evento transita idle → chase");
    check(m->clip() == "vulkancraft:anim_run", "clip do chase");
    check(m->state_speed() == 1.5, "speed do chase = 1.5");
    check(m->time_in_state() == 0.0, "tempo resetado na transição");
}

void test_condition_and_speed() {
    auto m = create_anim_state_machine();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->start(err), "start");
    m->send_event("enemy_seen", err);
    check(m->state() == "chase", "chase");

    check(m->set_condition("low_hp", false, err), "cond false");
    check(m->tick(1.0, err), "tick 1s");
    check(m->state() == "chase", "condição falsa não transita");
    check(approx(m->time_in_state(), 1.5), "tempo avança com speed 1.5 (1s·1.5)");

    check(m->set_condition("low_hp", true, err), "cond true");
    check(m->tick(0.0, err), "tick 0");
    check(m->state() == "flee", "condição verdadeira transita chase → flee");
    check(!m->is_looping(), "flee não loopa");
}

void test_timer_transition() {
    auto m = create_anim_state_machine();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->start(err), "start");
    m->send_event("enemy_seen", err);
    m->set_condition("low_hp", true, err);
    m->tick(0.1, err);
    check(m->state() == "flee", "condição verdadeira transita no mesmo tick");
    check(approx(m->time_in_state(), 0.0), "tempo resetado ao entrar em flee");

    check(m->tick(0.9, err), "tick 0.9");
    check(m->state() == "flee", "0.9s·2.0=1.8 < 2s ainda flee");
    check(approx(m->time_in_state(), 1.8), "tempo flee acumula com speed 2.0");
    check(m->tick(0.1, err), "tick 0.1 (>= 2s total)");
    check(m->state() == "dead", "timer 2s transita flee → dead");
    check(m->done(), "dead é terminal");
    check(m->time_in_state() == 0.0, "tempo resetado");
}

void test_terminal_no_further_transition() {
    auto m = create_anim_state_machine();
    std::string err;
    check(m->configure(make_spec(), err), "configure");
    check(m->start(err), "start");
    m->send_event("enemy_seen", err);
    m->set_condition("low_hp", true, err);
    m->tick(0.1, err);
    m->tick(2.0, err);
    check(m->state() == "dead" && m->done(), "dead");
    check(m->send_event("enemy_seen", err), "event em terminal é no-op");
    check(m->state() == "dead", "estado permanece dead");
}

void test_transition_order_first_match_wins() {
    AnimStateMachineSpec spec;
    spec.id = "t";
    spec.initial = "s0";
    spec.states = {{"s0", "c0", 1.0, true}, {"s1", "c1", 1.0, true},
                   {"s2", "c2", 1.0, true}};
    spec.transitions = {
        {"s0", "s1", "", "both", 0.0, 0.0},
        {"s0", "s2", "", "both", 0.0, 0.0},
    };
    auto m = create_anim_state_machine();
    std::string err;
    check(m->configure(spec, err), "configure");
    check(m->start(err), "start");
    check(m->set_condition("both", true, err), "cond");
    check(m->tick(0.1, err), "tick");
    check(m->state() == "s1", "primeira transição que casa vence (ordem)");
}

void test_state_roundtrip() {
    auto a = create_anim_state_machine();
    std::string err;
    check(a->configure(make_spec(), err), "configure");
    check(a->start(err), "start");
    a->send_event("enemy_seen", err);
    check(a->tick(0.5, err), "tick");
    check(a->set_condition("low_hp", true, err), "cond");

    const std::string state = a->serialize_state();
    auto b = create_anim_state_machine();
    check(b->configure(make_spec(), err), "configure b");
    check(b->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(b->serialize_state() == state, "state round-trip bit-exact");
    check(b->state() == "chase" && approx(b->time_in_state(), 0.75),
          "estado/tempo/speed restaurados (0.5s·1.5)");

    auto c = create_anim_state_machine();
    check(c->configure(make_spec(), err), "configure c");
    check(!c->deserialize_state(
              "{\"state\":\"nope\",\"time\":0,\"started\":true,\"conditions\":{}}",
              err) &&
              !err.empty(),
          "estado desconhecido recusa");
}

void test_determinism() {
    auto a = create_anim_state_machine();
    auto b = create_anim_state_machine();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    check(a->start(err), "start a");
    check(b->start(err), "start b");
    a->send_event("enemy_seen", err);
    b->send_event("enemy_seen", err);
    a->set_condition("low_hp", true, err);
    b->set_condition("low_hp", true, err);
    for (int i = 0; i < 12; ++i) {
        a->tick(0.5, err);
        b->tick(0.5, err);
    }
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado bit-exato");
    check(a->state() == b->state() && a->time_in_state() == b->time_in_state(),
          "determinismo: estado/tempo idênticos");
}

}  // namespace

int main() {
    test_validate();
    test_spec_roundtrip();
    test_start_and_clip();
    test_event_transition();
    test_condition_and_speed();
    test_timer_transition();
    test_terminal_no_further_transition();
    test_transition_order_first_match_wins();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "anim_state_machine_tests: all checks passed\n";
    } else {
        std::cout << "anim_state_machine_tests: " << g_failures
                  << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}

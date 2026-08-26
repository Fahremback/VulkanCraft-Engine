// FsmTests — gate do contrato público de máquina de estados finita (agente 4
// §3 item 3). Prova que a FSM data-driven é determinística, all-or-nothing no
// load, bit-exact no round-trip JSON, e que transições por evento/condição/
// timer + actions enter/update/exit + terminal se comportam como documentado.

#include "engine/ai/IFsm.hpp"

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

using engine::ai::FsmSpec;
using engine::ai::FsmState;
using engine::ai::FsmTransition;
using engine::ai::IFsm;
using engine::ai::create_fsm;

// idle --(enemy_seen)--> chase --(low_hp)--> flee --(after 2s)--> dead(terminal)
FsmSpec make_spec() {
    FsmSpec spec;
    spec.initial = "idle";
    spec.states = {
        {"idle", "a_idle_enter", "a_idle_update", "a_idle_exit", false},
        {"chase", "a_chase_enter", "a_chase_update", "a_chase_exit", false},
        {"flee", "a_flee_enter", "a_flee_update", "a_flee_exit", false},
        {"dead", "a_dead_enter", "", "", true},
    };
    spec.transitions = {
        {"idle", "chase", "enemy_seen", "", 0.0},
        {"chase", "flee", "", "low_hp", 0.0},
        {"flee", "dead", "", "", 2.0},
    };
    return spec;
}

void test_validate_all_or_nothing() {
    FsmSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    FsmSpec bad = s;
    bad.states.clear();
    check(!bad.validate(err) && !err.empty(), "sem estados recusa");

    bad = s;
    bad.states[1].id = "idle";  // duplicado
    check(!bad.validate(err) && !err.empty(), "id duplicado recusa");

    bad = s;
    bad.initial = "nope";
    check(!bad.validate(err) && !err.empty(), "initial desconhecido recusa");

    bad = s;
    bad.transitions[0].from = "nope";
    check(!bad.validate(err) && !err.empty(), "from desconhecido recusa");

    bad = s;
    bad.transitions[0].on_condition = "x";  // agora tem 2 gatilhos
    check(!bad.validate(err) && !err.empty(), "dois gatilhos recusam");

    bad = s;
    bad.transitions[2].after_seconds = -1.0;
    check(!bad.validate(err) && !err.empty(), "after_seconds negativo recusa");

    bad = s;
    bad.transitions[2].after_seconds = 0.0;  // nenhum gatilho ativo
    check(!bad.validate(err) && !err.empty(), "sem gatilho recusa");
}

void test_spec_json_roundtrip() {
    const FsmSpec spec = make_spec();
    const std::string json = spec.to_json();
    FsmSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    FsmSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "spec JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json(
              "{\"version\":2,\"initial\":\"idle\",\"states\":[{\"id\":\"idle\"}]}",
              err) &&
              !err.empty(),
          "versão desconhecida recusa");
}

void test_start_and_update() {
    auto f = create_fsm();
    std::string err;
    check(f->configure(make_spec(), err), "configure");
    check(f->start(err), "start");
    check(f->state() == "idle", "estado inicial = idle");

    auto actions = f->drain_actions();
    check(actions.size() == 1 && actions[0] == "a_idle_enter",
          "start emite enter_action do inicial");

    check(f->tick(0.5, err), "tick");
    actions = f->drain_actions();
    check(actions.size() == 1 && actions[0] == "a_idle_update",
          "tick emite update_action");
}

void test_condition_transition() {
    auto f = create_fsm();
    std::string err;
    check(f->configure(make_spec(), err), "configure");
    check(f->start(err), "start");
    f->drain_actions();

    // evento leva idle → chase
    check(f->send_event("enemy_seen", err), "send_event enemy_seen");
    auto actions = f->drain_actions();
    check(f->state() == "chase", "evento transita idle → chase");
    check(actions.size() == 2 && actions[0] == "a_idle_exit" &&
              actions[1] == "a_chase_enter",
          "transição emite exit(velho) → enter(novo) em ordem");

    // condição falsa → fica
    f->set_condition("low_hp", false);
    check(f->tick(0.1, err), "tick cond false");
    check(f->state() == "chase", "condição falsa não transita");
    f->drain_actions();  // descarta o update do estado que ficou

    // condição verdadeira → chase → flee
    f->set_condition("low_hp", true);
    check(f->tick(0.1, err), "tick cond true");
    check(f->state() == "flee", "condição verdadeira transita chase → flee");
    actions = f->drain_actions();
    check(actions.size() == 3 && actions[0] == "a_chase_exit" &&
              actions[1] == "a_flee_enter" && actions[2] == "a_flee_update",
          "transição + update do novo estado em ordem");
}

void test_timer_transition() {
    auto f = create_fsm();
    std::string err;
    check(f->configure(make_spec(), err), "configure");
    check(f->start(err), "start");
    f->drain_actions();
    check(f->send_event("enemy_seen", err), "evento p/ chase");
    f->set_condition("low_hp", true);
    check(f->tick(0.1, err), "tick p/ flee");
    f->drain_actions();
    check(f->state() == "flee", "estado flee");

    check(f->tick(1.5, err), "tick 1.5s");
    check(f->state() == "flee", "1.5s < 2s ainda em flee");
    check(f->time_in_state() == 1.5, "time_in_state acumula");
    f->drain_actions();  // descarta o update de flee

    check(f->tick(0.6, err), "tick 0.6s (>= 2.1s total)");
    check(f->state() == "dead", "timer 2s transita flee → dead");
    check(f->done(), "estado terminal → done()");
    check(f->time_in_state() == 0.0, "time_in_state reseta na transição");

    auto actions = f->drain_actions();
    check(actions.size() == 2 && actions[0] == "a_flee_exit" &&
              actions[1] == "a_dead_enter",
          "terminal emite exit/enter");
}

void test_terminal_no_further_transition() {
    auto f = create_fsm();
    std::string err;
    check(f->configure(make_spec(), err), "configure");
    check(f->start(err), "start");
    f->drain_actions();
    check(f->send_event("enemy_seen", err), "evento");
    f->set_condition("low_hp", true);
    check(f->tick(0.1, err), "tick");
    check(f->tick(2.0, err), "tick 2s → dead");
    f->drain_actions();
    check(f->done(), "done");

    // dead não tem transições de saída; event não faz nada
    check(f->send_event("enemy_seen", err), "event em terminal é no-op");
    check(f->state() == "dead", "estado permanece dead");
    check(f->drain_actions().empty(), "nenhuma action em terminal");
}

void test_transition_order_first_match_wins() {
    FsmSpec spec;
    spec.initial = "s0";
    spec.states = {{"s0", "", "", "", false}, {"s1", "", "", "", false},
                   {"s2", "", "", "", false}};
    spec.transitions = {
        {"s0", "s1", "", "both", 0.0},
        {"s0", "s2", "", "both", 0.0},
    };
    auto f = create_fsm();
    std::string err;
    check(f->configure(spec, err), "configure");
    check(f->start(err), "start");
    f->set_condition("both", true);
    check(f->tick(0.1, err), "tick");
    check(f->state() == "s1", "primeira transição que casa vence (ordem de declaração)");
}

void test_state_roundtrip() {
    auto f = create_fsm();
    std::string err;
    check(f->configure(make_spec(), err), "configure");
    check(f->start(err), "start");
    f->drain_actions();
    check(f->send_event("enemy_seen", err), "evento");
    check(f->tick(0.5, err), "tick");
    f->drain_actions();

    const std::string state = f->serialize_state();
    auto g = create_fsm();
    check(g->configure(make_spec(), err), "configure g");
    check(g->deserialize_state(state, err) && err.empty(), "deserialize state");
    check(g->serialize_state() == state, "state round-trip bit-exact");
    check(g->state() == "chase" && g->time_in_state() == 0.5,
          "estado/timer restaurados");

    auto h = create_fsm();
    check(h->configure(make_spec(), err), "configure h");
    check(!h->deserialize_state("{\"current\":\"nope\",\"time_in_state\":0}", err) &&
              !err.empty(),
          "estado desconhecido recusa");
}

void test_determinism() {
    auto a = create_fsm();
    auto b = create_fsm();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    check(a->start(err), "start a");
    check(b->start(err), "start b");
    a->drain_actions();
    b->drain_actions();

    std::vector<std::string> seq_a, seq_b;
    a->send_event("enemy_seen", err);
    b->send_event("enemy_seen", err);
    a->set_condition("low_hp", true);
    b->set_condition("low_hp", true);
    for (int i = 0; i < 12; ++i) {
        a->tick(0.5, err);
        b->tick(0.5, err);
    }
    const auto da = a->drain_actions();
    const auto db = b->drain_actions();
    (void)seq_a;
    (void)seq_b;

    check(da == db, "determinismo: streams de actions idênticos");
    check(a->state() == b->state() && a->time_in_state() == b->time_in_state(),
          "determinismo: estado/timer idênticos");
    check(a->serialize_state() == b->serialize_state(),
          "determinismo: estado serializado idêntico");
}

}  // namespace

int main() {
    test_validate_all_or_nothing();
    test_spec_json_roundtrip();
    test_start_and_update();
    test_condition_transition();
    test_timer_transition();
    test_terminal_no_further_transition();
    test_transition_order_first_match_wins();
    test_state_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "fsm_tests: all checks passed\n";
    } else {
        std::cout << "fsm_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}

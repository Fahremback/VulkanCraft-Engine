// UtilityAiTests — gate do contrato público de utility AI (agente 4 §3 item 3).
// Prova que a seleção por utilidade é pura, determinística, all-or-nothing no
// load, bit-exact no round-trip JSON, e que curvas/pesos/remap/seleção se
// comportam como documentado.

#include "engine/ai/IUtilityAi.hpp"

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

using engine::ai::UtilityAction;
using engine::ai::UtilityConsideration;
using engine::ai::UtilityCurve;
using engine::ai::UtilitySpec;
using engine::ai::create_utility_ai;

// attack: enemy_near (inverse, min 0 max 10) ×2 + hp (linear) ×1
// flee: hp (inverse) ×1
UtilitySpec make_spec() {
    UtilitySpec spec;
    UtilityAction attack;
    attack.id = "attack";
    attack.considerations = {
        {"enemy_near", UtilityCurve::Inverse, 2.0, 0.0, 10.0, 0.5},
        {"hp", UtilityCurve::Linear, 1.0, 0.0, 1.0, 0.5},
    };
    UtilityAction flee;
    flee.id = "flee";
    flee.considerations = {
        {"hp", UtilityCurve::Inverse, 1.0, 0.0, 1.0, 0.5},
    };
    spec.actions = {attack, flee};
    return spec;
}

void test_validate_all_or_nothing() {
    UtilitySpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    UtilitySpec bad = s;
    bad.actions.clear();
    check(!bad.validate(err) && !err.empty(), "sem ações recusa");

    bad = s;
    bad.actions[1].id = "attack";  // duplicado
    check(!bad.validate(err) && !err.empty(), "id duplicado recusa");

    bad = s;
    bad.actions[0].considerations[0].max = 0.0;  // max <= min
    check(!bad.validate(err) && !err.empty(), "max <= min recusa");

    bad = s;
    bad.actions[0].considerations[0].weight = -1.0;
    check(!bad.validate(err) && !err.empty(), "weight negativo recusa");

    bad = s;
    bad.actions[0].considerations[0].threshold = 1.5;
    check(!bad.validate(err) && !err.empty(), "threshold fora de [0,1] recusa");
}

void test_spec_json_roundtrip() {
    const UtilitySpec spec = make_spec();
    const std::string json = spec.to_json();
    UtilitySpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    UtilitySpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");

    check(!loaded.load_from_json(
              "{\"version\":9,\"actions\":[{\"id\":\"a\",\"considerations\":[]}]}",
              err) &&
              !err.empty(),
          "versão desconhecida recusa");
    check(!loaded.load_from_json(
              "{\"version\":1,\"actions\":[{\"id\":\"a\",\"considerations\":["
              "{\"input\":\"x\",\"curve\":\"quadratic\"}]}]}",
              err) &&
              !err.empty(),
          "curva desconhecida recusa");
}

void test_curves_and_remap() {
    UtilitySpec spec;
    UtilityAction a;
    a.id = "a";
    a.considerations = {
        {"lin", UtilityCurve::Linear, 1.0, 0.0, 1.0, 0.5},
        {"inv", UtilityCurve::Inverse, 1.0, 0.0, 1.0, 0.5},
        {"step", UtilityCurve::Step, 1.0, 0.0, 1.0, 0.5},
        {"remap", UtilityCurve::Linear, 1.0, 0.0, 20.0, 0.5},
    };
    spec.actions = {a};
    auto u = create_utility_ai();
    std::string err;
    check(u->configure(spec, err), "configure");

    u->set_input("lin", 0.8);
    u->set_input("inv", 0.8);
    u->set_input("step", 0.4);   // < 0.5 → 0
    u->set_input("remap", 10.0); // 10/20 → 0.5
    // utilidade = (0.8 + 0.2 + 0.0 + 0.5) / 4 = 0.375
    check(u->score("a") == 0.375, "curvas linear/inverse/step + remap min/max");

    u->set_input("step", 0.6);   // >= 0.5 → 1
    // (0.8 + 0.2 + 1.0 + 0.5) / 4 = 0.625
    check(u->score("a") == 0.625, "step cruza o threshold");
}

void test_weights_and_selection() {
    auto u = create_utility_ai();
    std::string err;
    check(u->configure(make_spec(), err), "configure");

    // enemy distante (enemy_near 9 → inverse ≈ 0.1), hp cheio (1)
    // attack = (2·0.1 + 1·1.0)/3 = 0.4 ; flee = (1·0.0)/1 = 0.0 → attack
    u->set_input("enemy_near", 9.0);
    u->set_input("hp", 1.0);
    auto sel = u->select();
    check(sel.id == "attack", "seleciona a ação de maior utilidade");

    // hp baixo (0.1): attack = (2·0.9 + 1·0.1)/3 = 0.633 ; flee = (1·0.9)/1 = 0.9
    // o peso do inverse em enemy_near puxa attack, mas flee domina com hp baixo
    u->set_input("hp", 0.1);
    sel = u->select();
    check(sel.id == "flee", "peso/curva inversa levam a fugir com hp baixo");
    check(u->score("flee") > u->score("attack"), "flee > attack com hp baixo");
}

void test_tie_breaks_declaration_order() {
    UtilitySpec spec;
    spec.actions = {
        {"first", {{"x", UtilityCurve::Linear, 1.0, 0.0, 1.0, 0.5}}},
        {"second", {{"x", UtilityCurve::Linear, 1.0, 0.0, 1.0, 0.5}}},
    };
    auto u = create_utility_ai();
    std::string err;
    check(u->configure(spec, err), "configure");
    u->set_input("x", 0.5);
    check(u->select().id == "first", "empate → primeira na ordem de declaração");
}

void test_utilities_sorted_and_unknown() {
    auto u = create_utility_ai();
    std::string err;
    check(u->configure(make_spec(), err), "configure");
    u->set_input("enemy_near", 9.0);
    u->set_input("hp", 1.0);

    check(u->score("nope") == 0.0, "score de id desconhecido = 0");

    const auto scores = u->utilities();
    check(scores.size() == 2, "utilities lista todas as ações");
    check(scores[0].utility >= scores[1].utility, "utilities ordenadas desc");
}

void test_determinism() {
    auto a = create_utility_ai();
    auto b = create_utility_ai();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");

    const double inputs[][2] = {{0.0, 1.0}, {0.3, 0.7}, {1.0, 0.0}, {5.0, 0.5}};
    for (const auto& in : inputs) {
        a->set_input("enemy_near", in[0]);
        a->set_input("hp", in[1]);
        b->set_input("enemy_near", in[0]);
        b->set_input("hp", in[1]);
        const auto sa = a->select();
        const auto sb = b->select();
        check(sa.id == sb.id && sa.utility == sb.utility,
              "determinismo: seleção idêntica");
        check(a->utilities() == b->utilities(), "determinismo: utilidades bit-exatas");
    }
}

}  // namespace

int main() {
    test_validate_all_or_nothing();
    test_spec_json_roundtrip();
    test_curves_and_remap();
    test_weights_and_selection();
    test_tie_breaks_declaration_order();
    test_utilities_sorted_and_unknown();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "utility_ai_tests: all checks passed\n";
    } else {
        std::cout << "utility_ai_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}

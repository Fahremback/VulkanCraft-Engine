// PlannerTests — gate do contrato público de planejamento GOAP (agente 4 §3
// item 3). Prova que o planejador é determinístico, all-or-nothing no load,
// bit-exact no round-trip JSON, e que preconditions/effects/custo/limite de
// plano se comportam como documentado.

#include "engine/ai/IPlanner.hpp"

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

using engine::ai::PlannerAction;
using engine::ai::PlannerSpec;
using engine::ai::create_planner;

// has_wood precisa de has_axe; has_axe precisa de wood_near.
// collect_wood: pre has_axe, effect has_wood (cost 1)
// craft_axe: pre wood_near, effect has_axe (cost 2)
// gather: effect wood_near (cost 3) — "andar até a madeira"
PlannerSpec make_spec() {
    PlannerSpec spec;
    spec.actions = {
        {"collect_wood", 1.0, {{"has_axe", true}}, {{"has_wood", true}}},
        {"craft_axe", 2.0, {{"wood_near", true}}, {{"has_axe", true}}},
        {"gather", 3.0, {}, {{"wood_near", true}}},
    };
    return spec;
}

void test_validate_all_or_nothing() {
    PlannerSpec s = make_spec();
    std::string err;
    check(s.validate(err) && err.empty(), "spec válida aceita");

    PlannerSpec bad = s;
    bad.actions.clear();
    check(!bad.validate(err) && !err.empty(), "sem ações recusa");

    bad = s;
    bad.actions[1].id = "collect_wood";
    check(!bad.validate(err) && !err.empty(), "id duplicado recusa");

    bad = s;
    bad.actions[0].cost = 0.0;
    check(!bad.validate(err) && !err.empty(), "cost <= 0 recusa");

    bad = s;
    bad.max_plan_length = 0;
    check(!bad.validate(err) && !err.empty(), "max_plan_length < 1 recusa");
}

void test_spec_json_roundtrip() {
    const PlannerSpec spec = make_spec();
    const std::string json = spec.to_json();
    PlannerSpec loaded;
    std::string err;
    check(loaded.load_from_json(json, err) && err.empty(), "spec round-trip carrega");
    check(loaded.to_json() == json, "spec round-trip bit-exact");

    PlannerSpec keep = loaded;
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "JSON inválido recusa");
    check(loaded.to_json() == keep.to_json(), "recusa não muta (all-or-nothing)");
    check(!loaded.load_from_json(
              "{\"version\":3,\"actions\":[{\"id\":\"a\"}]}", err) &&
              !err.empty(),
          "versão desconhecida recusa");
    check(!loaded.load_from_json(
              "{\"version\":1,\"actions\":[{\"id\":\"a\",\"effects\":"
              "{\"x\":1}}]}", err) &&
              !err.empty(),
          "effect não-bool recusa");
}

void test_plan_with_chain() {
    auto p = create_planner();
    std::string err;
    check(p->configure(make_spec(), err), "configure");
    // estado vazio → objetivo has_wood exige gather → craft_axe → collect_wood
    p->set_goal("has_wood", true);
    auto plan = p->plan(err);
    check(plan.success, "plano existe");
    check(plan.actions.size() == 3 && plan.actions[0] == "gather" &&
              plan.actions[1] == "craft_axe" && plan.actions[2] == "collect_wood",
          "plano em ordem: gather → craft_axe → collect_wood");
    check(plan.total_cost == 6.0, "custo total = 1+2+3");
}

void test_plan_uses_existing_state() {
    auto p = create_planner();
    std::string err;
    check(p->configure(make_spec(), err), "configure");
    p->set_atom("wood_near", true);
    p->set_goal("has_wood", true);
    auto plan = p->plan(err);
    check(plan.success && plan.actions.size() == 2 &&
              plan.actions[0] == "craft_axe" && plan.actions[1] == "collect_wood",
          "estado existente encurta o plano (pula gather)");
    check(plan.total_cost == 3.0, "custo = 1+2");
}

void test_goal_already_satisfied() {
    auto p = create_planner();
    std::string err;
    check(p->configure(make_spec(), err), "configure");
    p->set_atom("has_wood", true);
    p->set_goal("has_wood", true);
    auto plan = p->plan(err);
    check(plan.success && plan.actions.empty() && plan.total_cost == 0.0,
          "objetivo já satisfeito → plano vazio");
}

void test_impossible_goal() {
    auto p = create_planner();
    std::string err;
    check(p->configure(make_spec(), err), "configure");
    p->set_goal("has_dragon", true);
    auto plan = p->plan(err);
    check(!plan.success, "objetivo impossível → sem plano");
}

void test_goal_false_requires_absence() {
    PlannerSpec spec;
    spec.actions = {
        {"remove_fire", 1.0, {}, {{"on_fire", false}}},
    };
    auto p = create_planner();
    std::string err;
    check(p->configure(spec, err), "configure");
    p->set_atom("on_fire", true);
    p->set_goal("on_fire", false);  // quer AUSENTE
    auto plan = p->plan(err);
    check(plan.success && plan.actions.size() == 1 &&
              plan.actions[0] == "remove_fire",
          "goal false exige a ausência do átomo");
}

void test_no_repeat_and_limit() {
    // única ação com custo 1 e efeito que não muda nada de útil → plano
    // limitado por max_plan_length (sem repetição, termina).
    PlannerSpec spec;
    spec.max_plan_length = 4;
    spec.actions = {
        {"spin", 1.0, {}, {{"a", true}}},
    };
    auto p = create_planner();
    std::string err;
    check(p->configure(spec, err), "configure");
    p->set_goal("b", true);  // inalcançável
    auto plan = p->plan(err);
    check(!plan.success, "sem repetição + limite → falha limpa (termina)");
}

void test_cost_tie_breaks_declaration_order() {
    // duas ações de custo 1 que alcançam o objetivo → primeira na declaração.
    PlannerSpec spec;
    spec.actions = {
        {"first", 1.0, {}, {{"done", true}}},
        {"second", 1.0, {}, {{"done", true}}},
    };
    auto p = create_planner();
    std::string err;
    check(p->configure(spec, err), "configure");
    p->set_goal("done", true);
    auto plan = p->plan(err);
    check(plan.success && plan.actions.size() == 1 && plan.actions[0] == "first",
          "empate de custo → primeira na ordem de declaração");
}

void test_determinism() {
    auto a = create_planner();
    auto b = create_planner();
    std::string err;
    check(a->configure(make_spec(), err), "configure a");
    check(b->configure(make_spec(), err), "configure b");
    a->set_atom("wood_near", true);
    b->set_atom("wood_near", true);
    a->set_goal("has_wood", true);
    b->set_goal("has_wood", true);

    const auto pa = a->plan(err);
    const auto pb = b->plan(err);
    check(pa.success == pb.success && pa.actions == pb.actions &&
              pa.total_cost == pb.total_cost,
          "determinismo: planos idênticos cross-instance");
}

}  // namespace

int main() {
    test_validate_all_or_nothing();
    test_spec_json_roundtrip();
    test_plan_with_chain();
    test_plan_uses_existing_state();
    test_goal_already_satisfied();
    test_impossible_goal();
    test_goal_false_requires_absence();
    test_no_repeat_and_limit();
    test_cost_tie_breaks_declaration_order();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "planner_tests: all checks passed\n";
    } else {
        std::cout << "planner_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}

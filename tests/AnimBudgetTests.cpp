// AnimBudgetTests — gate do contrato público de orçamento de atualização de
// animação (agente 4 §4 item 49, componente "atualização por budget"). Prova
// a seleção determinística dentro do orçamento (sem item parcial), a
// FAIRNESS anti-starvation (pulados ganham boost e alternam nos frames
// seguintes), os erros all-or-nothing e o round-trip JSON.

#include "engine/animation/IAnimBudget.hpp"

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

using engine::animation::AnimUpdateEntry;
using engine::animation::BudgetFrame;
using engine::animation::IAnimBudget;
using engine::animation::create_anim_budget;

void test_select_basic() {
    auto b = create_anim_budget();
    std::string err;
    check(b->configure(5.0, 1.0, err) && err.empty(), "configure 5ms/1.0");

    // A(3) + B(2) = 5 ≤ 5 → ambos; C(3) estoura → pulado.
    const BudgetFrame f = b->select(
        {{"A", 1.0, 3.0}, {"B", 0.5, 2.0}, {"C", 0.4, 3.0}}, err);
    check(err.empty(), "select sem erro");
    check(f.selected.size() == 2 && f.selected[0] == "A" &&
              f.selected[1] == "B" && f.skipped.size() == 1 &&
              f.skipped[0] == "C",
          "seleciona A+B (5ms), pula C");
    check(approx(f.used_ms, 5.0, 1e-12), "used_ms = 5");

    // Sem item parcial: A(3) cabe; B(3) estouraria (3+3=6 > 5) → pulado.
    const BudgetFrame f2 = b->select({{"A", 1.0, 3.0}, {"B", 0.9, 3.0}}, err);
    check(err.empty() && f2.selected.size() == 1 && f2.selected[0] == "A" &&
              f2.skipped.size() == 1 && f2.skipped[0] == "B",
          "nunca excede o orçamento (B estoura)");

    // Custo 0 entra sempre.
    const BudgetFrame f3 = b->select(
        {{"A", 1.0, 3.0}, {"Z", 0.1, 0.0}, {"Y", 0.2, 0.0}}, err);
    check(err.empty() && f3.selected.size() == 3,
          "custo 0 sempre selecionado");
}

void test_fairness() {
    auto b = create_anim_budget();
    std::string err;
    check(b->configure(5.0, 1.0, err) && err.empty(), "configure");

    // Frame 1: A(imp 1, cost 3) > B(imp 0.9, cost 3) → A; B pulado + boost.
    const BudgetFrame f1 = b->select({{"A", 1.0, 3.0}, {"B", 0.9, 3.0}}, err);
    check(err.empty() && f1.selected.size() == 1 && f1.selected[0] == "A" &&
              f1.skipped.size() == 1,
          "frame 1: A selecionado, B pulado");
    check(approx(b->effective_priority("B"), 0.9 + 1.0, 1e-12),
          "B ganhou boost (0.9 + 1.0)");

    // Frame 2: B agora tem prioridade 1.9 > A 1.0 → B; A pulado + boost.
    const BudgetFrame f2 = b->select({{"A", 1.0, 3.0}, {"B", 0.9, 3.0}}, err);
    check(err.empty() && f2.selected.size() == 1 && f2.selected[0] == "B",
          "frame 2: B selecionado (fairness)");
    check(approx(b->effective_priority("A"), 1.0 + 1.0, 1e-12),
          "A ganhou boost (1.0 + 1.0)");
    check(approx(b->effective_priority("B"), 0.9, 1e-12),
          "B zerou o owed ao ser selecionado");

    // Frame 3: A(2.0) > B(0.9) → A de novo; alternância sem starvation.
    const BudgetFrame f3 = b->select({{"A", 1.0, 3.0}, {"B", 0.9, 3.0}}, err);
    check(err.empty() && f3.selected[0] == "A",
          "frame 3: A volta (alternância)");

    // reset zera o fairness.
    b->reset();
    check(approx(b->effective_priority("B"), 0.0, 1e-12),
          "reset zera o owed");
}

void test_errors() {
    auto b = create_anim_budget();
    std::string err;
    check(!b->configure(0.0, 1.0, err) && !err.empty(),
          "budget 0 → erro");
    err.clear();
    check(!b->configure(5.0, -1.0, err) && !err.empty(),
          "boost negativo → erro");
    err.clear();

    check(b->configure(5.0, 1.0, err) && err.empty(), "configure ok");
    check(!b->select({{"", 1.0, 1.0}}, err).used_ms &&
              err.find("empty") != std::string::npos,
          "id vazio → erro");
    check(!b->select({{"A", 1.0, 1.0}, {"A", 1.0, 1.0}}, err).used_ms &&
              err.find("duplicate") != std::string::npos,
          "id duplicado → erro");
    check(!b->select({{"A", 1.0, -1.0}}, err).used_ms &&
              err.find("cost") != std::string::npos,
          "custo negativo → erro");
}

void test_state() {
    auto b = create_anim_budget();
    std::string err;
    check(b->configure(5.0, 1.0, err) && err.empty(), "configure");
    (void)b->select({{"A", 1.0, 3.0}, {"B", 0.9, 3.0}}, err);
    check(err.empty(), "select");

    const std::string s1 = b->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto b2 = create_anim_budget();
    check(b2->deserialize_state(s1, err) && err.empty(), "deserialize ok");
    check(b2->serialize_state() == s1, "round-trip bit-exact");
    check(approx(b2->effective_priority("B"), 1.9, 1e-12),
          "boost restaurado (0.9 + 1.0)");

    const BudgetFrame f = b2->select({{"A", 1.0, 3.0}, {"B", 0.9, 3.0}}, err);
    check(err.empty() && f.selected.size() == 1 && f.selected[0] == "B",
          "seleção pós-restore reproduz o frame 2");

    const std::string s2 = b2->serialize_state();  // estado pós-select
    check(!b2->deserialize_state(
              "{\"budget_ms\":0,\"boost\":1,\"owed\":{},\"importance\":{}}",
              err),
          "restore com budget 0 rejeitado");
    check(b2->serialize_state() == s2, "estado intacto após falha");
}

}  // namespace

int main() {
    test_select_basic();
    test_fairness();
    test_errors();
    test_state();

    if (g_failures == 0) {
        std::cout << "anim_budget_tests: all checks passed\n";
        return 0;
    }
    std::cout << "anim_budget_tests: " << g_failures << " failure(s)\n";
    return 1;
}

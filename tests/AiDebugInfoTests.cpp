// AiDebugInfoTests — gate do contrato IAiDebugInfo (§3 item 40, snapshot de
// IA CORE): prova gravação por tick (nós na ordem de visita), blackboard
// (último valor por chave, ordenado), nó duplicado recusado, JSON e
// determinismo cross-instance.

#include "engine/ai/IAiDebugInfo.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

void test_record() {
    auto recorder = engine::ai::create_ai_debug_recorder();
    recorder->begin_tick(7, "hunt_tree", 42);
    check(recorder->node_visit("root", "running", 0, "selector"), "node root");
    check(recorder->node_visit("move", "succeeded", 1, "speed=3"), "node move");
    check(!recorder->node_visit("root", "running", 0, "dup"), "nó duplicado recusa");
    check(recorder->blackboard_set("target", "hero") &&
              recorder->blackboard_set("hp", "12") &&
              recorder->blackboard_set("target", "hero2"),
          "blackboard set x3 (target substituído)");

    const engine::ai::AiDebugSnapshot* snap = recorder->snapshot();
    check(snap != nullptr && snap->agentId == 7 && snap->treeName == "hunt_tree" &&
              snap->tick == 42,
          "snapshot: agent/tree/tick");
    check(snap->nodes.size() == 2 && snap->nodes[0].id == "root" &&
              snap->nodes[1].id == "move" && snap->nodes[1].depth == 1,
          "nós na ordem de visita com profundidade");
    check(snap->blackboard.size() == 2 && snap->blackboard[0].key == "hp" &&
              snap->blackboard[1].key == "target" &&
              snap->blackboard[1].value == "hero2",
          "blackboard ordenado por chave, último valor");
}

void test_tick_reset_and_json() {
    auto recorder = engine::ai::create_ai_debug_recorder();
    recorder->begin_tick(1, "tree_a", 10);
    recorder->node_visit("n1", "succeeded", 0, "x");
    recorder->blackboard_set("k", "v1");

    const std::string first = recorder->to_json();
    check(first.find("\"agentId\":1") != std::string::npos &&
              first.find("\"key\":\"k\"") != std::string::npos,
          "JSON contém agent e blackboard");

    // Novo tick zera o snapshot corrente.
    recorder->begin_tick(2, "tree_b", 11);
    const engine::ai::AiDebugSnapshot* snap = recorder->snapshot();
    check(snap->agentId == 2 && snap->nodes.empty() && snap->blackboard.empty(),
          "novo tick zera nós/blackboard");
    check(recorder->to_json().find("\"treeName\":\"tree_b\"") != std::string::npos,
          "JSON do novo tick");

    recorder->clear();
    check(recorder->snapshot()->agentId == 0 && recorder->snapshot()->nodes.empty(),
          "clear zera tudo");
    // Após clear, node_visit sem begin_tick recusa.
    check(!recorder->node_visit("a", "running", 0, ""), "node_visit sem tick recusa");
}

void test_determinism() {
    auto a = engine::ai::create_ai_debug_recorder();
    auto b = engine::ai::create_ai_debug_recorder();
    a->begin_tick(5, "t", 1);
    b->begin_tick(5, "t", 1);
    // Ordem de blackboard DIFERENTE em b — JSON deve ser igual (sorted).
    a->blackboard_set("z", "1");
    a->blackboard_set("a", "2");
    b->blackboard_set("a", "2");
    b->blackboard_set("z", "1");
    a->node_visit("n1", "succeeded", 0, "d");
    b->node_visit("n1", "succeeded", 0, "d");
    check(a->to_json() == b->to_json(),
          "JSON determinístico (ordem de blackboard irrelevante)");
}

}  // namespace

int main() {
    test_record();
    test_tick_reset_and_json();
    test_determinism();

    if (failures == 0) {
        std::printf("ai_debug_info_tests: all checks passed\n");
        return 0;
    }
    std::printf("ai_debug_info_tests: %d failure(s)\n", failures);
    return 1;
}

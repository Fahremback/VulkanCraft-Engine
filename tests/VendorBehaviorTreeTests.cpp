// VendorBehaviorTreeTests — gate do contrato IVendorBehaviorTree (§8
// behavior-tree-cpp, DEPENDENCY_POLICY): prova o runtime REAL do clone
// vendido (external/solutions/behavior-tree-cpp) atrás da superfície pública
// — árvores XML data-driven (Sequence/Fallback/SetBlackboard), nós
// customizados por callback (ação/condição), blackboard, tick RUNNING,
// recusas all-or-nothing e determinismo.

#include "engine/ai/IVendorBehaviorTree.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

const char* kRootStart = "<root BTCPP_format=\"4\">";

void test_builtin_types() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    check(tree != nullptr, "create vendor behavior tree");
    const std::vector<std::string> names = tree->builtin_node_names();
    check(!names.empty(), "builtin node types não-vazio");
    bool hasSequence = false, hasFallback = false, hasRetry = false;
    for (const std::string& name : names) {
        if (name == "Sequence") hasSequence = true;
        if (name == "Fallback") hasFallback = true;
        if (name == "RetryUntilSuccessful") hasRetry = true;
    }
    check(hasSequence && hasFallback && hasRetry,
          "builtins incluem Sequence/Fallback/RetryUntilSuccessful");
}

void test_sequence_success() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    int calls = 0;
    check(tree->register_action(
              "DoWork", [&calls](const std::string&) {
                  ++calls;
                  return engine::ai::VendorNodeStatus::SUCCESS;
              },
              error),
          "registrar ação DoWork");
    const std::string xml = std::string(kRootStart) +
                            "<BehaviorTree ID=\"main\">"
                            "  <Sequence>"
                            "    <DoWork/>"
                            "    <AlwaysSuccess/>"
                            "  </Sequence>"
                            "</BehaviorTree></root>";
    check(tree->load_from_xml(xml, error), "carregar Sequence com ação");
    check(tree->tick(error) == engine::ai::VendorNodeStatus::SUCCESS,
          "tick da Sequence → SUCCESS");
    check(calls == 1, "ação executada uma vez");
}

void test_fallback() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    const std::string xml = std::string(kRootStart) +
                            "<BehaviorTree ID=\"main\">"
                            "  <Fallback>"
                            "    <AlwaysFailure/>"
                            "    <AlwaysSuccess/>"
                            "  </Fallback>"
                            "</BehaviorTree></root>";
    check(tree->load_from_xml(xml, error), "carregar Fallback");
    check(tree->tick(error) == engine::ai::VendorNodeStatus::SUCCESS,
          "Fallback com 1º filho falho → SUCCESS");
}

void test_running_then_success() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    int calls = 0;
    check(tree->register_action(
              "LongWork", [&calls](const std::string&) {
                  ++calls;
                  return calls == 1 ? engine::ai::VendorNodeStatus::RUNNING
                                    : engine::ai::VendorNodeStatus::SUCCESS;
              },
              error),
          "registrar ação LongWork");
    const std::string xml = std::string(kRootStart) +
                            "<BehaviorTree ID=\"main\">"
                            "  <Sequence><LongWork/><AlwaysSuccess/></Sequence>"
                            "</BehaviorTree></root>";
    check(tree->load_from_xml(xml, error), "carregar árvore com LongWork");
    const engine::ai::VendorNodeStatus first = tree->tick(error);
    const engine::ai::VendorNodeStatus second = tree->tick(error);
    check(first == engine::ai::VendorNodeStatus::RUNNING,
          "1º tick → RUNNING");
    check(second == engine::ai::VendorNodeStatus::SUCCESS,
          "2º tick → SUCCESS");
    check(calls == 2, "callback chamado uma vez por tick (2 ticks)");
}

void test_blackboard() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    check(tree->set_blackboard("word", "bom dia", error),
          "set_blackboard word");
    const std::string xml = std::string(kRootStart) +
                            "<BehaviorTree ID=\"main\">"
                            "  <Sequence>"
                            "    <SetBlackboard value=\"{word}\" output_key=\"copied\"/>"
                            "  </Sequence>"
                            "</BehaviorTree></root>";
    check(tree->load_from_xml(xml, error), "carregar árvore com SetBlackboard");
    check(tree->tick(error) == engine::ai::VendorNodeStatus::SUCCESS,
          "tick com SetBlackboard → SUCCESS");
    std::string copied;
    const bool got = tree->get_blackboard("copied", copied);
    check(got && copied == "bom dia",
          "blackboard copiou {word} → 'bom dia'");
}

void test_condition() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    bool flag = false;
    check(tree->register_condition(
              "HasFlag", [&flag](const std::string&) { return flag; }, error),
          "registrar condição HasFlag");

    const std::string xml = std::string(kRootStart) +
                            "<BehaviorTree ID=\"main\">"
                            "  <Fallback>"
                            "    <HasFlag/>"
                            "    <AlwaysSuccess/>"
                            "  </Fallback>"
                            "</BehaviorTree></root>";
    check(tree->load_from_xml(xml, error), "carregar árvore com condição");
    check(tree->tick(error) == engine::ai::VendorNodeStatus::SUCCESS,
          "condição falsa + fallback → SUCCESS");
}

void test_determinism() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);
    const std::string xml = std::string(kRootStart) +
                            "<BehaviorTree ID=\"main\">"
                            "  <Sequence>"
                            "    <AlwaysSuccess/><AlwaysSuccess/>"
                            "  </Sequence>"
                            "</BehaviorTree></root>";
    check(tree->load_from_xml(xml, error), "carregar árvore determinística");
    check(tree->tick(error) == engine::ai::VendorNodeStatus::SUCCESS &&
              tree->tick(error) == engine::ai::VendorNodeStatus::SUCCESS,
          "tick repetido determinístico → SUCCESS, SUCCESS");
}

void test_rejections() {
    std::string error;
    auto tree = engine::ai::create_vendor_behavior_tree(error);

    // Nó de tipo desconhecido.
    const std::string badXml = std::string(kRootStart) +
                               "<BehaviorTree ID=\"main\">"
                               "  <NoSuchNode/>"
                               "</BehaviorTree></root>";
    check(!tree->load_from_xml(badXml, error),
          "tipo de nó desconhecido recusa");

    // XML malformado.
    check(!tree->load_from_xml("<root BTCPP_format=\"4\">"
                               "<BehaviorTree ID=\"main\"><Sequence>"
                               "</BehaviorTree>",
                               error),
          "XML malformado recusa");
    check(!tree->load_from_xml("", error), "XML vazio recusa");

    // Registro duplicado.
    check(tree->register_action(
              "Dupe", [](const std::string&) {
                  return engine::ai::VendorNodeStatus::SUCCESS;
              },
              error),
          "registrar Dupe");
    check(!tree->register_action(
              "Dupe", [](const std::string&) {
                  return engine::ai::VendorNodeStatus::SUCCESS;
              },
              error),
          "Dupe duplicado recusa");
    check(!tree->register_action("", [](const std::string&) {
              return engine::ai::VendorNodeStatus::SUCCESS;
          }, error),
          "nome vazio recusa");

    // Tick sem árvore carregada.
    auto empty = engine::ai::create_vendor_behavior_tree(error);
    check(empty->tick(error) == engine::ai::VendorNodeStatus::FAILURE,
          "tick sem árvore → FAILURE");
}

}  // namespace

int main() {
    test_builtin_types();
    test_sequence_success();
    test_fallback();
    test_running_then_success();
    test_blackboard();
    test_condition();
    test_determinism();
    test_rejections();

    if (failures == 0) {
        std::printf("vendor_behavior_tree_tests: all checks passed\n");
        return 0;
    }
    std::printf("vendor_behavior_tree_tests: %d failure(s)\n", failures);
    return 1;
}

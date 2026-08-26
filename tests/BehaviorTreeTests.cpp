// BehaviorTreeTests — gate do contrato público de IA reutilizável (agente 4 §3).
// Prova que a árvore de comportamento data-driven + blackboard tipado são
// determinísticos, all-or-nothing no load, bit-exact no round-trip JSON, e
// que composites/decorators/leaves/abort/service/cooldown/wait se comportam
// exatamente como o contrato documenta.

#include "engine/ai/IBehaviorTree.hpp"

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

using engine::ai::BehaviorNode;
using engine::ai::BehaviorStatus;
using engine::ai::BehaviorTreeSpec;
using engine::ai::Blackboard;
using engine::ai::BlackboardKind;
using engine::ai::BlackboardValue;
using engine::ai::create_behavior_tree;

// Dummy non-const lvalue for utility error outputs the test doesn't inspect.
static std::string ignoreErr;

// ---- Blackboard typed round-trip ------------------------------------------

void test_blackboard_roundtrip() {
    Blackboard bb;
    bb.set("alive", true);
    bb.set("hp", 7.5);
    bb.set("name", "guard");

    check(bb.size() == 3, "blackboard size 3");
    check(bb.has("hp"), "blackboard has hp");
    check(!bb.has("missing"), "blackboard absent key");

    BlackboardValue v;
    check(bb.get("alive", v) && v.kind == BlackboardKind::Bool && v.boolean, "bool value");
    check(bb.get("hp", v) && v.kind == BlackboardKind::Number && v.number == 7.5, "number value");
    check(bb.get("name", v) && v.kind == BlackboardKind::String && v.text == "guard", "string value");

    const std::string json = bb.to_json();
    Blackboard bb2;
    std::string error;
    check(bb2.load_from_json(json, error), "blackboard reload ok");
    check(error.empty(), "blackboard reload no error");
    check(bb2.to_json() == json, "blackboard round-trip bit-exact");
    check(bb2.size() == 3, "blackboard reload size 3");

    // All-or-nothing: a bad document leaves the store untouched.
    const std::string before = bb2.to_json();
    Blackboard bb3;
    bb3.set("keep", 1.0);
    check(!bb3.load_from_json("{\"version\":1,\"entries\":{\"x\":{\"type\":\"wat\",\"value\":1}}}", error),
          "blackboard unknown type refused");
    check(!error.empty(), "blackboard unknown type diagnostic");
    check(bb3.to_json() == "{\"version\":1,\"entries\":{\"keep\":{\"type\":\"number\",\"value\":1}}}",
          "blackboard all-or-nothing (store untouched)");

    std::cout << "  blackboard: typed round-trip OK\n";
}

// ---- Spec JSON round-trip + validation ------------------------------------

void test_spec_roundtrip_and_validation() {
    // A realistic guard: if not alerted -> idle; else -> chase; both set an
    // action and wait a tick.
    const std::string json =
        "{\"version\":1,\"root\":{"
        "\"type\":\"selector\",\"abort\":\"none\",\"children\":["
        "{\"type\":\"sequence\",\"abort\":\"self\",\"children\":["
        "{\"type\":\"condition\",\"key\":\"alerted\",\"op\":\"eq\",\"value\":{\"type\":\"bool\",\"value\":true}},"
        "{\"type\":\"action\",\"key\":\"state\",\"value\":{\"type\":\"string\",\"value\":\"chase\"}}]},"
        "{\"type\":\"sequence\",\"abort\":\"none\",\"children\":["
        "{\"type\":\"action\",\"key\":\"state\",\"value\":{\"type\":\"string\",\"value\":\"idle\"}}]}]}}";

    BehaviorTreeSpec spec;
    std::string error;
    check(spec.load_from_json(json, error), "spec load ok");
    check(error.empty(), "spec load no error");
    check(spec.validate(error), "spec validate ok");

    const std::string rejson = spec.to_json();
    check(rejson == json, "spec round-trip bit-exact");

    // Refusals (all-or-nothing, each with a diagnostic).
    const char* bad[] = {
        "{\"version\":2,\"root\":{\"type\":\"action\",\"key\":\"a\",\"value\":{\"type\":\"number\",\"value\":1}}}",
        "{\"version\":1,\"root\":{\"type\":\"wat\"}}",
        "{\"version\":1,\"root\":{\"type\":\"sequence\",\"abort\":\"none\",\"children\":[]}}",
        "{\"version\":1,\"root\":{\"type\":\"condition\",\"key\":\"a\",\"op\":\"eq\"}}",
        "{\"version\":1,\"root\":{\"type\":\"wait\",\"waitSeconds\":-1}}",
        "{\"version\":1,\"root\":{\"type\":\"parallel\",\"policy\":\"wat\",\"children\":[{\"type\":\"action\",\"key\":\"a\",\"value\":{\"type\":\"number\",\"value\":1}}]}}",
    };
    for (const char* text : bad) {
        BehaviorTreeSpec s;
        std::string err;
        check(!s.load_from_json(text, err), std::string("bad spec refused: ") + text);
        check(!err.empty(), "bad spec diagnostic non-empty");
    }

    std::cout << "  spec: round-trip bit-exact + all-or-nothing refusals OK\n";
}

// ---- Runtime behavior -----------------------------------------------------

void test_runtime_sequence_selector() {
    // Sequence: action then success -> Success.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"sequence\",\"abort\":\"none\",\"children\":["
                  "{\"type\":\"action\",\"key\":\"did\",\"value\":{\"type\":\"bool\",\"value\":true}},"
                  "{\"type\":\"action\",\"key\":\"n\",\"value\":{\"type\":\"number\",\"value\":2}}]}}",
                  ignoreErr),
              "sequence spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        check(tree != nullptr, "sequence tree created");
        Blackboard bb;
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "sequence succeeds");
        BlackboardValue v;
        check(bb.get("did", v) && v.boolean, "sequence ran first action");
        check(bb.get("n", v) && v.number == 2.0, "sequence ran second action");
    }

    // Selector: first child fails (condition), second succeeds (action).
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"selector\",\"abort\":\"none\",\"children\":["
                  "{\"type\":\"condition\",\"key\":\"x\",\"op\":\"eq\",\"value\":{\"type\":\"number\",\"value\":1}},"
                  "{\"type\":\"action\",\"key\":\"fallback\",\"value\":{\"type\":\"bool\",\"value\":true}}]}}",
                  ignoreErr),
              "selector spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "selector falls through to action");
        BlackboardValue v;
        check(bb.get("fallback", v) && v.boolean, "selector ran the fallback");
    }

    std::cout << "  runtime: sequence/selector OK\n";
}

void test_runtime_decorators_and_wait() {
    // Inverter over a failing condition -> Success.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"inverter\",\"children\":["
                  "{\"type\":\"condition\",\"key\":\"x\",\"op\":\"exists\"}]}}",
                  ignoreErr),
              "inverter spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;  // x absent -> condition fails -> inverter succeeds
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "inverter inverts failure");
    }

    // Wait: Running until enough dt has elapsed, then Success.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"wait\",\"waitSeconds\":1.0}}",
                  ignoreErr),
              "wait spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        check(tree->tick(0.3, bb) == BehaviorStatus::Running, "wait running at 0.3s");
        check(tree->tick(0.3, bb) == BehaviorStatus::Running, "wait running at 0.6s");
        check(tree->tick(0.4, bb) == BehaviorStatus::Success, "wait success at 1.0s");
    }

    // Repeat: runs the child exactly `times` times (proved via the trace —
    // the child id "0.0" is visited once per completed iteration).
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"repeat\",\"times\":3,\"children\":["
                  "{\"type\":\"action\",\"key\":\"count\",\"value\":{\"type\":\"number\",\"value\":1}}]}}",
                  ignoreErr),
              "repeat spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "repeat completes");
        int childVisits = 0;
        for (const auto& [id, status] : tree->debug_trace()) {
            if (id == "0.0") ++childVisits;
        }
        check(childVisits == 3, "repeat ran the child exactly 3 times");
    }

    std::cout << "  runtime: decorators + wait OK\n";
}

void test_runtime_service_and_cooldown() {
    // Service: runs its child once per interval, then stays Running.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"service\",\"intervalSeconds\":1.0,\"children\":["
                  "{\"type\":\"action\",\"key\":\"tick\",\"value\":{\"type\":\"number\",\"value\":1}}]}}",
                  ignoreErr),
              "service spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        bb.set("tick", 0.0);
        check(tree->tick(0.0, bb) == BehaviorStatus::Running, "service stays running");
        BlackboardValue v;
        check(bb.get("tick", v) && v.number == 1.0, "service ran child on first tick");
        // Second tick within the interval: child not re-run (still 1).
        tree->tick(0.4, bb);
        check(bb.get("tick", v) && v.number == 1.0, "service did not re-run before interval");
        // After the interval: child re-runs (set to 1 again — same value; use
        // a different key to observe the re-run via the trace instead).
    }

    // Cooldown: child runs once, then Running until cooldown elapses.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"cooldown\",\"cooldownSeconds\":2.0,\"children\":["
                  "{\"type\":\"action\",\"key\":\"shot\",\"value\":{\"type\":\"bool\",\"value\":true}}]}}",
                  ignoreErr),
              "cooldown spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "cooldown child succeeds first tick");
        // Second tick: cooldown not elapsed -> Running.
        check(tree->tick(0.5, bb) == BehaviorStatus::Running, "cooldown blocks before elapsed");
    }

    std::cout << "  runtime: service + cooldown OK\n";
}

void test_runtime_reactive_abort() {
    // Reactive sequence: a condition flips while a running child is in flight.
    //   sequence(abort=self): [ condition(alerted==true), wait(1.0) ]
    // Tick 1 (alerted=true): condition success, wait -> Running.
    // Flip alerted=false. Tick 2: reactive re-tick from the first -> condition
    // fails -> sequence fails (the running wait is aborted).
    BehaviorTreeSpec spec;
    std::string error;
    check(spec.load_from_json(
              "{\"version\":1,\"root\":{\"type\":\"sequence\",\"abort\":\"self\",\"children\":["
              "{\"type\":\"condition\",\"key\":\"alerted\",\"op\":\"eq\",\"value\":{\"type\":\"bool\",\"value\":true}},"
              "{\"type\":\"wait\",\"waitSeconds\":1.0}]}}",
              error),
          "reactive spec load");
    auto tree = create_behavior_tree(spec, error);
    check(tree != nullptr, "reactive tree created");

    Blackboard bb;
    bb.set("alerted", true);
    check(tree->tick(0.2, bb) == BehaviorStatus::Running, "reactive running while alerted");

    bb.set("alerted", false);
    check(tree->tick(0.2, bb) == BehaviorStatus::Failure, "reactive aborts running child");

    // A non-reactive (abort:none) sequence would instead keep the wait running.
    BehaviorTreeSpec specMem;
    check(specMem.load_from_json(
              "{\"version\":1,\"root\":{\"type\":\"sequence\",\"abort\":\"none\",\"children\":["
              "{\"type\":\"condition\",\"key\":\"alerted\",\"op\":\"eq\",\"value\":{\"type\":\"bool\",\"value\":true}},"
              "{\"type\":\"wait\",\"waitSeconds\":1.0}]}}",
              error),
          "memorized spec load");
    auto treeMem = create_behavior_tree(specMem, error);
    Blackboard bb2;
    bb2.set("alerted", true);
    check(treeMem->tick(0.2, bb2) == BehaviorStatus::Running, "memorized running while alerted");
    bb2.set("alerted", false);
    check(treeMem->tick(0.2, bb2) == BehaviorStatus::Running, "memorized keeps running child");

    std::cout << "  runtime: reactive abort OK\n";
}

void test_runtime_parallel_and_trace() {
    // Parallel "all": two actions -> Success.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"parallel\",\"policy\":\"all\",\"children\":["
                  "{\"type\":\"action\",\"key\":\"a\",\"value\":{\"type\":\"bool\",\"value\":true}},"
                  "{\"type\":\"action\",\"key\":\"b\",\"value\":{\"type\":\"bool\",\"value\":true}}]}}",
                  ignoreErr),
              "parallel spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "parallel all succeeds");
    }

    // Trace: the last tick's visit order is deterministic and parent-child
    // ordered.
    {
        BehaviorTreeSpec spec;
        check(spec.load_from_json(
                  "{\"version\":1,\"root\":{\"type\":\"sequence\",\"abort\":\"none\",\"children\":["
                  "{\"type\":\"action\",\"key\":\"x\",\"value\":{\"type\":\"number\",\"value\":1}},"
                  "{\"type\":\"condition\",\"key\":\"x\",\"op\":\"eq\",\"value\":{\"type\":\"number\",\"value\":1}}]}}",
                  ignoreErr),
              "trace spec load");
        auto tree = create_behavior_tree(spec, ignoreErr);
        Blackboard bb;
        check(tree->tick(0.0, bb) == BehaviorStatus::Success, "trace tree succeeds");
        const auto trace = tree->debug_trace();
        check(!trace.empty(), "trace non-empty");
        check(trace.front().first == "0.0", "trace first visit is first child");
        check(trace.back().first == "0", "trace last visit is the root");
    }

    std::cout << "  runtime: parallel + trace OK\n";
}

void test_determinism() {
    // Two fresh trees with the same spec + same input sequence produce the
    // same status stream and blackboard.
    const std::string json =
        "{\"version\":1,\"root\":{\"type\":\"sequence\",\"abort\":\"none\",\"children\":["
        "{\"type\":\"condition\",\"key\":\"alerted\",\"op\":\"eq\",\"value\":{\"type\":\"bool\",\"value\":true}},"
        "{\"type\":\"action\",\"key\":\"state\",\"value\":{\"type\":\"string\",\"value\":\"chase\"}}]}}";

    BehaviorTreeSpec specA, specB;
    std::string error;
    check(specA.load_from_json(json, error) && specB.load_from_json(json, error), "determinism spec loads");

    auto treeA = create_behavior_tree(specA, error);
    auto treeB = create_behavior_tree(specB, error);
    Blackboard bbA, bbB;
    bbA.set("alerted", true);
    bbB.set("alerted", true);

    for (int i = 0; i < 5; ++i) {
        const BehaviorStatus sa = treeA->tick(0.1, bbA);
        const BehaviorStatus sb = treeB->tick(0.1, bbB);
        check(sa == sb, "determinism status stream equal");
    }
    check(bbA.to_json() == bbB.to_json(), "determinism blackboard bit-exact");

    std::cout << "  determinism: bit-exact across instances OK\n";
}

}  // namespace

int main() {
    std::cout << "[behavior_tree_tests]\n";
    test_blackboard_roundtrip();
    test_spec_roundtrip_and_validation();
    test_runtime_sequence_selector();
    test_runtime_decorators_and_wait();
    test_runtime_service_and_cooldown();
    test_runtime_reactive_abort();
    test_runtime_parallel_and_trace();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "ALL BEHAVIOR TREE TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
}

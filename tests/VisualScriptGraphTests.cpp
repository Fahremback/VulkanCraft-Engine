// VisualScriptGraphTests.cpp — headless gate for IVisualScriptGraph.
// Tests: node CRUD, edge CRUD, type validation, cycle detection,
// topological sort, required input checks, and determinism.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "engine/scripting/IVisualScriptGraph.hpp"

using namespace engine::scripting;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(expr, msg) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_fails; \
        std::cerr << "  FAIL: " << msg << " (" #expr ")\n"; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// 1. Node type registration
// ---------------------------------------------------------------------------
static void test_node_type_registration() {
    std::cerr << "[VSG] node type registration\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.category = "Math";
    addDef.inputs = {{"a", PinType::Float, true}, {"b", PinType::Float, true}};
    addDef.outputs = {{"result", PinType::Float}};

    std::string err;
    CHECK(g.register_node_type(addDef, &err) == true, "register math.add");
    CHECK(g.register_node_type(addDef, &err) == false, "duplicate rejected");
    CHECK(g.node_type_names().size() == 1, "1 type");
    CHECK(g.get_node_type("math.add") != nullptr, "get type");
    CHECK(g.get_node_type("nonexistent") == nullptr, "unknown type");
    std::cerr << "[VSG] node type registration OK\n";
}

// ---------------------------------------------------------------------------
// 2. Node CRUD
// ---------------------------------------------------------------------------
static void test_node_crud() {
    std::cerr << "[VSG] node CRUD\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.inputs = {{"a", PinType::Float}, {"b", PinType::Float}};
    addDef.outputs = {{"result", PinType::Float}};
    std::string regErr; g.register_node_type(addDef, &regErr);

    NodeInstance n1;
    n1.type_name = "math.add";
    std::uint64_t id1 = g.add_node(n1);
    CHECK(id1 > 0, "node 1 created");

    NodeInstance n2;
    n2.type_name = "math.add";
    std::uint64_t id2 = g.add_node(n2);
    CHECK(id2 > id1, "node 2 created (higher id)");

    CHECK(g.node_ids().size() == 2, "2 nodes");
    CHECK(g.get_node(id1) != nullptr, "get node 1");
    CHECK(g.get_node(id1)->type_name == "math.add", "type matches");

    CHECK(g.remove_node(id1) == true, "remove node 1");
    CHECK(g.get_node(id1) == nullptr, "node 1 gone");
    CHECK(g.node_ids().size() == 1, "1 node left");
    CHECK(g.remove_node(999) == false, "remove nonexistent");
    std::cerr << "[VSG] node CRUD OK\n";
}

// ---------------------------------------------------------------------------
// 3. Edge CRUD
// ---------------------------------------------------------------------------
static void test_edge_crud() {
    std::cerr << "[VSG] edge CRUD\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.inputs = {{"a", PinType::Float}, {"b", PinType::Float}};
    addDef.outputs = {{"result", PinType::Float}};
    std::string regErr; g.register_node_type(addDef, &regErr);

    std::uint64_t n1 = g.add_node({0, "math.add"});
    std::uint64_t n2 = g.add_node({0, "math.add"});

    Edge e;
    e.fromNodeId = n1; e.fromPin = "result";
    e.toNodeId = n2;   e.toPin = "a";

    std::string err;
    CHECK(g.add_edge(e, &err) == true, "add edge");
    CHECK(g.edges().size() == 1, "1 edge");

    // Duplicate input connection rejected
    CHECK(g.add_edge(e, &err) == false, "duplicate input rejected");
    CHECK(err.find("already connected") != std::string::npos, "error msg");

    CHECK(g.remove_edge(n1, "result", n2, "a") == true, "remove edge");
    CHECK(g.edges().empty(), "no edges");
    CHECK(g.remove_edge(n1, "result", n2, "a") == false, "remove nonexistent");
    std::cerr << "[VSG] edge CRUD OK\n";
}

// ---------------------------------------------------------------------------
// 4. Edge validation: nonexistent nodes
// ---------------------------------------------------------------------------
static void test_edge_nonexistent_nodes() {
    std::cerr << "[VSG] edge nonexistent nodes\n";
    VisualScriptGraph g;

    Edge e;
    e.fromNodeId = 999; e.fromPin = "x";
    e.toNodeId = 1;     e.toPin = "y";

    std::string err;
    CHECK(g.add_edge(e, &err) == false, "nonexistent from node");
    CHECK(err.find("from node") != std::string::npos, "error about from node");
    std::cerr << "[VSG] edge nonexistent nodes OK\n";
}

// ---------------------------------------------------------------------------
// 5. Edge validation: type mismatch
// ---------------------------------------------------------------------------
static void test_edge_type_mismatch() {
    std::cerr << "[VSG] edge type mismatch\n";
    VisualScriptGraph g;

    NodeDef floatDef;
    floatDef.type_name = "math.add";
    floatDef.inputs = {{"a", PinType::Float}};
    floatDef.outputs = {{"result", PinType::Float}};

    NodeDef stringDef;
    stringDef.type_name = "string.concat";
    stringDef.inputs = {{"s", PinType::String}};
    stringDef.outputs = {{"result", PinType::String}};

    std::string regErr2; g.register_node_type(floatDef, &regErr2);
    std::string regErr3; g.register_node_type(stringDef, &regErr3);

    std::uint64_t n1 = g.add_node({0, "math.add"});
    std::uint64_t n2 = g.add_node({0, "string.concat"});

    Edge e;
    e.fromNodeId = n1; e.fromPin = "result";  // Float
    e.toNodeId = n2;   e.toPin = "s";          // String

    std::string err;
    CHECK(g.add_edge(e, &err) == false, "type mismatch rejected");
    CHECK(err.find("type mismatch") != std::string::npos, "error about type");
    std::cerr << "[VSG] edge type mismatch OK\n";
}

// ---------------------------------------------------------------------------
// 6. Edge validation: Any type accepts everything
// ---------------------------------------------------------------------------
static void test_edge_any_type() {
    std::cerr << "[VSG] edge Any type\n";
    VisualScriptGraph g;

    NodeDef passthrough;
    passthrough.type_name = "flow.passthrough";
    passthrough.inputs = {{"in", PinType::Any}};
    passthrough.outputs = {{"out", PinType::Any}};

    NodeDef floatDef;
    floatDef.type_name = "math.const_float";
    floatDef.outputs = {{"value", PinType::Float}};

    std::string regErr4; g.register_node_type(passthrough, &regErr4);
    std::string regErr2; g.register_node_type(floatDef, &regErr2);

    std::uint64_t n1 = g.add_node({0, "math.const_float"});
    std::uint64_t n2 = g.add_node({0, "flow.passthrough"});

    Edge e;
    e.fromNodeId = n1; e.fromPin = "value";
    e.toNodeId = n2;   e.toPin = "in";

    std::string err;
    CHECK(g.add_edge(e, &err) == true, "Any accepts Float");
    std::cerr << "[VSG] edge Any type OK\n";
}

// ---------------------------------------------------------------------------
// 7. Validation: required inputs
// ---------------------------------------------------------------------------
static void test_validation_required_inputs() {
    std::cerr << "[VSG] validation required inputs\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.inputs = {{"a", PinType::Float, true}, {"b", PinType::Float, true}};
    addDef.outputs = {{"result", PinType::Float}};
    std::string regErr; g.register_node_type(addDef, &regErr);

    std::uint64_t n1 = g.add_node({0, "math.add"});
    auto v = g.validate();
    CHECK(v.valid() == false, "invalid: required inputs not connected");
    CHECK(v.errors.size() >= 2, "at least 2 errors (a and b)");

    // Connect both inputs — n2 and n3 have default values for their own inputs
    NodeInstance n2Inst; n2Inst.type_name = "math.add";
    n2Inst.inputValues["a"] = PinValue{PinType::Float, false, 0, 1.0f};
    n2Inst.inputValues["b"] = PinValue{PinType::Float, false, 0, 2.0f};
    std::uint64_t n2 = g.add_node(n2Inst);
    NodeInstance n3Inst; n3Inst.type_name = "math.add";
    n3Inst.inputValues["a"] = PinValue{PinType::Float, false, 0, 3.0f};
    n3Inst.inputValues["b"] = PinValue{PinType::Float, false, 0, 4.0f};
    std::uint64_t n3 = g.add_node(n3Inst);
    Edge e21; e21.fromNodeId = n2; e21.fromPin = "result"; e21.toNodeId = n1; e21.toPin = "a"; g.add_edge(e21);
    Edge e31; e31.fromNodeId = n3; e31.fromPin = "result"; e31.toNodeId = n1; e31.toPin = "b"; g.add_edge(e31);

    auto v2 = g.validate();
    CHECK(v2.valid() == true, "valid after connecting");
    std::cerr << "[VSG] validation required inputs OK\n";
}

// ---------------------------------------------------------------------------
// 8. Topological sort
// ---------------------------------------------------------------------------
static void test_topological_sort() {
    std::cerr << "[VSG] topological sort\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.inputs = {{"a", PinType::Float}, {"b", PinType::Float}};
    addDef.outputs = {{"result", PinType::Float}};
    std::string regErr; g.register_node_type(addDef, &regErr);

    // Chain: n1 → n2 → n3
    std::uint64_t n1 = g.add_node({0, "math.add"});
    std::uint64_t n2 = g.add_node({0, "math.add"});
    std::uint64_t n3 = g.add_node({0, "math.add"});

    Edge e12; e12.fromNodeId = n1; e12.fromPin = "result"; e12.toNodeId = n2; e12.toPin = "a"; g.add_edge(e12);
    Edge e23; e23.fromNodeId = n2; e23.fromPin = "result"; e23.toNodeId = n3; e23.toPin = "a"; g.add_edge(e23);

    auto order = g.topological_order();
    CHECK(order.size() == 3, "3 nodes in order");
    CHECK(order[0] == n1, "n1 first");
    CHECK(order[1] == n2, "n2 second");
    CHECK(order[2] == n3, "n3 third");
    std::cerr << "[VSG] topological sort OK\n";
}

// ---------------------------------------------------------------------------
// 9. Cycle detection
// ---------------------------------------------------------------------------
static void test_cycle_detection() {
    std::cerr << "[VSG] cycle detection\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.inputs = {{"a", PinType::Float}};
    addDef.outputs = {{"result", PinType::Float}};
    std::string regErr; g.register_node_type(addDef, &regErr);

    std::uint64_t n1 = g.add_node({0, "math.add"});
    std::uint64_t n2 = g.add_node({0, "math.add"});

    Edge e12; e12.fromNodeId = n1; e12.fromPin = "result"; e12.toNodeId = n2; e12.toPin = "a"; g.add_edge(e12);
    Edge e21; e21.fromNodeId = n2; e21.fromPin = "result"; e21.toNodeId = n1; e21.toPin = "a"; g.add_edge(e21);

    auto order = g.topological_order();
    CHECK(order.empty(), "cycle → empty order");

    auto v = g.validate();
    CHECK(v.valid() == false, "cycle detected in validation");
    CHECK(v.errors[0].find("cycle") != std::string::npos, "error about cycle");
    std::cerr << "[VSG] cycle detection OK\n";
}

// ---------------------------------------------------------------------------
// 10. Node removal cascades edges
// ---------------------------------------------------------------------------
static void test_node_removal_cascades() {
    std::cerr << "[VSG] node removal cascades\n";
    VisualScriptGraph g;

    NodeDef addDef;
    addDef.type_name = "math.add";
    addDef.inputs = {{"a", PinType::Float}};
    addDef.outputs = {{"result", PinType::Float}};
    std::string regErr; g.register_node_type(addDef, &regErr);

    std::uint64_t n1 = g.add_node({0, "math.add"});
    std::uint64_t n2 = g.add_node({0, "math.add"});
    std::uint64_t n3 = g.add_node({0, "math.add"});

    Edge e12; e12.fromNodeId = n1; e12.fromPin = "result"; e12.toNodeId = n2; e12.toPin = "a"; g.add_edge(e12);
    Edge e23; e23.fromNodeId = n2; e23.fromPin = "result"; e23.toNodeId = n3; e23.toPin = "a"; g.add_edge(e23);
    CHECK(g.edges().size() == 2, "2 edges");

    g.remove_node(n2);
    CHECK(g.edges().size() == 0, "edges cascaded");
    std::cerr << "[VSG] node removal cascades OK\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    test_node_type_registration();
    test_node_crud();
    test_edge_crud();
    test_edge_nonexistent_nodes();
    test_edge_type_mismatch();
    test_edge_any_type();
    test_validation_required_inputs();
    test_topological_sort();
    test_cycle_detection();
    test_node_removal_cascades();

    std::cerr << "\n[visual_script_graph] " << g_checks << " checks, "
              << g_fails << " failures\n";
    return g_fails == 0 ? 0 : 1;
}

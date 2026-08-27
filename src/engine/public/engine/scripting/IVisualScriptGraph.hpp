#pragma once

// IVisualScriptGraph — headless node graph for visual scripting.
//
// A visual script is a directed acyclic graph (DAG) of typed nodes connected
// by typed edges. Each node has inputs/outputs with types; edges connect
// output pins to input pins. The graph is evaluated deterministically by
// topological sort — no cycles allowed (rejected at validation time).
//
// Headless-testable: build a graph, add nodes/edges, validate, evaluate —
// no GPU, no UI, no filesystem.
//
// This is the data model for §3 lines 44-45 ("Consolidar scripting visual
// para consumir somente reflection e serviços públicos" / "Completar
// variáveis, funções, eventos, fluxo, async, coroutines, erros, breakpoints,
// watch e profiling").

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::scripting {

/// Type identifiers for pin values (subset — extensible by registration).
enum class PinType {
    Bool,
    Int,
    Float,
    String,
    Vec2,
    Vec3,
    Vec4,
    Entity,
    Asset,
    Event,
    Any,      // wildcard — accepts any type at runtime
};

/// A single value carried on a pin/connection.
struct PinValue {
    PinType type{PinType::Any};
    bool    boolVal{false};
    int     intVal{0};
    float   floatVal{0.0f};
    std::string stringVal;
    std::uint64_t entityHandle{0};
    std::uint64_t assetHandle{0};

    [[nodiscard]] bool is_compatible(PinType expected) const {
        return expected == PinType::Any || type == PinType::Any || type == expected;
    }
};

/// A pin on a node (input or output).
struct PinDef {
    std::string name;
    PinType     type{PinType::Any};
    bool        required{true};   // input: must be connected
    PinValue    defaultValue;     // input: used when unconnected
};

/// Definition of a node type (what pins it has, what it does).
struct NodeDef {
    std::string type_name;              // e.g. "math.add", "flow.if", "event.on_tick"
    std::string category;               // e.g. "Math", "Flow", "Event"
    std::string description;
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
};

/// An instance of a node in the graph.
struct NodeInstance {
    std::uint64_t id{0};               // unique within the graph
    std::string   type_name;            // references NodeDef::type_name
    std::unordered_map<std::string, PinValue> inputValues;   // overrides
    std::unordered_map<std::string, PinValue> outputValues;  // computed
};

/// A connection between two pins.
struct Edge {
    std::uint64_t fromNodeId{0};
    std::string   fromPin;
    std::uint64_t toNodeId{0};
    std::string   toPin;
};

/// Validation result for the graph.
struct GraphValidation {
    std::vector<std::string> errors;
    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
};

/// The visual script graph — a DAG of typed nodes with connections.
/// Pure data structure; evaluation is done by IVisualScriptRunner.
class IVisualScriptGraph {
public:
    virtual ~IVisualScriptGraph() = default;

    // --- Node management ---

    /// Add a node instance. Returns the assigned id (0 on failure).
    virtual std::uint64_t add_node(const NodeInstance& node) = 0;

    /// Remove a node and all its edges. Returns false if not found.
    virtual bool remove_node(std::uint64_t nodeId) = 0;

    /// Get a node by id. Returns nullptr if not found.
    [[nodiscard]] virtual const NodeInstance* get_node(std::uint64_t nodeId) const = 0;

    /// List all node ids.
    [[nodiscard]] virtual std::vector<std::uint64_t> node_ids() const = 0;

    // --- Edge management ---

    /// Add a connection. Returns false if pins don't exist or types incompatible.
    virtual bool add_edge(const Edge& edge, std::string* error = nullptr) = 0;

    /// Remove a connection.
    virtual bool remove_edge(std::uint64_t fromNodeId, const std::string& fromPin,
                             std::uint64_t toNodeId, const std::string& toPin) = 0;

    /// List all edges.
    [[nodiscard]] virtual const std::vector<Edge>& edges() const = 0;

    // --- Validation ---

    /// Validate the graph: no cycles, all required inputs connected,
    /// type compatibility, all referenced node types exist.
    [[nodiscard]] virtual GraphValidation validate() const = 0;

    // --- Topological order ---

    /// Compute evaluation order (topological sort). Returns empty on cycle.
    [[nodiscard]] virtual std::vector<std::uint64_t> topological_order() const = 0;

    // --- Node type registry ---

    /// Register a node type definition.
    virtual bool register_node_type(const NodeDef& def, std::string* error = nullptr) = 0;

    /// Get a node type definition.
    [[nodiscard]] virtual const NodeDef* get_node_type(const std::string& typeName) const = 0;

    /// List all registered node types.
    [[nodiscard]] virtual std::vector<std::string> node_type_names() const = 0;
};

/// Default implementation of IVisualScriptGraph.
class VisualScriptGraph final : public IVisualScriptGraph {
public:
    std::uint64_t add_node(const NodeInstance& node) override {
        const std::uint64_t id = nextId_++;
        NodeInstance inst = node;
        inst.id = id;
        nodes_.emplace(id, std::move(inst));
        return id;
    }

    bool remove_node(std::uint64_t nodeId) override {
        if (nodes_.erase(nodeId) == 0) return false;
        edges_.erase(
            std::remove_if(edges_.begin(), edges_.end(), [nodeId](const Edge& e) {
                return e.fromNodeId == nodeId || e.toNodeId == nodeId;
            }),
            edges_.end());
        return true;
    }

    [[nodiscard]] const NodeInstance* get_node(std::uint64_t nodeId) const override {
        auto it = nodes_.find(nodeId);
        return it != nodes_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::vector<std::uint64_t> node_ids() const override {
        std::vector<std::uint64_t> ids;
        ids.reserve(nodes_.size());
        for (const auto& [id, _] : nodes_) ids.push_back(id);
        return ids;
    }

    bool add_edge(const Edge& edge, std::string* error = nullptr) override {
        // Validate nodes exist
        if (!get_node(edge.fromNodeId)) {
            if (error) *error = "from node not found";
            return false;
        }
        if (!get_node(edge.toNodeId)) {
            if (error) *error = "to node not found";
            return false;
        }
        // Validate pin types
        const auto* fromNode = get_node(edge.fromNodeId);
        const auto* toNode = get_node(edge.toNodeId);
        PinType outType = PinType::Any;
        for (const auto& p : get_node_type_outputs(fromNode->type_name)) {
            if (p.name == edge.fromPin) { outType = p.type; break; }
        }
        PinType inType = PinType::Any;
        for (const auto& p : get_node_type_inputs(toNode->type_name)) {
            if (p.name == edge.toPin) { inType = p.type; break; }
        }
        if (outType != PinType::Any && inType != PinType::Any && outType != inType) {
            if (error) *error = "type mismatch";
            return false;
        }
        // No duplicate edges to same input
        for (const auto& e : edges_) {
            if (e.toNodeId == edge.toNodeId && e.toPin == edge.toPin) {
                if (error) *error = "input already connected";
                return false;
            }
        }
        edges_.push_back(edge);
        return true;
    }

    bool remove_edge(std::uint64_t fromNodeId, const std::string& fromPin,
                     std::uint64_t toNodeId, const std::string& toPin) override {
        auto it = std::find_if(edges_.begin(), edges_.end(), [&](const Edge& e) {
            return e.fromNodeId == fromNodeId && e.fromPin == fromPin &&
                   e.toNodeId == toNodeId && e.toPin == toPin;
        });
        if (it == edges_.end()) return false;
        edges_.erase(it);
        return true;
    }

    [[nodiscard]] const std::vector<Edge>& edges() const override { return edges_; }

    [[nodiscard]] GraphValidation validate() const override {
        GraphValidation result;
        // Check all required inputs are connected
        for (const auto& [id, node] : nodes_) {
            const auto* def = get_node_type(node.type_name);
            if (!def) {
                result.errors.push_back("unknown node type: " + node.type_name);
                continue;
            }
            for (const auto& inp : def->inputs) {
                if (inp.required) {
                    bool connected = false;
                    for (const auto& e : edges_) {
                        if (e.toNodeId == id && e.toPin == inp.name) {
                            connected = true;
                            break;
                        }
                    }
                    // Also check if default value is set
                    if (!connected && node.inputValues.count(inp.name) == 0) {
                        result.errors.push_back(
                            "node " + std::to_string(id) + ": required input '" +
                            inp.name + "' not connected and no default");
                    }
                }
            }
        }
        // Check for cycles
        auto order = topological_order();
        if (order.empty() && !nodes_.empty()) {
            result.errors.push_back("cycle detected");
        }
        return result;
    }

    [[nodiscard]] std::vector<std::uint64_t> topological_order() const override {
        // Kahn's algorithm
        std::unordered_map<std::uint64_t, int> inDegree;
        for (const auto& [id, _] : nodes_) inDegree[id] = 0;
        for (const auto& e : edges_) inDegree[e.toNodeId]++;

        std::vector<std::uint64_t> queue;
        for (const auto& [id, deg] : inDegree) {
            if (deg == 0) queue.push_back(id);
        }

        std::vector<std::uint64_t> result;
        std::size_t qi = 0;
        while (qi < queue.size()) {
            std::uint64_t n = queue[qi++];
            result.push_back(n);
            for (const auto& e : edges_) {
                if (e.fromNodeId == n) {
                    if (--inDegree[e.toNodeId] == 0) {
                        queue.push_back(e.toNodeId);
                    }
                }
            }
        }
        return result.size() == nodes_.size() ? result : std::vector<std::uint64_t>{};
    }

    bool register_node_type(const NodeDef& def, std::string* error) override {
        if (def.type_name.empty()) {
            if (error) *error = "type_name must not be empty";
            return false;
        }
        if (types_.count(def.type_name)) {
            if (error) *error = "duplicate type: " + def.type_name;
            return false;
        }
        types_.emplace(def.type_name, def);
        return true;
    }

    [[nodiscard]] const NodeDef* get_node_type(const std::string& typeName) const override {
        auto it = types_.find(typeName);
        return it != types_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::vector<std::string> node_type_names() const override {
        std::vector<std::string> names;
        for (const auto& [name, _] : types_) names.push_back(name);
        return names;
    }

private:
    [[nodiscard]] const std::vector<PinDef>& get_node_type_inputs(
            const std::string& typeName) const {
        static const std::vector<PinDef> empty;
        auto it = types_.find(typeName);
        return it != types_.end() ? it->second.inputs : empty;
    }
    [[nodiscard]] const std::vector<PinDef>& get_node_type_outputs(
            const std::string& typeName) const {
        static const std::vector<PinDef> empty;
        auto it = types_.find(typeName);
        return it != types_.end() ? it->second.outputs : empty;
    }

    std::uint64_t nextId_{1};
    std::unordered_map<std::uint64_t, NodeInstance> nodes_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, NodeDef> types_;
};

} // namespace engine::scripting

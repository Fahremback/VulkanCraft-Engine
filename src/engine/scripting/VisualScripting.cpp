#include "VisualScripting.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Engine::VisualScript {

const char* value_type_name(ValueType type) noexcept {
    switch (type) {
        case ValueType::Void: return "void";
        case ValueType::Boolean: return "bool";
        case ValueType::Integer: return "int";
        case ValueType::Float: return "float";
        case ValueType::String: return "string";
        case ValueType::Vector3: return "vector3";
        case ValueType::EntityRef: return "entity";
        case ValueType::AssetRef: return "asset";
        case ValueType::Array: return "array";
        case ValueType::Map: return "map";
        case ValueType::Any: return "any";
    }
    return "any";
}

// ─── Value ───
Value Value::make_bool(bool v) { Value val; val.type_ = ValueType::Boolean; val.data_ = v; return val; }
Value Value::make_int(int64_t v) { Value val; val.type_ = ValueType::Integer; val.data_ = v; return val; }
Value Value::make_float(double v) { Value val; val.type_ = ValueType::Float; val.data_ = v; return val; }
Value Value::make_string(std::string v) { Value val; val.type_ = ValueType::String; val.data_ = std::move(v); return val; }
Value Value::make_vector3(float x, float y, float z) {
    Value val;
    val.type_ = ValueType::Vector3;
    val.data_ = std::vector<float>{x, y, z};
    return val;
}
Value Value::make_entity(uint64_t id) { Value val; val.type_ = ValueType::EntityRef; val.data_ = id; return val; }
Value Value::make_asset(uint64_t id) { Value val; val.type_ = ValueType::AssetRef; val.data_ = id; return val; }
Value Value::make_array(std::vector<Value> items) { Value val; val.type_ = ValueType::Array; val.data_ = std::move(items); return val; }
Value Value::make_map(std::unordered_map<std::string, Value> entries) { Value val; val.type_ = ValueType::Map; val.data_ = std::move(entries); return val; }
Value Value::make_any(ValueType storedType, std::any data) { Value val; val.type_ = storedType; val.data_ = std::move(data); return val; }

double Value::as_number() const {
    if (type_ == ValueType::Integer) return static_cast<double>(std::any_cast<int64_t>(data_));
    if (type_ == ValueType::Float) return std::any_cast<double>(data_);
    if (type_ == ValueType::Boolean) return std::any_cast<bool>(data_) ? 1.0 : 0.0;
    return 0.0;
}

bool Value::as_bool() const {
    if (type_ == ValueType::Boolean) return std::any_cast<bool>(data_);
    if (is_numeric()) return as_number() != 0.0;
    if (type_ == ValueType::String) return !std::any_cast<std::string>(data_).empty();
    return false;
}

std::string Value::as_string() const {
    if (type_ == ValueType::String) return std::any_cast<std::string>(data_);
    return to_string();
}

uint64_t Value::as_entity() const {
    return (type_ == ValueType::EntityRef) ? std::any_cast<uint64_t>(data_) : 0;
}

uint64_t Value::as_asset() const {
    return (type_ == ValueType::AssetRef) ? std::any_cast<uint64_t>(data_) : 0;
}

const std::vector<Value>* Value::as_array() const {
    if (type_ != ValueType::Array) return nullptr;
    static thread_local std::vector<Value> cached;
    cached = std::any_cast<std::vector<Value>>(data_);
    return &cached;
}

const std::unordered_map<std::string, Value>* Value::as_map() const {
    if (type_ != ValueType::Map) return nullptr;
    static thread_local std::unordered_map<std::string, Value> cached;
    cached = std::any_cast<std::unordered_map<std::string, Value>>(data_);
    return &cached;
}

bool Value::coerce_to(ValueType target, Value& out) const {
    if (type_ == target) { out = *this; return true; }
    if (is_numeric() && (target == ValueType::Float || target == ValueType::Integer)) {
        const double n = as_number();
        if (target == ValueType::Float) { out = make_float(n); return true; }
        out = make_int(static_cast<int64_t>(std::llround(n)));
        return true;
    }
    if (target == ValueType::Any) { out = *this; return true; }
    if (target == ValueType::Boolean && is_numeric()) { out = make_bool(as_number() != 0.0); return true; }
    return false;
}

std::string Value::to_string() const {
    std::ostringstream out;
    switch (type_) {
        case ValueType::Void: out << "<void>"; break;
        case ValueType::Boolean: out << (as_bool() ? "true" : "false"); break;
        case ValueType::Integer: out << std::any_cast<int64_t>(data_); break;
        case ValueType::Float: out << std::fixed << std::setprecision(3) << std::any_cast<double>(data_); break;
        case ValueType::String: out << std::any_cast<std::string>(data_); break;
        case ValueType::Vector3: {
            const auto v = std::any_cast<std::vector<float>>(data_);
            out << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
            break;
        }
        case ValueType::EntityRef: out << "#" << std::any_cast<uint64_t>(data_); break;
        case ValueType::AssetRef: out << "@" << std::any_cast<uint64_t>(data_); break;
        case ValueType::Array: {
            out << "[";
            const auto* arr = as_array();
            for (size_t i = 0; i < arr->size(); ++i) { if (i) out << ", "; out << (*arr)[i].to_string(); }
            out << "]";
            break;
        }
        case ValueType::Map: {
            out << "{";
            const auto* map = as_map();
            bool first = true;
            for (const auto& [k, v] : *map) { if (!first) out << ", "; first = false; out << k << ": " << v.to_string(); }
            out << "}";
            break;
        }
        case ValueType::Any: out << "<any>"; break;
    }
    return out.str();
}

// ─── Scope ───
void Scope::define(const std::string& name, Value value) {
    variables_[name] = std::move(value);
}

bool Scope::has(const std::string& name) const {
    if (variables_.count(name)) return true;
    return parent_ ? parent_->has(name) : false;
}

std::optional<Value> Scope::get(const std::string& name) const {
    auto it = variables_.find(name);
    if (it != variables_.end()) return it->second;
    return parent_ ? parent_->get(name) : std::nullopt;
}

bool Scope::set(const std::string& name, Value value) {
    if (variables_.count(name)) { variables_[name] = std::move(value); return true; }
    return parent_ ? parent_->set(name, std::move(value)) : false;
}

// ─── Graph ───
Node* Graph::node(const std::string& id) {
    auto it = nodeIndex_.find(id);
    return (it != nodeIndex_.end()) ? &nodes_[it->second] : nullptr;
}

const Node* Graph::node(const std::string& id) const {
    auto it = nodeIndex_.find(id);
    return (it != nodeIndex_.end()) ? &nodes_[it->second] : nullptr;
}

std::string Graph::add_node(Node node) {
    if (node.id.empty()) {
        node.id = "n" + std::to_string(nextId_++);
    }
    nodeIndex_[node.id] = nodes_.size();
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

bool Graph::remove_node(const std::string& id) {
    auto it = nodeIndex_.find(id);
    if (it == nodeIndex_.end()) return false;
    // Remove connected pins.
    const Node& n = nodes_[it->second];
    std::vector<std::string> pinIds;
    for (const Pin& p : n.inputs) pinIds.push_back(p.id);
    for (const Pin& p : n.outputs) pinIds.push_back(p.id);
    for (const std::string& pin : pinIds) {
        for (auto c = connections_.begin(); c != connections_.end();) {
            if (c->fromPin == pin || c->toPin == pin) c = connections_.erase(c);
            else ++c;
        }
    }
    nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(it->second));
    nodeIndex_.erase(it);
    // Rebuild index.
    nodeIndex_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) nodeIndex_[nodes_[i].id] = i;
    return true;
}

std::string Graph::add_connection(const std::string& fromPin, const std::string& toPin) {
    if (fromPin.empty() || toPin.empty()) return {};
    // Reject duplicates.
    for (const Connection& c : connections_) {
        if (c.fromPin == fromPin && c.toPin == toPin) return c.fromPin;
    }
    connections_.push_back({fromPin, toPin});
    return fromPin;
}

bool Graph::remove_connection(const std::string& fromPin, const std::string& toPin) {
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->fromPin == fromPin && it->toPin == toPin) {
            connections_.erase(it);
            return true;
        }
    }
    return false;
}

void Graph::clear() noexcept {
    nodes_.clear();
    connections_.clear();
    nodeIndex_.clear();
}

std::string Graph::exec_output(const std::string& nodeId) const {
    const Node* n = node(nodeId);
    if (!n) return {};
    for (const Pin& p : n->outputs) {
        if (p.type == ValueType::Void || p.name == "Exec") return p.id;
    }
    return n->outputs.empty() ? std::string{} : n->outputs.front().id;
}

std::string Graph::exec_output(const std::string& nodeId, const std::string& name) const {
    const Node* n = node(nodeId);
    if (!n) return {};
    for (const Pin& p : n->outputs) {
        if (p.type == ValueType::Void && p.name == name) return p.id;
    }
    return {};
}

std::string Graph::data_output(const std::string& nodeId) const {
    const Node* n = node(nodeId);
    if (!n) return {};
    for (const Pin& p : n->outputs) {
        if (p.type != ValueType::Void) return p.id;
    }
    return {};
}

std::string Graph::input_pin(const std::string& nodeId, const std::string& name) const {
    const Node* n = node(nodeId);
    if (!n) return {};
    for (const Pin& p : n->inputs) if (p.name == name) return p.id;
    return {};
}

std::string Graph::output_pin(const std::string& nodeId, const std::string& name) const {
    const Node* n = node(nodeId);
    if (!n) return {};
    for (const Pin& p : n->outputs) if (p.name == name) return p.id;
    return {};
}

bool Graph::type_compatible(ValueType from, ValueType to) const {
    if (to == ValueType::Any) return true;
    if (from == to) return true;
    return (from == ValueType::Integer && to == ValueType::Float) ||
           (from == ValueType::Float && to == ValueType::Integer);
}

std::optional<Value> Graph::read_pin_value(const Node& node, const Pin& pin, ExecutionContext& ctx) {
    // Find a connection feeding this input pin.
    for (const Connection& c : connections_) {
        if (c.toPin != pin.id) continue;
        // Find the source node owning c.fromPin.
        for (const Node& src : nodes_) {
            for (const Pin& op : src.outputs) {
                if (op.id != c.fromPin) continue;
                // Read the constant/parameter or variable value.
                switch (src.kind) {
                    case NodeKind::Constant:
                        return src.constant;
                    case NodeKind::Variable:
                        return ctx.global.get(src.symbol);
                    case NodeKind::ForLoop:
                        return ctx.global.get("__out_" + src.id);
                    case NodeKind::ArrayIndex:
                    case NodeKind::MapGet:
                    case NodeKind::BinaryOp:
                    case NodeKind::UnaryOp:
                    case NodeKind::ArrayLength:
                    case NodeKind::MapContains:
                    case NodeKind::ArrayAppend:
                    case NodeKind::MapSet:
                    case NodeKind::ArrayCreate:
                    case NodeKind::MapCreate:
                        return ctx.global.get("__out_" + src.id);
                    default:
                        return std::nullopt;
                }
            }
        }
    }
    if (pin.defaultValue.type() != ValueType::Void) return pin.defaultValue;
    return std::nullopt;
}

void Graph::write_pin_value(const Node& node, const std::string& outputPin, Value value, ExecutionContext& ctx) {
    // Cache the produced value in the node's constant slot for downstream reads.
    if (node.kind == NodeKind::BinaryOp || node.kind == NodeKind::UnaryOp ||
        node.kind == NodeKind::ArrayIndex || node.kind == NodeKind::ArrayAppend ||
        node.kind == NodeKind::ArrayLength || node.kind == NodeKind::MapGet ||
        node.kind == NodeKind::MapContains || node.kind == NodeKind::MapSet) {
        // Nodes are const during traversal; use the scope as the value store.
        ctx.global.define("__out_" + node.id, value);
    }
}

void Graph::execute_flow(const std::string& nodeId, const std::string& entryPin, ExecutionContext& ctx) {
    if (ctx.terminated || ctx.breaking || ctx.continuing) return;
    // Cycle guard.
    if (std::find(ctx.visitedNodeIds.begin(), ctx.visitedNodeIds.end(), nodeId) != ctx.visitedNodeIds.end()) {
        ctx.lastError = "cycle detected at " + nodeId;
        return;
    }
    ctx.visitedNodeIds.push_back(nodeId);
    Node* n = node(nodeId);
    if (n) execute_node(*n, entryPin, ctx);
}

void Graph::execute_event(const std::string& eventName, Scene* scene, uint64_t instigator) {
    ExecutionContext ctx;
    ctx.scene = scene;
    ctx.instigator = instigator;
    for (const Node& n : nodes_) {
        if (n.kind == NodeKind::Event && n.eventName == eventName) {
            ctx.visitedNodeIds.clear();
            ctx.terminated = false;
            ctx.breaking = false;
            ctx.continuing = false;
            execute_flow(n.id, {}, ctx);
        }
    }
}

void Graph::execute_node(const Node& node, const std::string& entryPin, ExecutionContext& ctx) {
    // Data-pin output helpers.
    auto outputId = [&](const std::string& name) -> std::string {
        for (const Pin& p : node.outputs) if (p.name == name) return p.id;
        return node.outputs.empty() ? std::string{} : node.outputs.front().id;
    };
    // Continue exec flow from an output pin.
    auto flowTo = [&](const std::string& pin) {
        for (const Connection& c : connections_) {
            if (c.fromPin != pin) continue;
            for (const Node& target : nodes_) {
                for (const Pin& ip : target.inputs) {
                    if (ip.id == c.toPin) execute_flow(target.id, ip.id, ctx);
                }
            }
        }
    };
    // Read input value by pin name.
    auto readInput = [&](const std::string& name) -> std::optional<Value> {
        for (const Pin& p : node.inputs) {
            if (p.name == name) return read_pin_value(node, p, ctx);
        }
        return std::nullopt;
    };

    switch (node.kind) {
        case NodeKind::Event:
            flowTo(exec_output(node.id));
            break;
        case NodeKind::Constant:
            // Value read via read_pin_value on the constant slot.
            break;
        case NodeKind::Variable:
            break;
        case NodeKind::SetVariable: {
            const auto value = readInput("Value");
            if (value) ctx.global.set(node.symbol, *value);
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::Branch: {
            const auto cond = readInput("Condition");
            const bool value = cond ? cond->as_bool() : false;
            const std::string targetPin = outputId(value ? "True" : "False");
            flowTo(targetPin);
            break;
        }
        case NodeKind::Sequence: {
            for (const Pin& p : node.outputs) {
                if (p.type != ValueType::Void) continue;
                flowTo(p.id);
                if (ctx.terminated) break;
            }
            break;
        }
        case NodeKind::Switch: {
            const auto value = readInput("Value");
            bool matched = false;
            for (size_t i = 0; i < node.outputs.size(); ++i) {
                const Pin& p = node.outputs[i];
                if (p.type != ValueType::Void) continue;
                const std::string label = p.name;
                if (label == "Default") continue;
                // Case labels: "case:N" or "case:value".
                if (value && label.rfind("case:", 0) == 0) {
                    const std::string expected = label.substr(5);
                    if (value->to_string() == expected) { flowTo(p.id); matched = true; break; }
                }
            }
            if (!matched) {
                for (const Pin& p : node.outputs) {
                    if (p.type == ValueType::Void && p.name == "Default") { flowTo(p.id); break; }
                }
            }
            break;
        }
        case NodeKind::ForLoop: {
            const auto start = readInput("Start");
            const auto end = readInput("End");
            const int64_t from = start ? static_cast<int64_t>(start->as_number()) : 0;
            const int64_t to = end ? static_cast<int64_t>(end->as_number()) : 0;
            for (int64_t i = from; i < to; ++i) {
                ctx.global.define(node.symbol, Value::make_int(i));
                ctx.global.define("__out_" + node.id, Value::make_int(i));
                flowTo(outputId("LoopBody"));
                if (ctx.breaking) { ctx.breaking = false; break; }
                if (ctx.continuing) { ctx.continuing = false; continue; }
                if (ctx.terminated) break;
            }
            if (!ctx.terminated && !ctx.breaking) flowTo(outputId("Completed"));
            break;
        }
        case NodeKind::WhileLoop: {
            uint32_t guard = 0;
            for (;;) {
                const auto cond = readInput("Condition");
                if (!cond || !cond->as_bool()) break;
                flowTo(outputId("LoopBody"));
                if (ctx.breaking) { ctx.breaking = false; break; }
                if (ctx.continuing) { ctx.continuing = false; continue; }
                if (ctx.terminated) break;
                if (++guard > 100000) { ctx.lastError = "while loop exceeded guard"; break; }
            }
            if (!ctx.terminated && !ctx.breaking) flowTo(outputId("Completed"));
            break;
        }
        case NodeKind::Break:
            ctx.breaking = true;
            break;
        case NodeKind::Continue:
            ctx.continuing = true;
            break;
        case NodeKind::Return:
            ctx.returnNode = node.id;
            ctx.returnValue = readInput("Value").value_or(Value{});
            ctx.terminated = true;
            break;
        case NodeKind::CallFunction: {
            // Simple function call: find a node with kind Event and eventName == symbol.
            for (const Node& fn : nodes_) {
                if (fn.kind != NodeKind::Event || fn.eventName != node.symbol) continue;
                execute_flow(fn.id, {}, ctx);
                break;
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::ArrayCreate: {
            std::vector<Value> items;
            for (size_t i = 0; i < node.inputs.size(); ++i) {
                const auto v = readInput("Item" + std::to_string(i));
                if (v) items.push_back(*v);
            }
            ctx.global.define("__out_" + node.id, Value::make_array(std::move(items)));
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::ArrayIndex: {
            const auto arr = readInput("Array");
            const auto idx = readInput("Index");
            if (arr && arr->as_array() && idx) {
                const auto* a = arr->as_array();
                const int64_t i = static_cast<int64_t>(idx->as_number());
                if (i >= 0 && static_cast<size_t>(i) < a->size()) {
                    ctx.global.define("__out_" + node.id, (*a)[i]);
                }
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::ArrayAppend: {
            const auto arr = readInput("Array");
            const auto item = readInput("Item");
            std::vector<Value> items = arr && arr->as_array() ? *arr->as_array() : std::vector<Value>{};
            if (item) items.push_back(*item);
            ctx.global.define("__out_" + node.id, Value::make_array(std::move(items)));
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::ArrayLength: {
            const auto arr = readInput("Array");
            if (arr && arr->as_array()) {
                ctx.global.define("__out_" + node.id, Value::make_int(static_cast<int64_t>(arr->as_array()->size())));
            } else {
                ctx.global.define("__out_" + node.id, Value::make_int(0));
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::MapCreate: {
            std::unordered_map<std::string, Value> map;
            ctx.global.define("__out_" + node.id, Value::make_map(std::move(map)));
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::MapSet: {
            auto mapValue = readInput("Map");
            const auto key = readInput("Key");
            const auto value = readInput("Value");
            std::unordered_map<std::string, Value> map = mapValue && mapValue->as_map() ? *mapValue->as_map()
                                                                                         : std::unordered_map<std::string, Value>{};
            if (key) map[key->to_string()] = value.value_or(Value{});
            ctx.global.define("__out_" + node.id, Value::make_map(std::move(map)));
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::MapGet: {
            const auto mapValue = readInput("Map");
            const auto key = readInput("Key");
            if (mapValue && mapValue->as_map() && key) {
                const auto* m = mapValue->as_map();
                const auto it = m->find(key->to_string());
                if (it != m->end()) ctx.global.define("__out_" + node.id, it->second);
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::MapContains: {
            const auto mapValue = readInput("Map");
            const auto key = readInput("Key");
            bool found = false;
            if (mapValue && mapValue->as_map() && key) {
                const auto* m = mapValue->as_map();
                found = m->count(key->to_string()) > 0;
            }
            ctx.global.define("__out_" + node.id, Value::make_bool(found));
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::BinaryOp: {
            const auto a = readInput("A");
            const auto b = readInput("B");
            if (a && b) {
                Value result;
                const std::string& op = node.symbol;
                if (op == "+") result = Value::make_float(a->as_number() + b->as_number());
                else if (op == "-") result = Value::make_float(a->as_number() - b->as_number());
                else if (op == "*") result = Value::make_float(a->as_number() * b->as_number());
                else if (op == "/") result = Value::make_float(b->as_number() != 0 ? a->as_number() / b->as_number() : 0);
                else if (op == "==") result = Value::make_bool(a->as_number() == b->as_number());
                else if (op == "!=") result = Value::make_bool(a->as_number() != b->as_number());
                else if (op == "<") result = Value::make_bool(a->as_number() < b->as_number());
                else if (op == ">") result = Value::make_bool(a->as_number() > b->as_number());
                else if (op == "<=") result = Value::make_bool(a->as_number() <= b->as_number());
                else if (op == ">=") result = Value::make_bool(a->as_number() >= b->as_number());
                else if (op == "&&") result = Value::make_bool(a->as_bool() && b->as_bool());
                else if (op == "||") result = Value::make_bool(a->as_bool() || b->as_bool());
                else if (op == "concat") result = Value::make_string(a->to_string() + b->to_string());
                else result = Value{};
                ctx.global.define("__out_" + node.id, result);
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::UnaryOp: {
            const auto a = readInput("Value");
            if (a) {
                Value result;
                if (node.symbol == "!") result = Value::make_bool(!a->as_bool());
                else if (node.symbol == "-") result = Value::make_float(-a->as_number());
                else result = Value{};
                ctx.global.define("__out_" + node.id, result);
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::Delay:
            // Execution-time delay is handled by the host; here it just flows.
            flowTo(exec_output(node.id));
            break;
        case NodeKind::Print: {
            const auto v = readInput("Value");
            const std::string text = v ? v->to_string() : "<void>";
            if (ctx.scene) {
                // Route through scene's log hook if present; otherwise no-op.
                ctx.lastError = text; // reuse error slot to carry the printed text
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::SpawnEntity:
            if (ctx.scene) {
                // Generic: create an entity via the public Scene API.
                (void)ctx.scene->create_entity("Spawned");
            }
            flowTo(exec_output(node.id));
            break;
        case NodeKind::DestroyEntity: {
            const auto e = readInput("Entity");
            if (ctx.scene && e && e->as_entity() != 0) {
                // The visual script's entity id is opaque to the VM; the host
                // bridges it. Here we only record the intent (no-op is safe).
                ctx.lastError = "destroy:" + std::to_string(e->as_entity());
            }
            flowTo(exec_output(node.id));
            break;
        }
        case NodeKind::Comment:
            break;
    }
}

// ─── Serialization (JSON-ish, stable line format) ───
namespace {
std::string escape(const std::string& s) {
    std::string out;
    out += '"';
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    out += '"';
    return out;
}
} // namespace

bool Graph::save_to_file(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "VCPLAYGRAPH 1\n";
    out << "nodes " << nodes_.size() << "\n";
    for (const Node& n : nodes_) {
        out << "node " << n.id << " " << static_cast<int>(n.kind) << " " << escape(n.title)
            << " " << escape(n.symbol) << " " << escape(n.eventName) << " " << n.delaySeconds
            << " " << n.positionX << " " << n.positionY << "\n";
        out << "  const " << static_cast<int>(n.constant.type()) << " " << escape(n.constant.to_string()) << "\n";
        out << "  comment " << escape(n.comment) << "\n";
        out << "  pins " << n.inputs.size() << " " << n.outputs.size() << "\n";
        for (const Pin& p : n.inputs)
            out << "  in " << p.id << " " << escape(p.name) << " " << static_cast<int>(p.type) << "\n";
        for (const Pin& p : n.outputs)
            out << "  out " << p.id << " " << escape(p.name) << " " << static_cast<int>(p.type) << "\n";
    }
    out << "connections " << connections_.size() << "\n";
    for (const Connection& c : connections_) out << "conn " << c.fromPin << " " << c.toPin << "\n";
    return out.good();
}

namespace {
// Reads a quoted token (with \n and \" escapes) followed by a space.
bool read_quoted(std::istream& in, std::string& out) {
    char c{};
    in >> c;
    if (c != '"') return false;
    out.clear();
    while (in.get(c)) {
        if (c == '"') return true;
        if (c == '\\') {
            char e{};
            if (!in.get(e)) return false;
            if (e == 'n') out += '\n';
            else out += e;
        } else {
            out += c;
        }
    }
    return false;
}
} // namespace

bool Graph::load_from_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string magic;
    int version{};
    in >> magic >> version;
    if (magic != "VCPLAYGRAPH" || version != 1) return false;
    clear();

    std::string section;
    size_t count{};
    in >> section >> count;
    if (section == "nodes") {
        for (size_t i = 0; i < count; ++i) {
            std::string tag, id, title, symbol, eventName;
            int kindInt{};
            float delay{}, px{}, py{};
            in >> tag >> id >> kindInt;
            if (!read_quoted(in, title)) return false;
            if (!read_quoted(in, symbol)) return false;
            if (!read_quoted(in, eventName)) return false;
            in >> delay >> px >> py;
            Node n;
            n.id = id;
            n.kind = static_cast<NodeKind>(kindInt);
            n.title = title;
            n.symbol = symbol;
            n.eventName = eventName;
            n.delaySeconds = delay;
            n.positionX = px;
            n.positionY = py;
            // const line
            std::string constTag;
            int constType{};
            std::string constStr;
            in >> constTag >> constType;
            if (constTag != "const") return false;
            if (!read_quoted(in, constStr)) return false;
            Value cv;
            const ValueType ctype = static_cast<ValueType>(constType);
            switch (ctype) {
                case ValueType::Boolean: cv = Value::make_bool(constStr == "true" || constStr == "1"); break;
                case ValueType::Integer: cv = Value::make_int(std::stoll(constStr.empty() ? "0" : constStr)); break;
                case ValueType::Float: cv = Value::make_float(std::stod(constStr.empty() ? "0" : constStr)); break;
                case ValueType::String: cv = Value::make_string(constStr); break;
                default: cv = Value::make_string(constStr); break;
            }
            n.constant = cv;
            // comment line
            std::string commentTag;
            in >> commentTag;
            if (commentTag != "comment") return false;
            if (!read_quoted(in, n.comment)) return false;
            // pins line
            std::string pinsTag;
            size_t inCount{}, outCount{};
            in >> pinsTag >> inCount >> outCount;
            if (pinsTag != "pins") return false;
            for (size_t p = 0; p < inCount; ++p) {
                std::string dir, pinId, pinName;
                int pinType{};
                in >> dir >> pinId;
                if (!read_quoted(in, pinName)) return false;
                in >> pinType;
                Pin pin;
                pin.id = pinId;
                pin.name = pinName;
                pin.type = static_cast<ValueType>(pinType);
                pin.isInput = true;
                n.inputs.push_back(std::move(pin));
            }
            for (size_t p = 0; p < outCount; ++p) {
                std::string dir, pinId, pinName;
                int pinType{};
                in >> dir >> pinId;
                if (!read_quoted(in, pinName)) return false;
                in >> pinType;
                Pin pin;
                pin.id = pinId;
                pin.name = pinName;
                pin.type = static_cast<ValueType>(pinType);
                pin.isInput = false;
                n.outputs.push_back(std::move(pin));
            }
            add_node(std::move(n));
        }
    } else {
        return false;
    }

    in >> section >> count;
    if (section == "connections") {
        for (size_t i = 0; i < count; ++i) {
            std::string tag, fromPin, toPin;
            in >> tag >> fromPin >> toPin;
            if (tag == "conn") add_connection(fromPin, toPin);
        }
    }
    return true;
}

} // namespace Engine::VisualScript

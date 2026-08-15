#pragma once

#include "../scene/Scene.hpp"

#include <any>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::VisualScript {

// ─── Typed value model ───
enum class ValueType : uint8_t {
    Void,
    Boolean,
    Integer,
    Float,
    String,
    Vector3,
    EntityRef,
    AssetRef,
    Array,
    Map,
    Any
};

[[nodiscard]] const char* value_type_name(ValueType type) noexcept;

// Runtime value: typed variant supporting the full pin type set.
class Value final {
public:
    Value() = default;
    static Value make_bool(bool v);
    static Value make_int(int64_t v);
    static Value make_float(double v);
    static Value make_string(std::string v);
    static Value make_vector3(float x, float y, float z);
    static Value make_entity(uint64_t id);
    static Value make_asset(uint64_t id);
    static Value make_array(std::vector<Value> items);
    static Value make_map(std::unordered_map<std::string, Value> entries);
    static Value make_any(ValueType storedType, std::any data);

    [[nodiscard]] ValueType type() const noexcept { return type_; }
    [[nodiscard]] bool is_numeric() const noexcept {
        return type_ == ValueType::Integer || type_ == ValueType::Float;
    }
    [[nodiscard]] double as_number() const;
    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::string as_string() const;
    [[nodiscard]] uint64_t as_entity() const;
    [[nodiscard]] uint64_t as_asset() const;
    [[nodiscard]] const std::vector<Value>* as_array() const;
    [[nodiscard]] const std::unordered_map<std::string, Value>* as_map() const;
    // Type check with implicit numeric promotion (int↔float).
    [[nodiscard]] bool coerce_to(ValueType target, Value& out) const;
    [[nodiscard]] std::string to_string() const;

private:
    ValueType type_{ValueType::Void};
    std::any data_;
};

// ─── Pins and nodes ───
struct Pin {
    std::string id;
    std::string name;
    ValueType type{ValueType::Any};
    bool isInput{true};
    Value defaultValue;
};

enum class NodeKind : uint8_t {
    Event,
    Constant,
    Variable,
    SetVariable,
    Branch,
    Sequence,
    Switch,
    ForLoop,
    WhileLoop,
    Break,
    Continue,
    CallFunction,
    Return,
    ArrayCreate,
    ArrayIndex,
    ArrayAppend,
    ArrayLength,
    MapCreate,
    MapSet,
    MapGet,
    MapContains,
    BinaryOp,
    UnaryOp,
    Delay,
    Print,
    SpawnEntity,
    DestroyEntity,
    Comment
};

struct Node {
    std::string id;
    std::string title;
    NodeKind kind{NodeKind::Comment};
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    // Parameter-like data (constants, variable names, op codes, string args).
    Value constant;
    std::string symbol;              // variable/function name, op token
    std::string eventName;           // for Event nodes
    float delaySeconds{0.0f};
    std::string comment;
    float positionX{0.0f}, positionY{0.0f}; // canvas position
};

struct Connection {
    std::string fromPin;
    std::string toPin;
};

// ─── Execution model ───
// A scope maps variable names to values; nested scopes chain to a parent.
class Scope final {
public:
    explicit Scope(Scope* parent = nullptr) : parent_(parent) {}
    void define(const std::string& name, Value value);
    [[nodiscard]] bool has(const std::string& name) const;
    [[nodiscard]] std::optional<Value> get(const std::string& name) const;
    bool set(const std::string& name, Value value);
    void clear() noexcept { variables_.clear(); }
    [[nodiscard]] Scope* parent() noexcept { return parent_; }
    [[nodiscard]] const std::unordered_map<std::string, Value>& variables() const noexcept { return variables_; }

private:
    Scope* parent_;
    std::unordered_map<std::string, Value> variables_;
};

struct ExecutionContext {
    Scene* scene{nullptr};
    uint64_t instigator{0};
    Scope global;
    std::vector<std::string> visitedNodeIds; // cycle guard
    bool terminated{false};
    bool breaking{false};
    bool continuing{false};
    std::optional<std::string> returnNode;
    Value returnValue;
    std::string lastError;
};

// ─── Graph ───
class Graph final {
public:
    [[nodiscard]] Node* node(const std::string& id);
    [[nodiscard]] const Node* node(const std::string& id) const;
    [[nodiscard]] std::string add_node(Node node);
    bool remove_node(const std::string& id);
    [[nodiscard]] std::string add_connection(const std::string& fromPin, const std::string& toPin);
    bool remove_connection(const std::string& fromPin, const std::string& toPin);
    void clear() noexcept;

    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<Connection>& connections() const noexcept { return connections_; }

    // Finds the output pin id of a node's first exec output, first data output,
    // or a named pin — helpers for wiring tools.
    [[nodiscard]] std::string exec_output(const std::string& nodeId) const;
    [[nodiscard]] std::string exec_output(const std::string& nodeId, const std::string& name) const;
    [[nodiscard]] std::string data_output(const std::string& nodeId) const;
    [[nodiscard]] std::string input_pin(const std::string& nodeId, const std::string& name) const;
    [[nodiscard]] std::string output_pin(const std::string& nodeId, const std::string& name) const;

    // Execution entry: runs every Event node matching eventName (exec flows
    // through connected pins; data pins carry Values).
    void execute_event(const std::string& eventName, Scene* scene, uint64_t instigator);

    // Serialization (JSON-like text format with version header).
    [[nodiscard]] bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);

private:
    [[nodiscard]] bool type_compatible(ValueType from, ValueType to) const;
    void execute_node(const Node& node, const std::string& entryPin, ExecutionContext& ctx);
    void execute_flow(const std::string& nodeId, const std::string& entryPin, ExecutionContext& ctx);
    [[nodiscard]] std::optional<Value> read_pin_value(const Node& node, const Pin& pin, ExecutionContext& ctx);
    void write_pin_value(const Node& node, const std::string& outputPin, Value value, ExecutionContext& ctx);

    std::vector<Node> nodes_;
    std::vector<Connection> connections_;
    std::unordered_map<std::string, size_t> nodeIndex_;
    std::unordered_map<std::string, size_t> connectionIndex_;
    uint64_t nextId_{1};
};

} // namespace Engine::VisualScript

#pragma once

#include "../core/uuid/UUID.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Engine {

enum class ScriptNodeKind {
    Event, ConstantFloat, ConstantInteger, ConstantBoolean,
    GetVariable, SetVariable, AddFloat, SubtractFloat, MultiplyFloat,
    Branch, Wait, EmitEvent, Return,
    Function, FunctionCall, Log, Scope, ScopeEnd
};

using ScriptValue = std::variant<std::monostate, bool, int64_t, double, std::string, UUID>;

struct TypedScriptNode {
    UUID id;
    ScriptNodeKind kind{ScriptNodeKind::Return};
    std::string event;
    std::string variable;
    ScriptValue literal;
};

struct ScriptNodeLink { UUID from; UUID to; };

struct ScriptGraphAsset {
    UUID id;
    std::string name{"Script Graph"};
    std::vector<TypedScriptNode> nodes;
    std::vector<ScriptNodeLink> links;
    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path);
};

enum class OpCode {
    Nop, PushFloat, PushInteger, PushBoolean, LoadVariable, StoreVariable,
    AddFloat, SubtractFloat, MultiplyFloat, JumpIfFalse, Jump, Wait,
    EmitEvent, Return, Call, Log, PushScope, PopScope
};

struct Instruction {
    OpCode opcode{OpCode::Return};
    ScriptValue operand;
    std::string text;
    size_t target{};
    UUID sourceNode;
};

struct ScriptProgram {
    std::vector<Instruction> instructions;
    std::unordered_map<std::string, size_t> eventEntries;
    uint64_t sourceHash{};
};

struct ScriptDiagnostic {
    UUID node;
    std::string message;
};

struct ScriptCompileResult {
    bool success{};
    ScriptProgram program;
    std::vector<ScriptDiagnostic> diagnostics;
    explicit operator bool() const noexcept { return success; }
};

class ScriptCompiler final {
public:
    static ScriptCompileResult compile(const ScriptGraphAsset& graph);
};

enum class VMStatus { Idle, Running, Waiting, Paused, Completed, Error };

class ScriptVM final {
public:
    void load(ScriptProgram program);
    bool start_event(const std::string& eventName);
    VMStatus run(float deltaTime, size_t instructionBudget = 10000);
    VMStatus step(float deltaTime);
    void add_breakpoint(size_t instructionIndex);
    void remove_breakpoint(size_t instructionIndex);
    void clear_breakpoints();
    void set_variable(std::string name, ScriptValue value);
    const ScriptValue* variable(const std::string& name) const;
    /// Enumerates all script variables (name → value). Used by the script
    /// debugger for variable inspection and by hot reload to snapshot and
    /// restore global state across program swaps.
    const std::unordered_map<std::string, ScriptValue>& variables() const noexcept { return variables_; }
    double float_variable(const std::string& name) const;
    VMStatus status() const noexcept { return status_; }
    size_t instruction_pointer() const noexcept { return ip_; }
    /// The compiled program (instructions + event entries). Used by the
    /// script debugger panel to list the bytecode with breakpoint markers.
    const ScriptProgram& program() const noexcept { return program_; }
    const std::string& error() const noexcept { return error_; }
    const std::vector<std::string>& emitted_events() const noexcept { return emittedEvents_; }
    bool has_event(const std::string& eventName) const {
        return program_.eventEntries.contains(eventName);
    }
    /// Drains the events the program emitted via EmitEvent into `out` (the
    /// caller dispatches them back through start_event).
    void consume_emitted_events(std::vector<std::string>& out) {
        out.clear();
        out.swap(emittedEvents_);
        emittedEvents_.clear();
    }

private:
    VMStatus execute_one(float deltaTime, bool ignoreBreakpoint);
    std::optional<double> pop_number();
    ScriptProgram program_;
    std::vector<ScriptValue> stack_;
    std::vector<size_t> callStack_;   // return addresses for OpCode::Call
    std::unordered_map<std::string, ScriptValue> variables_; // global frame
    std::vector<std::unordered_map<std::string, ScriptValue>> scopeStack_; // local frames
    std::unordered_set<size_t> breakpoints_;
    std::vector<std::string> emittedEvents_;
    size_t ip_{};
    float waitRemaining_{};
    VMStatus status_{VMStatus::Idle};
    std::string error_;
};

/// Human-readable name of an opcode, for the debugger's program listing.
const char* script_opcode_name(OpCode opcode) noexcept;
/// Human-readable name of a graph node kind (for the debugger's node view).
const char* script_node_kind_name(ScriptNodeKind kind) noexcept;

class ScriptHotReloader final {
public:
    bool watch(const std::filesystem::path& path);
    bool changed() const;
    bool reload_if_changed(ScriptVM& vm, std::string* error = nullptr);
private:
    std::filesystem::path path_;
    std::filesystem::file_time_type writeTime_{};
};

} // namespace Engine

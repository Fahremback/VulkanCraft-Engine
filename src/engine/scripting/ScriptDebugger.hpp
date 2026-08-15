#pragma once

// ---------------------------------------------------------------------------
// ScriptDebugger.hpp
//
// UI-independent debugger model for the scripting VM (engine/scripting/
// ScriptRuntime.hpp). Two integration modes:
//
//  1. Self-hosted interpreter (default)
//     load(DebugProgram) and drive the debugger directly. The debugger owns
//     a small call-aware bytecode interpreter (DebugOpCode::Call / Return)
//     so call stacks genuinely grow and shrink and step over / step out are
//     meaningful. Build a DebugProgram by hand, or convert a real compiled
//     ScriptProgram with from_script_program() and debug the real bytecode.
//
//  2. VM driver mode
//     attach(ScriptVM&) binds the debugger to a live ScriptVM. Breakpoints
//     are forwarded to the VM, stepping delegates to ScriptVM::step/run and
//     variables are read from the VM. The real VM bytecode is flat (no Call
//     opcode yet), so in this mode the call stack is a single synthetic
//     frame and step over / step out degrade to "step until next breakpoint
//     or state change" (documented in the .cpp).
//
// Watch expressions are evaluated with a small recursive-descent expression
// parser over the current variable scope (see evaluate_expression()).
// ---------------------------------------------------------------------------

#include "engine/scripting/ScriptRuntime.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine::Scripting {

using Engine::Instruction;
using Engine::OpCode;
using Engine::ScriptProgram;
using Engine::ScriptValue;
using Engine::ScriptVM;

enum class ScriptDebugState {
    Idle,     // no program loaded / no event started
    Running,  // executing
    Paused,   // stopped at a breakpoint (or paused mid-run)
    Stopped   // program completed or halted with an error
};

[[nodiscard]] const char* script_debug_state_name(ScriptDebugState state) noexcept;

// Call-aware instruction set used by the self-hosted interpreter. The real
// ScriptVM bytecode is mapped onto this set by from_script_program(); the
// VM has no Call opcode, so converted programs run as a single "main" frame.
enum class DebugOpCode {
    Nop,
    Push,          // operand → value stack
    LoadVariable,  // text (variable name) → value stack
    StoreVariable, // value stack → variables_[text]
    Add, Subtract, Multiply,
    JumpIfFalse,   // pop condition; jump to target if false
    Jump,          // unconditional jump to target
    Call,          // push frame {name, target, ip+1}; ip = target
    Return,        // pop frame; ip = frame.returnIp (Stopped if stack empty)
    Halt           // stop with the current state
};

struct DebugInstruction {
    DebugOpCode opcode{ DebugOpCode::Nop };
    ScriptValue operand;   // Push literal
    std::string text;      // variable name (Load/Store)
    size_t target{ 0 };    // jump/call target
    int line{ 0 };         // source line this instruction maps to (0 = unknown)
};

struct DebugFunction {
    std::string name;
    size_t entry{ 0 };
    size_t exit{ 0 };      // first instruction index past the function body
};

struct DebugProgram {
    std::vector<DebugInstruction> instructions;
    std::unordered_map<std::string, size_t> entries;   // event name → entry ip
    std::vector<DebugFunction> functions;
    std::unordered_map<int, size_t> lineToInstruction; // source line → ip
};

struct DebugFrame {
    std::string name;
    size_t entry{ 0 };
    size_t returnIp{ 0 };
};

struct WatchEntry {
    size_t id{ 0 };
    std::string expression;
    std::string result;    // refreshed by evaluate_watches()
};

class ScriptDebugger final {
public:
    ScriptDebugger() = default;
    ~ScriptDebugger() = default;
    ScriptDebugger(const ScriptDebugger&) = delete;
    ScriptDebugger& operator=(const ScriptDebugger&) = delete;

    // ------------------------------------------------------------------
    // Program / VM binding
    // ------------------------------------------------------------------

    /// Load a debug program for the self-hosted interpreter.
    void load(DebugProgram program);

    /// Bridge from the real scripting VM bytecode: converts every
    /// ScriptProgram::Instruction into a DebugInstruction (1:1 semantics;
    /// Wait/EmitEvent become Nop — timing/events are not simulated by the
    /// debugger). The converted program runs as a single "main" frame.
    static DebugProgram from_script_program(const ScriptProgram& program);

    /// Drive a live ScriptVM instead of the built-in interpreter.
    void attach(ScriptVM& vm);
    void detach();
    ScriptVM* vm() const noexcept { return vm_; }
    bool attached() const noexcept { return vm_ != nullptr; }

    // ------------------------------------------------------------------
    // Execution
    // ------------------------------------------------------------------

    /// Enter an event handler. In self-hosted mode pushes the initial frame
    /// and sets state to Running. Returns false if the event is unknown.
    bool start_event(const std::string& eventName);

    /// Execute exactly one instruction. If it is a Call, the callee frame is
    /// pushed (call stack grows); a Return pops back to the caller.
    ScriptDebugState step_into(float deltaTime = 0.0f);

    /// Run until the current call returns (skips whole callee if standing on
    /// a Call) or a breakpoint / state change interrupts.
    ScriptDebugState step_over(float deltaTime = 0.0f);

    /// Run until the current frame pops back to its caller.
    ScriptDebugState step_out(float deltaTime = 0.0f);

    /// Run until a breakpoint, completion, or the instruction budget runs out.
    ScriptDebugState continue_run(size_t instructionBudget = 10000, float deltaTime = 0.0f);

    /// Clear execution state (keeps program, breakpoints and variables).
    void reset();

    // ------------------------------------------------------------------
    // Breakpoints
    // ------------------------------------------------------------------

    void add_breakpoint(size_t instructionIndex);
    /// Map a source line to an instruction via program.lineToInstruction and
    /// set a breakpoint there. No-op if the line is unknown.
    void add_line_breakpoint(int line);
    void remove_breakpoint(size_t instructionIndex);
    void clear_breakpoints();
    bool has_breakpoint(size_t instructionIndex) const;
    const std::unordered_set<size_t>& breakpoints() const noexcept { return breakpoints_; }
    std::optional<size_t> breakpoint_at_line(int line) const;

    // ------------------------------------------------------------------
    // Introspection
    // ------------------------------------------------------------------

    ScriptDebugState state() const noexcept { return state_; }
    size_t instruction_pointer() const noexcept { return ip_; }
    const std::vector<DebugFrame>& call_stack() const noexcept { return frames_; }
    size_t frame_count() const noexcept { return frames_.size(); }
    const DebugProgram& program() const noexcept { return program_; }
    const std::string& error() const noexcept { return error_; }
    /// Source line of the next instruction to execute (-1 when unknown).
    int current_line() const;

    // ------------------------------------------------------------------
    // Variables
    // ------------------------------------------------------------------

    /// name → stringified value, for UI tables. In VM mode reads the VM.
    std::unordered_map<std::string, std::string> variables() const;
    std::optional<std::string> variable(const std::string& name) const;
    const ScriptValue* raw_variable(const std::string& name) const;
    void set_variable(std::string name, ScriptValue value);

    // ------------------------------------------------------------------
    // Watch expressions
    // ------------------------------------------------------------------

    size_t add_watch(std::string expression);
    bool remove_watch(size_t watchId);
    void clear_watches();
    /// Re-evaluate every watch against the current variable scope.
    void evaluate_watches();
    const std::vector<WatchEntry>& watches() const noexcept { return watches_; }

    /// Evaluate a single expression against a variable scope.
    /// Supports numbers, "strings", identifiers, + - * / %,
    /// == != < <= > >=, && || ! and parentheses.
    /// Returns the stringified result, "undefined" for unknown identifiers,
    /// or "error: <reason>".
    static std::string evaluate_expression(
        const std::string& expression,
        const std::unordered_map<std::string, ScriptValue>& scope);

    // ------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------

    using StateCallback = std::function<void(ScriptDebugState)>;
    using BreakpointCallback = std::function<void(size_t instructionIndex, int line)>;
    void set_state_callback(StateCallback callback) { stateCallback_ = std::move(callback); }
    void set_breakpoint_callback(BreakpointCallback callback) { breakpointCallback_ = std::move(callback); }

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    static std::string value_to_string(const ScriptValue& value);

private:
    ScriptDebugState execute_one(bool ignoreBreakpoints);
    void notify_state();
    void notify_breakpoint();
    void sync_from_vm();
    std::optional<double> pop_number();
    bool pop_truthy();
    std::string frame_name_for_call(size_t target) const;
    std::unordered_map<std::string, ScriptValue> current_scope() const;

    ScriptVM* vm_{ nullptr };
    DebugProgram program_;
    std::vector<ScriptValue> valueStack_;
    std::unordered_map<std::string, ScriptValue> variables_;
    std::vector<DebugFrame> frames_;
    std::unordered_set<size_t> breakpoints_;
    std::vector<WatchEntry> watches_;
    size_t nextWatchId_{ 1 };
    size_t ip_{ 0 };
    ScriptDebugState state_{ ScriptDebugState::Idle };
    std::string error_;
    std::string currentEvent_;
    ScriptDebugState lastNotifiedState_{ ScriptDebugState::Idle };
    StateCallback stateCallback_;
    BreakpointCallback breakpointCallback_;
};

} // namespace Engine::Scripting

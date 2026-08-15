#include "engine/scripting/ScriptDebugger.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace Engine::Scripting {

namespace {

// ---------------------------------------------------------------------------
// Watch-expression lexer and parser.
// ---------------------------------------------------------------------------

bool to_number(const ScriptValue& value, double& out) {
    if (const double* d = std::get_if<double>(&value)) { out = *d; return true; }
    if (const int64_t* i = std::get_if<int64_t>(&value)) { out = static_cast<double>(*i); return true; }
    if (const bool* b = std::get_if<bool>(&value)) { out = *b ? 1.0 : 0.0; return true; }
    return false;
}

bool watch_truthy(const ScriptValue& value) {
    if (const bool* b = std::get_if<bool>(&value)) return *b;
    double n = 0.0;
    if (to_number(value, n)) return n != 0.0;
    if (const std::string* s = std::get_if<std::string>(&value)) return !s->empty();
    return false;
}

enum class WatchTokKind {
    End, Number, String, Ident,
    Plus, Minus, Star, Slash, Percent,
    LParen, RParen,
    EqEq, NotEq, Less, LessEq, Greater, GreaterEq,
    AndAnd, OrOr, Not
};

struct WatchToken {
    WatchTokKind kind{ WatchTokKind::End };
    std::string text;
    double number{ 0.0 };
};

class WatchLexer {
public:
    explicit WatchLexer(const std::string& source) : source_(source) {}

    WatchToken next() {
        skip_space();
        if (pos_ >= source_.size()) return {};
        const char c = source_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && pos_ + 1 < source_.size() &&
             std::isdigit(static_cast<unsigned char>(source_[pos_ + 1])))) {
            return number();
        }
        if (c == '"') return string_literal();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return identifier();
        ++pos_;
        switch (c) {
            case '+': return { WatchTokKind::Plus, "+" };
            case '-': return { WatchTokKind::Minus, "-" };
            case '*': return { WatchTokKind::Star, "*" };
            case '/': return { WatchTokKind::Slash, "/" };
            case '%': return { WatchTokKind::Percent, "%" };
            case '(': return { WatchTokKind::LParen, "(" };
            case ')': return { WatchTokKind::RParen, ")" };
            case '=': if (match('=')) return { WatchTokKind::EqEq, "==" }; break;
            case '!': if (match('=')) return { WatchTokKind::NotEq, "!=" }; return { WatchTokKind::Not, "!" };
            case '<': if (match('=')) return { WatchTokKind::LessEq, "<=" }; return { WatchTokKind::Less, "<" };
            case '>': if (match('=')) return { WatchTokKind::GreaterEq, ">=" }; return { WatchTokKind::Greater, ">" };
            case '&': if (match('&')) return { WatchTokKind::AndAnd, "&&" }; break;
            case '|': if (match('|')) return { WatchTokKind::OrOr, "||" }; break;
            default: break;
        }
        return {};
    }

private:
    bool match(char c) {
        if (pos_ < source_.size() && source_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    void skip_space() {
        while (pos_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[pos_]))) ++pos_;
    }
    WatchToken number() {
        const size_t start = pos_;
        bool dot = false;
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) { ++pos_; continue; }
            if (c == '.' && !dot) { dot = true; ++pos_; continue; }
            break;
        }
        WatchToken t;
        t.kind = WatchTokKind::Number;
        t.text = source_.substr(start, pos_ - start);
        t.number = std::strtod(t.text.c_str(), nullptr);
        return t;
    }
    WatchToken string_literal() {
        ++pos_; // opening quote
        std::string value;
        while (pos_ < source_.size()) {
            const char c = source_[pos_++];
            if (c == '"') break;
            if (c == '\\' && pos_ < source_.size()) {
                const char escaped = source_[pos_++];
                switch (escaped) {
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    case '\\': value += '\\'; break;
                    case '"': value += '"'; break;
                    default: value += escaped; break;
                }
                continue;
            }
            value += c;
        }
        WatchToken t;
        t.kind = WatchTokKind::String;
        t.text = value;
        return t;
    }
    WatchToken identifier() {
        const size_t start = pos_;
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ++pos_; else break;
        }
        WatchToken t;
        t.kind = WatchTokKind::Ident;
        t.text = source_.substr(start, pos_ - start);
        return t;
    }

    const std::string& source_;
    size_t pos_{ 0 };
};

class WatchParser {
public:
    WatchParser(const std::string& source,
                const std::unordered_map<std::string, ScriptValue>& scope)
        : scope_(scope), lexer_(source) {
        current_ = lexer_.next();
    }

    std::optional<ScriptValue> parse() {
        ScriptValue result = parse_or();
        if (!error_.empty()) return std::nullopt;
        if (current_.kind != WatchTokKind::End) {
            fail("unexpected token '" + current_.text + "'");
            return std::nullopt;
        }
        return result;
    }

    const std::string& error() const { return error_; }

private:
    void advance() { current_ = lexer_.next(); }
    void fail(const std::string& message) { if (error_.empty()) error_ = message; }

    ScriptValue parse_or() {
        ScriptValue lhs = parse_and();
        while (error_.empty() && current_.kind == WatchTokKind::OrOr) {
            advance();
            const bool a = watch_truthy(lhs);
            const bool b = watch_truthy(parse_and());
            lhs = ScriptValue{ a || b };
        }
        return lhs;
    }

    ScriptValue parse_and() {
        ScriptValue lhs = parse_eq();
        while (error_.empty() && current_.kind == WatchTokKind::AndAnd) {
            advance();
            const bool a = watch_truthy(lhs);
            const bool b = watch_truthy(parse_eq());
            lhs = ScriptValue{ a && b };
        }
        return lhs;
    }

    ScriptValue parse_eq() {
        ScriptValue lhs = parse_rel();
        while (error_.empty() && (current_.kind == WatchTokKind::EqEq ||
                                  current_.kind == WatchTokKind::NotEq)) {
            const bool negate = current_.kind == WatchTokKind::NotEq;
            advance();
            ScriptValue rhs = parse_rel();
            bool result = false;
            if (error_.empty()) {
                double a = 0.0, b = 0.0;
                if (to_number(lhs, a) && to_number(rhs, b)) {
                    result = (a == b);
                } else if (std::holds_alternative<bool>(lhs) && std::holds_alternative<bool>(rhs)) {
                    result = (std::get<bool>(lhs) == std::get<bool>(rhs));
                } else if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
                    result = (std::get<std::string>(lhs) == std::get<std::string>(rhs));
                } else {
                    fail("incomparable operands for ==");
                    break;
                }
                lhs = ScriptValue{ negate ? !result : result };
            }
        }
        return lhs;
    }

    ScriptValue parse_rel() {
        ScriptValue lhs = parse_add();
        while (error_.empty() && (current_.kind == WatchTokKind::Less ||
                                  current_.kind == WatchTokKind::LessEq ||
                                  current_.kind == WatchTokKind::Greater ||
                                  current_.kind == WatchTokKind::GreaterEq)) {
            const WatchTokKind op = current_.kind;
            advance();
            ScriptValue rhs = parse_add();
            bool result = false;
            if (error_.empty()) {
                double a = 0.0, b = 0.0;
                if (to_number(lhs, a) && to_number(rhs, b)) {
                    switch (op) {
                        case WatchTokKind::Less: result = a < b; break;
                        case WatchTokKind::LessEq: result = a <= b; break;
                        case WatchTokKind::Greater: result = a > b; break;
                        default: result = a >= b; break;
                    }
                } else if (std::holds_alternative<std::string>(lhs) &&
                           std::holds_alternative<std::string>(rhs)) {
                    const std::string& sa = std::get<std::string>(lhs);
                    const std::string& sb = std::get<std::string>(rhs);
                    switch (op) {
                        case WatchTokKind::Less: result = sa < sb; break;
                        case WatchTokKind::LessEq: result = sa <= sb; break;
                        case WatchTokKind::Greater: result = sa > sb; break;
                        default: result = sa >= sb; break;
                    }
                } else {
                    fail("incomparable operands for relational operator");
                    break;
                }
                lhs = ScriptValue{ result };
            }
        }
        return lhs;
    }

    ScriptValue parse_add() {
        ScriptValue lhs = parse_mul();
        while (error_.empty() && (current_.kind == WatchTokKind::Plus ||
                                  current_.kind == WatchTokKind::Minus)) {
            const bool isPlus = current_.kind == WatchTokKind::Plus;
            advance();
            ScriptValue rhs = parse_mul();
            if (!error_.empty()) break;
            const bool anyString = std::holds_alternative<std::string>(lhs) ||
                                   std::holds_alternative<std::string>(rhs);
            if (isPlus && anyString) {
                lhs = ScriptValue{ ScriptDebugger::value_to_string(lhs) +
                                   ScriptDebugger::value_to_string(rhs) };
                continue;
            }
            double a = 0.0, b = 0.0;
            if (!to_number(lhs, a) || !to_number(rhs, b)) {
                fail("arithmetic on non-numeric operands");
                break;
            }
            lhs = ScriptValue{ isPlus ? (a + b) : (a - b) };
        }
        return lhs;
    }

    ScriptValue parse_mul() {
        ScriptValue lhs = parse_unary();
        while (error_.empty() && (current_.kind == WatchTokKind::Star ||
                                  current_.kind == WatchTokKind::Slash ||
                                  current_.kind == WatchTokKind::Percent)) {
            const WatchTokKind op = current_.kind;
            advance();
            ScriptValue rhs = parse_unary();
            if (!error_.empty()) break;
            double a = 0.0, b = 0.0;
            if (!to_number(lhs, a) || !to_number(rhs, b)) {
                fail("arithmetic on non-numeric operands");
                break;
            }
            switch (op) {
                case WatchTokKind::Star: lhs = ScriptValue{ a * b }; break;
                case WatchTokKind::Slash:
                    if (b == 0.0) { fail("division by zero"); return lhs; }
                    lhs = ScriptValue{ a / b };
                    break;
                default:
                    if (b == 0.0) { fail("division by zero"); return lhs; }
                    lhs = ScriptValue{ std::fmod(a, b) };
                    break;
            }
        }
        return lhs;
    }

    ScriptValue parse_unary() {
        if (current_.kind == WatchTokKind::Minus) {
            advance();
            ScriptValue v = parse_unary();
            double n = 0.0;
            if (!to_number(v, n)) { fail("unary minus on non-numeric operand"); return {}; }
            return ScriptValue{ -n };
        }
        if (current_.kind == WatchTokKind::Not) {
            advance();
            return ScriptValue{ !watch_truthy(parse_unary()) };
        }
        return parse_primary();
    }

    ScriptValue parse_primary() {
        switch (current_.kind) {
            case WatchTokKind::Number: {
                ScriptValue v{ current_.number };
                advance();
                return v;
            }
            case WatchTokKind::String: {
                ScriptValue v{ current_.text };
                advance();
                return v;
            }
            case WatchTokKind::Ident: {
                auto it = scope_.find(current_.text);
                if (it == scope_.end()) {
                    fail("undefined variable '" + current_.text + "'");
                    return {};
                }
                ScriptValue v = it->second;
                advance();
                return v;
            }
            case WatchTokKind::LParen: {
                advance();
                ScriptValue v = parse_or();
                if (current_.kind != WatchTokKind::RParen) {
                    fail("expected ')'");
                    return {};
                }
                advance();
                return v;
            }
            default:
                fail("unexpected token '" + current_.text + "'");
                return {};
        }
    }

    const std::unordered_map<std::string, ScriptValue>& scope_;
    WatchLexer lexer_;
    WatchToken current_;
    std::string error_;
};

} // namespace

// ---------------------------------------------------------------------------
// ScriptDebugger
// ---------------------------------------------------------------------------

const char* script_debug_state_name(ScriptDebugState state) noexcept {
    switch (state) {
        case ScriptDebugState::Idle: return "Idle";
        case ScriptDebugState::Running: return "Running";
        case ScriptDebugState::Paused: return "Paused";
        case ScriptDebugState::Stopped: return "Stopped";
    }
    return "Unknown";
}

std::string ScriptDebugger::value_to_string(const ScriptValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return "<void>";
    if (const bool* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (const int64_t* i = std::get_if<int64_t>(&value)) return std::to_string(*i);
    if (const double* d = std::get_if<double>(&value)) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.6g", *d);
        return buffer;
    }
    if (const std::string* s = std::get_if<std::string>(&value)) return *s;
    if (const Engine::UUID* u = std::get_if<Engine::UUID>(&value)) return u->to_string();
    return "<unknown>";
}

DebugProgram ScriptDebugger::from_script_program(const ScriptProgram& program) {
    DebugProgram out;
    out.instructions.reserve(program.instructions.size());
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        const Instruction& in = program.instructions[i];
        DebugInstruction d;
        d.operand = in.operand;
        d.text = in.text;
        d.target = in.target;
        d.line = static_cast<int>(i + 1); // synthetic 1-based source lines
        switch (in.opcode) {
            case OpCode::Nop: d.opcode = DebugOpCode::Nop; break;
            case OpCode::PushFloat:
            case OpCode::PushInteger:
            case OpCode::PushBoolean: d.opcode = DebugOpCode::Push; break;
            case OpCode::LoadVariable: d.opcode = DebugOpCode::LoadVariable; break;
            case OpCode::StoreVariable: d.opcode = DebugOpCode::StoreVariable; break;
            case OpCode::AddFloat: d.opcode = DebugOpCode::Add; break;
            case OpCode::SubtractFloat: d.opcode = DebugOpCode::Subtract; break;
            case OpCode::MultiplyFloat: d.opcode = DebugOpCode::Multiply; break;
            case OpCode::JumpIfFalse: d.opcode = DebugOpCode::JumpIfFalse; break;
            case OpCode::Jump: d.opcode = DebugOpCode::Jump; break;
            case OpCode::Wait:
            case OpCode::EmitEvent:
                // Timing and cross-script events are not simulated by the
                // debugger; stepping over them is a no-op.
                d.opcode = DebugOpCode::Nop;
                break;
            case OpCode::Return: d.opcode = DebugOpCode::Return; break;
        }
        out.instructions.push_back(d);
        out.lineToInstruction[d.line] = i;
    }
    out.entries = program.eventEntries;
    // The real VM bytecode is flat: no Call/Return pairs, so everything runs
    // inside a single synthetic "main" frame.
    out.functions.push_back({ "main", 0, program.instructions.size() });
    return out;
}

void ScriptDebugger::load(DebugProgram program) {
    program_ = std::move(program);
    reset();
}

void ScriptDebugger::attach(ScriptVM& vm) {
    vm_ = &vm;
    reset();
    sync_from_vm();
}

void ScriptDebugger::detach() {
    vm_ = nullptr;
    reset();
}

void ScriptDebugger::reset() {
    valueStack_.clear();
    frames_.clear();
    ip_ = 0;
    error_.clear();
    currentEvent_.clear();
    state_ = ScriptDebugState::Idle;
    lastNotifiedState_ = ScriptDebugState::Idle;
}

bool ScriptDebugger::start_event(const std::string& eventName) {
    if (vm_) {
        if (!vm_->start_event(eventName)) return false;
        currentEvent_ = eventName;
        sync_from_vm();
        return true;
    }
    const auto it = program_.entries.find(eventName);
    if (it == program_.entries.end()) return false;
    ip_ = it->second;
    valueStack_.clear();
    frames_.clear();
    frames_.push_back({ eventName, ip_, program_.instructions.size() });
    currentEvent_ = eventName;
    error_.clear();
    state_ = ScriptDebugState::Running;
    notify_state();
    return true;
}

ScriptDebugState ScriptDebugger::step_into(float deltaTime) {
    if (vm_) {
        if (vm_->status() != Engine::VMStatus::Paused &&
            vm_->status() != Engine::VMStatus::Running) {
            return state_;
        }
        vm_->step(deltaTime);
        ip_ = vm_->instruction_pointer();
        sync_from_vm();
        if (state_ == ScriptDebugState::Running &&
            breakpoints_.count(vm_->instruction_pointer())) {
            state_ = ScriptDebugState::Paused;
            notify_breakpoint();
            notify_state();
        }
        return state_;
    }
    if (state_ == ScriptDebugState::Idle || state_ == ScriptDebugState::Stopped) return state_;
    if (state_ == ScriptDebugState::Paused) state_ = ScriptDebugState::Running;
    execute_one(true);
    if (state_ == ScriptDebugState::Running && ip_ < program_.instructions.size() &&
        breakpoints_.count(ip_)) {
        state_ = ScriptDebugState::Paused;
        notify_breakpoint();
    }
    notify_state();
    return state_;
}

ScriptDebugState ScriptDebugger::step_over(float deltaTime) {
    if (vm_) {
        // The real VM bytecode has no call frames, so step over degrades to
        // "step until the next breakpoint or state change" (capped so a
        // long-running script cannot block the editor).
        if (vm_->status() == Engine::VMStatus::Paused) vm_->step(deltaTime);
        size_t guard = 0;
        bool hitBreakpoint = false;
        while (vm_->status() == Engine::VMStatus::Running && guard < 256) {
            vm_->step(deltaTime);
            ++guard;
            if (breakpoints_.count(vm_->instruction_pointer())) {
                hitBreakpoint = true;
                break;
            }
        }
        ip_ = vm_->instruction_pointer();
        sync_from_vm();
        if (hitBreakpoint) {
            state_ = ScriptDebugState::Paused;
            notify_breakpoint();
            notify_state();
        }
        return state_;
    }
    if (state_ == ScriptDebugState::Idle || state_ == ScriptDebugState::Stopped) return state_;
    if (state_ == ScriptDebugState::Paused) state_ = ScriptDebugState::Running;

    const size_t depth0 = frames_.size();
    const bool onCall = ip_ < program_.instructions.size() &&
                        program_.instructions[ip_].opcode == DebugOpCode::Call;
    const size_t callReturn = onCall ? ip_ + 1 : std::numeric_limits<size_t>::max();

    for (;;) {
        execute_one(true);
        if (state_ != ScriptDebugState::Running) break;
        if (ip_ < program_.instructions.size() && breakpoints_.count(ip_)) {
            state_ = ScriptDebugState::Paused;
            notify_breakpoint();
            break;
        }
        if (frames_.size() < depth0) break;                    // left current frame
        if (onCall && frames_.size() == depth0 && ip_ >= callReturn) break; // call returned
    }
    notify_state();
    return state_;
}

ScriptDebugState ScriptDebugger::step_out(float deltaTime) {
    if (vm_) {
        return step_over(deltaTime); // no call frames in the flat VM bytecode
    }
    if (state_ == ScriptDebugState::Idle || state_ == ScriptDebugState::Stopped) return state_;
    if (state_ == ScriptDebugState::Paused) state_ = ScriptDebugState::Running;

    const size_t depth0 = frames_.size();
    for (;;) {
        execute_one(true);
        if (state_ != ScriptDebugState::Running) break;
        if (ip_ < program_.instructions.size() && breakpoints_.count(ip_)) {
            state_ = ScriptDebugState::Paused;
            notify_breakpoint();
            break;
        }
        if (frames_.size() < depth0) break; // returned to the caller
    }
    notify_state();
    return state_;
}

ScriptDebugState ScriptDebugger::continue_run(size_t instructionBudget, float deltaTime) {
    if (vm_) {
        vm_->run(deltaTime, instructionBudget);
        ip_ = vm_->instruction_pointer();
        sync_from_vm();
        return state_;
    }
    if (state_ == ScriptDebugState::Idle || state_ == ScriptDebugState::Stopped) return state_;
    if (state_ == ScriptDebugState::Paused) state_ = ScriptDebugState::Running;
    for (size_t i = 0; i < instructionBudget && state_ == ScriptDebugState::Running; ++i) {
        execute_one(false);
    }
    notify_state();
    return state_;
}

void ScriptDebugger::add_breakpoint(size_t instructionIndex) {
    breakpoints_.insert(instructionIndex);
    if (vm_) vm_->add_breakpoint(instructionIndex);
}

void ScriptDebugger::add_line_breakpoint(int line) {
    const auto it = program_.lineToInstruction.find(line);
    if (it != program_.lineToInstruction.end()) add_breakpoint(it->second);
}

void ScriptDebugger::remove_breakpoint(size_t instructionIndex) {
    breakpoints_.erase(instructionIndex);
    if (vm_) vm_->remove_breakpoint(instructionIndex);
}

void ScriptDebugger::clear_breakpoints() {
    breakpoints_.clear();
    if (vm_) vm_->clear_breakpoints();
}

bool ScriptDebugger::has_breakpoint(size_t instructionIndex) const {
    return breakpoints_.count(instructionIndex) != 0;
}

std::optional<size_t> ScriptDebugger::breakpoint_at_line(int line) const {
    const auto it = program_.lineToInstruction.find(line);
    if (it == program_.lineToInstruction.end()) return std::nullopt;
    if (breakpoints_.count(it->second) == 0) return std::nullopt;
    return it->second;
}

int ScriptDebugger::current_line() const {
    if (vm_ || ip_ >= program_.instructions.size()) return -1;
    // Prefer the source line that maps to the current instruction (reverse of
    // lineToInstruction), falling back to the instruction's own line field.
    for (const auto& [line, instruction] : program_.lineToInstruction) {
        if (instruction == ip_) return static_cast<int>(line);
    }
    return program_.instructions[ip_].line;
}

std::unordered_map<std::string, std::string> ScriptDebugger::variables() const {
    std::unordered_map<std::string, std::string> out;
    if (vm_) {
        for (const auto& [name, value] : vm_->variables()) out[name] = value_to_string(value);
        return out;
    }
    for (const auto& [name, value] : variables_) out[name] = value_to_string(value);
    return out;
}

std::optional<std::string> ScriptDebugger::variable(const std::string& name) const {
    const ScriptValue* value = raw_variable(name);
    if (!value) return std::nullopt;
    return value_to_string(*value);
}

const ScriptValue* ScriptDebugger::raw_variable(const std::string& name) const {
    if (vm_) return vm_->variable(name);
    const auto it = variables_.find(name);
    return it != variables_.end() ? &it->second : nullptr;
}

void ScriptDebugger::set_variable(std::string name, ScriptValue value) {
    if (vm_) {
        vm_->set_variable(std::move(name), std::move(value));
        return;
    }
    variables_[std::move(name)] = std::move(value);
}

size_t ScriptDebugger::add_watch(std::string expression) {
    watches_.push_back({ nextWatchId_, std::move(expression), "" });
    return nextWatchId_++;
}

bool ScriptDebugger::remove_watch(size_t watchId) {
    for (auto it = watches_.begin(); it != watches_.end(); ++it) {
        if (it->id == watchId) {
            watches_.erase(it);
            return true;
        }
    }
    return false;
}

void ScriptDebugger::clear_watches() {
    watches_.clear();
}

void ScriptDebugger::evaluate_watches() {
    const auto scope = current_scope();
    for (auto& watch : watches_) {
        watch.result = evaluate_expression(watch.expression, scope);
    }
}

std::string ScriptDebugger::evaluate_expression(
    const std::string& expression,
    const std::unordered_map<std::string, ScriptValue>& scope) {
    WatchParser parser(expression, scope);
    const std::optional<ScriptValue> result = parser.parse();
    if (!result) {
        const std::string& err = parser.error();
        if (err.rfind("undefined variable", 0) == 0) return "undefined";
        return "error: " + err;
    }
    return value_to_string(*result);
}

ScriptDebugState ScriptDebugger::execute_one(bool ignoreBreakpoints) {
    if (state_ == ScriptDebugState::Stopped) return state_;
    if (ip_ >= program_.instructions.size()) {
        state_ = ScriptDebugState::Stopped;
        return state_;
    }
    if (!ignoreBreakpoints && breakpoints_.count(ip_)) {
        state_ = ScriptDebugState::Paused;
        notify_breakpoint();
        return state_;
    }

    const DebugInstruction& inst = program_.instructions[ip_];
    switch (inst.opcode) {
        case DebugOpCode::Nop:
            ++ip_;
            break;
        case DebugOpCode::Push:
            valueStack_.push_back(inst.operand);
            ++ip_;
            break;
        case DebugOpCode::LoadVariable:
            valueStack_.push_back(variables_[inst.text]);
            ++ip_;
            break;
        case DebugOpCode::StoreVariable:
            if (!valueStack_.empty()) {
                variables_[inst.text] = valueStack_.back();
                valueStack_.pop_back();
            }
            ++ip_;
            break;
        case DebugOpCode::Add:
        case DebugOpCode::Subtract:
        case DebugOpCode::Multiply: {
            const std::optional<double> b = pop_number();
            const std::optional<double> a = pop_number();
            if (!a || !b) {
                error_ = "arithmetic on non-numeric values";
                state_ = ScriptDebugState::Stopped;
                return state_;
            }
            double result = 0.0;
            switch (inst.opcode) {
                case DebugOpCode::Add: result = *a + *b; break;
                case DebugOpCode::Subtract: result = *a - *b; break;
                default: result = *a * *b; break;
            }
            valueStack_.push_back(result);
            ++ip_;
            break;
        }
        case DebugOpCode::JumpIfFalse: {
            const bool condition = pop_truthy();
            if (!condition) {
                if (inst.target >= program_.instructions.size()) {
                    error_ = "jump target out of range";
                    state_ = ScriptDebugState::Stopped;
                    return state_;
                }
                ip_ = inst.target;
            } else {
                ++ip_;
            }
            break;
        }
        case DebugOpCode::Jump:
            if (inst.target >= program_.instructions.size()) {
                error_ = "jump target out of range";
                state_ = ScriptDebugState::Stopped;
                return state_;
            }
            ip_ = inst.target;
            break;
        case DebugOpCode::Call: {
            if (inst.target >= program_.instructions.size()) {
                error_ = "call target out of range";
                state_ = ScriptDebugState::Stopped;
                return state_;
            }
            frames_.push_back({ frame_name_for_call(inst.target), inst.target, ip_ + 1 });
            ip_ = inst.target;
            break;
        }
        case DebugOpCode::Return: {
            if (frames_.empty()) {
                state_ = ScriptDebugState::Stopped;
                return state_;
            }
            const DebugFrame frame = frames_.back();
            frames_.pop_back();
            ip_ = frame.returnIp;
            break;
        }
        case DebugOpCode::Halt:
            state_ = ScriptDebugState::Stopped;
            return state_;
    }

    if (ip_ >= program_.instructions.size()) state_ = ScriptDebugState::Stopped;
    return state_;
}

void ScriptDebugger::notify_state() {
    if (state_ != lastNotifiedState_ && stateCallback_) {
        stateCallback_(state_);
    }
    lastNotifiedState_ = state_;
}

void ScriptDebugger::notify_breakpoint() {
    if (!breakpointCallback_) return;
    const int line = (ip_ < program_.instructions.size()) ? program_.instructions[ip_].line : -1;
    breakpointCallback_(ip_, line);
}

void ScriptDebugger::sync_from_vm() {
    if (!vm_) return;
    ip_ = vm_->instruction_pointer();
    switch (vm_->status()) {
        case Engine::VMStatus::Idle: state_ = ScriptDebugState::Idle; break;
        case Engine::VMStatus::Running:
        case Engine::VMStatus::Waiting: state_ = ScriptDebugState::Running; break;
        case Engine::VMStatus::Paused:
            state_ = ScriptDebugState::Paused;
            notify_breakpoint();
            break;
        case Engine::VMStatus::Completed:
        case Engine::VMStatus::Error: state_ = ScriptDebugState::Stopped; break;
    }
    if (vm_->status() == Engine::VMStatus::Error) error_ = vm_->error();
    notify_state();
}

std::optional<double> ScriptDebugger::pop_number() {
    if (valueStack_.empty()) return std::nullopt;
    ScriptValue value = valueStack_.back();
    valueStack_.pop_back();
    if (const double* d = std::get_if<double>(&value)) return *d;
    if (const int64_t* i = std::get_if<int64_t>(&value)) return static_cast<double>(*i);
    if (const bool* b = std::get_if<bool>(&value)) return *b ? 1.0 : 0.0;
    return std::nullopt;
}

bool ScriptDebugger::pop_truthy() {
    if (valueStack_.empty()) return false;
    ScriptValue value = valueStack_.back();
    valueStack_.pop_back();
    return watch_truthy(value);
}

std::string ScriptDebugger::frame_name_for_call(size_t target) const {
    for (const auto& function : program_.functions) {
        if (function.entry == target) return function.name;
    }
    return "fn@" + std::to_string(target);
}

std::unordered_map<std::string, ScriptValue> ScriptDebugger::current_scope() const {
    if (vm_) return vm_->variables();
    return variables_;
}

} // namespace Engine::Scripting

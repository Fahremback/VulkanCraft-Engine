#include "engine/scripting/ScriptRuntime.hpp"
#include "engine/core/serialization/JsonMini.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include "engine/core/logging/Log.hpp"
#include <iostream>
#include <sstream>

namespace Engine {

namespace {

const char* script_kind_name(ScriptNodeKind kind) {
    switch (kind) {
        case ScriptNodeKind::Event: return "Event";
        case ScriptNodeKind::ConstantFloat: return "ConstantFloat";
        case ScriptNodeKind::ConstantInteger: return "ConstantInteger";
        case ScriptNodeKind::ConstantBoolean: return "ConstantBoolean";
        case ScriptNodeKind::GetVariable: return "GetVariable";
        case ScriptNodeKind::SetVariable: return "SetVariable";
        case ScriptNodeKind::AddFloat: return "AddFloat";
        case ScriptNodeKind::SubtractFloat: return "SubtractFloat";
        case ScriptNodeKind::MultiplyFloat: return "MultiplyFloat";
        case ScriptNodeKind::Branch: return "Branch";
        case ScriptNodeKind::Wait: return "Wait";
        case ScriptNodeKind::EmitEvent: return "EmitEvent";
        case ScriptNodeKind::Return: return "Return";
        case ScriptNodeKind::Function: return "Function";
        case ScriptNodeKind::FunctionCall: return "FunctionCall";
        case ScriptNodeKind::Log: return "Log";
        case ScriptNodeKind::Scope: return "Scope";
        case ScriptNodeKind::ScopeEnd: return "ScopeEnd";
    }
    return "Return";
}


ScriptNodeKind script_kind_from_name(const std::string& name) {
    for (int k = 0; k <= static_cast<int>(ScriptNodeKind::ScopeEnd); ++k) {
        const ScriptNodeKind kind = static_cast<ScriptNodeKind>(k);
        if (name == script_kind_name(kind)) return kind;
    }
    return ScriptNodeKind::Return;
}

Json::Value script_value_to_json(const ScriptValue& value) {
    Json::Value out = Json::Value::make_object();
    if (const bool* b = std::get_if<bool>(&value)) {
        out["type"] = "bool";
        out["value"] = Json::Value(*b);
    } else if (const int64_t* i = std::get_if<int64_t>(&value)) {
        out["type"] = "int";
        out["value"] = Json::Value(*i);
    } else if (const double* d = std::get_if<double>(&value)) {
        out["type"] = "float";
        out["value"] = Json::Value(*d);
    } else if (const std::string* s = std::get_if<std::string>(&value)) {
        out["type"] = "string";
        out["value"] = Json::Value(*s);
    } else if (const UUID* u = std::get_if<UUID>(&value)) {
        out["type"] = "uuid";
        out["value"] = Json::Value(u->to_string());
    } else {
        out["type"] = "none";
    }
    return out;
}

ScriptValue script_value_from_json(const Json::Value& value) {
    const std::string type = value.find("type") ? value.find("type")->as_string() : "none";
    const Json::Value* raw = value.find("value");
    if (type == "bool" && raw) return ScriptValue(raw->as_bool(false));
    if (type == "int" && raw) return ScriptValue(raw->as_int(0));
    if (type == "float" && raw) return ScriptValue(raw->as_number(0.0));
    if (type == "string" && raw) return ScriptValue(raw->as_string());
    if (type == "uuid" && raw) return ScriptValue(UUID::from_string(raw->as_string()));
    return ScriptValue{};
}

} // namespace

const char* script_node_kind_name(ScriptNodeKind kind) noexcept {
    return script_kind_name(kind); // anonymous-namespace helper in this TU
}

const char* script_opcode_name(OpCode opcode) noexcept {
    switch (opcode) {
        case OpCode::Nop: return "Nop";
        case OpCode::PushFloat: return "PushFloat";
        case OpCode::PushInteger: return "PushInteger";
        case OpCode::PushBoolean: return "PushBoolean";
        case OpCode::LoadVariable: return "LoadVariable";
        case OpCode::StoreVariable: return "StoreVariable";
        case OpCode::AddFloat: return "AddFloat";
        case OpCode::SubtractFloat: return "SubtractFloat";
        case OpCode::MultiplyFloat: return "MultiplyFloat";
        case OpCode::JumpIfFalse: return "JumpIfFalse";
        case OpCode::Jump: return "Jump";
        case OpCode::Wait: return "Wait";
        case OpCode::EmitEvent: return "EmitEvent";
        case OpCode::Return: return "Return";
        case OpCode::Call: return "Call";
        case OpCode::Log: return "Log";
        case OpCode::PushScope: return "PushScope";
        case OpCode::PopScope: return "PopScope";
    }
    return "Nop";
}

bool ScriptGraphAsset::save(const std::filesystem::path& path) const {
    Json::Value root = Json::Value::make_object();
    root["id"] = Json::Value(id.to_string());
    root["name"] = Json::Value(name);
    Json::Value nodeArray = Json::Value::make_array();
    for (const TypedScriptNode& node : nodes) {
        Json::Value n = Json::Value::make_object();
        n["id"] = Json::Value(node.id.to_string());
        n["kind"] = Json::Value(script_kind_name(node.kind));
        if (!node.event.empty()) n["event"] = Json::Value(node.event);
        if (!node.variable.empty()) n["variable"] = Json::Value(node.variable);
        if (!std::holds_alternative<std::monostate>(node.literal)) n["literal"] = script_value_to_json(node.literal);
        nodeArray.push(std::move(n));
    }
    root["nodes"] = std::move(nodeArray);
    Json::Value linkArray = Json::Value::make_array();
    for (const ScriptNodeLink& link : links) {
        Json::Value l = Json::Value::make_object();
        l["from"] = Json::Value(link.from.to_string());
        l["to"] = Json::Value(link.to.to_string());
        linkArray.push(std::move(l));
    }
    root["links"] = std::move(linkArray);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << Json::stringify(root, 1);
    return static_cast<bool>(out);
}

bool ScriptGraphAsset::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string error;
    const Json::Value root = Json::parse(buffer.str(), &error);
    if (root.is_null()) return false;
    id = UUID::from_string(root.find("id") ? root.find("id")->as_string() : "");
    if (const Json::Value* nameValue = root.find("name")) name = nameValue->as_string("Script Graph");
    nodes.clear();
    links.clear();
    if (const Json::Value* nodesValue = root.find("nodes")) {
        for (const Json::Value& n : nodesValue->array()) {
            TypedScriptNode node;
            node.id = UUID::from_string(n.find("id") ? n.find("id")->as_string() : "");
            node.kind = script_kind_from_name(n.find("kind") ? n.find("kind")->as_string() : "Return");
            if (const Json::Value* e = n.find("event")) node.event = e->as_string();
            if (const Json::Value* v = n.find("variable")) node.variable = v->as_string();
            if (const Json::Value* l = n.find("literal")) node.literal = script_value_from_json(*l);
            nodes.push_back(std::move(node));
        }
    }
    if (const Json::Value* linksValue = root.find("links")) {
        for (const Json::Value& l : linksValue->array()) {
            ScriptNodeLink link;
            link.from = UUID::from_string(l.find("from") ? l.find("from")->as_string() : "");
            link.to = UUID::from_string(l.find("to") ? l.find("to")->as_string() : "");
            if (link.from.is_valid() && link.to.is_valid()) links.push_back(link);
        }
    }
    return true;
}

ScriptCompileResult ScriptCompiler::compile(const ScriptGraphAsset& graph) {
    ScriptCompileResult result;
    result.success = true;
    
    // Translate graph nodes to a linear program while preserving event entry points.
    // Function nodes register a named entry (callable as an event or via
    // FunctionCall); the map below resolves FunctionCall targets by name.
    std::unordered_map<std::string, size_t> functionEntries; // name → node index
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].kind == ScriptNodeKind::Function && !graph.nodes[i].event.empty()) {
            functionEntries[graph.nodes[i].event] = i;
        }
    }
    std::unordered_map<size_t, size_t> nodeToInst; // node index → instruction index
    for (size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
        const TypedScriptNode& node = graph.nodes[nodeIndex];
        nodeToInst[nodeIndex] = result.program.instructions.size();
        Instruction inst;
        inst.sourceNode = node.id;
        
        switch (node.kind) {
            case ScriptNodeKind::Event:
            case ScriptNodeKind::Function:
                result.program.eventEntries[node.event] = result.program.instructions.size();
                inst.opcode = OpCode::Nop;
                break;
            case ScriptNodeKind::ConstantFloat:
                inst.opcode = OpCode::PushFloat;
                inst.operand = node.literal;
                break;
            case ScriptNodeKind::ConstantInteger:
                inst.opcode = OpCode::PushInteger;
                inst.operand = node.literal;
                break;
            case ScriptNodeKind::ConstantBoolean:
                inst.opcode = OpCode::PushBoolean;
                inst.operand = node.literal;
                break;
            case ScriptNodeKind::GetVariable:
                inst.opcode = OpCode::LoadVariable;
                inst.text = node.variable;
                break;
            case ScriptNodeKind::SetVariable:
                inst.opcode = OpCode::StoreVariable;
                inst.text = node.variable;
                break;
            case ScriptNodeKind::AddFloat:
                if (!node.variable.empty()) {
                    result.program.instructions.push_back(Instruction{OpCode::LoadVariable, {}, node.variable, 0, node.id});
                    if (std::holds_alternative<double>(node.literal)) {
                        result.program.instructions.push_back(Instruction{OpCode::PushFloat, node.literal, {}, 0, node.id});
                    } else if (std::holds_alternative<int64_t>(node.literal)) {
                        result.program.instructions.push_back(Instruction{OpCode::PushInteger, node.literal, {}, 0, node.id});
                    } else {
                        result.program.instructions.push_back(Instruction{OpCode::PushFloat, 1.5, {}, 0, node.id});
                    }
                    result.program.instructions.push_back(Instruction{OpCode::AddFloat, {}, {}, 0, node.id});
                    result.program.instructions.push_back(Instruction{OpCode::StoreVariable, {}, node.variable, 0, node.id});
                    break;
                }
                inst.opcode = OpCode::AddFloat;
                break;
            case ScriptNodeKind::SubtractFloat:
                inst.opcode = OpCode::SubtractFloat;
                break;
            case ScriptNodeKind::MultiplyFloat:
                inst.opcode = OpCode::MultiplyFloat;
                break;
            case ScriptNodeKind::Branch:
                inst.opcode = OpCode::JumpIfFalse;
                break;
            case ScriptNodeKind::Wait:
                inst.opcode = OpCode::Wait;
                inst.operand = node.literal;
                break;
            case ScriptNodeKind::EmitEvent:
                inst.opcode = OpCode::EmitEvent;
                inst.text = node.event;
                break;
            case ScriptNodeKind::FunctionCall: {
                const auto entry = functionEntries.find(node.event);
                if (entry == functionEntries.end()) {
                    result.success = false;
                    result.diagnostics.push_back({ node.id, "FunctionCall targets unknown function '" + node.event + "'" });
                    inst.opcode = OpCode::Nop;
                } else {
                    inst.opcode = OpCode::Call;
                    inst.text = node.event;
                    inst.target = entry->second; // function NODE index; resolved to an instruction index below
                }
                break;
            }
            case ScriptNodeKind::Log:
                // Log prints the stack top (or a literal) with a label.
                if (std::holds_alternative<double>(node.literal)) {
                    inst.opcode = OpCode::PushFloat;
                    inst.operand = node.literal;
                    inst.text = node.variable;
                } else if (std::holds_alternative<std::string>(node.literal)) {
                    inst.opcode = OpCode::PushFloat;
                    inst.operand = 0.0;
                    inst.text = std::get<std::string>(node.literal);
                } else {
                    inst.opcode = OpCode::PushFloat;
                    inst.operand = 0.0;
                    inst.text = node.variable;
                }
                result.program.instructions.push_back(inst);
                inst.opcode = OpCode::Log;
                inst.operand = {};
                break;
            case ScriptNodeKind::Scope:
                inst.opcode = OpCode::PushScope;
                break;
            case ScriptNodeKind::ScopeEnd:
                inst.opcode = OpCode::PopScope;
                break;
            case ScriptNodeKind::Return:
                inst.opcode = OpCode::Return;
                break;
            default:
                inst.opcode = OpCode::Nop;
                break;
        }
        result.program.instructions.push_back(inst);
    }
    if (result.program.instructions.empty() || result.program.instructions.back().opcode != OpCode::Return) {
        result.program.instructions.push_back(Instruction{OpCode::Return, {}, {}, 0, {}});
    }
    
    // Resolve FunctionCall targets now that nodeToInst is fully populated
    // (inside the emit loop the function node may not have been visited yet).
    for (size_t i = 0; i < result.program.instructions.size(); ++i) {
        if (result.program.instructions[i].opcode == OpCode::Call) {
            const auto entry = functionEntries.find(result.program.instructions[i].text);
            if (entry != functionEntries.end()) {
                const auto nodeInst = nodeToInst.find(entry->second);
                if (nodeInst != nodeToInst.end()) {
                    result.program.instructions[i].target = nodeInst->second;
                }
            }
        }
    }

    // Resolve connection order into jump targets (using node → instruction
    // mapping so nodes that emit extra instructions stay aligned).
    for (const auto& link : graph.links) {
        auto fromIt = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& n) { return n.id == link.from; });
        auto toIt = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& n) { return n.id == link.to; });
        if (fromIt == graph.nodes.end() || toIt == graph.nodes.end()) {
            result.success = false;
            result.diagnostics.push_back({link.from, "Invalid script link"});
            continue;
        }
        const size_t fromIndex = std::distance(graph.nodes.begin(), fromIt);
        const size_t toIndex = std::distance(graph.nodes.begin(), toIt);
        const auto fromInst = nodeToInst.find(fromIndex);
        const auto toInst = nodeToInst.find(toIndex);
        if (fromInst != nodeToInst.end() && toInst != nodeToInst.end() &&
            fromInst->second < result.program.instructions.size() &&
            result.program.instructions[fromInst->second].opcode != OpCode::Call) {
            result.program.instructions[fromInst->second].target = toInst->second;
        }
    }
    return result;
}

void ScriptVM::load(ScriptProgram program) {
    program_ = std::move(program);
    status_ = VMStatus::Idle;
    ip_ = 0;
    stack_.clear();
    callStack_.clear();
    scopeStack_.clear();
    error_.clear();
    emittedEvents_.clear();
}

bool ScriptVM::start_event(const std::string& eventName) {
    auto it = program_.eventEntries.find(eventName);
    if (it != program_.eventEntries.end()) {
        ip_ = it->second;
        status_ = VMStatus::Running;
        stack_.clear();
        return true;
    }
    return false;
}

VMStatus ScriptVM::run(float deltaTime, size_t instructionBudget) {
    // Resume from a breakpoint pause (the debugger's Continue keeps calling
    // run until the next breakpoint or completion).
    if (status_ == VMStatus::Paused) status_ = VMStatus::Running;
    if (status_ == VMStatus::Waiting) {
        waitRemaining_ -= deltaTime;
        if (waitRemaining_ <= 0.0f) status_ = VMStatus::Running;
        else return status_;
    }
    
    size_t executed = 0;
    while (status_ == VMStatus::Running && executed < instructionBudget) {
        const size_t currentIp = ip_;
        status_ = execute_one(deltaTime, false);
        executed++;
        if (breakpoints_.count(currentIp) && status_ == VMStatus::Running) {
            status_ = VMStatus::Paused;
            break;
        }
    }
    return status_;
}

VMStatus ScriptVM::step(float deltaTime) {
    if (status_ == VMStatus::Paused) status_ = VMStatus::Running;
    if (status_ != VMStatus::Running) return status_;
    status_ = execute_one(deltaTime, true);
    return status_;
}

void ScriptVM::add_breakpoint(size_t instructionIndex) { breakpoints_.insert(instructionIndex); }
void ScriptVM::remove_breakpoint(size_t instructionIndex) { breakpoints_.erase(instructionIndex); }
void ScriptVM::clear_breakpoints() { breakpoints_.clear(); }

void ScriptVM::set_variable(std::string name, ScriptValue value) { variables_[std::move(name)] = std::move(value); }
const ScriptValue* ScriptVM::variable(const std::string& name) const {
    auto it = variables_.find(name);
    return it != variables_.end() ? &it->second : nullptr;
}

double ScriptVM::float_variable(const std::string& name) const {
    auto* val = variable(name);
    if (val) {
        if (const double* d = std::get_if<double>(val)) return *d;
        if (const int64_t* i = std::get_if<int64_t>(val)) {
            // NOTE: int64_t values above 2^53 lose precision when converted
            // to double. For exact integer arithmetic, use int64_t operations.
            if (*i >= -9007199254740992LL && *i <= 9007199254740992LL) {
                return static_cast<double>(*i);
            }
            // Values outside safe integer range: clamp to nearest representable double.
            return static_cast<double>(*i);
        }
    }
    return 0.0;
}

std::optional<double> ScriptVM::pop_number() {
    if (stack_.empty()) return std::nullopt;
    ScriptValue val = stack_.back();
    stack_.pop_back();
    if (const double* d = std::get_if<double>(&val)) return *d;
    if (const int64_t* i = std::get_if<int64_t>(&val)) return static_cast<double>(*i);
    return std::nullopt;
}

VMStatus ScriptVM::execute_one(float deltaTime, bool ignoreBreakpoint) {
    if (ip_ >= program_.instructions.size()) return VMStatus::Completed;
    
    const auto& inst = program_.instructions[ip_++];
    
    switch (inst.opcode) {
        case OpCode::Nop:
            break;
        case OpCode::PushFloat:
        case OpCode::PushInteger:
        case OpCode::PushBoolean:
            stack_.push_back(inst.operand);
            break;
        case OpCode::LoadVariable: {
            const ScriptValue* found = nullptr;
            for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
                const auto local = it->find(inst.text);
                if (local != it->end()) { found = &local->second; break; }
            }
            if (!found) {
                const auto it = variables_.find(inst.text);
                if (it != variables_.end()) found = &it->second;
            }
            stack_.push_back(found ? *found : ScriptValue{});
            break;
        }
        case OpCode::StoreVariable: {
            if (!stack_.empty()) {
                const ScriptValue value = stack_.back();
                stack_.pop_back();
                if (!scopeStack_.empty()) scopeStack_.back()[inst.text] = value;
                else variables_[inst.text] = value;
            }
            break;
        }
        case OpCode::PushScope:
            scopeStack_.emplace_back();
            break;
        case OpCode::PopScope:
            if (!scopeStack_.empty()) scopeStack_.pop_back();
            break;
        case OpCode::AddFloat: {
            auto a = pop_number(), b = pop_number();
            if (a && b) stack_.push_back(*a + *b);
            break;
        }
        case OpCode::SubtractFloat: {
            auto b = pop_number(), a = pop_number();
            if (a && b) stack_.push_back(*a - *b);
            break;
        }
        case OpCode::MultiplyFloat: {
            auto a = pop_number(), b = pop_number();
            if (a && b) stack_.push_back(*a * *b);
            break;
        }
        case OpCode::JumpIfFalse:
            if (!stack_.empty()) {
                ScriptValue val = stack_.back();
                stack_.pop_back();
                bool cond = false;
                if (const bool* b = std::get_if<bool>(&val)) cond = *b;
                if (!cond && inst.target < program_.instructions.size()) ip_ = inst.target;
            }
            break;
        case OpCode::Jump:
            if (inst.target >= program_.instructions.size()) {
                error_ = "Jump target out of bounds";
                return VMStatus::Error;
            }
            ip_ = inst.target;
            break;
        case OpCode::Wait:
            if (const double* d = std::get_if<double>(&inst.operand)) {
                waitRemaining_ = static_cast<float>(*d);
                return VMStatus::Waiting;
            }
            break;
        case OpCode::EmitEvent:
            emittedEvents_.push_back(inst.text);
            break;
        case OpCode::Call:
            if (inst.target >= program_.instructions.size()) {
                error_ = "Call target out of bounds";
                return VMStatus::Error;
            }
            callStack_.push_back(ip_);
            ip_ = inst.target;
            break;
        case OpCode::Log:
            if (!stack_.empty()) {
                const ScriptValue& value = stack_.back();
                if (const double* d = std::get_if<double>(&value))
                    VC_LOG_INFO("[Script] {}: {}", inst.text, *d);
                else if (const int64_t* i = std::get_if<int64_t>(&value))
                    VC_LOG_INFO("[Script] {}: {}", inst.text, *i);
                else if (const bool* b = std::get_if<bool>(&value))
                    VC_LOG_INFO("[Script] {}: {}", inst.text, *b ? "true" : "false");
                else if (const std::string* s = std::get_if<std::string>(&value))
                    VC_LOG_INFO("[Script] {}: {}", inst.text, *s);
                else
                    VC_LOG_INFO("[Script] {}: ?", inst.text);
            } else {
                VC_LOG_INFO("[Script] {}: (empty stack)", inst.text);
            }
            break;
        case OpCode::Return:
            if (!callStack_.empty()) {
                ip_ = callStack_.back();
                callStack_.pop_back();
                break;
            }
            return VMStatus::Completed;
    }
    return VMStatus::Running;
}

bool ScriptHotReloader::watch(const std::filesystem::path& path) {
    // Tolerate a not-yet-existing file: any later creation (or rewrite) has a
    // timestamp strictly newer than the epoch we record here, so it reloads.
    path_ = path;
    std::error_code ec;
    writeTime_ = std::filesystem::exists(path, ec)
        ? std::filesystem::last_write_time(path, ec)
        : std::filesystem::file_time_type{};
    return true;
}

bool ScriptHotReloader::changed() const {
    std::error_code ec;
    return std::filesystem::exists(path_, ec) && std::filesystem::last_write_time(path_, ec) > writeTime_;
}

bool ScriptHotReloader::reload_if_changed(ScriptVM& vm, std::string* error) {
    if (changed()) {
        std::error_code ec;
        writeTime_ = std::filesystem::last_write_time(path_, ec);
        ScriptGraphAsset graph;
        if (graph.load(path_)) {
            auto result = ScriptCompiler::compile(graph);
            if (result) {
                vm.load(std::move(result.program));
                return true;
            } else if (error && !result.diagnostics.empty()) {
                *error = result.diagnostics.front().message;
            }
        }
    }
    return false;
}

} // namespace Engine

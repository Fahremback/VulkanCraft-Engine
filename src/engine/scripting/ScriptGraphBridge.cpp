#include "ScriptGraphBridge.hpp"

#include <algorithm>
#include <cstdint>

namespace Engine {

namespace {

ScriptPin make_pin(const std::string& name, PinType type, bool isInput) {
    ScriptPin pin;
    pin.id = UUID();
    pin.name = name;
    pin.type = type;
    pin.isInput = isInput;
    return pin;
}

std::string kind_to_event(const ScriptNodeKind kind) {
    switch (kind) {
        case ScriptNodeKind::Event: return "OnStart";
        case ScriptNodeKind::EmitEvent: return "Emit";
        case ScriptNodeKind::Function: return "Func";
        default: return "";
    }
}

} // namespace

VisualScriptGraph to_visual_graph(const ScriptGraphAsset& asset) {
    VisualScriptGraph graph;
    graph.id = asset.id.is_valid() ? asset.id : UUID();
    graph.name = asset.name;
    graph.nodes.reserve(asset.nodes.size());
    for (const TypedScriptNode& typed : asset.nodes) {
        ScriptNode node;
        node.id = typed.id.is_valid() ? typed.id : UUID();
        switch (typed.kind) {
            case ScriptNodeKind::ConstantFloat:
                node.title = "Constant Float";
                if (const double* d = std::get_if<double>(&typed.literal)) node.title += ": " + std::to_string(*d);
                node.outputs.push_back(make_pin("Value", PinType::Float, false));
                break;
            case ScriptNodeKind::ConstantInteger:
                node.title = "Constant Integer";
                if (const int64_t* i = std::get_if<int64_t>(&typed.literal)) node.title += ": " + std::to_string(*i);
                node.outputs.push_back(make_pin("Value", PinType::Integer, false));
                break;
            case ScriptNodeKind::ConstantBoolean:
                node.title = "Constant Boolean";
                if (const bool* b = std::get_if<bool>(&typed.literal)) node.title += std::string(": ") + (*b ? "true" : "false");
                node.outputs.push_back(make_pin("Value", PinType::Boolean, false));
                break;
            case ScriptNodeKind::GetVariable:
                node.title = typed.variable.empty() ? "Get Variable" : ("Get Variable: " + typed.variable);
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.outputs.push_back(make_pin("Value", PinType::Float, false));
                break;
            case ScriptNodeKind::SetVariable:
                node.title = typed.variable.empty() ? "Set Variable" : ("Set Variable: " + typed.variable);
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.inputs.push_back(make_pin("Value", PinType::Float, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::AddFloat:
                node.title = "Add Float";
                node.inputs.push_back(make_pin("A", PinType::Float, true));
                node.inputs.push_back(make_pin("B", PinType::Float, true));
                node.outputs.push_back(make_pin("Result", PinType::Float, false));
                break;
            case ScriptNodeKind::SubtractFloat:
                node.title = "Subtract Float";
                node.inputs.push_back(make_pin("A", PinType::Float, true));
                node.inputs.push_back(make_pin("B", PinType::Float, true));
                node.outputs.push_back(make_pin("Result", PinType::Float, false));
                break;
            case ScriptNodeKind::MultiplyFloat:
                node.title = "Multiply Float";
                node.inputs.push_back(make_pin("A", PinType::Float, true));
                node.inputs.push_back(make_pin("B", PinType::Float, true));
                node.outputs.push_back(make_pin("Result", PinType::Float, false));
                break;
            case ScriptNodeKind::Branch:
                node.title = "Branch";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.inputs.push_back(make_pin("Condition", PinType::Boolean, true));
                node.outputs.push_back(make_pin("True", PinType::Execution, false));
                node.outputs.push_back(make_pin("False", PinType::Execution, false));
                break;
            case ScriptNodeKind::Wait:
                node.title = "Wait";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.inputs.push_back(make_pin("Seconds", PinType::Float, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::EmitEvent:
                node.title = typed.event.empty() ? "Emit Event" : ("Emit: " + typed.event);
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::Return:
                node.title = "Return";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                break;
            case ScriptNodeKind::Function:
                node.title = typed.event.empty() ? "Function" : ("Function: " + typed.event);
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::FunctionCall:
                node.title = "Function Call";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::Log:
                node.title = "Log";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.inputs.push_back(make_pin("Message", PinType::Float, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::Scope:
                node.title = "Scope";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
            case ScriptNodeKind::ScopeEnd:
                node.title = "Scope End";
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                break;
            default:
                node.title = "Event: " + (typed.event.empty() ? "OnStart" : typed.event);
                node.inputs.push_back(make_pin("In", PinType::Execution, true));
                node.outputs.push_back(make_pin("Out", PinType::Execution, false));
                break;
        }
        graph.add_node(std::move(node));
    }
    // Link: node→node becomes source primary output → target primary input.
    for (const ScriptNodeLink& link : asset.links) {
        const ScriptNode* from = nullptr;
        const ScriptNode* to = nullptr;
        for (const ScriptNode& node : graph.nodes) {
            if (node.id == link.from) from = &node;
            if (node.id == link.to) to = &node;
        }
        if (!from || !to || from->outputs.empty() || to->inputs.empty()) continue;
        graph.connect_pins(from->outputs.front().id, to->inputs.front().id);
    }
    return graph;
}

ScriptGraphAsset from_visual_graph(const VisualScriptGraph& graph, const ScriptGraphAsset& original) {
    ScriptGraphAsset asset;
    asset.id = graph.id.is_valid() ? graph.id : (original.id.is_valid() ? original.id : UUID());
    asset.name = graph.name.empty() ? original.name : graph.name;
    asset.nodes.reserve(graph.nodes.size());

    // Map canvas node → executable node (kind chosen from title when the
    // canvas node has no stored kind; title round-trips through to_visual).
    struct Mapping { const ScriptNode* node; TypedScriptNode typed; };
    std::vector<Mapping> mappings;
    mappings.reserve(graph.nodes.size());

    const auto kind_from_title = [](const std::string& title) -> ScriptNodeKind {
        if (title.rfind("Event:", 0) == 0) return ScriptNodeKind::Event;
        if (title == "Constant Float" || title.rfind("Constant Float: ", 0) == 0) return ScriptNodeKind::ConstantFloat;
        if (title == "Constant Integer" || title.rfind("Constant Integer: ", 0) == 0) return ScriptNodeKind::ConstantInteger;
        if (title == "Constant Boolean" || title.rfind("Constant Boolean: ", 0) == 0) return ScriptNodeKind::ConstantBoolean;
        if (title == "Get Variable" || title.rfind("Get Variable: ", 0) == 0) return ScriptNodeKind::GetVariable;
        if (title == "Set Variable" || title.rfind("Set Variable: ", 0) == 0) return ScriptNodeKind::SetVariable;
        if (title == "Add Float") return ScriptNodeKind::AddFloat;
        if (title == "Subtract Float") return ScriptNodeKind::SubtractFloat;
        if (title == "Multiply Float") return ScriptNodeKind::MultiplyFloat;
        if (title == "Branch") return ScriptNodeKind::Branch;
        if (title == "Wait") return ScriptNodeKind::Wait;
        if (title.rfind("Emit:", 0) == 0 || title == "Emit Event") return ScriptNodeKind::EmitEvent;
        if (title == "Return") return ScriptNodeKind::Return;
        if (title.rfind("Function:", 0) == 0) return ScriptNodeKind::Function;
        if (title == "Function Call") return ScriptNodeKind::FunctionCall;
        if (title == "Log") return ScriptNodeKind::Log;
        if (title == "Scope") return ScriptNodeKind::Scope;
        if (title == "Scope End") return ScriptNodeKind::ScopeEnd;
        return ScriptNodeKind::Event;
    };

    for (const ScriptNode& node : graph.nodes) {
        TypedScriptNode typed;
        typed.id = node.id;
        typed.kind = kind_from_title(node.title);
        if (typed.kind == ScriptNodeKind::ConstantFloat && node.title.rfind("Constant Float: ", 0) == 0) {
            try { typed.literal = std::stod(node.title.substr(16)); } catch (...) { typed.literal = 0.0; }
        } else if (typed.kind == ScriptNodeKind::ConstantInteger && node.title.rfind("Constant Integer: ", 0) == 0) {
            try { typed.literal = static_cast<int64_t>(std::stoll(node.title.substr(18))); } catch (...) { typed.literal = int64_t{0}; }
        } else if (typed.kind == ScriptNodeKind::ConstantBoolean && node.title.rfind("Constant Boolean: ", 0) == 0) {
            const std::string flag = node.title.substr(18);
            typed.literal = (flag == "true");
        } else if (typed.kind == ScriptNodeKind::SetVariable && node.title.rfind("Set Variable: ", 0) == 0) {
            typed.variable = node.title.substr(14);
        } else if (typed.kind == ScriptNodeKind::GetVariable && node.title.rfind("Get Variable: ", 0) == 0) {
            typed.variable = node.title.substr(14);
        } else if (typed.kind == ScriptNodeKind::Event && node.title.rfind("Event:", 0) == 0) {
            typed.event = node.title.substr(7);   // skip "Event: "
        } else if (typed.kind == ScriptNodeKind::EmitEvent && node.title.rfind("Emit:", 0) == 0) {
            typed.event = node.title.substr(6);   // skip "Emit: "
        } else if (typed.kind == ScriptNodeKind::Function && node.title.rfind("Function:", 0) == 0) {
            typed.event = node.title.substr(10);  // skip "Function: "
        } else if (typed.kind == ScriptNodeKind::Event || typed.kind == ScriptNodeKind::EmitEvent || typed.kind == ScriptNodeKind::Function) {
            typed.event = kind_to_event(typed.kind);
        }
        asset.nodes.push_back(typed);
        mappings.push_back({&node, typed});
    }

    // Connections: pin→pin → node→node (deduplicated).
    const auto pin_owner = [&graph](UUID pinID) -> UUID {
        for (const ScriptNode& node : graph.nodes) {
            for (const ScriptPin& pin : node.inputs) if (pin.id == pinID) return node.id;
            for (const ScriptPin& pin : node.outputs) if (pin.id == pinID) return node.id;
        }
        return UUID{0, 0};
    };
    for (const ScriptConnection& connection : graph.connections) {
        const UUID fromOwner = pin_owner(connection.fromPinID);
        const UUID toOwner = pin_owner(connection.toPinID);
        if (!fromOwner.is_valid() || !toOwner.is_valid()) continue;
        const bool exists = std::any_of(asset.links.begin(), asset.links.end(),
                                        [&](const ScriptNodeLink& l) { return l.from == fromOwner && l.to == toOwner; });
        if (exists) continue;
        asset.links.push_back({fromOwner, toOwner});
    }
    return asset;
}

} // namespace Engine

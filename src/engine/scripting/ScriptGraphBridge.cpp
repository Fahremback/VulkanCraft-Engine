#include "ScriptGraphBridge.hpp"
#include "engine/core/serialization/JsonMini.hpp"
#include "engine/entity/IEntityWorld.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <unordered_set>

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

namespace engine::scripting {
namespace {

using engine::entity::ComponentData;
using engine::entity::EntityId;
using engine::entity::Health;
using engine::entity::IEntityWorld;
using engine::entity::Position;

BridgeResult ok(std::string data = {}) { return {true, {}, std::move(data)}; }
BridgeResult fail(std::string error) { return {false, std::move(error), {}}; }

std::string handle_string(EntityId id) {
    return std::to_string(id.id) + ":" + std::to_string(id.generation);
}

bool parse_handle(const std::string& value, EntityId& out) {
    const auto colon = value.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) return false;
    try {
        const auto id = std::stoul(value.substr(0, colon));
        const auto generation = std::stoul(value.substr(colon + 1));
        if (id == 0 || id > 0xFFFFFFFFull || generation > 0xFFFFFFFFull) return false;
        out = {static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(generation)};
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_object(const std::string& data, Engine::Json::Value& out, std::string& error) {
    out = Engine::Json::parse(data, &error);
    return error.empty() && out.is_object();
}

std::string position_json(const Position& p) {
    Engine::Json::Value value = Engine::Json::Value::make_object();
    value["x"] = static_cast<double>(p.x);
    value["y"] = static_cast<double>(p.y);
    value["z"] = static_cast<double>(p.z);
    return Engine::Json::stringify(value);
}

std::string health_json(const Health& h) {
    Engine::Json::Value value = Engine::Json::Value::make_object();
    value["value"] = static_cast<double>(h.value);
    value["max"] = static_cast<double>(h.max);
    return Engine::Json::stringify(value);
}

void shallow_merge(Engine::Json::Value& target, const Engine::Json::Value& patch) {
    for (const auto& [key, value] : patch.object()) target[key] = value;
}

class EcsScriptingBridge final : public IScriptingBridge, public IScriptingBridgeBatch {
public:
    EcsScriptingBridge(IEntityWorld& world, EcsScriptingBridgeConfig config)
        : world_(world), config_(std::move(config)), permissions_(config_.permissions.begin(), config_.permissions.end()) {
        if (config_.context_id.empty()) config_.context_id = "runtime.scripting";
        if (config_.spawn_type.empty()) config_.spawn_type = "script.entity";
    }

    const std::string& context_id() const override { return config_.context_id; }
    bool has_permission(BridgePermission permission) const override {
        return permissions_.count(permission) != 0;
    }

    BridgeResult read_component(const EntityHandle& entity, const std::string& type) const override {
        if (!has_permission(BridgePermission::ReadComponents)) return fail("permission:read_components");
        EntityId id{};
        if (!parse_live(entity, id)) return fail("not_found:entity");
        if (type == "Position" || type == "Transform") {
            Position p{};
            return world_.get_position(id, p) ? ok(position_json(p)) : fail("not_found:Position");
        }
        if (type == "Health") {
            Health h{};
            return world_.get_health(id, h) ? ok(health_json(h)) : fail("not_found:Health");
        }
        ComponentData component;
        if (!world_.get_component(id, type, component)) return fail("not_found:" + type);
        return ok(component.blob);
    }

    BridgeResult list_components(const EntityHandle& entity) const override {
        if (!has_permission(BridgePermission::ReadComponents)) return fail("permission:read_components");
        EntityId id{};
        if (!parse_live(entity, id)) return fail("not_found:entity");
        Engine::Json::Value array = Engine::Json::Value::make_array();
        array.push("Position");
        array.push("Health");
        world_.for_each_component(id, [&](const ComponentData& c) { array.push(c.type); });
        return ok(Engine::Json::stringify(array));
    }

    BridgeResult write_component(const EntityHandle& entity, const std::string& type,
                                 const std::string& data) override {
        if (!has_permission(BridgePermission::WriteComponents)) return fail("permission:write_components");
        if (type.empty()) return fail("invalid:component_type");
        EntityId id{};
        if (!parse_live(entity, id)) return fail("not_found:entity");
        std::string parseError;
        Engine::Json::Value patch;
        if (!parse_object(data, patch, parseError)) return fail("invalid:json:" + parseError);

        if (type == "Position" || type == "Transform") {
            Position p{};
            if (!world_.get_position(id, p)) return fail("not_found:Position");
            for (const auto& [key, value] : patch.object()) {
                if (!value.is_number()) return fail("invalid:Position." + key);
                if (key == "x") p.x = static_cast<float>(value.as_number());
                else if (key == "y") p.y = static_cast<float>(value.as_number());
                else if (key == "z") p.z = static_cast<float>(value.as_number());
                else return fail("invalid:Position." + key);
            }
            return world_.set_position(id, p) ? ok(position_json(p)) : fail("runtime:set_position");
        }
        if (type == "Health") {
            Health h{};
            if (!world_.get_health(id, h)) return fail("not_found:Health");
            for (const auto& [key, value] : patch.object()) {
                if (!value.is_number()) return fail("invalid:Health." + key);
                if (key == "value") h.value = static_cast<float>(value.as_number());
                else if (key == "max") h.max = static_cast<float>(value.as_number());
                else return fail("invalid:Health." + key);
            }
            if (h.max < 0.0f || h.value < 0.0f || h.value > h.max) return fail("invalid:Health.range");
            return world_.set_health(id, h) ? ok(health_json(h)) : fail("runtime:set_health");
        }

        Engine::Json::Value merged = Engine::Json::Value::make_object();
        ComponentData current;
        if (world_.get_component(id, type, current) && !current.blob.empty()) {
            std::string existingError;
            auto existing = Engine::Json::parse(current.blob, &existingError);
            if (existingError.empty() && existing.is_object()) merged = std::move(existing);
        }
        shallow_merge(merged, patch);
        ComponentData component{type, current.version == 0 ? 1u : current.version,
                                Engine::Json::stringify(merged)};
        return world_.set_component(id, component) ? ok(component.blob) : fail("runtime:set_component");
    }

    BridgeResult remove_component(const EntityHandle& entity, const std::string& type) override {
        if (!has_permission(BridgePermission::WriteComponents)) return fail("permission:write_components");
        EntityId id{};
        if (!parse_live(entity, id)) return fail("not_found:entity");
        if (type == "Position" || type == "Transform" || type == "Health") return fail("invalid:builtin_component");
        return world_.remove_component(id, type) ? ok() : fail("runtime:remove_component");
    }

    BridgeResult spawn_entity() override {
        if (!has_permission(BridgePermission::SpawnEntities)) return fail("permission:spawn_entities");
        std::string error;
        const EntityId id = world_.spawn(config_.spawn_type, Position{}, error);
        if (!id.valid()) return fail("runtime:spawn:" + error);
        return ok("\"" + handle_string(id) + "\"");
    }

    BridgeResult destroy_entity(const EntityHandle& entity) override {
        if (!has_permission(BridgePermission::DestroyEntities)) return fail("permission:destroy_entities");
        EntityId id{};
        if (!parse_live(entity, id)) return fail("not_found:entity");
        return world_.despawn(id) ? ok() : fail("runtime:despawn");
    }

    BridgeResult query_entities(const EntityQuery& query) const override {
        if (!has_permission(BridgePermission::QueryEntities)) return fail("permission:query_entities");
        Engine::Json::Value array = Engine::Json::Value::make_array();
        std::uint32_t count = 0;
        world_.for_each_entity([&](EntityId id) {
            if (query.limit != 0 && count >= query.limit) return;
            for (const auto& type : query.required_components) if (!has_component(id, type)) return;
            for (const auto& type : query.excluded_components) if (has_component(id, type)) return;
            array.push(handle_string(id));
            ++count;
        });
        return ok(Engine::Json::stringify(array));
    }

    BridgeResult send_event(const std::string& eventType, const std::string& data) override {
        if (!has_permission(BridgePermission::SendEvents)) return fail("permission:send_events");
        if (eventType.empty()) return fail("invalid:event_type");
        std::string parseError;
        (void)Engine::Json::parse(data, &parseError);
        if (!parseError.empty()) return fail("invalid:event_json:" + parseError);
        if (config_.event_sink) {
            std::string error;
            if (!config_.event_sink(eventType, data, error)) return fail("runtime:event:" + error);
        }
        ++eventsSent_;
        return ok("{\"sequence\":" + std::to_string(eventsSent_) + "}");
    }

    BridgeResult list_component_types() const override {
        if (!has_permission(BridgePermission::ReadComponents)) return fail("permission:read_components");
        std::set<std::string> types{"Health", "Position"};
        for (const auto& [name, schema] : config_.component_schemas) { (void)schema; types.insert(name); }
        world_.for_each_entity([&](EntityId id) {
            world_.for_each_component(id, [&](const ComponentData& c) { types.insert(c.type); });
        });
        Engine::Json::Value array = Engine::Json::Value::make_array();
        for (const auto& type : types) array.push(type);
        return ok(Engine::Json::stringify(array));
    }

    BridgeResult get_component_schema(const std::string& type) const override {
        if (!has_permission(BridgePermission::ReadComponents)) return fail("permission:read_components");
        if (type == "Position" || type == "Transform") {
            return ok("{\"type\":\"object\",\"fields\":[\"x\",\"y\",\"z\"]}");
        }
        if (type == "Health") {
            return ok("{\"type\":\"object\",\"fields\":[\"value\",\"max\"]}");
        }
        const auto found = config_.component_schemas.find(type);
        return found == config_.component_schemas.end() ? fail("not_found:schema") : ok(found->second);
    }

    BatchResult execute_batch(const std::vector<BatchOperation>& operations) override {
        BatchResult result;
        result.results.reserve(operations.size());

        // Phase 1 is read-only and simulates entity/component existence in
        // operation order. Once it succeeds, every ECS operation below is on
        // a live handle with valid JSON/permissions, so the commit phase has
        // no expected refusal and never needs a rollback that would recycle
        // generational handles.
        std::unordered_map<std::string, std::unordered_set<std::string>> state;
        world_.for_each_entity([&](EntityId id) {
            const std::string handle = handle_string(id);
            auto& components = state[handle];
            components.insert("Position");
            components.insert("Transform");
            components.insert("Health");
            world_.for_each_component(id, [&](const ComponentData& component) {
                components.insert(component.type);
            });
        });

        auto preflight_fail = [&](std::string error) {
            result.all_ok = false;
            result.error = std::move(error);
            return result;
        };

        for (const auto& operation : operations) {
            switch (operation.kind) {
                case BatchOperation::Kind::ReadComponent: {
                    if (!has_permission(BridgePermission::ReadComponents))
                        return preflight_fail("permission:read_components");
                    const auto entity = state.find(operation.entity);
                    if (entity == state.end()) return preflight_fail("not_found:entity");
                    if (!entity->second.count(operation.component_type))
                        return preflight_fail("not_found:" + operation.component_type);
                    break;
                }
                case BatchOperation::Kind::WriteComponent: {
                    if (!has_permission(BridgePermission::WriteComponents))
                        return preflight_fail("permission:write_components");
                    auto entity = state.find(operation.entity);
                    if (entity == state.end()) return preflight_fail("not_found:entity");
                    const auto validation = validate_write(operation.entity, operation.component_type,
                                                           operation.data);
                    if (!validation.ok) return preflight_fail(validation.error);
                    entity->second.insert(operation.component_type == "Transform"
                                              ? "Position" : operation.component_type);
                    break;
                }
                case BatchOperation::Kind::RemoveComponent: {
                    if (!has_permission(BridgePermission::WriteComponents))
                        return preflight_fail("permission:write_components");
                    auto entity = state.find(operation.entity);
                    if (entity == state.end()) return preflight_fail("not_found:entity");
                    if (operation.component_type == "Position" || operation.component_type == "Transform" ||
                        operation.component_type == "Health")
                        return preflight_fail("invalid:builtin_component");
                    entity->second.erase(operation.component_type);
                    break;
                }
                case BatchOperation::Kind::SpawnEntity:
                    if (!has_permission(BridgePermission::SpawnEntities))
                        return preflight_fail("permission:spawn_entities");
                    break;
                case BatchOperation::Kind::DestroyEntity: {
                    if (!has_permission(BridgePermission::DestroyEntities))
                        return preflight_fail("permission:destroy_entities");
                    const auto entity = state.find(operation.entity);
                    if (entity == state.end()) return preflight_fail("not_found:entity");
                    state.erase(entity);
                    break;
                }
                case BatchOperation::Kind::SendEvent: {
                    if (!has_permission(BridgePermission::SendEvents))
                        return preflight_fail("permission:send_events");
                    if (operation.event_type.empty()) return preflight_fail("invalid:event_type");
                    std::string parseError;
                    (void)Engine::Json::parse(operation.data, &parseError);
                    if (!parseError.empty()) return preflight_fail("invalid:event_json:" + parseError);
                    // An arbitrary external callback cannot participate in an
                    // ECS transaction. Refuse before mutation rather than
                    // claiming atomicity across an irreversible side effect.
                    if (config_.event_sink) return preflight_fail("atomic:external_event_sink");
                    break;
                }
            }
        }

        // Phase 2 commits only operations proven valid above.
        for (const auto& operation : operations) {
            BridgeResult item;
            switch (operation.kind) {
                case BatchOperation::Kind::ReadComponent:
                    item = read_component(operation.entity, operation.component_type); break;
                case BatchOperation::Kind::WriteComponent:
                    item = write_component(operation.entity, operation.component_type, operation.data); break;
                case BatchOperation::Kind::RemoveComponent:
                    item = remove_component(operation.entity, operation.component_type); break;
                case BatchOperation::Kind::SpawnEntity:
                    item = spawn_entity(); break;
                case BatchOperation::Kind::DestroyEntity:
                    item = destroy_entity(operation.entity); break;
                case BatchOperation::Kind::SendEvent:
                    item = send_event(operation.event_type, operation.data); break;
            }
            result.results.push_back(item);
            if (!item.ok) {
                result.all_ok = false;
                result.error = "commit_invariant:" + item.error;
                return result;
            }
        }
        result.all_ok = true;
        return result;
    }

private:
    BridgeResult validate_write(const EntityHandle& entity, const std::string& type,
                                const std::string& data) const {
        if (type.empty()) return fail("invalid:component_type");
        EntityId id{};
        if (!parse_live(entity, id)) return fail("not_found:entity");
        std::string parseError;
        Engine::Json::Value patch;
        if (!parse_object(data, patch, parseError)) return fail("invalid:json:" + parseError);
        if (type == "Position" || type == "Transform") {
            for (const auto& [key, value] : patch.object()) {
                if (!value.is_number() || (key != "x" && key != "y" && key != "z"))
                    return fail("invalid:Position." + key);
            }
        } else if (type == "Health") {
            Health health{};
            if (!world_.get_health(id, health)) return fail("not_found:Health");
            for (const auto& [key, value] : patch.object()) {
                if (!value.is_number()) return fail("invalid:Health." + key);
                if (key == "value") health.value = static_cast<float>(value.as_number());
                else if (key == "max") health.max = static_cast<float>(value.as_number());
                else return fail("invalid:Health." + key);
            }
            if (health.max < 0.0f || health.value < 0.0f || health.value > health.max)
                return fail("invalid:Health.range");
        }
        return ok();
    }

    bool parse_live(const EntityHandle& handle, EntityId& id) const {
        return parse_handle(handle, id) && world_.alive(id);
    }
    bool has_component(EntityId id, const std::string& type) const {
        if (type == "Position" || type == "Transform" || type == "Health") return world_.alive(id);
        ComponentData data;
        return world_.get_component(id, type, data);
    }

    IEntityWorld& world_;
    EcsScriptingBridgeConfig config_;
    std::unordered_set<BridgePermission> permissions_;
    std::uint64_t eventsSent_{0};
};

} // namespace

std::unique_ptr<IScriptingBridge> create_ecs_scripting_bridge(
    engine::entity::IEntityWorld& world, EcsScriptingBridgeConfig config) {
    return std::make_unique<EcsScriptingBridge>(world, std::move(config));
}

} // namespace engine::scripting

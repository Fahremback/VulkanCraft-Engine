#include "engine/scripting/IVisualScriptRuntime.hpp"
#include "../scripting/ScriptGraphBridge.hpp"
#include "engine/entity/IEntityWorld.hpp"

#include <algorithm>
#include <map>
#include <optional>

namespace engine::scripting {
namespace {

class Runtime final : public IVisualScriptRuntime {
public:
    Runtime() {
        world_ = engine::entity::create_entity_world();
        if (!world_) return;
        EcsScriptingBridgeConfig config;
        config.context_id = "visual_script.runtime";
        config.permissions = {
            BridgePermission::ReadComponents, BridgePermission::WriteComponents,
            BridgePermission::SpawnEntities, BridgePermission::DestroyEntities,
            BridgePermission::SendEvents, BridgePermission::QueryEntities};
        config.component_schemas["VisualScript.Runtime"] =
            "{\"type\":\"object\",\"fields\":[\"event\",\"node\",\"steps\"]}";
        bridge_ = create_ecs_scripting_bridge(*world_, std::move(config));
        if (bridge_) {
            auto spawned = bridge_->spawn_entity();
            if (spawned.ok && spawned.data.size() >= 2 && spawned.data.front() == '"')
                runtimeEntity_ = spawned.data.substr(1, spawned.data.size() - 2);
        }
    }

    bool load(const IVisualScriptGraph& graph, std::string& error) override {
        const auto validation = graph.validate();
        if (!validation.valid()) {
            error = validation.errors.front();
            error_ = ScriptError{"invalid_graph", error, 0};
            return false;
        }
        graph_ = &graph;
        order_ = graph.topological_order();
        cursor_ = 0;
        paused_ = false;
        error_.reset();
        profile_ = {};
        error.clear();
        return true;
    }

    bool set_variable(const std::string& name, const PinValue& value, std::string& error) override {
        if (name.empty()) { error = "empty_variable"; return false; }
        variables_[name] = value;
        error.clear();
        return true;
    }

    const PinValue* variable(const std::string& name) const override {
        const auto found = variables_.find(name);
        return found == variables_.end() ? nullptr : &found->second;
    }

    bool emit_event(const std::string& name, const std::vector<PinValue>& args,
                    std::string& error) override {
        if (!graph_) { error = "runtime_not_loaded"; return false; }
        if (name.empty()) { error = "empty_event"; return false; }
        event_ = name;
        eventArgs_ = args;
        cursor_ = 0;
        paused_ = false;
        error.clear();
        return true;
    }

    bool step(std::uint32_t count, std::string& error) override {
        if (!graph_) { error = "runtime_not_loaded"; return false; }
        if (paused_ && cursor_ < order_.size() && at_breakpoint()) return true;
        paused_ = false;
        std::uint32_t executed = 0;
        while (cursor_ < order_.size() && executed < count) {
            if (at_breakpoint() && executed != 0) { paused_ = true; break; }
            const std::uint64_t nodeId = order_[cursor_];
            if (!execute_node(nodeId, error)) {
                error_ = ScriptError{"runtime_error", error, nodeId};
                paused_ = true;
                return false;
            }
            ++cursor_;
            ++executed;
            ++profile_.steps;
            ++profile_.elapsed_ticks;
        }
        if (cursor_ >= order_.size()) paused_ = true;
        error.clear();
        return true;
    }

    bool run_until_yield(std::string& error) override {
        const auto remaining = order_.size() > cursor_ ? order_.size() - cursor_ : 0;
        return step(static_cast<std::uint32_t>(remaining), error);
    }

    bool resume(std::string& error) override {
        if (!graph_) { error = "runtime_not_loaded"; return false; }
        paused_ = false;
        return run_until_yield(error);
    }

    bool add_breakpoint(const ScriptBreakpoint& breakpoint, std::string& error) override {
        if (!graph_ || !graph_->get_node(breakpoint.node_id)) { error = "node_not_found"; return false; }
        const auto found = std::find_if(breakpoints_.begin(), breakpoints_.end(),
                                        [&](const auto& item) { return item.node_id == breakpoint.node_id; });
        if (found == breakpoints_.end()) breakpoints_.push_back(breakpoint);
        else *found = breakpoint;
        error.clear();
        return true;
    }

    bool remove_breakpoint(std::uint64_t id) override {
        const auto it = std::remove_if(breakpoints_.begin(), breakpoints_.end(),
                                       [&](const auto& item) { return item.node_id == id; });
        const bool removed = it != breakpoints_.end();
        breakpoints_.erase(it, breakpoints_.end());
        return removed;
    }

    std::vector<ScriptBreakpoint> breakpoints() const override { return breakpoints_; }
    std::vector<std::string> variables() const override {
        std::vector<std::string> out;
        for (const auto& [name, value] : variables_) { (void)value; out.push_back(name); }
        return out;
    }
    const ScriptError* last_error() const override { return error_ ? &*error_ : nullptr; }
    ScriptProfile profile() const override { return profile_; }
    bool paused() const noexcept override { return paused_; }
    const IScriptingBridge* scripting_bridge() const noexcept override { return bridge_.get(); }

private:
    static std::string pin_string(const NodeInstance& node, const std::string& name) {
        const auto found = node.inputValues.find(name);
        return found == node.inputValues.end() ? std::string{} : found->second.stringVal;
    }

    bool execute_node(std::uint64_t nodeId, std::string& error) {
        const NodeInstance* node = graph_->get_node(nodeId);
        if (!node) { error = "node_not_found"; return false; }
        if (!bridge_) { error = "scripting_bridge_unavailable"; return false; }

        if (node->type_name == "ECS.WriteComponent") {
            const auto result = bridge_->write_component(pin_string(*node, "Entity"),
                                                         pin_string(*node, "Component"),
                                                         pin_string(*node, "Data"));
            if (!result.ok) { error = result.error; return false; }
            return true;
        }
        if (node->type_name == "ECS.SendEvent") {
            const auto result = bridge_->send_event(pin_string(*node, "Event"),
                                                    pin_string(*node, "Data"));
            if (!result.ok) { error = result.error; return false; }
            return true;
        }

        if (!runtimeEntity_.empty()) {
            const std::string data = "{\"event\":\"" + event_ + "\",\"node\":" +
                                     std::to_string(nodeId) + ",\"steps\":" +
                                     std::to_string(profile_.steps + 1) + "}";
            const auto result = bridge_->write_component(runtimeEntity_, "VisualScript.Runtime", data);
            if (!result.ok) { error = result.error; return false; }
        }
        return true;
    }

    bool at_breakpoint() const {
        if (cursor_ >= order_.size()) return false;
        return std::any_of(breakpoints_.begin(), breakpoints_.end(), [&](const auto& breakpoint) {
            return breakpoint.enabled && breakpoint.node_id == order_[cursor_];
        });
    }

    const IVisualScriptGraph* graph_{nullptr};
    std::vector<std::uint64_t> order_;
    std::size_t cursor_{0};
    bool paused_{false};
    std::map<std::string, PinValue> variables_;
    std::vector<ScriptBreakpoint> breakpoints_;
    std::optional<ScriptError> error_;
    ScriptProfile profile_{};
    std::string event_;
    std::vector<PinValue> eventArgs_;
    std::unique_ptr<engine::entity::IEntityWorld> world_;
    std::unique_ptr<IScriptingBridge> bridge_;
    std::string runtimeEntity_;
};

} // namespace

std::unique_ptr<IVisualScriptRuntime> create_visual_script_runtime() {
    return std::make_unique<Runtime>();
}

} // namespace engine::scripting

#include "engine/voxel/IBlockEntityScripting.hpp"

#include "engine/core/serialization/JsonMini.hpp"
#include "engine/scripting/ScriptRuntime.hpp"
#include "engine/voxel/IVoxelBlockEntity.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace engine {
namespace voxel {
namespace {

// glm::ivec3 lacks operator<; provide a lexicographic comparator for
// deterministic ordered containers (set/map) used by the bridge.
struct Ivec3Less {
    bool operator()(const glm::ivec3& a, const glm::ivec3& b) const noexcept {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }
};

// The canonical node-kind names (must match ScriptRuntime's script_kind_name
// table — the .script JSON schema is defined by ScriptGraphAsset::save).
Engine::ScriptNodeKind script_kind_from_name(const std::string& name) {
    if (name == "Event") return Engine::ScriptNodeKind::Event;
    if (name == "ConstantFloat") return Engine::ScriptNodeKind::ConstantFloat;
    if (name == "ConstantInteger") return Engine::ScriptNodeKind::ConstantInteger;
    if (name == "ConstantBoolean") return Engine::ScriptNodeKind::ConstantBoolean;
    if (name == "GetVariable") return Engine::ScriptNodeKind::GetVariable;
    if (name == "SetVariable") return Engine::ScriptNodeKind::SetVariable;
    if (name == "AddFloat") return Engine::ScriptNodeKind::AddFloat;
    if (name == "SubtractFloat") return Engine::ScriptNodeKind::SubtractFloat;
    if (name == "MultiplyFloat") return Engine::ScriptNodeKind::MultiplyFloat;
    if (name == "Branch") return Engine::ScriptNodeKind::Branch;
    if (name == "Wait") return Engine::ScriptNodeKind::Wait;
    if (name == "EmitEvent") return Engine::ScriptNodeKind::EmitEvent;
    if (name == "Return") return Engine::ScriptNodeKind::Return;
    if (name == "Function") return Engine::ScriptNodeKind::Function;
    if (name == "FunctionCall") return Engine::ScriptNodeKind::FunctionCall;
    if (name == "Log") return Engine::ScriptNodeKind::Log;
    if (name == "Scope") return Engine::ScriptNodeKind::Scope;
    if (name == "ScopeEnd") return Engine::ScriptNodeKind::ScopeEnd;
    return Engine::ScriptNodeKind::Return;
}

// {"type":"float","value":1.0} -> ScriptValue.
Engine::ScriptValue script_literal_from_json(const Engine::Json::Value& value) {
    const std::string type =
        value.find("type") ? value.find("type")->as_string() : std::string("none");
    const Engine::Json::Value* raw = value.find("value");
    if (type == "bool" && raw) return Engine::ScriptValue(raw->as_bool(false));
    if (type == "int" && raw) return Engine::ScriptValue(raw->as_int(0));
    if (type == "float" && raw) return Engine::ScriptValue(raw->as_number(0.0));
    if (type == "string" && raw) return Engine::ScriptValue(raw->as_string());
    if (type == "uuid" && raw) {
        return Engine::ScriptValue(Engine::UUID::from_string(raw->as_string()));
    }
    return Engine::ScriptValue{};
}

// Parse an in-memory .script JSON document into a ScriptGraphAsset.
bool parse_script_graph(const std::string& json, Engine::ScriptGraphAsset& out) {
    std::string error;
    const Engine::Json::Value root = Engine::Json::parse(json, &error);
    if (root.is_null()) return false;
    out.id = Engine::UUID::from_string(
        root.find("id") ? root.find("id")->as_string() : std::string());
    if (const Engine::Json::Value* nameValue = root.find("name")) {
        out.name = nameValue->as_string("Script Graph");
    }
    out.nodes.clear();
    out.links.clear();
    if (const Engine::Json::Value* nodesValue = root.find("nodes")) {
        for (const Engine::Json::Value& n : nodesValue->array()) {
            Engine::TypedScriptNode node;
            node.id = Engine::UUID::from_string(
                n.find("id") ? n.find("id")->as_string() : std::string());
            node.kind = script_kind_from_name(
                n.find("kind") ? n.find("kind")->as_string() : std::string("Return"));
            if (const Engine::Json::Value* e = n.find("event")) node.event = e->as_string();
            if (const Engine::Json::Value* v = n.find("variable")) node.variable = v->as_string();
            if (const Engine::Json::Value* l = n.find("literal")) {
                node.literal = script_literal_from_json(*l);
            }
            out.nodes.push_back(std::move(node));
        }
    }
    if (const Engine::Json::Value* linksValue = root.find("links")) {
        for (const Engine::Json::Value& l : linksValue->array()) {
            Engine::ScriptNodeLink link;
            link.from = Engine::UUID::from_string(
                l.find("from") ? l.find("from")->as_string() : std::string());
            link.to = Engine::UUID::from_string(
                l.find("to") ? l.find("to")->as_string() : std::string());
            if (link.from.is_valid() && link.to.is_valid()) out.links.push_back(link);
        }
    }
    return true;
}

// One running script instance: its VM plus the per-instance seed state.
struct Instance {
    Engine::ScriptVM vm;
    bool loaded{ false };
};

constexpr std::size_t kInstructionBudget = 256;  // per instance per tick

}  // namespace

class BlockEntityScripting final : public IBlockEntityScripting {
public:
    explicit BlockEntityScripting(IVoxelWorld& world) : world_(world) {
        world_.set_block_entity_listener(
            [this](const BlockEntityEvent& event) { on_event(event); });
    }

    ~BlockEntityScripting() override {
        world_.set_block_entity_listener({});
    }

    bool register_script(const BlockEntityScriptSpec& spec) override {
        Engine::ScriptGraphAsset graph;
        if (!parse_script_graph(spec.graphJson, graph)) {
            lastError_ = "invalid script JSON for '" + spec.scriptId + "'";
            return false;
        }
        Engine::ScriptCompileResult result =
            Engine::ScriptCompiler::compile(graph);
        if (!result) {
            lastError_ = result.diagnostics.empty()
                ? ("script compile failed for '" + spec.scriptId + "'")
                : result.diagnostics.front().message;
            return false;
        }
        programs_[spec.scriptId] = std::move(result.program);
        lastError_.clear();
        return true;
    }

    bool unregister_script(const std::string& scriptId) override {
        return programs_.erase(scriptId) > 0;
    }

    bool has_script(const std::string& scriptId) const override {
        return programs_.find(scriptId) != programs_.end();
    }

    void tick(double dt) override {
        const std::vector<glm::ivec3> positions(tracked_.begin(), tracked_.end());
        for (const glm::ivec3& pos : positions) {
            std::shared_ptr<IVoxelBlockEntity> entity =
                world_.block_entity_at(pos.x, pos.y, pos.z);
            if (!entity) {
                tracked_.erase(pos);
                instances_.erase(pos);
                continue;
            }
            const std::string scriptId = entity->script_id();
            auto prog = programs_.find(scriptId);
            if (prog == programs_.end()) {
                instances_.erase(pos);
                continue;
            }
            Instance& inst = instances_[pos];
            if (!inst.loaded) {
                inst.vm.load(prog->second);
                inst.loaded = true;
                inst.vm.set_variable("x", static_cast<double>(pos.x));
                inst.vm.set_variable("y", static_cast<double>(pos.y));
                inst.vm.set_variable("z", static_cast<double>(pos.z));
                inst.vm.set_variable("scriptId", scriptId);
                if (inst.vm.has_event("init")) {
                    inst.vm.start_event("init");
                    const Engine::VMStatus initStatus =
                        inst.vm.run(static_cast<float>(dt), kInstructionBudget);
                    if (initStatus == Engine::VMStatus::Error) {
                        ++failedRuns_;
                        lastError_ = inst.vm.error();
                    }
                }
            }
            const Engine::VMStatus before = inst.vm.status();
            if (before == Engine::VMStatus::Idle ||
                before == Engine::VMStatus::Completed) {
                inst.vm.start_event("on_tick");
            }
            const Engine::VMStatus after =
                inst.vm.run(static_cast<float>(dt), kInstructionBudget);
            if (after == Engine::VMStatus::Completed) ++completedRuns_;
            if (after == Engine::VMStatus::Error) {
                ++failedRuns_;
                lastError_ = inst.vm.error();
            }
            std::vector<std::string> emitted;
            inst.vm.consume_emitted_events(emitted);
            for (const std::string& ev : emitted) inst.vm.start_event(ev);
        }
    }

    std::size_t active_instances() const override { return instances_.size(); }
    std::uint64_t completed_runs() const override { return completedRuns_; }
    std::uint64_t failed_runs() const override { return failedRuns_; }
    std::string last_error() const override { return lastError_; }

    bool script_variable(const glm::ivec3& position,
                         const std::string& name,
                         double& out) const override {
        auto it = instances_.find(position);
        if (it == instances_.end() || !it->second.loaded) return false;
        const Engine::ScriptValue* value = it->second.vm.variable(name);
        if (!value) return false;
        if (const double* d = std::get_if<double>(value)) {
            out = *d;
            return true;
        }
        if (const int64_t* i = std::get_if<int64_t>(value)) {
            out = static_cast<double>(*i);
            return true;
        }
        if (const bool* b = std::get_if<bool>(value)) {
            out = *b ? 1.0 : 0.0;
            return true;
        }
        return false;
    }

private:
    void on_event(const BlockEntityEvent& event) {
        const glm::ivec3 pos = event.position;
        if (event.kind == BlockEntityEvent::Kind::Attached) {
            tracked_.insert(pos);
            instances_.erase(pos);
        } else {
            tracked_.erase(pos);
            instances_.erase(pos);
        }
    }

    IVoxelWorld& world_;
    std::set<glm::ivec3, Ivec3Less> tracked_;
    std::map<glm::ivec3, Instance, Ivec3Less> instances_;
    std::map<std::string, Engine::ScriptProgram> programs_;
    std::string lastError_;
    std::uint64_t completedRuns_{ 0 };
    std::uint64_t failedRuns_{ 0 };
};

std::unique_ptr<IBlockEntityScripting> create_block_entity_scripting(
    IVoxelWorld& world) {
    return std::make_unique<BlockEntityScripting>(world);
}

}  // namespace voxel
}  // namespace engine

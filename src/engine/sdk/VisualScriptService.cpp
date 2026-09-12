#include "engine/scripting/IVisualScriptService.hpp"
#include "engine/semantic/ISemanticApi.hpp"
#include "engine/core/serialization/JsonMini.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

namespace engine::scripting {
namespace {

class Service final : public IVisualScriptService {
public:
    Service() : semantic_(engine::semantic::create_semantic_api()) {
        if (!semantic_) return;
        engine::semantic::SemanticKind kind;
        kind.name = "visual_script.service";
        kind.version = 1;
        engine::semantic::SemanticField stable;
        stable.name = "stable_id";
        stable.type = engine::semantic::SemanticFieldType::String;
        stable.required = true;
        engine::semantic::SemanticField version;
        version.name = "version";
        version.type = engine::semantic::SemanticFieldType::String;
        version.required = true;
        engine::semantic::SemanticField events;
        events.name = "events";
        events.type = engine::semantic::SemanticFieldType::StringArray;
        events.required = true;
        kind.fields = {stable, version, events};
        std::string ignored;
        semanticReady_ = semantic_->register_kind(kind, ignored);
    }

    bool register_service(const VisualScriptServiceDescriptor& descriptor,
                          std::string& error) override {
        if (descriptor.stable_id.empty()) { error = "stable_id_required"; return false; }
        if (descriptor.version.empty()) { error = "version_required"; return false; }
        if (descriptors_.count(descriptor.stable_id)) { error = "duplicate_service"; return false; }
        if (descriptor.events.empty()) { error = "events_required"; return false; }
        if (std::any_of(descriptor.events.begin(), descriptor.events.end(),
                        [](const auto& value) { return value.empty(); })) {
            error = "empty_event";
            return false;
        }
        const auto eventNames = sorted(descriptor.events);
        if (std::adjacent_find(eventNames.begin(), eventNames.end()) != eventNames.end()) {
            error = "duplicate_event";
            return false;
        }
        if (!semantic_ || !semanticReady_) {
            error = "semantic_api_unavailable";
            return false;
        }

        Engine::Json::Value document = Engine::Json::Value::make_object();
        document["stable_id"] = descriptor.stable_id;
        document["version"] = descriptor.version;
        Engine::Json::Value eventArray = Engine::Json::Value::make_array();
        for (const auto& event : descriptor.events) eventArray.push(event);
        document["events"] = std::move(eventArray);
        std::string canonical;
        if (!semantic_->validate("visual_script.service", Engine::Json::stringify(document),
                                 canonical, error)) {
            return false;
        }
        descriptors_.emplace(descriptor.stable_id, descriptor);
        canonicalDescriptors_.emplace(descriptor.stable_id, std::move(canonical));
        error.clear();
        return true;
    }

    bool attach(const std::string& id, IVisualScriptRuntime& runtime,
                std::string& error) override {
        const auto descriptor = descriptors_.find(id);
        if (descriptor == descriptors_.end()) { error = "service_not_found"; return false; }

        auto graph = std::make_unique<VisualScriptGraph>();
        for (const auto& event : descriptor->second.events) {
            NodeDef definition;
            definition.type_name = "Event." + event;
            definition.category = "Events";
            definition.description = "Service entry point " + event;
            definition.outputs.push_back(PinDef{"Event", PinType::Event, false, {}});
            if (!graph->register_node_type(definition, &error)) return false;
            NodeInstance instance;
            instance.type_name = definition.type_name;
            (void)graph->add_node(instance);
        }
        if (!runtime.load(*graph, error)) return false;
        graphs_[id] = std::move(graph);
        runtimes_[id] = &runtime;
        error.clear();
        return true;
    }

    bool dispatch(const std::string& id, const std::string& event,
                  const std::vector<PinValue>& args, std::string& error) override {
        const auto descriptor = descriptors_.find(id);
        const auto runtime = runtimes_.find(id);
        if (descriptor == descriptors_.end()) { error = "service_not_found"; return false; }
        if (runtime == runtimes_.end()) { error = "service_not_attached"; return false; }
        if (std::find(descriptor->second.events.begin(), descriptor->second.events.end(), event) ==
            descriptor->second.events.end()) {
            error = "event_not_declared";
            return false;
        }
        if (!runtime->second->emit_event(event, args, error)) return false;
        if (!runtime->second->run_until_yield(error)) return false;
        ++dispatchCount_[id];
        error.clear();
        return true;
    }

    std::vector<VisualScriptServiceDescriptor> services() const override {
        std::vector<VisualScriptServiceDescriptor> out;
        out.reserve(descriptors_.size());
        for (const auto& [id, descriptor] : descriptors_) { (void)id; out.push_back(descriptor); }
        return out;
    }

    void clear() override {
        runtimes_.clear();
        graphs_.clear();
        descriptors_.clear();
        canonicalDescriptors_.clear();
        dispatchCount_.clear();
    }

private:
    static std::vector<std::string> sorted(const std::vector<std::string>& source) {
        auto copy = source;
        std::sort(copy.begin(), copy.end());
        return copy;
    }

    std::map<std::string, VisualScriptServiceDescriptor> descriptors_;
    std::map<std::string, IVisualScriptRuntime*> runtimes_;
    std::map<std::string, std::unique_ptr<VisualScriptGraph>> graphs_;
    std::map<std::string, std::string> canonicalDescriptors_;
    std::map<std::string, std::uint64_t> dispatchCount_;
    std::unique_ptr<engine::semantic::ISemanticApi> semantic_;
    bool semanticReady_{false};
};

} // namespace

std::unique_ptr<IVisualScriptService> create_visual_script_service() {
    return std::make_unique<Service>();
}

} // namespace engine::scripting

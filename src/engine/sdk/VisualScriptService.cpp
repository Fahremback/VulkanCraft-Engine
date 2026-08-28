#include "engine/scripting/IVisualScriptService.hpp"
#include <algorithm>
#include <map>
#include <utility>
namespace engine::scripting { namespace {
class Service final : public IVisualScriptService {
    std::map<std::string, VisualScriptServiceDescriptor> descriptors_;
    std::map<std::string, IVisualScriptRuntime*> runtimes_;
public:
    bool register_service(const VisualScriptServiceDescriptor& d, std::string& e) override {
        if (d.stable_id.empty()) { e="stable_id_required"; return false; }
        if (descriptors_.count(d.stable_id)) { e="duplicate_service"; return false; }
        if (std::any_of(d.events.begin(), d.events.end(), [](const auto& x){ return x.empty(); })) { e="empty_event"; return false; }
        descriptors_.emplace(d.stable_id, d); return true;
    }
    bool attach(const std::string& id, IVisualScriptRuntime& runtime, std::string& e) override {
        if (!descriptors_.count(id)) { e="service_not_found"; return false; }
        runtimes_[id] = &runtime; return true;
    }
    bool dispatch(const std::string& id, const std::string& event, const std::vector<PinValue>& args, std::string& e) override {
        auto d=descriptors_.find(id); auto r=runtimes_.find(id);
        if (d==descriptors_.end()) { e="service_not_found"; return false; }
        if (r==runtimes_.end()) { e="service_not_attached"; return false; }
        if (std::find(d->second.events.begin(), d->second.events.end(), event)==d->second.events.end()) { e="event_not_declared"; return false; }
        return r->second->emit_event(event, args, e);
    }
    std::vector<VisualScriptServiceDescriptor> services() const override { std::vector<VisualScriptServiceDescriptor> o; for(const auto&[_,d]:descriptors_)o.push_back(d); return o; }
    void clear() override { descriptors_.clear(); runtimes_.clear(); }
}; }
std::unique_ptr<IVisualScriptService> create_visual_script_service(){return std::make_unique<Service>();}
}

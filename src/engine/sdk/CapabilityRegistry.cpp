#include "engine/capabilities/ICapabilityRegistry.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

namespace engine::capabilities {
namespace {
class Registry final : public ICapabilityRegistry {
    std::map<std::string, CapabilityDescriptor> entries_;
    static bool valid(const CapabilityDescriptor& d, std::string& e) {
        if (d.stable_id.empty()) { e = "stable_id_required"; return false; }
        if (d.display_name.empty()) { e = "display_name_required"; return false; }
        return true;
    }
    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) { if (c == '\\' || c == '"') out += '\\'; out += c; }
        return out;
    }
public:
    bool register_capability(const CapabilityDescriptor& d, std::string& e) override {
        if (!valid(d, e)) return false;
        if (entries_.count(d.stable_id)) { e = "duplicate_capability"; return false; }
        entries_.emplace(d.stable_id, d); return true;
    }
    bool unregister_capability(const std::string& id, std::string& e) override {
        if (!entries_.erase(id)) { e = "capability_not_found"; return false; }
        return true;
    }
    const CapabilityDescriptor* find(const std::string& id) const override {
        auto i = entries_.find(id); return i == entries_.end() ? nullptr : &i->second;
    }
    std::vector<CapabilityDescriptor> list() const override {
        std::vector<CapabilityDescriptor> out; for (const auto& [_, d] : entries_) out.push_back(d); return out;
    }
    std::vector<CapabilityDescriptor> list(CapabilityKind kind) const override {
        std::vector<CapabilityDescriptor> out; for (const auto& [_, d] : entries_) if (d.kind == kind) out.push_back(d); return out;
    }
    std::string to_json() const override {
        std::ostringstream out; out << "{\"version\":1,\"capabilities\":["; bool first = true;
        for (const auto& [_, d] : entries_) { if (!first) out << ','; first = false; out << "{\"id\":\"" << escape(d.stable_id) << "\",\"name\":\"" << escape(d.display_name) << "\",\"kind\":" << static_cast<unsigned>(d.kind) << ",\"version\":\"" << escape(d.version) << "\",\"alias\":\"" << escape(d.alias) << "\",\"deprecated\":" << (d.deprecated ? "true" : "false") << "}"; }
        out << "]}"; return out.str();
    }
    bool load_json(const std::string& json, std::string& e) override {
        if (json.find("\"version\":1") == std::string::npos || json.find("\"capabilities\":[") == std::string::npos) { e = "unsupported_capability_schema"; return false; }
        return true;
    }
    void clear() override { entries_.clear(); }
};
}
std::unique_ptr<ICapabilityRegistry> create_capability_registry() { return std::make_unique<Registry>(); }
}

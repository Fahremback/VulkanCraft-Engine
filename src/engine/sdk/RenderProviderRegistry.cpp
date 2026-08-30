#include "engine/rendering/IRenderProviderRegistry.hpp"

#include <algorithm>
#include <cstdint>

namespace Engine::Rendering {

namespace {

// Ordered map (insertion order preserved) with last-write-wins per system.
class RenderProviderRegistry final : public IRenderProviderRegistry {
public:
    void set(const RenderProviderEntry& entry) override {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const RenderProviderEntry& existing) {
                                   return existing.system == entry.system;
                               });
        if (it != entries_.end()) {
            *it = entry;
        } else {
            entries_.push_back(entry);
        }
    }

    const RenderProviderEntry* find(const std::string& system) const override {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const RenderProviderEntry& entry) {
                                         return entry.system == system;
                                     });
        return it == entries_.end() ? nullptr : &*it;
    }

    std::vector<RenderProviderEntry> all() const override { return entries_; }

    std::string to_json() const override {
        std::string json = "{\"systems\":[";
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (i != 0) json += ",";
            json += "{\"system\":\"" + entries_[i].system + "\","
                    "\"provider\":\"" + entries_[i].provider + "\","
                    "\"callSite\":\"" + entries_[i].callSite + "\","
                    "\"artifact\":\"" + entries_[i].artifact + "\","
                    "\"capability\":\"" + entries_[i].capability + "\"}";
        }
        json += "]}";
        return json;
    }

    void clear() override { entries_.clear(); }

private:
    std::vector<RenderProviderEntry> entries_;
};

}  // namespace

std::unique_ptr<IRenderProviderRegistry> create_render_provider_registry(
    std::string& /*errorOut*/) {
    return std::make_unique<RenderProviderRegistry>();
}

}  // namespace Engine::Rendering

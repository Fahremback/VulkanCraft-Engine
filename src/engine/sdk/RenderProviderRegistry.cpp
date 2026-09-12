#include "engine/rendering/IRenderProviderRegistry.hpp"
#include "../rendering/lighting/RenderProviderSelection.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Engine::Rendering {

namespace {

// Ordered map (insertion order preserved) with last-write-wins per system.
class RenderProviderRegistry final : public IRenderProviderRegistry {
public:
    void set(const RenderProviderEntry& entry) override {
        RenderProviderEntry resolved = entry;
        vc::rendering::provider_selection::Selection runtimeSelection;
        if (vc::rendering::provider_selection::find(entry.system,
                                                     runtimeSelection)) {
            resolved.provider = runtimeSelection.provider;
            resolved.capability = runtimeSelection.capability;
        }
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const RenderProviderEntry& existing) {
                                   return existing.system == resolved.system;
                               });
        if (it != entries_.end()) {
            *it = resolved;
        } else {
            entries_.push_back(std::move(resolved));
        }
    }

    const RenderProviderEntry* find(const std::string& system) const override {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const RenderProviderEntry& entry) {
                                         return entry.system == system;
                                     });
        if (it == entries_.end()) return nullptr;
        vc::rendering::provider_selection::Selection runtimeSelection;
        if (vc::rendering::provider_selection::find(system, runtimeSelection)) {
            it->provider = runtimeSelection.provider;
            it->capability = runtimeSelection.capability;
        }
        return &*it;
    }

    std::vector<RenderProviderEntry> all() const override {
        std::vector<RenderProviderEntry> resolved = entries_;
        for (RenderProviderEntry& entry : resolved) {
            vc::rendering::provider_selection::Selection runtimeSelection;
            if (vc::rendering::provider_selection::find(entry.system,
                                                         runtimeSelection)) {
                entry.provider = runtimeSelection.provider;
                entry.capability = runtimeSelection.capability;
            }
        }
        return resolved;
    }

    std::string to_json() const override {
        const auto resolved = all();
        std::string json = "{\"systems\":[";
        for (std::size_t i = 0; i < resolved.size(); ++i) {
            if (i != 0) json += ",";
            json += "{\"system\":\"" + resolved[i].system + "\","
                    "\"provider\":\"" + resolved[i].provider + "\","
                    "\"callSite\":\"" + resolved[i].callSite + "\","
                    "\"artifact\":\"" + resolved[i].artifact + "\","
                    "\"capability\":\"" + resolved[i].capability + "\"}";
        }
        json += "]}";
        return json;
    }

    void clear() override { entries_.clear(); }

private:
    mutable std::vector<RenderProviderEntry> entries_;
};

}  // namespace

std::unique_ptr<IRenderProviderRegistry> create_render_provider_registry(
    std::string& /*errorOut*/) {
    auto registry = std::make_unique<RenderProviderRegistry>();
    vc::rendering::provider_selection::Selection selected;
    if (vc::rendering::provider_selection::find("giProvider", selected)) {
        registry->set(RenderProviderEntry{
            "giProvider", selected.provider,
            "runtime provider selected by create_global_illumination_provider()",
            "GlobalIllumination.cpp", selected.capability });
    }
    if (vc::rendering::provider_selection::find("reflectionProvider", selected)) {
        registry->set(RenderProviderEntry{
            "reflectionProvider", selected.provider,
            "runtime provider selected by create_reflection_provider()",
            "GlobalIllumination.cpp", selected.capability });
    }
    return registry;
}

}  // namespace Engine::Rendering

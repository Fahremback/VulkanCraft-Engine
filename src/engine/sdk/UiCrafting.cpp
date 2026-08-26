// UiCrafting.cpp — the ONLY TU with the crafting-UI session behavior
// (agente 2 §A item 7). Presentation/decision ONLY: the UI session asks
// RecipeRegistry what is craftable (never re-implements satisfiability) and
// delegates the craft itself (atomic consume/produce). The craftable list is
// derived from RecipeRegistry::all_names() (sorted -> deterministic) filtered
// by recipes_for() — the registry's internal unordered_map iteration never
// leaks into observable order. No window/GPU.

#include "engine/ui/ICrafting.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace engine {
namespace ui {

bool CraftingSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported crafting spec version";
        return false;
    }
    if (seed == 0) {
        errorOut = "crafting seed must not be zero";
        return false;
    }
    return true;
}

std::string CraftingSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"station\":\"" << station
        << "\",\"seed\":" << seed << "}";
    return out.str();
}

bool CraftingSpec::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "crafting spec document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported crafting spec version";
        return false;
    }
    CraftingSpec candidate;
    candidate.version = version;
    candidate.station = sdk::json_string(doc, "station", "");
    candidate.seed = static_cast<std::uint64_t>(
        sdk::json_number(doc, "seed", 1.0));
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

class UiCraftingRuntime final : public IUiCrafting {
public:
    explicit UiCraftingRuntime(const CraftingSpec& spec) : spec_(spec) {}

    bool refresh(const engine::registry::Inventory& inv,
                 const engine::registry::RecipeRegistry& recipes,
                 const engine::registry::ItemRegistry& items,
                 std::string& errorOut) override {
        errorOut.clear();
        // Satisfiable set (registry authority; internal iteration order).
        std::set<std::string> satisfiable;
        for (const engine::registry::RecipeDefinition* recipe :
             recipes.recipes_for(inv, spec_.station, items)) {
            satisfiable.insert(recipe->namespaced());
        }
        // Observable order: sorted all_names() filtered by the set.
        craftable_.clear();
        for (const std::string& name : recipes.all_names()) {
            if (satisfiable.count(name)) craftable_.push_back(name);
        }
        // Keep the selection only while still craftable.
        if (!selected_.empty() &&
            !std::binary_search(craftable_.begin(), craftable_.end(), selected_)) {
            selected_.clear();
        }
        return true;
    }

    std::vector<std::string> craftable() const override { return craftable_; }

    bool select(const std::string& namespacedName,
                std::string& errorOut) override {
        errorOut.clear();
        if (namespacedName.empty()) {
            errorOut = "crafting selection must not be empty";
            return false;
        }
        if (!std::binary_search(craftable_.begin(), craftable_.end(),
                                namespacedName)) {
            errorOut = "recipe '" + namespacedName +
                       "' is not craftable with the current inventory";
            return false;
        }
        selected_ = namespacedName;
        return true;
    }

    std::string selected() const override { return selected_; }

    engine::registry::CraftResult craft(
        engine::registry::Inventory& inv,
        const engine::registry::RecipeRegistry& recipes,
        const engine::registry::ItemRegistry& items,
        std::string& errorOut) override {
        errorOut.clear();
        if (selected_.empty()) {
            errorOut = "no recipe selected";
            return engine::registry::CraftResult{};
        }
        const engine::registry::RecipeDefinition* recipe =
            recipes.find_by_name(selected_);
        if (recipe == nullptr) {
            errorOut = "selected recipe no longer exists: " + selected_;
            selected_.clear();
            return engine::registry::CraftResult{};
        }
        // Authority + atomicity live in RecipeRegistry::craft.
        lastResult_ = recipes.craft(inv, *recipe, spec_.station, items, spec_.seed);
        return lastResult_;
    }

    engine::registry::CraftResult last_result() const override {
        return lastResult_;
    }

    void set_seed(std::uint64_t seed) override {
        if (seed != 0) spec_.seed = seed;
    }

    const CraftingSpec& spec() const override { return spec_; }

private:
    CraftingSpec spec_;
    std::vector<std::string> craftable_;
    std::string selected_;
    engine::registry::CraftResult lastResult_;
};

}  // namespace

std::unique_ptr<IUiCrafting> create_ui_crafting(const CraftingSpec& spec,
                                                std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<UiCraftingRuntime>(spec);
}

}  // namespace ui
}  // namespace engine

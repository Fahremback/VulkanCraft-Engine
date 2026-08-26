#pragma once

// ICrafting (agente 2 §A item 7): the PUBLIC crafting-UI contract — the
// presentation/decision layer for crafting that DELEGATES to the
// authoritative RecipeRegistry (engine/registry). The registry owns what is
// craftable and how a craft consumes inputs (atomic); this contract owns the
// SESSION: which station is active, the deterministic order of craftable
// recipes, the selected recipe, and the seed driving byproduct rolls.
//   - REFRESH: recomputes craftable recipes for a given inventory +
//     registry + station by calling RecipeRegistry::recipes_for — the UI
//     never re-implements satisfiability.
//   - SELECT: picks a recipe by namespaced name; refused (no mutation) when
//     unknown or not craftable with the CURRENT inventory.
//   - CRAFT: delegates to RecipeRegistry::craft (validates station + inputs,
//     consumes exactly what the recipe needs, returns outputs/byproducts).
//     On failure the inventory is untouched (atomicity comes from the
//     registry). The result is kept as last_result() for display.
//   - DETERMINISM: same registry + inventory + station + seed -> identical
//     craftable order, selection decisions and craft outcomes (bit-exact).
//
// Self-contained (std + engine/ui + engine/registry only). The SDK adapter
// (src/engine/sdk/UiCrafting.cpp) is the ONLY TU with behavior; gameplay
// authority stays in RecipeRegistry.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/registry/Inventory.hpp"
#include "engine/registry/ItemRegistry.hpp"
#include "engine/registry/RecipeRegistry.hpp"

namespace engine {
namespace ui {

// Session configuration. A station of "" accepts recipes with no station
// requirement (or any station — matching RecipeRegistry::recipes_for).
struct CraftingSpec {
    int version{ 1 };
    std::string station;   // required station id ("" = any)
    uint64_t seed{ 1 };    // drives byproduct rolls (deterministic LCG)

    bool operator==(const CraftingSpec& other) const {
        return version == other.version && station == other.station &&
               seed == other.seed;
    }
    bool operator!=(const CraftingSpec& other) const { return !(*this == other); }

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

class IUiCrafting {
public:
    virtual ~IUiCrafting() = default;

    // Recomputes craftable recipes for the given inventory + registry (in
    // the registry's declaration order via all_names, filtered by
    // recipes_for). The selection is kept only when still craftable.
    virtual bool refresh(const engine::registry::Inventory& inv,
                         const engine::registry::RecipeRegistry& recipes,
                         const engine::registry::ItemRegistry& items,
                         std::string& errorOut) = 0;

    // Craftable recipe names (namespaced), registry declaration order.
    virtual std::vector<std::string> craftable() const = 0;

    // Selects a recipe by namespaced name. Refused (no mutation) when the
    // name is unknown or NOT craftable with the current inventory.
    virtual bool select(const std::string& namespacedName,
                        std::string& errorOut) = 0;

    // The currently selected recipe name ("" when none).
    virtual std::string selected() const = 0;

    // Crafts the selected recipe by DELEGATING to RecipeRegistry::craft
    // (atomic: on failure the inventory is untouched). Refused when nothing
    // is selected. The result is stored for last_result().
    virtual engine::registry::CraftResult craft(
        engine::registry::Inventory& inv,
        const engine::registry::RecipeRegistry& recipes,
        const engine::registry::ItemRegistry& items,
        std::string& errorOut) = 0;

    // The most recent craft result (ok=false when none yet).
    virtual engine::registry::CraftResult last_result() const = 0;

    // Replaces the byproduct-roll seed (also takes effect on the next craft).
    virtual void set_seed(std::uint64_t seed) = 0;

    virtual const CraftingSpec& spec() const = 0;
};

// Validates the spec and creates the session (rejected -> nullptr +
// errorOut).
std::unique_ptr<IUiCrafting> create_ui_crafting(const CraftingSpec& spec,
                                                std::string& errorOut);

}  // namespace ui
}  // namespace engine

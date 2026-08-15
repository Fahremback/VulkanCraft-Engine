#pragma once

// Data-driven recipe graph (SDK, META section 14). Recipes are assets: inputs
// (an item, a tag, or alternatives) with counts, optional station / time /
// energy / fuel / conditions, and outputs with byproducts. The graph resolves
// which recipes are craftable from an inventory, executes a craft
// authoritatively (consumes exactly the inputs, never duplicating or losing),
// and exposes dependency edges with cycle detection.
//
// Recipe JSON schema (object with "recipes" array, or a bare array):
// {
//   "namespace": "vulkancraft",
//   "recipes": [
//     {
//       "name": "stone_pickaxe",
//       "station": "vulkancraft:crafting_table",
//       "time": 2.0, "energy": 0.0, "fuel": "",
//       "conditions": ["unlocked:stone_age"],
//       "inputs": [ {"item":"vulkancraft:cobblestone","count":3},
//                   {"tag":"vulkancraft:sticks","count":2} ],
//       "outputs": [ {"item":"vulkancraft:stone_pickaxe","count":1} ]
//     },
//     {
//       "name": "smelt_iron",
//       "station": "vulkancraft:furnace", "energy": 1.0,
//       "inputs": [ {"item":"vulkancraft:iron_ore"} ],
//       "outputs": [ {"item":"vulkancraft:iron_ingot"} ],
//       "byproducts": [ {"item":"vulkancraft:slag","count":1,"chance":0.5} ]
//     }
//   ]
// }
// Inputs accept "item" (exact id), "tag" (any registered item carrying the
// tag) and "alternatives" (ids any of which satisfies). Conditions are
// validated structurally (non-empty strings); their semantics belong to the
// project. When the registry was constructed with an ItemRegistry, every item
// reference is validated at registration time — an unknown item/tag is
// refused, never guessed.

#include "engine/registry/Inventory.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace registry {

class ItemRegistry;

struct RecipeInput {
    std::string item;                       // namespaced id (either item or tag)
    std::string tag;                        // tag filter (either item or tag)
    std::vector<std::string> alternatives;  // any of these ids also satisfies
    int count{ 1 };
};

struct RecipeOutput {
    std::string item;          // namespaced id
    int count{ 1 };
    double chance{ 1.0 };      // (0, 1]; < 1 = probabilistic roll (byproduct)
    bool byproduct{ false };   // explicit byproduct even at chance 1.0
};

struct RecipeDefinition {
    std::string uuid;
    std::string ns{ "vulkancraft" };
    std::string name;  // required
    std::vector<RecipeInput> inputs;
    std::vector<RecipeOutput> outputs;
    std::string station;   // required station id (empty = any station)
    double time{ 1.0 };    // seconds
    double energy{ 0.0 };  // units (0 = free)
    std::string fuel;      // fuel item id (empty = none)
    std::vector<std::string> conditions;
    std::vector<std::string> tags;
    int32_t version{ 1 };

    std::string namespaced() const { return ns + ":" + name; }
};

struct CraftResult {
    bool ok{ false };
    std::string error;
    std::vector<RecipeOutput> outputs;    // guaranteed results of this craft
    std::vector<RecipeOutput> byproducts; // rolled (< 1 chance) or flagged
    double energy{ 0.0 };
    double time{ 0.0 };
};

class RecipeRegistry {
public:
    // `items` optional: when set, every item/tag reference in a recipe is
    // validated against it at registration time.
    explicit RecipeRegistry(const ItemRegistry* items = nullptr);

    bool register_recipe(RecipeDefinition definition, std::string& errorOut);
    bool load_from_json(const std::string& jsonText, std::string& errorOut);

    const RecipeDefinition* find_by_name(const std::string& namespaced) const;
    const RecipeDefinition* find_by_uuid(const std::string& uuid) const;
    std::size_t size() const;
    std::vector<std::string> all_names() const;

    // Recipes whose inputs are satisfiable from the inventory (counts across
    // slots, tag/alternative expansion). `station` filters out recipes that
    // require a different station.
    std::vector<const RecipeDefinition*> recipes_for(
        const Inventory& inv, const std::string& station,
        const ItemRegistry& items) const;

    // Authoritative craft: validates station + inputs first, consumes exactly
    // what the recipe needs from `source`, then returns outputs/byproducts.
    // `seed` drives byproduct rolls (deterministic LCG). On failure the
    // inventory is left untouched (atomicity).
    CraftResult craft(Inventory& source, const RecipeDefinition& recipe,
                      const std::string& station, const ItemRegistry& items,
                      uint64_t seed = 1) const;

    // Dependency graph: edge recipeA -> recipeB when A produces an input of B.
    std::vector<std::pair<std::string, std::string>> dependency_edges() const;
    // Cycle detection over the graph; false + path when a cycle exists.
    bool has_cycle(std::string& cyclePath) const;

private:
    bool input_satisfiable(const RecipeInput& input, const Inventory& inv,
                           const ItemRegistry& items) const;
    int count_satisfying(const RecipeInput& input, const Inventory& inv,
                         const ItemRegistry& items) const;
    bool input_matches(const RecipeInput& input, const ItemStack& stack,
                       const ItemRegistry& items) const;
    bool item_known(const std::string& namespaced, const ItemRegistry& items) const;
    bool tag_known(const std::string& tag, const ItemRegistry& items) const;

    const ItemRegistry* items_;
    std::unordered_map<std::string, RecipeDefinition> byName_;
    std::unordered_map<std::string, RecipeDefinition> byUuid_;
};

}  // namespace registry
}  // namespace engine

// RecipeRegistry.cpp — data-driven recipe graph (META section 14).
// Recipes are assets (JSON). Registration validates every item/tag reference
// against the ItemRegistry when one is provided — an unknown reference is
// refused, never guessed. Crafting is authoritative and atomic: inputs are
// validated before anything is consumed, and on failure the inventory is
// untouched.

#include "engine/registry/RecipeRegistry.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace engine {
namespace registry {

namespace {

bool parse_recipe_input(const sdk::JsonValue& object, RecipeInput& input,
                        std::string& errorOut) {
    input.item = sdk::json_string(object, "item", "");
    input.tag = sdk::json_string(object, "tag", "");
    input.count = static_cast<int>(sdk::json_number(object, "count", 1.0));
    if (const sdk::JsonValue* alts = object.field("alternatives");
        alts != nullptr && alts->is_array()) {
        for (const sdk::JsonValue& alt : alts->array) {
            if (alt.is_string()) input.alternatives.push_back(alt.string);
        }
    }
    if (input.item.empty() && input.tag.empty()) {
        errorOut = "recipe input requires 'item' or 'tag'";
        return false;
    }
    if (input.count < 1) {
        errorOut = "recipe input count must be >= 1";
        return false;
    }
    return true;
}

bool parse_recipe_output(const sdk::JsonValue& object, RecipeOutput& output,
                         std::string& errorOut) {
    output.item = sdk::json_string(object, "item", "");
    output.count = static_cast<int>(sdk::json_number(object, "count", 1.0));
    output.chance = sdk::json_number(object, "chance", 1.0);
    output.byproduct = sdk::json_bool(object, "byproduct", false);
    if (output.item.empty()) {
        errorOut = "recipe output requires 'item'";
        return false;
    }
    if (output.count < 1) {
        errorOut = "recipe output count must be >= 1";
        return false;
    }
    if (output.chance <= 0.0 || output.chance > 1.0) {
        errorOut = "recipe output chance must be in (0, 1]";
        return false;
    }
    return true;
}

}  // namespace

RecipeRegistry::RecipeRegistry(const ItemRegistry* items) : items_(items) {}

bool RecipeRegistry::item_known(const std::string& namespaced,
                                const ItemRegistry& items) const {
    return items.find_by_name(namespaced) != nullptr;
}

bool RecipeRegistry::tag_known(const std::string& tag,
                               const ItemRegistry& items) const {
    for (const std::string& name : items.all_names()) {
        const ItemDefinition* def = items.find_by_name(name);
        if (def == nullptr) continue;
        for (const std::string& have : def->tags) {
            if (have == tag) return true;
        }
    }
    return false;
}

bool RecipeRegistry::register_recipe(RecipeDefinition definition,
                                     std::string& errorOut) {
    if (definition.name.empty()) {
        errorOut = "recipe 'name' is required";
        return false;
    }
    if (definition.ns.empty()) {
        errorOut = "recipe '" + definition.name + "': 'namespace' cannot be empty";
        return false;
    }
    if (definition.inputs.empty()) {
        errorOut = "recipe '" + definition.namespaced() + "' needs at least one input";
        return false;
    }
    if (definition.outputs.empty()) {
        errorOut = "recipe '" + definition.namespaced() + "' needs at least one output";
        return false;
    }
    if (!definition.station.empty() &&
        definition.station.find(':') == std::string::npos) {
        errorOut = "recipe '" + definition.namespaced() +
                   "': station must be namespaced (ns:name)";
        return false;
    }
    if (definition.time < 0.0 || definition.energy < 0.0) {
        errorOut = "recipe '" + definition.namespaced() +
                   "': time and energy cannot be negative";
        return false;
    }
    if (items_ != nullptr) {
        for (const RecipeInput& input : definition.inputs) {
            if (!input.item.empty() && !item_known(input.item, *items_)) {
                errorOut = "recipe '" + definition.namespaced() +
                           "': unknown input item '" + input.item + "'";
                return false;
            }
            if (!input.tag.empty() && !tag_known(input.tag, *items_)) {
                errorOut = "recipe '" + definition.namespaced() +
                           "': no registered item carries tag '" + input.tag + "'";
                return false;
            }
            for (const std::string& alt : input.alternatives) {
                if (!item_known(alt, *items_)) {
                    errorOut = "recipe '" + definition.namespaced() +
                               "': unknown alternative item '" + alt + "'";
                    return false;
                }
            }
        }
        for (const RecipeOutput& output : definition.outputs) {
            if (!item_known(output.item, *items_)) {
                errorOut = "recipe '" + definition.namespaced() +
                           "': unknown output item '" + output.item + "'";
                return false;
            }
        }
        if (!definition.fuel.empty() && !item_known(definition.fuel, *items_)) {
            errorOut = "recipe '" + definition.namespaced() +
                       "': unknown fuel item '" + definition.fuel + "'";
            return false;
        }
    }

    RecipeDefinition resolved = definition;
    const std::string namespaced = resolved.namespaced();
    resolved.uuid = sdk::uuid_or_derived(resolved.uuid, namespaced);
    if (byName_.count(namespaced) != 0) {
        errorOut = "recipe '" + namespaced + "' is already registered";
        return false;
    }
    if (byUuid_.count(resolved.uuid) != 0) {
        errorOut = "recipe uuid '" + resolved.uuid + "' is already registered";
        return false;
    }
    byName_.emplace(namespaced, resolved);
    byUuid_.emplace(resolved.uuid, resolved);
    errorOut.clear();
    return true;
}

bool RecipeRegistry::load_from_json(const std::string& jsonText,
                                    std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut)) return false;

    std::vector<sdk::JsonValue> entries;
    std::string defaultNs = "vulkancraft";
    if (root.is_object() && root.field("recipes") != nullptr) {
        const sdk::JsonValue* recipes = root.field("recipes");
        if (!recipes->is_array()) {
            errorOut = "recipe asset 'recipes' must be an array";
            return false;
        }
        defaultNs = sdk::json_string(root, "namespace", "vulkancraft");
        entries = recipes->array;
    } else if (root.is_array()) {
        entries = root.array;
    } else if (root.is_object()) {
        entries.push_back(root);
    } else {
        errorOut = "recipe asset must be an object or an array";
        return false;
    }

    bool anyRegistered = false;
    for (const sdk::JsonValue& entry : entries) {
        if (!entry.is_object()) continue;
        RecipeDefinition recipe;
        recipe.ns = sdk::json_string(entry, "namespace", defaultNs);
        recipe.name = sdk::json_string(entry, "name", "");
        recipe.uuid = sdk::json_string(entry, "id", "");
        recipe.station = sdk::json_string(entry, "station", "");
        recipe.time = sdk::json_number(entry, "time", 1.0);
        recipe.energy = sdk::json_number(entry, "energy", 0.0);
        recipe.fuel = sdk::json_string(entry, "fuel", "");
        recipe.conditions = sdk::json_string_array(entry, "conditions");
        recipe.tags = sdk::json_string_array(entry, "tags");
        recipe.version = static_cast<int32_t>(sdk::json_number(entry, "version", 1.0));

        bool valid = true;
        std::string entryError;
        if (const sdk::JsonValue* inputs = entry.field("inputs");
            inputs != nullptr && inputs->is_array()) {
            for (const sdk::JsonValue& inputObj : inputs->array) {
                if (!inputObj.is_object()) continue;
                RecipeInput input;
                if (!parse_recipe_input(inputObj, input, entryError)) {
                    valid = false;
                    break;
                }
                recipe.inputs.push_back(std::move(input));
            }
        }
        if (const sdk::JsonValue* outputs = entry.field("outputs");
            valid && outputs != nullptr && outputs->is_array()) {
            for (const sdk::JsonValue& outputObj : outputs->array) {
                if (!outputObj.is_object()) continue;
                RecipeOutput output;
                if (!parse_recipe_output(outputObj, output, entryError)) {
                    valid = false;
                    break;
                }
                recipe.outputs.push_back(std::move(output));
            }
        }
        if (const sdk::JsonValue* byproducts = entry.field("byproducts");
            valid && byproducts != nullptr && byproducts->is_array()) {
            for (const sdk::JsonValue& outputObj : byproducts->array) {
                if (!outputObj.is_object()) continue;
                RecipeOutput output;
                if (!parse_recipe_output(outputObj, output, entryError)) {
                    valid = false;
                    break;
                }
                output.byproduct = true;
                recipe.outputs.push_back(std::move(output));
            }
        }
        if (!valid) {
            errorOut += entryError + "; ";
            continue;
        }
        std::string regError;
        if (register_recipe(std::move(recipe), regError)) {
            anyRegistered = true;
        } else {
            errorOut += regError + "; ";
        }
    }
    if (!anyRegistered) {
        errorOut = "no valid recipe entries found: " + errorOut;
        return false;
    }
    return true;
}

const RecipeDefinition* RecipeRegistry::find_by_name(const std::string& namespaced) const {
    const auto found = byName_.find(namespaced);
    return found == byName_.end() ? nullptr : &found->second;
}

const RecipeDefinition* RecipeRegistry::find_by_uuid(const std::string& uuid) const {
    const auto found = byUuid_.find(uuid);
    return found == byUuid_.end() ? nullptr : &found->second;
}

std::size_t RecipeRegistry::size() const { return byName_.size(); }

std::vector<std::string> RecipeRegistry::all_names() const {
    std::vector<std::string> names;
    names.reserve(byName_.size());
    for (const auto& [name, recipe] : byName_) {
        (void)recipe;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool RecipeRegistry::input_matches(const RecipeInput& input, const ItemStack& stack,
                                   const ItemRegistry& items) const {
    if (stack.empty()) return false;
    if (!input.item.empty() && stack.item == input.item) return true;
    for (const std::string& alt : input.alternatives) {
        if (stack.item == alt) return true;
    }
    if (!input.tag.empty()) {
        const ItemDefinition* def = items.find_by_name(stack.item);
        if (def != nullptr) {
            for (const std::string& have : def->tags) {
                if (have == input.tag) return true;
            }
        }
    }
    return false;
}

int RecipeRegistry::count_satisfying(const RecipeInput& input, const Inventory& inv,
                                     const ItemRegistry& items) const {
    int total = 0;
    for (int s = 0; s < inv.slot_count(); ++s) {
        const ItemStack& stack = inv.get(s);
        if (input_matches(input, stack, items)) total += stack.count;
    }
    return total;
}

bool RecipeRegistry::input_satisfiable(const RecipeInput& input, const Inventory& inv,
                                       const ItemRegistry& items) const {
    return count_satisfying(input, inv, items) >= input.count;
}

std::vector<const RecipeDefinition*> RecipeRegistry::recipes_for(
    const Inventory& inv, const std::string& station,
    const ItemRegistry& items) const {
    std::vector<const RecipeDefinition*> matches;
    for (const auto& [name, recipe] : byName_) {
        (void)name;
        if (!recipe.station.empty() && recipe.station != station) continue;
        bool satisfiable = true;
        for (const RecipeInput& input : recipe.inputs) {
            if (!input_satisfiable(input, inv, items)) {
                satisfiable = false;
                break;
            }
        }
        if (satisfiable) matches.push_back(&recipe);
    }
    return matches;
}

CraftResult RecipeRegistry::craft(Inventory& source, const RecipeDefinition& recipe,
                                  const std::string& station,
                                  const ItemRegistry& items, uint64_t seed) const {
    CraftResult result;
    if (!recipe.station.empty() && recipe.station != station) {
        result.error = "recipe '" + recipe.namespaced() + "' requires station '" +
                       recipe.station + "' (present: '" + station + "')";
        return result;
    }
    // Validate first — never consume on failure (atomicity).
    for (const RecipeInput& input : recipe.inputs) {
        const int available = count_satisfying(input, source, items);
        if (available < input.count) {
            result.error = "recipe '" + recipe.namespaced() + "' needs " +
                           std::to_string(input.count) + " of '" +
                           (input.item.empty() ? input.tag : input.item) +
                           "' but only " + std::to_string(available) +
                           " available";
            return result;
        }
    }
    // Consume exactly what the recipe needs (availability was validated).
    for (const RecipeInput& input : recipe.inputs) {
        int needed = input.count;
        for (int s = 0; s < source.slot_count() && needed > 0; ++s) {
            const ItemStack& stack = source.get(s);
            if (!input_matches(input, stack, items)) continue;
            const int taken = source.consume(s, needed);
            needed -= taken;
        }
    }
    result.ok = true;
    result.time = recipe.time;
    result.energy = recipe.energy;
    for (const RecipeOutput& output : recipe.outputs) {
        const bool roll = output.chance < 1.0;
        if (roll) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            const double unit = static_cast<double>(seed >> 11) *
                                (1.0 / 9007199254740992.0);
            if (unit >= output.chance) continue;  // rolled out
        }
        if (output.byproduct || output.chance < 1.0) {
            result.byproducts.push_back(output);
        } else {
            result.outputs.push_back(output);
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> RecipeRegistry::dependency_edges() const {
    // item -> recipes that produce it
    std::unordered_map<std::string, std::vector<std::string>> producers;
    for (const auto& [name, recipe] : byName_) {
        (void)name;
        for (const RecipeOutput& output : recipe.outputs) {
            producers[output.item].push_back(recipe.namespaced());
        }
    }
    std::vector<std::pair<std::string, std::string>> edges;
    const auto add_edge = [&](std::unordered_set<std::string>& seen,
                              const std::string& recipeId,
                              const std::string& producer) {
        if (producer == recipeId) return;
        if (seen.insert(producer).second) {
            edges.emplace_back(recipeId, producer);
        }
    };
    for (const auto& [name, recipe] : byName_) {
        (void)name;
        std::unordered_set<std::string> seen;
        for (const RecipeInput& input : recipe.inputs) {
            if (!input.item.empty()) {
                const auto found = producers.find(input.item);
                if (found != producers.end()) {
                    for (const std::string& producer : found->second) {
                        add_edge(seen, recipe.namespaced(), producer);
                    }
                }
            }
            for (const std::string& alt : input.alternatives) {
                const auto altFound = producers.find(alt);
                if (altFound == producers.end()) continue;
                for (const std::string& producer : altFound->second) {
                    add_edge(seen, recipe.namespaced(), producer);
                }
            }
            // Tag inputs: any recipe producing an item that carries the tag
            // satisfies the input (resolved only when a registry is present).
            if (!input.tag.empty() && items_ != nullptr) {
                for (const auto& [outItem, producerList] : producers) {
                    const ItemDefinition* def = items_->find_by_name(outItem);
                    if (def == nullptr) continue;
                    bool carriesTag = false;
                    for (const std::string& have : def->tags) {
                        if (have == input.tag) {
                            carriesTag = true;
                            break;
                        }
                    }
                    if (!carriesTag) continue;
                    for (const std::string& producer : producerList) {
                        add_edge(seen, recipe.namespaced(), producer);
                    }
                }
            }
        }
    }
    return edges;
}

bool RecipeRegistry::has_cycle(std::string& cyclePath) const {
    // Iterative DFS over dependency edges with white/gray/black coloring.
    std::unordered_map<std::string, int> color;  // 0 white, 1 gray, 2 black
    std::unordered_map<std::string, std::string> parent;
    const auto edges = dependency_edges();
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& [from, to] : edges) adjacency[from].push_back(to);
    for (const auto& [name, recipe] : byName_) {
        (void)recipe;
        if (color[name] != 0) continue;
        std::vector<std::string> stack{ name };
        color[name] = 1;
        while (!stack.empty()) {
            const std::string current = stack.back();
            const auto found = adjacency.find(current);
            bool advanced = false;
            if (found != adjacency.end()) {
                for (const std::string& next : found->second) {
                    if (color[next] == 1) {
                        // Cycle: back-edge current -> next.
                        cyclePath = next;
                        std::string walk = current;
                        while (walk != next && parent.count(walk) != 0) {
                            cyclePath += " -> " + walk;
                            walk = parent[walk];
                        }
                        cyclePath += " -> " + next;
                        return true;
                    }
                    if (color[next] == 0) {
                        color[next] = 1;
                        parent[next] = current;
                        stack.push_back(next);
                        advanced = true;
                        break;
                    }
                }
            }
            if (advanced) continue;
            color[current] = 2;
            stack.pop_back();
        }
    }
    cyclePath.clear();
    return false;
}

}  // namespace registry
}  // namespace engine

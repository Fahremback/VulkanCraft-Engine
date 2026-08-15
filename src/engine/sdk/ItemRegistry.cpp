#include "engine/registry/ItemRegistry.hpp"

#include "RegistryJson.hpp"

#include <algorithm>

namespace engine {
namespace registry {

namespace {

bool add_item_from_json(ItemRegistry& registry, const sdk::JsonValue& object,
                        std::string& errorOut) {
    ItemDefinition definition;
    definition.ns = sdk::json_string(object, "namespace", "vulkancraft");
    definition.name = sdk::json_string(object, "name", "");
    definition.uuid = sdk::json_string(object, "id", "");
    definition.maxStack = static_cast<int>(sdk::json_number(object, "maxStack", 64.0));
    definition.durability = static_cast<int>(sdk::json_number(object, "durability", 0.0));
    definition.icon = sdk::json_string(object, "icon", "");
    definition.model = sdk::json_string(object, "model", "");
    definition.tags = sdk::json_string_array(object, "tags");
    definition.version = static_cast<int32_t>(sdk::json_number(object, "version", 1.0));
    return registry.register_item(std::move(definition), errorOut);
}

}  // namespace

ItemRegistry::ItemRegistry() {
    std::string error;
    fallback_.uuid = sdk::stable_uuid("vulkancraft:unknown");
    fallback_.ns = "vulkancraft";
    fallback_.name = "unknown";
    fallback_.maxStack = 1;
    fallback_.version = 1;
}

bool ItemRegistry::register_item(const ItemDefinition& definition, std::string& errorOut) {
    if (definition.name.empty()) {
        errorOut = "item 'name' is required";
        return false;
    }
    if (definition.ns.empty()) {
        errorOut = "item '" + definition.name + "': 'namespace' cannot be empty";
        return false;
    }
    if (definition.maxStack < 1 || definition.maxStack > 64) {
        errorOut = "item '" + definition.ns + ':' + definition.name +
                   "': maxStack must be in [1, 64]";
        return false;
    }

    ItemDefinition resolved = definition;
    const std::string namespaced = resolved.namespaced();
    resolved.uuid = sdk::uuid_or_derived(resolved.uuid, namespaced);
    if (byName_.count(namespaced) != 0) {
        errorOut = "item '" + namespaced + "' is already registered";
        return false;
    }
    if (byUuid_.count(resolved.uuid) != 0) {
        errorOut = "item uuid '" + resolved.uuid + "' is already registered";
        return false;
    }

    byUuid_.emplace(resolved.uuid, resolved);
    byName_.emplace(namespaced, resolved);
    errorOut.clear();
    return true;
}

bool ItemRegistry::load_from_json(const std::string& jsonText, std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut)) return false;

    if (root.is_array()) {
        bool anyRegistered = false;
        for (const sdk::JsonValue& entry : root.array) {
            if (!entry.is_object()) continue;
            std::string entryError;
            if (add_item_from_json(*this, entry, entryError)) {
                anyRegistered = true;
            } else {
                errorOut += entryError + "; ";
            }
        }
        if (!anyRegistered) {
            errorOut = "no valid item entries found: " + errorOut;
            return false;
        }
        return true;
    }

    if (!root.is_object()) {
        errorOut = "item asset must be an object or an array of objects";
        return false;
    }
    return add_item_from_json(*this, root, errorOut);
}

const ItemDefinition* ItemRegistry::find_by_uuid(const std::string& uuid) const {
    const auto found = byUuid_.find(uuid);
    return found == byUuid_.end() ? nullptr : &found->second;
}

const ItemDefinition* ItemRegistry::find_by_name(const std::string& namespacedName) const {
    const auto found = byName_.find(namespacedName);
    return found == byName_.end() ? nullptr : &found->second;
}

std::vector<std::string> ItemRegistry::all_names() const {
    std::vector<std::string> names;
    names.reserve(byName_.size());
    for (const auto& [name, definition] : byName_) {
        (void)definition;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace registry
}  // namespace engine

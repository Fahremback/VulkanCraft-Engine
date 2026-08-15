#include "engine/registry/BlockRegistry.hpp"

#include "RegistryJson.hpp"

#include "../../simulation/voxel/core/Voxel.hpp"

#include <algorithm>
#include <sstream>

namespace engine {
namespace registry {

namespace {

struct BuiltinBlock {
    const char* name;
    uint32_t builtinId;
    BlockClass blockClass;
    float hardness;
    float lightEmission;
    bool opaque;
    bool collidable;
};

// Canonical engine blocks (BlockType order). Names are snake_case; ids match
// the engine's baked-in BlockType enum so definitions can map to storage.
const BuiltinBlock kBuiltinBlocks[] = {
    { "air", 0, BlockClass::NonSolid, 0.0f, 0.0f, false, false },
    { "grass", 1, BlockClass::Solid, 0.6f, 0.0f, true, true },
    { "dirt", 2, BlockClass::Solid, 0.5f, 0.0f, true, true },
    { "stone", 3, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "bedrock", 4, BlockClass::Solid, -1.0f, 0.0f, true, true },
    { "sand", 5, BlockClass::Solid, 0.5f, 0.0f, true, true },
    { "wood", 6, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "leaves", 7, BlockClass::Transparent, 0.2f, 0.0f, false, true },
    { "planks", 8, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "cobblestone", 9, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "glass", 10, BlockClass::Transparent, 0.3f, 0.0f, false, true },
    { "bricks", 11, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "water", 12, BlockClass::Fluid, 100.0f, 0.0f, false, false },
    { "lava", 13, BlockClass::Fluid, 100.0f, 15.0f, false, false },
    { "clay", 14, BlockClass::Solid, 0.6f, 0.0f, true, true },
    { "coal_ore", 15, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "iron_ore", 16, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "gold_ore", 17, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "diamond_ore", 18, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "emerald_ore", 19, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "redstone_ore", 20, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "lapis_ore", 21, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "copper_ore", 22, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "birch_wood", 23, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "birch_leaves", 24, BlockClass::Transparent, 0.2f, 0.0f, false, true },
    { "birch_planks", 25, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "spruce_wood", 26, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "spruce_leaves", 27, BlockClass::Transparent, 0.2f, 0.0f, false, true },
    { "spruce_planks", 28, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "granite", 29, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "diorite", 30, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "andesite", 31, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "deepslate", 32, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "blackstone", 33, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "basalt", 34, BlockClass::Solid, 1.25f, 0.0f, true, true },
    { "netherrack", 35, BlockClass::Solid, 0.4f, 0.0f, true, true },
    { "end_stone", 36, BlockClass::Solid, 3.0f, 0.0f, true, true },
    { "obsidian", 37, BlockClass::Solid, 50.0f, 0.0f, true, true },
    { "sandstone", 38, BlockClass::Solid, 0.8f, 0.0f, true, true },
    { "terracotta", 39, BlockClass::Solid, 1.25f, 0.0f, true, true },
    { "glowstone", 40, BlockClass::Solid, 0.3f, 15.0f, false, true },
    { "sea_lantern", 41, BlockClass::Solid, 0.3f, 15.0f, false, true },
    { "magma_block", 42, BlockClass::Solid, 0.5f, 7.0f, false, true },
    { "crafting_table", 43, BlockClass::Solid, 2.5f, 0.0f, true, true },
    { "furnace", 44, BlockClass::Solid, 3.5f, 0.0f, true, true },
    { "chest", 45, BlockClass::Solid, 2.5f, 0.0f, true, true },
    { "tnt", 46, BlockClass::Solid, 0.0f, 0.0f, true, true },
    { "bookshelf", 47, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "prismarine", 48, BlockClass::Solid, 1.5f, 0.0f, true, true },
    { "mossy_cobblestone", 49, BlockClass::Solid, 2.0f, 0.0f, true, true },
    { "snow_block", 50, BlockClass::Solid, 0.2f, 0.0f, true, true },
};

BlockClass parse_block_class(const std::string& value) {
    if (value == "fluid") return BlockClass::Fluid;
    if (value == "transparent") return BlockClass::Transparent;
    if (value == "nonsolid" || value == "non_solid") return BlockClass::NonSolid;
    return BlockClass::Solid;
}

// Builds a definition from a parsed JSON object and registers it. Kept out of
// the public header because it touches the internal JSON helper.
bool add_block_from_json(BlockRegistry& registry, const sdk::JsonValue& object,
                         std::string& errorOut) {
    BlockDefinition definition;
    definition.ns = sdk::json_string(object, "namespace", "vulkancraft");
    definition.name = sdk::json_string(object, "name", "");
    definition.uuid = sdk::json_string(object, "id", "");
    definition.blockClass = parse_block_class(sdk::json_string(object, "class", "solid"));
    definition.hardness = static_cast<float>(sdk::json_number(object, "hardness", 1.0));
    definition.lightEmission = static_cast<float>(sdk::json_number(object, "lightEmission", 0.0));
    definition.lightAbsorption = static_cast<float>(sdk::json_number(object, "lightAbsorption", 1.0));
    definition.opaque = sdk::json_bool(object, "opaque", true);
    definition.collidable = sdk::json_bool(object, "collidable", true);
    definition.builtinId = static_cast<uint32_t>(sdk::json_number(object, "builtinId", 0.0));
    // Data-driven base color for JSON-only blocks: "color": [r, g, b] or
    // [r, g, b, a] (0..1). Missing/invalid entries keep the white default.
    const std::vector<double> color = sdk::json_number_array(object, "color");
    if (color.size() >= 3) {
        definition.color = glm::vec4(static_cast<float>(color[0]),
                                     static_cast<float>(color[1]),
                                     static_cast<float>(color[2]),
                                     color.size() >= 4 ? static_cast<float>(color[3]) : 1.0f);
    }
    // A storage mapping exists only when the asset explicitly declares a
    // builtinId; an absent field means "catalog-only" (world rejects setting
    // this block until the data-driven storage milestone lands).
    definition.hasBuiltinMapping = object.field("builtinId") != nullptr;
    definition.tags = sdk::json_string_array(object, "tags");
    definition.drops = sdk::json_string_array(object, "drops");
    definition.version = static_cast<int32_t>(sdk::json_number(object, "version", 1.0));
    if (definition.drops.empty() && !definition.name.empty()) {
        definition.drops.push_back("vulkancraft:" + definition.name);
    }
    return registry.register_block(std::move(definition), errorOut);
}

}  // namespace

BlockRegistry::BlockRegistry() {
    std::string error;
    fallback_.uuid = sdk::stable_uuid("vulkancraft:unknown");
    fallback_.ns = "vulkancraft";
    fallback_.name = "unknown";
    fallback_.blockClass = BlockClass::NonSolid;
    fallback_.opaque = false;
    fallback_.collidable = false;
    fallback_.hardness = -1.0f;
    fallback_.version = 1;

    for (const BuiltinBlock& builtin : kBuiltinBlocks) {
        BlockDefinition definition;
        definition.ns = "vulkancraft";
        definition.name = builtin.name;
        definition.blockClass = builtin.blockClass;
        definition.hardness = builtin.hardness;
        definition.lightEmission = builtin.lightEmission;
        definition.opaque = builtin.opaque;
        definition.collidable = builtin.collidable;
        definition.builtinId = builtin.builtinId;
        definition.hasBuiltinMapping = true;
        definition.tags = { "builtin" };
        definition.drops = { "vulkancraft:" + definition.name };
        definition.version = 1;
        add(std::move(definition), error);
    }
}

bool BlockRegistry::register_block(const BlockDefinition& definition, std::string& errorOut) {
    return add(definition, errorOut);
}

bool BlockRegistry::load_from_json(const std::string& jsonText, std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut)) return false;

    if (root.is_array()) {
        bool anyRegistered = false;
        int skipped = 0;
        for (const sdk::JsonValue& entry : root.array) {
            if (!entry.is_object()) {
                ++skipped;
                continue;
            }
            std::string entryError;
            if (!add_block_from_json(*this, entry, entryError)) {
                ++skipped;
                errorOut += entryError + "; ";
            } else {
                anyRegistered = true;
            }
        }
        if (!anyRegistered) {
            errorOut = "no valid block entries found: " + errorOut;
            return false;
        }
        return true;
    }

    if (!root.is_object()) {
        errorOut = "block asset must be an object or an array of objects";
        return false;
    }
    return add_block_from_json(*this, root, errorOut);
}

const BlockDefinition* BlockRegistry::find_by_uuid(const std::string& uuid) const {
    const auto found = byUuid_.find(uuid);
    return found == byUuid_.end() ? nullptr : &found->second;
}

const BlockDefinition* BlockRegistry::find_by_name(const std::string& namespacedName) const {
    const auto found = byName_.find(namespacedName);
    return found == byName_.end() ? nullptr : &found->second;
}

const BlockDefinition* BlockRegistry::find_by_builtin(uint32_t builtinId) const {
    const auto found = byBuiltin_.find(builtinId);
    return found == byBuiltin_.end() ? nullptr : &found->second;
}

std::vector<std::string> BlockRegistry::all_names() const {
    std::vector<std::string> names;
    names.reserve(byName_.size());
    for (const auto& [name, definition] : byName_) {
        (void)definition;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<BlockDefinition> BlockRegistry::all_definitions() const {
    std::vector<BlockDefinition> definitions;
    definitions.reserve(byUuid_.size());
    for (const auto& [uuid, definition] : byUuid_) {
        (void)uuid;
        definitions.push_back(definition);
    }
    // Deterministic order: the world allocates dynamic runtime ids from this
    // sequence, so the same registry content always maps to the same ids no
    // matter the JSON load order.
    std::sort(definitions.begin(), definitions.end(),
              [](const BlockDefinition& a, const BlockDefinition& b) {
                  return a.uuid < b.uuid;
              });
    return definitions;
}

bool BlockRegistry::add(BlockDefinition definition, std::string& errorOut) {
    if (definition.name.empty()) {
        errorOut = "block 'name' is required";
        return false;
    }
    if (definition.ns.empty()) {
        errorOut = "block '" + definition.name + "': 'namespace' cannot be empty";
        return false;
    }
    if (definition.hasBuiltinMapping &&
        definition.builtinId >= static_cast<uint32_t>(BlockType::Count)) {
        std::ostringstream message;
        message << "block '" << definition.ns << ':' << definition.name
                << "': builtinId " << definition.builtinId << " out of range [0, "
                << static_cast<uint32_t>(BlockType::Count) - 1 << ')';
        errorOut = message.str();
        return false;
    }

    const std::string namespaced = definition.namespaced();
    definition.uuid = sdk::uuid_or_derived(definition.uuid, namespaced);
    if (byName_.count(namespaced) != 0) {
        errorOut = "block '" + namespaced + "' is already registered";
        return false;
    }
    if (byUuid_.count(definition.uuid) != 0) {
        errorOut = "block uuid '" + definition.uuid + "' is already registered";
        return false;
    }
    if (definition.hasBuiltinMapping && byBuiltin_.count(definition.builtinId) != 0) {
        std::ostringstream message;
        message << "block '" << namespaced << "': builtinId " << definition.builtinId
                << " already used";
        errorOut = message.str();
        return false;
    }

    byUuid_.emplace(definition.uuid, definition);
    byName_.emplace(namespaced, definition);
    if (definition.hasBuiltinMapping) {
        byBuiltin_.emplace(definition.builtinId, definition);
    }
    errorOut.clear();
    return true;
}

}  // namespace registry
}  // namespace engine

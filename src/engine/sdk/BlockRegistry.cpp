#include "engine/registry/BlockRegistry.hpp"

#include "engine/registry/ItemRegistry.hpp"

#include "RegistryJson.hpp"

#include "../../simulation/voxel/core/Voxel.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

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

// Strict tool parsing: an explicit but unknown value is refused (all-or-
// nothing, never silently mapped to Any). Returns false on unknown.
bool parse_block_tool(const std::string& value, BlockTool& out) {
    if (value.empty() || value == "any") { out = BlockTool::Any; return true; }
    if (value == "pickaxe") { out = BlockTool::Pickaxe; return true; }
    if (value == "axe") { out = BlockTool::Axe; return true; }
    if (value == "shovel") { out = BlockTool::Shovel; return true; }
    if (value == "hoe") { out = BlockTool::Hoe; return true; }
    if (value == "sword") { out = BlockTool::Sword; return true; }
    return false;
}

// Strict shape parsing (FALTANTES item 2): explicit but unknown values are
// refused, never silently mapped to Full. Returns false on unknown.
bool parse_collision_shape(const std::string& value, CollisionShape& out) {
    if (value.empty() || value == "full") { out = CollisionShape::Full; return true; }
    if (value == "cross") { out = CollisionShape::Cross; return true; }
    if (value == "none") { out = CollisionShape::None; return true; }
    return false;
}

bool parse_selection_shape(const std::string& value, SelectionShape& out) {
    if (value.empty() || value == "full") { out = SelectionShape::Full; return true; }
    if (value == "cross") { out = SelectionShape::Cross; return true; }
    if (value == "none") { out = SelectionShape::None; return true; }
    return false;
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
    // Collision/selection shapes (FALTANTES item 2): strict enums — an
    // explicit unknown value refuses the whole asset.
    const std::string collisionShape = sdk::json_string(object, "collisionShape", "");
    if (!parse_collision_shape(collisionShape, definition.collisionShape)) {
        errorOut = "block '" + definition.ns + ':' + definition.name +
                   "': collisionShape must be full|cross|none";
        return false;
    }
    const std::string selectionShape = sdk::json_string(object, "selectionShape", "");
    if (!parse_selection_shape(selectionShape, definition.selectionShape)) {
        errorOut = "block '" + definition.ns + ':' + definition.name +
                   "': selectionShape must be full|cross|none";
        return false;
    }
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
    // Per-face material overrides (FALTANTES §14 "material por face"):
    // "faceTop"/"faceBottom"/"faceSide": [r, g, b(, a)] (0..1). Absent
    // entries leave the *_set flag false (base color is used for that face).
    const auto faceColor = [&](const char* key, glm::vec4& target, bool& set) {
        const std::vector<double> arr = sdk::json_number_array(object, key);
        if (arr.size() >= 3) {
            target = glm::vec4(static_cast<float>(arr[0]), static_cast<float>(arr[1]),
                               static_cast<float>(arr[2]),
                               arr.size() >= 4 ? static_cast<float>(arr[3]) : 1.0f);
            set = true;
        }
    };
    faceColor("faceTop", definition.faceTop, definition.faceTopSet);
    faceColor("faceBottom", definition.faceBottom, definition.faceBottomSet);
    faceColor("faceSide", definition.faceSide, definition.faceSideSet);
    definition.occludes = sdk::json_bool(object, "occlusion", true);
    definition.renderLayer = static_cast<int32_t>(sdk::json_number(object, "renderLayer", 0.0));
    // Named states + versioned transitions (FALTANTES item 5): each state is
    // an object with its own base color/face overrides/light; transitions are
    // {from, to, trigger} rules ("" = default state). Structural checks happen
    // in add(); unknown refs are refused there (all-or-nothing).
    const auto stateFaceColor = [&](const sdk::JsonValue& entry, const char* key,
                                    glm::vec4& target, bool& set) {
        const std::vector<double> arr = sdk::json_number_array(entry, key);
        if (arr.size() >= 3) {
            target = glm::vec4(static_cast<float>(arr[0]), static_cast<float>(arr[1]),
                               static_cast<float>(arr[2]),
                               arr.size() >= 4 ? static_cast<float>(arr[3]) : 1.0f);
            set = true;
        }
    };
    if (const sdk::JsonValue* states = object.field("states"); states && states->is_array()) {
        for (const sdk::JsonValue& entry : states->array) {
            if (!entry.is_object()) {
                errorOut = "block '" + definition.ns + ':' + definition.name +
                           "': each state must be an object";
                return false;
            }
            BlockState state;
            state.name = sdk::json_string(entry, "name", "");
            const std::vector<double> stateColor = sdk::json_number_array(entry, "color");
            if (stateColor.size() >= 3) {
                state.color = glm::vec4(static_cast<float>(stateColor[0]),
                                        static_cast<float>(stateColor[1]),
                                        static_cast<float>(stateColor[2]),
                                        stateColor.size() >= 4 ? static_cast<float>(stateColor[3]) : 1.0f);
            }
            stateFaceColor(entry, "faceTop", state.faceTop, state.faceTopSet);
            stateFaceColor(entry, "faceBottom", state.faceBottom, state.faceBottomSet);
            stateFaceColor(entry, "faceSide", state.faceSide, state.faceSideSet);
            state.lightEmission = static_cast<float>(sdk::json_number(entry, "lightEmission", 0.0));
            definition.states.push_back(std::move(state));
        }
    }
    if (const sdk::JsonValue* transitions = object.field("transitions");
        transitions && transitions->is_array()) {
        for (const sdk::JsonValue& entry : transitions->array) {
            if (!entry.is_object()) {
                errorOut = "block '" + definition.ns + ':' + definition.name +
                           "': each transition must be an object";
                return false;
            }
            BlockTransition transition;
            transition.fromState = sdk::json_string(entry, "from", "");
            transition.toState = sdk::json_string(entry, "to", "");
            transition.trigger = sdk::json_string(entry, "trigger", "");
            definition.transitions.push_back(std::move(transition));
        }
    }
    // Inline fluid binding (FALTANTES item 7): "fluid": { ... } declares the
    // fluid behavior this block drives; absent = no inline binding. Range
    // checks happen in add() (all-or-nothing, never clamped).
    if (const sdk::JsonValue* fluid = object.field("fluid"); fluid && fluid->is_object()) {
        FluidBinding binding;
        binding.declared = true;
        binding.viscosity = static_cast<float>(sdk::json_number(*fluid, "viscosity", 0.5));
        binding.density = static_cast<float>(sdk::json_number(*fluid, "density", 1.0));
        binding.range = static_cast<int>(sdk::json_number(*fluid, "range", 7.0));
        binding.tickInterval = static_cast<float>(sdk::json_number(*fluid, "tickInterval", 0.08));
        binding.source = sdk::json_bool(*fluid, "source", true);
        binding.falling = sdk::json_bool(*fluid, "falling", true);
        binding.evaporation = sdk::json_bool(*fluid, "evaporation", true);
        binding.damagePerTick = static_cast<float>(sdk::json_number(*fluid, "damagePerTick", 0.0));
        binding.compressible = sdk::json_bool(*fluid, "compressible", false);
        definition.fluid = binding;
    }
    // Sound/particle/tool/resistance/physics component (FALTANTES item 4).
    definition.soundPlace = sdk::json_string(object, "soundPlace", "");
    definition.soundBreak = sdk::json_string(object, "soundBreak", "");
    definition.soundStep = sdk::json_string(object, "soundStep", "");
    definition.soundHit = sdk::json_string(object, "soundHit", "");
    definition.particleBreak = sdk::json_string(object, "particleBreak", "");
    const std::string tool = sdk::json_string(object, "tool", "");
    if (!parse_block_tool(tool, definition.tool)) {
        errorOut = "block '" + definition.ns + ':' + definition.name +
                   "': tool must be any|pickaxe|axe|shovel|hoe|sword";
        return false;
    }
    definition.toolTier = static_cast<int>(sdk::json_number(object, "toolTier", 0.0));
    definition.resistance = static_cast<float>(sdk::json_number(object, "resistance", 0.0));
    definition.friction = static_cast<float>(sdk::json_number(object, "friction", 0.5));
    definition.bounciness = static_cast<float>(sdk::json_number(object, "bounciness", 0.0));
    definition.density = static_cast<float>(sdk::json_number(object, "density", 1.0));
    definition.flammability = static_cast<float>(sdk::json_number(object, "flammability", 0.0));
    definition.behaviorId = sdk::json_string(object, "behaviorId", "");
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

bool BlockRegistry::validate_item_references(const ItemRegistry& items,
                                             std::vector<std::string>& errorsOut) const {
    bool clean = true;
    for (const auto& [name, definition] : byName_) {
        // Builtin blocks are the engine's own contract (their drops resolve
        // against the engine's default item set); user-authored (catalog-only)
        // blocks are where a dangling drop is a real authoring bug.
        if (definition.hasBuiltinMapping) continue;
        for (const std::string& drop : definition.drops) {
            if (drop.find(':') == std::string::npos) {
                errorsOut.push_back("block '" + name + "': drop '" + drop +
                                   "' is not namespaced (ns:name)");
                clean = false;
            } else if (items.find_by_name(drop) == nullptr) {
                errorsOut.push_back("block '" + name + "': drop '" + drop +
                                   "' references an unknown item");
                clean = false;
            }
        }
    }
    return clean;
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
    if (definition.renderLayer < 0 || definition.renderLayer > 255) {
        std::ostringstream message;
        message << "block '" << definition.ns << ':' << definition.name
                << "': renderLayer " << definition.renderLayer << " out of range [0, 255]";
        errorOut = message.str();
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
    // FALTANTES item 9: drops reference items by namespaced id; an
    // unnamespaced drop cannot resolve and is refused (all-or-nothing).
    for (const std::string& drop : definition.drops) {
        if (drop.find(':') == std::string::npos) {
            errorOut = "block '" + definition.ns + ':' + definition.name +
                       "': drop '" + drop + "' must be namespaced (ns:name)";
            return false;
        }
    }
    // FALTANTES item 5: named states must be unique/non-empty, and every
    // transition must reference a known state (or the default ""), have a
    // non-empty trigger and no duplicate (from, trigger) rule (all-or-nothing).
    for (const BlockState& state : definition.states) {
        if (state.name.empty()) {
            errorOut = "block '" + definition.ns + ':' + definition.name +
                       "': state name cannot be empty";
            return false;
        }
    }
    for (std::size_t i = 0; i < definition.states.size(); ++i) {
        for (std::size_t j = i + 1; j < definition.states.size(); ++j) {
            if (definition.states[i].name == definition.states[j].name) {
                errorOut = "block '" + definition.ns + ':' + definition.name +
                           "': duplicate state '" + definition.states[i].name + "'";
                return false;
            }
        }
    }
    for (const BlockTransition& transition : definition.transitions) {
        if (transition.trigger.empty()) {
            errorOut = "block '" + definition.ns + ':' + definition.name +
                       "': transition trigger cannot be empty";
            return false;
        }
        if (transition.fromState == transition.toState) {
            errorOut = "block '" + definition.ns + ':' + definition.name +
                       "': transition '" + transition.trigger +
                       "' from and to state are identical";
            return false;
        }
        if (!transition.fromState.empty() && definition.state_index(transition.fromState) < 0) {
            errorOut = "block '" + definition.ns + ':' + definition.name +
                       "': transition '" + transition.trigger +
                       "' references unknown from-state '" + transition.fromState + "'";
            return false;
        }
        if (!transition.toState.empty() && definition.state_index(transition.toState) < 0) {
            errorOut = "block '" + definition.ns + ':' + definition.name +
                       "': transition '" + transition.trigger +
                       "' references unknown to-state '" + transition.toState + "'";
            return false;
        }
    }
    {
        std::set<std::pair<std::string, std::string>> rules;
        for (const BlockTransition& transition : definition.transitions) {
            if (!rules.insert({ transition.fromState, transition.trigger }).second) {
                errorOut = "block '" + definition.ns + ':' + definition.name +
                           "': duplicate transition (from '" + transition.fromState +
                           "', trigger '" + transition.trigger + "')";
                return false;
            }
        }
    }
    // FALTANTES item 7: inline fluid binding ranges are all-or-nothing (the
    // world table clamps as defense-in-depth, but a declared asset must be
    // within contract so round-trip stays bit-exact).
    if (definition.fluid.declared) {
        const std::string prefix =
            "block '" + definition.ns + ':' + definition.name + "': fluid ";
        if (definition.fluid.range < 1 || definition.fluid.range > 7) {
            errorOut = prefix + "range must be in [1, 7]";
            return false;
        }
        if (definition.fluid.viscosity < 0.0f || definition.fluid.viscosity > 1.0f) {
            errorOut = prefix + "viscosity must be in [0, 1]";
            return false;
        }
        if (definition.fluid.density < 0.0f) {
            errorOut = prefix + "density cannot be negative";
            return false;
        }
        if (definition.fluid.tickInterval < 0.0f) {
            errorOut = prefix + "tickInterval cannot be negative";
            return false;
        }
        if (definition.fluid.damagePerTick < 0.0f) {
            errorOut = prefix + "damagePerTick cannot be negative";
            return false;
        }
    }
    // FALTANTES item 4: sound/particle refs are namespaced, tool tier is
    // 0..4, and the physical properties are within contract (all-or-nothing).
    const std::string blockName = definition.ns + ':' + definition.name;
    for (const std::string* ref : { &definition.soundPlace, &definition.soundBreak,
                                    &definition.soundStep, &definition.soundHit,
                                    &definition.particleBreak }) {
        if (!ref->empty() && ref->find(':') == std::string::npos) {
            errorOut = "block '" + blockName + "': sound/particle reference '" +
                       *ref + "' must be namespaced (ns:name)";
            return false;
        }
    }
    if (definition.toolTier < 0 || definition.toolTier > 4) {
        errorOut = "block '" + blockName + "': toolTier must be in [0, 4]";
        return false;
    }
    if (definition.resistance < 0.0f) {
        errorOut = "block '" + blockName + "': resistance cannot be negative";
        return false;
    }
    if (definition.friction < 0.0f || definition.friction > 1.0f) {
        errorOut = "block '" + blockName + "': friction must be in [0, 1]";
        return false;
    }
    if (definition.bounciness < 0.0f || definition.bounciness > 1.0f) {
        errorOut = "block '" + blockName + "': bounciness must be in [0, 1]";
        return false;
    }
    if (definition.flammability < 0.0f || definition.flammability > 1.0f) {
        errorOut = "block '" + blockName + "': flammability must be in [0, 1]";
        return false;
    }
    if (definition.density <= 0.0f) {
        errorOut = "block '" + blockName + "': density must be positive";
        return false;
    }
    // FALTANTES item 6: a declared behavior reference must be namespaced
    // (validated, not resolved — resolution belongs to the abilities/block
    // entity milestone, mirroring ItemDefinition.behaviorId).
    if (!definition.behaviorId.empty() &&
        definition.behaviorId.find(':') == std::string::npos) {
        errorOut = "block '" + blockName +
                   "': behaviorId must be namespaced (ns:name)";
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

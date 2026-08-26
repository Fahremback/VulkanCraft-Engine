#include "engine/registry/FluidRegistry.hpp"

#include "engine/registry/BlockRegistry.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace engine {
namespace registry {

namespace {

bool add_fluid_from_json(FluidRegistry& registry, const sdk::JsonValue& object,
                         std::string& errorOut) {
    FluidDefinition definition;
    definition.block = sdk::json_string(object, "block", "");
    definition.uuid = sdk::json_string(object, "id", "");
    definition.viscosity = static_cast<float>(sdk::json_number(object, "viscosity", 0.5));
    definition.density = static_cast<float>(sdk::json_number(object, "density", 1.0));
    definition.range = static_cast<int>(sdk::json_number(object, "range", 7.0));
    definition.tickInterval = static_cast<float>(
        sdk::json_number(object, "tickInterval", 0.08));
    definition.source = sdk::json_bool(object, "source", true);
    definition.falling = sdk::json_bool(object, "falling", true);
    definition.evaporation = sdk::json_bool(object, "evaporation", true);
    definition.damagePerTick = static_cast<float>(
        sdk::json_number(object, "damagePerTick", 0.0));
    definition.compressible = sdk::json_bool(object, "compressible", false);
    definition.temperature = static_cast<float>(
        sdk::json_number(object, "temperature", 300.0));
    definition.solidifiesInto = sdk::json_string(object, "solidifiesInto", "");
    definition.ignites = sdk::json_bool(object, "ignites", false);
    definition.version = static_cast<int32_t>(sdk::json_number(object, "version", 1.0));
    const std::vector<double> color = sdk::json_number_array(object, "color");
    if (color.size() >= 3) {
        definition.color = glm::vec4(static_cast<float>(color[0]),
                                     static_cast<float>(color[1]),
                                     static_cast<float>(color[2]),
                                     color.size() >= 4 ? static_cast<float>(color[3]) : 1.0f);
    }
    return registry.register_fluid(std::move(definition), errorOut);
}

}  // namespace

FluidRegistry::FluidRegistry() = default;

bool FluidRegistry::register_fluid(const FluidDefinition& definition, std::string& errorOut) {
    return add(definition, errorOut);
}

bool FluidRegistry::load_from_json(const std::string& jsonText, std::string& errorOut) {
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
            if (!add_fluid_from_json(*this, entry, entryError)) {
                ++skipped;
                errorOut += entryError + "; ";
            } else {
                anyRegistered = true;
            }
        }
        if (!anyRegistered) {
            errorOut = "no valid fluid entries found: " + errorOut;
            return false;
        }
        return true;
    }

    if (!root.is_object()) {
        errorOut = "fluid asset must be an object or an array of objects";
        return false;
    }
    return add_fluid_from_json(*this, root, errorOut);
}

const FluidDefinition* FluidRegistry::find_by_uuid(const std::string& uuid) const {
    const auto found = byUuid_.find(uuid);
    return found == byUuid_.end() ? nullptr : &found->second;
}

const FluidDefinition* FluidRegistry::find_by_block(const std::string& namespacedBlock) const {
    const auto found = byBlock_.find(namespacedBlock);
    return found == byBlock_.end() ? nullptr : &found->second;
}

std::vector<FluidDefinition> FluidRegistry::all_definitions() const {
    std::vector<FluidDefinition> definitions;
    definitions.reserve(byUuid_.size());
    for (const auto& [uuid, definition] : byUuid_) {
        (void)uuid;
        definitions.push_back(definition);
    }
    std::sort(definitions.begin(), definitions.end(),
              [](const FluidDefinition& a, const FluidDefinition& b) {
                  return a.uuid < b.uuid;
              });
    return definitions;
}

bool FluidRegistry::validate_block_references(const BlockRegistry& blocks,
                                              std::vector<std::string>& errorsOut) const {
    bool clean = true;
    for (const auto& [block, definition] : byBlock_) {
        (void)definition;
        if (blocks.find_by_name(block) == nullptr) {
            errorsOut.push_back("fluid for block '" + block +
                                "' references an unknown block");
            clean = false;
        }
    }
    return clean;
}

bool FluidRegistry::add(FluidDefinition definition, std::string& errorOut) {
    if (definition.block.empty()) {
        errorOut = "fluid 'block' is required (the namespaced block name it drives)";
        return false;
    }
    definition.viscosity = std::clamp(definition.viscosity, 0.0f, 1.0f);
    definition.range = std::clamp(definition.range, 1, 7);
    definition.tickInterval = std::max(definition.tickInterval, 0.0f);
    definition.damagePerTick = std::max(definition.damagePerTick, 0.0f);
    if (!std::isfinite(definition.temperature)) {
        errorOut = "fluid '" + definition.block + "': temperature must be finite";
        return false;
    }

    definition.uuid = sdk::uuid_or_derived(definition.uuid, definition.block);
    if (byBlock_.count(definition.block) != 0) {
        errorOut = "fluid for block '" + definition.block + "' is already registered";
        return false;
    }
    if (byUuid_.count(definition.uuid) != 0) {
        errorOut = "fluid uuid '" + definition.uuid + "' is already registered";
        return false;
    }
    byBlock_.emplace(definition.block, definition);
    byUuid_.emplace(definition.uuid, definition);
    errorOut.clear();
    return true;
}

}  // namespace registry
}  // namespace engine

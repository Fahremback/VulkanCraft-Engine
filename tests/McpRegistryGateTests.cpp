// McpRegistryGateTests — equivalence gate for the MCP registry-asset surface
// (FALTANTES item 23 / prioridade 5). The MCP semantic layer
// (tools/mcp-server/game-authoring.mjs) authors versioned JSON registry assets
// under Content/Registry/<kind>/<name>.json; the JS validator mirrors the
// public contracts' rules. This gate mirrors the EXACT documents the MCP
// emits (see tools/mcp-server/protocol-smoke.mjs) and proves they load through
// the PUBLIC factories unchanged — blocks/items/fluids/recipes through
// engine/registry/*, biomes and structures through engine/procgen/*.
//
// The test TU compiles against ONLY the public headers (src/engine/public) plus
// glm, like voxel_sdk_tests — the stand-in for an external project. The engine
// implementation comes from the SDK/voxel OBJECT modules.
//
// No UI, no GPU. Build/run like the other engine tests (standalone main() with
// CHECK).

#include "engine/procgen/IClimateBiome.hpp"
#include "engine/procgen/IStructureGenerator.hpp"
#include "engine/registry/BlockRegistry.hpp"
#include "engine/registry/FluidRegistry.hpp"
#include "engine/registry/ItemRegistry.hpp"
#include "engine/registry/RecipeRegistry.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "McpRegistryGateTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

// ---------------------------------------------------------------------------
// Documents exactly as authored by the MCP smoke test. Keep these strings in
// sync with tools/mcp-server/protocol-smoke.mjs.
// ---------------------------------------------------------------------------

const char* kBlockDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"Titanium\","
    "\"class\":\"solid\",\"hardness\":3.5,\"lightEmission\":0,"
    "\"lightAbsorption\":1,\"opaque\":true,\"collidable\":true,"
    "\"collisionShape\":\"cross\",\"selectionShape\":\"none\","
    "\"tags\":[\"metal\"],\"drops\":[\"vulkancraft:titanium\"],"
    "\"faceTop\":[0.2,0.8,0.2],"
    "\"faceSide\":[0.5,0.35,0.2],\"occlusion\":false,\"renderLayer\":1,"
    "\"states\":[{\"name\":\"base\",\"color\":[0.5,0.4,0.3]},"
    "{\"name\":\"lit\",\"color\":[1.0,0.6,0.1],\"faceTop\":[0.9,0.9,0.9],\"lightEmission\":0.8}],"
    "\"transitions\":[{\"from\":\"\",\"to\":\"lit\",\"trigger\":\"ignite\"},"
    "{\"from\":\"lit\",\"to\":\"\",\"trigger\":\"extinguish\"}],"
    "\"soundPlace\":\"vulkancraft:titanium_place\",\"soundBreak\":\"vulkancraft:titanium_break\","
    "\"particleBreak\":\"vulkancraft:metal_dust\","
    "\"tool\":\"pickaxe\",\"toolTier\":2,\"resistance\":30,\"friction\":0.4,"
    "\"bounciness\":0.02,\"density\":7.5,\"behaviorId\":\"vulkancraft:reinforced\"}";

const char* kSludgeBlockDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"sludge\","
    "\"class\":\"solid\",\"hardness\":1,\"opaque\":true,\"collidable\":true,"
    "\"tags\":[],\"drops\":[],"
    "\"fluid\":{\"viscosity\":0.8,\"density\":1.2,\"range\":4,\"tickInterval\":0.08,"
    "\"source\":true,\"falling\":true,\"evaporation\":false,\"damagePerTick\":2,"
    "\"compressible\":false}}";

const char* kSludgeItemDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"sludge\","
    "\"maxStack\":64,\"durability\":0,\"icon\":\"\",\"model\":\"\",\"tags\":[]}";

const char* kItemDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"titanium_ingot\","
    "\"maxStack\":16,\"durability\":0,\"icon\":\"\",\"model\":\"\","
    "\"tags\":[\"metal\"],\"useCooldown\":300,\"useMode\":\"instant\","
    "\"equipSlot\":\"hand\",\"attackDamage\":4.5,\"armor\":0,"
    "\"behaviorId\":\"vulkancraft:combat_use\"}";

const char* kInputItemDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"titanium\","
    "\"maxStack\":64,\"durability\":0,\"icon\":\"\",\"model\":\"\",\"tags\":[]}";

const char* kFluidDocument =
    "{\"version\":1,\"block\":\"vulkancraft:sludge\",\"viscosity\":0.8,"
    "\"density\":1,\"range\":7,\"tickInterval\":0.08,\"source\":true,"
    "\"falling\":true,\"evaporation\":true,\"damagePerTick\":2,"
    "\"compressible\":false}";

const char* kRecipeDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"TitaniumIngot\","
    "\"station\":\"vulkancraft:furnace\",\"time\":1,\"energy\":0,\"fuel\":\"\","
    "\"conditions\":[],\"tags\":[],"
    "\"inputs\":[{\"item\":\"vulkancraft:titanium\",\"count\":2}],"
    "\"outputs\":[{\"item\":\"vulkancraft:titanium_ingot\",\"count\":1}]}";

const char* kBiomeDocument =
    "{\"version\":1,\"biomes\":[{\"name\":\"crystal_plains\","
    "\"engineBiomeIndex\":12,\"climate\":{\"temperature\":[0.2,0.8],"
    "\"moisture\":[0.3,0.9]},\"surface\":[{\"blockId\":3,\"minDepth\":0,"
    "\"maxDepth\":3}]}]}";

const char* kStructureDocument =
    "{\"version\":1,\"sampleWidth\":4,\"sampleHeight\":4,"
    "\"sample\":[1,1,1,1,1,0,0,1,1,0,0,1,1,1,1,1],\"patternSize\":2,"
    "\"symmetry\":1,\"periodicOutput\":false,\"ground\":false,\"seed\":0,"
    "\"profiles\":[{\"blockId\":1,\"layers\":[1,1,2]}]}";

bool has_string(const std::vector<std::string>& values, const std::string& needle) {
    for (const std::string& value : values) {
        if (value == needle) return true;
    }
    return false;
}

bool test_block_gate() {
    engine::registry::BlockRegistry blocks;
    std::string error;
    CHECK(blocks.load_from_json(kBlockDocument, error));
    CHECK(error.empty());

    const engine::registry::BlockDefinition* titanium =
        blocks.find_by_name("vulkancraft:Titanium");  // emitted name preserves case
    CHECK(titanium != nullptr);
    CHECK(titanium->hardness == 3.5f);
    CHECK(titanium->opaque);
    CHECK(titanium->collidable);
    // §2 item 2: collision/selection shapes round-trip; none wins over the
    // collidable bool for the runtime solidity.
    CHECK(titanium->collisionShape == engine::registry::CollisionShape::Cross);
    CHECK(titanium->selectionShape == engine::registry::SelectionShape::None);
    CHECK(titanium->is_collidable());  // cross != none: still solid at cell granularity
    CHECK(has_string(titanium->tags, "metal"));
    CHECK(!titanium->hasBuiltinMapping);  // catalog-only: no builtinId declared
    // §14: per-face materials, occlusion and renderLayer round-trip.
    CHECK(titanium->faceTopSet);
    CHECK(titanium->faceTop.g == 0.8f);
    CHECK(!titanium->faceBottomSet);
    CHECK(titanium->faceSideSet);
    CHECK(titanium->faceSide.r == 0.5f);
    CHECK(!titanium->occludes);
    CHECK(titanium->renderLayer == 1);
    // §2 item 5: named states + versioned transitions round-trip.
    CHECK(titanium->states.size() == 2);
    CHECK(titanium->state_index("base") == 0);
    CHECK(titanium->state_index("lit") == 1);
    CHECK(titanium->state_index("nope") == -1);
    CHECK(titanium->states[1].faceTopSet);
    CHECK(std::abs(titanium->states[1].faceTop.r - 0.9f) < 1e-3f);
    CHECK(titanium->transitions.size() == 2);
    CHECK(titanium->transitions[0].fromState.empty());
    CHECK(titanium->transitions[0].toState == "lit");
    CHECK(titanium->transitions[0].trigger == "ignite");
    // §2 item 4: sound/particle/tool/resistance/physics round-trip.
    CHECK(titanium->tool == engine::registry::BlockTool::Pickaxe);
    CHECK(titanium->toolTier == 2);
    CHECK(titanium->resistance == 30.0f);
    CHECK(titanium->friction == 0.4f);
    CHECK(titanium->bounciness == 0.02f);
    CHECK(titanium->density == 7.5f);
    CHECK(titanium->soundPlace == "vulkancraft:titanium_place");
    CHECK(titanium->soundBreak == "vulkancraft:titanium_break");
    CHECK(titanium->particleBreak == "vulkancraft:metal_dust");
    // §2 item 6: declarative behavior reference round-trips (validated, not
    // resolved — the abilities/block entity milestone resolves it).
    CHECK(titanium->behaviorId == "vulkancraft:reinforced");

    // Deterministic: the same document loads identically into a fresh registry.
    engine::registry::BlockRegistry again;
    CHECK(again.load_from_json(kBlockDocument, error));
    const engine::registry::BlockDefinition* titaniumAgain =
        again.find_by_name("vulkancraft:Titanium");
    CHECK(titaniumAgain != nullptr);
    CHECK(titaniumAgain->uuid == titanium->uuid);

    // The JS validator refuses every declared builtinId; the C++ side agrees:
    // ids in [0, Count) collide with the baked-in table ("already used").
    const char* usedBuiltin =
        "{\"namespace\":\"vulkancraft\",\"name\":\"Bad\",\"builtinId\":3}";
    engine::registry::BlockRegistry strict;
    CHECK(!strict.load_from_json(usedBuiltin, error));
    CHECK(error.find("already used") != std::string::npos);
    CHECK(strict.find_by_name("vulkancraft:bad") == nullptr);  // all-or-nothing

    // Out-of-range ids (>= BlockType::Count) are refused too.
    const char* outOfRange =
        "{\"namespace\":\"vulkancraft\",\"name\":\"Bad\",\"builtinId\":200}";
    engine::registry::BlockRegistry range;
    CHECK(!range.load_from_json(outOfRange, error));
    CHECK(error.find("out of range") != std::string::npos);

    std::cout << "[mcp-gate] block: authored document loads through the public "
                 "factory; builtinId refused (already used / out of range) OK\n";
    return true;
}

bool test_item_gate() {
    engine::registry::ItemRegistry items;
    std::string error;
    CHECK(items.load_from_json(kItemDocument, error));
    CHECK(error.empty());

    const engine::registry::ItemDefinition* ingot =
        items.find_by_name("vulkancraft:titanium_ingot");
    CHECK(ingot != nullptr);
    CHECK(ingot->maxStack == 16);
    CHECK(has_string(ingot->tags, "metal"));
    // §2 item 8: the authored use/equipment/behavior components resolve.
    CHECK(ingot->useCooldownMs == 300);
    CHECK(ingot->useMode == engine::registry::ItemUseMode::Instant);
    CHECK(ingot->equipSlot == engine::registry::ItemEquipSlot::Hand);
    CHECK(std::abs(ingot->attackDamage - 4.5f) < 1e-3f);
    CHECK(ingot->armor == 0.0f);
    CHECK(ingot->behaviorId == "vulkancraft:combat_use");

    std::cout << "[mcp-gate] item: authored document loads through the public "
                 "factory OK\n";
    return true;
}

bool test_cross_reference_gate() {
    // FALTANTES item 9: the emitted documents form a consistent project —
    // Titanium drops the authored item "titanium", Sludge drives the authored
    // block "sludge" — and cross-reference validation resolves every link.
    std::string error;
    engine::registry::BlockRegistry blocks;
    CHECK(blocks.load_from_json(kBlockDocument, error));
    CHECK(blocks.load_from_json(kSludgeBlockDocument, error));
    // §2 item 7: the emitted inline fluid binding round-trips.
    const engine::registry::BlockDefinition* sludge =
        blocks.find_by_name("vulkancraft:sludge");
    CHECK(sludge != nullptr);
    CHECK(sludge->fluid.declared);
    CHECK(sludge->fluid.viscosity == 0.8f);
    CHECK(sludge->fluid.density == 1.2f);
    CHECK(sludge->fluid.range == 4);
    CHECK(!sludge->fluid.evaporation);
    CHECK(sludge->fluid.damagePerTick == 2.0f);
    engine::registry::ItemRegistry items;
    CHECK(items.load_from_json(kInputItemDocument, error));  // vulkancraft:titanium
    CHECK(items.load_from_json(kItemDocument, error));
    CHECK(items.load_from_json(kSludgeItemDocument, error));  // sludge auto-drop

    std::vector<std::string> refErrors;
    CHECK(blocks.validate_item_references(items, refErrors));
    CHECK(refErrors.empty());
    engine::registry::FluidRegistry fluids;
    CHECK(fluids.load_from_json(kFluidDocument, error));
    CHECK(fluids.validate_block_references(blocks, refErrors));
    CHECK(refErrors.empty());

    // A drop referencing an un-authored item is flagged (never guessed).
    engine::registry::BlockRegistry dangling;
    CHECK(dangling.load_from_json(
        "{\"namespace\":\"vulkancraft\",\"name\":\"Dangling\",\"drops\":[\"vulkancraft:nope\"]}",
        error));
    std::vector<std::string> danglingErrors;
    CHECK(!dangling.validate_item_references(items, danglingErrors));
    bool foundNope = false;
    for (const std::string& message : danglingErrors) {
        if (message.find("vulkancraft:nope") != std::string::npos) foundNope = true;
    }
    CHECK(foundNope);

    std::cout << "[mcp-gate] cross-references: emitted drops->items and "
                 "fluids->blocks resolve through the public factories OK\n";
    return true;
}

bool test_fluid_gate() {
    engine::registry::FluidRegistry fluids;
    std::string error;
    CHECK(fluids.load_from_json(kFluidDocument, error));
    CHECK(error.empty());

    const engine::registry::FluidDefinition* sludge =
        fluids.find_by_block("vulkancraft:sludge");
    CHECK(sludge != nullptr);
    CHECK(sludge->viscosity == 0.8f);
    CHECK(sludge->damagePerTick == 2.0f);
    CHECK(sludge->range == 7);
    CHECK(sludge->falling);
    CHECK(sludge->evaporation);

    std::cout << "[mcp-gate] fluid: authored document loads through the public "
                 "factory OK\n";
    return true;
}

bool test_recipe_gate() {
    // The recipe's item/tag references are validated against a real
    // ItemRegistry (the C++ contract): the input item must exist.
    engine::registry::ItemRegistry items;
    std::string error;
    CHECK(items.load_from_json(kInputItemDocument, error));
    CHECK(items.load_from_json(kItemDocument, error));

    engine::registry::RecipeRegistry recipes(&items);
    CHECK(recipes.load_from_json(kRecipeDocument, error));
    CHECK(error.empty());

    const engine::registry::RecipeDefinition* recipe =
        recipes.find_by_name("vulkancraft:TitaniumIngot");  // emitted recipe name
    CHECK(recipe != nullptr);
    CHECK(recipe->station == "vulkancraft:furnace");
    CHECK(recipe->inputs.size() == 1);
    CHECK(recipe->inputs[0].item == "vulkancraft:titanium");
    CHECK(recipe->inputs[0].count == 2);
    CHECK(recipe->outputs.size() == 1);
    CHECK(recipe->outputs[0].item == "vulkancraft:titanium_ingot");
    CHECK(recipe->outputs[0].count == 1);

    // Unknown references are refused, never guessed.
    const char* unknownRef =
        "{\"namespace\":\"vulkancraft\",\"name\":\"Ghost\","
        "\"inputs\":[{\"item\":\"vulkancraft:ghost_ore\",\"count\":1}],"
        "\"outputs\":[{\"item\":\"vulkancraft:titanium_ingot\",\"count\":1}]}";
    engine::registry::RecipeRegistry strict(&items);
    CHECK(!strict.load_from_json(unknownRef, error));
    CHECK(error.find("unknown input item") != std::string::npos);

    std::cout << "[mcp-gate] recipe: authored document loads through the public "
                 "factory against a real ItemRegistry; unknown refs refused OK\n";
    return true;
}

bool test_biome_gate() {
    std::string error;
    auto registry = engine::procgen::create_biome_registry_from_json(
        kBiomeDocument, error);
    CHECK(registry != nullptr);
    CHECK(error.empty());
    CHECK(registry->biome_count() == 1);

    engine::procgen::BiomeDefinition def;
    CHECK(registry->biome_definition(0, def));
    CHECK(def.name == "crystal_plains");
    CHECK(def.engineBiomeIndex == 12);
    CHECK(def.surface.size() == 1);
    CHECK(def.surface[0].blockId == 3);
    CHECK(def.surface[0].maxDepth == 3);

    // The climate bounds classify deterministically.
    const engine::procgen::ClimatePoint inPlains{ 0.5f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f };
    const engine::procgen::ClimatePoint outPlains{ -0.5f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::uint32_t index = 999;
    CHECK(registry->biome_for(inPlains, index));
    CHECK(index == 0);
    CHECK(!registry->biome_for(outPlains, index));

    // Same document -> identical registry (determinism between instances).
    auto again = engine::procgen::create_biome_registry_from_json(
        kBiomeDocument, error);
    CHECK(again != nullptr);
    std::uint32_t a = 0, b = 0;
    CHECK(registry->biome_for(inPlains, a) && again->biome_for(inPlains, b));
    CHECK(a == b);

    std::cout << "[mcp-gate] biome: authored document loads through the public "
                 "factory; classification deterministic OK\n";
    return true;
}

bool test_structure_gate() {
    std::string error;
    auto generator = engine::procgen::create_structure_generator_from_json(
        kStructureDocument, error);
    CHECK(generator != nullptr);
    CHECK(error.empty());

    const engine::procgen::StructureAssetSpec& asset = generator->asset();
    CHECK(asset.sampleWidth == 4);
    CHECK(asset.sampleHeight == 4);
    CHECK(asset.sample.size() == 16);
    CHECK(asset.patternSize == 2);
    CHECK(asset.profiles.size() == 1);
    CHECK(asset.profiles[0].first == 1);
    CHECK(asset.profiles[0].second.size() == 3);

    // Deterministic generation: two runs are bit-identical.
    engine::procgen::StructureOutput first;
    engine::procgen::StructureOutput second;
    CHECK(generator->generate(8, 8, first, error));
    CHECK(first.succeeded);
    CHECK(first.plan.size() == 64);
    CHECK(generator->generate(8, 8, second, error));
    CHECK(first.plan == second.plan);
    CHECK(first.blocks == second.blocks);
    CHECK(first.seedUsed == second.seedUsed);

    std::cout << "[mcp-gate] structure: authored document loads through the "
                 "public factory; generation deterministic OK\n";
    return true;
}

bool run_all() {
    if (!test_block_gate()) return false;
    if (!test_item_gate()) return false;
    if (!test_fluid_gate()) return false;
    if (!test_recipe_gate()) return false;
    if (!test_biome_gate()) return false;
    if (!test_structure_gate()) return false;
    if (!test_cross_reference_gate()) return false;
    return true;
}

}  // namespace

int main() {
    if (run_all()) {
        std::cout << "McpRegistryGateTests: all gates passed\n";
        return 0;
    }
    return 1;
}

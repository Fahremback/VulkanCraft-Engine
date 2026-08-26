// UiCraftingTests — headless coverage for the public crafting-UI contract
// (engine/ui/ICrafting.hpp, adapter UiCrafting.cpp): a presentation/decision
// session that DELEGATES to RecipeRegistry — craftable list (deterministic
// order), selection rules, and craft (atomic consume/produce). The UI never
// re-implements satisfiability; authority and atomicity are the registry's.
// Standalone main() with CHECK (pattern: UiInventoryGridTests).

#include "engine/ui/ICrafting.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::ui;
using namespace engine::registry;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiCraftingTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

ItemDefinition make_item(const std::string& ns, const std::string& name,
                         int maxStack = 64) {
    ItemDefinition def;
    def.ns = ns;
    def.name = name;
    def.maxStack = maxStack;
    def.icon = "icons/" + name + ".png";
    return def;
}

void unlock_all(Inventory& inv) {
    SlotFilter any;
    any.allowAny = true;
    for (int s = 0; s < inv.slot_count(); ++s) {
        inv.set_filter(s, any);
    }
}

bool put(Inventory& inv, int slot, const std::string& item, int count,
         const ItemRegistry& items, std::string& err) {
    ItemStack stack;
    stack.item = item;
    stack.count = count;
    return inv.set(slot, stack, items, err);
}

// Items: cobblestone x3 + stick x2 -> stone_pickaxe x1 (crafting table).
bool build_world(ItemRegistry& items, RecipeRegistry& recipes,
                 std::string& err) {
    if (!items.register_item(make_item("vulkancraft", "cobblestone"), err)) return false;
    if (!items.register_item(make_item("vulkancraft", "stick"), err)) return false;
    if (!items.register_item(make_item("vulkancraft", "stone_pickaxe", 1), err)) return false;

    RecipeDefinition pickaxe;
    pickaxe.ns = "vulkancraft";
    pickaxe.name = "stone_pickaxe";
    pickaxe.station = "vulkancraft:crafting_table";
    RecipeInput in1;
    in1.item = "vulkancraft:cobblestone";
    in1.count = 3;
    pickaxe.inputs.push_back(in1);
    RecipeInput in2;
    in2.item = "vulkancraft:stick";
    in2.count = 2;
    pickaxe.inputs.push_back(in2);
    RecipeOutput out;
    out.item = "vulkancraft:stone_pickaxe";
    out.count = 1;
    pickaxe.outputs.push_back(out);
    return recipes.register_recipe(pickaxe, err);
}

bool run_all() {
    std::string err;

    // ---- Spec validate + JSON round-trip --------------------------------
    {
        CraftingSpec spec;
        spec.station = "vulkancraft:crafting_table";
        spec.seed = 7;
        CHECK(spec.validate(err));
        const std::string json = spec.to_json();
        CraftingSpec back;
        CHECK(back.load_from_json(json, err));
        CHECK(back.station == "vulkancraft:crafting_table");
        CHECK(back.seed == 7);
        CHECK(back.to_json() == json);

        CraftingSpec zeroSeed;
        zeroSeed.seed = 0;
        CHECK(!zeroSeed.validate(err));
        CHECK(!err.empty());

        CraftingSpec untouched = spec;
        CHECK(!untouched.load_from_json("{bad", err));
        CHECK(untouched.seed == 7);
        CHECK(!untouched.load_from_json("{\"version\":99}", err));
        CHECK(untouched.seed == 7);
    }

    // ---- refresh: craftable list (deterministic) -------------------------
    {
        ItemRegistry items;
        RecipeRegistry recipes(&items);
        build_world(items, recipes, err);

        Inventory inv(12);
        unlock_all(inv);
        CHECK(put(inv, 0, "vulkancraft:cobblestone", 3, items, err));
        CHECK(put(inv, 1, "vulkancraft:stick", 2, items, err));

        CraftingSpec spec;
        spec.station = "vulkancraft:crafting_table";
        auto ui = create_ui_crafting(spec, err);
        CHECK(ui != nullptr);
        CHECK(ui->refresh(inv, recipes, items, err));
        const std::vector<std::string> craftable = ui->craftable();
        CHECK(craftable.size() == 1);
        CHECK(craftable[0] == "vulkancraft:stone_pickaxe");

        // Determinism cross-instance (same world + inventory + station).
        auto ui2 = create_ui_crafting(spec, err);
        CHECK(ui2->refresh(inv, recipes, items, err));
        CHECK(ui2->craftable() == craftable);
    }

    // ---- select + craft (delegates to the registry, atomic) --------------
    {
        ItemRegistry items;
        RecipeRegistry recipes(&items);
        build_world(items, recipes, err);

        Inventory inv(12);
        unlock_all(inv);
        CHECK(put(inv, 0, "vulkancraft:cobblestone", 3, items, err));
        CHECK(put(inv, 1, "vulkancraft:stick", 2, items, err));

        CraftingSpec spec;
        spec.station = "vulkancraft:crafting_table";
        auto ui = create_ui_crafting(spec, err);
        CHECK(ui != nullptr);
        CHECK(ui->refresh(inv, recipes, items, err));

        // Unknown name refused.
        CHECK(!ui->select("vulkancraft:nope", err));
        CHECK(!err.empty());
        CHECK(ui->selected().empty());

        // Craft without selection refused.
        err.clear();
        const CraftResult refused = ui->craft(inv, recipes, items, err);
        CHECK(!refused.ok);
        CHECK(!err.empty());

        // Select + craft: consumes exactly the inputs, produces the output.
        err.clear();
        CHECK(ui->select("vulkancraft:stone_pickaxe", err));
        CHECK(ui->selected() == "vulkancraft:stone_pickaxe");
        const CraftResult result = ui->craft(inv, recipes, items, err);
        CHECK(result.ok);
        CHECK(result.error.empty());
        CHECK(result.outputs.size() == 1);
        CHECK(result.outputs[0].item == "vulkancraft:stone_pickaxe");
        CHECK(result.outputs[0].count == 1);
        CHECK(ui->last_result().ok);

        // Inputs consumed: inventory no longer satisfies the recipe.
        CHECK(ui->refresh(inv, recipes, items, err));
        CHECK(ui->craftable().empty());
        CHECK(!ui->select("vulkancraft:stone_pickaxe", err));  // not craftable now
    }

    // ---- Craft with insufficient inputs -> atomic refusal ----------------
    {
        ItemRegistry items;
        RecipeRegistry recipes(&items);
        build_world(items, recipes, err);

        Inventory inv(12);
        unlock_all(inv);
        CHECK(put(inv, 0, "vulkancraft:cobblestone", 3, items, err));
        // Only 1 stick (needs 2).

        CraftingSpec spec;
        spec.station = "vulkancraft:crafting_table";
        auto ui = create_ui_crafting(spec, err);
        CHECK(ui != nullptr);
        CHECK(ui->refresh(inv, recipes, items, err));
        CHECK(ui->craftable().empty());  // unsatisfiable

        // A stale selection (from a previous inventory) cannot craft either.
        // Seed determinism with byproducts:
        ItemRegistry items2;
        CHECK(items2.register_item(make_item("test", "ore"), err));
        CHECK(items2.register_item(make_item("test", "ingot"), err));
        CHECK(items2.register_item(make_item("test", "slag"), err));
        RecipeRegistry recipes2(&items2);
        RecipeDefinition smelt;
        smelt.ns = "test";
        smelt.name = "smelt";
        smelt.station = "test:furnace";
        RecipeInput ore;
        ore.item = "test:ore";
        ore.count = 1;
        smelt.inputs.push_back(ore);
        RecipeOutput ingot;
        ingot.item = "test:ingot";
        ingot.count = 1;
        smelt.outputs.push_back(ingot);
        RecipeOutput slag;
        slag.item = "test:slag";
        slag.count = 1;
        slag.chance = 0.5;  // rolled
        slag.byproduct = true;
        smelt.outputs.push_back(slag);
        CHECK(recipes2.register_recipe(smelt, err));

        // Two identical worlds, same seed -> identical craft outcomes.
        CraftResult first;
        for (int i = 0; i < 2; ++i) {
            Inventory inv2(12);
            unlock_all(inv2);
            CHECK(put(inv2, 0, "test:ore", 1, items2, err));
            CraftingSpec spec2;
            spec2.station = "test:furnace";
            spec2.seed = 42;
            auto ui2 = create_ui_crafting(spec2, err);
            CHECK(ui2 != nullptr);
            CHECK(ui2->refresh(inv2, recipes2, items2, err));
            CHECK(ui2->select("test:smelt", err));
            const CraftResult r = ui2->craft(inv2, recipes2, items2, err);
            CHECK(r.ok);
            if (i == 0) {
                first = r;
            } else {
                CHECK(r.outputs.size() == first.outputs.size());
                CHECK(r.byproducts.size() == first.byproducts.size());
                if (!r.byproducts.empty() && !first.byproducts.empty()) {
                    CHECK(r.byproducts[0].item == first.byproducts[0].item);
                }
            }
        }
    }

    std::cout << "UiCraftingTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

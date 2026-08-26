// UiInventoryGridTests — headless coverage for the public inventory-slot
// presentation contract (engine/ui/IInventoryGrid.hpp, adapter
// UiInventoryGrid.cpp). Presentation only: the grid resolves slot rects,
// icon/tooltip and hit-tests, and drives drag-and-drop by DELEGATING to
// Inventory::transfer/swap. All mutation logic (filters, merge, maxStack,
// undo/redo) stays in Inventory — this gate only tests that the delegation is
// correct (same-inventory swap vs cross-inventory move, count/merge, filter
// refusal).
// Standalone main() with CHECK (pattern: ActionMapTests/UiLayoutTests).

#include "engine/ui/IInventoryGrid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::ui;
using namespace engine::registry;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiInventoryGridTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

// Helper: register a simple item (stackable, no tag filter).
ItemDefinition make_item(const std::string& ns, const std::string& name,
                         int maxStack = 64, int durability = 0) {
    ItemDefinition def;
    def.ns = ns;
    def.name = name;
    def.maxStack = maxStack;
    def.durability = durability;
    def.icon = "icons/" + name + ".png";
    return def;
}

// A default SlotFilter is a LOCKED slot (allowAny=false, empty lists); the
// tests unlock every slot they touch (pattern: VoxelSdkTests).
void unlock_all(Inventory& inv) {
    SlotFilter any;
    any.allowAny = true;
    for (int s = 0; s < inv.slot_count(); ++s) {
        inv.set_filter(s, any);
    }
}

bool run_all() {
    std::string err;

    // ---- Spec validate + JSON round-trip --------------------------------
    SlotGridSpec spec;
    spec.rows = 3;
    spec.cols = 4;
    spec.cell_w = 80.0;
    spec.cell_h = 80.0;
    spec.gap_x = 4.0;
    spec.gap_y = 4.0;
    spec.origin_x = 10.0;
    spec.origin_y = 20.0;
    CHECK(spec.validate(err));

    const std::string json = spec.to_json();
    SlotGridSpec back;
    CHECK(back.load_from_json(json, err));
    CHECK(back.rows == 3);
    CHECK(back.cols == 4);
    CHECK(back.cell_w == 80.0);
    CHECK(back.cell_h == 80.0);
    CHECK(back.gap_x == 4.0);
    CHECK(back.gap_y == 4.0);
    CHECK(back.origin_x == 10.0);
    CHECK(back.origin_y == 20.0);
    CHECK(back.to_json() == json);

    // Malformed refused all-or-nothing (target untouched).
    SlotGridSpec keep = back;
    CHECK(!back.load_from_json("{bad", err));
    CHECK(back.to_json() == keep.to_json());

    // Version 2 refused.
    CHECK(!back.load_from_json("{\"version\":2,\"rows\":1,\"cols\":1,\"cell_w\":1,\"cell_h\":1,\"gap_x\":0,\"gap_y\":0,\"origin_x\":0,\"origin_y\":0}", err));

    // ---- Slot rects: cell geometry (row-major) --------------------------
    auto grid = create_inventory_grid(spec, err);
    CHECK(grid != nullptr);

    ItemRegistry items;
    CHECK(items.register_item(make_item("test", "ruby"), err));
    CHECK(items.register_item(make_item("test", "sapphire"), err));
    CHECK(items.register_item(make_item("test", "emerald"), err));

    // Empty inventory: all cells render empty (no icons/tooltips).
    Inventory inv(12);
    unlock_all(inv);
    const std::vector<SlotView> empty_slots = grid->slots(inv, items);
    CHECK(empty_slots.size() == 12);
    CHECK(empty_slots[0].item.empty() && empty_slots[0].count == 0);
    CHECK(empty_slots[0].tooltip.empty());
    CHECK(empty_slots[0].icon.empty());

    // Cell geometry: cell 0 at (origin, origin).
    CHECK(empty_slots[0].x == 10.0);
    CHECK(empty_slots[0].y == 20.0);
    CHECK(empty_slots[0].w == 80.0);
    CHECK(empty_slots[0].h == 80.0);

    // Cell 4 (second row, first col = row 1, col 0): y = origin + 1*(cell_h+gap).
    CHECK(empty_slots[4].x == 10.0);
    CHECK(empty_slots[4].y == 20.0 + 1.0 * (80.0 + 4.0)); // 104.0

    // Populate slot 0 with a stack.
    ItemStack ruby;
    ruby.item = "test:ruby";
    ruby.count = 5;
    CHECK(inv.set(0, ruby, items, err));
    const std::vector<SlotView> filled = grid->slots(inv, items);
    CHECK(filled[0].item == "test:ruby");
    CHECK(filled[0].count == 5);
    CHECK(filled[0].damage == 0);
    CHECK(filled[0].icon == items.find_by_name("test:ruby")->icon);

    // Tooltip: "test:ruby x5".
    CHECK(filled[0].tooltip == "test:ruby x5");

    // Damage tooltip: "test:ruby x1 (dmg 3)".
    ItemStack damaged;
    damaged.item = "test:ruby";
    damaged.count = 1;
    damaged.damage = 3;
    CHECK(inv.set(1, damaged, items, err));
    const std::vector<SlotView> dmg = grid->slots(inv, items);
    CHECK(dmg[1].tooltip == "test:ruby x1 (dmg 3)");

    // ---- slot_at: hit-test -----------------------------------------------
    // Cell 0: (10,20)-(90,100).
    CHECK(grid->slot_at(10.0, 20.0) == 0);  // top-left corner (inclusive x/y)
    CHECK(grid->slot_at(89.9, 99.9) == 0);  // just inside
    CHECK(grid->slot_at(90.0, 20.0) == -1); // past right edge (exclusive)
    CHECK(grid->slot_at(10.0, 100.0) == -1); // past bottom edge (exclusive)

    // ---- Drag-and-drop: same-inventory merge -----------------------------
    Inventory a(12);
    unlock_all(a);
    ItemRegistry reg;
    CHECK(reg.register_item(make_item("test", "iron", 64, 0), err));
    CHECK(reg.register_item(make_item("test", "gold", 64, 0), err));
    CHECK(reg.register_item(make_item("test", "pickaxe", 1, 250), err));

    // Put 10 iron in slot 0, 3 in slot 1.
    ItemStack iron10;
    iron10.item = "test:iron";
    iron10.count = 10;
    CHECK(a.set(0, iron10, reg, err));
    ItemStack iron3;
    iron3.item = "test:iron";
    iron3.count = 3;
    CHECK(a.set(1, iron3, reg, err));

    // Drag 5 iron from slot 0 to slot 1: merge.
    DragResult dr = grid->drag(a, 0, a, 1, 5, reg);
    CHECK(dr.accepted);
    CHECK(dr.moved == 5);
    CHECK(a.get(0).count == 5);  // 10-5
    CHECK(a.get(1).count == 8);  // 3+5

    // Drag remaining 5 from slot 0 to empty slot 2: move.
    dr = grid->drag(a, 0, a, 2, 5, reg);
    CHECK(dr.accepted);
    CHECK(dr.moved == 5);
    CHECK(a.get(0).empty());
    CHECK(a.get(2).count == 5);

    // ---- Drag-and-drop: same-inventory swap ------------------------------
    Inventory b(12);
    unlock_all(b);
    ItemStack gold;
    gold.item = "test:gold";
    gold.count = 1;
    CHECK(b.set(0, iron10, reg, err));
    CHECK(b.set(1, gold, reg, err));
    dr = grid->drag(b, 0, b, 1, 1, reg);
    CHECK(dr.accepted);
    CHECK(dr.swapped);
    CHECK(b.get(0).item == "test:gold");
    CHECK(b.get(1).item == "test:iron");

    // ---- Drag-and-drop: cross-inventory move -----------------------------
    Inventory src(12);
    unlock_all(src);
    Inventory dst(12);
    unlock_all(dst);
    CHECK(src.set(0, iron10, reg, err));
    dr = grid->drag(src, 0, dst, 0, 3, reg);
    CHECK(dr.accepted);
    CHECK(dr.moved == 3);
    CHECK(src.get(0).count == 7);
    CHECK(dst.get(0).count == 3);

    // ---- Drag-and-drop: src==dst no-op (valid no-op) ----------------------
    Inventory c(12);
    unlock_all(c);
    CHECK(c.set(0, iron3, reg, err));
    dr = grid->drag(c, 0, c, 0, 1, reg);
    CHECK(dr.accepted);
    CHECK(dr.moved == 0);
    CHECK(c.get(0).count == 3); // untouched

    // ---- Drag-and-drop: refusal (slot out of range) -----------------------
    dr = grid->drag(src, -1, dst, 0, 1, reg);
    CHECK(!dr.accepted);
    dr = grid->drag(src, 0, dst, 99, 1, reg);
    CHECK(!dr.accepted);

    // ---- Drag-and-drop: refusal (count <= 0) ------------------------------
    dr = grid->drag(src, 0, dst, 0, 0, reg);
    CHECK(!dr.accepted);

    // ---- Drag-and-drop: filter refusal (Inventory handles this) -----------
    Inventory locked(12);
    locked.set_filter(0, SlotFilter{}); // allowAny=false, allowItems empty = locked
    dr = grid->drag(src, 0, locked, 0, 1, reg);
    CHECK(!dr.accepted);

    // ---- Deterministic cross-instance -------------------------------------
    auto g1 = create_inventory_grid(spec, err);
    auto g2 = create_inventory_grid(spec, err);
    Inventory det(12);
    unlock_all(det);
    ItemRegistry detReg;
    CHECK(detReg.register_item(make_item("test", "stone", 64), err));
    ItemStack stone;
    stone.item = "test:stone";
    stone.count = 8;
    CHECK(det.set(3, stone, detReg, err));
    const std::vector<SlotView> s1 = g1->slots(det, detReg);
    const std::vector<SlotView> s2 = g2->slots(det, detReg);
    CHECK(s1.size() == s2.size());
    for (std::size_t i = 0; i < s1.size(); ++i) {
        CHECK(s1[i].item == s2[i].item);
        CHECK(s1[i].count == s2[i].count);
        CHECK(s1[i].x == s2[i].x && s1[i].y == s2[i].y);
        CHECK(s1[i].icon == s2[i].icon);
        CHECK(s1[i].tooltip == s2[i].tooltip);
    }

    std::cout << "UiInventoryGridTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
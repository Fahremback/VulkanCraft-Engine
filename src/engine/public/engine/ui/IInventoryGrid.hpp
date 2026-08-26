#pragma once

// IInventoryGrid (agente 2 §A item 2): the PUBLIC inventory-slot presentation
// contract. Gameplay data (items, stacks, slots, filters, authoritative
// mutation) already lives in engine::registry::Inventory/ItemRegistry — this
// contract is PRESENTATION ONLY: it maps an inventory to a deterministic grid
// of slot rects, resolves per-slot icon/tooltip, hit-tests pointer positions,
// and drives drag-and-drop by DELEGATING to Inventory::transfer/swap (it never
// reimplements mutation logic — nothing is ever lost or duplicated).
//   - GRID: slots laid out row-major over `rows`×`cols` cells with a cell size
//     and gap; a grid may have MORE cells than the inventory has slots (the
//     extra cells render empty).
//   - TOOLTIP: "namespaced_id xCount" (+ " (dmg D)" when damage>0); empty slot
//     -> empty tooltip. The icon is the ItemDefinition::icon path (empty when
//     the item is unknown or has no icon).
//   - DRAG-AND-DROP: drag(src, srcSlot, dst, dstSlot, count) implements the
//     standard policy using the authoritative Inventory API:
//       * cross-inventory -> Inventory::transfer (move/merge, filter-checked);
//       * same inventory, destination holds a DIFFERENT item -> Inventory::swap;
//       * same inventory, destination empty/mergeable -> Inventory::transfer;
//       * srcSlot == dstSlot on the same inventory -> accepted no-op.
//     A refused drag (bad slot, empty source, filter reject, unknown item)
//     leaves BOTH inventories untouched (Inventory is already all-or-nothing).
//   - PERSISTENCE: SlotGridSpec JSON versioned, bit-exact round-trip
//     (load_from_json/to_json), all-or-nothing on malformed input.
//
// Deterministic and headless: same spec + inventory + registry -> identical
// rects/tooltips/drag outcomes, bit-exact. Self-contained (std + engine/
// registry only). The SDK adapter (src/engine/sdk/UiInventoryGrid.cpp) is the
// ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/registry/Inventory.hpp"
#include "engine/registry/ItemRegistry.hpp"

namespace engine {
namespace ui {

// Grid geometry. Cell (row r, col c) has index r*cols + c.
struct SlotGridSpec {
    int version{ 1 };
    int rows{ 0 };
    int cols{ 0 };
    double cell_w{ 0.0 };
    double cell_h{ 0.0 };
    double gap_x{ 0.0 };
    double gap_y{ 0.0 };
    double origin_x{ 0.0 };
    double origin_y{ 0.0 };

    int cell_count() const { return rows * cols; }

    // All-or-nothing: rows/cols > 0; cell_w/cell_h > 0; gaps/origins finite
    // and >= 0; origin finite.
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// One slot's presentation: absolute rect + resolved item/icon/tooltip.
struct SlotView {
    int slot{ 0 };         // cell index (row-major)
    double x{ 0.0 };
    double y{ 0.0 };
    double w{ 0.0 };
    double h{ 0.0 };
    std::string item;      // namespaced id (empty = empty slot)
    int count{ 0 };
    int damage{ 0 };
    std::string icon;      // resolved ItemDefinition::icon (empty = none/unknown)
    std::string tooltip;   // "namespaced xCount[ (dmg D)]"; empty for empty slot
};

// Outcome of a drag-and-drop.
struct DragResult {
    bool accepted{ false };  // the drag did something (or was a valid no-op)
    bool swapped{ false };   // true when a swap was performed
    int moved{ 0 };          // units moved (0 for a swap or no-op)
    std::string error;       // diagnostic when !accepted
};

class IUiInventoryGrid {
public:
    virtual ~IUiInventoryGrid() = default;

    // Resolves every cell (0..cell_count-1) into a SlotView. Cells past
    // `inv.slot_count()` render empty. Deterministic row-major order.
    virtual std::vector<SlotView> slots(
        const registry::Inventory& inv,
        const registry::ItemRegistry& items) const = 0;

    // Which cell contains (x, y), or -1 when outside every cell.
    virtual int slot_at(double x, double y) const = 0;

    // Transactional drag-and-drop, delegating to Inventory::transfer/swap.
    // All-or-nothing: a refusal leaves both inventories untouched.
    virtual DragResult drag(registry::Inventory& src, int srcSlot,
                            registry::Inventory& dst, int dstSlot, int count,
                            const registry::ItemRegistry& items) = 0;

    virtual const SlotGridSpec& spec() const = 0;
};

// Parses+validates a spec and compiles it (rejected -> nullptr + errorOut).
std::unique_ptr<IUiInventoryGrid> create_inventory_grid(
    const SlotGridSpec& spec, std::string& errorOut);

}  // namespace ui
}  // namespace engine

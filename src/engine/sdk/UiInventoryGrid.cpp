// UiInventoryGrid.cpp — the ONLY TU with the inventory-slot presentation
// runtime (agente 2 §A item 2). Presentation only: it maps an Inventory to a
// deterministic grid, resolves icon/tooltip, hit-tests, and drives drag-and-
// drop by DELEGATING to Inventory::transfer/swap. It never reimplements
// mutation logic (filters, merge, maxStack, undo/redo all stay in Inventory).
// JSON parse/emit uses the shared RegistryJson helpers.

#include "engine/ui/IInventoryGrid.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <sstream>

namespace engine {
namespace ui {

namespace {

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

bool is_finite(double v) { return std::isfinite(v); }

std::string num(double v) {
    std::ostringstream out;
    out.precision(9);
    out << v;
    return out.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// SlotGridSpec validation + JSON
// ---------------------------------------------------------------------------

bool SlotGridSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported slot grid version";
        return false;
    }
    if (rows <= 0 || cols <= 0) {
        errorOut = "slot grid rows/cols must be > 0";
        return false;
    }
    if (!is_finite(cell_w) || cell_w <= 0.0) {
        errorOut = "slot grid cell_w must be finite and > 0";
        return false;
    }
    if (!is_finite(cell_h) || cell_h <= 0.0) {
        errorOut = "slot grid cell_h must be finite and > 0";
        return false;
    }
    if (!is_finite(gap_x) || gap_x < 0.0) {
        errorOut = "slot grid gap_x must be finite and >= 0";
        return false;
    }
    if (!is_finite(gap_y) || gap_y < 0.0) {
        errorOut = "slot grid gap_y must be finite and >= 0";
        return false;
    }
    if (!is_finite(origin_x)) {
        errorOut = "slot grid origin_x must be finite";
        return false;
    }
    if (!is_finite(origin_y)) {
        errorOut = "slot grid origin_y must be finite";
        return false;
    }
    return true;
}

std::string SlotGridSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1,\"rows\":" << rows << ",\"cols\":" << cols
        << ",\"cell_w\":" << num(cell_w) << ",\"cell_h\":" << num(cell_h)
        << ",\"gap_x\":" << num(gap_x) << ",\"gap_y\":" << num(gap_y)
        << ",\"origin_x\":" << num(origin_x) << ",\"origin_y\":" << num(origin_y)
        << "}";
    return out.str();
}

bool SlotGridSpec::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "slot grid document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported slot grid version";
        return false;
    }
    SlotGridSpec candidate;
    candidate.version = version;
    candidate.rows = static_cast<int>(sdk::json_number(doc, "rows", 0));
    candidate.cols = static_cast<int>(sdk::json_number(doc, "cols", 0));
    candidate.cell_w = sdk::json_number(doc, "cell_w", 0.0);
    candidate.cell_h = sdk::json_number(doc, "cell_h", 0.0);
    candidate.gap_x = sdk::json_number(doc, "gap_x", 0.0);
    candidate.gap_y = sdk::json_number(doc, "gap_y", 0.0);
    candidate.origin_x = sdk::json_number(doc, "origin_x", 0.0);
    candidate.origin_y = sdk::json_number(doc, "origin_y", 0.0);
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

struct CellGeometry {
    double x, y, w, h;
};

CellGeometry cell_geometry(const SlotGridSpec& spec, int cell) {
    const int col = cell % spec.cols;
    const int row = cell / spec.cols;
    CellGeometry g;
    g.x = spec.origin_x + col * (spec.cell_w + spec.gap_x);
    g.y = spec.origin_y + row * (spec.cell_h + spec.gap_y);
    g.w = spec.cell_w;
    g.h = spec.cell_h;
    return g;
}

class UiInventoryGridRuntime final : public IUiInventoryGrid {
public:
    explicit UiInventoryGridRuntime(const SlotGridSpec& spec) : spec_(spec) {}

    std::vector<SlotView> slots(const registry::Inventory& inv,
                                const registry::ItemRegistry& items) const override {
        std::vector<SlotView> result;
        result.reserve(static_cast<std::size_t>(spec_.cell_count()));
        const int slotCount = inv.slot_count();
        for (int cell = 0; cell < spec_.cell_count(); ++cell) {
            const CellGeometry g = cell_geometry(spec_, cell);
            SlotView view;
            view.slot = cell;
            view.x = g.x;
            view.y = g.y;
            view.w = g.w;
            view.h = g.h;
            if (cell < slotCount) {
                const registry::ItemStack& stack = inv.get(cell);
                if (!stack.empty()) {
                    view.item = stack.item;
                    view.count = stack.count;
                    view.damage = stack.damage;
                    const registry::ItemDefinition* def = items.find_by_name(stack.item);
                    if (def != nullptr) {
                        view.icon = def->icon;
                    }
                    // Tooltip: "namespaced xCount[ (dmg D)]".
                    std::ostringstream tip;
                    tip << stack.item << " x" << stack.count;
                    if (stack.damage > 0) tip << " (dmg " << stack.damage << ")";
                    view.tooltip = tip.str();
                }
            }
            result.push_back(std::move(view));
        }
        return result;
    }

    int slot_at(double x, double y) const override {
        for (int cell = 0; cell < spec_.cell_count(); ++cell) {
            const CellGeometry g = cell_geometry(spec_, cell);
            if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) {
                return cell;
            }
        }
        return -1;
    }

    DragResult drag(registry::Inventory& src, int srcSlot,
                    registry::Inventory& dst, int dstSlot, int count,
                    const registry::ItemRegistry& items) override {
        DragResult result;
        std::string errorOut;
        if (srcSlot < 0 || srcSlot >= src.slot_count() || dstSlot < 0 ||
            dstSlot >= dst.slot_count()) {
            result.error = "slot out of range";
            return result;
        }
        if (count <= 0) {
            result.error = "drag count must be >= 1";
            return result;
        }

        const bool same_inventory = (&src == &dst);
        if (same_inventory && srcSlot == dstSlot) {
            // Valid no-op: dropping an item back onto its own slot.
            result.accepted = true;
            result.moved = 0;
            return result;
        }

        if (same_inventory) {
            // Within one inventory: a full slot holding a DIFFERENT item swaps;
            // an empty/mergeable slot moves+merges (transfer).
            const registry::ItemStack& target = dst.get(dstSlot);
            const registry::ItemStack& source = src.get(srcSlot);
            if (!target.empty() && !registry::ItemStack::can_merge(target, source)) {
                if (!registry::Inventory::swap(src, srcSlot, dstSlot, items, errorOut)) {
                    result.error = errorOut;
                    return result;
                }
                result.accepted = true;
                result.swapped = true;
                result.moved = 0;
                return result;
            }
            const int moved =
                registry::Inventory::transfer(src, srcSlot, dst, dstSlot, count,
                                              items, errorOut);
            if (moved <= 0) {
                result.error = errorOut.empty() ? "nothing moved" : errorOut;
                return result;
            }
            result.accepted = true;
            result.moved = moved;
            return result;
        }

        // Cross-inventory: move/merge via transfer.
        const int moved =
            registry::Inventory::transfer(src, srcSlot, dst, dstSlot, count, items,
                                          errorOut);
        if (moved <= 0) {
            result.error = errorOut.empty() ? "nothing moved" : errorOut;
            return result;
        }
        result.accepted = true;
        result.moved = moved;
        return result;
    }

    const SlotGridSpec& spec() const override { return spec_; }

private:
    SlotGridSpec spec_;
};

}  // namespace

std::unique_ptr<IUiInventoryGrid> create_inventory_grid(
    const SlotGridSpec& spec, std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<UiInventoryGridRuntime>(spec);
}

}  // namespace ui
}  // namespace engine

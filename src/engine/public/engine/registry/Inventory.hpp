#pragma once

// Slot inventory with typed filters (SDK, META section 14). Every mutation is
// authoritative: it validates the slot filter and clamps counts to the item
// registry's maxStack, and nothing is ever lost or duplicated (adding returns
// the un-added remainder; transfer returns the moved count). A version counter
// and an optional change callback fire on every successful mutation (change
// events). Serialization is data-driven: namespaced item ids, never numeric
// ids, so inventories round-trip and are ready for replication (section 17).

#include "engine/registry/ItemRegistry.hpp"
#include "engine/registry/ItemStack.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine {
namespace registry {

// Typed slot filter. A slot accepts any item when allowAny is set, else one of
// allowItems (exact namespaced ids) or any item carrying one of allowTags.
// An empty filter without allowAny is a locked slot.
struct SlotFilter {
    std::vector<std::string> allowItems;
    std::vector<std::string> allowTags;
    bool allowAny{ false };

    bool accepts(const ItemDefinition& def) const {
        if (allowAny) return true;
        const std::string namespaced = def.namespaced();
        for (const std::string& id : allowItems) {
            if (id == namespaced) return true;
        }
        for (const std::string& tag : allowTags) {
            for (const std::string& have : def.tags) {
                if (have == tag) return true;
            }
        }
        return false;
    }
};

class Inventory {
public:
    explicit Inventory(int slotCount);

    int slot_count() const noexcept { return static_cast<int>(slots_.size()); }

    // ---- Filters --------------------------------------------------------
    void set_filter(int slot, SlotFilter filter);
    const SlotFilter& filter(int slot) const;

    // ---- Reads ----------------------------------------------------------
    const ItemStack& get(int slot) const;

    // ---- Mutations (authoritative: validate first, then apply) ----------
    // Replaces slot contents. An empty stack clears the slot. Unknown items
    // and filter violations are rejected with a diagnostic (never guessed).
    bool set(int slot, const ItemStack& stack, const ItemRegistry& items,
             std::string& errorOut);

    // Fills across slots (respecting filters and maxStack). Returns the
    // remainder that could NOT be added — empty means everything fit. Nothing
    // is lost: the caller decides what to do with the remainder.
    ItemStack add(const ItemStack& stack, const ItemRegistry& items,
                  std::string& errorOut);

    // Removes up to `count` units of `item` (namespaced id) across all slots;
    // returns how many were actually removed.
    int remove(const std::string& item, int count, const ItemRegistry& items,
               std::string& errorOut);

    // Consumes up to `amount` from one slot; returns the amount removed.
    // Consuming is always allowed (no filter check) — used by craft/fuel.
    int consume(int slot, int amount);

    int count_of(const std::string& item) const;

    // ---- Cross-inventory operations --------------------------------------
    // Moves up to `count` from src.slot to dst.slot. Validates the destination
    // filter and capacity; returns the moved count (0 when nothing moved).
    static int transfer(Inventory& src, int srcSlot, Inventory& dst,
                        int dstSlot, int count, const ItemRegistry& items,
                        std::string& errorOut);

    // Swaps two slots of one inventory; both slot filters are validated.
    static bool swap(Inventory& inv, int a, int b, const ItemRegistry& items,
                     std::string& errorOut);

    // ---- Events ----------------------------------------------------------
    // Bumps on every successful mutation.
    uint64_t version() const noexcept { return version_; }
    void set_change_callback(std::function<void(const Inventory&)> callback) {
        onChange_ = std::move(callback);
    }

    // ---- Undo/redo history (META section 14, task E.3) ------------------
    // Transactional: every successful slot mutation pushes the pre-mutation
    // slot vector, so undo()/redo() restore exact contents with no loss or
    // duplication. Filters are structural (not part of the undoable state).
    // A new mutation clears the redo tail (standard linear history). Undo and
    // redo are themselves mutations (they bump version + fire the callback).
    bool can_undo() const noexcept { return !undoStack_.empty(); }
    bool can_redo() const noexcept { return !redoStack_.empty(); }
    // Restores the slot contents to the state before the last mutation.
    bool undo();
    // Re-applies the last undone mutation.
    bool redo();
    void clear_history() noexcept;

    // ---- Serialization (data-driven, ready for replication) --------------
    // {"version":1,"slots":[{"item":"...","count":N,"damage":D,"data":"..."}, null, ...]}
    // Unknown versions are refused on load (never guessed).
    std::string serialize_json() const;
    // All-or-nothing: parses and validates every slot against the registry and
    // the filters before mutating anything.
    bool deserialize_json(const std::string& jsonText, const ItemRegistry& items,
                          std::string& errorOut);

private:
    int max_stack_of(const ItemStack& stack, const ItemRegistry& items) const;
    bool slot_accepts(int slot, const ItemStack& stack,
                      const ItemRegistry& items, std::string& errorOut) const;
    void bump();
    void push_undo(std::vector<ItemStack>&& before);

    std::vector<ItemStack> slots_;
    std::vector<SlotFilter> filters_;
    std::vector<std::vector<ItemStack>> undoStack_;
    std::vector<std::vector<ItemStack>> redoStack_;
    uint64_t version_{ 0 };
    std::function<void(const Inventory&)> onChange_;
};

}  // namespace registry
}  // namespace engine

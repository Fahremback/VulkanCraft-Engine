#pragma once

// Data-driven item stack (SDK, META section 14). The canonical identity of a
// stack is the namespaced item id (e.g. "vulkancraft:cobblestone") — never a
// numeric id, so stacks survive reorder, updates, save/load and multiplayer.
// Counts clamp to the item registry's maxStack at every mutation.
//
// The stack is extensible by components: `damage` tracks durability already
// used and `data` is an opaque payload the project may own (enchantments,
// NBT-like data). Two stacks merge only when item, damage and data agree.

#include <algorithm>
#include <string>

namespace engine {
namespace registry {

struct ItemStack {
    std::string item;   // namespaced id, e.g. "vulkancraft:cobblestone"
    int count{ 0 };
    int damage{ 0 };    // durability already used (0 = pristine)
    std::string data;   // opaque component payload

    bool empty() const { return item.empty() || count <= 0; }
    void clear() {
        item.clear();
        count = 0;
        damage = 0;
        data.clear();
    }

    // Two stacks can share a slot only when they are the same item with the
    // same damage and the same data payload (a damaged tool never merges into
    // a pristine one).
    static bool can_merge(const ItemStack& a, const ItemStack& b) {
        return a.item == b.item && a.damage == b.damage && a.data == b.data;
    }

    // How many more units fit in this stack before maxStack.
    int capacity_left(int maxStack) const {
        return std::max(0, maxStack - count);
    }

    // Adds up to `amount` units, clamped to maxStack. Returns how many were
    // actually added.
    int add(int amount, int maxStack) {
        const int accepted = std::min(amount, capacity_left(maxStack));
        count += accepted;
        return accepted;
    }

    // Splits `amount` units off this stack into a new stack. amount <= 0
    // yields an empty stack without changing this one; amount >= count moves
    // the whole stack (this becomes empty).
    ItemStack split(int amount) {
        ItemStack out;
        if (amount <= 0 || count <= 0) return out;
        const int taken = std::min(amount, count);
        out = *this;
        out.count = taken;
        count -= taken;
        if (count == 0) clear();
        return out;
    }
};

}  // namespace registry
}  // namespace engine

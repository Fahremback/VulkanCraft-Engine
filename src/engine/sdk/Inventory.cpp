// Inventory.cpp — authoritative slot container (META section 14).
// Every mutation validates the slot filter and clamps to the item registry's
// maxStack; operations are all-or-nothing and never lose or duplicate items.

#include "engine/registry/Inventory.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <sstream>

namespace engine {
namespace registry {

namespace {

// Version of the serialized inventory format. Bump on incompatible changes;
// the loader rejects unknown versions instead of guessing.
constexpr int kSerializedVersion = 1;

// Minimal JSON string escaping for the emitter.
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

}  // namespace

Inventory::Inventory(int slotCount) {
    if (slotCount < 0) slotCount = 0;
    slots_.resize(static_cast<std::size_t>(slotCount));
    filters_.resize(static_cast<std::size_t>(slotCount));
}

void Inventory::set_filter(int slot, SlotFilter filter) {
    if (slot < 0 || slot >= slot_count()) return;
    filters_[static_cast<std::size_t>(slot)] = std::move(filter);
}

const SlotFilter& Inventory::filter(int slot) const {
    static const SlotFilter kLocked;
    if (slot < 0 || slot >= slot_count()) return kLocked;
    return filters_[static_cast<std::size_t>(slot)];
}

const ItemStack& Inventory::get(int slot) const {
    static const ItemStack kEmpty;
    if (slot < 0 || slot >= slot_count()) return kEmpty;
    return slots_[static_cast<std::size_t>(slot)];
}

int Inventory::max_stack_of(const ItemStack& stack, const ItemRegistry& items) const {
    if (stack.item.empty()) return 1;
    const ItemDefinition* def = items.find_by_name(stack.item);
    return def == nullptr ? 1 : def->maxStack;
}

bool Inventory::slot_accepts(int slot, const ItemStack& stack,
                             const ItemRegistry& items,
                             std::string& errorOut) const {
    if (stack.empty()) return true;  // clearing is always allowed
    const ItemDefinition* def = items.find_by_name(stack.item);
    if (def == nullptr) {
        errorOut = "inventory: unknown item '" + stack.item + "'";
        return false;
    }
    if (!filter(slot).accepts(*def)) {
        errorOut = "inventory: slot " + std::to_string(slot) +
                   " rejects item '" + stack.item + "'";
        return false;
    }
    return true;
}

void Inventory::bump() {
    ++version_;
    if (onChange_) onChange_(*this);
}

bool Inventory::set(int slot, const ItemStack& stack, const ItemRegistry& items,
                    std::string& errorOut) {
    if (slot < 0 || slot >= slot_count()) {
        errorOut = "inventory: slot " + std::to_string(slot) + " out of range";
        return false;
    }
    if (!slot_accepts(slot, stack, items, errorOut)) return false;
    if (stack.empty()) {
        slots_[static_cast<std::size_t>(slot)].clear();
    } else {
        ItemStack clamped = stack;
        clamped.count = std::min(clamped.count, max_stack_of(stack, items));
        if (clamped.count <= 0) clamped.clear();
        slots_[static_cast<std::size_t>(slot)] = clamped;
    }
    bump();
    errorOut.clear();
    return true;
}

ItemStack Inventory::add(const ItemStack& stack, const ItemRegistry& items,
                         std::string& errorOut) {
    if (stack.empty() || stack.count <= 0) {
        errorOut.clear();
        return {};
    }
    const ItemDefinition* def = items.find_by_name(stack.item);
    if (def == nullptr) {
        errorOut = "inventory: unknown item '" + stack.item + "'";
        return stack;  // nothing added, nothing lost
    }
    const int maxStack = def->maxStack;

    ItemStack remainder = stack;
    bool changed = false;
    // Pass 1: merge into matching stacks that still have room.
    for (int s = 0; s < slot_count() && !remainder.empty(); ++s) {
        ItemStack& slot = slots_[static_cast<std::size_t>(s)];
        if (slot.empty() || !ItemStack::can_merge(slot, remainder)) continue;
        if (!filter(s).accepts(*def)) continue;
        const int added = slot.add(remainder.count, maxStack);
        remainder.count -= added;
        if (added > 0) changed = true;
        if (remainder.count <= 0) remainder.clear();
    }
    // Pass 2: place into empty accepted slots.
    for (int s = 0; s < slot_count() && !remainder.empty(); ++s) {
        ItemStack& slot = slots_[static_cast<std::size_t>(s)];
        if (!slot.empty()) continue;
        if (!filter(s).accepts(*def)) continue;
        slot = remainder;
        slot.count = std::min(slot.count, maxStack);
        remainder.count -= slot.count;
        changed = true;
        if (remainder.count <= 0) remainder.clear();
    }
    if (changed) bump();
    errorOut.clear();
    return remainder;
}

int Inventory::remove(const std::string& item, int count, const ItemRegistry&,
                      std::string& errorOut) {
    if (count <= 0 || item.empty()) {
        errorOut.clear();
        return 0;
    }
    int removed = 0;
    for (int s = 0; s < slot_count() && removed < count; ++s) {
        ItemStack& slot = slots_[static_cast<std::size_t>(s)];
        if (slot.empty() || slot.item != item) continue;
        const int take = std::min(count - removed, slot.count);
        slot.count -= take;
        removed += take;
        if (slot.count <= 0) slot.clear();
    }
    if (removed > 0) bump();
    errorOut.clear();
    return removed;
}

int Inventory::consume(int slot, int amount) {
    if (slot < 0 || slot >= slot_count() || amount <= 0) return 0;
    ItemStack& stack = slots_[static_cast<std::size_t>(slot)];
    if (stack.empty()) return 0;
    const int taken = std::min(amount, stack.count);
    stack.count -= taken;
    if (stack.count <= 0) stack.clear();
    bump();
    return taken;
}

int Inventory::count_of(const std::string& item) const {
    int total = 0;
    for (const ItemStack& stack : slots_) {
        if (!stack.empty() && stack.item == item) total += stack.count;
    }
    return total;
}

int Inventory::transfer(Inventory& src, int srcSlot, Inventory& dst, int dstSlot,
                        int count, const ItemRegistry& items,
                        std::string& errorOut) {
    if (count <= 0) {
        errorOut.clear();
        return 0;
    }
    if (srcSlot < 0 || srcSlot >= src.slot_count() || dstSlot < 0 ||
        dstSlot >= dst.slot_count()) {
        errorOut = "inventory: transfer slot out of range";
        return 0;
    }
    ItemStack& source = src.slots_[static_cast<std::size_t>(srcSlot)];
    if (source.empty()) {
        errorOut = "inventory: source slot is empty";
        return 0;
    }
    const ItemDefinition* def = items.find_by_name(source.item);
    if (def == nullptr) {
        errorOut = "inventory: unknown item '" + source.item + "'";
        return 0;
    }
    if (!dst.filter(dstSlot).accepts(*def)) {
        errorOut = "inventory: destination slot rejects item '" + source.item + "'";
        return 0;
    }
    ItemStack& target = dst.slots_[static_cast<std::size_t>(dstSlot)];
    int space = 0;
    if (target.empty()) {
        space = def->maxStack;
    } else if (ItemStack::can_merge(target, source)) {
        space = target.capacity_left(def->maxStack);
    } else {
        errorOut = "inventory: destination slot holds a different item";
        return 0;
    }
    if (space <= 0) {
        errorOut.clear();
        return 0;
    }
    const int moved = std::min(count, std::min(space, source.count));
    if (moved <= 0) {
        errorOut.clear();
        return 0;
    }
    source.count -= moved;
    if (source.count <= 0) source.clear();
    target.count += moved;
    src.bump();
    dst.bump();
    errorOut.clear();
    return moved;
}

bool Inventory::swap(Inventory& inv, int a, int b, const ItemRegistry& items,
                     std::string& errorOut) {
    if (a < 0 || a >= inv.slot_count() || b < 0 || b >= inv.slot_count()) {
        errorOut = "inventory: swap slot out of range";
        return false;
    }
    if (a == b) {
        errorOut.clear();
        return true;
    }
    ItemStack& stackA = inv.slots_[static_cast<std::size_t>(a)];
    ItemStack& stackB = inv.slots_[static_cast<std::size_t>(b)];
    // The destination of each side must accept the incoming item.
    if (!inv.slot_accepts(b, stackA, items, errorOut)) return false;
    if (!inv.slot_accepts(a, stackB, items, errorOut)) return false;
    std::swap(stackA, stackB);
    inv.bump();
    errorOut.clear();
    return true;
}

std::string Inventory::serialize_json() const {
    std::ostringstream out;
    out << "{\"version\":" << kSerializedVersion << ",\"slots\":[";
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (i != 0) out << ',';
        const ItemStack& stack = slots_[i];
        if (stack.empty()) {
            out << "null";
        } else {
            out << "{\"item\":\"" << json_escape(stack.item) << "\",\"count\":"
                << stack.count << ",\"damage\":" << stack.damage << ",\"data\":\""
                << json_escape(stack.data) << "\"}";
        }
    }
    out << "]}";
    return out.str();
}

bool Inventory::deserialize_json(const std::string& jsonText,
                                 const ItemRegistry& items, std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut)) return false;
    if (!root.is_object()) {
        errorOut = "inventory asset must be an object";
        return false;
    }
    const double version = sdk::json_number(root, "version", 1.0);
    if (version != static_cast<double>(kSerializedVersion)) {
        errorOut = "inventory asset version " +
                   std::to_string(static_cast<int>(version)) +
                   " is unsupported (expected " +
                   std::to_string(kSerializedVersion) + ")";
        return false;
    }
    const sdk::JsonValue* slots = root.field("slots");
    if (slots == nullptr || !slots->is_array()) {
        errorOut = "inventory asset requires a 'slots' array";
        return false;
    }
    if (slots->array.size() != slots_.size()) {
        errorOut = "inventory asset has " + std::to_string(slots->array.size()) +
                   " slots but the container holds " +
                   std::to_string(slots_.size());
        return false;
    }

    // Validate everything into a temporary before mutating (all-or-nothing).
    std::vector<ItemStack> next = slots_;
    std::vector<SlotFilter> nextFilters = filters_;
    for (std::size_t i = 0; i < slots->array.size(); ++i) {
        const sdk::JsonValue& entry = slots->array[i];
        if (!entry.is_object()) continue;  // null = empty slot
        ItemStack stack;
        stack.item = sdk::json_string(entry, "item", "");
        stack.count = static_cast<int>(sdk::json_number(entry, "count", 0.0));
        stack.damage = static_cast<int>(sdk::json_number(entry, "damage", 0.0));
        stack.data = sdk::json_string(entry, "data", "");
        if (stack.item.empty()) {
            errorOut = "inventory: slot " + std::to_string(i) +
                       " has no 'item'";
            return false;
        }
        const ItemDefinition* def = items.find_by_name(stack.item);
        if (def == nullptr) {
            errorOut = "inventory: unknown item '" + stack.item + "'";
            return false;
        }
        if (stack.count < 1 || stack.count > def->maxStack) {
            errorOut = "inventory: slot " + std::to_string(i) + " count " +
                       std::to_string(stack.count) + " out of [1, " +
                       std::to_string(def->maxStack) + "]";
            return false;
        }
        if (!nextFilters[i].accepts(*def)) {
            errorOut = "inventory: slot " + std::to_string(i) +
                       " rejects item '" + stack.item + "'";
            return false;
        }
        next[i] = stack;
    }
    slots_ = std::move(next);
    filters_ = std::move(nextFilters);
    bump();
    errorOut.clear();
    return true;
}

}  // namespace registry
}  // namespace engine

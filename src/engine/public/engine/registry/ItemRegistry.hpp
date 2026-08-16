#pragma once

// Data-driven item registry (SDK). Items are defined by assets (JSON) or
// programmatically; identities are persistent UUIDs, so ids survive reorder,
// updates, save/load and multiplayer. Invalid assets fail with a clear
// diagnostic and fall back safely.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace registry {

// Use/equipment/behavior components (FALTANTES item 8). Items are metadata +
// validation today; the actual gameplay consumers (weapon/armor/behavior
// runtimes) resolve these fields in the abilities milestone (Fase 6).
enum class ItemUseMode { None, Instant, Continuous };
enum class ItemEquipSlot { None, Hand, OffHand, Head, Chest, Legs, Feet };

struct ItemDefinition {
    // Persistent canonical UUID. Empty means "derive a stable id from ns:name".
    std::string uuid;
    std::string ns{ "vulkancraft" };
    std::string name;

    int maxStack{ 64 };
    int durability{ 0 };  // 0 = unbreakable
    std::string icon;     // asset path or texture name
    std::string model;    // asset path

    // Use component: how the item is used when activated.
    int useCooldownMs{ 0 };              // 0..60000
    ItemUseMode useMode{ ItemUseMode::None };

    // Equipment component: where it equips and the combat/armor stats.
    ItemEquipSlot equipSlot{ ItemEquipSlot::None };
    float attackDamage{ 0.0f };          // 0..100
    float armor{ 0.0f };                 // 0..100

    // Behavior component: namespaced reference (ns:name) to a registered
    // behavior id; empty = no behavior. Validated, not resolved, at registry
    // time (resolution belongs to the abilities milestone).
    std::string behaviorId;

    std::vector<std::string> tags;
    int32_t version{ 1 };

    std::string namespaced() const { return ns + ":" + name; }
};

// Registry JSON schema (single object or array of objects):
// {
//   "id": "00000000-0000-0000-0000-000000000001",   // optional; derived from ns:name when absent
//   "name": "stone_pickaxe",                        // required
//   "namespace": "vulkancraft",
//   "maxStack": 1,
//   "durability": 250,
//   "icon": "textures/items/stone_pickaxe.png",
//   "model": "models/items/stone_pickaxe.gltf",
//   "useCooldown": 300,                              // 0..60000
//   "useMode": "instant",                           // none|instant|continuous
//   "equipSlot": "hand",                            // none|hand|offhand|head|chest|legs|feet
//   "attackDamage": 4.0,                             // 0..100
//   "armor": 0.0,                                    // 0..100
//   "behaviorId": "vulkancraft:mine",               // optional namespaced ref
//   "tags": ["tool", "pickaxe"],
//   "version": 1
// }

// Registry JSON schema (single object or array of objects):
// {
//   "id": "00000000-0000-0000-0000-000000000001",   // optional; derived from ns:name when absent
//   "name": "stone_pickaxe",                        // required
//   "namespace": "vulkancraft",
//   "maxStack": 1,
//   "durability": 250,
//   "icon": "textures/items/stone_pickaxe.png",
//   "model": "models/items/stone_pickaxe.gltf",
//   "tags": ["tool", "pickaxe"],
//   "version": 1
// }
class ItemRegistry {
public:
    ItemRegistry();

    bool register_item(const ItemDefinition& definition, std::string& errorOut);
    bool load_from_json(const std::string& jsonText, std::string& errorOut);

    const ItemDefinition* find_by_uuid(const std::string& uuid) const;
    const ItemDefinition* find_by_name(const std::string& namespacedName) const;

    const ItemDefinition& fallback() const { return fallback_; }
    std::size_t size() const { return byUuid_.size(); }
    std::vector<std::string> all_names() const;

private:
    bool add(ItemDefinition definition, std::string& errorOut);

    std::unordered_map<std::string, ItemDefinition> byUuid_;
    std::unordered_map<std::string, ItemDefinition> byName_;
    ItemDefinition fallback_;
};

}  // namespace registry
}  // namespace engine

#pragma once

#include "Voxel.hpp"
#include <string>
#include <vector>

enum class ItemType {
    None,
    BlockItem,
    WoodPickaxe,
    StonePickaxe,
    IronPickaxe,
    DiamondPickaxe,
    WoodSword,
    IronSword,
    DiamondSword,
    Coal,
    IronIngot,
    GoldIngot,
    Diamond,
    Apple,
    Meat
};

struct ItemStack {
    ItemType type{ ItemType::None };
    BlockType blockType{ BlockType::Air };
    int count{ 0 };
    int maxStack{ 64 };
};

class Inventory {
public:
    std::vector<ItemStack> slots;
    int selectedSlot{ 0 };

    Inventory() {
        slots.resize(36); // 9 slots na hotbar + 27 no inventário
        
        // Itens iniciais para teste imediato do jogador
        slots[0] = { ItemType::BlockItem, BlockType::Grass, 64 };
        slots[1] = { ItemType::BlockItem, BlockType::Stone, 64 };
        slots[2] = { ItemType::BlockItem, BlockType::WoodOak, 64 };
        slots[3] = { ItemType::BlockItem, BlockType::PlanksOak, 64 };
        slots[4] = { ItemType::BlockItem, BlockType::Glass, 64 };
        slots[5] = { ItemType::BlockItem, BlockType::Water, 64 };
        slots[6] = { ItemType::DiamondPickaxe, BlockType::Air, 1 };
        slots[7] = { ItemType::DiamondSword, BlockType::Air, 1 };
        slots[8] = { ItemType::BlockItem, BlockType::Glowstone, 32 };
    }

    bool add_item(ItemType type, BlockType block, int amount = 1) {
        for (auto& slot : slots) {
            if (slot.type == type && slot.blockType == block && slot.count < slot.maxStack) {
                slot.count += amount;
                return true;
            }
        }
        for (auto& slot : slots) {
            if (slot.type == ItemType::None) {
                slot.type = type;
                slot.blockType = block;
                slot.count = amount;
                return true;
            }
        }
        return false;
    }

    ItemStack get_selected_item() const {
        return slots[selectedSlot];
    }
};

#pragma once

#include "Voxel.hpp"
#include <glm/glm.hpp>
#include <functional>

class World;

enum class StructureType {
    SunTemple,
    FloatingWizardCitadel,
    CrystalLavaDungeon,
    OvergrownRuins,
    UnderwaterAtlantis
};

class Structures {
public:
    static void generate_structure(StructureType type, int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn);
    
    static void build_sun_temple(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn);
    static void build_wizard_citadel(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn);
    static void build_crystal_dungeon(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn);
    static void build_overgrown_ruins(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn);
    static void build_underwater_atlantis(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn);
};

#include "Structures.hpp"

void Structures::generate_structure(StructureType type, int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn) {
    switch (type) {
    case StructureType::SunTemple:
        build_sun_temple(startX, startY, startZ, set_block_fn);
        break;
    case StructureType::FloatingWizardCitadel:
        build_wizard_citadel(startX, startY, startZ, set_block_fn);
        break;
    case StructureType::CrystalLavaDungeon:
        build_crystal_dungeon(startX, startY, startZ, set_block_fn);
        break;
    case StructureType::OvergrownRuins:
        build_overgrown_ruins(startX, startY, startZ, set_block_fn);
        break;
    case StructureType::UnderwaterAtlantis:
        build_underwater_atlantis(startX, startY, startZ, set_block_fn);
        break;
    }
}

// 1. Templo do Sol (Pirâmide de Arenito com Câmara do Tesouro e Armadilhas)
void Structures::build_sun_temple(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn) {
    int size = 15;
    for (int step = 0; step < 7; step++) {
        int curSize = size - (step * 2);
        for (int dx = 0; dx < curSize; dx++) {
            for (int dz = 0; dz < curSize; dz++) {
                int x = startX + step + dx;
                int y = startY + step;
                int z = startZ + step + dz;

                // Paredes externas de Arenito
                if (dx == 0 || dx == curSize - 1 || dz == 0 || dz == curSize - 1) {
                    set_block_fn(x, y, z, BlockType::Sandstone);
                } else if (step == 0) {
                    set_block_fn(x, y, z, BlockType::Sandstone); // Piso
                } else {
                    set_block_fn(x, y, z, BlockType::Air); // Interior oco
                }
            }
        }
    }

    // Altar do Sol e Baú do Tesouro no centro
    int cx = startX + 7;
    int cz = startZ + 7;
    set_block_fn(cx, startY + 1, cz, BlockType::Glowstone);
    set_block_fn(cx, startY + 2, cz, BlockType::Chest);
    set_block_fn(cx - 1, startY + 1, cz, BlockType::TNT);
    set_block_fn(cx + 1, startY + 1, cz, BlockType::TNT);
}

// 2. Cidadela Flutuante dos Magos (Ilha no Céu com Blackstone, Glowstone e Pilares)
void Structures::build_wizard_citadel(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn) {
    int radius = 8;
    for (int dx = -radius; dx <= radius; dx++) {
        for (int dz = -radius; dz <= radius; dz++) {
            if (dx * dx + dz * dz <= radius * radius) {
                // Ilha de Blackstone e Obsidian no céu (Y ~ 75)
                set_block_fn(startX + dx, startY, startZ + dz, BlockType::Blackstone);
                set_block_fn(startX + dx, startY - 1, startZ + dz, BlockType::Obsidian);
                if (dx * dx + dz * dz <= (radius - 2) * (radius - 2)) {
                    set_block_fn(startX + dx, startY - 2, startZ + dz, BlockType::Stone);
                }
            }
        }
    }

    // Pilares de Glowstone e Torres de Encantamento
    int corners[4][2] = { {-5, -5}, {5, -5}, {-5, 5}, {5, 5} };
    for (auto& c : corners) {
        for (int h = 1; h <= 6; h++) {
            set_block_fn(startX + c[0], startY + h, startZ + c[1], BlockType::Blackstone);
        }
        set_block_fn(startX + c[0], startY + 7, startZ + c[1], BlockType::Glowstone);
    }

    // Altar central com livros e baú de magia
    set_block_fn(startX, startY + 1, startZ, BlockType::CraftingTable);
    set_block_fn(startX - 1, startY + 1, startZ, BlockType::Bookshelf);
    set_block_fn(startX + 1, startY + 1, startZ, BlockType::Bookshelf);
    set_block_fn(startX, startY + 1, startZ - 1, BlockType::Chest);
}

// 3. Calabouço Volcânico de Cristal (Basalto, Bloco de Magma, Quartzo e Minérios Ráros)
void Structures::build_crystal_dungeon(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn) {
    for (int dx = 0; dx < 10; dx++) {
        for (int dy = 0; dy < 6; dy++) {
            for (int dz = 0; dz < 10; dz++) {
                int x = startX + dx;
                int y = startY + dy;
                int z = startZ + dz;

                if (dx == 0 || dx == 9 || dz == 0 || dz == 9 || dy == 0 || dy == 5) {
                    set_block_fn(x, y, z, (dy == 0) ? BlockType::MagmaBlock : BlockType::Basalt);
                } else {
                    set_block_fn(x, y, z, BlockType::Air);
                }
            }
        }
    }

    // Minérios raros e Spawner de Monstro
    set_block_fn(startX + 5, startY + 1, startZ + 5, BlockType::DiamondOre);
    set_block_fn(startX + 4, startY + 1, startZ + 5, BlockType::GoldOre);
    set_block_fn(startX + 5, startY + 1, startZ + 4, BlockType::Chest);
    set_block_fn(startX + 2, startY + 2, startZ + 2, BlockType::Lava);
    set_block_fn(startX + 7, startY + 2, startZ + 7, BlockType::Lava);
}

// 4. Ruínas Antigas Sobrevividas (Pedregulho Musgoso, Videiras e Estátuas)
void Structures::build_overgrown_ruins(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn) {
    // Arcos de Pedregulho Musgoso
    for (int h = 0; h < 5; h++) {
        set_block_fn(startX, startY + h, startZ, BlockType::MossyCobble);
        set_block_fn(startX + 6, startY + h, startZ, BlockType::MossyCobble);
        set_block_fn(startX, startY + h, startZ + 6, BlockType::MossyCobble);
        set_block_fn(startX + 6, startY + h, startZ + 6, BlockType::MossyCobble);
    }
    for (int x = 0; x <= 6; x++) {
        set_block_fn(startX + x, startY + 5, startZ, BlockType::MossyCobble);
        set_block_fn(startX + x, startY + 5, startZ + 6, BlockType::MossyCobble);
    }

    set_block_fn(startX + 3, startY + 1, startZ + 3, BlockType::Chest);
    set_block_fn(startX + 3, startY + 1, startZ + 2, BlockType::EmeraldOre);
}

// 5. Santuário Submerso de Atlântida (Prismarina e Lanternas do Mar)
void Structures::build_underwater_atlantis(int startX, int startY, int startZ, std::function<void(int, int, int, BlockType)> set_block_fn) {
    int r = 6;
    for (int dx = -r; dx <= r; dx++) {
        for (int dz = -r; dz <= r; dz++) {
            if (dx * dx + dz * dz <= r * r) {
                set_block_fn(startX + dx, startY, startZ + dz, BlockType::Prismarine);
                if (dx * dx + dz * dz == r * r) {
                    for (int h = 1; h <= 4; h++) {
                        set_block_fn(startX + dx, startY + h, startZ + dz, BlockType::Prismarine);
                    }
                    set_block_fn(startX + dx, startY + 5, startZ + dz, BlockType::SeaLantern);
                }
            }
        }
    }
    set_block_fn(startX, startY + 1, startZ, BlockType::SeaLantern);
    set_block_fn(startX, startY + 2, startZ, BlockType::Chest);
}

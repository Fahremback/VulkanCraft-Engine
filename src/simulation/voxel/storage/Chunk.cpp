#include "Chunk.hpp"
#include "World.hpp"
#include "TerrainGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

SparseVoxelSection::SparseVoxelSection() {
    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
        for (int y = 0; y < VERTICAL_SECTION_SIZE; ++y) {
            for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                blocks[x][y][z] = kRuntimeAirId;
                waterLevels[x][y][z] = WATER_LEVEL_NONE;
            }
        }
    }
}

Chunk::Chunk(int cx, int cz, uint32_t generation)
    : chunkX(cx), chunkZ(cz), id_{{cx, cz}, generation} {
    constexpr std::size_t denseVoxelCount =
        static_cast<std::size_t>(CHUNK_SIZE_X) * GENERATED_TERRAIN_HEIGHT * CHUNK_SIZE_Z;
    std::fill_n(&blocks[0][0][0], denseVoxelCount, kRuntimeAirId);
    std::fill_n(&waterLevels[0][0][0], denseVoxelCount, WATER_LEVEL_NONE);
}

RuntimeBlockId Chunk::get_block(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) {
        return kRuntimeAirId;
    }
    if (y < GENERATED_TERRAIN_HEIGHT) {
        const auto changed = voxelOverrides.find(dense_key(x, y, z));
        return changed == voxelOverrides.end() ? blocks[x][y][z] : changed->second.type;
    }
    const int sectionIndex = y / VERTICAL_SECTION_SIZE;
    const auto found = upperSections.find(sectionIndex);
    if (found == upperSections.end()) return kRuntimeAirId;
    return found->second->blocks[x][y % VERTICAL_SECTION_SIZE][z];
}

void Chunk::set_block(int x, int y, int z, RuntimeBlockId type) {
    const RuntimeBlockId waterId = runtime_id(BlockType::Water);
    if (x >= 0 && x < CHUNK_SIZE_X && y >= 0 && y < CHUNK_SIZE_Y && z >= 0 && z < CHUNK_SIZE_Z) {
        if (y >= GENERATED_TERRAIN_HEIGHT) {
            const int sectionIndex = y / VERTICAL_SECTION_SIZE;
            auto found = upperSections.find(sectionIndex);
            if (found == upperSections.end()) {
                if (type == kRuntimeAirId) return;
                found = upperSections.emplace(sectionIndex, std::make_unique<SparseVoxelSection>()).first;
            }
            SparseVoxelSection& section = *found->second;
            section.blocks[x][y % VERTICAL_SECTION_SIZE][z] = type;
            section.waterLevels[x][y % VERTICAL_SECTION_SIZE][z] =
                type == waterId ? WATER_SOURCE_LEVEL : WATER_LEVEL_NONE;
            if (type != kRuntimeAirId) highestOccupiedY = std::max(highestOccupiedY, y);
            dataVersion.fetch_add(1, std::memory_order_release);
            return;
        }
        const uint32_t key = dense_key(x, y, z);
        const uint8_t level = type == waterId ? WATER_SOURCE_LEVEL : WATER_LEVEL_NONE;
        if (blocks[x][y][z] == type && waterLevels[x][y][z] == level) voxelOverrides.erase(key);
        else voxelOverrides[key] = VoxelOverride{ type, level };
        if (type != kRuntimeAirId) highestOccupiedY = std::max(highestOccupiedY, y);
        dataVersion.fetch_add(1, std::memory_order_release);
    }
}

uint8_t Chunk::get_sky_light(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) {
        return 0;
    }
    return static_cast<uint8_t>(y > skyOcclusionTop[x][z] ? 15 : 0);
}

uint8_t Chunk::get_block_light(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) {
        return 0;
    }
    const auto found = blockLight.find(light_key(x, y, z));
    return found == blockLight.end() ? 0 : found->second;
}

uint8_t Chunk::get_water_level(int x, int y, int z) const {
    const RuntimeBlockId waterId = runtime_id(BlockType::Water);
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) return WATER_LEVEL_NONE;
    if (y < GENERATED_TERRAIN_HEIGHT) {
        const auto changed = voxelOverrides.find(dense_key(x, y, z));
        if (changed != voxelOverrides.end())
            return changed->second.type == waterId ? changed->second.waterLevel : WATER_LEVEL_NONE;
        return blocks[x][y][z] == waterId ? waterLevels[x][y][z] : WATER_LEVEL_NONE;
    }
    const int sectionIndex = y / VERTICAL_SECTION_SIZE;
    const auto found = upperSections.find(sectionIndex);
    if (found == upperSections.end()) return WATER_LEVEL_NONE;
    const int localY = y % VERTICAL_SECTION_SIZE;
    return found->second->blocks[x][localY][z] == waterId
        ? found->second->waterLevels[x][localY][z] : WATER_LEVEL_NONE;
}

void Chunk::set_water(int x, int y, int z, uint8_t level) {
    const RuntimeBlockId waterId = runtime_id(BlockType::Water);
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) return;
    if (y < GENERATED_TERRAIN_HEIGHT) {
        const uint32_t key = dense_key(x, y, z);
        if (blocks[x][y][z] == waterId && waterLevels[x][y][z] == level) voxelOverrides.erase(key);
        else voxelOverrides[key] = VoxelOverride{ waterId, level };
        highestOccupiedY = std::max(highestOccupiedY, y);
        dataVersion.fetch_add(1, std::memory_order_release);
        return;
    }
    const int sectionIndex = y / VERTICAL_SECTION_SIZE;
    auto& section = upperSections[sectionIndex];
    if (!section) section = std::make_unique<SparseVoxelSection>();
    const int localY = y % VERTICAL_SECTION_SIZE;
    section->blocks[x][localY][z] = waterId;
    section->waterLevels[x][localY][z] = level;
    highestOccupiedY = std::max(highestOccupiedY, y);
    dataVersion.fetch_add(1, std::memory_order_release);
}

uint8_t Chunk::get_fluid_level(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) {
        return WATER_LEVEL_NONE;
    }
    if (y < GENERATED_TERRAIN_HEIGHT) {
        const auto changed = voxelOverrides.find(dense_key(x, y, z));
        if (changed != voxelOverrides.end()) return changed->second.waterLevel;
        return waterLevels[x][y][z];
    }
    const int sectionIndex = y / VERTICAL_SECTION_SIZE;
    const auto found = upperSections.find(sectionIndex);
    if (found == upperSections.end()) return WATER_LEVEL_NONE;
    return found->second->waterLevels[x][y % VERTICAL_SECTION_SIZE][z];
}

void Chunk::set_fluid_level(int x, int y, int z, uint8_t level) {
    if (x < 0 || x >= CHUNK_SIZE_X || y < 0 || y >= CHUNK_SIZE_Y || z < 0 || z >= CHUNK_SIZE_Z) return;
    if (y < GENERATED_TERRAIN_HEIGHT) {
        // The override keeps the CELL'S CURRENT block id so water-guarded
        // reads still answer NONE for non-water fluids (the byte belongs to
        // whatever block occupies the cell).
        const uint32_t key = dense_key(x, y, z);
        // Read through overrides: the cell may already be a placed fluid
        // (override) whose base array entry is still Air.
        const RuntimeBlockId type = get_block(x, y, z);
        if (waterLevels[x][y][z] == level) voxelOverrides.erase(key);
        else voxelOverrides[key] = VoxelOverride{ type, level };
        highestOccupiedY = std::max(highestOccupiedY, y);
        dataVersion.fetch_add(1, std::memory_order_release);
        return;
    }
    const int sectionIndex = y / VERTICAL_SECTION_SIZE;
    auto& section = upperSections[sectionIndex];
    if (!section) section = std::make_unique<SparseVoxelSection>();
    // Unlike set_water, the block is NOT rewritten here: the caller already
    // placed the fluid block; this only records its level.
    section->waterLevels[x][y % VERTICAL_SECTION_SIZE][z] = level;
    highestOccupiedY = std::max(highestOccupiedY, y);
    dataVersion.fetch_add(1, std::memory_order_release);
}

void Chunk::generate_terrain(const FastNoiseLite& noise, engine::voxel::IVoxelGenerator* generator) {
    (void)noise;
    highestOccupiedY = 0;
    int worldOffsetX = chunkX * CHUNK_SIZE_X;
    int worldOffsetZ = chunkZ * CHUNK_SIZE_Z;

    // Public SDK generator override: a registered generator replaces the
    // builtin height/cave/ore sampling (biomes clamp to a neutral index so
    // custom generators stay safe). nullptr keeps the builtin TerrainGenerator.
    const auto sample_terrain = [generator](float wx, float wz) -> TerrainSample {
        if (generator) {
            const engine::voxel::TerrainPoint point = generator->sample(wx, wz);
            TerrainSample sample;
            sample.height = point.height;
            sample.biome = static_cast<BiomeType>(std::min<unsigned>(
                point.biomeIndex, static_cast<unsigned>(BiomeType::Count) - 1u));
            sample.temperature = point.temperature;
            sample.moisture = point.moisture;
            sample.continentalness = point.continentalness;
            sample.river = point.river;
            sample.erosion = point.erosion;
            sample.weirdness = point.weirdness;
            sample.slope = point.slope;
            return sample;
        }
        return TerrainGenerator::sample(wx, wz);
    };
    const auto sample_cave = [generator](float wx, float wy, float wz) -> float {
        return generator ? generator->cave_density(wx, wy, wz)
                         : TerrainGenerator::cave_density(wx, wy, wz);
    };
    const auto sample_ore = [generator](float wx, float wy, float wz) -> float {
        return generator ? generator->ore_density(wx, wy, wz)
                         : TerrainGenerator::ore_density(wx, wy, wz);
    };

    constexpr int seaLevel = TerrainGenerator::SeaLevel;
    int heightMap[CHUNK_SIZE_X][CHUNK_SIZE_Z]{};
    BiomeType biomeMap[CHUNK_SIZE_X][CHUNK_SIZE_Z]{};

    for (int x = 0; x < CHUNK_SIZE_X; x++) {
        for (int z = 0; z < CHUNK_SIZE_Z; z++) {
            float worldX = static_cast<float>(worldOffsetX + x);
            float worldZ = static_cast<float>(worldOffsetZ + z);

            const TerrainSample terrain = sample_terrain(worldX, worldZ);
            const int height = terrain.height;
            heightMap[x][z] = height;
            biomeMap[x][z] = terrain.biome;
            highestOccupiedY = std::max(highestOccupiedY,
                height < seaLevel ? seaLevel : height);

            auto surface_block = [&](int depth) -> BlockType {
                const bool exposedCliff = terrain.slope > 4.20f;
                switch (terrain.biome) {
                case BiomeType::DeepOcean:
                case BiomeType::Ocean:
                case BiomeType::Coast:
                case BiomeType::River:
                    return depth <= 3 ? BlockType::Sand : BlockType::Sandstone;
                case BiomeType::Desert:
                    return depth <= 4 ? BlockType::Sand : BlockType::Sandstone;
                case BiomeType::DesertOasis:
                    return depth == 0 ? BlockType::Grass : (depth <= 3 ? BlockType::Dirt : BlockType::Sandstone);
                case BiomeType::Badlands:
                    if (depth <= 7) return ((height - depth) / 3) % 2 == 0 ? BlockType::Terracotta : BlockType::Sandstone;
                    return BlockType::Stone;
                case BiomeType::Savanna:
                    if (exposedCliff) return BlockType::Terracotta;
                    return depth == 0 ? BlockType::Grass : (depth <= 2 ? BlockType::Dirt : BlockType::Terracotta);
                case BiomeType::Volcanic:
                case BiomeType::VolcanicCrater:
                    return depth <= 1 ? BlockType::Blackstone : (depth <= 7 ? BlockType::Basalt : BlockType::Stone);
                case BiomeType::Alpine:
                case BiomeType::Glacial:
                    return depth == 0 ? BlockType::SnowBlock : (depth <= 2 ? BlockType::Diorite : BlockType::Stone);
                case BiomeType::RockyMountains:
                    if (depth <= 2) return terrain.temperature < -0.12f && height > 216 ? BlockType::SnowBlock : BlockType::Andesite;
                    return BlockType::Stone;
                case BiomeType::Yosemite:
                    if (depth <= 3) return exposedCliff ? BlockType::Diorite : (depth == 0 ? BlockType::Grass : BlockType::Dirt);
                    return BlockType::Stone;
                case BiomeType::Highlands:
                    if (height >= 252) return depth == 0 ? BlockType::SnowBlock : BlockType::Stone;
                    if (exposedCliff) return BlockType::Stone;
                    return depth == 0 ? BlockType::Grass : (depth <= 3 ? BlockType::Dirt : BlockType::Stone);
                default:
                    if (exposedCliff && depth <= 2) return BlockType::Stone;
                    return depth == 0 ? BlockType::Grass : (depth <= 3 ? BlockType::Dirt : BlockType::Stone);
                }
            };

            // O construtor já publicou ar/NONE em todo o volume. Não percorra
            // as 384 camadas para reescrever ar acima da coluna real.
            const int generatedColumnTop = std::min(std::max(height, seaLevel),
                                                    GENERATED_TERRAIN_HEIGHT - 1);
            for (int y = 0; y <= generatedColumnTop; y++) {
                if (y == 0) {
                    blocks[x][y][z] = runtime_id(BlockType::Bedrock);
                } else if (y <= height) {
                    const int depth = height - y;
                    const bool oceanic = terrain.biome == BiomeType::Ocean || terrain.biome == BiomeType::DeepOcean;
                    // Uma casca estrutural evita que cavernas rasas se juntem e
                    // deixem biomas inteiros suspensos por apenas cinco blocos.
                    const bool caveAllowed = y > 3 && depth > 12 &&
                        !(oceanic && y > seaLevel - 12);
                    if (caveAllowed && sample_cave(worldX, static_cast<float>(y), worldZ) > 0.0f) {
                        if (y <= 7) {
                            blocks[x][y][z] = runtime_id(BlockType::Lava);
                            // Generated lava is a source pool (like ocean
                            // water): it must never read as an unfed cell and
                            // evaporate when the fluid sim touches it, and its
                            // level byte must be canonical in saves (META §13).
                            waterLevels[x][y][z] = WATER_SOURCE_LEVEL;
                        } else {
                            blocks[x][y][z] = kRuntimeAirId;
                        }
                        continue;
                    }

                    BlockType block = surface_block(depth);
                    if (depth > 5 && (block == BlockType::Stone || block == BlockType::Basalt || block == BlockType::Sandstone)) {
                        if (y < 64) block = BlockType::Deepslate;
                        const float geology = sample_ore(worldX * 0.31f, static_cast<float>(y) * 0.31f, worldZ * 0.31f);
                        if (y > 64 && geology > 0.48f && geology < 0.58f) block = BlockType::Granite;
                        else if (y > 64 && geology < -0.52f && geology > -0.63f) block = BlockType::Diorite;
                        else if (y > 54 && std::abs(geology) < 0.045f) block = BlockType::Andesite;
                        const float ore = sample_ore(worldX, static_cast<float>(y), worldZ);
                        if (y < 80 && ore > 0.77f) block = BlockType::DiamondOre;
                        else if (y < 96 && ore > 0.74f) block = BlockType::GoldOre;
                        else if (y < 256 && ore > 0.70f) block = BlockType::IronOre;
                        else if (y < 320 && ore > 0.66f) block = BlockType::CoalOre;
                        else if (y > 80 && y < 176 && ore < -0.72f) block = BlockType::CopperOre;
                    }
                    blocks[x][y][z] = runtime_id(block);
                } else if (y <= seaLevel && height < seaLevel) {
                    blocks[x][y][z] = runtime_id(BlockType::Water);
                    waterLevels[x][y][z] = WATER_SOURCE_LEVEL;
                }
            }
        }
    }

    // Trees are generated only after every terrain column exists. Generating them
    // inside the terrain pass allowed later columns to erase most of each crown.
    auto hash_tree = [](float worldX, float worldZ, float salt) {
        float value = std::sin(worldX * 12.9898f + worldZ * 78.233f + salt * 31.719f) * 43758.5453f;
        return value - std::floor(value);
    };
    auto place_leaf = [&](int px, int py, int pz, RuntimeBlockId leafType) {
        if (px < 0 || px >= CHUNK_SIZE_X || py < 1 || py >= GENERATED_TERRAIN_HEIGHT || pz < 0 || pz >= CHUNK_SIZE_Z) return;
        if (blocks[px][py][pz] == kRuntimeAirId) blocks[px][py][pz] = leafType;
    };

    for (int x = 4; x < CHUNK_SIZE_X - 4; ++x) {
        for (int z = 4; z < CHUNK_SIZE_Z - 4; ++z) {
            const float worldX = static_cast<float>(worldOffsetX + x);
            const float worldZ = static_cast<float>(worldOffsetZ + z);
            const int height = heightMap[x][z];
            if (height <= seaLevel + 2 || blocks[x][height][z] != runtime_id(BlockType::Grass)) continue;

            const float treeChance = hash_tree(worldX, worldZ, 0.0f);
            float treeDensity = 0.0f;
            RuntimeBlockId woodType = runtime_id(BlockType::Wood);
            RuntimeBlockId leafType = runtime_id(BlockType::Leaves);
            switch (biomeMap[x][z]) {
            case BiomeType::Forest: treeDensity = 0.065f; break;
            case BiomeType::Jungle: treeDensity = 0.105f; break;
            case BiomeType::Swamp: treeDensity = 0.045f; break;
            case BiomeType::BirchTaiga: treeDensity = 0.072f; woodType = runtime_id(BlockType::WoodBirch); leafType = runtime_id(BlockType::LeavesBirch); break;
            case BiomeType::Meadow: treeDensity = 0.018f; woodType = runtime_id(BlockType::WoodBirch); leafType = runtime_id(BlockType::LeavesBirch); break;
            case BiomeType::Plains: treeDensity = 0.008f; woodType = runtime_id(BlockType::WoodBirch); leafType = runtime_id(BlockType::LeavesBirch); break;
            case BiomeType::DesertOasis: treeDensity = 0.028f; break;
            case BiomeType::Savanna: treeDensity = 0.016f; break;
            case BiomeType::Highlands: treeDensity = 0.022f; woodType = runtime_id(BlockType::WoodSpruce); leafType = runtime_id(BlockType::LeavesSpruce); break;
            case BiomeType::Yosemite: treeDensity = 0.025f; woodType = runtime_id(BlockType::WoodSpruce); leafType = runtime_id(BlockType::LeavesSpruce); break;
            case BiomeType::Alpine: treeDensity = height < 228 ? 0.020f : 0.0f; woodType = runtime_id(BlockType::WoodSpruce); leafType = runtime_id(BlockType::LeavesSpruce); break;
            default: break;
            }
            if (treeChance >= treeDensity) continue;

            int trunkHeight = 6 + static_cast<int>(hash_tree(worldX, worldZ, 1.0f) * 3.0f);
            if (biomeMap[x][z] == BiomeType::Jungle) trunkHeight += 4;
            if (woodType == runtime_id(BlockType::WoodSpruce)) trunkHeight += 2;
            if (height + trunkHeight + 3 >= GENERATED_TERRAIN_HEIGHT) continue;
            const int crownCenterY = height + trunkHeight - 1;
            highestOccupiedY = std::max(highestOccupiedY, height + trunkHeight + 1);
            for (int ty = 1; ty <= trunkHeight; ++ty) blocks[x][height + ty][z] = woodType;

            const int directions[4][2] = { {1, 0}, {0, 1}, {-1, 0}, {0, -1} };
            const int directionOffset = static_cast<int>(hash_tree(worldX, worldZ, 2.0f) * 4.0f) & 3;
            for (int branch = 0; branch < 3; ++branch) {
                const int* direction = directions[(directionOffset + branch) & 3];
                const int branchY = crownCenterY - 1 + (branch & 1);
                for (int step = 1; step <= 2; ++step) {
                    const int bx = x + direction[0] * step;
                    const int bz = z + direction[1] * step;
                    const int by = branchY + (step > 1 ? 1 : 0);
                    if (blocks[bx][by][bz] == kRuntimeAirId || is_leaf_block(as_builtin_block(blocks[bx][by][bz])))
                        blocks[bx][by][bz] = woodType;
                }
            }

            for (int ly = -2; ly <= 2; ++ly) {
                for (int lx = -3; lx <= 3; ++lx) {
                    for (int lz = -3; lz <= 3; ++lz) {
                        const float ellipsoid = static_cast<float>(lx * lx + lz * lz) / 9.0f
                                              + static_cast<float>(ly * ly) / 4.0f;
                        const float irregularity = (hash_tree(worldX + lx, worldZ + lz, static_cast<float>(ly + 8)) - 0.5f) * 0.32f;
                        if (ellipsoid <= 1.0f + irregularity) place_leaf(x + lx, crownCenterY + ly, z + lz, leafType);
                    }
                }
            }

            for (int branch = 0; branch < 3; ++branch) {
                const int* direction = directions[(directionOffset + branch) & 3];
                const int endX = x + direction[0] * 2;
                const int endZ = z + direction[1] * 2;
                const int endY = crownCenterY + (branch & 1);
                for (int ly = -1; ly <= 1; ++ly)
                    for (int lx = -1; lx <= 1; ++lx)
                        for (int lz = -1; lz <= 1; ++lz)
                            if (lx * lx + ly * ly + lz * lz <= 2) place_leaf(endX + lx, endY + ly, endZ + lz, leafType);
            }
        }
    }
    dataVersion.store(1, std::memory_order_release);
}

#pragma once

#include "ChunkConstants.hpp"
#include "Voxel.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>

struct SparseVoxelSection {
    // RuntimeBlockId, not the game enum: ids >= BlockType::Count are dynamic
    // registry blocks (identity by UUID; material from BlockDefinition).
    RuntimeBlockId blocks[CHUNK_SIZE_X][VERTICAL_SECTION_SIZE][CHUNK_SIZE_Z];
    uint8_t waterLevels[CHUNK_SIZE_X][VERTICAL_SECTION_SIZE][CHUNK_SIZE_Z];
    // Per-voxel block state index (FALTANTES item 2 "variantes de modelo"):
    // 0 = default state (states[0]); >0 = named state from BlockDefinition.
    // Stored alongside blocks so transitions/state queries are O(1). uint8_t
    // supports 256 states per block (far more than any practical asset).
    uint8_t stateIndices[CHUNK_SIZE_X][VERTICAL_SECTION_SIZE][CHUNK_SIZE_Z]{};

    SparseVoxelSection();
};

// Discrete world lighting (META section 12). The world height is huge
// (CHUNK_SIZE_Y), so light is NOT a dense volume per chunk:
//   - Skylight is a per-column occlusion height: sky(cell) = 15 when
//     cell.y > skyOcclusionTop[column], else 0 (O(1) query, no volume).
//   - Block light is sparse: only cells with light > 0 (emitters and the
//     cells they illuminate) have entries, keyed by packed (y, z, x).
// Both are recomputed deterministically from the blocks; they are NOT part of
// the save format ("persistência opcional da luz" stays a pending item).

struct VoxelOverride {
    RuntimeBlockId type{ kRuntimeAirId };
    uint8_t waterLevel{ WATER_LEVEL_NONE };
    // Per-voxel block state index (FALTANTES item 2): 0 = default; >0 = named
    // state from BlockDefinition. The override tracks state alongside type so
    // set_state can coexist with voxelOverrides without a separate map.
    uint8_t stateIndex{ 0 };
};

// Authoritative CPU voxel state. It deliberately contains no Vulkan handles.
struct ChunkData {
    // Runtime block ids (builtin prefix + dynamic registry blocks).
    RuntimeBlockId blocks[CHUNK_SIZE_X][GENERATED_TERRAIN_HEIGHT][CHUNK_SIZE_Z];
    uint8_t waterLevels[CHUNK_SIZE_X][GENERATED_TERRAIN_HEIGHT][CHUNK_SIZE_Z];
    // Per-voxel block state index (FALTANTES item 2 "variantes de modelo"):
    // 0 = default state (states[0]); >0 = named state from BlockDefinition.
    uint8_t stateIndices[CHUNK_SIZE_X][GENERATED_TERRAIN_HEIGHT][CHUNK_SIZE_Z]{};
    std::unordered_map<uint32_t, VoxelOverride> voxelOverrides;
    std::unordered_map<int, std::unique_ptr<SparseVoxelSection>> upperSections;
    int highestOccupiedY{0};

    // ---- Discrete lighting (META section 12) ----
    // Sky: per-column height of the highest sky-blocking cell; sky = 15 above
    // it, 0 at/below. Default 0 = no occlusion (empty chunk is fully lit).
    uint16_t skyOcclusionTop[CHUNK_SIZE_X][CHUNK_SIZE_Z]{};
    // Block light: sparse map of lit cells. Key = (y << 8) | (z << 4) | x.
    std::unordered_map<uint32_t, uint8_t> blockLight;

protected:
    static constexpr uint32_t dense_key(int x, int y, int z) {
        return static_cast<uint32_t>((y * CHUNK_SIZE_Z + z) * CHUNK_SIZE_X + x);
    }
};

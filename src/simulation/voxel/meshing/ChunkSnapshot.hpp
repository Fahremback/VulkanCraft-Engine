#pragma once

#include "ChunkConstants.hpp"
#include "ChunkId.hpp"
#include "Voxel.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct VoxelLayerSnapshot {
    // Runtime block ids (builtin prefix + dynamic registry blocks).
    std::array<RuntimeBlockId, CHUNK_SIZE_X * CHUNK_SIZE_Z> blocks{};
    std::array<uint8_t, CHUNK_SIZE_X * CHUNK_SIZE_Z> water{};
    // Per-voxel block state index (FALTANTES item 2): 0 = default; >0 = named.
    std::array<uint8_t, CHUNK_SIZE_X * CHUNK_SIZE_Z> stateIndices{};
    // Discrete block light (0..15) at each cell, captured at dispatch when the
    // world has lighting (FALTANTES item 8/9): the mesher scales vertex colors
    // by light/15. Meaningful only when ChunkSnapshot::hasLightData is set.
    std::array<uint8_t, CHUNK_SIZE_X * CHUNK_SIZE_Z> light{};
};

struct NeighborVoxelHaloLayer {
    static constexpr int Width = CHUNK_SIZE_X + 2;
    static constexpr int Depth = CHUNK_SIZE_Z + 2;
    std::array<RuntimeBlockId, Width * Depth> blocks{};
    std::array<uint8_t, Width * Depth> water{};
    // Combined light (max of sky and block, 0..15) of each halo cell,
    // captured from the neighbor chunk at dispatch (FALTANTES item 8/9).
    std::array<uint8_t, Width * Depth> light{};
    std::array<uint8_t, Width * Depth> known{};
};

struct ChunkSnapshot {
    ChunkId id{};
    uint64_t revision{0};
    int verticalExtent{0};
    std::vector<int> layers;
    std::unordered_map<int, VoxelLayerSnapshot> center;
    std::unordered_map<int, NeighborVoxelHaloLayer> halo;
    // Neighbor chunks read into the halo (with their revisions at read time).
    // Streaming re-meshes a neighbor only when this snapshot is stale w.r.t. it.
    std::vector<NeighborSeen> neighborSeen;
    // Material/behavior snapshot for DYNAMIC (registry-defined) blocks, copied
    // in at dispatch so worker threads never touch the registry. Builtin ids
    // (< BlockType::Count) always resolve through the engine material table.
    std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> runtimeBlocks;

    // Per-column sky occlusion height (top of the occluding column): a cell
    // at y > skyOcclusionTop[x][z] sees sky (light 15), else not. Captured at
    // dispatch so the mesher can shade surfaces even when the adjacent air
    // cell has no occupied layer in the snapshot (open-sky top faces).
    // uint16_t matches ChunkData::skyOcclusionTop (world height is 15000).
    std::array<uint16_t, CHUNK_SIZE_X * CHUNK_SIZE_Z> skyOcclusionTop{};

    // True when light data was captured from a lit world at dispatch. Mesher
    // consumers multiply vertex colors by light/15 ONLY when this is set, so
    // snapshots built without a light capture (tests, tooling) keep the
    // legacy full-bright material colors.
    bool hasLightData{ false };

    [[nodiscard]] const RuntimeBlockInfo* find_runtime_block(RuntimeBlockId id) const;
    [[nodiscard]] RuntimeBlockId block(int x, int y, int z) const;
    [[nodiscard]] uint8_t water_level(int x, int y, int z) const;
    [[nodiscard]] uint8_t state(int x, int y, int z) const;
    // Combined light (0..15) at a cell: the captured layer value when the
    // layer exists, else the sky term of the cell's column (open-sky air
    // above the surface, which has no occupied layer in the snapshot).
    [[nodiscard]] uint8_t light(int x, int y, int z) const;
    [[nodiscard]] RuntimeBlockId halo_block(int x, int y, int z, bool& known) const;
    [[nodiscard]] uint8_t halo_water(int x, int y, int z, bool& known) const;
    [[nodiscard]] uint8_t halo_light(int x, int y, int z, bool& known) const;
};

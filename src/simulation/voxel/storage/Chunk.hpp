#pragma once

#include "ChunkData.hpp"
#include "ChunkMeshData.hpp"
#include "ChunkRuntimeState.hpp"
#include "ChunkId.hpp"
#include "FastNoiseLite.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include <vector>

class World;
class VoxelMesher;

class Chunk final : public ChunkData,
                    public ChunkRuntimeState,
                    public ChunkMeshData {
public:
    int chunkX;
    int chunkZ;

    Chunk(int cx, int cz, uint32_t generation = 1);
    ~Chunk() = default;

    // generator == nullptr uses the builtin TerrainGenerator; a public SDK
    // generator replaces height/cave/ore sampling for this chunk.
    void generate_terrain(const FastNoiseLite& noise, engine::voxel::IVoxelGenerator* generator = nullptr);
    // Runtime block ids (builtin prefix + dynamic registry blocks).
    RuntimeBlockId get_block(int x, int y, int z) const;
    void set_block(int x, int y, int z, RuntimeBlockId type);
    uint8_t get_water_level(int x, int y, int z) const;
    void set_water(int x, int y, int z, uint8_t level);
    // Generic fluid-level byte (META section 13): the same per-cell byte that
    // carries water levels now serves ANY fluid block (the byte is interpreted
    // by the block that occupies the cell). Water-guarded accessors above keep
    // the historical water semantics; these are block-agnostic.
    uint8_t get_fluid_level(int x, int y, int z) const;
    void set_fluid_level(int x, int y, int z, uint8_t level);
    // Discrete lighting queries (META section 12): skylight from the column
    // occlusion height; block light from the sparse map (0 when unlit).
    uint8_t get_sky_light(int x, int y, int z) const;
    uint8_t get_block_light(int x, int y, int z) const;
    static uint32_t light_key(int x, int y, int z) {
        return (static_cast<uint32_t>(y) << 8) | (static_cast<uint32_t>(z) << 4) |
               static_cast<uint32_t>(x);
    }
    int vertical_render_extent() const { return highestOccupiedY + 1; }
    [[nodiscard]] ChunkId id() const { return id_; }
    [[nodiscard]] uint64_t revision() const { return dataVersion.load(std::memory_order_acquire); }

private:
    friend class VoxelMesher;
    ChunkId id_{};
};

#include "ChunkSnapshot.hpp"

namespace {
constexpr std::size_t center_index(int x, int z) {
    return static_cast<std::size_t>(z * CHUNK_SIZE_X + x);
}
constexpr std::size_t halo_index(int x, int z) {
    return static_cast<std::size_t>((z + 1) * NeighborVoxelHaloLayer::Width + (x + 1));
}
}

const RuntimeBlockInfo* ChunkSnapshot::find_runtime_block(RuntimeBlockId id) const {
    for (const auto& [entryId, info] : runtimeBlocks) {
        if (entryId == id) return &info;
    }
    return nullptr;
}

RuntimeBlockId ChunkSnapshot::block(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE_X || z < 0 || z >= CHUNK_SIZE_Z) return kRuntimeAirId;
    const auto layer = center.find(y);
    return layer == center.end() ? kRuntimeAirId : layer->second.blocks[center_index(x, z)];
}

uint8_t ChunkSnapshot::water_level(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE_X || z < 0 || z >= CHUNK_SIZE_Z) return WATER_LEVEL_NONE;
    const auto layer = center.find(y);
    return layer == center.end() ? WATER_LEVEL_NONE : layer->second.water[center_index(x, z)];
}

RuntimeBlockId ChunkSnapshot::halo_block(int x, int y, int z, bool& isKnown) const {
    const auto layer = halo.find(y);
    if (layer == halo.end() || x < -1 || x > CHUNK_SIZE_X || z < -1 || z > CHUNK_SIZE_Z) {
        isKnown = false;
        return kRuntimeAirId;
    }
    const auto index = halo_index(x, z);
    isKnown = layer->second.known[index] != 0;
    return layer->second.blocks[index];
}

uint8_t ChunkSnapshot::halo_water(int x, int y, int z, bool& isKnown) const {
    const auto layer = halo.find(y);
    if (layer == halo.end() || x < -1 || x > CHUNK_SIZE_X || z < -1 || z > CHUNK_SIZE_Z) {
        isKnown = false;
        return WATER_LEVEL_NONE;
    }
    const auto index = halo_index(x, z);
    isKnown = layer->second.known[index] != 0;
    return layer->second.water[index];
}

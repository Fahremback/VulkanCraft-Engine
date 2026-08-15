#pragma once

#include "ChunkId.hpp"
#include "ChunkMeshData.hpp"

#include <cstdint>
#include <vector>

struct ChunkMeshResult {
    ChunkId chunk{};
    uint64_t sourceRevision{0};
    int verticalExtent{0};
    bool valid{true};
    ChunkMeshData mesh{};
    // Neighbor revisions observed when the snapshot was taken (see NeighborSeen).
    std::vector<NeighborSeen> neighborSeen;
};

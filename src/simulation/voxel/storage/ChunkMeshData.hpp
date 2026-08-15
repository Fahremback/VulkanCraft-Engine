#pragma once

#include "Voxel.hpp"

#include <cstdint>
#include <vector>

// Disposable CPU snapshot produced by meshing jobs.
struct ChunkMeshData {
    uint64_t meshVersion{0};
    std::vector<VoxelVertex> meshVertices;
    std::vector<VoxelVertex> waterMeshVertices;
    std::vector<GrassInstance> grassInstances;
    std::vector<FoliageInstance> foliageInstances;
    uint32_t pendingVertexCount{0};
    uint32_t pendingWaterVertexCount{0};
    uint32_t pendingGrassInstanceCount{0};
    uint32_t pendingFoliageInstanceCount{0};
};

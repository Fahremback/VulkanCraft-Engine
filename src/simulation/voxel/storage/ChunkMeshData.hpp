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
    // Renderer material variants actually produced by this chunk (registry/
    // weighted, embedded at dispatch): one entry per DISTINCT material resolved
    // while meshing. The renderer uploads these as the real material buffer
    // instead of a synthetic count. Zero = color-only / no material.
    std::vector<std::uint32_t> materialVariants;
    uint32_t pendingVertexCount{0};
    uint32_t pendingWaterVertexCount{0};
    uint32_t pendingGrassInstanceCount{0};
    uint32_t pendingFoliageInstanceCount{0};
};

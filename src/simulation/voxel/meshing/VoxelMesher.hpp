#pragma once

#include "ChunkMeshResult.hpp"
#include "ChunkSnapshot.hpp"

// Sole public entry point for turning voxel state into a disposable CPU mesh.
class VoxelMesher final {
public:
    [[nodiscard]] static ChunkMeshResult build(const ChunkSnapshot& snapshot);
};

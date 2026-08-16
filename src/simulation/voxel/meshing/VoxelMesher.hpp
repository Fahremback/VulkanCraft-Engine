#pragma once

#include "ChunkMeshResult.hpp"
#include "ChunkSnapshot.hpp"

// Sole public entry point for turning voxel state into a disposable CPU mesh.
class VoxelMesher final {
public:
    [[nodiscard]] static ChunkMeshResult build(const ChunkSnapshot& snapshot);

    // Resolves the material color of a dynamic block in a named state
    // (FALTANTES item 5): with states declared, stateIndex k addresses
    // states[k] directly and 0 is the default state (states[0]); without
    // states, any index yields the base per-face material. Out-of-range
    // indices clamp to the default. Face precedence mirrors the block level:
    // top (+Y), bottom (-Y), side (horizontal), else the state/base color.
    // Exposed so the state-aware path is testable headless and reusable by
    // the renderer's state pass.
    [[nodiscard]] static glm::vec4 resolve_state_material(
        const RuntimeBlockInfo& info, int stateIndex, const glm::vec3& normal);
};

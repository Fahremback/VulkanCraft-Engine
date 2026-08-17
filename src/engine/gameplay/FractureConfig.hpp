#pragma once

// FractureConfig (FALTANTES §16 item 8) — configurable fracture, data-driven.
// A solid voxel region becomes a destructible whose chunks detach on damage
// (debris = dynamic Jolt bodies with mass from the block density). Blast/FEMFX
// are NVIDIA mesh-fracture middleware NOT vendored here: the MESH path is
// deferred to that middleware (documented in FALTANTES); this config drives
// the VOXEL path, which is the one applicable without Blast/FEMFX.

#include "DestructionRuntime.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Engine::Gameplay {

struct FractureConfig {
    // enabled = the region CAN fracture; indestructible = it never does
    // (both generate no chunks -> damage has no destructible to act on, the
    // voxels stay solid). chunkSize/chunkHealth/damageResistance/massScale
    // drive the generated chunks; materialIndex tags them for the caller.
    bool enabled{ true };
    bool indestructible{ false };
    int chunkSize{ 2 };            // voxel group side per chunk (1..16)
    float chunkHealth{ 25.0f };
    float damageResistance{ 0.0f };
    float massScale{ 1.0f };       // debris mass = density * cells * scale
    std::uint32_t materialIndex{ 0 };

    // Data-driven: JSON keys enabled / indestructible / chunkSize /
    // chunkHealth / damageResistance / massScale / materialIndex.
    // Out-of-range values are REFUSED with a diagnostic (never clamped).
    bool load_from_json(const std::string& json, std::string& errorOut);
};

// One solid voxel cell of the region (position + the block id whose density
// feeds the debris mass).
struct VoxelCell {
    glm::ivec3 position{ 0, 0, 0 };
    std::uint32_t blockId{ 0 };
};

// Deterministic voxel fracture: cells are grouped into chunkSize^3 spatial
// buckets (negative-safe floor division), visited in fixed scan order
// (y, then z, then x). Each bucket becomes one destruction chunk: box at the
// bucket's min corner, half extents chunkSize/2, health/resistance/material
// from the config, mass = densityOf(first cell) * occupied cell count *
// massScale. localPosition is relative to `root`. Same input + config ->
// identical chunk list (bit-exact). Empty when !enabled, indestructible or
// no cells.
std::vector<DestructionChunkDesc> generate_voxel_fracture_chunks(
    const std::vector<VoxelCell>& solidCells, const glm::ivec3& root,
    const std::function<float(std::uint32_t)>& densityOf,
    const FractureConfig& config);

}  // namespace Engine::Gameplay

// ILodTerrain.hpp
//
// Public coherent-LOD terrain sampling contracts (META section 18 /
// FALTANTES item 14: the distant LOD must query the SAME world function the
// detail uses). The renderer's clipmap (FarTerrain) used to sample the
// builtin TerrainGenerator directly — so a project that overrides the world
// generator (e.g. a noise-graph generator) produced distant terrain that did
// NOT match the detail. This contract samples the actual `IVoxelGenerator`
// the world uses (the generator override) on a coarse cell lattice: every
// cell carries the generator's sample at its anchor column, so the distant
// surface is the same function as the detail — no independent approximation.
//
// Coherence properties (proved by test):
//   - Level 0 (cellSize 1) IS the detail surface: every cell anchor is an
//     integer world column and equals gen.sample(anchor).height bit-exactly.
//   - Higher levels share anchors with level 0 (anchors are multiples of
//     cellSize), so every level-k cell equals gen.sample(anchor).height at
//     the same column the detail would use — cross-level coherent.
//   - interpolated_height returns exactly the cell height at an anchor and a
//     piecewise-bilinear blend between anchors elsewhere.
//
// Deterministic: a pure function of the generator and the grid parameters.
// This header is self-contained and never leaks a backend.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/voxel/IVoxelWorld.hpp"

namespace engine {
namespace procgen {

// One LOD cell of the distant height field. `anchorX`/`anchorZ` is the
// integer world column sampled (the cell's minimum corner); the cell covers
// [anchor, anchor + cellSize) in both axes.
struct LodCell {
    int anchorX{ 0 };
    int anchorZ{ 0 };
    int cellSize{ 1 };          // 1 = full detail
    float height{ 0.0f };       // generator height at the anchor column
    std::uint32_t biomeIndex{ 0 };  // generator biome at the anchor column
};

// Samples the world function on a coarse cell lattice and reconstructs a
// coherent distant surface from it.
class ILodTerrainSampler {
public:
    virtual ~ILodTerrainSampler() = default;

    // Samples `gen` — the SAME IVoxelGenerator the world uses for detail
    // (e.g. a project's graph override) — over a cellsX x cellsZ grid of
    // `cellSize` cells aligned to the origin (origin must be a multiple of
    // cellSize). `out` receives one LodCell per cell, row-major over
    // (z, x): out[x + z * cellsX]. Each cell is gen.sample(anchor).height /
    // biomeIndex at its anchor column. Returns false with a diagnostic on
    // invalid input (non-positive grid/cell size, unaligned origin).
    virtual bool sample(const voxel::IVoxelGenerator& gen, int originX,
                        int originZ, int cellsX, int cellsZ, int cellSize,
                        std::vector<LodCell>& out, std::string& error) = 0;

    // Coherent distant surface at (x, z): piecewise-bilinear interpolation
    // over the cell anchor lattice (anchors = [origin + i * cellSize]). At an
    // anchor the result is exactly that cell's height (the generator's
    // sample); elsewhere it is the same function blended between anchors.
    // Out-of-range positions clamp to the outermost cell. Returns 0.0f for
    // an empty `cells` vector.
    virtual float interpolated_height(const std::vector<LodCell>& cells,
                                      int cellsX, int cellsZ, int cellSize,
                                      float x, float z) const = 0;
};

// Factory (implemented by the SDK adapter — composes IVoxelGenerator; no new
// backend).
std::shared_ptr<ILodTerrainSampler> create_lod_terrain_sampler();

}  // namespace procgen
}  // namespace engine

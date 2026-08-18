// IMultiScaleStreaming.hpp
//
// Public multi-scale streaming contract (META §19 "streaming multi-escala" /
// FALTANTES §15 vision: solo -> atmosphere -> space transition). The world is
// streamed at SEVERAL SCALES at once around a focus — fine voxel detail near
// the player (scale 1), a coarser height-field for the mid range, a very
// coarse lattice for the far range — and ALL scales must sample the SAME
// world function the detail uses, so crossing a scale boundary never pops:
// every cell of every level is gen.sample(anchor) at its anchor column, and
// levels with power-of-two cell sizes share anchors (a cellSize-16 anchor is
// also a cellSize-1 column and a cellSize-256 anchor is also a cellSize-16
// one), making the stack cross-level coherent by construction.
//
// The transition is LOSSLESS and path-independent: streaming the near level
// directly equals streaming it after a far -> mid -> near zoom (the near
// cells are a pure function of the generator and the focus-aligned grid), so
// descending through the scales is deterministic and re-entering a scale
// reproduces exactly the data that scale had before.
//
// This is a pure composition of the existing ILodTerrainSampler (no new
// backend): the adapter derives each level's anchor-aligned origin from the
// focus and delegates the per-level lattice sampling. This header is
// self-contained and never leaks a backend.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/procgen/ILodTerrain.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

namespace engine {
namespace procgen {

// One scale of the streaming stack. `cellSize` is the world-column footprint
// of one cell (1 = full voxel detail); `cellsX`/`cellsZ` is the grid extent in
// cells. Powers of two are RECOMMENDED so levels share anchors.
struct ScaleLevel {
    int cellSize{ 1 };
    int cellsX{ 16 };
    int cellsZ{ 16 };
};

// One streamed level of the stack: the anchor-aligned origin of its grid and
// the sampled cells (row-major over (z, x): out[x + z * cellsX]).
struct ScaleStream {
    int level{ 0 };
    int cellSize{ 1 };
    int originX{ 0 };  // anchor-aligned (multiple of cellSize)
    int originZ{ 0 };
    std::vector<LodCell> cells;
};

class IMultiScaleStreaming {
public:
    virtual ~IMultiScaleStreaming() = default;

    // Configures the scale stack (level 0 = finest). All-or-nothing: empty
    // stack, non-positive cell sizes / grid extents are refused.
    virtual bool configure(const std::vector<ScaleLevel>& levels,
                           std::string& errorOut) = 0;

    // Streams the world function `gen` at EVERY configured scale around the
    // focus. Each level's origin is derived from the focus and aligned to its
    // cell size (floor(focus / cellSize) * cellSize), so anchors are shared
    // across power-of-two levels. `out` receives one ScaleStream per level in
    // stack order. Returns false with a diagnostic on invalid input (empty
    // stack, non-finite focus).
    virtual bool stream(const voxel::IVoxelGenerator& gen, float focusX,
                        float focusZ, std::vector<ScaleStream>& out,
                        std::string& errorOut) = 0;
};

// The only implementation of IMultiScaleStreaming
// (src/engine/sdk/MultiScaleStreaming.cpp) — composes ILodTerrainSampler.
std::shared_ptr<IMultiScaleStreaming> create_multi_scale_streaming();

}  // namespace procgen
}  // namespace engine

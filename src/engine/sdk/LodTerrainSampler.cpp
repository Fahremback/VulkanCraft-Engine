// LodTerrainSampler.cpp
//
// SDK adapter for engine/procgen/ILodTerrain.hpp (META section 18 /
// FALTANTES item 14: the distant LOD must query the SAME world function the
// detail uses). This TU samples the actual `IVoxelGenerator` the world uses
// (the generator override) on a coarse cell lattice: each LodCell carries
// gen.sample(anchor) at its anchor column, so the distant surface is the same
// function as the detail — no independent/builtin approximation. The
// reconstruction is a piecewise-bilinear surface over the anchor lattice that
// passes exactly through every generator sample. Deterministic and headless;
// no new backend (the generator IS the world function).

#include "engine/procgen/ILodTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {
namespace {

// Anchor lattice origin derived from the first cell (all cells share it).
int origin_axis(const std::vector<LodCell>& cells, int cellSize, bool zAxis) {
    if (cells.empty()) {
        return 0;
    }
    const int anchor = zAxis ? cells.front().anchorZ : cells.front().anchorX;
    return anchor - (anchor % cellSize);
}

}  // namespace

class LodTerrainSampler final : public ILodTerrainSampler {
public:
    bool sample(const voxel::IVoxelGenerator& gen, int originX, int originZ,
                int cellsX, int cellsZ, int cellSize,
                std::vector<LodCell>& out, std::string& error) override {
        if (cellsX <= 0 || cellsZ <= 0 || cellSize <= 0) {
            error = "lod terrain: grid and cell size must be positive";
            return false;
        }
        if (originX % cellSize != 0 || originZ % cellSize != 0) {
            error = "lod terrain: origin must be aligned to cellSize";
            return false;
        }
        out.clear();
        out.reserve(static_cast<std::size_t>(cellsX) *
                    static_cast<std::size_t>(cellsZ));
        for (int z = 0; z < cellsZ; ++z) {
            for (int x = 0; x < cellsX; ++x) {
                const int anchorX = originX + x * cellSize;
                const int anchorZ = originZ + z * cellSize;
                const voxel::TerrainPoint p =
                    gen.sample(static_cast<float>(anchorX),
                               static_cast<float>(anchorZ));
                LodCell cell;
                cell.anchorX = anchorX;
                cell.anchorZ = anchorZ;
                cell.cellSize = cellSize;
                cell.height = static_cast<float>(p.height);
                cell.biomeIndex = p.biomeIndex;
                out.push_back(cell);
            }
        }
        return true;
    }

    float interpolated_height(const std::vector<LodCell>& cells, int cellsX,
                              int cellsZ, int cellSize, float x,
                              float z) const override {
        if (cells.empty() || cellsX <= 0 || cellsZ <= 0 || cellSize <= 0) {
            return 0.0f;
        }
        const int originX = origin_axis(cells, cellSize, /*zAxis=*/false);
        const int originZ = origin_axis(cells, cellSize, /*zAxis=*/true);

        // Cell containing (x, z), clamped so the interpolation quad stays
        // inside the lattice ([0, cells-2]).
        const int ci = std::max(0, std::min(cellsX - 2, (static_cast<int>(
            std::floor((x - static_cast<float>(originX)) /
                       static_cast<float>(cellSize))))));
        const int cj = std::max(0, std::min(cellsZ - 2, (static_cast<int>(
            std::floor((z - static_cast<float>(originZ)) /
                       static_cast<float>(cellSize))))));

        // Normalized position inside the quad, 0 at the west/north anchors.
        const float tx = std::max(
            0.0f, std::min(1.0f, (x - static_cast<float>(originX + ci * cellSize)) /
                                      static_cast<float>(cellSize)));
        const float tz = std::max(
            0.0f, std::min(1.0f, (z - static_cast<float>(originZ + cj * cellSize)) /
                                      static_cast<float>(cellSize)));

        const auto at = [&](int i, int j) {
            return cells[static_cast<std::size_t>(i) +
                         static_cast<std::size_t>(j) * cellsX]
                .height;
        };
        const float h00 = at(ci, cj);
        const float h10 = at(ci + 1, cj);
        const float h01 = at(ci, cj + 1);
        const float h11 = at(ci + 1, cj + 1);

        const float top = h00 + tx * (h10 - h00);
        const float bottom = h01 + tx * (h11 - h01);
        return top + tz * (bottom - top);
    }
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<ILodTerrainSampler> create_lod_terrain_sampler() {
    return std::make_shared<LodTerrainSampler>();
}

}  // namespace procgen
}  // namespace engine

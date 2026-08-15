#pragma once

// Voxel-world bridge for navigation (SDK, META section 16). The provider
// contract (INavigationProvider.hpp) is world-free; this header connects it to
// the voxel world: sample_voxel_columns() walks the configured bounds and
// reports the walkable surface (top of the highest solid block with air
// above) as VoxelColumns ready for baking.

#include "engine/navigation/INavigationProvider.hpp"

#include <string>
#include <vector>

namespace engine {
namespace voxel {
class IVoxelWorld;  // fwd — no header coupling; the sampler is in the adapter TU
}

namespace navigation {

// Samples walkable columns for config.bounds from a voxel world (grid
// stepping = config.cellSize). A column is solid when get_block(x, y, z)
// returns a non-air block; the reported span is the top solid voxel (walkable
// surface at its top face). Blocks above the surface are ignored (walls are
// sampled as their own columns).
std::vector<VoxelColumn> sample_voxel_columns(
    const engine::voxel::IVoxelWorld& world, const NavmeshConfig& config,
    std::string& errorOut);

}  // namespace navigation
}  // namespace engine

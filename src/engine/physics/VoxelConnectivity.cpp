#include "engine/physics/VoxelConnectivity.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace Engine::Physics {

namespace {

// Fixed 6-neighbor order (-x, +x, -y, +y, -z, +z) — the determinism guarantee.
constexpr std::array<glm::ivec3, 6> kNeighbors = {
    glm::ivec3{-1, 0, 0}, glm::ivec3{1, 0, 0},  glm::ivec3{0, -1, 0},
    glm::ivec3{0, 1, 0},  glm::ivec3{0, 0, -1}, glm::ivec3{0, 0, 1}};

// Boxes overlap or touch (gap <= 1 in any axis) -> coalesce.
bool overlaps_or_adjacent(const glm::ivec3& aMin, const glm::ivec3& aMax,
                          const glm::ivec3& bMin, const glm::ivec3& bMax) {
    return aMin.x - 1 <= bMax.x && bMin.x - 1 <= aMax.x &&
           aMin.y - 1 <= bMax.y && bMin.y - 1 <= aMax.y &&
           aMin.z - 1 <= bMax.z && bMin.z - 1 <= aMax.z;
}

}  // namespace

VoxelConnectivity::VoxelConnectivity(std::size_t maxDirtyRegions)
    : maxDirtyRegions_(std::max<std::size_t>(1, maxDirtyRegions)) {}

void VoxelConnectivity::coalesce(const Box& box) {
    // Merge the new box with every dirty region it overlaps/adjacents.
    glm::ivec3 mergedMin = box.minimum;
    glm::ivec3 mergedMax = box.maximum;
    std::vector<Box> kept;
    kept.reserve(dirtyRegions_.size() + 1);
    for (const Box& existing : dirtyRegions_) {
        if (overlaps_or_adjacent(mergedMin, mergedMax,
                                 existing.minimum, existing.maximum)) {
            mergedMin = glm::min(mergedMin, existing.minimum);
            mergedMax = glm::max(mergedMax, existing.maximum);
        } else {
            kept.push_back(existing);
        }
    }
    kept.push_back(Box{mergedMin, mergedMax});
    dirtyRegions_ = std::move(kept);

    // Over the cap, fold everything into one region (bounded worst case).
    if (dirtyRegions_.size() > maxDirtyRegions_) {
        glm::ivec3 allMin = dirtyRegions_[0].minimum;
        glm::ivec3 allMax = dirtyRegions_[0].maximum;
        for (std::size_t i = 1; i < dirtyRegions_.size(); ++i) {
            allMin = glm::min(allMin, dirtyRegions_[i].minimum);
            allMax = glm::max(allMax, dirtyRegions_[i].maximum);
        }
        dirtyRegions_ = {Box{allMin, allMax}};
    }
}

void VoxelConnectivity::note_edit(const glm::ivec3& minimum,
                                  const glm::ivec3& maximum) {
    const glm::ivec3 lo = glm::min(minimum, maximum);
    const glm::ivec3 hi = glm::max(minimum, maximum);
    coalesce(Box{lo, hi});
}

void VoxelConnectivity::visit_solid_cell(
    const glm::ivec3& cell, const Box& region, std::uint8_t* visited,
    std::uint8_t marker, std::vector<glm::ivec3>& frontier) const {
    const std::size_t sx = static_cast<std::size_t>(region.maximum.x - region.minimum.x + 1);
    const std::size_t sy = static_cast<std::size_t>(region.maximum.y - region.minimum.y + 1);
    const std::size_t ox = static_cast<std::size_t>(cell.x - region.minimum.x);
    const std::size_t oy = static_cast<std::size_t>(cell.y - region.minimum.y);
    const std::size_t oz = static_cast<std::size_t>(cell.z - region.minimum.z);
    std::uint8_t& slot = visited[ox + oy * sx + oz * sx * sy];
    if (slot != 0u) return;
    slot = marker;
    ++scannedCells_;
    frontier.push_back(cell);
}

std::vector<VoxelIsland> VoxelConnectivity::sync(
    const engine::voxel::IVoxelWorld& world,
    const ConnectivitySettings& settings,
    const std::function<bool(std::uint32_t)>& isSolid) const {
    std::vector<VoxelIsland> islands;
    scannedCells_ = 0;
    if (!isSolid) return islands;

    for (const Box& base : dirtyRegions_) {
        const Box region{base.minimum - glm::ivec3(settings.regionMargin),
                         base.maximum + glm::ivec3(settings.regionMargin)};
        const int minX = region.minimum.x, maxX = region.maximum.x;
        const int minY = region.minimum.y, maxY = region.maximum.y;
        const int minZ = region.minimum.z, maxZ = region.maximum.z;
        const std::size_t sx = static_cast<std::size_t>(maxX - minX + 1);
        const std::size_t sy = static_cast<std::size_t>(maxY - minY + 1);
        const std::size_t sz = static_cast<std::size_t>(maxZ - minZ + 1);
        const std::size_t volume = sx * sy * sz;
        if (volume == 0) continue;

        std::vector<std::uint8_t> visited(volume, 0);
        const auto index = [&](const glm::ivec3& c) -> std::size_t {
            return static_cast<std::size_t>(c.x - minX) +
                   static_cast<std::size_t>(c.y - minY) * sx +
                   static_cast<std::size_t>(c.z - minZ) * sx * sy;
        };

        // Phase 1 — anchor seeds: solid border cells that continue into the
        // intact world (solid neighbor strictly OUTSIDE the region) plus the
        // bedrock band. A platform sitting on the region's TOP face is NOT a
        // seed (its outside neighbor is air) — it must reach the anchor mass
        // through solid cells inside the region.
        std::vector<glm::ivec3> frontier;
        frontier.reserve(volume / 4);
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                for (int z = minZ; z <= maxZ; ++z) {
                    const glm::ivec3 cell{x, y, z};
                    if (visited[index(cell)] != 0u) continue;
                    if (!isSolid(world.get_block(x, y, z))) continue;
                    if (settings.anchoredAtBottom && y <= settings.worldBottomY + settings.bottomMargin) {
                        visit_solid_cell(cell, region, visited.data(), 1u, frontier);
                        continue;
                    }
                    const bool onBorder = x == minX || x == maxX || y == minY ||
                                          y == maxY || z == minZ || z == maxZ;
                    if (!onBorder) continue;
                    // Outside-connected: at least one 6-neighbor outside the
                    // region is solid.
                    for (const glm::ivec3& delta : kNeighbors) {
                        const glm::ivec3 outside = cell + delta;
                        const bool out = outside.x < minX || outside.x > maxX ||
                                         outside.y < minY || outside.y > maxY ||
                                         outside.z < minZ || outside.z > maxZ;
                        if (out && isSolid(world.get_block(outside.x, outside.y, outside.z))) {
                            visit_solid_cell(cell, region, visited.data(), 1u, frontier);
                            break;
                        }
                    }
                }
            }
        }

        // Flood the anchored component through solid cells (fixed order).
        while (!frontier.empty()) {
            const glm::ivec3 cell = frontier.back();
            frontier.pop_back();
            for (const glm::ivec3& delta : kNeighbors) {
                const glm::ivec3 next = cell + delta;
                if (next.x < minX || next.x > maxX || next.y < minY ||
                    next.y > maxY || next.z < minZ || next.z > maxZ)
                    continue;
                if (visited[index(next)] != 0u) continue;
                if (!isSolid(world.get_block(next.x, next.y, next.z))) continue;
                visit_solid_cell(next, region, visited.data(), 1u, frontier);
            }
        }

        // Phase 2 — unvisited solid cells are detached islands; group each
        // connected component, accumulating its cells as the flood grows so
        // each component's bbox/count is exact (fixed scan order ->
        // deterministic output).
        std::vector<glm::ivec3> frontier2;
        std::vector<glm::ivec3> componentCells;
        for (int y = minY; y <= maxY; ++y) {
            for (int z = minZ; z <= maxZ; ++z) {
                for (int x = minX; x <= maxX; ++x) {
                    const glm::ivec3 cell{x, y, z};
                    if (visited[index(cell)] != 0u) continue;
                    if (!isSolid(world.get_block(x, y, z))) continue;
                    componentCells.clear();
                    frontier2.clear();
                    visit_solid_cell(cell, region, visited.data(), 2u, frontier2);
                    componentCells.push_back(cell);
                    while (!frontier2.empty()) {
                        const glm::ivec3 cur = frontier2.back();
                        frontier2.pop_back();
                        for (const glm::ivec3& delta : kNeighbors) {
                            const glm::ivec3 next = cur + delta;
                            if (next.x < minX || next.x > maxX || next.y < minY ||
                                next.y > maxY || next.z < minZ || next.z > maxZ)
                                continue;
                            if (visited[index(next)] != 0u) continue;
                            if (!isSolid(world.get_block(next.x, next.y, next.z))) continue;
                            visit_solid_cell(next, region, visited.data(), 2u, frontier2);
                            componentCells.push_back(next);
                        }
                    }
                    VoxelIsland island;
                    island.minimum = island.maximum = componentCells[0];
                    for (const glm::ivec3& probe : componentCells) {
                        island.minimum = glm::min(island.minimum, probe);
                        island.maximum = glm::max(island.maximum, probe);
                    }
                    island.solidCells = componentCells.size();
                    islands.push_back(island);
                }
            }
        }
    }

    std::sort(islands.begin(), islands.end(),
              [](const VoxelIsland& a, const VoxelIsland& b) {
                  if (a.minimum.x != b.minimum.x) return a.minimum.x < b.minimum.x;
                  if (a.minimum.y != b.minimum.y) return a.minimum.y < b.minimum.y;
                  if (a.minimum.z != b.minimum.z) return a.minimum.z < b.minimum.z;
                  if (a.maximum.x != b.maximum.x) return a.maximum.x < b.maximum.x;
                  if (a.maximum.y != b.maximum.y) return a.maximum.y < b.maximum.y;
                  return a.maximum.z < b.maximum.z;
              });
    return islands;
}

void VoxelConnectivity::clear_dirty() noexcept { dirtyRegions_.clear(); }

}  // namespace Engine::Physics

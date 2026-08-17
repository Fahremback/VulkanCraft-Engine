#pragma once

// VoxelConnectivity (FALTANTES §16 item 9): incremental voxel island
// detection. After terrain edits only voxel islands that LOST their connection
// to the anchored mass become dynamic Jolt bodies; terrain still connected to
// the anchor stays static. A solid component is ANCHORED when it 6-connects
// (through solid cells) to the scan-region border (assumed intact beyond the
// region) or to the world bottom; any solid component unreachable from those
// seeds is a DETACHED island.
//
// Incremental: callers mark the voxel boxes an edit touched with note_edit();
// sync() rescans ONLY those dirty regions (coalesced, bounded by
// maxDirtyRegions), so a small edit never rescan the whole world
// (scanned_cells() is the observability proof).
//
// Self-contained: depends only on the PUBLIC voxel world contract
// (engine::voxel::IVoxelWorld) + a caller-supplied is_solid predicate. No
// Jolt/engine internals. Deterministic: fixed neighbor order, regions scanned
// in insertion order, islands returned sorted by min corner.

#include "engine/voxel/IVoxelWorld.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Engine::Physics {

struct ConnectivitySettings {
    // Solid cells whose y <= worldBottomY + bottomMargin count as anchored
    // (the bedrock band the whole simulated mass rests on).
    bool anchoredAtBottom{ true };
    int worldBottomY{ 0 };
    int bottomMargin{ 1 };

    // Solid cells on the scan-region border count as anchored (assumed intact
    // beyond the region). Must be >= 1 so a 1-cell gap at the border is
    // covered by the flood margin.
    int regionMargin{ 1 };
};

// One detached island: the voxel AABB of the solid component plus how many
// solid cells it contains (feeds the debris mass).
struct VoxelIsland {
    glm::ivec3 minimum{ 0, 0, 0 };
    glm::ivec3 maximum{ 0, 0, 0 };
    std::size_t solidCells{ 0 };
};

class VoxelConnectivity final {
public:
    explicit VoxelConnectivity(std::size_t maxDirtyRegions = 16);

    // Incremental dirty tracking: mark the voxel box an edit touched (block
    // coordinates, inclusive). Boxes are coalesced; more than maxDirtyRegions
    // coalesce into one region (fallback full-rescan of that combined box).
    void note_edit(const glm::ivec3& minimum, const glm::ivec3& maximum);

    // Rescans every dirty region (bounded flood) and returns the detached
    // islands, deterministic (sorted by min corner, then max). `is_solid`
    // maps a block id. Does not clear the dirty set; call clear_dirty()
    // when the edits have been reconciled (or sync() is idempotent).
    std::vector<VoxelIsland> sync(
        const engine::voxel::IVoxelWorld& world,
        const ConnectivitySettings& settings,
        const std::function<bool(std::uint32_t)>& isSolid) const;

    void clear_dirty() noexcept;
    std::size_t dirty_region_count() const noexcept { return dirtyRegions_.size(); }
    // How many voxel cells the last sync() flood visited (bounded: far less
    // than the whole world for a small edit — the incremental proof).
    std::size_t scanned_cells() const noexcept { return scannedCells_; }

private:
    struct Box {
        glm::ivec3 minimum;
        glm::ivec3 maximum;
    };
    void coalesce(const Box& box);
    void visit_solid_cell(const glm::ivec3& cell, const Box& region,
                          std::uint8_t* visited, std::uint8_t marker,
                          std::vector<glm::ivec3>& frontier) const;

    std::size_t maxDirtyRegions_;
    std::vector<Box> dirtyRegions_;
    mutable std::size_t scannedCells_{ 0 };
};

}  // namespace Engine::Physics

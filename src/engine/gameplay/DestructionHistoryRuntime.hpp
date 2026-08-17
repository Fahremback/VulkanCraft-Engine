#pragma once

// DestructionHistoryRuntime (FALTANTES §16 item 13): reconstruction or
// historical restoration WITHOUT special cases. One generic mechanism
// captures the destruction state of a region — every voxel cell + every
// active debris inside it — and restores it later, regardless of how the
// damage happened (explosion carve, fracture detach, manual carve, debris
// revoxelization back into terrain). The snapshot is the full region state;
// there is no per-source code path.
//
//   capture  — walks the region in fixed (y,z,x) order, records every cell
//              (position + block id) and every active debris in the AABB
//              (any motion state — persistence filters do not apply). Bounded
//              by maxRegionCells (refused, never clamped). Deterministic.
//   restore  — writes every captured cell back (air stays air, solids come
//              back), despawns every CURRENT debris in the region, then
//              re-spawns the captured debris through the DebrisRuntime pool.
//              Deterministic order. The region is bit-identical to the
//              capture after restore.
//
// Self-contained: depends only on the PUBLIC voxel world contract + the
// physics runtime + DebrisRuntime (item 11). Data-driven config via JSON.

#include "engine/gameplay/DebrisRuntime.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Engine::Gameplay {

struct DestructionHistoryConfig {
    bool enabled{ true };
    std::size_t maxRegionCells{ 4096 };  // capture region budget (refused above)

    // JSON keys: enabled / maxRegionCells. Out-of-range values are REFUSED
    // with a diagnostic (never clamped), mirroring the other gameplay configs.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

// One captured voxel cell of a region.
struct DestructionCell {
    glm::ivec3 position{ 0 };
    std::uint32_t blockId{ 0 };
};

// Full destruction state of a region at a point in time. Cells are in fixed
// (y,z,x) scan order; debris in deterministic active_ order (by id).
struct DestructionSnapshot {
    glm::ivec3 minimum{ 0 };
    glm::ivec3 maximum{ -1, -1, -1 };  // empty region when min > max
    std::vector<DestructionCell> cells;
    std::vector<DebrisPersistRecord> debris;
    bool valid() const noexcept { return minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z; }
};

struct DestructionRestoreResult {
    std::size_t cellsWritten{ 0 };    // voxel cells actually changed
    std::size_t debrisSpawned{ 0 };   // captured debris re-created
    std::size_t debrisDespawned{ 0 }; // current debris removed from the region
};

class DestructionHistoryRuntime final {
public:
    // Does not take ownership of physics/debris; they must outlive the runtime.
    explicit DestructionHistoryRuntime(Physics::PhysicsRuntime& physics,
                                       DebrisRuntime& debris,
                                       DestructionHistoryConfig config = {});

    // Captures the full destruction state of the region [minimum, maximum]
    // (inclusive): every cell (y,z,x order) + every active debris in the
    // AABB. Refuses (false + diagnostic) an invalid region or one whose cell
    // count exceeds maxRegionCells — never clamped.
    bool capture(engine::voxel::IVoxelWorld& world, const glm::ivec3& minimum,
                 const glm::ivec3& maximum, DestructionSnapshot& out,
                 std::string& errorOut) const;

    // Restores the snapshot: writes every captured cell back (only cells that
    // differ are counted), despawns current debris in the region, re-spawns
    // the captured debris. Deterministic. Returns per-step counts.
    DestructionRestoreResult restore(engine::voxel::IVoxelWorld& world,
                                     const DestructionSnapshot& snapshot);

    const DestructionHistoryConfig& config() const noexcept { return config_; }

private:
    Physics::PhysicsRuntime& physics_;
    DebrisRuntime& debris_;
    DestructionHistoryConfig config_;
};

}  // namespace Engine::Gameplay

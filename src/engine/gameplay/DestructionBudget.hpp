#pragma once

// DestructionBudget (FALTANTES §16 item 14): ONE data-driven budget that
// bounds every stage of the destruction pipeline per step, so large-scale
// destruction (many explosions, many detached islands, many debris, big
// restores) cannot blow the frame/memory in a single call:
//
//   explosion carve  — maxCarvedCells  per apply_explosion call
//   explosion heat   — maxBurnedCells  per apply_explosion call
//   explosion impulse— maxImpulsedBodies per apply_explosion call
//   connectivity     — maxDetachedIslands promoted to debris per sync
//   debris spawn     — maxSpawnedDebris per spawn burst
//   revoxelization   — maxRevoxelizedDebris / maxRevoxelizedBlocks per pass
//   history restore  — maxRestoredCells per restore call
//
// The caps the leaf runtimes already carry (RevoxelizePolicy,
// DestructionHistoryConfig, ExplosionConfig) are MAPPED from this single
// source via apply_to() — one config to validate, never drift. The
// orchestration gates (max_islands/max_debris) bound the two stages with no
// leaf config: connectivity island promotion and debris spawn bursts; the
// caller checks them BEFORE acting and reports overflows to the telemetry.
//
// Validation is all-or-nothing (refuse, never clamp), mirroring the other
// gameplay configs. A cap of 0 means UNLIMITED for the explosion/connectivity/
// debris gates; the revoxelize/history caps follow the leaf ranges ([1, N]).

#include "engine/gameplay/DebrisRuntime.hpp"
#include "engine/gameplay/DestructionHistoryRuntime.hpp"
#include "engine/gameplay/ExplosionRuntime.hpp"

#include <cstddef>
#include <limits>
#include <string>

namespace Engine::Gameplay {

struct DestructionBudget {
    bool enabled{ true };              // false = no caps enforced

    // Explosion stage caps (0 = unlimited). Defaults bound a single large
    // blast to a bounded amount of voxel/body work per call.
    std::size_t maxCarvedCells{ 4096 };      // [0, 1048576]
    std::size_t maxBurnedCells{ 1024 };      // [0, 1048576]
    std::size_t maxImpulsedBodies{ 128 };    // [0, 65536]

    // Orchestration gates (0 = unlimited): bounded by the PIPELINE caller
    // before acting (islands promoted to debris, debris spawn bursts).
    std::size_t maxDetachedIslands{ 32 };    // [0, 4096]
    std::size_t maxSpawnedDebris{ 64 };      // [0, 4096]

    // Leaf-mapped caps (mirror the leaf config ranges exactly).
    std::size_t maxRevoxelizedDebris{ 16 };  // [1, 1024]  (RevoxelizePolicy)
    std::size_t maxRevoxelizedBlocks{ 32 };  // [1, 4096]  (RevoxelizePolicy)
    std::size_t maxRestoredCells{ 16384 };   // [1, 1000000] (history config)

    // JSON keys: enabled / maxCarvedCells / maxBurnedCells / maxImpulsedBodies
    // / maxDetachedIslands / maxSpawnedDebris / maxRevoxelizedDebris /
    // maxRevoxelizedBlocks / maxRestoredCells. Out-of-range values are REFUSED
    // with a diagnostic (never clamped), mirroring the other configs.
    bool load_from_json(const std::string& json, std::string& errorOut);

    // Maps the budget into the leaf configs that already carry caps. No-ops
    // when !enabled (the leaves keep their own config).
    void apply_to(ExplosionConfig& config) const;
    void apply_to(RevoxelizePolicy& policy) const;
    void apply_to(DestructionHistoryConfig& config) const;

    // Orchestration gates: the pipeline's per-call bound for the stages with
    // no leaf config. Returns SIZE_MAX (unlimited) when disabled or 0.
    std::size_t max_islands() const noexcept {
        return (enabled && maxDetachedIslands > 0)
                   ? maxDetachedIslands
                   : std::numeric_limits<std::size_t>::max();
    }
    std::size_t max_debris() const noexcept {
        return (enabled && maxSpawnedDebris > 0)
                   ? maxSpawnedDebris
                   : std::numeric_limits<std::size_t>::max();
    }
};

}  // namespace Engine::Gameplay

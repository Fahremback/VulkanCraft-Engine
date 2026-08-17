#pragma once

// DestructionTelemetry (FALTANTES §16 item 14): the observability half of the
// large-scale destruction budget system. Every pipeline stage reports what it
// did (note_*) and what the budget made it SKIP (note_spill) — step counters
// reset per simulation step (begin_step), cumulative totals kept for the
// session, plus a per-stage exceeded() flag for the budget gate.
//
// The counters mirror the pipeline results 1:1 (ExplosionResult,
// RevoxelizeResult, restore results, connectivity islands, debris lifecycle),
// so a caller feeds the leaf results in and reads ONE coherent report of the
// whole destruction step. Deterministic: plain counters, no randomness.

#include <cstddef>
#include <cstdint>

namespace Engine::Gameplay {

// The pipeline stages that can overflow their budget cap.
enum class DestructionStage : std::uint8_t {
    Carve,          // explosion terrain carve (maxCarvedCells)
    Burn,           // explosion heat (maxBurnedCells)
    Impulse,        // explosion body push (maxImpulsedBodies)
    Islands,        // connectivity island promotion (maxDetachedIslands)
    Spawn,          // debris spawn burst (maxSpawnedDebris)
    Revoxelize,     // debris revoxelized per pass (maxRevoxelizedDebris)
    Blocks,         // voxel blocks written per debris (maxRevoxelizedBlocks)
    Restore,        // history restore cells (maxRestoredCells)
};

struct DestructionCounters {
    // Done work (step and cumulative).
    std::size_t carvedCells{ 0 };        // explosion cells carved
    std::size_t burnedCells{ 0 };        // explosion cells burned
    std::size_t impulsedBodies{ 0 };     // explosion bodies pushed
    std::size_t detachedIslands{ 0 };    // islands promoted to debris
    std::size_t spawnedDebris{ 0 };      // debris spawned (incl. restores)
    std::size_t despawnedDebris{ 0 };    // debris despawned (incl. clears)
    std::size_t revoxelizedDebris{ 0 };  // sleeping debris written back
    std::size_t revoxelizedBlocks{ 0 };  // voxel cells written by revoxelization
    std::size_t restoredCells{ 0 };      // history restore cells written
    std::size_t cellsWritten{ 0 };       // history restore changed cells

    // Spills: work the budget made the stage SKIP (overflow observability).
    std::size_t spilledCarved{ 0 };
    std::size_t spilledBurned{ 0 };
    std::size_t spilledImpulsed{ 0 };
    std::size_t spilledIslands{ 0 };
    std::size_t spilledDebris{ 0 };
    std::size_t spilledRevoxelized{ 0 };
    std::size_t spilledBlocks{ 0 };
    std::size_t spilledRestored{ 0 };
};

class DestructionTelemetry final {
public:
    // Resets the STEP counters (cumulative totals are kept). Call once per
    // simulation step before the pipeline runs.
    void begin_step() { step_ = DestructionCounters{}; }

    // Full reset: step counters AND cumulative totals.
    void reset() {
        step_ = DestructionCounters{};
        total_ = DestructionCounters{};
    }

    void note_carved(std::size_t n) { add(step_.carvedCells, total_.carvedCells, n); }
    void note_burned(std::size_t n) { add(step_.burnedCells, total_.burnedCells, n); }
    void note_impulsed(std::size_t n) { add(step_.impulsedBodies, total_.impulsedBodies, n); }
    void note_islands(std::size_t n) { add(step_.detachedIslands, total_.detachedIslands, n); }
    void note_spawned(std::size_t n) { add(step_.spawnedDebris, total_.spawnedDebris, n); }
    void note_despawned(std::size_t n) { add(step_.despawnedDebris, total_.despawnedDebris, n); }
    void note_revoxelized(std::size_t n) { add(step_.revoxelizedDebris, total_.revoxelizedDebris, n); }
    void note_blocks(std::size_t n) { add(step_.revoxelizedBlocks, total_.revoxelizedBlocks, n); }
    void note_restored(std::size_t n) { add(step_.restoredCells, total_.restoredCells, n); }
    void note_written(std::size_t n) { add(step_.cellsWritten, total_.cellsWritten, n); }

    // Budget overflows: the stage skipped `n` candidates because its cap was
    // reached. Recorded in both step and cumulative spill counters.
    void note_spill(DestructionStage stage, std::size_t n);

    // Current step counters (since the last begin_step).
    const DestructionCounters& step() const noexcept { return step_; }
    // Session totals (never reset by begin_step).
    const DestructionCounters& cumulative() const noexcept { return total_; }
    // True when the current step spilled work at the given stage.
    bool exceeded(DestructionStage stage) const noexcept;

private:
    static void add(std::size_t& step, std::size_t& total, std::size_t n) {
        step += n;
        total += n;
    }
    DestructionCounters step_;
    DestructionCounters total_;
};

}  // namespace Engine::Gameplay

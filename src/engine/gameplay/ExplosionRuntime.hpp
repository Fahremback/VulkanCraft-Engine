#pragma once

// ExplosionRuntime (FALTANTES §16 item 10): integrated voxel explosions that
// combine the five axes of the item — materials, heat, pressure, impulse and
// terrain. Data-driven via ExplosionConfig (JSON), headless-provable, fully
// deterministic (fixed voxel scan order, no randomness).
//
//   terrain   — cells inside blastRadius are carved when the block's blast
//               resistance (BlockRuntimeView.resistance, mirror of the
//               registry JSON) is below the local blast pressure. Stone
//               (resistance 0) crumbles; reinforced blocks survive.
//   heat      — cells inside heatRadius with flammability > 0 burn away when
//               heatDamage * flammability >= ignitionThreshold. Non-flammable
//               blocks (flammability 0) ignore the fireball entirely.
//   pressure  — the blast pressure falls off as
//               maxPressure * (1 - d/blastRadius)^pressureDecay; it drives
//               both the terrain carve and the body impulse below.
//   impulse   — every DYNAMIC body inside blastRadius (found via the public
//               overlap_aabb) receives a radial impulse scaled by the local
//               pressure; physics mass (from block density, item 7) scales
//               the response, so heavier debris moves less.
//   terrain→  — the returned affected voxel box feeds the connectivity layer
//               (item 9): after the blast, note_edit(box) + the bridge turn
//               every detached island into a dynamic Jolt body that falls.
//
// Self-contained: depends only on the PUBLIC voxel world contract + the
// physics runtime. No Jolt/engine internals.

#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace Engine::Gameplay {

struct ExplosionConfig {
    // --- Terrain (blast) ---
    float blastRadius{ 4.0f };       // voxel-units sphere (carve radius)
    float maxPressure{ 40.0f };      // blast pressure at the epicenter
    float pressureDecay{ 2.0f };     // pressure = max * (1 - d/r)^decay
    float resistanceFactor{ 1.0f };  // survival: resistance >= pressure * factor
    bool carveTerrain{ true };       // false = blast does not edit the voxels

    // --- Heat (fireball) ---
    float heatRadius{ 5.0f };        // voxel-units heat sphere (>= 0)
    float heatDamage{ 1.0f };        // heat applied to flammables (>= 0)
    float ignitionThreshold{ 1.0f }; // burns when heatDamage*flammability >= this (> 0)

    // --- Impulse (pressure wave on dynamic bodies) ---
    float impulseScale{ 12.0f };     // impulse multiplier (>= 0)

    // --- Budgets (FALTANTES §16 item 14) ---
    // Per-call caps so a LARGE blast cannot carve/burn/push unbounded
    // cells/bodies in a single step (large-scale destruction is bounded and
    // observable). 0 = unlimited (the pre-budget behavior). When a cap is
    // hit, the remaining candidates of that stage are SKIPPED (counted in
    // ExplosionResult.*Skipped) — deterministic given the fixed scan order.
    // Heat and impulse are independent stages: a carve cap does not suppress
    // burning that same cell, and vice versa.
    std::size_t maxCarvedCells{ 0 };     // 0 = unlimited
    std::size_t maxBurnedCells{ 0 };     // 0 = unlimited
    std::size_t maxImpulsedBodies{ 0 };  // 0 = unlimited

    // Data-driven: JSON keys blastRadius / maxPressure / pressureDecay /
    // resistanceFactor / carveTerrain / heatRadius / heatDamage /
    // ignitionThreshold / impulseScale / maxCarvedCells / maxBurnedCells /
    // maxImpulsedBodies. Out-of-range values are REFUSED with a diagnostic
    // (never clamped), mirroring FractureConfig.
    bool load_from_json(const std::string& json, std::string& errorOut);
};

struct ExplosionResult {
    std::size_t blocksRemoved{ 0 };   // cells carved by the blast
    std::size_t blocksIgnited{ 0 };   // cells burned by heat
    std::size_t bodiesImpulsed{ 0 };  // dynamic bodies pushed by the wave
    // Budget skips (item 14): candidates left untouched because the stage's
    // per-call cap (ExplosionConfig.max*) was already reached. Telemetry
    // counts these as spills — large-scale destruction is bounded AND its
    // overflow is observable.
    std::size_t carvedSkipped{ 0 };   // cells left un-carved by the cap
    std::size_t burnedSkipped{ 0 };   // cells left un-burned by the cap
    std::size_t impulseSkipped{ 0 };  // bodies left unpushed by the cap
    glm::ivec3 affectedMin{ 0 };      // voxel box of all edits (for the
    glm::ivec3 affectedMax{ -1 };     // connectivity layer); empty when below
    bool affected_any() const noexcept { return affectedMin.x <= affectedMax.x; }
};

// Detonates an integrated explosion at `origin` (world position). Carves the
// terrain by material blast resistance, burns flammables by heat, pushes
// dynamic bodies by the pressure wave, and returns the affected voxel box so
// the caller can run the connectivity layer (item 9). Deterministic.
ExplosionResult apply_explosion(engine::voxel::IVoxelWorld& world,
                                Physics::PhysicsRuntime& physics,
                                const glm::vec3& origin,
                                const ExplosionConfig& config);

}  // namespace Engine::Gameplay

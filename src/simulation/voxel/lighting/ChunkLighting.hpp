#pragma once

// Discrete world lighting (META section 12). The engine owns the calculation;
// the project declares emission/absorption through block data. Deterministic:
// the light state is a pure function of the blocks — same blocks, same light,
// regardless of pass order or machine.
//
//   - Skylight: per-column occlusion height (the highest sky-blocking cell).
//     Sky = 15 above it, 0 at/below. Opaque cells (absorption >= 15) block.
//   - Block light: emitters seed their cell; light relaxes outward, each step
//     costing 1 + the SOURCE cell's absorption (air = 1 per step, water = 2,
//     opaque = 16 -> blocked). Fixed point reached in <= 16 passes (levels
//     strictly decrease along every edge).
//   - Chunk borders: a 1-cell halo reads the neighbors' COMPUTED light (and
//     their block ids for absorption), so light crosses chunk boundaries; the
//     world re-dirties neighbors when a chunk's light changes (convergence).
//   - Frontier: unloaded neighbor cells read as Air with no light (light never
//     enters from unloaded terrain).

#include "Chunk.hpp"

#include <cstdint>
#include <functional>

// Everything the light engine needs from the world, as world-space lookups.
// The callbacks lock the world's chunk state; the light pass runs on the frame
// thread, so recursive locking is safe.
struct ChunkLightAccess {
    // Runtime block id at world voxel (x, y, z); Air outside loaded chunks.
    std::function<RuntimeBlockId(int, int, int)> blockAt;
    // Computed block light at world voxel (x, y, z); 0 when the owning chunk
    // has no light map yet (unloaded or never computed).
    std::function<uint8_t(int, int, int)> blockLightAt;
    // Emission/absorption tables for a runtime id (builtin + dynamic).
    std::function<uint8_t(RuntimeBlockId)> emission;
    std::function<uint8_t(RuntimeBlockId)> absorption;
};

class ChunkLighting {
public:
    static constexpr uint8_t kMaxLight = 15;

    // Recomputes sky occlusion + block light for the chunk at (cx, cz) and
    // writes them into the chunk. Returns true when the chunk's light state
    // changed (the caller re-dirties neighbors so light crosses borders).
    //
    // skipSkylight (C.1): when the caller can PROVE the chunk's own content is
    // unchanged since the last compute (dataVersion/revision gate — every
    // block/fluid/state write bumps it), the per-column sky occlusion is
    // provably identical, so the 16x16x256-column rescan is skipped. Only the
    // block-light pass runs (its seed set must be complete — own emitters are
    // unchanged, halo inflow may have changed). Used for neighbor-convergence
    // re-dirties, where the chunk's content did NOT change.
    static bool compute(Chunk& chunk, const ChunkLightAccess& access,
                        bool skipSkylight = false);
};

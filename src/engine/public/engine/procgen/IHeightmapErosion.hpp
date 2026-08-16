#pragma once

// Public heightmap-erosion contracts (META section 18 / FALTANTES item 14).
//
// Headless, deterministic terrain erosion for the cooker/offline pipeline —
// NEVER for the game frame (erosion is a non-local, seed-keyed process; the
// frame must sample already-eroded terrain). The implementation follows the
// standard particle-based hydraulic model (water droplets carrying sediment
// down the height gradient) plus a thermal cascade (material slides off
// slopes steeper than a talus angle) — the same algorithm family as the
// catalog candidate soil-machine, but deterministic and library-shaped.
//
// Determinism: a seeded splitmix64 generator drives every random choice
// (droplet starts) in a fixed order, so the same (heightmap, spec) always
// produces a bit-identical result — on every run and every instance. The
// tile cache keys results by (seed, canonical spec hash, tile coords), so a
// cooker re-eroding the same seed/tile is a cache hit, never a recompute.
//
// Heights are normalized floats in [0, 1]; erosion conserves material
// (hydraulic redistribution + thermal sliding) within floating-point noise.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// A normalized heightmap: `width` x `height` floats in [0, 1], row-major
// (x = width axis, z = height axis).
struct Heightmap {
    int width{ 0 };
    int height{ 0 };
    std::vector<float> values;
};

// Data-driven erosion parameters.
struct ErosionSpec {
    // Hydraulic (water droplets).
    std::uint64_t seed{ 1 };
    int iterations{ 20000 };       // water droplets spawned
    int maxSteps{ 30 };            // steps per droplet before evaporation
    float evaporation{ 0.02f };    // water lost per step (0..1]
    float erosionRate{ 0.08f };    // fraction of erodible material per step
    float depositionRate{ 0.08f }; // fraction of carried sediment per step
    float capacityScale{ 0.05f };  // sediment capacity factor
    float gravity{ 4.0f };         // downhill acceleration per step
    float minSlope{ 0.01f };       // floor on the descent used for capacity
    float maxSpeed{ 6.0f };        // droplet speed clamp

    // Thermal (cascade).
    int thermalIterations{ 4 };    // passes over the grid
    float talusAngle{ 0.08f };     // slope threshold that stays put
};

// Runs deterministic erosion on a heightmap.
class IHeightmapErosion {
public:
    virtual ~IHeightmapErosion() = default;

    // Deterministically erodes `in` (hydraulic + thermal) into `out` (which
    // must have the same dimensions as `in`). Returns false with a message
    // on invalid input/spec. Two calls with the same (heightmap, spec)
    // produce bit-identical `out`.
    virtual bool erode(const Heightmap& in, const ErosionSpec& spec,
                       Heightmap& out, std::string& errorOut) = 0;

    virtual bool validate(const ErosionSpec& spec,
                          std::string& errorOut) const = 0;

    virtual bool serialize_spec(const ErosionSpec& spec,
                                std::string& out) const = 0;
    virtual bool deserialize_spec(const std::string& json, ErosionSpec& out,
                                  std::string& errorOut) const = 0;
};

// Per-(seed, spec, tile) erosion cache for the cooker: re-eroding the same
// tile with the same spec is a cache hit. The cache key is a deterministic
// hash of (seed, canonical spec JSON, tileX, tileY), so entries never
// collide across seeds, specs or tiles.
class ITileErosionCache {
public:
    virtual ~ITileErosionCache() = default;

    // Erodes `tile` for (spec, tileX, tileY), returning the cached result on
    // a hit. `out` receives a heightmap with the tile's dimensions.
    virtual bool erode_tile(const ErosionSpec& spec, int tileX, int tileY,
                            const Heightmap& tile, Heightmap& out,
                            std::string& errorOut) = 0;

    // Drops all cached entries.
    virtual void clear() = 0;

    // Number of cached entries (0 after construction/clear).
    virtual std::size_t size() const = 0;
};

// Factories (implemented by the SDK adapter — self-contained, no backend).
std::shared_ptr<IHeightmapErosion> create_heightmap_erosion();
std::shared_ptr<ITileErosionCache> create_tile_erosion_cache();

}  // namespace procgen
}  // namespace engine

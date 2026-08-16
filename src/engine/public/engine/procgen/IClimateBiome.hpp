#pragma once

// Public climate/biome/surface contracts (META section 18 / FALTANTES item 14).
//
// The hardcoded climate->biome decision tree and per-biome surface switch of
// the builtin generator become DATA: a climate sampler (six noise graphs, one
// per climate axis), a biome registry (ordered definitions with climate
// bounds, first match wins) and per-biome surface rules (first match wins by
// depth/height/slope). A data-driven voxel generator composes all three and
// implements the world's public IVoxelGenerator, so a project defines
// biomes/surfaces as assets without recompiling the engine — same contract
// family as INoiseGraph (self-contained, never leaks the backend).
//
// Climate axes follow the builtin convention: temperature/moisture and the
// other axes are normalized to [-1, 1] (the engine's surface code already
// thresholds on these values). Bounds are inclusive on the minimum and
// exclusive on the maximum, so adjacent biomes never overlap ambiguously.

#include "engine/procgen/INoiseGraph.hpp"
#include "engine/procgen/IWorldFeatures.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// Sampled climate at a world position (one axis per noise graph).
struct ClimatePoint {
    float temperature{ 0.0f };
    float moisture{ 0.0f };
    float continentalness{ 0.0f };
    float erosion{ 0.0f };
    float weirdness{ 0.0f };
    float river{ 0.0f };
};

// One data-driven surface rule inside a biome: places `blockId` when
// depth/height/slope all match. Depth is the distance below the terrain
// surface (0 = top block). Rules are evaluated in order, first match wins;
// when no rule matches the world keeps its builtin surface for the biome's
// mapped engine biome.
struct SurfaceRule {
    std::uint32_t blockId{ 0 };
    int minDepth{ 0 };
    int maxDepth{ 0x7FFFFFFF };
    int minHeight{ -0x7FFFFFFF - 1 };
    int maxHeight{ 0x7FFFFFFF };
    float minSlope{ 0.0f };
};

// Climate bounds used by biome selection (first definition whose bounds
// contain the sampled ClimatePoint wins; missing axes default to full range).
struct ClimateBounds {
    float minTemperature{ -1.0f }, maxTemperature{ 1.0f };
    float minMoisture{ -1.0f }, maxMoisture{ 1.0f };
    float minContinentalness{ -1.0f }, maxContinentalness{ 1.0f };
    float minErosion{ -1.0f }, maxErosion{ 1.0f };
    float minWeirdness{ -1.0f }, maxWeirdness{ 1.0f };
    float minRiver{ -1.0f }, maxRiver{ 1.0f };
};

// A data-driven biome definition. `engineBiomeIndex` maps the definition to
// the engine's BiomeType table (see src/simulation/voxel/generation/
// TerrainGenerator.hpp) so the builtin world places coherent surfaces for the
// depths no surface rule covers — e.g. cold -> Alpine/Glacial (snow top),
// hot+dry -> Desert (sand top).
struct BiomeDefinition {
    std::string name;
    ClimateBounds climate;
    std::uint8_t engineBiomeIndex{ 0 };
    std::vector<SurfaceRule> surface;
};

// Ordered biome registry. Deterministic: the same ClimatePoint always resolves
// to the same biome for a given definition list. Serializes to a versioned
// JSON asset; deserialize is all-or-nothing (on failure the registry keeps its
// previous valid state).
class IBiomeRegistry {
public:
    virtual ~IBiomeRegistry() = default;

    virtual std::size_t biome_count() const = 0;
    virtual bool biome_definition(std::size_t index, BiomeDefinition& out) const = 0;

    // First definition whose climate bounds contain `climate`. Returns false
    // when the table has no matching definition (no catch-all entry).
    virtual bool biome_for(const ClimatePoint& climate,
                           std::uint32_t& outIndex) const = 0;

    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Climate sampler: six deterministic noise graphs, one per axis (null axis
// samples 0.0f). The graphs are owned and mutable because deserialize restores
// them from the asset. Determinism is inherited from the graphs (same graphs +
// seed -> bit-identical ClimatePoints).
class IClimateSampler {
public:
    virtual ~IClimateSampler() = default;

    virtual ClimatePoint sample(float worldX, float worldZ) const = 0;

    // Versioned JSON asset: one embedded graph document (or null) per axis.
    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Evaluates a biome's surface rules (first match wins). Returns 0 when no rule
// matches — the caller (the world) then keeps the builtin surface of the
// mapped engine biome.
class ISurfaceResolver {
public:
    virtual ~ISurfaceResolver() = default;

    virtual std::uint32_t block_for(std::uint32_t biomeIndex,
                                    const ClimatePoint& climate, int height,
                                    int depth, float slope) const = 0;
};

// Data-driven voxel generator implementing the world's public IVoxelGenerator:
// height from a graph, climate from the sampler, biome from the registry and
// surface blocks from the resolver. `surface_block` returns 0 when no rule
// matches, so the world's builtin surface (for the mapped engine biome) fills
// the remaining depths.
class IClimateVoxelGenerator : public voxel::IVoxelGenerator {
public:
    virtual ~IClimateVoxelGenerator() = default;

    virtual ClimatePoint climate_at(float worldX, float worldZ) const = 0;
    // Registry-space biome index for the position.
    virtual std::uint32_t biome_at(float worldX, float worldZ) const = 0;
    // Engine BiomeType index the definition maps to (what TerrainPoint
    // carries; falls back to the neutral Plains biome when no definition
    // matches).
    virtual std::uint32_t engine_biome_at(float worldX, float worldZ) const = 0;
};

// Default registry: a compact engine-faithful climate table (ocean by
// continentalness, cold -> glacial/alpine, hot+dry -> desert/savanna/badlands,
// hot+wet -> jungle/swamp, meadow as catch-all).
std::shared_ptr<IBiomeRegistry> create_biome_registry();

// Builds a registry from a versioned JSON asset; nullptr + diagnostic on
// malformed input (no partial registry is returned).
std::shared_ptr<IBiomeRegistry> create_biome_registry_from_json(
    const std::string& json, std::string& errorOut);

// Any axis may be null (samples 0.0f). Graphs are owned by the sampler.
std::shared_ptr<IClimateSampler> create_climate_sampler(
    std::shared_ptr<INoiseGraph> temperature,
    std::shared_ptr<INoiseGraph> moisture,
    std::shared_ptr<INoiseGraph> continentalness,
    std::shared_ptr<INoiseGraph> erosion,
    std::shared_ptr<INoiseGraph> weirdness,
    std::shared_ptr<INoiseGraph> river);

std::shared_ptr<ISurfaceResolver> create_surface_resolver(
    std::shared_ptr<const IBiomeRegistry> registry);

// Data-driven voxel generator: surface height = baseHeight + round(height * amplitude),
// climate/biome/surface from the sampler/registry/resolver; caves/ores from
// the optional 3D density fields (null -> no caves/ores); ore distribution,
// carver fill and surface decorators from the optional world-feature tables
// (null -> builtin behavior for those hooks).
std::shared_ptr<IClimateVoxelGenerator> create_climate_voxel_generator(
    std::shared_ptr<const INoiseGraph> height,
    std::shared_ptr<const IClimateSampler> sampler,
    std::shared_ptr<const IBiomeRegistry> registry,
    std::shared_ptr<const ISurfaceResolver> resolver,
    std::shared_ptr<const IDensityFunction> caves,
    std::shared_ptr<const IDensityFunction> ores,
    int baseHeight, int amplitude,
    std::shared_ptr<const IOreTable> oreTable = nullptr,
    std::shared_ptr<const ICarver> carver = nullptr,
    std::shared_ptr<const IDecoratorSet> decoratorSet = nullptr);

}  // namespace procgen
}  // namespace engine

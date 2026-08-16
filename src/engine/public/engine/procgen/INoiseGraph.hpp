#pragma once

// Public procedural generation contracts (META section 18 / FALTANTES item 14).
//
// Codified generation becomes an editable ASSET: a noise graph is a list of
// typed nodes (the spec below), serializes to a stable JSON document and
// rebuilds deterministically from it. 3D density functions (caves, ores,
// extra terrain) and a data-driven voxel generator ride the same graph, so a
// project composes Minecraft-modern terrain/caves/ores without recompiling
// the engine.
//
// The SDK adapter implements this contract with the engine's in-project noise
// authority (FastNoiseLite — already used by the builtin TerrainGenerator);
// the catalog authority FastNoise2 is blocked at the promotion gate until its
// FastSIMD dependency is vendored (see DEPENDENCY_POLICY). The contract is
// self-contained and never leaks the backend. The same spec + seed yields
// bit-identical samples everywhere (determinism is seed-based, not
// thread-count-based).

#include "engine/voxel/IVoxelWorld.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// One typed node of the editable noise graph. `sources` reference EARLIER
// nodes by index (the graph is a DAG built in order). `params` are positional:
//
//   constant : [value]
//   value    : [frequency, seedOffset]
//   perlin   : [frequency, seedOffset]
//   simplex  : [frequency, seedOffset]
//   fbm      : sources=[source]   params=[octaves, gain, lacunarity, weightedStrength]
//   ridged   : sources=[source]   params=[octaves, gain, lacunarity, weightedStrength]
//   pingpong : sources=[source]   params=[octaves, gain, lacunarity, weightedStrength]
//   add      : sources=[a, b]
//   multiply : sources=[a, b]
//   min      : sources=[a, b]
//   max      : sources=[a, b]
//   lerp     : sources=[a, b, control]   -> lerp(a, b, control)
struct NoiseNodeSpec {
    std::string type;
    std::vector<float> params;
    std::vector<std::uint32_t> sources;
};

struct NoiseGraphSpec {
    std::uint32_t seed{ 0 };
    std::uint32_t root{ 0 };
    std::vector<NoiseNodeSpec> nodes;
};

// A seeded, deterministic noise graph. Sampling is in world-space units; the
// node frequencies/seed offsets are set in the spec. 2D drives height and
// climate, 3D drives density fields (caves/ores).
class INoiseGraph {
public:
    virtual ~INoiseGraph() = default;
    virtual const char* name() const = 0;
    virtual std::uint32_t seed() const = 0;
    virtual void set_seed(std::uint32_t seed) = 0;

    // Deterministic: identical spec + seed -> identical values for any caller,
    // any number of threads, any sampling order.
    virtual float sample_2d(float x, float z) const = 0;
    virtual float sample_3d(float x, float y, float z) const = 0;

    // Stable JSON asset form (versioned, editable). Round-trips exactly:
    // deserialize(serialize(g)) samples identically to g.
    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Continuous 3D density field over world space (caves, ores, extra terrain).
// The graph is the canonical implementation.
class IDensityFunction {
public:
    virtual ~IDensityFunction() = default;
    virtual float sample(float x, float y, float z) const = 0;
};

// Data-driven voxel generator implementing the world's public IVoxelGenerator
// contract: terrain height comes from the graph asset, caves/ores from 3D
// density fields. Projects compose worlds without recompiling the engine.
class IGraphVoxelGenerator : public voxel::IVoxelGenerator {
public:
    virtual ~IGraphVoxelGenerator() = default;
};

// Builds a graph from its spec. Fails with a diagnostic on unknown node
// types, out-of-range source indices or a root that does not exist.
std::shared_ptr<INoiseGraph> create_noise_graph_from_spec(
    const NoiseGraphSpec& spec, std::string& errorOut);

// Wraps a graph as a 3D density function: out = graph.sample_3d * scale + offset.
std::shared_ptr<IDensityFunction> create_graph_density_function(
    std::shared_ptr<const INoiseGraph> graph, float scale, float offset);

// Data-driven voxel generator: surface height = baseHeight + round(height * amplitude);
// cave_density/ore_density come from the 3D fields (positive = carved/ore).
std::shared_ptr<IGraphVoxelGenerator> create_graph_voxel_generator(
    std::shared_ptr<const INoiseGraph> height,
    std::shared_ptr<const IDensityFunction> caves,
    std::shared_ptr<const IDensityFunction> ores,
    int baseHeight, int amplitude);

}  // namespace procgen
}  // namespace engine

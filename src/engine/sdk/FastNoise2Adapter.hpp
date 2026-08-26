#pragma once

// FastNoise2 alternative backend for INoiseGraph.
// Uses the godot-voxel vendored fork (v0.10.0, self-contained FastSIMD).
// This is a separate factory from create_noise_graph_from_spec to avoid
// ODR violations. The test links this directly; production code can
// swap backends by calling the appropriate factory.

#include "engine/procgen/INoiseGraph.hpp"
#include <memory>
#include <string>

namespace engine {
namespace procgen {

// Creates an INoiseGraph backed by FastNoise2 (SIMD-accelerated).
// Same contract as create_noise_graph_from_spec: deterministic per seed.
// Returns nullptr on failure with errorOut populated.
std::shared_ptr<INoiseGraph> create_noise_graph_from_spec_fastnoise2(
    const NoiseGraphSpec& spec, std::string& errorOut);

}  // namespace procgen
}  // namespace engine

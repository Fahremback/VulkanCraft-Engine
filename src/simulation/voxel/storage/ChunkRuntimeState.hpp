#pragma once

#include "ChunkId.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

enum class ChunkState : uint8_t {
    Unloaded,
    Generating,
    MeshReady,
    Uploaded
};

// Scheduling/version state. It is separate from voxel data and disposable meshes.
struct ChunkRuntimeState {
    std::atomic<ChunkState> state{ChunkState::Unloaded};
    std::atomic_bool isDirty{false};
    std::atomic<uint64_t> dataVersion{0};
    // Revisions of the 4 neighbors observed at the last mesh of this chunk
    // (guarded by World::chunksMutex). Drives the border gap-fill gate.
    std::vector<NeighborSeen> meshNeighborSeen;
};

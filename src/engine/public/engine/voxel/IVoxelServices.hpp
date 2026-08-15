#pragma once

// Public voxel service contracts (META section 6). Each contract is published
// here without any renderer/engine dependency; the world exposes registration
// points so projects can swap implementations.
//
// Wiring status (2026-08-14):
//   - IVoxelEditService ......... WIRED — backed by the transactional edit path.
//   - IChunkStorage ............. WIRED — persistence delegates to a registered
//                                   service; the builtin version is the default.
//   - IVoxelGenerator ........... WIRED — see IVoxelWorld.hpp (register_generator).
//   - IVoxelMesher / IVoxelLighting / IVoxelFluidSimulation / IVoxelReplication
//     .......................... CONTRACT PUBLISHED — registrations are stored
//                                   and observable on the world; the runtime
//                                   integration lands with the storage / lighting /
//                                   fluids / replication milestones.
//
// Nothing here includes Vulkan, VMA or renderer internals.

#include "engine/voxel/IVoxelWorld.hpp"

#include <cstdint>
#include <string>

namespace engine {
namespace voxel {

// Explicit edit service: the transactional editing surface of the world.
// Every mutation flows through IVoxelTransaction and lands on the undo stack.
class IVoxelEditService {
public:
    virtual ~IVoxelEditService() = default;

    virtual std::unique_ptr<IVoxelTransaction> begin_transaction() = 0;
    virtual void set_block(int x, int y, int z, uint32_t blockId) = 0;
    virtual bool undo() = 0;
    virtual bool redo() = 0;
    virtual std::size_t undo_depth() const = 0;
};

// Chunk storage / persistence service. The world delegates save/load here; a
// project can supply its own backend (region files, compression, deltas) and
// keep the world API unchanged.
class IChunkStorage {
public:
    virtual ~IChunkStorage() = default;

    virtual std::string serialize_world(std::string& errorOut) = 0;
    virtual bool deserialize_world(const std::string& data, std::string& errorOut) = 0;
    virtual bool save_world(const std::string& filePath, std::string& errorOut) = 0;
    virtual bool load_world(const std::string& filePath, std::string& errorOut) = 0;
};

// CPU mesh production contract. The builtin VoxelMesher is the default; a
// project-registered mesher takes over when the meshing milestone lands.
class IVoxelMesher {
public:
    virtual ~IVoxelMesher() = default;
    // Returns a display name for diagnostics (e.g. "builtin", "greedy").
    virtual const char* name() const = 0;
};

// Discrete world lighting contract (skylight + block light).
class IVoxelLighting {
public:
    virtual ~IVoxelLighting() = default;
    virtual const char* name() const = 0;
};

// Generalized fluid simulation contract (water/lava defined by data).
class IVoxelFluidSimulation {
public:
    virtual ~IVoxelFluidSimulation() = default;
    virtual const char* name() const = 0;
};

// Authoritative voxel replication contract (server <-> clients).
class IVoxelReplication {
public:
    virtual ~IVoxelReplication() = default;
    virtual const char* name() const = 0;
};

}  // namespace voxel
}  // namespace engine

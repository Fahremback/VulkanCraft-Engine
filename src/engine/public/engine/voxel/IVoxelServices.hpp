#pragma once

// Public voxel service contracts (META section 6). Each contract is published
// here without any renderer/engine dependency; the world exposes registration
// points so projects can swap implementations.
//
// Wiring status (2026-08-15):
//   - IVoxelEditService ......... WIRED — backed by the transactional edit path.
//   - IChunkStorage ............. WIRED — persistence delegates to a registered
//                                   service; the builtin version is the default.
//   - IVoxelGenerator ........... WIRED — see IVoxelWorld.hpp (register_generator).
//   - IVoxelLighting ............ WIRED (item 2) — the world's light pass
//                                   consults the registered plugin's per-block
//                                   light_property_overrides() before its
//                                   builtin tables (default: builtin behavior).
//   - IVoxelMesher .............. WIRED (item 2) — the world's runtime block
//                                   table (what the mesher consumes) is derived
//                                   from the registry AND the registered
//                                   plugin's mesh_policy_overrides().
//   - IVoxelFluidSimulation ..... WIRED — per-block fluid behavior is already
//                                   substituted data-driven via set_fluid_table
//                                   (the FluidRegistry is the project's fluid
//                                   plugin); algorithm-level substitution
//                                   (replacing the cell simulation itself)
//                                   stays with the fluids milestone.
//   - IVoxelReplication ......... WIRED — see engine/voxel/IVoxelReplication.hpp;
//                                   the SDK adapter (create_voxel_replication)
//                                   is the server-authoritative replication
//                                   layer over the generic networking runtime.
//
// Nothing here includes Vulkan, VMA or renderer internals.

#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/voxel/IVoxelReplication.hpp"

#include <cstdint>
#include <string>
#include <vector>

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

    // Optional PAGED surface (FALTANTES §4 item 1). A region/paged backend
    // implements these so the world persists per-page payloads — one page per
    // region tile ("r.<x>.<z>") plus a "world" manifest page — instead of one
    // monolithic blob. The WORLD owns the page encoding (chunk/entity data);
    // the backend only stores opaque bytes (page id -> payload). Default
    // implementations report "not a paged store", so the facade falls back to
    // the monolithic path transparently.
    virtual bool supports_regions() const { return false; }
    virtual bool save_page(const std::string& pageId, const std::string& payload,
                           std::string& errorOut) {
        errorOut = "chunk store does not support paged saves";
        return false;
    }
    virtual bool load_page(const std::string& pageId, std::string& payloadOut,
                           std::string& errorOut) {
        errorOut = "chunk store does not support paged saves";
        return false;
    }
    virtual std::vector<std::string> page_ids() const { return {}; }

    // WAL commit (FALTANTES §4 item 8): after a paged save wrote every page,
    // the world calls this to make the save permanent — a journaling backend
    // drops its write-ahead log here (the default store has no journal, so
    // the default is a no-op). A backend that journals undo data in
    // save_page must NOT leave the journal on a successful save: an
    // interrupted save is recovered on the NEXT load_world by rolling the
    // partially-written pages back to the last committed save.
    virtual bool commit_save(std::string& errorOut) {
        errorOut.clear();
        return true;
    }
};

// Per-block mesh policy a mesher plugin can own (item 2): mirrors the
// RuntimeBlockInfo hints the builtin mesher reads, so a plugin changes
// face-culling/rendering for blocks WITHOUT modifying the core. Ids are the
// world's runtime block ids (builtin prefix + registry dynamic ids).
struct BlockMeshPolicy {
    bool occludes{ true };      // face-culling hint (false = draw vs opaque)
    bool transparent{ false };  // render pass / culling hints
    uint8_t renderLayer{ 0 };   // 0 = opaque world layer
};

// CPU mesh production contract. The builtin VoxelMesher is the default; a
// project-registered mesher takes over the mesh POLICY (per-block face-culling
// /render hints) via mesh_policy_overrides — the world's runtime block table,
// which the mesher consumes, is derived from the registry AND the plugin.
class IVoxelMesher {
public:
    virtual ~IVoxelMesher() = default;
    // Returns a display name for diagnostics (e.g. "builtin", "greedy").
    virtual const char* name() const = 0;
    // Optional per-block mesh policy overrides merged into the world's runtime
    // block table at registration. Empty = builtin (registry-derived) behavior.
    virtual std::vector<std::pair<uint32_t, BlockMeshPolicy>>
    mesh_policy_overrides() { return {}; }
};

// Per-block light properties a lighting plugin can own (item 2): the world's
// light pass consults these overrides before its builtin tables, so a plugin
// changes lighting without modifying the core. emission/absorption are the
// world's discrete units (0..15; 15 absorption = opaque, blocks light).
struct BlockLightProperties {
    uint8_t emission{ 0 };
    uint8_t absorption{ 15 };
};

// Discrete world lighting contract (skylight + block light). The builtin
// behavior (registry-derived emission/absorption) is the default; a registered
// plugin's overrides take precedence per block id.
class IVoxelLighting {
public:
    virtual ~IVoxelLighting() = default;
    virtual const char* name() const = 0;
    // Optional per-block light overrides the world merges into its light
    // tables at registration. Empty = builtin behavior.
    virtual std::vector<std::pair<uint32_t, BlockLightProperties>>
    light_property_overrides() { return {}; }
};

// Generalized fluid simulation contract (water/lava defined by data).
class IVoxelFluidSimulation {
public:
    virtual ~IVoxelFluidSimulation() = default;
    virtual const char* name() const = 0;
};

// Authoritative voxel replication contract (server <-> clients): full
// transport-free contract in engine/voxel/IVoxelReplication.hpp — chunk
// streaming by interest, ordered snapshots/deltas, server-validated
// transactions, prediction/correction, reconnect resync and dedicated-server
// persistence.

}  // namespace voxel
}  // namespace engine

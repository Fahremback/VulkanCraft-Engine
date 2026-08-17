#pragma once

// Public streaming, budget and observability contract (FALTANTES §3). The
// world exposes a snapshot of its streaming state — chunk state census,
// budgets and worker/entity counts — that projects, servers, tools and
// editors can read headless at any time, plus an optional push monitor that
// receives a snapshot after each update() where streaming state changed.
//
// Self-contained: no Vulkan, no internals, only primitives and shared_ptr.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace voxel {

// Streaming/budget snapshot (FALTANTES §3). All values are best-effort live
// measurements of the world's streaming state, safe to read from any thread
// the world's query methods support. Counts are a census over the loaded
// chunk set: every present chunk is in exactly one non-Unloaded state, so
// chunksGenerating + chunksMeshReady + chunksUploaded == chunksLoaded.
struct StreamingSnapshot {
    // --- Budgets (mirror of the world's streaming controls) ---
    int chunkBudget{ 0 };       // current streaming budget (chunk radius)
    int chunkBudgetMin{ 0 };    // clamp range of set_chunk_budget
    int chunkBudgetMax{ 0 };
    int farLodPercent{ 0 };     // far-LOD endpoint 0..100 (100 = farthest)
    std::size_t workerThreads{ 0 };  // world worker pool size (0 = unknown)

    // --- RAM budget (FALTANTES §4 item 6) ---
    uint64_t memoryBudgetBytes{ 0 };  // set_memory_budget; 0 = unlimited
    uint64_t ramUsageBytes{ 0 };      // estimated loaded chunk data bytes

    // --- Chunk state census ---
    std::size_t chunksLoaded{ 0 };      // present chunks (any state)
    std::size_t chunksGenerating{ 0 };  // Generating
    std::size_t chunksMeshReady{ 0 };   // MeshReady
    std::size_t chunksUploaded{ 0 };    // Uploaded
    std::size_t chunksDirty{ 0 };       // awaiting remesh
    std::size_t lightDirtyChunks{ 0 };  // awaiting relight

    // --- Other streaming-relevant state ---
    std::size_t blockEntities{ 0 };     // attached block entities
    std::size_t activeFluidCells{ 0 };  // fluid cells in the active queue

    // Structural equality: the monitor uses it to fire only on real change.
    bool operator==(const StreamingSnapshot&) const = default;
};

// Public view of one runtime block table entry (FALTANTES §3 item 2): what
// the world knows about a block — identity, solidity and the mesh/light hints
// the mesher and the light pass consume. Ids are the world's runtime uint32
// block ids (builtin prefix + registry dynamic ids). Tools/editors use this to
// inspect the EFFECTIVE table after the registry and plugin overrides merge.
struct BlockRuntimeView {
    uint32_t id{ 0 };
    std::string uuid;            // persistent identity (empty = builtin)
    bool solid{ false };         // collision / raycast
    bool transparent{ false };   // render pass / culling hints
    bool occludes{ true };       // face-culling hint (plugin-mergeable)
    uint8_t renderLayer{ 0 };    // 0 = opaque world layer
    uint8_t lightEmission{ 0 };  // 0..15 (plugin-mergeable)
    uint8_t lightAbsorption{ 15 };  // 0..15 (plugin-mergeable)

    // Physical material (FALTANTES §16 item 7 + item 10): mirror of the
    // registry's friction/bounciness/density/resistance/flammability,
    // consumed by the physics layer (terrain slab colliders carry the surface
    // block's material; density feeds mass scaling for debris/destruction;
    // resistance + flammability drive the explosion response). Data-driven:
    // the JSON registry is the single source, this view is what physics
    // consumers read.
    float friction{ 0.5f };      // 0..1 contact friction
    float bounciness{ 0.0f };    // 0..1 restitution
    float density{ 1.0f };       // mass per unit volume (> 0)
    float resistance{ 0.0f };    // blast/explosion resistance (>= 0)
    float flammability{ 0.0f };  // 0..1 heat/ignition axis
};

// Push observability (FALTANTES §3): an optional monitor installed on the
// world receives a snapshot after every update() where streaming state
// changed. The monitor is owned by the world (shared_ptr); setting a null
// pointer clears it. Fired on the frame thread, so the callback must not
// block or re-enter the world.
class IVoxelStreamingMonitor {
public:
    virtual ~IVoxelStreamingMonitor() = default;
    virtual void on_streaming_update(const StreamingSnapshot& snapshot) = 0;
};

}  // namespace voxel
}  // namespace engine

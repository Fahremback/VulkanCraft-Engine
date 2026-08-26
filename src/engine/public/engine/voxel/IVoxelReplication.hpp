#pragma once

// Authoritative voxel replication (META section 17 / FALTANTES item 13).
//
// A reusable layer over the generic networking runtime (ConnectionRegistry +
// RpcQueue in engine/networking): the server is the authority of the voxel
// world, clients stream chunks by interest and receive ordered deltas, block
// edits are validated server-side (bounds, loaded chunk, block registry,
// per-connection cooldown), predictions are corrected by the authoritative
// result, and the dedicated server persists through the world save.
//
// The contract is transport-free: connections are plain uint ids and all
// messages are plain data objects. The SDK adapter (create_voxel_replication)
// bridges to NetworkingRuntime/SocketTransport and keeps the generic headers
// behind the module boundary. A project runs this same service as dedicated
// server, client or local host by using the server or client methods.
//
// Self-contained: public SDK headers + glm only.

#include "engine/compression/ICompressionProvider.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace voxel {

class IVoxelWorld;

using ReplicationConnectionId = std::uint32_t;

// Chunk size the replication layer streams (matches the voxel chunk grid).
inline constexpr int kReplicationChunkSize = 16;
// Server-side default snapshot window (world blocks per chunk, minY..minY+height).
inline constexpr int kReplicationDefaultMinY = 0;
inline constexpr int kReplicationDefaultHeight = 128;
// Minimum server ticks between accepted edits from one connection (anti-spam).
inline constexpr std::uint64_t kReplicationEditCooldownTicks = 2;

// One authoritative block change. `blockId` 0 removes the block (Air).
// `sequence` orders messages per connection (shared with snapshots);
// `revision` is the per-position monotonic revision on the server.
struct BlockReplicationDelta {
    glm::ivec3 position{0, 0, 0};
    std::uint32_t blockId{0};
    std::uint32_t previousBlockId{0};
    std::uint32_t sequence{0};
    std::uint32_t revision{0};

    bool operator==(const BlockReplicationDelta&) const = default;
};

// Server -> client delivery unit. `deltas` are ordered by sequence;
// `rejected` lists client predictions the server refused (revert them).
struct ReplicationBatch {
    std::uint32_t sequence{0};  // max delta sequence (diagnostics)
    std::vector<BlockReplicationDelta> deltas;
    std::vector<glm::ivec3> rejected;
};

// Full block window of one voxel chunk. Layout:
//   blocks[(y * kReplicationChunkSize + z) * kReplicationChunkSize + x]
// with y in [0, height), blockId 0 = Air. Size is 16*16*height.
struct ChunkReplicationSnapshot {
    int chunkX{0};
    int chunkZ{0};
    int minY{0};
    std::uint32_t height{0};
    std::uint32_t sequence{0};
    std::vector<std::uint32_t> blocks;
};

// One block entity's replicated state: world position, stable namespaced type
// id, the entity's data version and its opaque project blob (META section 17 /
// FALTANTES item 6 — region replication of block entities). The engine never
// interprets the blob; the receiving side reconstructs the entity through the
// registered factory and deserialize_state (same contract as save/load).
struct BlockEntityReplicationState {
    glm::ivec3 position{0, 0, 0};
    std::string typeId;
    std::uint32_t dataVersion{0};
    std::vector<std::uint8_t> blob;

    bool operator==(const BlockEntityReplicationState&) const = default;
};

// A relevant fluid/light cell in a region snapshot (sparse: the server only
// enumerates cells where the layer differs from its default — fluid present,
// sky light below full or block light above zero — so the snapshot carries
// state without the bulk of unchanged cells). The receiving world converges
// deterministically to the same values after applying the region (same
// engine); the cells are the authoritative record of those layers.
struct FluidLightReplicationCell {
    glm::ivec3 position{0, 0, 0};
    std::uint8_t fluidLevel{0xFF};  // 0xFF = no fluid
    std::uint8_t skyLight{15};      // full sun
    std::uint8_t blockLight{0};

    bool operator==(const FluidLightReplicationCell&) const = default;
};

// Full region state-sync unit (META section 17 / FALTANTES item 6): the block
// windows of every loaded chunk in a client's interest plus the block entity
// states, relevant fluid/light cells and entity snapshots (with project
// components — a §14 inventory is a component whose blob is the inventory's
// JSON) inside the region. The client applies the whole bundle to converge to
// the authoritative region.
struct RegionReplicationSnapshot {
    std::uint32_t sequence{0};
    glm::ivec3 origin{0, 0, 0};  // interest position (server authority)
    int chunkRadius{0};
    std::vector<ChunkReplicationSnapshot> chunks;
    std::vector<BlockEntityReplicationState> blockEntities;
    std::vector<FluidLightReplicationCell> cells;
    std::vector<entity::EntitySnapshot> entities;
};// A client's streaming interest: world position + chunk radius around it.
struct ReplicationInterest {
    glm::ivec3 position{ 0, 0, 0 };
    int chunkRadius{ 0 };
};

// One catalog-only block in the server-negotiated identity palette (FALTANTES
// item 1 — identity stability / server-negotiated palette). Blocks are
// replicated by RUNTIME id; a JSON-only block's id is dynamic (>= the builtin
// prefix) and only meaningful to a client that registered the same definition
// (the same UUID allocates the same id — deterministic, load-order
// independent). The server is the authority: it ships every catalog-only
// block's full definition JSON (the exact asset form load_from_json parses,
// via serialize_block_definition), so a client reconstructs the block and its
// material without recompiling.
struct ReplicationPaletteEntry {
    std::uint32_t runtimeId{ 0 };  // the server's runtime id for this block
    std::string uuid;              // persistent identity (canonical UUID)
    std::string namespacedName;    // "ns:name"
    std::string definitionJson;    // single-object BlockDefinition asset

    bool operator==(const ReplicationPaletteEntry&) const = default;
};

// The authoritative block palette for one connection: every catalog-only
// (dynamic id) block the world knows. Builtins are the engine's contract —
// their ids are the stable builtin prefix both sides share — and are NOT
// shipped. Send the palette before the first snapshot/delta so the client can
// resolve dynamic ids; deltas/snapshots/regions referencing a palette block
// then apply verbatim.
struct ReplicationPalette {
    std::vector<ReplicationPaletteEntry> entries;
};

struct ReplicationEditResult {
    bool accepted{false};
    std::string error;
    std::uint32_t revision{0};
};

// Authoritative voxel replication service. One instance is bound to one world
// and plays one role (server methods for the authority, client methods for a
// remote/local client); the same project hosts both for local play.
class IVoxelReplication {
public:
    virtual ~IVoxelReplication() = default;
    virtual const char* name() const = 0;

    // ---- server (authority) ----
    virtual void server_register_connection(ReplicationConnectionId connection) = 0;
    virtual void server_unregister_connection(ReplicationConnectionId connection) = 0;
    virtual void server_set_interest(ReplicationConnectionId connection,
                                     const ReplicationInterest& interest) = 0;
    // Validates and applies a client edit against the authoritative world.
    // Rejections (unknown connection, cooldown, bounds, unloaded chunk, block
    // registry) never mutate the world and are reported to the client.
    virtual ReplicationEditResult server_submit_edit(ReplicationConnectionId connection,
                                                     int x, int y, int z,
                                                     std::uint32_t blockId) = 0;
    // Packs the full region of `connection`'s interest as one state-sync unit:
    // chunk block windows (existing snapshots) + block entity states + the
    // relevant fluid/light cells + entity snapshots inside the region.
    // Deterministic: the same authoritative world and interest always produce
    // the same region. False with a diagnostic for an unknown/unregistered
    // connection. Call after server_update() so the world state is current.
    virtual bool server_pack_region(ReplicationConnectionId connection,
                                    RegionReplicationSnapshot& out,
                                    std::string& errorOut) = 0;

    // Pump: advances the server tick and streams newly-in-interest chunks.
    // Call before pack_batch/pack_interest after submitting edits.
    virtual void server_update() = 0;
    virtual ReplicationBatch server_pack_batch(ReplicationConnectionId connection) = 0;
    virtual std::vector<ChunkReplicationSnapshot> server_pack_interest(
        ReplicationConnectionId connection) = 0;
    virtual void server_set_snapshot_window(int minY, int height) = 0;
    // Broadcasts committed edits to every registered connection (FALTANTES
    // §7 item 137). Called by the world's transaction service after a
    // successful commit, so edits committed directly on the authoritative
    // world (editor/MCP/host, not only through server_submit_edit) reach
    // clients as ordered deltas. No-op edits (blockId == previousBlockId)
    // and unregistered connections are skipped; per-position revisions stay
    // monotonic and match the delta the same edit would produce through
    // server_submit_edit. This is the single broadcast point —
    // server_submit_edit delegates here after its own commit.
    virtual void server_broadcast_edits(const std::vector<BlockEdit>& edits) = 0;
    // Dedicated server persistence: the authoritative world save.
    virtual bool server_save(const std::string& filePath, std::string& errorOut) = 0;
    virtual std::size_t server_edit_count() const = 0;

    // ---- client ----
    // Optimistic break/place: applies locally immediately and remembers the
    // pre-edit block so the authoritative result can confirm or correct.
    virtual bool client_predict(int x, int y, int z, std::uint32_t blockId) = 0;
    virtual void client_apply_batch(const ReplicationBatch& batch) = 0;
    virtual void client_apply_snapshot(const ChunkReplicationSnapshot& snapshot) = 0;
    // Applies a full region state-sync unit: chunk windows (blocks), then
    // block entity reconcile (attach snapshot entities via the registered
    // factories and deserialize their blobs; remove stale entities inside the
    // region), then entity region reconcile (despawn region entities not in
    // the snapshot; spawn/apply snapshot entities with their components — a
    // §14 inventory rides as a component blob). All-or-nothing for block
    // entities: every snapshot type must have a registered factory or nothing
    // is mutated and an error is reported.
    virtual bool client_apply_region(const RegionReplicationSnapshot& region,
                                     std::string& errorOut) = 0;
    virtual std::uint32_t client_applied_sequence() const = 0;
    virtual std::size_t client_pending_predictions() const = 0;
    virtual std::size_t client_stale_dropped() const = 0;

    // ---- block identity palette (FALTANTES item 1) ----
    // Server: packs the authoritative block palette for `connection` — every
    // catalog-only (dynamic id) block, as its full definition JSON + runtime
    // id (builtins excluded: the builtin prefix is the engine contract both
    // sides share). Deterministic for a given world + registry. The transport
    // ships this before the first snapshot/delta so the client can reconstruct
    // dynamic ids. False with a diagnostic for an unknown/unregistered
    // connection or a world without a registry.
    virtual bool server_pack_palette(ReplicationConnectionId connection,
                                     ReplicationPalette& out,
                                     std::string& errorOut) = 0;
    // Client: registers the transported definitions into the local world's
    // block registry — the server's definition WINS by UUID, the client's own
    // blocks are kept — and re-attaches it, so dynamic ids resolve exactly
    // like the server's (UUID-sorted allocation). All-or-nothing: a malformed
    // entry (empty uuid/definition, unknown uuid, unparseable JSON, a builtin
    // mapping) leaves the client's registry untouched and returns a diagnostic.
    virtual bool client_apply_palette(const ReplicationPalette& palette,
                                      std::string& errorOut) = 0;
};

// ---- Codec (batching + compression, META 17) ----
// Little-endian frames; with a provider the payload is zstd-compressed
// (flag byte). Round-trips exactly; decode fails on malformed frames.

std::vector<std::byte> encode_replication_batch(
    const ReplicationBatch& batch,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

bool decode_replication_batch(
    const std::vector<std::byte>& data, ReplicationBatch& out,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

std::vector<std::byte> encode_replication_snapshot(
    const ChunkReplicationSnapshot& snapshot,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

bool decode_replication_snapshot(
    const std::vector<std::byte>& data, ChunkReplicationSnapshot& out,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

std::vector<std::byte> encode_replication_region(
    const RegionReplicationSnapshot& region,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

bool decode_replication_region(
    const std::vector<std::byte>& data, RegionReplicationSnapshot& out,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

std::vector<std::byte> encode_replication_palette(
    const ReplicationPalette& palette,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

bool decode_replication_palette(
    const std::vector<std::byte>& data, ReplicationPalette& out,
    std::shared_ptr<const compression::ICompressionProvider> compression = nullptr);

// Factory: builds the adapter bound to `world` (the implementation lives in
// the engine SDK module; generic networking headers stay behind it).
std::shared_ptr<IVoxelReplication> create_voxel_replication(IVoxelWorld& world);

}  // namespace voxel
}  // namespace engine

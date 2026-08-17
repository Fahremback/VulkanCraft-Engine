#pragma once

// Public world-scoped replication contract (META §19 / FALTANTES item —
// "Implementar replicação e interesse por mundo"). A replication service over
// the IWorldManager that routes authoritative replication BY WORLD:
//
//   - interest carries the world identity (WorldReplicationInterest =
//     worldName + voxel interest), so a connection observes exactly ONE
//     world at a time and two worlds with overlapping coordinates never
//     leak state between their observers;
//   - a portal crossing (travel) is a pure interest transition: the
//     connection's interest moves to another world and the server re-binds
//     it to that world's replication (its chunks/batches/edits now come
//     from the destination world);
//   - edits are routed BY WORLD: server_submit_edit mutates the world the
//     connection is interested in, and server_broadcast_edits(worldName)
//     reaches only the connections registered on that world;
//   - the client side binds to ONE world at a time (client_bind) and its
//     predict/apply methods act on that world's local IVoxelWorld.
//
// The contract is transport-free (connections are plain uint ids, messages
// are the voxel replication data objects). The adapter
// (src/engine/sdk/WorldReplication.cpp, the only TU) is a pure composition
// of the public IWorldManager + per-world IVoxelReplication services — no
// backend of its own. Self-contained: public SDK headers + glm only.

#include "engine/voxel/IVoxelReplication.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/world/IWorldManager.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace world {

// A connection's streaming interest: WHICH world + the voxel interest inside
// it. The world name is authoritative (a connection observes one world).
struct WorldReplicationInterest {
    std::string worldName;
    voxel::ReplicationInterest interest;
};

// World-scoped replication service. Server methods are the authority over the
// manager's worlds; client methods act on one locally bound world.
class IWorldReplication {
public:
    virtual ~IWorldReplication() = default;

    // ---- server (authority over the manager's worlds) ----
    // Registers a connection with its initial per-world interest. Fails
    // all-or-nothing (unknown world / already registered) and never mutates.
    virtual bool server_register_connection(
        voxel::ReplicationConnectionId connection,
        const WorldReplicationInterest& interest, std::string& errorOut) = 0;
    virtual void server_unregister_connection(
        voxel::ReplicationConnectionId connection) = 0;
    // Re-evaluates a connection's interest. When the world CHANGES (portal
    // crossing / travel) the connection is atomically re-bound: it stops
    // observing the old world and starts observing the new one on the next
    // pump. Fails all-or-nothing (unknown connection/world).
    virtual bool server_set_interest(
        voxel::ReplicationConnectionId connection,
        const WorldReplicationInterest& interest, std::string& errorOut) = 0;
    virtual WorldReplicationInterest server_interest(
        voxel::ReplicationConnectionId connection) const = 0;

    // Validates and applies a client edit against the authoritative world
    // the connection is interested in. Same semantics as the per-world
    // service (rejections never mutate).
    virtual voxel::ReplicationEditResult server_submit_edit(
        voxel::ReplicationConnectionId connection, int x, int y, int z,
        std::uint32_t blockId) = 0;
    // Packs the full region of the connection's interest (its world) as one
    // state-sync unit. Deterministic per (world, interest).
    virtual bool server_pack_region(
        voxel::ReplicationConnectionId connection,
        voxel::RegionReplicationSnapshot& out, std::string& errorOut) = 0;

    // Pump: advances EVERY world's server tick (each world streams its own
    // newly-in-interest chunks). Call before pack_batch/pack_interest.
    virtual void server_update() = 0;
    virtual voxel::ReplicationBatch server_pack_batch(
        voxel::ReplicationConnectionId connection) = 0;
    virtual std::vector<voxel::ChunkReplicationSnapshot> server_pack_interest(
        voxel::ReplicationConnectionId connection) = 0;
    virtual void server_set_snapshot_window(int minY, int height) = 0;
    // Broadcasts committed edits of ONE world to every connection registered
    // on that world (FALTANTES §7 item 137 semantics, world-scoped). Edits of
    // world A never reach connections observing world B.
    virtual void server_broadcast_edits(
        const std::string& worldName,
        const std::vector<voxel::BlockEdit>& edits) = 0;
    virtual bool server_save(const std::string& worldName,
                             const std::string& filePath,
                             std::string& errorOut) = 0;
    virtual std::size_t server_edit_count() const = 0;

    // ---- client (acts on the locally bound world) ----
    // Binds the client side to ONE world. All predict/apply calls act on that
    // world's local IVoxelWorld. Fails all-or-nothing on unknown world.
    virtual bool client_bind(const std::string& worldName,
                             std::string& errorOut) = 0;
    virtual std::string client_world() const = 0;
    virtual bool client_predict(int x, int y, int z,
                                std::uint32_t blockId) = 0;
    virtual void client_apply_batch(const voxel::ReplicationBatch& batch) = 0;
    virtual void client_apply_snapshot(
        const voxel::ChunkReplicationSnapshot& snapshot) = 0;
    virtual bool client_apply_region(
        const voxel::RegionReplicationSnapshot& region,
        std::string& errorOut) = 0;
    virtual std::uint32_t client_applied_sequence() const = 0;
    virtual std::size_t client_pending_predictions() const = 0;
    virtual std::size_t client_stale_dropped() const = 0;
};

// The only implementation of IWorldReplication
// (src/engine/sdk/WorldReplication.cpp).
std::unique_ptr<IWorldReplication> create_world_replication(
    IWorldManager& manager);

}  // namespace world
}  // namespace engine

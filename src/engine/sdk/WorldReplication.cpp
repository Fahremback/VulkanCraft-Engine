// WorldReplication.cpp — the only translation unit implementing the public
// IWorldReplication (META §19 / FALTANTES "replicação e interesse por
// mundo"). A pure composition of the public IWorldManager and the per-world
// IVoxelReplication service: each manager world gets its own replication
// server (lazily created on first registration), a connection observes ONE
// world at a time (its interest carries the world identity), interest
// transitions (portal crossings) atomically re-bind the connection to the
// destination world's service, and edits are routed BY WORLD — a committed
// edit of world A is broadcast only to the connections registered on A.
// No backend of its own.

#include "engine/world/IWorldReplication.hpp"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace world {
namespace {

class WorldReplicationImpl final : public IWorldReplication {
public:
    explicit WorldReplicationImpl(IWorldManager& manager) : manager_(manager) {}

    // ---- server -----------------------------------------------------------

    bool server_register_connection(
        voxel::ReplicationConnectionId connection,
        const WorldReplicationInterest& interest,
        std::string& errorOut) override {
        if (connections_.count(connection) != 0) {
            errorOut = "replication: connection already registered";
            return false;
        }
        if (!manager_.has_world(interest.worldName)) {
            errorOut = "replication: unknown world '" + interest.worldName + "'";
            return false;
        }
        voxel::IVoxelReplication* service = service_for(interest.worldName);
        if (service == nullptr) {
            errorOut = "replication: world '" + interest.worldName +
                       "' has no entity layer";
            return false;
        }
        service->server_register_connection(connection);
        service->server_set_interest(connection, interest.interest);
        connections_[connection] = interest;
        errorOut.clear();
        return true;
    }

    void server_unregister_connection(
        voxel::ReplicationConnectionId connection) override {
        const auto it = connections_.find(connection);
        if (it == connections_.end()) return;
        voxel::IVoxelReplication* service = service_for(it->second.worldName);
        if (service != nullptr) service->server_unregister_connection(connection);
        connections_.erase(it);
    }

    bool server_set_interest(voxel::ReplicationConnectionId connection,
                             const WorldReplicationInterest& interest,
                             std::string& errorOut) override {
        const auto it = connections_.find(connection);
        if (it == connections_.end()) {
            errorOut = "replication: unknown connection";
            return false;
        }
        if (!manager_.has_world(interest.worldName)) {
            errorOut = "replication: unknown world '" + interest.worldName + "'";
            return false;
        }
        voxel::IVoxelReplication* service = service_for(interest.worldName);
        if (service == nullptr) {
            errorOut = "replication: world '" + interest.worldName +
                       "' has no entity layer";
            return false;
        }
        if (it->second.worldName != interest.worldName) {
            // Portal crossing / travel: atomically re-bind. Unregister from
            // the OLD world's service, register + set interest on the NEW
            // world's service, then commit the new interest. A failure in the
            // middle leaves the connection on the old world untouched.
            voxel::IVoxelReplication* oldService =
                service_for(it->second.worldName);
            if (oldService != nullptr)
                oldService->server_unregister_connection(connection);
            service->server_register_connection(connection);
            service->server_set_interest(connection, interest.interest);
            it->second = interest;
        } else {
            // Same world: just re-evaluate the voxel interest.
            service->server_set_interest(connection, interest.interest);
            it->second.interest = interest.interest;
        }
        errorOut.clear();
        return true;
    }

    WorldReplicationInterest server_interest(
        voxel::ReplicationConnectionId connection) const override {
        const auto it = connections_.find(connection);
        if (it == connections_.end()) return {};
        return it->second;
    }

    voxel::ReplicationEditResult server_submit_edit(
        voxel::ReplicationConnectionId connection, int x, int y, int z,
        std::uint32_t blockId) override {
        voxel::ReplicationEditResult result;
        const auto it = connections_.find(connection);
        if (it == connections_.end()) {
            result.accepted = false;
            result.error = "replication: unknown connection";
            return result;
        }
        voxel::IVoxelReplication* service = service_for(it->second.worldName);
        if (service == nullptr) {
            result.accepted = false;
            result.error = "replication: world service unavailable";
            return result;
        }
        return service->server_submit_edit(connection, x, y, z, blockId);
    }

    bool server_pack_region(voxel::ReplicationConnectionId connection,
                            voxel::RegionReplicationSnapshot& out,
                            std::string& errorOut) override {
        const auto it = connections_.find(connection);
        if (it == connections_.end()) {
            errorOut = "replication: unknown connection";
            return false;
        }
        voxel::IVoxelReplication* service = service_for(it->second.worldName);
        if (service == nullptr) {
            errorOut = "replication: world service unavailable";
            return false;
        }
        return service->server_pack_region(connection, out, errorOut);
    }

    void server_update() override {
        for (auto& [name, service] : services_) {
            (void)name;
            service->server_update();
        }
    }

    voxel::ReplicationBatch server_pack_batch(
        voxel::ReplicationConnectionId connection) override {
        const auto it = connections_.find(connection);
        if (it == connections_.end()) return {};
        voxel::IVoxelReplication* service = service_for(it->second.worldName);
        if (service == nullptr) return {};
        return service->server_pack_batch(connection);
    }

    std::vector<voxel::ChunkReplicationSnapshot> server_pack_interest(
        voxel::ReplicationConnectionId connection) override {
        const auto it = connections_.find(connection);
        if (it == connections_.end()) return {};
        voxel::IVoxelReplication* service = service_for(it->second.worldName);
        if (service == nullptr) return {};
        return service->server_pack_interest(connection);
    }

    void server_set_snapshot_window(int minY, int height) override {
        // STICKY: remember the window and apply it to every EXISTING service,
        // and to any service created later (the per-world services are lazy —
        // a world that gets its first connection after this call must still
        // use the configured window).
        snapshotMinY_ = minY;
        snapshotHeight_ = height;
        for (auto& [name, service] : services_) {
            (void)name;
            service->server_set_snapshot_window(minY, height);
        }
    }

    void server_broadcast_edits(
        const std::string& worldName,
        const std::vector<voxel::BlockEdit>& edits) override {
        if (!manager_.has_world(worldName)) return;
        voxel::IVoxelReplication* service = service_for(worldName);
        if (service == nullptr) return;
        service->server_broadcast_edits(edits);
    }

    bool server_save(const std::string& worldName,
                     const std::string& filePath,
                     std::string& errorOut) override {
        if (!manager_.has_world(worldName)) {
            errorOut = "replication: unknown world '" + worldName + "'";
            return false;
        }
        voxel::IVoxelReplication* service = service_for(worldName);
        if (service == nullptr) {
            errorOut = "replication: world service unavailable";
            return false;
        }
        return service->server_save(filePath, errorOut);
    }

    std::size_t server_edit_count() const override {
        std::size_t total = 0;
        for (const auto& [name, service] : services_) {
            (void)name;
            total += service->server_edit_count();
        }
        return total;
    }

    // ---- client -----------------------------------------------------------

    bool client_bind(const std::string& worldName,
                     std::string& errorOut) override {
        if (!manager_.has_world(worldName)) {
            errorOut = "replication: unknown world '" + worldName + "'";
            return false;
        }
        voxel::IVoxelReplication* service = service_for(worldName);
        if (service == nullptr) {
            errorOut = "replication: world '" + worldName +
                       "' has no entity layer";
            return false;
        }
        clientWorld_ = worldName;
        errorOut.clear();
        return true;
    }

    std::string client_world() const override { return clientWorld_; }

    bool client_predict(int x, int y, int z, std::uint32_t blockId) override {
        voxel::IVoxelReplication* service = client_service();
        return service != nullptr && service->client_predict(x, y, z, blockId);
    }

    void client_apply_batch(const voxel::ReplicationBatch& batch) override {
        voxel::IVoxelReplication* service = client_service();
        if (service != nullptr) service->client_apply_batch(batch);
    }

    void client_apply_snapshot(
        const voxel::ChunkReplicationSnapshot& snapshot) override {
        voxel::IVoxelReplication* service = client_service();
        if (service != nullptr) service->client_apply_snapshot(snapshot);
    }

    bool client_apply_region(const voxel::RegionReplicationSnapshot& region,
                             std::string& errorOut) override {
        voxel::IVoxelReplication* service = client_service();
        if (service == nullptr) {
            errorOut = "replication: no world bound on the client";
            return false;
        }
        return service->client_apply_region(region, errorOut);
    }

    std::uint32_t client_applied_sequence() const override {
        voxel::IVoxelReplication* service = client_service();
        return service != nullptr ? service->client_applied_sequence() : 0;
    }

    std::size_t client_pending_predictions() const override {
        voxel::IVoxelReplication* service = client_service();
        return service != nullptr ? service->client_pending_predictions() : 0;
    }

    std::size_t client_stale_dropped() const override {
        voxel::IVoxelReplication* service = client_service();
        return service != nullptr ? service->client_stale_dropped() : 0;
    }

private:
    // Lazily creates the per-world replication service (one per world). The
    // service is bound to the manager's live world instance; if the world is
    // unloaded/reloaded later the manager replaces the instance and the old
    // service's views stay orphaned but harmless (same contract as the
    // per-world adapter's re-registration).
    voxel::IVoxelReplication* service_for(const std::string& worldName) {
        const auto it = services_.find(worldName);
        if (it != services_.end()) return it->second.get();
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) return nullptr;
        auto service = voxel::create_voxel_replication(*world);
        if (!service) return nullptr;
        // Apply the configured (sticky) snapshot window to the fresh service.
        service->server_set_snapshot_window(snapshotMinY_, snapshotHeight_);
        voxel::IVoxelReplication* raw = service.get();
        services_[worldName] = std::move(service);
        return raw;
    }

    voxel::IVoxelReplication* client_service() const {
        if (clientWorld_.empty()) return nullptr;
        const auto it = services_.find(clientWorld_);
        return it == services_.end() ? nullptr : it->second.get();
    }

    IWorldManager& manager_;
    // Connection -> its current per-world interest.
    std::unordered_map<voxel::ReplicationConnectionId,
                       WorldReplicationInterest>
        connections_;
    // Per-world replication services (lazy). std::map for deterministic order
    // in server_update / server_edit_count iteration.
    std::map<std::string, std::shared_ptr<voxel::IVoxelReplication>> services_;
    std::string clientWorld_;
    int snapshotMinY_{ voxel::kReplicationDefaultMinY };
    int snapshotHeight_{ voxel::kReplicationDefaultHeight };
};

}  // namespace

std::unique_ptr<IWorldReplication> create_world_replication(
    IWorldManager& manager) {
    return std::make_unique<WorldReplicationImpl>(manager);
}

}  // namespace world
}  // namespace engine

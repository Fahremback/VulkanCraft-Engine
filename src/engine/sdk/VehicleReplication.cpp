// VehicleReplication.cpp — the ONLY TU implementing the public vehicle
// replication contract (FALTANTES §17 item 11): server authority + client
// prediction. The server owns the registered IVehicle instances (steps them
// with the last client input and advances the world); the client registers its
// LOCAL predicted copy, steps it with the same input, applies the
// authoritative state the wire delivers, and reconciles (snap + damage sync).
// The codec (encode/decode VehicleReplicationState) makes the snapshot
// wire-ready — bit-exact little-endian, versioned, refuses non-finite values.
//
// The adapter uses ONLY public contracts (IGameplayRuntime + IVehicle +
// IVehicleDamage) — no internal layers, no networking headers.

#include "engine/vehicles/IVehicleReplication.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace vehicles {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool finite_quat(const glm::quat& q) {
    return finite_float(q.x) && finite_float(q.y) && finite_float(q.z) &&
           finite_float(q.w);
}

struct ServerVehicle {
    gameplay::IVehicle* vehicle{ nullptr };
    gameplay::VehicleInput lastInput{};
};

struct ClientVehicle {
    gameplay::IVehicle* predicted{ nullptr };
    gameplay::VehicleInput lastInput{};
    std::optional<VehicleReplicationState> authoritative;
};

class VehicleReplicationImpl final : public IVehicleReplication {
public:
    explicit VehicleReplicationImpl(gameplay::IGameplayRuntime& runtime)
        : runtime_(runtime) {}

    // ---- server (authority) ----

    bool server_register(const std::string& id, gameplay::IVehicle& vehicle,
                         std::string& errorOut) override {
        if (serverVehicles_.find(id) != serverVehicles_.end()) {
            errorOut = "vehicle replication: duplicate server id '" + id + "'";
            return false;
        }
        ServerVehicle entry;
        entry.vehicle = &vehicle;
        serverVehicles_.emplace(id, std::move(entry));
        return true;
    }

    bool server_unregister(const std::string& id) override {
        return serverVehicles_.erase(id) > 0;
    }

    void server_submit_input(const std::string& id,
                             const gameplay::VehicleInput& input) override {
        const auto found = serverVehicles_.find(id);
        if (found == serverVehicles_.end()) return;
        found->second.lastInput = clamp_input(input);
    }

    void server_tick(float deltaTime) override {
        if (!(deltaTime > 0.0f)) return;
        // Deterministic order (std::map iterates by key).
        for (auto& [id, entry] : serverVehicles_) {
            (void)id;
            entry.vehicle->set_input(entry.lastInput);
            entry.vehicle->update(deltaTime);
        }
        runtime_.step(deltaTime);
        ++tick_;
    }

    std::uint64_t server_tick_count() const override { return tick_; }

    bool server_snapshot(const std::string& id, VehicleReplicationState& out,
                         std::string& errorOut) const override {
        const auto found = serverVehicles_.find(id);
        if (found == serverVehicles_.end()) {
            errorOut = "vehicle replication: unknown server vehicle '" + id + "'";
            return false;
        }
        return snapshot_of(*found->second.vehicle, out, tick_);
    }

    // ---- client (prediction) ----

    bool client_submit_input(const std::string& id,
                             const gameplay::VehicleInput& input,
                             std::string& errorOut) override {
        const auto found = clients_.find(id);
        if (found == clients_.end()) {
            errorOut = "vehicle replication: unknown client vehicle '" + id + "'";
            return false;
        }
        found->second.lastInput = clamp_input(input);
        return true;
    }

    bool client_register_prediction(const std::string& id,
                                    gameplay::IVehicle& vehicle,
                                    std::string& errorOut) override {
        if (clients_.find(id) != clients_.end()) {
            errorOut = "vehicle replication: duplicate client id '" + id + "'";
            return false;
        }
        ClientVehicle entry;
        entry.predicted = &vehicle;
        clients_.emplace(id, std::move(entry));
        return true;
    }

    void client_predict(float deltaTime) override {
        if (!(deltaTime > 0.0f)) return;
        for (auto& [id, entry] : clients_) {
            (void)id;
            entry.predicted->set_input(entry.lastInput);
            entry.predicted->update(deltaTime);
        }
        runtime_.step(deltaTime);
        ++clientTick_;
    }

    bool client_apply_state(const std::string& id,
                            const VehicleReplicationState& state,
                            std::string& errorOut) override {
        const auto found = clients_.find(id);
        if (found == clients_.end()) {
            errorOut = "vehicle replication: unknown client vehicle '" + id + "'";
            return false;
        }
        found->second.authoritative = state;
        return true;
    }

    bool client_state(const std::string& id,
                      VehicleReplicationState& out) const override {
        const auto found = clients_.find(id);
        if (found == clients_.end() || !found->second.authoritative) return false;
        out = *found->second.authoritative;
        return true;
    }

    bool client_predicted(const std::string& id,
                          VehicleReplicationState& out) const override {
        const auto found = clients_.find(id);
        if (found == clients_.end()) return false;
        return snapshot_of(*found->second.predicted, out, clientTick_);
    }

    bool client_reconcile(const std::string& id,
                          std::string& errorOut) override {
        const auto found = clients_.find(id);
        if (found == clients_.end()) {
            errorOut = "vehicle replication: unknown client vehicle '" + id + "'";
            return false;
        }
        ClientVehicle& client = found->second;
        if (!client.authoritative) {
            errorOut = "vehicle replication: no authoritative state applied";
            return false;
        }
        const VehicleReplicationState& auth = *client.authoritative;
        // Snap the predicted chassis to the authoritative pose AND momentum —
        // without the velocity snap the corrected prediction would coast on
        // the stale prediction's momentum.
        runtime_.physics().set_transform(client.predicted->chassis(),
                                         auth.position, auth.rotation);
        runtime_.physics().set_velocity(client.predicted->chassis(),
                                        auth.linearVelocity, auth.angularVelocity);
        // Copy the authoritative part healths (damage the client did not
        // predict propagates through the damage/repair deltas).
        const std::size_t count =
            std::min<std::size_t>(client.predicted->part_count(),
                                  auth.partHealth.size());
        for (std::size_t i = 0; i < count; ++i) {
            const float target = auth.partHealth[i];
            const float current = client.predicted->part_info(i).health;
            std::string error;
            if (target > current) {
                client.predicted->repair(i, target - current, error);
            } else if (target < current) {
                client.predicted->apply_damage(i, current - target, error);
            }
        }
        return true;
    }

private:
    static gameplay::VehicleInput clamp_input(const gameplay::VehicleInput& input) {
        gameplay::VehicleInput out;
        out.throttle = glm::clamp(input.throttle, -1.0f, 1.0f);
        out.steering = glm::clamp(input.steering, -1.0f, 1.0f);
        out.brake = glm::clamp(input.brake, 0.0f, 1.0f);
        out.handbrake = glm::clamp(input.handbrake, 0.0f, 1.0f);
        return out;
    }

    // Builds a replication state from a vehicle's current world state.
    bool snapshot_of(gameplay::IVehicle& vehicle,
                     VehicleReplicationState& out,
                     std::uint64_t tick) const {
        gameplay::BodyState body;
        if (!runtime_.physics().body_state(vehicle.chassis(), body)) return false;
        out.tick = tick;
        out.position = body.position;
        out.rotation = body.rotation;
        out.linearVelocity = body.linearVelocity;
        out.angularVelocity = body.angularVelocity;
        out.partHealth.clear();
        out.partHealth.reserve(vehicle.part_count());
        for (std::size_t i = 0; i < vehicle.part_count(); ++i) {
            out.partHealth.push_back(vehicle.part_info(i).health);
        }
        return true;
    }

    gameplay::IGameplayRuntime& runtime_;
    std::map<std::string, ServerVehicle> serverVehicles_;
    std::unordered_map<std::string, ClientVehicle> clients_;
    std::uint64_t tick_{ 0 };
    std::uint64_t clientTick_{ 0 };
};

// ---- codec ---------------------------------------------------------------

template <typename T>
void append_le(std::vector<std::byte>& out, const T& value) {
    const std::byte* bytes = reinterpret_cast<const std::byte*>(&value);
    const std::byte* end = bytes + sizeof(T);
    // Little-endian frames (the platform is x86_64); the project's other
    // replication codecs assume the same layout.
    for (const std::byte* it = bytes; it != end; ++it) out.push_back(*it);
}

bool read_le(const std::vector<std::byte>& data, std::size_t& offset,
             void* out, std::size_t size) {
    if (offset + size > data.size()) return false;
    std::memcpy(out, data.data() + offset, size);
    offset += size;
    return true;
}

}  // namespace

std::vector<std::byte> encode_vehicle_state(const VehicleReplicationState& state) {
    std::vector<std::byte> out;
    out.reserve(64 + state.partHealth.size() * sizeof(float));
    const std::uint32_t version = 1;
    append_le(out, version);
    append_le(out, state.tick);
    append_le(out, state.position.x);
    append_le(out, state.position.y);
    append_le(out, state.position.z);
    append_le(out, state.rotation.x);
    append_le(out, state.rotation.y);
    append_le(out, state.rotation.z);
    append_le(out, state.rotation.w);
    append_le(out, state.linearVelocity.x);
    append_le(out, state.linearVelocity.y);
    append_le(out, state.linearVelocity.z);
    append_le(out, state.angularVelocity.x);
    append_le(out, state.angularVelocity.y);
    append_le(out, state.angularVelocity.z);
    const std::uint32_t count =
        static_cast<std::uint32_t>(state.partHealth.size());
    append_le(out, count);
    for (const float health : state.partHealth) append_le(out, health);
    return out;
}

bool decode_vehicle_state(const std::vector<std::byte>& data,
                          VehicleReplicationState& out) {
    std::size_t offset = 0;
    std::uint32_t version = 0;
    if (!read_le(data, offset, &version, sizeof(version)) || version != 1)
        return false;
    if (!read_le(data, offset, &out.tick, sizeof(out.tick))) return false;
    if (!read_le(data, offset, &out.position.x, sizeof(float)) ||
        !read_le(data, offset, &out.position.y, sizeof(float)) ||
        !read_le(data, offset, &out.position.z, sizeof(float)) ||
        !read_le(data, offset, &out.rotation.x, sizeof(float)) ||
        !read_le(data, offset, &out.rotation.y, sizeof(float)) ||
        !read_le(data, offset, &out.rotation.z, sizeof(float)) ||
        !read_le(data, offset, &out.rotation.w, sizeof(float)) ||
        !read_le(data, offset, &out.linearVelocity.x, sizeof(float)) ||
        !read_le(data, offset, &out.linearVelocity.y, sizeof(float)) ||
        !read_le(data, offset, &out.linearVelocity.z, sizeof(float)) ||
        !read_le(data, offset, &out.angularVelocity.x, sizeof(float)) ||
        !read_le(data, offset, &out.angularVelocity.y, sizeof(float)) ||
        !read_le(data, offset, &out.angularVelocity.z, sizeof(float))) {
        return false;
    }
    if (!finite_float(out.position.x) || !finite_float(out.position.y) ||
        !finite_float(out.position.z) || !finite_quat(out.rotation) ||
        !finite_float(out.linearVelocity.x) || !finite_float(out.linearVelocity.y) ||
        !finite_float(out.linearVelocity.z) || !finite_float(out.angularVelocity.x) ||
        !finite_float(out.angularVelocity.y) || !finite_float(out.angularVelocity.z)) {
        return false;
    }
    std::uint32_t count = 0;
    if (!read_le(data, offset, &count, sizeof(count))) return false;
    if (count > 4096) return false;
    out.partHealth.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!read_le(data, offset, &out.partHealth[i], sizeof(float))) return false;
        if (!finite_float(out.partHealth[i]) || out.partHealth[i] < 0.0f)
            return false;
    }
    return true;
}

std::shared_ptr<IVehicleReplication> create_vehicle_replication(
    gameplay::IGameplayRuntime& runtime) {
    return std::make_shared<VehicleReplicationImpl>(runtime);
}

}  // namespace vehicles
}  // namespace engine

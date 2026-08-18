#pragma once

// Vehicle replication (FALTANTES §17 item 11): server authority + client
// prediction for vehicles.
//
// The SERVER owns the authoritative simulation: the project registers its
// IVehicle instances (server_register), clients send input (server_submit_input
// — the wire delivers it), and server_tick steps every owned vehicle with the
// last input (idle when none) and advances the world. The authoritative state
// of a vehicle is exposed as a VehicleReplicationState (chassis transform +
// velocities + per-part healths + tick) — the damage/parts of §17 item 9 ride
// the same snapshot, so damage applied on the server propagates to clients.
//
// The CLIENT predicts: it creates its own LOCAL copy of the vehicle (from the
// same asset, in the client world) and registers it as the predicted vehicle;
// client_predict steps it with the same input the client submits to the
// server, so the predicted state is available immediately with no round trip.
// When the authoritative state arrives (the wire delivers it to
// client_apply_state), the client reconciles: the predicted copy snaps to the
// authoritative transform and copies the authoritative part healths
// (client_reconcile) — error-driven correction, not rollback.
//
// The contract is TRANSPORT-FREE (same as IVoxelReplication): server/client
// methods are plain in-process calls; the codec (encode/decode
// VehicleReplicationState) makes the snapshot wire-ready — the same flow the
// voxel replication uses (encode -> transport -> decode -> apply). One service
// is bound to ONE runtime (the role's physics world): a host runs a server
// service on the server world and a client service on the client world.
//
// Self-contained: public SDK headers + glm only. The only implementation is
// src/engine/sdk/VehicleReplication.cpp (the adapter behind the factory).

#include "engine/gameplay/IGameplayRuntime.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace vehicles {

// The authoritative snapshot of one vehicle: chassis state + per-part health
// (damage authority, §17 item 9) + the server tick that produced it.
struct VehicleReplicationState {
    std::uint64_t tick{ 0 };
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 linearVelocity{ 0.0f };
    glm::vec3 angularVelocity{ 0.0f };
    // One entry per part (the server's auto-derived parts, order preserved).
    std::vector<float> partHealth;
};

// Server authority + client prediction service. One instance is bound to one
// IGameplayRuntime (the role's physics world) and plays one role (server
// methods for the authority, client methods for a remote/local client); a host
// runs both on their own worlds.
class IVehicleReplication {
public:
    virtual ~IVehicleReplication() = default;

    // ---- server (authority) ----
    // Registers a vehicle the server owns (registered by id). False with a
    // diagnostic for a duplicate id.
    virtual bool server_register(const std::string& id, gameplay::IVehicle& vehicle,
                                 std::string& errorOut) = 0;
    virtual bool server_unregister(const std::string& id) = 0;
    // The wire delivers a client's input for a vehicle (last one wins;
    // unknown ids are ignored). Empty input = idle.
    virtual void server_submit_input(const std::string& id,
                                     const gameplay::VehicleInput& input) = 0;
    // Advances the authoritative simulation: every registered vehicle is
    // stepped with its last client input (idle when none) and the world steps
    // once. The authoritative tick advances; server_snapshot reflects it.
    virtual void server_tick(float deltaTime) = 0;
    virtual std::uint64_t server_tick_count() const = 0;
    virtual bool server_snapshot(const std::string& id,
                                 VehicleReplicationState& out,
                                 std::string& errorOut) const = 0;

    // ---- client (prediction) ----
    // The client's input for a vehicle: used for prediction AND the input the
    // host forwards to the server over the wire. False for an unknown vehicle.
    virtual bool client_submit_input(const std::string& id,
                                     const gameplay::VehicleInput& input,
                                     std::string& errorOut) = 0;
    // The client's LOCAL predicted copy of a vehicle (created by the project
    // in the client world from the same asset). One predicted vehicle per id.
    virtual bool client_register_prediction(const std::string& id,
                                            gameplay::IVehicle& vehicle,
                                            std::string& errorOut) = 0;
    // Steps every registered predicted vehicle with its last submitted input
    // and advances the client world (the prediction — no round trip).
    virtual void client_predict(float deltaTime) = 0;
    // What the wire delivers: the client records the authoritative state.
    // False (with a diagnostic) for an id with no registered prediction.
    virtual bool client_apply_state(const std::string& id,
                                    const VehicleReplicationState& state,
                                    std::string& errorOut) = 0;
    virtual bool client_state(const std::string& id,
                              VehicleReplicationState& out) const = 0;
    virtual bool client_predicted(const std::string& id,
                                  VehicleReplicationState& out) const = 0;
    // Error-driven correction: the predicted vehicle snaps to the latest
    // authoritative state — the chassis transform is set to the authoritative
    // pose and the part healths are copied (damage the client did not predict
    // propagates). The prediction continues from the corrected state.
    virtual bool client_reconcile(const std::string& id,
                                  std::string& errorOut) = 0;
};

// ---- Codec (wire-ready, mirrors the voxel replication codec) ----
// Little-endian frame, versioned; decode fails on malformed/short frames and
// refuses non-finite values. Round-trips bit-exactly (float memcpy).
std::vector<std::byte> encode_vehicle_state(const VehicleReplicationState& state);

bool decode_vehicle_state(const std::vector<std::byte>& data,
                          VehicleReplicationState& out);

// Factory: builds the adapter bound to `runtime` (the implementation lives in
// the engine SDK module). One service plays one role; a host creates one
// service per world.
std::shared_ptr<IVehicleReplication> create_vehicle_replication(
    gameplay::IGameplayRuntime& runtime);

}  // namespace vehicles
}  // namespace engine

// VehicleReplicationTests.cpp
//
// FALTANTES §17 item 11: "Implementar autoridade e prediction de rede" for
// vehicles. The server owns the authoritative simulation (steps registered
// vehicles with the last client input, exposes authoritative snapshots); the
// client predicts with its own LOCAL copy of the vehicle (same asset, same
// world config) and reconciles against the authoritative state the wire
// delivers. These tests prove: the codec round-trips bit-exact and refuses
// malformed frames, server authority (the server moves under client input and
// the snapshot reflects it), client prediction (moves with NO round trip),
// authority wins (the applied server state is the truth), reconcile (snap to
// the authoritative pose + damage the client did not predict propagates and
// the corrected prediction stalls like the server), validation, and
// determinism.

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"
#include "engine/vehicles/IVehicleReplication.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

bool same_vec3(const glm::vec3& a, const glm::vec3& b) {
    return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0;
}

bool same_quat(const glm::quat& a, const glm::quat& b) {
    return std::memcmp(&a, &b, sizeof(glm::quat)) == 0;
}

engine::vehicles::VehicleAsset make_car_asset() {
    using namespace engine::vehicles;
    VehicleAsset asset;
    asset.name = "replication-car";
    asset.position = {0.0f, 1.2f, 0.0f};
    asset.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    asset.chassis.shape = ChassisShape::Box;
    asset.chassis.halfExtents = {0.95f, 0.37f, 0.61f};
    asset.chassis.mass = 1200.0f;
    const glm::vec3 locals[4] = {
        {-1.3f, -0.1f, -0.8f}, {-1.3f, -0.1f, 0.8f},
        {1.3f, -0.1f, -0.8f}, {1.3f, -0.1f, 0.8f},
    };
    for (const glm::vec3& local : locals) {
        WheelComponent wheel;
        wheel.localPosition = local;
        wheel.driven = true;
        wheel.steering = local.z < 0.0f;
        asset.wheels.push_back(wheel);
    }
    return asset;
}

void add_ground(engine::gameplay::IGameplayRuntime& runtime) {
    using namespace engine::gameplay;
    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    ground.position = {0.0f, -0.5f, 0.0f};
    runtime.physics().create_body(ground);
}

// --- 1. Codec ---------------------------------------------------------------

void test_codec_round_trip() {
    engine::vehicles::VehicleReplicationState state;
    state.tick = 12345;
    state.position = {1.5f, 2.25f, -3.75f};
    state.rotation = glm::normalize(glm::quat(0.6f, 0.0f, 0.8f, 0.0f));
    state.linearVelocity = {0.25f, -0.5f, 7.75f};
    state.angularVelocity = {0.1f, 0.2f, -0.3f};
    state.partHealth = {100.0f, 62.5f, 0.0f, 33.25f, 100.0f, 99.9f};

    const std::vector<std::byte> data = encode_vehicle_state(state);
    engine::vehicles::VehicleReplicationState parsed;
    check(decode_vehicle_state(data, parsed), "codec: decode succeeds");
    check(parsed.tick == state.tick, "codec: tick round-trips");
    check(same_vec3(parsed.position, state.position), "codec: position bit-exact");
    check(same_quat(parsed.rotation, state.rotation), "codec: rotation bit-exact");
    check(same_vec3(parsed.linearVelocity, state.linearVelocity),
          "codec: linearVelocity bit-exact");
    check(same_vec3(parsed.angularVelocity, state.angularVelocity),
          "codec: angularVelocity bit-exact");
    check(parsed.partHealth.size() == state.partHealth.size(),
          "codec: part health count round-trips");
    bool healthsExact = parsed.partHealth.size() == state.partHealth.size();
    for (std::size_t i = 0; i < state.partHealth.size() && healthsExact; ++i) {
        healthsExact = std::memcmp(&parsed.partHealth[i], &state.partHealth[i],
                                   sizeof(float)) == 0;
    }
    check(healthsExact, "codec: part healths bit-exact");
    std::printf("[vehicle-replication] codec round-trip bit-exact OK\n");
}

void test_codec_rejects_malformed() {
    engine::vehicles::VehicleReplicationState state;
    state.tick = 7;
    state.position = {1.0f, 2.0f, 3.0f};
    state.partHealth = {100.0f};

    const std::vector<std::byte> valid = encode_vehicle_state(state);
    check(!decode_vehicle_state({}, state), "codec: empty frame refused");
    std::vector<std::byte> truncated(valid.begin(), valid.end() - 3);
    check(!decode_vehicle_state(truncated, state),
          "codec: truncated frame refused");

    std::vector<std::byte> badVersion = valid;
    badVersion[0] = static_cast<std::byte>(2);
    check(!decode_vehicle_state(badVersion, state),
          "codec: wrong version refused");

    engine::vehicles::VehicleReplicationState nanState;
    nanState.position = {std::nanf(""), 0.0f, 0.0f};
    const std::vector<std::byte> nanBytes = encode_vehicle_state(nanState);
    check(!decode_vehicle_state(nanBytes, state),
          "codec: non-finite position refused");

    engine::vehicles::VehicleReplicationState negState;
    negState.partHealth = {-1.0f};
    const std::vector<std::byte> negBytes = encode_vehicle_state(negState);
    check(!decode_vehicle_state(negBytes, state),
          "codec: negative health refused");
    std::printf("[vehicle-replication] codec malformed frames refused OK\n");
}

// --- 2. Server authority -----------------------------------------------------

void test_server_authority() {
    using namespace engine::gameplay;
    using namespace engine::vehicles;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr && vehicle->valid(), "server vehicle assembled");

    auto replication = create_vehicle_replication(*runtime);
    std::string error;
    check(replication->server_register("car", *vehicle, error),
          "server registers the vehicle");
    check(!replication->server_register("car", *vehicle, error) && !error.empty(),
          "duplicate server id refused");

    // Unknown input ids are ignored (no crash, no effect).
    VehicleInput throttle;
    throttle.throttle = 1.0f;
    replication->server_submit_input("nope", throttle);

    // Authority: the server steps the vehicle with the client input and the
    // snapshot reflects the motion.
    const float dt = 1.0f / 60.0f;
    replication->server_submit_input("car", throttle);
    for (int i = 0; i < 90; ++i) replication->server_tick(dt);
    check(replication->server_tick_count() == 90,
          "server tick advanced exactly 90");

    engine::vehicles::VehicleReplicationState state;
    check(replication->server_snapshot("car", state, error),
          "server snapshot succeeds");
    check(state.tick == 90, "snapshot carries the authoritative tick");
    check(state.position.z < -0.5f,
          "authoritative state moved forward under client input");
    check(state.partHealth.size() == vehicle->part_count(),
          "snapshot carries the server part healths");
    std::printf("[vehicle-replication] server authority: tick %llu, moved "
                "%.2f m OK\n",
                (unsigned long long)state.tick, -state.position.z);
}

// --- 3. Client prediction (no round trip) ------------------------------------

void test_client_prediction() {
    using namespace engine::gameplay;
    using namespace engine::vehicles;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr && vehicle->valid(), "client vehicle assembled");

    auto replication = create_vehicle_replication(*runtime);
    std::string error;
    check(replication->client_register_prediction("car", *vehicle, error),
          "client registers its predicted copy");
    check(!replication->client_register_prediction("car", *vehicle, error) &&
              !error.empty(),
          "duplicate client id refused");

    VehicleInput throttle;
    throttle.throttle = 1.0f;
    check(replication->client_submit_input("car", throttle, error),
          "client submits input");
    check(!replication->client_submit_input("nope", throttle, error) &&
              !error.empty(),
          "unknown client id refused");

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 90; ++i) replication->client_predict(dt);

    engine::vehicles::VehicleReplicationState predicted;
    check(replication->client_predicted("car", predicted),
          "predicted state readable");
    check(predicted.position.z < -0.5f,
          "prediction moved the local copy (no round trip)");
    std::printf("[vehicle-replication] client prediction: moved %.2f m with "
                "no server round trip OK\n", -predicted.position.z);
}

// --- 4. Authority wins + reconcile (unpredicted damage propagates) -----------

void test_reconcile() {
    using namespace engine::gameplay;
    using namespace engine::vehicles;
    engine::vehicles::VehicleAsset asset = make_car_asset();

    // Server world + authoritative vehicle.
    auto serverRuntime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*serverRuntime);
    auto serverVehicle = serverRuntime->create_vehicle_from_asset(asset);
    auto serverReplication = create_vehicle_replication(*serverRuntime);
    std::string error;
    check(serverReplication->server_register("car", *serverVehicle, error),
          "server registered");

    // Client world + predicted copy (same asset -> same start).
    auto clientRuntime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*clientRuntime);
    auto clientVehicle = clientRuntime->create_vehicle_from_asset(asset);
    auto clientReplication = create_vehicle_replication(*clientRuntime);
    check(clientReplication->client_register_prediction("car", *clientVehicle,
                                                        error),
          "client registered its prediction");

    // Both step with the same throttle (prediction == authority while the
    // world agrees).
    const float dt = 1.0f / 60.0f;
    VehicleInput throttle;
    throttle.throttle = 1.0f;
    for (int i = 0; i < 60; ++i) {
        clientReplication->client_submit_input("car", throttle, error);
        clientReplication->client_predict(dt);
        serverReplication->server_submit_input("car", throttle);
        serverReplication->server_tick(dt);
    }

    // The prediction and the authority agree (shared deterministic sim).
    engine::vehicles::VehicleReplicationState predicted, authoritative;
    check(clientReplication->client_predicted("car", predicted),
          "predicted readable");
    check(serverReplication->server_snapshot("car", authoritative, error),
          "authoritative readable");
    check(std::abs(predicted.position.z - authoritative.position.z) < 0.1f,
          "prediction matches the authority while the world agrees");

    // The server applies an UNPREDICTED event: the drivetrain is destroyed.
    check(serverVehicle->apply_damage(1, 100.0f, error),
          "server destroys the drivetrain (unpredicted)");

    // The client keeps predicting with throttle — its copy still drives.
    for (int i = 0; i < 30; ++i) {
        clientReplication->client_submit_input("car", throttle, error);
        clientReplication->client_predict(dt);
    }
    engine::vehicles::VehicleReplicationState stillDriving;
    check(clientReplication->client_predicted("car", stillDriving),
          "predicted (stale) readable");
    check(stillDriving.position.z < authoritative.position.z - 0.5f,
          "the stale prediction kept moving (the damage was not predicted)");

    // The wire delivers the authoritative state; the client reconciles.
    engine::vehicles::VehicleReplicationState fresh;
    check(serverReplication->server_snapshot("car", fresh, error),
          "post-damage snapshot");
    check(clientReplication->client_apply_state("car", fresh, error),
          "client applies the authoritative state");
    check(clientReplication->client_reconcile("car", error),
          "client reconciles");

    // The predicted copy snapped to the authoritative pose.
    BodyState chassisState;
    check(clientRuntime->physics().body_state(clientVehicle->chassis(),
                                              chassisState),
          "predicted chassis state readable");
    check(glm::length(chassisState.position - fresh.position) < 0.001f,
          "reconcile snapped the predicted chassis to the authoritative pose");
    // The unpredicted damage propagated (drivetrain health synced).
    const VehiclePartInfo drivetrain = clientVehicle->part_info(1);
    check(drivetrain.health == 0.0f && drivetrain.separated,
          "reconcile propagated the server-side damage (drivetrain dead)");

    // Prediction continues from the corrected state with the SAME input both
    // sides: the authoritative vehicle (drivetrain dead, coasting on its
    // momentum) and the corrected prediction must stay in lockstep — the
    // corrected copy no longer runs at the stale full-throttle speed.
    for (int i = 0; i < 30; ++i) {
        clientReplication->client_submit_input("car", throttle, error);
        clientReplication->client_predict(dt);
        serverReplication->server_submit_input("car", throttle);
        serverReplication->server_tick(dt);
    }
    engine::vehicles::VehicleReplicationState corrected, serverNow;
    check(clientReplication->client_predicted("car", corrected),
          "corrected prediction readable");
    check(serverReplication->server_snapshot("car", serverNow, error),
          "authority after the same ticks");
    check(std::abs(corrected.position.z - serverNow.position.z) < 0.05f,
          "the corrected prediction stays in lockstep with the authority");
    check(corrected.position.z > stillDriving.position.z - 0.1f,
          "the corrected copy no longer runs the stale full-throttle speed");
    std::printf("[vehicle-replication] reconcile: prediction matched %.3f, "
                "stale ran %.2f beyond, corrected tracks authority %.3f OK\n",
                std::abs(predicted.position.z - authoritative.position.z),
                stillDriving.position.z - authoritative.position.z,
                std::abs(corrected.position.z - serverNow.position.z));
}

// --- 5. Damage authority via the codec ---------------------------------------

void test_damage_propagates_via_codec() {
    using namespace engine::gameplay;
    using namespace engine::vehicles;
    engine::vehicles::VehicleAsset asset = make_car_asset();

    auto serverRuntime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*serverRuntime);
    auto serverVehicle = serverRuntime->create_vehicle_from_asset(asset);
    auto serverReplication = create_vehicle_replication(*serverRuntime);
    std::string error;
    serverReplication->server_register("car", *serverVehicle, error);

    // Server-side wheel damage (2 = wheel 0 down to 25%).
    check(serverVehicle->apply_damage(2, 75.0f, error), "server damages wheel 0");

    // Wire round trip: snapshot -> encode -> decode -> client apply -> reconcile.
    engine::vehicles::VehicleReplicationState snapshot;
    check(serverReplication->server_snapshot("car", snapshot, error),
          "snapshot with damage");
    const std::vector<std::byte> bytes = encode_vehicle_state(snapshot);
    engine::vehicles::VehicleReplicationState decoded;
    check(decode_vehicle_state(bytes, decoded), "decode the damaged state");

    auto clientRuntime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*clientRuntime);
    auto clientVehicle = clientRuntime->create_vehicle_from_asset(asset);
    auto clientReplication = create_vehicle_replication(*clientRuntime);
    check(clientReplication->client_register_prediction("car", *clientVehicle,
                                                        error),
          "client predicted copy");
    check(clientReplication->client_apply_state("car", decoded, error),
          "client applies the decoded state");
    check(clientReplication->client_reconcile("car", error), "client reconciles");

    check(clientVehicle->part_info(2).health == 25.0f,
          "wheel damage propagated through the wire codec + reconcile");
    check(clientVehicle->part_info(0).health == 100.0f,
          "undamaged parts untouched");
    std::printf("[vehicle-replication] damage authority via codec: wheel 0 "
                "health %.1f OK\n", clientVehicle->part_info(2).health);
}

// --- 6. Validation -----------------------------------------------------------

void test_validation() {
    using namespace engine::gameplay;
    using namespace engine::vehicles;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    auto replication = create_vehicle_replication(*runtime);

    std::string error;
    engine::vehicles::VehicleReplicationState state;
    check(!replication->server_snapshot("missing", state, error) &&
              !error.empty(),
          "unknown server vehicle snapshot refused");
    check(!replication->client_predicted("missing", state),
          "unknown client prediction refused");
    check(!replication->client_apply_state("missing", state, error) &&
              !error.empty(),
          "unknown client apply refused");
    check(!replication->client_reconcile("missing", error) && !error.empty(),
          "unknown client reconcile refused");
    std::printf("[vehicle-replication] validation OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_codec_round_trip();
    test_codec_rejects_malformed();
    test_server_authority();
    test_client_prediction();
    test_reconcile();
    test_damage_propagates_via_codec();
    test_validation();
    if (g_failures == 0) {
        std::printf("[vehicle-replication] ALL PASSED\n");
        return 0;
    }
    std::printf("[vehicle-replication] %d FAILURE(S)\n", g_failures);
    return 1;
}

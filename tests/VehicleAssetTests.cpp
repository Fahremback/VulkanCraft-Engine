// VehicleAssetTests.cpp
//
// FALTANTES §17 item 2: "Transformar o runtime atual em VehicleAsset e
// componentes públicos combináveis". The vehicle runtime is assembled from
// COMPOSABLE PUBLIC COMPONENTS — chassis, wheels, drivetrain — serialized as a
// versioned data-driven VehicleAsset (JSON, all-or-nothing, bit-exact
// round-trip) and mounted through IGameplayRuntime::create_vehicle_from_asset.
// These tests prove the PUBLIC contract end-to-end on the public runtime:
// JSON round-trip stability, validation, real Jolt assembly + motion, the
// drivetrain actually driving the engine, determinism, and the builtin
// fallback.

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"

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

// A car asset with DISTINCTIVE values in every field — so a round-trip that
// drops or mangles anything fails loudly.
engine::vehicles::VehicleAsset make_car_asset() {
    using namespace engine::vehicles;
    VehicleAsset asset;
    asset.name = "scout";
    asset.position = {1.5f, 2.25f, -3.75f};
    asset.rotation = glm::normalize(glm::quat(0.6f, 0.0f, 0.8f, 0.0f));

    asset.chassis.shape = ChassisShape::Box;
    asset.chassis.halfExtents = {0.95f, 0.37f, 0.61f};
    asset.chassis.mass = 1350.0f;
    asset.chassis.friction = 0.6f;
    asset.chassis.restitution = 0.07f;

    const glm::vec3 locals[4] = {
        {-1.3f, -0.1f, -0.8f}, {-1.3f, -0.1f, 0.8f},
        {1.3f, -0.1f, -0.8f},  {1.3f, -0.1f, 0.8f},
    };
    asset.wheels.resize(4);
    for (int i = 0; i < 4; ++i) {
        asset.wheels[i].localPosition = locals[i];
        asset.wheels[i].radius = 0.39f;
        asset.wheels[i].suspensionRestLength = 0.48f;
        asset.wheels[i].suspensionTravel = 0.2f;
        asset.wheels[i].springStrength = 28000.0f;
        asset.wheels[i].damperStrength = 3400.0f;
        asset.wheels[i].tireGrip = 1.4f;
        asset.wheels[i].maxDriveForce = 4600.0f;
        asset.wheels[i].maxBrakeForce = 6400.0f;
        asset.wheels[i].maxSteerAngle = 0.6f;
        asset.wheels[i].steering = i < 2;
        asset.wheels[i].driven = true;
    }

    asset.drivetrain.engineMaxTorque = 0.0f;  // derived from the wheels
    asset.drivetrain.engineMinRPM = 1100.0f;
    asset.drivetrain.engineMaxRPM = 6500.0f;
    asset.drivetrain.differentialRatio = 3.6f;
    asset.drivetrain.gearRatios = {2.9f, 1.9f, 1.4f, 1.0f, 0.8f};
    return asset;
}

// to_json() emits every field with %.9g (exact for float32); loading that
// document back reproduces the asset bit-exactly. The id is derived from the
// name when absent (stable).
void test_json_round_trip() {
    using namespace engine::vehicles;
    VehicleAsset asset = make_car_asset();
    asset.id = "123e4567-e89b-12d3-a456-426614174000";  // canonical UUID
    const std::string json = asset.to_json();

    VehicleAsset loaded;
    std::string error;
    check(loaded.load_from_json(json, error), "load of emitted document succeeds");
    if (loaded.name != asset.name) { check(false, "name round-trips"); }
    check(loaded.id == asset.id, "id round-trips");
    check(loaded.version == asset.version, "version round-trips");
    check(same_vec3(loaded.position, asset.position), "position round-trips bit-exact");
    check(same_quat(loaded.rotation, asset.rotation), "rotation round-trips bit-exact");
    check(loaded.chassis.shape == asset.chassis.shape, "chassis shape round-trips");
    check(same_vec3(loaded.chassis.halfExtents, asset.chassis.halfExtents),
          "chassis halfExtents round-trips bit-exact");
    check(loaded.chassis.mass == asset.chassis.mass, "chassis mass round-trips");
    check(loaded.chassis.friction == asset.chassis.friction, "chassis friction round-trips");
    check(loaded.chassis.restitution == asset.chassis.restitution,
          "chassis restitution round-trips");
    check(loaded.wheels.size() == asset.wheels.size(), "wheel count round-trips");
    for (std::size_t i = 0; i < asset.wheels.size(); ++i) {
        const WheelComponent& a = asset.wheels[i];
        const WheelComponent& b = loaded.wheels[i];
        check(same_vec3(b.localPosition, a.localPosition), "wheel position round-trips");
        check(b.radius == a.radius, "wheel radius round-trips");
        check(b.suspensionRestLength == a.suspensionRestLength,
              "wheel rest length round-trips");
        check(b.suspensionTravel == a.suspensionTravel, "wheel travel round-trips");
        check(b.springStrength == a.springStrength, "wheel spring round-trips");
        check(b.damperStrength == a.damperStrength, "wheel damper round-trips");
        check(b.tireGrip == a.tireGrip, "wheel grip round-trips");
        check(b.maxDriveForce == a.maxDriveForce, "wheel drive force round-trips");
        check(b.maxBrakeForce == a.maxBrakeForce, "wheel brake force round-trips");
        check(b.maxSteerAngle == a.maxSteerAngle, "wheel steer angle round-trips");
        check(b.steering == a.steering, "wheel steering round-trips");
        check(b.driven == a.driven, "wheel driven round-trips");
    }
    check(loaded.drivetrain.engineMaxTorque == asset.drivetrain.engineMaxTorque,
          "engine torque round-trips");
    check(loaded.drivetrain.engineMinRPM == asset.drivetrain.engineMinRPM,
          "engine min RPM round-trips");
    check(loaded.drivetrain.engineMaxRPM == asset.drivetrain.engineMaxRPM,
          "engine max RPM round-trips");
    check(loaded.drivetrain.differentialRatio == asset.drivetrain.differentialRatio,
          "differential ratio round-trips");
    check(loaded.drivetrain.gearRatios.size() == asset.drivetrain.gearRatios.size(),
          "gear count round-trips");
    for (std::size_t i = 0; i < asset.drivetrain.gearRatios.size(); ++i) {
        check(loaded.drivetrain.gearRatios[i] == asset.drivetrain.gearRatios[i],
              "gear ratio round-trips");
    }

    // id derived from the name when absent — stable across loads.
    VehicleAsset noIdSource = make_car_asset();
    noIdSource.id.clear();
    const std::string jsonWithoutId = noIdSource.to_json();
    VehicleAsset withoutId;
    std::string idError;
    check(withoutId.load_from_json(jsonWithoutId, idError), "load without id succeeds");
    check(!withoutId.id.empty(), "id derived from name");
    check(withoutId.id != asset.id, "derived id differs from explicit id");
    std::printf("[vehicle-asset] JSON round-trip: bit-exact, id derived OK\n");
}

// Malformed documents are refused ALL-OR-NOTHING with a diagnostic; the target
// asset is left untouched.
void test_validation() {
    using namespace engine::vehicles;
    auto refuses = [](const std::string& json, const char* what) {
        VehicleAsset target = make_car_asset();
        std::string error;
        const bool accepted = target.load_from_json(json, error);
        check(!accepted, what);
        check(!error.empty(), "diagnostic provided");
        // All-or-nothing: a failed load must not clobber the target.
        check(target.name == "scout", "target untouched on failure");
    };
    refuses("not json", "non-JSON refused");
    refuses("[]", "non-object root refused");
    refuses(R"({"name":"x","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[],"drivetrain":{"gearRatios":[1]}})",
            "empty wheels refused");
    refuses(R"({"name":"x","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"tetrahedron"},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"gearRatios":[1]}})",
            "unknown chassis shape refused");
    refuses(R"({"name":"x","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box","mass":-5},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"gearRatios":[1]}})",
            "non-positive chassis mass refused");
    refuses(R"({"name":"x","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[{"localPosition":[0,0,0],"radius":0}],
             "drivetrain":{"gearRatios":[1]}})",
            "zero wheel radius refused");
    refuses(R"({"name":"x","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"engineMinRPM":8000,"engineMaxRPM":1000}})",
            "inverted RPM range refused");
    refuses(R"({"name":"x","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"gearRatios":[]}})",
            "empty gear ratios refused");
    refuses(R"({"name":"","position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"gearRatios":[1]}})",
            "empty name refused");
    refuses(R"({"name":"x","version":2,"position":[1,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"gearRatios":[1]}})",
            "unsupported version refused");
    refuses(R"({"name":"x","position":[1e400,2,3],"rotation":[0,0,0,1],
             "chassis":{"shape":"box"},"wheels":[{"localPosition":[0,0,0]}],
             "drivetrain":{"gearRatios":[1]}})",
            "non-finite position refused");
    std::printf("[vehicle-asset] validation: all-or-nothing OK\n");
}

// The asset assembles a REAL vehicle on the Jolt runtime: the chassis body is
// created at asset.position, the vehicle is valid, and full throttle produces
// real motion (the chassis actually moves forward, not an eager mirror).
void test_assembly_and_drive() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    check(runtime->physics_backend() == PhysicsBackend::Jolt,
          "runtime uses Jolt backend");

    // Ground so the vehicle can settle and drive.
    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    runtime->physics().create_body(ground);

    engine::vehicles::VehicleAsset asset = make_car_asset();
    asset.position = {0.0f, 1.2f, 0.0f};
    asset.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr, "create_vehicle_from_asset succeeds");
    check(vehicle->valid(), "assembled vehicle is valid");
    check(vehicle->chassis().valid(), "chassis body id is valid");

    BodyState chassisState;
    check(runtime->physics().body_state(vehicle->chassis(), chassisState),
          "chassis body state readable");
    check(std::abs(chassisState.position.y - 1.2f) < 0.001f,
          "chassis spawned at asset.position");

    const float dt = 1.0f / 60.0f;
    VehicleInput idle;
    for (int i = 0; i < 120; ++i) {
        vehicle->set_input(idle);
        vehicle->update(dt);
        runtime->step(dt);
    }
    check(vehicle->speed() < 1.0f, "idle vehicle does not creep");

    BodyState before;
    runtime->physics().body_state(vehicle->chassis(), before);
    VehicleInput drive;
    drive.throttle = 1.0f;
    for (int i = 0; i < 60; ++i) {
        vehicle->set_input(drive);
        vehicle->update(dt);
        runtime->step(dt);
    }
    check(vehicle->speed() > 2.0f, "full throttle exceeds 2 m/s");

    BodyState after;
    runtime->physics().body_state(vehicle->chassis(), after);
    check(after.position.z < before.position.z - 0.5f,
          "chassis really moved forward (>0.5 m in 1 s)");

    const auto states = vehicle->wheel_states();
    check(states.size() == 4, "four wheel states from the constraint");
    bool grounded = true;
    for (const auto& state : states) grounded = grounded && state.grounded;
    check(grounded, "all wheels grounded after settling");
    std::printf("[vehicle-asset] assembly + drive: real motion (%.3f m/s, "
                "moved %.3f m) OK\n",
                vehicle->speed(), before.position.z - after.position.z);
}

// The asset's drivetrain drives the Jolt engine: a weak explicit engine torque
// accelerates far slower than the derived (full-force) default.
void test_drivetrain_drives_engine() {
    using namespace engine::gameplay;
    auto run = [](float engineTorque, float& speedOut) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        BodySpec ground;
        ground.motion = MotionType::Static;
        ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
        runtime->physics().create_body(ground);

        engine::vehicles::VehicleAsset asset = make_car_asset();
        asset.position = {0.0f, 1.2f, 0.0f};
        asset.drivetrain.engineMaxTorque = engineTorque;
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        if (!vehicle || !vehicle->valid()) {
            speedOut = -1.0f;
            return;
        }
        const float dt = 1.0f / 60.0f;
        VehicleInput idle;
        for (int i = 0; i < 120; ++i) {
            vehicle->set_input(idle);
            vehicle->update(dt);
            runtime->step(dt);
        }
        VehicleInput drive;
        drive.throttle = 1.0f;
        // 4 s window: derived torque keeps accelerating (auto-shift through the
        // gears) while the weak engine plateaus at a much lower speed — a clear
        // discriminator (the 1 s window lands both near 4 m/s).
        for (int i = 0; i < 240; ++i) {
            vehicle->set_input(drive);
            vehicle->update(dt);
            runtime->step(dt);
        }
        speedOut = vehicle->speed();
    };

    float derived = 0.0f, weak = 0.0f;
    run(0.0f, derived);    // derived: first gear delivers maxDriveForce
    run(5.0f, weak);       // explicit weak engine (no wheel spin-out)
    check(derived > 2.0f, "derived torque accelerates past 2 m/s");
    check(weak > 0.0f && weak < derived * 0.5f,
          "weak explicit torque accelerates far slower");
    std::printf("[vehicle-asset] drivetrain: derived %.3f vs weak %.3f m/s OK\n",
                derived, weak);
}

// Determinism: identical assets + identical input on fresh runtimes produce
// bit-identical chassis state.
void test_determinism() {
    using namespace engine::gameplay;
    auto run = [](float& posZ, float& yaw) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        BodySpec ground;
        ground.motion = MotionType::Static;
        ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
        runtime->physics().create_body(ground);

        engine::vehicles::VehicleAsset asset = make_car_asset();
        asset.position = {0.0f, 1.2f, 0.0f};
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        const float dt = 1.0f / 60.0f;
        VehicleInput idle;
        for (int i = 0; i < 60; ++i) {
            vehicle->set_input(idle);
            vehicle->update(dt);
            runtime->step(dt);
        }
        VehicleInput drive;
        drive.throttle = 1.0f;
        drive.steering = 0.5f;
        for (int i = 0; i < 90; ++i) {
            vehicle->set_input(drive);
            vehicle->update(dt);
            runtime->step(dt);
        }
        BodyState state;
        runtime->physics().body_state(vehicle->chassis(), state);
        posZ = state.position.z;
        yaw = state.rotation.y;
    };

    float z1 = 0.0f, yaw1 = 0.0f, z2 = 0.0f, yaw2 = 0.0f;
    run(z1, yaw1);
    run(z2, yaw2);
    check(z1 == z2, "identical chassis position across identical runs");
    check(yaw1 == yaw2, "identical chassis yaw across identical runs");
    std::printf("[vehicle-asset] determinism: bit-identical across instances OK\n");
}

// The public contract also assembles on builtin worlds (the raycast fallback).
void test_builtin_fallback() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Builtin);
    check(runtime->physics_backend() == PhysicsBackend::Builtin,
          "runtime uses builtin backend");

    // Ground so the raycast fallback has a surface to push against.
    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    runtime->physics().create_body(ground);

    engine::vehicles::VehicleAsset asset = make_car_asset();
    asset.position = {0.0f, 1.2f, 0.0f};
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr, "asset assembles on builtin");
    check(vehicle->valid(), "builtin vehicle is valid");

    VehicleInput drive;
    drive.throttle = 1.0f;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 180; ++i) {
        vehicle->set_input(drive);
        vehicle->update(dt);
        runtime->step(dt);
    }
    check(vehicle->speed() > 2.0f, "raycast fallback accelerates (mirror)");
    std::printf("[vehicle-asset] builtin fallback: raycast path OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible on crash
    test_json_round_trip();
    test_validation();
    test_assembly_and_drive();
    test_drivetrain_drives_engine();
    test_determinism();
    test_builtin_fallback();
    if (g_failures == 0) {
        std::printf("[vehicle-asset] ALL PASSED\n");
        return 0;
    }
    std::printf("[vehicle-asset] %d FAILURE(S)\n", g_failures);
    return 1;
}

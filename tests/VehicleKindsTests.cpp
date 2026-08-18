// VehicleKindsTests.cpp
//
// FALTANTES §17 item 3: "Suportar carro, moto e esteira pelo Jolt; asas /
// propulsão / flutuação por módulos públicos". The Jolt backend assembles the
// controller family from VehicleDesc.kind (WheeledVehicleController,
// MotorcycleController with the lean spring, TrackedVehicleController with
// two tracks); the propulsion modules (Wing / Thruster / Buoyancy) are pure
// force generators applied through the physics seam on every backend. These
// tests prove: the wheeled regression still drives, the motorcycle stays
// upright (lean spring), the tank drives and pivots on its tracks, each
// propulsion module produces the intended motion, kind+propulsion survive the
// JSON round-trip, validation is all-or-nothing, and the whole system is
// deterministic.

#include "../src/engine/physics/PhysicsRuntime.hpp"
#include "../src/engine/gameplay/VehicleRuntime.hpp"

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// Internal path (PhysicsRuntime + VehicleRuntime) helpers.
// ---------------------------------------------------------------------------

struct Car {
    Engine::Physics::BodyHandle ground{Engine::Physics::InvalidBody};
    Engine::Physics::BodyHandle chassis{Engine::Physics::InvalidBody};
    std::vector<Engine::Gameplay::WheelDesc> wheels;
};

Car make_ground_and_chassis(Engine::Physics::PhysicsRuntime& world, float mass,
                            const glm::vec3& position, const glm::vec3& halfExtents) {
    Car car;
    Engine::Physics::BodyDesc ground;
    ground.motion = Engine::Physics::MotionType::Static;
    ground.collider.shape = Engine::Physics::BoxShape{{50.0f, 1.0f, 50.0f}};
    car.ground = world.create_body(ground);

    Engine::Physics::BodyDesc chassis;
    chassis.motion = Engine::Physics::MotionType::Dynamic;
    chassis.mass = mass;
    chassis.position = position;
    chassis.collider.shape = Engine::Physics::BoxShape{halfExtents};
    car.chassis = world.create_body(chassis);
    return car;
}

// Chassis only, no ground: used by the pure force-module tests (wing /
// thruster / buoyancy) where the body must fly freely.
Car make_free_chassis(Engine::Physics::PhysicsRuntime& world, float mass,
                      const glm::vec3& position, const glm::vec3& halfExtents) {
    Car car;
    car.ground = Engine::Physics::InvalidBody;
    Engine::Physics::BodyDesc chassis;
    chassis.motion = Engine::Physics::MotionType::Dynamic;
    chassis.mass = mass;
    chassis.position = position;
    chassis.collider.shape = Engine::Physics::BoxShape{halfExtents};
    car.chassis = world.create_body(chassis);
    return car;
}

// 4-wheel car: steering on the front axle, driven everywhere.
Car make_car(Engine::Physics::PhysicsRuntime& world, float startZ = 0.0f) {
    Car car = make_ground_and_chassis(world, 1200.0f, {0.0f, 1.2f, startZ}, {0.9f, 0.35f, 0.56f});
    const glm::vec3 locals[4] = {
        {-1.3f, -0.1f, -0.8f}, {-1.3f, -0.1f, 0.8f},
        {1.3f, -0.1f, -0.8f},  {1.3f, -0.1f, 0.8f},
    };
    car.wheels.resize(4);
    for (int i = 0; i < 4; ++i) {
        car.wheels[i].localPosition = locals[i];
        car.wheels[i].steering = i < 2;
        car.wheels[i].driven = true;
    }
    return car;
}

// Motorcycle: 2 wheels in line. The lean spring balances a TALL chassis with
// the wheels low and a LONG wheelbase — the calibrated geometry the Jolt
// Motorcycle controller can hold upright while driving.
Car make_motorcycle(Engine::Physics::PhysicsRuntime& world, float startZ = 0.0f) {
    Car moto = make_ground_and_chassis(world, 300.0f, {0.0f, 1.2f, startZ}, {0.4f, 0.8f, 0.9f});
    moto.wheels.resize(2);
    moto.wheels[0].localPosition = {0.0f, -0.3f, -1.1f};
    moto.wheels[1].localPosition = {0.0f, -0.3f, 1.1f};
    moto.wheels[0].steering = true;
    moto.wheels[1].steering = true;
    moto.wheels[0].driven = true;
    moto.wheels[1].driven = true;
    moto.wheels[0].radius = 0.45f;
    moto.wheels[1].radius = 0.45f;
    return moto;
}

// Tank: tracked kind with two tracks, each with a pair of wheels by local x
// sign (the backend splits the wheels into tracks by x).
Car make_tank(Engine::Physics::PhysicsRuntime& world, float startZ = 0.0f) {
    Car tank = make_ground_and_chassis(world, 2500.0f, {0.0f, 1.2f, startZ}, {1.1f, 0.5f, 0.9f});
    const glm::vec3 locals[4] = {
        {-1.2f, -0.2f, -0.7f}, {-1.2f, -0.2f, 0.7f},
        {1.2f, -0.2f, -0.7f},  {1.2f, -0.2f, 0.7f},
    };
    tank.wheels.resize(4);
    for (int i = 0; i < 4; ++i) {
        tank.wheels[i].localPosition = locals[i];
        tank.wheels[i].steering = false;
        tank.wheels[i].driven = true;
        tank.wheels[i].radius = 0.4f;
    }
    return tank;
}

void tick(Engine::Gameplay::VehicleRuntime& vehicle,
          Engine::Physics::PhysicsRuntime& world,
          const Engine::Gameplay::VehicleInput& input, int ticks) {
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < ticks; ++i) {
        vehicle.set_input(input);
        vehicle.update(world, dt);
        world.step(dt);
    }
}

// ---------------------------------------------------------------------------
// Tests (internal path).
// ---------------------------------------------------------------------------

// The wheeled regression: the adapter still drives the chassis forward with
// real displacement (the Jolt VehicleConstraint simulates, not just an eager
// mirror).
void test_wheeled_regression() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_car(world);
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));
    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 120);
    const float startZ = world.body(car.chassis)->position.z;

    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    tick(vehicle, world, drive, 60);
    const Engine::Physics::RigidBody* after = world.body(car.chassis);
    check(vehicle.speed(world) > 2.0f, "wheeled: full throttle exceeds 2 m/s");
    check(after->position.z < startZ - 0.5f,
          "wheeled: chassis actually moved forward (>0.5 m in 1 s)");
    std::printf("[vehicle-kinds] wheeled regression: speed %.3f m/s, moved %.3f "
                "m OK\n",
                vehicle.speed(world), startZ - after->position.z);
}

// The motorcycle stays upright while driving: the lean spring (Motorcycle
// controller) balances the chassis — roll stays small and the bike advances.
void test_motorcycle_upright() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car moto = make_motorcycle(world);
    Engine::Gameplay::VehicleRuntime vehicle(moto.chassis, std::move(moto.wheels),
                                             Engine::Gameplay::VehicleDrivetrain{},
                                             Engine::Physics::VehicleKind::Motorcycle, {});
    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 180);  // settle + balance

    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 0.5f;
    float maxRoll = 0.0f;
    for (int i = 0; i < 240; ++i) {
        vehicle.set_input(drive);
        vehicle.update(world, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
        const Engine::Physics::RigidBody* body = world.body(moto.chassis);
        const glm::vec3 up = glm::normalize(body->rotation * glm::vec3(0.0f, 1.0f, 0.0f));
        maxRoll = std::max(maxRoll, std::acos(glm::clamp(up.y, -1.0f, 1.0f)));
    }
    check(maxRoll < 0.4f, "motorcycle: chassis stays upright (roll < 0.4 rad)");
    check(vehicle.speed(world) > 2.0f, "motorcycle: drives forward");
    std::printf("[vehicle-kinds] motorcycle: max roll %.3f rad, speed %.3f m/s "
                "OK\n",
                maxRoll, vehicle.speed(world));
}

// The tank drives on its tracks and pivots: throttle advances the chassis and
// steering rotates it (differential track ratios).
void test_tank_moves_and_turns() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car tank = make_tank(world);
    Engine::Gameplay::VehicleRuntime vehicle(tank.chassis, std::move(tank.wheels),
                                             Engine::Gameplay::VehicleDrivetrain{},
                                             Engine::Physics::VehicleKind::Tracked, {});
    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 120);

    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    tick(vehicle, world, drive, 120);
    const float yawStraight = world.body(tank.chassis)->rotation.y;
    check(vehicle.speed(world) > 1.5f, "tank: drives forward on its tracks");
    check(vehicle.valid(world), "tank: vehicle valid");

    // Steer at speed: the tracks run at differential ratios -> yaw changes.
    Engine::Gameplay::VehicleInput steer;
    steer.throttle = 1.0f;
    steer.steering = 0.8f;
    tick(vehicle, world, steer, 120);
    const float yawSteered = world.body(tank.chassis)->rotation.y;
    check(std::abs(yawSteered - yawStraight) > 0.05f,
          "tank: steering turned the chassis (differential tracks)");
    std::printf("[vehicle-kinds] tank: speed %.3f m/s, yaw delta %.3f rad OK\n",
                vehicle.speed(world), yawSteered - yawStraight);
}

// Wing: lift from forward speed — a body with forward velocity and a wing
// module rises (lift exceeds gravity) while the same body without the wing
// falls. No ground: the wing is a pure force module, the comparison is the
// control run.
void test_wing_module() {
    auto run = [](bool withWing, float& yAfter, float& vyAfter) {
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        Car car = make_free_chassis(world, 50.0f, {0.0f, 5.0f, 0.0f}, {0.5f, 0.2f, 0.5f});
        std::vector<Engine::Gameplay::WheelDesc> wheels(1);
        wheels[0].localPosition = {0.0f, -0.4f, 0.0f};
        Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(wheels));
        if (withWing) {
            Engine::Physics::PropulsionModule wing;
            wing.kind = Engine::Physics::PropulsionKind::Wing;
            wing.area = 25.0f;
            wing.liftCoefficient = 1.2f;
            vehicle.set_propulsion({wing});
        }
        // Give the body a fast forward velocity (the wing only lifts in
        // flight).
        Engine::Physics::RigidBody* body = world.body(car.chassis);
        body->linearVelocity = {0.0f, 0.0f, -30.0f};
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 60; ++i) {
            vehicle.update(world, dt);
            world.step(dt);
        }
        yAfter = world.body(car.chassis)->position.y;
        vyAfter = world.body(car.chassis)->linearVelocity.y;
    };

    float yWing = 0.0f, vyWing = 0.0f, yNoWing = 0.0f, vyNoWing = 0.0f;
    run(true, yWing, vyWing);
    run(false, yNoWing, vyNoWing);
    check(yWing > yNoWing, "wing: lifting body rises above the no-wing control");
    check(vyWing > vyNoWing, "wing: lifting body has higher vertical velocity "
                             "than control");
    std::printf("[vehicle-kinds] wing: y %.3f (wing) vs %.3f (control) OK\n",
                yWing, yNoWing);
}

// Thruster: throttle pushes along the axis — a hanging body with a thruster
// along +Y rises against gravity.
void test_thruster_module() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_free_chassis(world, 100.0f, {0.0f, 5.0f, 0.0f}, {0.5f, 0.3f, 0.5f});
    std::vector<Engine::Gameplay::WheelDesc> wheels(1);
    wheels[0].localPosition = {0.0f, -0.4f, 0.0f};
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(wheels));
    Engine::Physics::PropulsionModule thruster;
    thruster.kind = Engine::Physics::PropulsionKind::Thruster;
    thruster.axis = {0.0f, 1.0f, 0.0f};
    thruster.maxForce = 2500.0f;
    vehicle.set_propulsion({thruster});

    Engine::Gameplay::VehicleInput full;
    full.throttle = 1.0f;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 90; ++i) {
        vehicle.set_input(full);
        vehicle.update(world, dt);
        world.step(dt);
    }
    const Engine::Physics::RigidBody* body = world.body(car.chassis);
    check(body->position.y > 5.5f, "thruster: body rose against gravity");
    check(body->linearVelocity.y > 0.0f, "thruster: body moving upward");
    std::printf("[vehicle-kinds] thruster: y %.3f, vy %.3f OK\n",
                body->position.y, body->linearVelocity.y);
}

// Buoyancy: Archimedes — a body below the water level with a buoyancy module
// floats (net upward). The module force is rho * g * maxForce * submerged
// with g = |gravity| (glm::length — NOT the vec3::length() component count).
// No ground: the chassis floats freely and the module's net force exceeds
// gravity, so the body accelerates upward.
void test_buoyancy_module() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_free_chassis(world, 100.0f, {0.0f, -2.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    std::vector<Engine::Gameplay::WheelDesc> wheels(1);
    wheels[0].localPosition = {0.0f, -0.5f, 0.0f};
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(wheels));
    Engine::Physics::PropulsionModule buoyancy;
    buoyancy.kind = Engine::Physics::PropulsionKind::Buoyancy;
    buoyancy.fluidDensity = 1000.0f;
    buoyancy.maxForce = 0.3f;
    buoyancy.waterLevel = 0.0f;
    vehicle.set_propulsion({buoyancy});

    Engine::Gameplay::VehicleInput idle;
    const float dt = 1.0f / 60.0f;
    // Sample while the body is still below the water line (net force is
    // constant there: buoyancy 2943 N > weight 981 N -> a = +19.6 m/s^2).
    // After it crosses the surface the module ramps off and gravity takes
    // over again, so a longer window would mask the float.
    for (int i = 0; i < 22; ++i) {
        vehicle.set_input(idle);
        vehicle.update(world, dt);
        world.step(dt);
    }
    const Engine::Physics::RigidBody* body = world.body(car.chassis);
    check(body->position.y > -1.0f, "buoyancy: body floated up (net buoyant)");
    check(body->linearVelocity.y > 1.0f,
          "buoyancy: body accelerating upward (force exceeds weight)");
    std::printf("[vehicle-kinds] buoyancy: y %.3f, vy %.3f (started -2.0) OK\n",
                body->position.y, body->linearVelocity.y);
}

// Determinism: identical worlds + identical input -> bit-identical chassis
// state across instances (motorcycle kind, exercising the lean spring path).
void test_determinism() {
    auto run = [](float& posZ, float& yaw, float& roll) {
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        Car moto = make_motorcycle(world, 7.0f);
        Engine::Gameplay::VehicleRuntime vehicle(moto.chassis, std::move(moto.wheels),
                                                 Engine::Gameplay::VehicleDrivetrain{},
                                                 Engine::Physics::VehicleKind::Motorcycle, {});
        Engine::Gameplay::VehicleInput idle;
        tick(vehicle, world, idle, 120);
        Engine::Gameplay::VehicleInput drive;
        drive.throttle = 0.5f;
        drive.steering = 0.5f;
        tick(vehicle, world, drive, 90);
        const Engine::Physics::RigidBody* body = world.body(moto.chassis);
        posZ = body->position.z;
        yaw = body->rotation.y;
        roll = body->rotation.x;
    };

    float z1 = 0.0f, yaw1 = 0.0f, roll1 = 0.0f;
    float z2 = 0.0f, yaw2 = 0.0f, roll2 = 0.0f;
    run(z1, yaw1, roll1);
    run(z2, yaw2, roll2);
    check(z1 == z2, "determinism: identical chassis position");
    check(yaw1 == yaw2, "determinism: identical chassis yaw");
    check(roll1 == roll2, "determinism: identical chassis roll");
    std::printf("[vehicle-kinds] determinism: bit-identical across instances "
                "OK\n");
}

// ---------------------------------------------------------------------------
// Public path: kind + propulsion survive the JSON round-trip and drive the
// assembly (create_vehicle_from_asset).
// ---------------------------------------------------------------------------

void test_json_kind_propulsion_round_trip() {
    engine::vehicles::VehicleAsset asset;
    asset.id = "9e6f5c2a-1d44-4c9b-8e5a-3f0a2b1c4d5e";  // canonical UUID
    asset.name = "tank-turbo";
    asset.kind = engine::vehicles::VehicleKind::Tracked;
    asset.chassis.mass = 2500.0f;
    asset.wheels.resize(4);
    const glm::vec3 locals[4] = {
        {-1.2f, -0.2f, -0.7f}, {-1.2f, -0.2f, 0.7f},
        {1.2f, -0.2f, -0.7f},  {1.2f, -0.2f, 0.7f},
    };
    for (int i = 0; i < 4; ++i) {
        asset.wheels[i].localPosition = locals[i];
        asset.wheels[i].driven = true;
        asset.wheels[i].radius = 0.4f;
    }
    engine::vehicles::PropulsionModule thruster;
    thruster.kind = engine::vehicles::PropulsionKind::Thruster;
    thruster.axis = {0.0f, 1.0f, 0.0f};
    thruster.maxForce = 8000.0f;
    engine::vehicles::PropulsionModule buoyancy;
    buoyancy.kind = engine::vehicles::PropulsionKind::Buoyancy;
    buoyancy.fluidDensity = 1025.0f;
    buoyancy.maxForce = 0.5f;
    buoyancy.waterLevel = -3.0f;
    asset.propulsion = {thruster, buoyancy};

    const std::string json = asset.to_json();
    engine::vehicles::VehicleAsset parsed;
    std::string error;
    check(parsed.load_from_json(json, error), "json: load_from_json succeeds");
    check(parsed.kind == engine::vehicles::VehicleKind::Tracked,
          "json: kind round-trips (tracked)");
    check(parsed.propulsion.size() == 2, "json: two propulsion modules");
    check(parsed.propulsion[0].kind == engine::vehicles::PropulsionKind::Thruster,
          "json: thruster kind round-trips");
    check(parsed.propulsion[1].kind == engine::vehicles::PropulsionKind::Buoyancy,
          "json: buoyancy kind round-trips");
    check(parsed.propulsion[1].fluidDensity == 1025.0f,
          "json: buoyancy fluidDensity round-trips");
    check(parsed.propulsion[0].maxForce == 8000.0f,
          "json: thruster maxForce round-trips");
    check(parsed.wheels.size() == 4, "json: wheels round-trip");
    check(parsed.id == asset.id, "json: canonical id preserved");
    std::printf("[vehicle-kinds] json: kind + propulsion round-trip OK\n");
}

void test_json_validation() {
    engine::vehicles::VehicleAsset asset;
    asset.kind = engine::vehicles::VehicleKind::Wheeled;
    asset.wheels.resize(2);
    std::string error;

    // Unknown kind is refused all-or-nothing.
    const std::string badKind =
        "{\"name\":\"x\",\"kind\":\"flying-saucer\",\"wheels\":[{\"radius\":0.3},"
        "{\"radius\":0.3}]}";
    check(!asset.load_from_json(badKind, error), "validation: unknown kind refused");
    check(!error.empty(), "validation: diagnostic provided");

    // Propulsion must be an array.
    const std::string badProp =
        "{\"name\":\"x\",\"kind\":\"wheeled\",\"propulsion\":42,\"wheels\":[{\"radius\":0.3}]}";
    std::string error2;
    check(!asset.load_from_json(badProp, error2),
          "validation: non-array propulsion refused");

    // A wheel with radius 0 is refused.
    const std::string badWheel =
        "{\"name\":\"x\",\"kind\":\"wheeled\",\"wheels\":[{\"radius\":0.0}]}";
    std::string error3;
    check(!asset.load_from_json(badWheel, error3),
          "validation: zero-radius wheel refused");
    std::printf("[vehicle-kinds] validation: all-or-nothing OK\n");
}

// The public assembly drives the tank kind end-to-end: create the runtime from
// the asset and the chassis moves on its tracks.
void test_asset_assembly_drives() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    check(runtime->physics_backend() == PhysicsBackend::Jolt,
          "runtime uses Jolt backend");

    // Ground so the tank can settle and drive.
    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    runtime->physics().create_body(ground);

    engine::vehicles::VehicleAsset asset;
    asset.name = "public-tank";
    asset.kind = engine::vehicles::VehicleKind::Tracked;
    asset.position = {0.0f, 1.2f, 0.0f};
    asset.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    asset.chassis.mass = 2500.0f;
    const glm::vec3 locals[4] = {
        {-1.2f, -0.2f, -0.7f}, {-1.2f, -0.2f, 0.7f},
        {1.2f, -0.2f, -0.7f},  {1.2f, -0.2f, 0.7f},
    };
    for (int i = 0; i < 4; ++i) {
        asset.wheels.push_back(engine::vehicles::WheelComponent{});
        asset.wheels.back().localPosition = locals[i];
        asset.wheels.back().driven = true;
        asset.wheels.back().radius = 0.4f;
    }
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr, "asset assembly: vehicle created");
    check(vehicle->valid(), "asset assembly: assembled tank is valid");
    check(vehicle->chassis().valid(), "asset assembly: chassis body id valid");

    const float dt = 1.0f / 60.0f;
    VehicleInput idle;
    for (int i = 0; i < 120; ++i) {
        vehicle->set_input(idle);
        vehicle->update(dt);
        runtime->step(dt);
    }
    check(vehicle->speed() < 1.0f, "asset assembly: idle tank does not creep");

    BodyState before;
    runtime->physics().body_state(vehicle->chassis(), before);
    VehicleInput drive;
    drive.throttle = 1.0f;
    for (int i = 0; i < 120; ++i) {
        vehicle->set_input(drive);
        vehicle->update(dt);
        runtime->step(dt);
    }
    check(vehicle->speed() > 1.5f, "asset assembly: tank drives forward");

    BodyState after;
    runtime->physics().body_state(vehicle->chassis(), after);
    check(after.position.z < before.position.z - 0.5f,
          "asset assembly: tank really moved forward (>0.5 m in 2 s)");
    std::printf("[vehicle-kinds] asset assembly: drove %.3f m (kind tracked) "
                "OK\n",
                before.position.z - after.position.z);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible on crash
    test_wheeled_regression();
    test_motorcycle_upright();
    test_tank_moves_and_turns();
    test_wing_module();
    test_thruster_module();
    test_buoyancy_module();
    test_determinism();
    test_json_kind_propulsion_round_trip();
    test_json_validation();
    test_asset_assembly_drives();
    if (g_failures == 0) {
        std::printf("[vehicle-kinds] ALL PASSED\n");
        return 0;
    }
    std::printf("[vehicle-kinds] %d FAILURE(S)\n", g_failures);
    return 1;
}

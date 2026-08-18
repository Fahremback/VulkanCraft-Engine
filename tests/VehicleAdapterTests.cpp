// VehicleAdapterTests.cpp
//
// FALTANTES §17 item 1: "Substituir o raycast vehicle artesanal pelo adapter
// Jolt Vehicles". The legacy raycast model is replaced by a real Jolt
// VehicleConstraint on worlds with a vehicle solver; builtin/bullet keep the
// raycast fallback. These tests prove the ADAPTER path — real motion (the
// chassis actually moves, not just an eager mirror velocity), wheel states
// read from the constraint, steering that turns the chassis, braking that
// decelerates, validation, determinism — and that the fallback still works on
// builtin worlds.

#include "../src/engine/physics/PhysicsRuntime.hpp"
#include "../src/engine/gameplay/VehicleRuntime.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

struct Car {
    Engine::Physics::BodyHandle ground{Engine::Physics::InvalidBody};
    Engine::Physics::BodyHandle chassis{Engine::Physics::InvalidBody};
    std::vector<Engine::Gameplay::WheelDesc> wheels;
};

Car make_car(Engine::Physics::PhysicsRuntime& world, float startZ = 0.0f) {
    Car car;
    Engine::Physics::BodyDesc ground;
    ground.motion = Engine::Physics::MotionType::Static;
    ground.collider.shape = Engine::Physics::BoxShape{{50.0f, 1.0f, 50.0f}};
    car.ground = world.create_body(ground);

    Engine::Physics::BodyDesc chassis;
    chassis.motion = Engine::Physics::MotionType::Dynamic;
    chassis.mass = 1200.0f;
    chassis.position = {0.0f, 1.2f, startZ};
    chassis.collider.shape = Engine::Physics::BoxShape{{0.9f, 0.35f, 0.56f}};
    car.chassis = world.create_body(chassis);

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

// The chassis really moves: after throttle the position advances along -Z (the
// engine forward convention) and the reported speed exceeds the threshold.
void test_drives_forward() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_car(world);
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));
    check(vehicle.valid(world), "vehicle valid on Jolt world");
    check(world.supports_vehicles(), "Jolt world supports vehicles");

    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 120);  // suspension settles on the ground

    const Engine::Physics::RigidBody* before = world.body(car.chassis);
    check(before != nullptr, "chassis body alive");
    const float startZ = before->position.z;
    check(vehicle.speed(world) < 1.0f, "idle vehicle does not creep");

    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    tick(vehicle, world, drive, 60);

    const Engine::Physics::RigidBody* after = world.body(car.chassis);
    check(after != nullptr, "chassis body alive after drive");
    check(vehicle.speed(world) > 2.0f, "full throttle exceeds 2 m/s");
    // Real displacement (the adapter simulates, not just an eager mirror).
    check(after->position.z < startZ - 0.5f,
          "chassis actually moved forward (>0.5 m in 1 s)");
    std::printf("[vehicle-adapter] drives forward: speed %.3f m/s, moved %.3f m "
                "OK\n",
                vehicle.speed(world), startZ - after->position.z);
}

// Wheel states come from the Jolt constraint: grounded wheels report contact
// and a suspension length inside [min, max]; steering wheels report the steer
// angle after input.
void test_wheel_states() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_car(world);
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));

    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 120);

    const auto& states = vehicle.wheel_states();
    check(states.size() == 4, "four wheel states");
    bool allGrounded = true;
    for (const auto& state : states) allGrounded = allGrounded && state.grounded;
    check(allGrounded, "all wheels grounded after settling");
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto& state = states[i];
        const auto& desc = vehicle.wheels()[i];
        check(state.suspensionLength >= desc.suspensionRestLength - desc.suspensionTravel - 0.01f,
              "suspension not shorter than min");
        check(state.suspensionLength <= desc.suspensionRestLength + desc.suspensionTravel + 0.01f,
              "suspension not longer than max");
        check(state.compression >= 0.0f, "compression non-negative");
    }

    Engine::Gameplay::VehicleInput steer;
    steer.steering = 1.0f;
    tick(vehicle, world, steer, 10);
    const auto& after = vehicle.wheel_states();
    check(std::abs(after[0].steerAngle) > 0.01f, "front (steering) wheel steered");
    check(std::abs(after[2].steerAngle) < 0.01f, "rear (non-steering) wheel not steered");
    std::printf("[vehicle-adapter] wheel states: grounded, suspension in range, "
                "steer applied OK\n");
}

// Steering input turns the chassis: yaw changes vs the straight-line run.
void test_steering_turns() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_car(world);
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));

    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 120);

    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    tick(vehicle, world, drive, 60);  // straight approach
    const float yawStraight = world.body(car.chassis)->rotation.y;

    Engine::Gameplay::VehicleInput steer;
    steer.throttle = 1.0f;
    steer.steering = 1.0f;
    tick(vehicle, world, steer, 60);  // full lock
    const float yawSteered = world.body(car.chassis)->rotation.y;
    check(std::abs(yawSteered - yawStraight) > 0.05f,
          "steering changed the chassis yaw");
    std::printf("[vehicle-adapter] steering turns: yaw delta %.3f rad OK\n",
                yawSteered - yawStraight);
}

// Brake input decelerates a moving vehicle.
void test_brake() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_car(world);
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));

    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 120);
    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    tick(vehicle, world, drive, 60);
    const float driveSpeed = vehicle.speed(world);
    check(driveSpeed > 2.0f, "car is moving before braking");

    Engine::Gameplay::VehicleInput stop;
    stop.brake = 1.0f;
    tick(vehicle, world, stop, 90);
    check(vehicle.speed(world) < driveSpeed, "brake reduced the speed");
    std::printf("[vehicle-adapter] brake: %.3f -> %.3f m/s OK\n",
                driveSpeed, vehicle.speed(world));
}

// Validation: invalid descriptors and handles are refused all-or-nothing.
void test_validation() {
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Car car = make_car(world);

    Engine::Physics::VehicleDesc emptyWheels;
    emptyWheels.chassis = car.chassis;
    check(world.create_vehicle(emptyWheels) == Engine::Physics::InvalidVehicle,
          "vehicle with no wheels refused");

    Engine::Physics::VehicleDesc badChassis;
    badChassis.chassis = Engine::Physics::InvalidBody;
    badChassis.wheels = car.wheels;
    check(world.create_vehicle(badChassis) == Engine::Physics::InvalidVehicle,
          "vehicle with invalid chassis refused");

    Engine::Physics::VehicleDesc ghostChassis;
    ghostChassis.chassis = 9999;
    ghostChassis.wheels = car.wheels;
    check(world.create_vehicle(ghostChassis) == Engine::Physics::InvalidVehicle,
          "vehicle with unknown chassis refused");

    Engine::Gameplay::VehicleInput input;
    check(!world.set_vehicle_input(Engine::Physics::InvalidVehicle, input),
          "input on invalid handle refused");
    Engine::Physics::WheelState out;
    check(!world.vehicle_wheel_state(Engine::Physics::InvalidVehicle, 0, out),
          "wheel state on invalid handle refused");

    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, car.wheels);
    Engine::Gameplay::VehicleInput idle;
    tick(vehicle, world, idle, 1);
    check(vehicle.valid(world), "vehicle created through the runtime");
    const auto& states = vehicle.wheel_states();
    check(states.size() == 4, "four states after creation");
    std::printf("[vehicle-adapter] validation: all-or-nothing OK\n");
}

// Determinism: identical worlds + identical input -> bit-identical chassis
// state after the same number of ticks.
void test_determinism() {
    auto run = [](float& posZ, float& yaw) {
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        Car car = make_car(world, 7.0f);
        Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));
        Engine::Gameplay::VehicleInput idle;
        tick(vehicle, world, idle, 60);
        Engine::Gameplay::VehicleInput drive;
        drive.throttle = 1.0f;
        drive.steering = 0.5f;
        tick(vehicle, world, drive, 90);
        const Engine::Physics::RigidBody* body = world.body(car.chassis);
        posZ = body->position.z;
        yaw = body->rotation.y;
    };

    float z1 = 0.0f, yaw1 = 0.0f, z2 = 0.0f, yaw2 = 0.0f;
    run(z1, yaw1);
    run(z2, yaw2);
    check(z1 == z2, "identical chassis position across identical runs");
    check(yaw1 == yaw2, "identical chassis yaw across identical runs");
    std::printf("[vehicle-adapter] determinism: bit-identical across instances "
                "OK\n");
}

// Builtin worlds have no vehicle solver: the legacy raycast path stays as the
// fallback (eager mirror velocity semantics preserved).
void test_builtin_fallback() {
    Engine::Physics::PhysicsRuntime world;  // Builtin
    check(!world.supports_vehicles(), "builtin world has no vehicle solver");
    Car car = make_car(world);
    Engine::Gameplay::VehicleRuntime vehicle(car.chassis, std::move(car.wheels));
    check(vehicle.valid(world), "vehicle valid on builtin world");

    Engine::Gameplay::VehicleInput idle;
    for (int i = 0; i < 120; ++i) vehicle.update(world, 1.0f / 60.0f);
    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    for (int i = 0; i < 60; ++i) {
        vehicle.set_input(drive);
        vehicle.update(world, 1.0f / 60.0f);
    }
    check(vehicle.speed(world) > 2.0f, "raycast fallback accelerates (mirror)");
    std::printf("[vehicle-adapter] builtin fallback: raycast path OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible on crash
    test_drives_forward();
    test_wheel_states();
    test_steering_turns();
    test_brake();
    test_validation();
    test_determinism();
    test_builtin_fallback();
    if (g_failures == 0) {
        std::printf("[vehicle-adapter] ALL PASSED\n");
        return 0;
    }
    std::printf("[vehicle-adapter] %d FAILURE(S)\n", g_failures);
    return 1;
}

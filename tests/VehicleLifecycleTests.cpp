// VehicleLifecycleTests.cpp
//
// FALTANTES §17 items 8-9: occupants (seats, entry, exit, kinematic ride) and
// damage / part separation. Parts are AUTO-DERIVED from the vehicle's
// components — Chassis + Drivetrain + Wheel per wheel (rigid) and + Beam per
// beam (XPBD chassis) — damage degrades their behavior (damaged wheel loses
// grip/drive, damaged drivetrain loses torque, damaged beam degrades the
// deformable chassis stiffness) and at 0 health a part SEPARATES (wheel pops
// off, beam deactivates). These tests prove: seats JSON round-trip (both
// assets), occupant enter/ride/exit on rigid + beam chassis (the body follows
// the seat and spawns at the exit offset), occupant validation, damage parts
// auto-derivation, wheel/drivetrain damage + separation reducing acceleration,
// beam damage increasing deformation (incl. separation), repair restoring, the
// solver's set_edge_stiffness deactivation, and determinism.

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"
#include "engine/vehicles/IBeamGraphAsset.hpp"
#include "engine/deformable/IDeformableProvider.hpp"

#include "../src/engine/physics/PhysicsRuntime.hpp"
#include "../src/engine/gameplay/BeamChassisRuntime.hpp"

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

// A car asset with distinctive seat values + 4 driven wheels.
engine::vehicles::VehicleAsset make_car_asset() {
    using namespace engine::vehicles;
    VehicleAsset asset;
    asset.name = "lifecycle-car";
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
    VehicleSeat driver;
    driver.name = "driver";
    driver.localPosition = {0.0f, 0.6f, -0.45f};
    driver.exitOffset = {0.0f, 1.2f, 1.2f};
    asset.seats.push_back(driver);
    return asset;
}

// Adds a ground plane (top at y=0) to a public runtime world.
void add_ground(engine::gameplay::IGameplayRuntime& runtime) {
    using namespace engine::gameplay;
    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    ground.position = {0.0f, -0.5f, 0.0f};
    runtime.physics().create_body(ground);
}

template <typename TVehicle>
void public_drive(TVehicle& vehicle,
                  engine::gameplay::IGameplayRuntime& runtime,
                  float throttle, int ticks) {
    const float dt = 1.0f / 60.0f;
    engine::gameplay::VehicleInput input;
    input.throttle = throttle;
    for (int i = 0; i < ticks; ++i) {
        vehicle.set_input(input);
        vehicle.update(dt);
        runtime.step(dt);
    }
}

// --- 1/2. Seats JSON round-trip (both assets) -------------------------------

void test_vehicle_seats_json() {
    engine::vehicles::VehicleAsset asset = make_car_asset();
    asset.seats[0].name = "driver";
    asset.seats[0].localPosition = {0.05f, 0.55f, -0.42f};
    asset.seats[0].exitOffset = {0.1f, 1.25f, 1.05f};
    const std::string json = asset.to_json();
    engine::vehicles::VehicleAsset parsed;
    std::string error;
    check(parsed.load_from_json(json, error), "vehicle seats: load succeeds");
    check(parsed.seats.size() == 1, "vehicle seats: seat count round-trips");
    check(parsed.seats[0].name == "driver", "vehicle seats: name round-trips");
    check(same_vec3(parsed.seats[0].localPosition, asset.seats[0].localPosition),
          "vehicle seats: localPosition bit-exact");
    check(same_vec3(parsed.seats[0].exitOffset, asset.seats[0].exitOffset),
          "vehicle seats: exitOffset bit-exact");
    std::printf("[vehicle-lifecycle] vehicle seats JSON round-trip OK\n");
}

void test_beam_seats_json() {
    engine::vehicles::BeamGraphAsset asset;
    asset.name = "beam-lifecycle";
    asset.position = {0.0f, 1.4f, 0.0f};
    asset.nodes = { { {0.0f, 0.0f, 0.0f}, false },
                    { {1.0f, 0.0f, 0.0f}, false } };
    asset.beams = { { 0, 1, 0.9f } };
    engine::vehicles::VehicleSeat seat;
    seat.name = "driver";
    seat.localPosition = {0.0f, 0.5f, 0.0f};
    seat.exitOffset = {0.3f, 1.4f, 0.6f};
    asset.seats.push_back(seat);
    const std::string json = asset.to_json();
    engine::vehicles::BeamGraphAsset parsed;
    std::string error;
    check(parsed.load_from_json(json, error), "beam seats: load succeeds");
    check(parsed.seats.size() == 1, "beam seats: seat count round-trips");
    check(parsed.seats[0].name == "driver", "beam seats: name round-trips");
    check(same_vec3(parsed.seats[0].localPosition, seat.localPosition),
          "beam seats: localPosition bit-exact");
    check(same_vec3(parsed.seats[0].exitOffset, seat.exitOffset),
          "beam seats: exitOffset bit-exact");
    std::printf("[vehicle-lifecycle] beam seats JSON round-trip OK\n");
}

// --- 3. Seats validation -----------------------------------------------------

void test_seats_validation() {
    engine::vehicles::VehicleAsset car;
    std::string error;
    check(!car.load_from_json(
              "{\"name\":\"x\",\"wheels\":[{\"localPosition\":[0,0,0]}],"
              "\"seats\":[{\"localPosition\":[0,0,0],\"exitOffset\":[nan,0,0]}]}",
              error),
          "vehicle seats: non-finite exitOffset refused");
    engine::vehicles::BeamGraphAsset beam;
    check(!beam.load_from_json(
              "{\"name\":\"x\",\"nodes\":[{\"position\":[0,0,0]},"
              "{\"position\":[1,0,0]}],\"beams\":[{\"a\":0,\"b\":1}],"
              "\"seats\":[{\"localPosition\":[0,0,0],\"exitOffset\":[nan,0,0]}]}",
              error),
          "beam seats: non-finite exitOffset refused");
    std::printf("[vehicle-lifecycle] seats validation OK\n");
}

// --- 4. Occupant enter / ride / exit (public rigid vehicle) -----------------

void test_occupant_enter_ride_exit() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr && vehicle->valid(), "vehicle assembled");
    check(vehicle->seat_count() == 1, "one seat exposed");

    BodySpec occupantSpec;
    occupantSpec.motion = MotionType::Dynamic;
    occupantSpec.shape = CapsuleShape{0.3f, 0.7f};
    occupantSpec.mass = 75.0f;
    occupantSpec.position = asset.position + glm::vec3(0.0f, 1.0f, 0.0f);
    const BodyId occupant = runtime->physics().create_body(occupantSpec);
    check(occupant.valid(), "occupant body created");

    std::string error;
    // Settle the vehicle BEFORE measuring (the landing pitch during the first
    // ticks would otherwise dominate the displacement comparison).
    public_drive(*vehicle, *runtime, 0.0f, 120);
    check(vehicle->enter(occupant, 0, error), "occupant enters seat 0");
    check(vehicle->seat_occupied(0), "seat 0 occupied");
    check(vehicle->occupant(0).id == occupant.id, "occupant id reported");
    error.clear();
    check(vehicle->enter(occupant, 0, error) == false && !error.empty(),
          "double enter refused (seat taken)");

    // Snap the occupant to the seat pose (one update places it) BEFORE the
    // baseline — the spawn point is above the seat, and that initial drop
    // must not count as ride motion.
    engine::gameplay::VehicleInput idleInput;
    vehicle->set_input(idleInput);
    vehicle->update(1.0f / 60.0f);

    // Ride: the chassis drives forward; the occupant follows the seat (it is
    // driven to the seat pose each update — the occupant lags the chassis by
    // at most one physics step, so the comparison is loose).
    BodyState c0, o0;
    runtime->physics().body_state(vehicle->chassis(), c0);
    runtime->physics().body_state(occupant, o0);
    public_drive(*vehicle, *runtime, 1.0f, 60);
    BodyState c1, o1;
    runtime->physics().body_state(vehicle->chassis(), c1);
    runtime->physics().body_state(occupant, o1);
    const glm::vec3 chassisDelta = c1.position - c0.position;
    const glm::vec3 occupantDelta = o1.position - o0.position;
    check(glm::length(chassisDelta) > 0.5f, "chassis really moved during the ride");
    check(glm::length(occupantDelta - chassisDelta) < 0.2f,
          "occupant followed the seat (same displacement as the chassis)");
    check(glm::length(o1.position - vehicle->seat_position(0)) < 0.15f,
          "occupant glued to the seat pose");

    // Exit: the seat empties and the occupant spawns at the exit offset.
    check(vehicle->exit(0, error), "occupant exits seat 0");
    check(!vehicle->seat_occupied(0), "seat empty after exit");
    BodyState exitState;
    runtime->physics().body_state(occupant, exitState);
    const glm::vec3 expected = c1.position + c1.rotation * asset.seats[0].exitOffset;
    check(glm::length(exitState.position - expected) < 0.05f,
          "occupant spawned at the seat exit offset");
    check(vehicle->exit(0, error) == false && !error.empty(),
          "exit of an empty seat refused");
    std::printf("[vehicle-lifecycle] occupant enter/ride/exit: chassis moved "
                "%.2f m, occupant followed within %.3f m, exit at offset %.2f OK\n",
                glm::length(chassisDelta),
                glm::length(occupantDelta - chassisDelta),
                glm::length(exitState.position - expected));
}

// --- 5. Occupant on the beam chassis ----------------------------------------

void test_occupant_beam_ride() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);

    engine::vehicles::BeamGraphAsset asset;
    asset.name = "beam-ride";
    asset.position = {0.0f, 1.4f, 0.0f};
    asset.nodes = {
        { {-1.2f, 0.0f, -0.8f}, false },  // 0 front-left
        { {-1.2f, 0.0f, 0.8f}, false },   // 1 back-left
        { {1.2f, 0.0f, -0.8f}, false },   // 2 front-right
        { {1.2f, 0.0f, 0.8f}, false },    // 3 back-right
        { {0.0f, 0.0f, 0.0f}, false },    // 4 center
    };
    asset.beams = {
        { 0, 1, 0.9f }, { 1, 3, 0.9f }, { 3, 2, 0.9f }, { 2, 0, 0.9f },
        { 0, 4, 0.5f }, { 4, 3, 0.5f },
    };
    engine::vehicles::BeamWheelMount mount;
    mount.wheel.radius = 0.36f;
    mount.wheel.springStrength = 26000.0f;
    mount.wheel.damperStrength = 3200.0f;
    mount.wheel.maxDriveForce = 4200.0f;
    mount.wheel.maxBrakeForce = 6000.0f;
    mount.wheel.maxSteerAngle = 0.55f;
    for (std::uint32_t i = 0; i < 4; ++i) {
        engine::vehicles::BeamWheelMount m = mount;
        m.node = i;
        m.steering = i < 2;
        m.driven = true;
        asset.wheels.push_back(m);
    }
    engine::vehicles::VehicleSeat seat;
    seat.name = "driver";
    seat.localPosition = {0.0f, 0.5f, 0.0f};
    seat.exitOffset = {0.0f, 1.2f, 1.0f};
    asset.seats.push_back(seat);

    auto vehicle = runtime->create_beam_vehicle(asset);
    check(vehicle != nullptr && vehicle->valid(), "beam vehicle assembled");
    check(vehicle->seat_count() == 1, "beam vehicle exposes one seat");

    BodySpec occupantSpec;
    occupantSpec.motion = MotionType::Dynamic;
    occupantSpec.shape = CapsuleShape{0.3f, 0.7f};
    occupantSpec.mass = 75.0f;
    occupantSpec.position = asset.position + glm::vec3(0.0f, 1.0f, 0.0f);
    const BodyId occupant = runtime->physics().create_body(occupantSpec);
    std::string error;
    check(vehicle->enter(occupant, 0, error), "occupant enters the beam seat");
    check(vehicle->seat_occupied(0), "beam seat occupied");

    // Drive: the occupant rides the DEFORMED frame (seat_position tracks it).
    const float dt = 1.0f / 60.0f;
    BodyState o0;
    runtime->physics().body_state(occupant, o0);
    public_drive(*vehicle, *runtime, 1.0f, 90);
    BodyState o1;
    runtime->physics().body_state(occupant, o1);
    check(glm::length(o1.position - o0.position) > 0.5f,
          "occupant moved with the beam chassis");
    check(glm::length(o1.position - vehicle->seat_position(0)) < 0.15f,
          "occupant still glued to the deformed seat");

    check(vehicle->exit(0, error), "occupant exits the beam seat");
    check(!vehicle->seat_occupied(0), "beam seat empty after exit");
    BodyState exitState;
    runtime->physics().body_state(occupant, exitState);
    // The exit offset (0, 1.2, 1.0) is OUTSIDE the chassis: the occupant
    // spawned away from the vehicle origin, unlike the seat (0, 0.5, 0).
    check(glm::length(exitState.position - vehicle->chassis_position()) > 1.0f,
          "occupant spawned at the exit offset (outside the beam chassis)");
    std::printf("[vehicle-lifecycle] occupant beam ride: moved %.2f m, glued "
                "%.3f OK\n",
                glm::length(o1.position - o0.position),
                glm::length(o1.position - vehicle->seat_position(0)));
}

// --- 6. Occupant validation --------------------------------------------------

void test_occupant_validation() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);

    BodySpec spec;
    spec.motion = MotionType::Dynamic;
    spec.shape = CapsuleShape{0.3f, 0.7f};
    spec.mass = 75.0f;
    spec.position = asset.position + glm::vec3(0.0f, 1.0f, 0.0f);
    const BodyId occupant = runtime->physics().create_body(spec);
    const BodyId staticBody = runtime->physics().create_body(
        [&] { BodySpec s = spec; s.motion = MotionType::Static; return s; }());

    std::string error;
    check(!vehicle->enter(occupant, 3, error) && !error.empty(),
          "out-of-range seat refused");
    error.clear();
    check(!vehicle->enter(staticBody, 0, error) && !error.empty(),
          "static body cannot enter");
    error.clear();
    check(vehicle->enter(occupant, 0, error), "valid enter accepted");
    std::printf("[vehicle-lifecycle] occupant validation OK\n");
}

// --- 7. Damage parts auto-derived -------------------------------------------

void test_parts_auto_derived() {
    using namespace engine::gameplay;
    // Rigid: chassis + drivetrain + 4 wheels.
    {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        add_ground(*runtime);
        engine::vehicles::VehicleAsset asset = make_car_asset();
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        check(vehicle->part_count() == 6, "rigid parts: chassis+drivetrain+4 wheels");
        const VehiclePartInfo chassis = vehicle->part_info(0);
        const VehiclePartInfo drivetrain = vehicle->part_info(1);
        const VehiclePartInfo wheel0 = vehicle->part_info(2);
        check(chassis.kind == VehiclePartKind::Chassis && chassis.name == "chassis",
              "part 0 is the chassis");
        check(drivetrain.kind == VehiclePartKind::Drivetrain &&
                  drivetrain.name == "drivetrain",
              "part 1 is the drivetrain");
        check(wheel0.kind == VehiclePartKind::Wheel &&
                  wheel0.componentIndex == 0,
              "part 2 is wheel 0 (componentIndex 0)");
        check(chassis.health == chassis.maxHealth && !chassis.separated,
              "parts start at full health");
    }
    // Beam: chassis + 4 wheels + 6 beams.
    {
        engine::vehicles::BeamGraphAsset asset;
        asset.name = "beam-parts";
        asset.position = {0.0f, 1.4f, 0.0f};
        asset.nodes = { { {0.0f, 0.0f, 0.0f}, false },
                        { {1.0f, 0.0f, 0.0f}, false } };
        asset.beams = { { 0, 1, 0.9f } };
        engine::vehicles::BeamWheelMount mount;
        mount.wheel.radius = 0.3f;
        mount.wheel.springStrength = 26000.0f;
        mount.wheel.damperStrength = 3200.0f;
        for (std::uint32_t i = 0; i < 4; ++i) {
            engine::vehicles::BeamWheelMount m = mount;
            m.node = i % 2;
            asset.wheels.push_back(m);
        }
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        add_ground(*runtime);
        auto vehicle = runtime->create_beam_vehicle(asset);
        check(vehicle->part_count() == 1 + 4 + 1,
              "beam parts: chassis+4 wheels+1 beam");
        const VehiclePartInfo beam0 = vehicle->part_info(1 + 4);
        check(beam0.kind == VehiclePartKind::Beam && beam0.componentIndex == 0,
              "last part is beam 0");
    }
    std::printf("[vehicle-lifecycle] parts auto-derived OK\n");
}

// --- 8-10. Damage effects on acceleration (public rigid, Jolt) --------------

void test_wheel_damage_reduces_acceleration() {
    using namespace engine::gameplay;
    auto run = [](bool damaged, float& speed) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        add_ground(*runtime);
        engine::vehicles::VehicleAsset asset = make_car_asset();
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        if (damaged) {
            std::string error;
            // All four driven wheels down to 25% health (drive_scale 0.25).
            for (std::size_t i = 2; i < 6; ++i) {
                check(vehicle->apply_damage(i, 75.0f, error),
                      "driven wheel damaged");
            }
        }
        public_drive(*vehicle, *runtime, 1.0f, 90);
        speed = vehicle->speed();
        return vehicle->valid();
    };
    float healthy = 0.0f, damaged = 0.0f;
    check(run(false, healthy), "healthy vehicle valid");
    check(run(true, damaged), "damaged vehicle valid");
    check(healthy > damaged + 1.0f,
          "damaged driven wheels reduce acceleration");
    std::printf("[vehicle-lifecycle] wheel damage: healthy %.2f vs damaged "
                "%.2f m/s OK\n", healthy, damaged);
}

void test_drivetrain_damage_stops_drive() {
    using namespace engine::gameplay;
    auto run = [](bool destroyDrivetrain, float& speed, bool& separated) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        add_ground(*runtime);
        engine::vehicles::VehicleAsset asset = make_car_asset();
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        if (destroyDrivetrain) {
            std::string error;
            check(vehicle->apply_damage(1, 100.0f, error), "drivetrain destroyed");
            separated = vehicle->is_separated(1);
        }
        public_drive(*vehicle, *runtime, 1.0f, 90);
        speed = vehicle->speed();
    };
    float healthy = 0.0f, dead = 0.0f;
    bool separated = false;
    run(false, healthy, separated);
    run(true, dead, separated);
    check(separated, "destroyed drivetrain is separated");
    check(healthy > 2.0f && dead < 0.5f,
          "a separated drivetrain stops the drive entirely");
    std::printf("[vehicle-lifecycle] drivetrain separation: healthy %.2f vs "
                "separated %.2f m/s OK\n", healthy, dead);
}

void test_all_wheels_separated_stall() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    add_ground(*runtime);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    std::string error;
    for (std::size_t i = 2; i < 6; ++i) {
        check(vehicle->apply_damage(i, 100.0f, error), "wheel separated");
        check(vehicle->is_separated(i), "wheel reported separated");
    }
    public_drive(*vehicle, *runtime, 1.0f, 90);
    check(vehicle->speed() < 0.5f,
          "separating every wheel stalls the vehicle (no drive force)");
    std::printf("[vehicle-lifecycle] wheel separation stalls: speed %.2f m/s "
                "OK\n", vehicle->speed());
}

// --- 11. Beam damage increases deformation ----------------------------------

// Beam chassis: 4 corner nodes + center, cross beams 4/5 are the damage target
// (the center node sags under gravity — stiffness controls how much).
struct BeamRun {
    Engine::Gameplay::BeamChassisRuntime* chassis;
    Engine::Physics::PhysicsRuntime* world;
};

void beam_tick(Engine::Gameplay::BeamChassisRuntime& chassis, int ticks) {
    const float dt = 1.0f / 60.0f;
    Engine::Physics::VehicleInput idle;
    for (int i = 0; i < ticks; ++i) {
        chassis.set_input(idle);
        chassis.update(dt);
    }
}

engine::vehicles::BeamGraphAsset make_beam_asset(float centerStiffness = 0.5f) {
    engine::vehicles::BeamGraphAsset asset;
    asset.name = "beam-damage";
    asset.position = {0.0f, 1.4f, 0.0f};
    asset.nodes = {
        { {-1.2f, 0.0f, -0.8f}, false },
        { {-1.2f, 0.0f, 0.8f}, false },
        { {1.2f, 0.0f, -0.8f}, false },
        { {1.2f, 0.0f, 0.8f}, false },
        { {0.0f, 0.0f, 0.0f}, false },
    };
    asset.beams = {
        { 0, 1, 0.9f }, { 1, 3, 0.9f }, { 3, 2, 0.9f }, { 2, 0, 0.9f },
        { 0, 4, centerStiffness }, { 4, 3, centerStiffness },
    };
    engine::vehicles::BeamWheelMount mount;
    mount.wheel.radius = 0.36f;
    mount.wheel.springStrength = 26000.0f;
    mount.wheel.damperStrength = 3200.0f;
    mount.wheel.maxDriveForce = 4200.0f;
    mount.wheel.maxBrakeForce = 6000.0f;
    mount.wheel.maxSteerAngle = 0.55f;
    for (std::uint32_t i = 0; i < 4; ++i) {
        engine::vehicles::BeamWheelMount m = mount;
        m.node = i;
        m.steering = i < 2;
        m.driven = true;
        asset.wheels.push_back(m);
    }
    return asset;
}

void test_beam_damage_increases_deformation() {
    auto run = [](float beamDamage, float chassisDamage, float& deformation) {
        engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.5f);
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        Engine::Physics::BodyDesc ground;
        ground.motion = Engine::Physics::MotionType::Static;
        ground.collider.shape = Engine::Physics::BoxShape{{50.0f, 1.0f, 50.0f}};
        world.create_body(ground);
        std::string error;
        Engine::Gameplay::BeamChassisRuntime chassis(world, asset, error);
        check(chassis.valid(), "beam chassis valid");
        if (beamDamage > 0.0f) {
            // Parts: [0]=chassis, [1..4]=wheels, [5..10]=beams 0..5 — the
            // cross beams (asset indices 4/5) are parts 9 and 10.
            check(chassis.apply_damage(world, 9, beamDamage, error),
                  "cross beam 0 damaged");
            check(chassis.apply_damage(world, 10, beamDamage, error),
                  "cross beam 1 damaged");
        }
        if (chassisDamage > 0.0f) {
            check(chassis.apply_damage(world, 0, chassisDamage, error),
                  "chassis damaged");
        }
        beam_tick(chassis, 180);
        deformation = chassis.deformation();
    };

    float healthy = 0.0f, damaged = 0.0f, separated = 0.0f, chassisHit = 0.0f;
    run(0.0f, 0.0f, healthy);
    run(75.0f, 0.0f, damaged);     // cross beams at 25% stiffness 0.125
    run(100.0f, 0.0f, separated);  // cross beams separated (stiffness 0)
    run(0.0f, 75.0f, chassisHit);  // chassis at 25% -> every beam * 0.25
    check(damaged > healthy + 0.02f,
          "beam damage increases the chassis deformation");
    check(separated > damaged + 0.02f,
          "separated beams deform strictly more than damaged beams");
    check(chassisHit > healthy + 0.02f,
          "chassis damage degrades every beam (more deformation)");
    std::printf("[vehicle-lifecycle] beam damage: healthy %.3f < chassis-hit "
                "%.3f < beam-damaged %.3f < separated %.3f OK\n",
                healthy, chassisHit, damaged, separated);
}

// --- 12. Repair restores -----------------------------------------------------

void test_repair_restores() {
    using namespace engine::gameplay;
    auto run = [](bool repair, float& speed) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        add_ground(*runtime);
        engine::vehicles::VehicleAsset asset = make_car_asset();
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        std::string error;
        check(vehicle->apply_damage(1, 100.0f, error), "drivetrain destroyed");
        if (repair) {
            check(vehicle->repair(1, 100.0f, error), "drivetrain repaired");
            check(!vehicle->is_separated(1), "no longer separated after repair");
            const VehiclePartInfo part = vehicle->part_info(1);
            check(part.health == part.maxHealth, "health restored to max");
        }
        public_drive(*vehicle, *runtime, 1.0f, 90);
        speed = vehicle->speed();
    };
    float broken = 0.0f, repaired = 0.0f;
    run(false, broken);
    run(true, repaired);
    check(broken < 0.5f && repaired > 2.0f,
          "repair restores the drivetrain (acceleration recovers)");
    std::printf("[vehicle-lifecycle] repair: broken %.2f vs repaired %.2f m/s "
                "OK\n", broken, repaired);
}

// --- 13. Determinism ---------------------------------------------------------

void test_damage_determinism() {
    using namespace engine::gameplay;
    auto run = [](float& finalSpeed) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        add_ground(*runtime);
        engine::vehicles::VehicleAsset asset = make_car_asset();
        auto vehicle = runtime->create_vehicle_from_asset(asset);
        std::string error;
        vehicle->apply_damage(1, 40.0f, error);   // drivetrain 60%
        vehicle->apply_damage(2, 50.0f, error);   // wheel 0 at 50%
        vehicle->apply_damage(4, 25.0f, error);   // wheel 2 at 75%
        // Part states are pure arithmetic — bit-identical across instances.
        bool statesMatch = true;
        for (std::size_t i = 0; i < vehicle->part_count(); ++i) {
            const VehiclePartInfo a = vehicle->part_info(i);
            if (!(a.health == a.maxHealth - (i == 1 ? 40.0f : i == 2 ? 50.0f
                                              : i == 4 ? 25.0f : 0.0f)) ||
                a.separated) {
                statesMatch = false;
            }
        }
        check(statesMatch, "deterministic part health arithmetic");
        public_drive(*vehicle, *runtime, 1.0f, 90);
        finalSpeed = vehicle->speed();
    };
    float a = 0.0f, b = 0.0f;
    run(a);
    run(b);
    check(std::abs(a - b) < 0.01f,
          "determinism: identical damage sequence -> identical speed");
    std::printf("[vehicle-lifecycle] determinism: speed %.4f vs %.4f OK\n", a, b);
}

// --- 14. Solver edge stiffness (separation deactivates the constraint) ------

void test_solver_edge_stiffness() {
    std::string error;
    auto make = [&](bool separate) {
        Engine::Deformable::DeformableConfig config;
        config.groundCollision = false;
        auto provider = Engine::Deformable::create_deformable_provider(
            Engine::Deformable::DeformableProviderKind::Xpbd, config, error);
        Engine::Deformable::DeformableMeshDesc desc;
        desc.nodes = { glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
        desc.fixed = { true, false };
        desc.edges = { { 0, 1 } };
        desc.stiffness = { 0.9f };
        const Engine::Deformable::DeformableBodyHandle body =
            provider->create_body(desc, error);
        if (separate) provider->set_edge_stiffness(body, 0, 0.0f);
        return std::make_pair(std::move(provider), body);
    };

    auto [rigidProvider, rigidBody] = make(false);
    auto [softProvider, softBody] = make(true);
    for (int i = 0; i < 240; ++i) {
        rigidProvider->step(1.0f / 60.0f);
        softProvider->step(1.0f / 60.0f);
    }
    const float rigidLength = glm::length(rigidProvider->node_position(rigidBody, 1) -
                                          rigidProvider->node_position(rigidBody, 0));
    const float separatedLength = glm::length(softProvider->node_position(softBody, 1) -
                                              softProvider->node_position(softBody, 0));
    check(rigidLength < 1.15f, "stiff edge holds the hanging node");
    check(separatedLength > 1.5f,
          "separated edge (stiffness 0) no longer holds — the node falls");

    // Restoring the stiffness re-activates the constraint (the mesh heals).
    softProvider->set_edge_stiffness(softBody, 0, 0.9f);
    for (int i = 0; i < 180; ++i) softProvider->step(1.0f / 60.0f);
    const float restoredLength = glm::length(softProvider->node_position(softBody, 1) -
                                             softProvider->node_position(softBody, 0));
    check(restoredLength < separatedLength - 0.3f && restoredLength < 1.5f,
          "restoring the stiffness re-activates the edge");
    std::printf("[vehicle-lifecycle] solver edge stiffness: rigid %.3f, "
                "separated %.3f, restored %.3f OK\n",
                rigidLength, separatedLength, restoredLength);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_vehicle_seats_json();
    test_beam_seats_json();
    test_seats_validation();
    test_occupant_enter_ride_exit();
    test_occupant_beam_ride();
    test_occupant_validation();
    test_parts_auto_derived();
    test_wheel_damage_reduces_acceleration();
    test_drivetrain_damage_stops_drive();
    test_all_wheels_separated_stall();
    test_beam_damage_increases_deformation();
    test_repair_restores();
    test_damage_determinism();
    test_solver_edge_stiffness();
    if (g_failures == 0) {
        std::printf("[vehicle-lifecycle] ALL PASSED\n");
        return 0;
    }
    std::printf("[vehicle-lifecycle] %d FAILURE(S)\n", g_failures);
    return 1;
}

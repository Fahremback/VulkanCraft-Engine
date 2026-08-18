// BeamVehicleTests.cpp
//
// FALTANTES §17 item 4: "Criar `BeamGraphAsset` XPBD para chassis deformáveis
// no estilo node/beam". A beam chassis is a graph of nodes (mass points)
// connected by beams (distance constraints with PER-BEAM stiffness), solved by
// XPBD (the public IDeformableProvider); wheels are mounted on nodes and the
// suspension/drive forces are applied AT THE NODES, so the chassis BENDS under
// load. These tests prove: the asset JSON round-trips bit-exact, validation is
// all-or-nothing, the chassis deforms under gravity (soft beams sag more than
// stiff), it drives forward and steers, per-beam stiffness actually changes the
// solver behavior (soft vs stiff beam), determinism is bit-exact, and the
// public create_beam_vehicle path works end-to-end.

#include "../src/engine/physics/PhysicsRuntime.hpp"
#include "../src/engine/gameplay/BeamChassisRuntime.hpp"

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IBeamGraphAsset.hpp"

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

bool same_vec3(const glm::vec3& a, const glm::vec3& b) {
    return a == b;
}

// A flat rectangular beam chassis: 4 corner nodes + a center node, perimeter
// and cross beams. The center node carries no wheel, so it SAGS under gravity
// while the corners are held by the suspension — the deformability proof.
// wheelMounts mount at the corners.
engine::vehicles::BeamGraphAsset make_beam_asset(float centerStiffness = 0.5f) {
    engine::vehicles::BeamGraphAsset asset;
    asset.name = "beam-buggy";
    asset.position = {0.0f, 1.4f, 0.0f};
    asset.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    asset.nodes = {
        engine::vehicles::BeamNode{ {-1.2f, 0.0f, -0.8f}, false },  // 0 front-left
        engine::vehicles::BeamNode{ {-1.2f, 0.0f, 0.8f}, false },   // 1 back-left
        engine::vehicles::BeamNode{ {1.2f, 0.0f, -0.8f}, false },   // 2 front-right
        engine::vehicles::BeamNode{ {1.2f, 0.0f, 0.8f}, false },    // 3 back-right
        engine::vehicles::BeamNode{ {0.0f, 0.0f, 0.0f}, false },    // 4 center
    };

    asset.beams = {
        { 0, 1, 0.9f }, { 1, 3, 0.9f }, { 3, 2, 0.9f }, { 2, 0, 0.9f },  // perimeter
        { 0, 4, centerStiffness }, { 4, 3, centerStiffness },              // cross (vary)
    };

    engine::vehicles::BeamWheelMount mount;
    mount.wheel.radius = 0.36f;
    mount.wheel.suspensionRestLength = 0.55f;
    mount.wheel.suspensionTravel = 0.22f;
    mount.wheel.springStrength = 26000.0f;
    mount.wheel.damperStrength = 3200.0f;
    mount.wheel.tireGrip = 1.35f;
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

// Adds a ground plane (the wheels raycast against it) to an existing world.
void add_ground(Engine::Physics::PhysicsRuntime& world) {
    Engine::Physics::BodyDesc ground;
    ground.motion = Engine::Physics::MotionType::Static;
    ground.collider.shape = Engine::Physics::BoxShape{{50.0f, 1.0f, 50.0f}};
    world.create_body(ground);
}

void tick(Engine::Gameplay::BeamChassisRuntime& chassis,
          const Engine::Physics::VehicleInput& input, int ticks) {
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < ticks; ++i) {
        chassis.set_input(input);
        chassis.update(dt);
    }
}

// 1. Asset JSON round-trip: every field survives bit-exact (%.9g).
void test_json_round_trip() {
    engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.42f);
    asset.id = "9e6f5c2a-1d44-4c9b-8e5a-3f0a2b1c4d5e";  // canonical UUID
    asset.nodes[4].fixed = true;
    asset.mass = 1350.0f;
    asset.solver.substeps = 3;
    asset.solver.solverIterations = 16;
    asset.solver.stiffness = 0.85f;
    asset.solver.damping = 0.2f;
    asset.solver.gravity = {0.0f, -12.0f, 0.0f};

    const std::string json = asset.to_json();
    engine::vehicles::BeamGraphAsset parsed;
    std::string error;
    check(parsed.load_from_json(json, error), "json: load_from_json succeeds");
    check(parsed.nodes.size() == 5 && parsed.beams.size() == 6 &&
              parsed.wheels.size() == 4,
          "json: nodes/beams/wheels round-trip");
    for (std::size_t i = 0; i < parsed.nodes.size(); ++i) {
        check(same_vec3(parsed.nodes[i].position, asset.nodes[i].position),
              "json: node position round-trips bit-exact");
        check(parsed.nodes[i].fixed == asset.nodes[i].fixed,
              "json: node fixed flag round-trips");
    }
    check(parsed.beams[4].a == 0 && parsed.beams[4].b == 4 &&
              parsed.beams[4].stiffness == 0.42f,
          "json: beam indices + per-beam stiffness round-trip");
    check(parsed.wheels[0].node == 0 && parsed.wheels[0].driven &&
              parsed.wheels[0].steering,
          "json: wheel mount node/steering/driven round-trip");
    check(parsed.wheels[0].wheel.radius == 0.36f &&
              parsed.wheels[0].wheel.springStrength == 26000.0f,
          "json: wheel geometry round-trips bit-exact");
    check(parsed.solver.substeps == 3 && parsed.solver.solverIterations == 16 &&
              parsed.solver.stiffness == 0.85f && parsed.solver.damping == 0.2f,
          "json: solver config round-trips");
    check(same_vec3(parsed.solver.gravity, asset.solver.gravity),
          "json: solver gravity round-trips");
    check(parsed.id == asset.id, "json: canonical id preserved");
    check(parsed.nodes[4].fixed, "json: fixed node preserved");
    check(parsed.mass == 1350.0f, "json: chassis mass round-trips");
    std::printf("[beam-vehicle] json: round-trip bit-exact OK\n");
}

// 2. Validation: malformed documents are refused all-or-nothing.
void test_validation() {
    engine::vehicles::BeamGraphAsset asset;
    std::string error;
    check(!asset.load_from_json("{\"name\":\"x\",\"nodes\":[],\"beams\":[]}", error),
          "validation: empty nodes refused");
    check(!asset.load_from_json(
              "{\"name\":\"x\",\"nodes\":[{\"position\":[0,0,0]},"
              "{\"position\":[1,0,0]}],\"beams\":[{\"a\":0,\"b\":9}]}",
              error),
          "validation: beam referencing invalid node refused");
    check(!asset.load_from_json(
              "{\"name\":\"x\",\"nodes\":[{\"position\":[0,0,0]},"
              "{\"position\":[1,0,0]}],\"beams\":[{\"a\":0,\"b\":1,"
              "\"stiffness\":1.5}]}",
              error),
          "validation: beam stiffness > 1 refused");
    check(!asset.load_from_json(
              "{\"name\":\"x\",\"nodes\":[{\"position\":[0,0,0]},"
              "{\"position\":[1,0,0]}],\"beams\":[{\"a\":0,\"b\":1}],"
              "\"wheels\":[{\"node\":7,\"wheel\":{\"radius\":0.3}}]}",
              error),
          "validation: wheel mounting invalid node refused");
    check(!asset.load_from_json(
              "{\"name\":\"x\",\"nodes\":[{\"position\":[0,0,0]},"
              "{\"position\":[1,0,0]}],\"beams\":[{\"a\":0,\"b\":1}],"
              "\"solver\":{\"substeps\":0}}",
              error),
          "validation: solver substeps 0 refused");
    std::printf("[beam-vehicle] validation: all-or-nothing OK\n");
}

// 3. Deformability: the chassis BENDS under gravity — the unsupported center
// node sags below its rest position while the corners stay supported.
void test_deforms_under_gravity() {
    engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.5f);
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    add_ground(world);
    std::string error;
    Engine::Gameplay::BeamChassisRuntime chassis(world, asset, error);
    check(chassis.valid(), "chassis built from the asset");
    check(chassis.node_count() == 5, "five nodes in the deformable body");

    Engine::Physics::VehicleInput idle;
    tick(chassis, idle, 120);  // settle: gravity + suspension reach equilibrium

    const float centerY = chassis.node_position(4).y;
    const glm::vec3 restCenter = asset.position + glm::vec3(0.0f, 0.0f, 0.0f);
    check(centerY < restCenter.y - 0.02f,
          "center node sagged below rest (chassis deforms under load)");
    check(chassis.deformation() > 0.01f,
          "deformation observable reports the sag");
    std::printf("[beam-vehicle] deforms: center sagged %.3f m under gravity "
                "(def %.3f) OK\n",
                restCenter.y - centerY, chassis.deformation());
}

// 4. Per-beam stiffness changes the solver: the SAME geometry with soft cross
// beams sags strictly more than with stiff cross beams.
void test_soft_beams_sag_more() {
    auto run = [](float stiffness, float& centerY) {
        engine::vehicles::BeamGraphAsset asset = make_beam_asset(stiffness);
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        add_ground(world);
        std::string error;
        Engine::Gameplay::BeamChassisRuntime chassis(world, asset, error);
        Engine::Physics::VehicleInput idle;
        tick(chassis, idle, 240);
        centerY = chassis.node_position(4).y;
    };

    float softY = 0.0f, stiffY = 0.0f;
    run(0.1f, softY);
    run(0.98f, stiffY);
    check(softY < stiffY - 0.01f,
          "soft cross beams sag strictly more than stiff beams");
    std::printf("[beam-vehicle] stiffness: soft y %.3f < stiff y %.3f OK\n",
                softY, stiffY);
}

// 5. Drives forward: throttle applies drive forces at the mount nodes and the
// deformed chassis advances along -Z (the engine forward convention).
void test_drives_forward() {
    engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.5f);
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    add_ground(world);
    std::string error;
    Engine::Gameplay::BeamChassisRuntime chassis(world, asset, error);
    check(chassis.valid(), "chassis valid");

    Engine::Physics::VehicleInput idle;
    tick(chassis, idle, 120);  // settle on the wheels
    const float startZ = chassis.chassis_position().z;

    Engine::Physics::VehicleInput drive;
    drive.throttle = 1.0f;
    tick(chassis, drive, 90);

    check(chassis.speed(world) > 0.5f, "beam chassis reaches speed > 0.5 m/s");
    check(chassis.chassis_position().z < startZ - 0.2f,
          "beam chassis moved forward (>0.2 m)");
    std::printf("[beam-vehicle] drives: speed %.3f m/s, moved %.3f m OK\n",
                chassis.speed(world), startZ - chassis.chassis_position().z);
}

// 6. Steering turns the chassis: the front wheels steer and the drive pushes
// along the steered direction — the chassis yaw changes.
void test_steering_turns() {
    auto run = [](float steering, float& yaw) {
        engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.5f);
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        add_ground(world);
        std::string error;
        Engine::Gameplay::BeamChassisRuntime chassis(world, asset, error);
        Engine::Physics::VehicleInput idle;
        tick(chassis, idle, 120);
        Engine::Physics::VehicleInput drive;
        drive.throttle = 1.0f;
        drive.steering = steering;
        tick(chassis, drive, 120);
        const glm::vec3 fwd = chassis.chassis_forward();
        yaw = std::atan2(fwd.x, -fwd.z);  // engine forward = -Z
    };

    float yawStraight = 0.0f, yawSteered = 0.0f;
    run(0.0f, yawStraight);
    run(1.0f, yawSteered);
    check(std::abs(yawSteered - yawStraight) > 0.05f,
          "steering turned the beam chassis");
    std::printf("[beam-vehicle] steering: yaw delta %.3f rad OK\n",
                yawSteered - yawStraight);
}

// 7. Determinism: identical asset + identical steps -> bit-identical node
// positions across instances.
void test_determinism() {
    auto run = []() {
        engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.5f);
        Engine::Physics::PhysicsRuntime world(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        add_ground(world);
        std::string error;
        Engine::Gameplay::BeamChassisRuntime chassis(world, asset, error);
        Engine::Physics::VehicleInput drive;
        drive.throttle = 1.0f;
        drive.steering = 0.5f;
        tick(chassis, drive, 90);
        std::vector<glm::vec3> nodes;
        for (std::size_t i = 0; i < chassis.node_count(); ++i) {
            nodes.push_back(chassis.node_position(i));
        }
        return nodes;
    };

    const std::vector<glm::vec3> a = run();
    const std::vector<glm::vec3> b = run();
    bool identical = a.size() == b.size();
    if (identical) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "determinism: bit-identical node positions");
    std::printf("[beam-vehicle] determinism: bit-identical across instances OK\n");
}

// 8. The deformable solver accepts per-edge stiffness (the additive §17 item 4
// extension to IDeformableProvider): mismatched lists are refused.
void test_solver_edge_stiffness() {
    std::string error;
    std::unique_ptr<Engine::Deformable::IDeformableProvider> provider =
        Engine::Deformable::create_deformable_provider(
            Engine::Deformable::DeformableProviderKind::Xpbd,
            Engine::Deformable::DeformableConfig{}, error);
    check(provider != nullptr, "xpbd provider created");

    Engine::Deformable::DeformableMeshDesc desc;
    desc.nodes = { glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                   glm::vec3(0.0f, 2.0f, 0.0f) };
    desc.edges = { { 0, 1 }, { 1, 2 } };
    desc.stiffness = { 0.9f };  // mismatched with 2 edges
    check(provider->create_body(desc, error) ==
              Engine::Deformable::InvalidDeformableBody,
          "per-edge stiffness mismatched with edges refused");

    desc.stiffness = { 0.95f, 0.05f };  // rigid top, soft bottom
    const Engine::Deformable::DeformableBodyHandle body =
        provider->create_body(desc, error);
    check(body != Engine::Deformable::InvalidDeformableBody,
          "valid per-edge stiffness accepted");
    // The soft edge (1,2) under gravity stretches more than the rigid (0,1).
    for (int i = 0; i < 120; ++i) provider->step(1.0f / 60.0f);
    const float d01 = std::abs(glm::length(provider->node_position(body, 1) -
                                           provider->node_position(body, 0)) - 1.0f);
    const float d12 = std::abs(glm::length(provider->node_position(body, 2) -
                                           provider->node_position(body, 1)) - 1.0f);
    check(d12 > d01, "soft beam stretches more than the rigid beam");
    std::printf("[beam-vehicle] solver per-edge stiffness: soft %.4f > rigid "
                "%.4f OK\n",
                d12, d01);
}

// 9. Public path: create_beam_vehicle through IGameplayRuntime assembles the
// XPBD chassis and drives it.
void test_public_assembly() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    check(runtime->physics_backend() == PhysicsBackend::Jolt,
          "runtime uses Jolt backend");

    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    runtime->physics().create_body(ground);

    engine::vehicles::BeamGraphAsset asset = make_beam_asset(0.5f);
    auto vehicle = runtime->create_beam_vehicle(asset);
    check(vehicle != nullptr, "create_beam_vehicle returns a vehicle");
    check(vehicle->valid(), "assembled beam vehicle is valid");
    check(vehicle->node_count() == 5, "five nodes exposed");

    VehicleInput idle;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i) {
        vehicle->set_input(idle);
        vehicle->update(dt);
        runtime->step(dt);
    }
    const float startZ = vehicle->chassis_position().z;
    check(vehicle->deformation() > 0.01f,
          "public chassis deformed under gravity (deformable, not rigid)");

    VehicleInput drive;
    drive.throttle = 1.0f;
    for (int i = 0; i < 90; ++i) {
        vehicle->set_input(drive);
        vehicle->update(dt);
        runtime->step(dt);
    }
    check(vehicle->speed() > 0.5f, "public beam vehicle reaches speed");
    check(vehicle->chassis_position().z < startZ - 0.2f,
          "public beam vehicle moved forward");
    const auto states = vehicle->wheel_states();
    check(states.size() == 4, "four wheel states");
    std::printf("[beam-vehicle] public assembly: speed %.3f m/s, moved %.3f m "
                "OK\n",
                vehicle->speed(), startZ - vehicle->chassis_position().z);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep progress visible on crash
    test_json_round_trip();
    test_validation();
    test_deforms_under_gravity();
    test_soft_beams_sag_more();
    test_drives_forward();
    test_steering_turns();
    test_determinism();
    test_solver_edge_stiffness();
    test_public_assembly();
    if (g_failures == 0) {
        std::printf("[beam-vehicle] ALL PASSED\n");
        return 0;
    }
    std::printf("[beam-vehicle] %d FAILURE(S)\n", g_failures);
    return 1;
}

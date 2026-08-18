// GameplayRuntimeTests.cpp
//
// Public gameplay runtime (FALTANTES item 9 / META section 20): the
// consolidated public surface (engine/gameplay/IGameplayRuntime.hpp) composes
// the internal physics/gameplay runtimes. Each scenario runs TWICE — through
// the public contract and through the internal runtimes directly — and the
// outcomes must match (equivalence gate). The public contract never leaks the
// internal layers; this TU is the only place where both sides meet.

#include <engine/gameplay/IGameplayRuntime.hpp>

#include "../src/engine/physics/PhysicsRuntime.hpp"
#include "../src/engine/physics/Ragdoll.hpp"
#include "../src/engine/gameplay/DestructionRuntime.hpp"
#include "../src/engine/gameplay/VehicleRuntime.hpp"
#include "../src/engine/gameplay/WeaponSystem.hpp"
#include "../src/engine/core/uuid/UUID.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "Failure at line " << __LINE__ << ": " #cond "\n"; \
            return false;                                                    \
        }                                                                    \
    } while (0)

// ---- Destruction ------------------------------------------------------------

bool test_destruction_equivalence() {
    using namespace engine::gameplay;

    // Public contract.
    {
        auto runtime = create_gameplay_runtime();
        CHECK(runtime != nullptr);
        DestructionSpec spec;
        spec.position = { 0.0f, 0.0f, 0.0f };  // mirrors the internal gate
        for (int i = 0; i < 4; ++i) {
            DestructionChunk chunk;
            chunk.localPosition =
                glm::vec3((i % 2 == 0) ? -0.75f : 0.75f,
                          (i < 2) ? -0.75f : 0.75f, 0.0f);
            chunk.halfExtents = { 0.25f, 0.25f, 0.25f };
            chunk.health = 25.0f;
            spec.chunks.push_back(chunk);
        }
        auto destructible = runtime->create_destruction(spec);
        CHECK(destructible != nullptr);
        CHECK(destructible->chunk_count() == 4);
        CHECK(!destructible->fully_destroyed());
        float health = 0.0f;
        CHECK(destructible->chunk_health(0, health) && health == 25.0f);
        for (int i = 0; i < 60; ++i) runtime->step(1.0f / 60.0f);
        const auto events = destructible->apply_radial_damage(
            { 0.0f, 0.0f, 0.0f }, 3.0f, 100.0f, 5.0f);
        CHECK(!events.empty());
        CHECK(destructible->fully_destroyed());
        std::size_t detached = 0;
        for (std::size_t i = 0; i < destructible->chunk_count(); ++i) {
            if (destructible->chunk_detached(i)) ++detached;
            CHECK(destructible->chunk_body(i).valid());
        }
        CHECK(detached == 4);

        // Equivalence: the same scenario through the internal runtime, on the
        // SAME authority (Jolt is the standard-world backend — FALTANTES item 1).
        Engine::Physics::PhysicsRuntime internalWorld(
            {}, Engine::Physics::PhysicsBackendKind::Jolt);
        Engine::Gameplay::DestructibleRuntime internal;
        std::vector<Engine::Gameplay::DestructionChunkDesc> chunks;
        for (int i = 0; i < 4; ++i) {
            Engine::Gameplay::DestructionChunkDesc chunk;
            chunk.localPosition =
                glm::vec3((i % 2 == 0) ? -0.75f : 0.75f,
                          (i < 2) ? -0.75f : 0.75f, 0.0f);
            chunk.halfExtents = { 0.25f, 0.25f, 0.25f };
            chunk.health = 25.0f;
            chunks.push_back(chunk);
        }
        CHECK(internal.create(internalWorld, { 0.0f, 0.0f, 0.0f },
                              glm::quat(1.0f, 0.0f, 0.0f, 0.0f), chunks));
        for (int i = 0; i < 60; ++i) internalWorld.step(1.0f / 60.0f);
        const auto internalEvents = internal.apply_radial_damage(
            internalWorld, { 0.0f, 0.0f, 0.0f }, 3.0f, 100.0f, 5.0f);
        CHECK(!internalEvents.empty());
        CHECK(internal.fully_destroyed());
        CHECK(internalEvents.size() == events.size());  // same detachment
    }
    return true;
}

// ---- Vehicle ----------------------------------------------------------------

bool test_vehicle_equivalence() {
    using namespace engine::gameplay;

    const glm::vec3 wheelLocals[4] = {
        { -1.3f, -0.1f, -0.8f }, { -1.3f, -0.1f, 0.8f },
        { 1.3f, -0.1f, -0.8f },  { 1.3f, -0.1f, 0.8f },
    };
    float publicDriveSpeed = 0.0f;

    // Public contract.
    {
        auto runtime = create_gameplay_runtime();
        CHECK(runtime != nullptr);
        BodySpec ground;
        ground.motion = MotionType::Static;
        ground.shape = BoxShape{ { 50.0f, 1.0f, 50.0f } };
        CHECK(runtime->physics().create_body(ground).valid());
        BodySpec chassisSpec;
        chassisSpec.motion = MotionType::Dynamic;
        chassisSpec.mass = 1200.0f;
        chassisSpec.position = { 0.0f, 1.2f, 0.0f };
        chassisSpec.shape = BoxShape{ { 0.9f, 0.35f, 0.56f } };
        const BodyId chassis = runtime->physics().create_body(chassisSpec);
        CHECK(chassis.valid());

        std::vector<WheelSpec> wheels(4);
        for (int i = 0; i < 4; ++i) {
            wheels[i].localPosition = wheelLocals[i];
            wheels[i].steering = i < 2;
            wheels[i].driven = true;
        }
        auto vehicle = runtime->create_vehicle(chassis, wheels);
        CHECK(vehicle != nullptr);
        CHECK(vehicle->valid());
        CHECK(vehicle->wheel_states().size() == 4);

        VehicleInput idle;
        for (int i = 0; i < 180; ++i) {
            vehicle->set_input(idle);
            vehicle->update(1.0f / 60.0f);
            runtime->step(1.0f / 60.0f);
        }
        CHECK(vehicle->speed() < 1.0f);

        VehicleInput drive;
        drive.throttle = 1.0f;
        for (int i = 0; i < 60; ++i) {
            vehicle->set_input(drive);
            vehicle->update(1.0f / 60.0f);
            runtime->step(1.0f / 60.0f);
        }
        publicDriveSpeed = vehicle->speed();
        CHECK(publicDriveSpeed > 2.0f);

        VehicleInput stop;
        stop.brake = 1.0f;
        for (int i = 0; i < 90; ++i) {
            vehicle->set_input(stop);
            vehicle->update(1.0f / 60.0f);
            runtime->step(1.0f / 60.0f);
        }
        CHECK(vehicle->speed() < publicDriveSpeed);
    }

    // Equivalence: the same scenario through the internal runtime, on the
    // SAME authority (Jolt is the standard-world backend — FALTANTES item 1).
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    Engine::Physics::BodyDesc groundDesc;
    groundDesc.motion = Engine::Physics::MotionType::Static;
    groundDesc.collider.shape =
        Engine::Physics::BoxShape{ { 50.0f, 1.0f, 50.0f } };
    (void)world.create_body(groundDesc);
    Engine::Physics::BodyDesc chassisDesc;
    chassisDesc.motion = Engine::Physics::MotionType::Dynamic;
    chassisDesc.mass = 1200.0f;
    chassisDesc.position = { 0.0f, 1.2f, 0.0f };
    chassisDesc.collider.shape =
        Engine::Physics::BoxShape{ { 0.9f, 0.35f, 0.56f } };
    const auto chassis = world.create_body(chassisDesc);
    CHECK(chassis != Engine::Physics::InvalidBody);
    std::vector<Engine::Gameplay::WheelDesc> wheels(4);
    for (int i = 0; i < 4; ++i) {
        wheels[i].localPosition = wheelLocals[i];
        wheels[i].steering = i < 2;
        wheels[i].driven = true;
    }
    Engine::Gameplay::VehicleRuntime internal(chassis, std::move(wheels));
    CHECK(internal.valid(world));
    Engine::Gameplay::VehicleInput idle;
    for (int i = 0; i < 180; ++i) {
        internal.set_input(idle);
        internal.update(world, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    Engine::Gameplay::VehicleInput drive;
    drive.throttle = 1.0f;
    for (int i = 0; i < 60; ++i) {
        internal.set_input(drive);
        internal.update(world, 1.0f / 60.0f);
        world.step(1.0f / 60.0f);
    }
    const float internalDriveSpeed = internal.speed(world);
    CHECK(internalDriveSpeed > 2.0f);
    // The public surface produced the same forward speed.
    CHECK(std::abs(publicDriveSpeed - internalDriveSpeed) < 0.5f);
    return true;
}

// ---- Weapon -----------------------------------------------------------------

bool test_weapon_equivalence() {
    using namespace engine::gameplay;

    const std::string kRifleId =
        "00000000-0000-0000-0000-000000000063";  // {0, 99}, string form
    const std::string kTargetId =
        "00000000-0000-0000-0000-000000000007";  // {0, 7}, string form
    std::vector<std::uint32_t> publicAmmo;

    // Public contract.
    {
        auto runtime = create_gameplay_runtime();
        CHECK(runtime != nullptr);
        WeaponSpec spec;
        spec.id = kRifleId;
        spec.name = "Rifle";
        spec.fireMode = WeaponSpec::FireMode::Single;
        spec.magazineSize = 5;
        spec.reserveAmmo = 10;
        spec.damage = 25.0f;
        spec.range = 100.0f;
        auto weapon = runtime->create_weapon(spec);
        CHECK(weapon != nullptr);
        weapon->set_raycast(
            [kTargetId](const glm::vec3& o, const glm::vec3& d,
                        float) -> std::optional<WeaponHit> {
                WeaponHit hit;
                hit.entity = kTargetId;
                hit.position = o + d * 10.0f;
                hit.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                hit.distance = 10.0f;
                return hit;
            });
        CHECK(weapon->ammo() == 5);
        CHECK(weapon->reserve() == 10);
        const glm::vec3 origin(0.0f, 0.0f, 0.0f), dir(0.0f, 0.0f, -1.0f);
        CHECK(weapon->trigger_pressed(origin, dir));
        CHECK(weapon->ammo() == 4);
        CHECK(weapon->hits().size() == 1);
        CHECK(weapon->hits().back().entity == kTargetId);  // id round-trips
        CHECK(std::abs(weapon->hits().back().damage - 25.0f) < 1e-4f);
        CHECK(std::abs(weapon->hits().back().distance - 10.0f) < 0.05f);
        publicAmmo.push_back(weapon->ammo());
        weapon->trigger_released();
        CHECK(!weapon->trigger_pressed(origin, dir));  // cooldown blocks
        weapon->update(0.1f, origin, dir);
        CHECK(weapon->trigger_pressed(origin, dir));
        weapon->trigger_released();
        CHECK(weapon->ammo() == 3);
        publicAmmo.push_back(weapon->ammo());
        for (int i = 0; i < 3; ++i) {
            weapon->update(0.11f, origin, dir);
            CHECK(weapon->trigger_pressed(origin, dir));
            weapon->trigger_released();
        }
        CHECK(weapon->ammo() == 0);
        CHECK(!weapon->trigger_pressed(origin, dir));
        CHECK(weapon->reload());
        CHECK(weapon->reloading());
        weapon->update(3.0f, origin, dir);
        CHECK(!weapon->reloading());
        CHECK(weapon->ammo() == 5);
        CHECK(weapon->reserve() == 5);
        publicAmmo.push_back(weapon->ammo());
    }

    // Equivalence: the same scenario through the internal runtime.
    std::vector<std::uint32_t> internalAmmo;
    {
        Engine::WeaponDefinition def;
        def.id = Engine::UUID{ 0, 99 };
        def.name = "Rifle";
        def.fireMode = Engine::FireMode::Single;
        def.magazineSize = 5;
        def.reserveAmmo = 10;
        def.damage = 25.0f;
        def.range = 100.0f;
        Engine::WeaponRuntime weapon(std::move(def));
        weapon.set_raycast(
            [](const glm::vec3& o, const glm::vec3& d,
               float) -> std::optional<Engine::WeaponHit> {
                Engine::WeaponHit hit;
                hit.entity = Engine::UUID{ 0, 7 };
                hit.position = o + d * 10.0f;
                hit.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                hit.distance = 10.0f;
                return hit;
            });
        CHECK(weapon.ammo() == 5);
        const glm::vec3 origin(0.0f, 0.0f, 0.0f), dir(0.0f, 0.0f, -1.0f);
        CHECK(weapon.trigger_pressed(origin, dir));
        CHECK(weapon.ammo() == 4);
        internalAmmo.push_back(weapon.ammo());
        weapon.trigger_released();
        CHECK(!weapon.trigger_pressed(origin, dir));
        weapon.update(0.1f, origin, dir);
        CHECK(weapon.trigger_pressed(origin, dir));
        weapon.trigger_released();
        CHECK(weapon.ammo() == 3);
        internalAmmo.push_back(weapon.ammo());
        for (int i = 0; i < 3; ++i) {
            weapon.update(0.11f, origin, dir);
            CHECK(weapon.trigger_pressed(origin, dir));
            weapon.trigger_released();
        }
        CHECK(weapon.ammo() == 0);
        CHECK(weapon.reload());
        weapon.update(3.0f, origin, dir);
        CHECK(weapon.ammo() == 5);
        CHECK(weapon.reserve() == 5);
        internalAmmo.push_back(weapon.ammo());
    }
    CHECK(publicAmmo == internalAmmo);  // identical ammo sequence
    return true;
}

// ---- Ragdoll ----------------------------------------------------------------

bool test_ragdoll_equivalence() {
    using namespace engine::gameplay;

    // Public contract.
    {
        auto runtime = create_gameplay_runtime();
        CHECK(runtime != nullptr);
        std::vector<RagdollBone> bones;
        RagdollBone spine;
        spine.name = "spine";
        spine.position = { 0.0f, 1.0f, 0.0f };
        spine.length = 0.5f;
        spine.radius = 0.12f;
        spine.mass = 5.0f;
        bones.push_back(spine);
        RagdollBone head;
        head.name = "head";
        head.parent = "spine";
        head.position = { 0.0f, 1.6f, 0.0f };
        head.length = 0.25f;
        head.radius = 0.15f;
        head.mass = 2.0f;
        bones.push_back(head);
        auto ragdoll = runtime->create_ragdoll(bones, { 0.0f, 0.0f, 0.0f });
        CHECK(ragdoll != nullptr);
        CHECK(ragdoll->bone_count() == 2);
        CHECK(ragdoll->bone_body("spine").valid());
        CHECK(ragdoll->bone_body("head").valid());
        CHECK(!ragdoll->bone_body("missing").valid());

        // Impulse on the head changes its velocity through the public physics
        // view.
        const BodyId headBody = ragdoll->bone_body("head");
        BodyState before;
        CHECK(runtime->physics().body_state(headBody, before));
        ragdoll->apply_impulse("head", { 0.0f, 3.0f, 0.0f });
        BodyState after;
        CHECK(runtime->physics().body_state(headBody, after));
        CHECK(after.linearVelocity.y > before.linearVelocity.y + 0.5f);
        CHECK(ragdoll->pose().size() == 2);
    }

    // Equivalence: the internal ragdoll creates the same bone set, on the
    // SAME authority (Jolt is the standard-world backend — FALTANTES item 1).
    Engine::Physics::PhysicsRuntime world(
        {}, Engine::Physics::PhysicsBackendKind::Jolt);
    std::vector<Engine::Physics::RagdollBoneDesc> descs;
    Engine::Physics::RagdollBoneDesc spine;
    spine.name = "spine";
    spine.position = { 0.0f, 1.0f, 0.0f };
    spine.length = 0.5f;
    spine.radius = 0.12f;
    spine.mass = 5.0f;
    descs.push_back(spine);
    Engine::Physics::RagdollBoneDesc head;
    head.name = "head";
    head.parent = "spine";
    head.position = { 0.0f, 1.6f, 0.0f };
    head.length = 0.25f;
    head.radius = 0.15f;
    head.mass = 2.0f;
    descs.push_back(head);
    Engine::Physics::Ragdoll internal;
    CHECK(internal.create(world, descs, { 0.0f, 0.0f, 0.0f }));
    CHECK(internal.bone_body("spine") != Engine::Physics::InvalidBody);
    CHECK(internal.bone_body("head") != Engine::Physics::InvalidBody);
    CHECK(internal.pose(world).size() == 2);
    // FALTANTES item 2: on the standard world (Jolt) the ragdoll joints are
    // real swing-twist constraints, not the handcrafted distance fallback.
    CHECK(internal.uses_swing_twist_joints());
    return true;
}

// ---- Standard-world authority (FALTANTES item 1 / META section 20) --------
// The standard world defaults to Jolt as the single authority for rigid
// bodies/contacts/constraints; Builtin/Bullet remain explicitly selectable.
// Proves the default AND that the public surface drives Jolt correctly for the
// core physics operations (rigid bodies, contacts, raycast, impulse).
bool test_standard_world_backend() {
    using namespace engine::gameplay;

    // Default factory -> Jolt.
    {
        auto runtime = create_gameplay_runtime();
        CHECK(runtime != nullptr);
        CHECK(runtime->physics_backend() == PhysicsBackend::Jolt);
    }
    // Explicit selection still works (Builtin/Bullet remain usable).
    {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Builtin);
        CHECK(runtime != nullptr);
        CHECK(runtime->physics_backend() == PhysicsBackend::Builtin);
    }
    {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Bullet);
        CHECK(runtime != nullptr);
        CHECK(runtime->physics_backend() == PhysicsBackend::Bullet);
    }

    // Jolt as the standard-world authority: a dynamic box dropped on a static
    // floor falls, generates contacts, and rests (rigid body + contact path).
    {
        auto runtime = create_gameplay_runtime();  // Jolt
        CHECK(runtime != nullptr);
        BodySpec floor;
        floor.motion = MotionType::Static;
        floor.position = { 0.0f, -0.5f, 0.0f };
        floor.shape = BoxShape{ { 0.5f, 0.5f, 0.5f } };
        CHECK(runtime->physics().create_body(floor).valid());
        BodySpec box;
        box.motion = MotionType::Dynamic;
        box.position = { 0.0f, 10.0f, 0.0f };
        box.mass = 1.0f;
        box.shape = BoxShape{ { 0.5f, 0.5f, 0.5f } };
        const BodyId falling = runtime->physics().create_body(box);
        CHECK(falling.valid());

        for (int i = 0; i < 180; ++i) runtime->step(1.0f / 60.0f);
        BodyState state;
        CHECK(runtime->physics().body_state(falling, state));
        CHECK(state.position.y > 0.35f && state.position.y < 1.2f);
        CHECK(std::fabs(state.linearVelocity.y) < 0.5f);

        // Raycast through the public surface hits the floor body.
        RaycastHit hit;
        CHECK(runtime->physics().raycast(
            { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f, hit));
        CHECK(hit.body.valid());
        CHECK(hit.distance > 3.5f && hit.distance < 6.0f);

        // Impulse changes the linear velocity through the public surface.
        BodyState before;
        CHECK(runtime->physics().body_state(falling, before));
        runtime->physics().apply_impulse(falling, { 0.0f, 5.0f, 0.0f });
        BodyState after;
        CHECK(runtime->physics().body_state(falling, after));
        CHECK(after.linearVelocity.y > before.linearVelocity.y + 1.0f);
    }
    return true;
}

}  // namespace

int main() {
    if (!test_destruction_equivalence()) return EXIT_FAILURE;
    if (!test_vehicle_equivalence()) return EXIT_FAILURE;
    if (!test_weapon_equivalence()) return EXIT_FAILURE;
    if (!test_ragdoll_equivalence()) return EXIT_FAILURE;
    if (!test_standard_world_backend()) return EXIT_FAILURE;
    std::cout << "GameplayRuntimeTests: public gameplay runtime consolidated "
                 "with equivalence gates OK\n";
    std::cout << "GameplayRuntimeTests: standard world = Jolt (FALTANTES item 1) "
                 "with rigid-body/contact/raycast/impulse battery OK\n";
    return EXIT_SUCCESS;
}

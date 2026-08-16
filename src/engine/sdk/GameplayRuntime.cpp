// GameplayRuntime.cpp — the only translation unit implementing the public
// IGameplayRuntime (META section 20 / FALTANTES item 9). It consolidates the
// engine's internal gameplay runtimes (DestructibleRuntime, VehicleRuntime,
// WeaponRuntime, Ragdoll) behind the self-contained public contract and is the
// ONLY place that crosses from the public surface into those internal layers.
//
// Physics world is the internal PhysicsRuntime (builtin solver by default;
// Jolt/Bullet stay selectable through the internal world). The public surface
// exposes exactly the physics operations the gameplay subsystems need.

#include "engine/gameplay/IGameplayRuntime.hpp"

#include "../physics/PhysicsRuntime.hpp"
#include "../physics/Ragdoll.hpp"
#include "../gameplay/DestructionRuntime.hpp"
#include "../gameplay/VehicleRuntime.hpp"
#include "../gameplay/WeaponSystem.hpp"
#include "../core/uuid/UUID.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace engine {
namespace gameplay {
namespace {

// ---- IPhysicsWorld over the internal runtime -------------------------------

class PhysicsWorldImpl final : public IPhysicsWorld {
public:
    explicit PhysicsWorldImpl(Engine::Physics::PhysicsRuntime& world)
        : world_(world) {}

    BodyId create_body(const BodySpec& spec) override {
        Engine::Physics::BodyDesc desc;
        switch (spec.motion) {
            case MotionType::Static:
                desc.motion = Engine::Physics::MotionType::Static;
                break;
            case MotionType::Dynamic:
                desc.motion = Engine::Physics::MotionType::Dynamic;
                break;
            case MotionType::Kinematic:
                desc.motion = Engine::Physics::MotionType::Kinematic;
                break;
        }
        desc.position = spec.position;
        desc.rotation = spec.rotation;
        desc.linearVelocity = spec.linearVelocity;
        desc.mass = spec.mass;
        desc.continuous = spec.continuous;
        desc.collider.friction = spec.friction;
        desc.collider.restitution = spec.restitution;
        if (const auto* sphere = std::get_if<SphereShape>(&spec.shape)) {
            desc.collider.shape =
                Engine::Physics::SphereShape{ sphere->radius };
        } else if (const auto* box = std::get_if<BoxShape>(&spec.shape)) {
            desc.collider.shape =
                Engine::Physics::BoxShape{ box->halfExtents };
        } else if (const auto* capsule = std::get_if<CapsuleShape>(&spec.shape)) {
            desc.collider.shape = Engine::Physics::CapsuleShape{
                capsule->radius, capsule->halfHeight
            };
        }
        return BodyId{ world_.create_body(desc) };
    }

    bool destroy_body(BodyId body) override {
        return world_.destroy_body(body.id);
    }

    bool body_state(BodyId body, BodyState& out) const override {
        const Engine::Physics::RigidBody* rb = world_.body(body.id);
        if (rb == nullptr) return false;
        out.position = rb->position;
        out.rotation = rb->rotation;
        out.linearVelocity = rb->linearVelocity;
        out.angularVelocity = rb->angularVelocity;
        return true;
    }

    void apply_impulse(BodyId body, const glm::vec3& impulse) override {
        world_.apply_impulse(body.id, impulse);
    }

    void add_force(BodyId body, const glm::vec3& force) override {
        world_.add_force(body.id, force);
    }

    bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                 float maxDistance, RaycastHit& out) const override {
        const auto hit = world_.raycast(origin, direction, maxDistance);
        if (!hit) return false;
        out.body = BodyId{ hit->body };
        out.point = hit->point;
        out.normal = hit->normal;
        out.distance = hit->distance;
        return true;
    }

    void step(float deltaTime) override { world_.step(deltaTime); }

private:
    Engine::Physics::PhysicsRuntime& world_;
};

// ---- IDestruction -----------------------------------------------------------

class DestructionImpl final : public IDestruction {
public:
    DestructionImpl(Engine::Physics::PhysicsRuntime& world,
                    const DestructionSpec& spec) {
        std::vector<Engine::Gameplay::DestructionChunkDesc> chunks;
        chunks.reserve(spec.chunks.size());
        for (const DestructionChunk& chunk : spec.chunks) {
            Engine::Gameplay::DestructionChunkDesc desc;
            desc.localPosition = chunk.localPosition;
            desc.localRotation = chunk.localRotation;
            desc.halfExtents = chunk.halfExtents;
            desc.mass = chunk.mass;
            desc.health = chunk.health;
            desc.damageResistance = chunk.damageResistance;
            chunks.push_back(desc);
        }
        created_ = runtime_.create(world, spec.position, spec.rotation, chunks);
        world_ = &world;
    }

    bool valid() const { return created_; }

    std::size_t chunk_count() const override {
        return runtime_.chunks().size();
    }

    bool fully_destroyed() const override {
        return runtime_.fully_destroyed();
    }

    std::vector<DestructionEvent> apply_radial_damage(
        const glm::vec3& origin, float radius, float damage,
        float impulseStrength) override {
        const auto events =
            runtime_.apply_radial_damage(*world_, origin, radius, damage,
                                         impulseStrength);
        std::vector<DestructionEvent> out;
        out.reserve(events.size());
        for (const auto& event : events) {
            DestructionEvent ev;
            ev.chunkIndex = event.chunkIndex;
            ev.body = BodyId{ event.body };
            ev.position = event.position;
            ev.impulse = event.impulse;
            out.push_back(ev);
        }
        return out;
    }

    bool detach_chunk(std::size_t index,
                      const glm::vec3& impulse) override {
        return runtime_.detach_chunk(*world_, index, impulse);
    }

    bool chunk_health(std::size_t index, float& out) const override {
        const auto& states = runtime_.chunks();
        if (index >= states.size()) return false;
        out = states[index].health;
        return true;
    }

    bool chunk_detached(std::size_t index) const override {
        const auto& states = runtime_.chunks();
        return index < states.size() && states[index].detached;
    }

    BodyId chunk_body(std::size_t index) const override {
        const auto& states = runtime_.chunks();
        return index < states.size() ? BodyId{ states[index].body }
                                     : BodyId{};
    }

private:
    Engine::Gameplay::DestructibleRuntime runtime_;
    Engine::Physics::PhysicsRuntime* world_{ nullptr };
    bool created_{ false };
};

// ---- IVehicle ---------------------------------------------------------------

class VehicleImpl final : public IVehicle {
public:
    VehicleImpl(Engine::Physics::PhysicsRuntime& world, BodyId chassis,
                const std::vector<WheelSpec>& wheels)
        : world_(world), chassis_(chassis.id) {
        std::vector<Engine::Gameplay::WheelDesc> descs;
        descs.reserve(wheels.size());
        for (const WheelSpec& wheel : wheels) {
            Engine::Gameplay::WheelDesc desc;
            desc.localPosition = wheel.localPosition;
            desc.radius = wheel.radius;
            desc.suspensionRestLength = wheel.suspensionRestLength;
            desc.suspensionTravel = wheel.suspensionTravel;
            desc.springStrength = wheel.springStrength;
            desc.damperStrength = wheel.damperStrength;
            desc.tireGrip = wheel.tireGrip;
            desc.maxDriveForce = wheel.maxDriveForce;
            desc.maxBrakeForce = wheel.maxBrakeForce;
            desc.maxSteerAngle = wheel.maxSteerAngle;
            desc.steering = wheel.steering;
            desc.driven = wheel.driven;
            descs.push_back(desc);
        }
        vehicle_ = std::make_unique<Engine::Gameplay::VehicleRuntime>(
            chassis.id, std::move(descs));
    }

    void set_input(const VehicleInput& input) override {
        Engine::Gameplay::VehicleInput in;
        in.throttle = input.throttle;
        in.steering = input.steering;
        in.brake = input.brake;
        in.handbrake = input.handbrake;
        vehicle_->set_input(in);
    }

    void update(float deltaTime) override { vehicle_->update(world_, deltaTime); }

    BodyId chassis() const override { return BodyId{ chassis_ }; }

    std::vector<WheelState> wheel_states() const override {
        const auto& states = vehicle_->wheel_states();
        std::vector<WheelState> out;
        out.reserve(states.size());
        for (const auto& state : states) {
            WheelState ws;
            ws.grounded = state.grounded;
            ws.groundBody = BodyId{ state.groundBody };
            ws.contactPoint = state.contactPoint;
            ws.contactNormal = state.contactNormal;
            ws.suspensionLength = state.suspensionLength;
            ws.compression = state.compression;
            ws.rotation = state.rotation;
            ws.angularSpeed = state.angularSpeed;
            ws.steerAngle = state.steerAngle;
            out.push_back(ws);
        }
        return out;
    }

    float speed() const override { return vehicle_->speed(world_); }
    bool valid() const override { return vehicle_->valid(world_); }

private:
    Engine::Physics::PhysicsRuntime& world_;
    Engine::Physics::BodyHandle chassis_{ 0 };
    std::unique_ptr<Engine::Gameplay::VehicleRuntime> vehicle_;
};

// ---- IWeapon -----------------------------------------------------------------

class WeaponImpl final : public IWeapon {
public:
    explicit WeaponImpl(const WeaponSpec& spec) {
        Engine::WeaponDefinition def;
        def.id = Engine::UUID::from_string(spec.id);
        def.name = spec.name;
        switch (spec.fireMode) {
            case WeaponSpec::FireMode::Single:
                def.fireMode = Engine::FireMode::Single;
                break;
            case WeaponSpec::FireMode::Burst:
                def.fireMode = Engine::FireMode::Burst;
                break;
            case WeaponSpec::FireMode::Automatic:
                def.fireMode = Engine::FireMode::Automatic;
                break;
        }
        def.magazineSize = spec.magazineSize;
        def.reserveAmmo = spec.reserveAmmo;
        def.burstCount = spec.burstCount;
        def.roundsPerMinute = spec.roundsPerMinute;
        def.reloadSeconds = spec.reloadSeconds;
        def.damage = spec.damage;
        def.range = spec.range;
        def.spreadDegrees = spec.spreadDegrees;
        def.hitscan = spec.hitscan;
        weapon_ = std::make_unique<Engine::WeaponRuntime>(std::move(def));
        weapon_->set_raycast(
            [this](const glm::vec3& origin, const glm::vec3& direction,
                   float maxDistance) -> std::optional<Engine::WeaponHit> {
                if (!raycast_) return std::nullopt;
                const auto hit = raycast_(origin, direction, maxDistance);
                if (!hit) return std::nullopt;
                Engine::WeaponHit out;
                out.entity =
                    Engine::UUID::from_string(hit->entity);
                out.position = hit->position;
                out.normal = hit->normal;
                out.distance = hit->distance;
                out.damage = hit->damage;
                return out;
            });
        weapon_->set_projectile_spawn(
            [this](const glm::vec3& origin, const glm::vec3& direction,
                   float speed) {
                if (spawn_) spawn_(origin, direction, speed);
            });
    }

    void set_raycast(RaycastFn callback) override {
        raycast_ = std::move(callback);
    }

    void set_projectile_spawn(
        std::function<void(const glm::vec3&, const glm::vec3&, float)>
            callback) override {
        spawn_ = std::move(callback);
    }

    bool trigger_pressed(const glm::vec3& origin,
                         const glm::vec3& direction) override {
        return weapon_->trigger_pressed(origin, direction);
    }

    void trigger_released() override { weapon_->trigger_released(); }

    void update(float deltaTime, const glm::vec3& origin,
                const glm::vec3& direction) override {
        weapon_->update(deltaTime, origin, direction);
    }

    bool reload() override { return weapon_->reload(); }

    std::uint32_t ammo() const override { return weapon_->ammo(); }
    std::uint32_t reserve() const override { return weapon_->reserve(); }
    bool reloading() const override { return weapon_->reloading(); }

    std::vector<WeaponHit> hits() const override {
        const auto& hits = weapon_->hits();
        std::vector<WeaponHit> out;
        out.reserve(hits.size());
        for (const auto& hit : hits) {
            WeaponHit wh;
            wh.entity = hit.entity.to_string();
            wh.position = hit.position;
            wh.normal = hit.normal;
            wh.distance = hit.distance;
            wh.damage = hit.damage;
            out.push_back(wh);
        }
        return out;
    }

    void clear_hits() override { weapon_->clear_hits(); }

private:
    std::unique_ptr<Engine::WeaponRuntime> weapon_;
    RaycastFn raycast_;
    std::function<void(const glm::vec3&, const glm::vec3&, float)> spawn_;
};

// ---- IRagdoll -----------------------------------------------------------------

class RagdollImpl final : public IRagdoll {
public:
    RagdollImpl(Engine::Physics::PhysicsRuntime& world,
                const std::vector<RagdollBone>& bones,
                const glm::vec3& rootPosition)
        : world_(world) {
        std::vector<Engine::Physics::RagdollBoneDesc> descs;
        descs.reserve(bones.size());
        for (const RagdollBone& bone : bones) {
            Engine::Physics::RagdollBoneDesc desc;
            desc.name = bone.name;
            desc.parent = bone.parent;
            desc.position = bone.position;
            desc.rotation = bone.rotation;
            desc.length = bone.length;
            desc.radius = bone.radius;
            desc.mass = bone.mass;
            descs.push_back(desc);
        }
        created_ = ragdoll_.create(world, descs, rootPosition);
    }

    bool valid() const { return created_; }

    std::size_t bone_count() const override { return pose().size(); }

    void apply_impulse(const std::string& bone,
                       const glm::vec3& impulse) override {
        ragdoll_.apply_impulse(world_, bone, impulse);
    }

    void set_awake(bool awake) override { ragdoll_.set_awake(world_, awake); }

    std::vector<RagdollPoseBone> pose() const override {
        const auto bones = ragdoll_.pose(world_);
        std::vector<RagdollPoseBone> out;
        out.reserve(bones.size());
        for (const auto& bone : bones) {
            RagdollPoseBone pb;
            pb.name = bone.name;
            pb.position = bone.position;
            pb.rotation = bone.rotation;
            out.push_back(pb);
        }
        return out;
    }

    BodyId bone_body(const std::string& bone) const override {
        return BodyId{ ragdoll_.bone_body(bone) };
    }

private:
    Engine::Physics::PhysicsRuntime& world_;
    Engine::Physics::Ragdoll ragdoll_;
    bool created_{ false };
};

// ---- IGameplayRuntime --------------------------------------------------------

class GameplayRuntimeImpl final : public IGameplayRuntime {
public:
    GameplayRuntimeImpl()
        : world_(std::make_unique<Engine::Physics::PhysicsRuntime>()),
          physics_(std::make_unique<PhysicsWorldImpl>(*world_)) {}

    IPhysicsWorld& physics() override { return *physics_; }

    void step(float deltaTime) override { world_->step(deltaTime); }

    std::unique_ptr<IDestruction> create_destruction(
        const DestructionSpec& spec) override {
        auto impl = std::make_unique<DestructionImpl>(*world_, spec);
        if (!impl->valid()) return nullptr;
        return impl;
    }

    std::unique_ptr<IVehicle> create_vehicle(
        BodyId chassis, const std::vector<WheelSpec>& wheels) override {
        auto impl = std::make_unique<VehicleImpl>(*world_, chassis, wheels);
        if (!impl->valid()) return nullptr;
        return impl;
    }

    std::unique_ptr<IWeapon> create_weapon(const WeaponSpec& spec) override {
        return std::make_unique<WeaponImpl>(spec);
    }

    std::unique_ptr<IRagdoll> create_ragdoll(
        const std::vector<RagdollBone>& bones,
        const glm::vec3& rootPosition) override {
        auto impl = std::make_unique<RagdollImpl>(*world_, bones, rootPosition);
        if (!impl->valid()) return nullptr;
        return impl;
    }

private:
    std::unique_ptr<Engine::Physics::PhysicsRuntime> world_;
    std::unique_ptr<PhysicsWorldImpl> physics_;
};

}  // namespace

std::unique_ptr<IGameplayRuntime> create_gameplay_runtime() {
    return std::make_unique<GameplayRuntimeImpl>();
}

}  // namespace gameplay
}  // namespace engine

// GameplayRuntime.cpp — the only translation unit implementing the public
// IGameplayRuntime (META section 20 / FALTANTES item 9). It consolidates the
// engine's internal gameplay runtimes (DestructibleRuntime, VehicleRuntime,
// WeaponRuntime, Ragdoll) behind the self-contained public contract and is the
// ONLY place that crosses from the public surface into those internal layers.
//
// Physics world is the internal PhysicsRuntime. The standard world defaults to
// Jolt as the single authority for rigid bodies/contacts/constraints (FALTANTES
// item 1 / META section 20); Builtin and Bullet remain explicitly selectable
// through the public factory for specialized or fallback use.

#include "engine/gameplay/IGameplayRuntime.hpp"

#include "../physics/PhysicsRuntime.hpp"
#include "../physics/Ragdoll.hpp"
#include "../gameplay/DestructionRuntime.hpp"
#include "../gameplay/VehicleRuntime.hpp"
#include "../gameplay/BeamChassisRuntime.hpp"
#include "../gameplay/WeaponSystem.hpp"
#include "../core/uuid/UUID.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

    void set_transform(BodyId body, const glm::vec3& position,
                       const glm::quat& rotation) override {
        world_.set_transform(body.id, position, rotation);
    }

    void set_velocity(BodyId body, const glm::vec3& linearVelocity,
                      const glm::vec3& angularVelocity) override {
        world_.set_velocity(body.id, linearVelocity, angularVelocity);
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

    // ---- Provider ownership (FALTANTES §17 item 10) ------------------------
    bool claim_provider(BodyId body, const std::string& provider,
                        std::string& errorOut) override {
        if (body.id == 0 || !world_.body(body.id)) {
            errorOut = "provider ownership: unknown body";
            return false;
        }
        if (provider.empty()) {
            errorOut = "provider ownership: provider name must not be empty";
            return false;
        }
        const auto found = claims_.find(body.id);
        if (found != claims_.end()) {
            if (found->second == provider) return true;  // idempotent
            errorOut = "provider ownership: body already simulated by '" +
                       found->second + "'";
            return false;
        }
        claims_.emplace(body.id, provider);
        return true;
    }

    bool release_provider(BodyId body, const std::string& provider) override {
        const auto found = claims_.find(body.id);
        if (found == claims_.end() || found->second != provider) return false;
        claims_.erase(found);
        return true;
    }

    std::string provider_of(BodyId body) const override {
        const auto found = claims_.find(body.id);
        return found != claims_.end() ? found->second : std::string();
    }

private:
    Engine::Physics::PhysicsRuntime& world_;
    std::unordered_map<std::uint32_t, std::string> claims_;
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
            descs.push_back(to_wheel_desc(wheel));
        }
        vehicle_ = std::make_unique<Engine::Gameplay::VehicleRuntime>(
            chassis.id, std::move(descs));
    }

    // Asset assembly (FALTANTES §17 item 2): wheels from the public WheelComponent
    // + the drivetrain override so the asset controls the Jolt engine.
    VehicleImpl(Engine::Physics::PhysicsRuntime& world, BodyId chassis,
                const std::vector<Engine::Gameplay::WheelDesc>& descs,
                const Engine::Gameplay::VehicleDrivetrain& drivetrain)
        : world_(world), chassis_(chassis.id) {
        vehicle_ = std::make_unique<Engine::Gameplay::VehicleRuntime>(
            chassis.id, descs, drivetrain);
    }

    // Asset assembly (FALTANTES §17 item 3): kind + propulsion modules.
    VehicleImpl(Engine::Physics::PhysicsRuntime& world, BodyId chassis,
                const std::vector<Engine::Gameplay::WheelDesc>& descs,
                const Engine::Gameplay::VehicleDrivetrain& drivetrain,
                Engine::Physics::VehicleKind kind,
                std::vector<Engine::Physics::PropulsionModule> propulsion)
        : world_(world), chassis_(chassis.id) {
        vehicle_ = std::make_unique<Engine::Gameplay::VehicleRuntime>(
            chassis.id, descs, drivetrain, kind, std::move(propulsion));
    }

    void set_input(const VehicleInput& input) override {
        Engine::Gameplay::VehicleInput in;
        in.throttle = input.throttle;
        in.steering = input.steering;
        in.brake = input.brake;
        in.handbrake = input.handbrake;
        // The asset's control mapping (FALTANTES §17 item 7) transforms the
        // RAW input — deadzone + sensitivity curve + inversion — before the
        // physics clamp. Identity mapping (defaults) is a no-op.
        vehicle_->set_raw_input(in);
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

    // --- IVehicleOccupants (FALTANTES §17 item 8) ---------------------------
    std::size_t seat_count() const override { return vehicle_->seat_count(); }
    std::string seat_name(std::size_t index) const override {
        return vehicle_->seat(index).name;
    }
    bool seat_occupied(std::size_t index) const override {
        return vehicle_->seat_occupied(index);
    }
    BodyId occupant(std::size_t index) const override {
        return BodyId{ vehicle_->occupant(index) };
    }
    glm::vec3 seat_position(std::size_t index) const override {
        return vehicle_->seat_position(world_, index);
    }
    bool enter(BodyId occupant, std::size_t index, std::string& errorOut) override {
        return vehicle_->enter_occupant(world_, occupant.id, index, errorOut);
    }
    bool exit(std::size_t index, std::string& errorOut) override {
        return vehicle_->exit_occupant(world_, index, errorOut);
    }

    // --- IVehicleDamage (FALTANTES §17 item 9) ------------------------------
    std::size_t part_count() const override { return vehicle_->part_count(); }
    VehiclePartInfo part_info(std::size_t index) const override {
        VehiclePartInfo out;
        const auto& part = vehicle_->part(index);
        out.name = part.name;
        switch (part.kind) {
            case Engine::Physics::VehiclePartKind::Drivetrain:
                out.kind = VehiclePartKind::Drivetrain;
                break;
            case Engine::Physics::VehiclePartKind::Wheel:
                out.kind = VehiclePartKind::Wheel;
                break;
            case Engine::Physics::VehiclePartKind::Beam:
                out.kind = VehiclePartKind::Beam;
                break;
            case Engine::Physics::VehiclePartKind::Chassis: break;
        }
        out.maxHealth = part.maxHealth;
        out.health = part.health;
        out.separated = part.separated;
        out.componentIndex = part.componentIndex;
        return out;
    }
    bool apply_damage(std::size_t index, float amount,
                      std::string& errorOut) override {
        return vehicle_->apply_damage(index, amount, errorOut);
    }
    bool repair(std::size_t index, float amount,
                std::string& errorOut) override {
        return vehicle_->repair(index, amount, errorOut);
    }
    bool is_separated(std::size_t index) const override {
        return vehicle_->is_separated(index);
    }

    // --- IVehiclePower (FALTANTES §17 item 7) -------------------------------
    float fuel_level() const override { return vehicle_->fuel_level(); }
    float charge_level() const override { return vehicle_->charge_level(); }
    bool powered() const override { return vehicle_->powered(); }
    void refuel(float fraction) override { vehicle_->refuel(fraction); }
    void recharge(float fraction) override { vehicle_->recharge(fraction); }

    void set_seats(std::vector<Engine::Physics::VehicleSeat> seats) {
        vehicle_->set_seats(std::move(seats));
    }

    void set_power(const vehicles::VehiclePower& power) {
        vehicle_->set_power(to_power(power));
    }

    void set_raw_input(const VehicleInput& input) {
        Engine::Gameplay::VehicleInput in;
        in.throttle = input.throttle;
        in.steering = input.steering;
        in.brake = input.brake;
        in.handbrake = input.handbrake;
        vehicle_->set_raw_input(in);
    }

private:
    static Engine::Physics::VehiclePower to_power(const vehicles::VehiclePower& power) {
        Engine::Physics::VehiclePower out;
        out.fuel.capacity = power.fuel.capacity;
        out.fuel.initialLevel = power.fuel.initialLevel;
        out.fuel.burnPerSecond = power.fuel.burnPerSecond;
        out.fuel.idleBurnPerSecond = power.fuel.idleBurnPerSecond;
        out.fuel.minLevelToRun = power.fuel.minLevelToRun;
        out.fuelLevel = power.fuel.initialLevel;
        out.energy.capacity = power.energy.capacity;
        out.energy.initialCharge = power.energy.initialCharge;
        out.energy.drawPerSecond = power.energy.drawPerSecond;
        out.energy.regenPerSecond = power.energy.regenPerSecond;
        out.energy.minChargeToRun = power.energy.minChargeToRun;
        out.chargeLevel = power.energy.initialCharge;
        out.controls.throttleDeadzone = power.controls.throttleDeadzone;
        out.controls.throttleSensitivity = power.controls.throttleSensitivity;
        out.controls.throttleInvert = power.controls.throttleInvert;
        out.controls.steeringDeadzone = power.controls.steeringDeadzone;
        out.controls.steeringSensitivity = power.controls.steeringSensitivity;
        out.controls.steeringInvert = power.controls.steeringInvert;
        out.controls.brakeDeadzone = power.controls.brakeDeadzone;
        out.controls.brakeSensitivity = power.controls.brakeSensitivity;
        return out;
    }

    static Engine::Gameplay::WheelDesc to_wheel_desc(const WheelSpec& wheel) {
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
        return desc;
    }

    Engine::Physics::PhysicsRuntime& world_;
    Engine::Physics::BodyHandle chassis_{ 0 };
    std::unique_ptr<Engine::Gameplay::VehicleRuntime> vehicle_;
};

// ---- IBeamVehicle -------------------------------------------------------------
// FALTANTES §17 item 4: the XPBD node/beam deformable chassis over the public
// BeamGraphAsset (nodes + beams with per-beam stiffness + wheel mounts).

class BeamVehicleImpl final : public IBeamVehicle {
public:
    BeamVehicleImpl(Engine::Physics::PhysicsRuntime& world,
                    const vehicles::BeamGraphAsset& asset)
        : world_(world) {
        std::string error;
        chassis_ = std::make_unique<Engine::Gameplay::BeamChassisRuntime>(world, asset, error);
        if (!chassis_->valid()) {
            chassis_.reset();
            return;
        }
        std::vector<Engine::Physics::VehicleSeat> seats;
        seats.reserve(asset.seats.size());
        for (const vehicles::VehicleSeat& seat : asset.seats) {
            Engine::Physics::VehicleSeat internal;
            internal.name = seat.name;
            internal.localPosition = seat.localPosition;
            internal.exitOffset = seat.exitOffset;
            seats.push_back(internal);
        }
        chassis_->set_seats(std::move(seats));
    }

    void set_input(const VehicleInput& input) override {
        if (!chassis_) return;
        Engine::Gameplay::VehicleInput in;
        in.throttle = input.throttle;
        in.steering = input.steering;
        in.brake = input.brake;
        in.handbrake = input.handbrake;
        // The asset's control mapping (FALTANTES §17 item 7) transforms the
        // RAW input — deadzone + sensitivity curve + inversion — before the
        // physics clamp.
        chassis_->set_raw_input(in);
    }

    void update(float deltaTime) override {
        if (!chassis_) return;
        chassis_->update(deltaTime);
    }

    bool valid() const override { return chassis_ != nullptr && chassis_->valid(); }

    std::size_t node_count() const override {
        return chassis_ ? chassis_->node_count() : 0;
    }

    glm::vec3 node_position(std::size_t index) const override {
        return chassis_ ? chassis_->node_position(index) : glm::vec3(0.0f);
    }

    glm::vec3 chassis_position() const override {
        return chassis_ ? chassis_->chassis_position() : glm::vec3(0.0f);
    }

    float speed() const override {
        return chassis_ ? chassis_->speed(world_) : 0.0f;
    }

    float deformation() const override {
        return chassis_ ? chassis_->deformation() : 0.0f;
    }

    std::vector<WheelState> wheel_states() const override {
        std::vector<WheelState> out;
        if (!chassis_) return out;
        const auto& states = chassis_->wheel_states();
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

    // --- IVehicleOccupants (FALTANTES §17 item 8) ---------------------------
    std::size_t seat_count() const override {
        return chassis_ ? chassis_->seat_count() : 0;
    }
    std::string seat_name(std::size_t index) const override {
        return chassis_ ? chassis_->seat(index).name : std::string();
    }
    bool seat_occupied(std::size_t index) const override {
        return chassis_ != nullptr && chassis_->seat_occupied(index);
    }
    BodyId occupant(std::size_t index) const override {
        return BodyId{ chassis_ ? chassis_->occupant(index) : 0 };
    }
    glm::vec3 seat_position(std::size_t index) const override {
        return chassis_ ? chassis_->seat_position(index) : glm::vec3(0.0f);
    }
    bool enter(BodyId occupant, std::size_t index, std::string& errorOut) override {
        return chassis_ != nullptr &&
               chassis_->enter_occupant(world_, occupant.id, index, errorOut);
    }
    bool exit(std::size_t index, std::string& errorOut) override {
        return chassis_ != nullptr && chassis_->exit_occupant(world_, index, errorOut);
    }

    // --- IVehicleDamage (FALTANTES §17 item 9) ------------------------------
    std::size_t part_count() const override {
        return chassis_ ? chassis_->part_count() : 0;
    }
    VehiclePartInfo part_info(std::size_t index) const override {
        VehiclePartInfo out;
        if (!chassis_) return out;
        const auto& part = chassis_->part(index);
        out.name = part.name;
        switch (part.kind) {
            case Engine::Physics::VehiclePartKind::Drivetrain:
                out.kind = VehiclePartKind::Drivetrain;
                break;
            case Engine::Physics::VehiclePartKind::Wheel:
                out.kind = VehiclePartKind::Wheel;
                break;
            case Engine::Physics::VehiclePartKind::Beam:
                out.kind = VehiclePartKind::Beam;
                break;
            case Engine::Physics::VehiclePartKind::Chassis: break;
        }
        out.maxHealth = part.maxHealth;
        out.health = part.health;
        out.separated = part.separated;
        out.componentIndex = part.componentIndex;
        return out;
    }
    bool apply_damage(std::size_t index, float amount,
                      std::string& errorOut) override {
        return chassis_ != nullptr &&
               chassis_->apply_damage(world_, index, amount, errorOut);
    }
    bool repair(std::size_t index, float amount,
                std::string& errorOut) override {
        return chassis_ != nullptr &&
               chassis_->repair(world_, index, amount, errorOut);
    }
    bool is_separated(std::size_t index) const override {
        return chassis_ != nullptr && chassis_->is_separated(index);
    }

    // --- IVehiclePower (FALTANTES §17 item 7) -------------------------------
    float fuel_level() const override {
        return chassis_ ? chassis_->fuel_level() : 1.0f;
    }
    float charge_level() const override {
        return chassis_ ? chassis_->charge_level() : 1.0f;
    }
    bool powered() const override {
        return chassis_ == nullptr || chassis_->powered();
    }
    void refuel(float fraction) override {
        if (chassis_) chassis_->refuel(fraction);
    }
    void recharge(float fraction) override {
        if (chassis_) chassis_->recharge(fraction);
    }

    void set_power(const vehicles::VehiclePower& power) {
        if (chassis_) chassis_->set_power(to_power(power));
    }

    void set_raw_input(const VehicleInput& input) {
        if (!chassis_) return;
        Engine::Gameplay::VehicleInput in;
        in.throttle = input.throttle;
        in.steering = input.steering;
        in.brake = input.brake;
        in.handbrake = input.handbrake;
        chassis_->set_raw_input(in);
    }

private:
    static Engine::Physics::VehiclePower to_power(const vehicles::VehiclePower& power) {
        Engine::Physics::VehiclePower out;
        out.fuel.capacity = power.fuel.capacity;
        out.fuel.initialLevel = power.fuel.initialLevel;
        out.fuel.burnPerSecond = power.fuel.burnPerSecond;
        out.fuel.idleBurnPerSecond = power.fuel.idleBurnPerSecond;
        out.fuel.minLevelToRun = power.fuel.minLevelToRun;
        out.fuelLevel = power.fuel.initialLevel;
        out.energy.capacity = power.energy.capacity;
        out.energy.initialCharge = power.energy.initialCharge;
        out.energy.drawPerSecond = power.energy.drawPerSecond;
        out.energy.regenPerSecond = power.energy.regenPerSecond;
        out.energy.minChargeToRun = power.energy.minChargeToRun;
        out.chargeLevel = power.energy.initialCharge;
        out.controls.throttleDeadzone = power.controls.throttleDeadzone;
        out.controls.throttleSensitivity = power.controls.throttleSensitivity;
        out.controls.throttleInvert = power.controls.throttleInvert;
        out.controls.steeringDeadzone = power.controls.steeringDeadzone;
        out.controls.steeringSensitivity = power.controls.steeringSensitivity;
        out.controls.steeringInvert = power.controls.steeringInvert;
        out.controls.brakeDeadzone = power.controls.brakeDeadzone;
        out.controls.brakeSensitivity = power.controls.brakeSensitivity;
        return out;
    }

    Engine::Physics::PhysicsRuntime& world_;
    std::unique_ptr<Engine::Gameplay::BeamChassisRuntime> chassis_;
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
    explicit GameplayRuntimeImpl(PhysicsBackend backend)
        : world_(std::make_unique<Engine::Physics::PhysicsRuntime>(
              Engine::Physics::WorldSettings{},
              to_internal_backend(backend))),
          physics_(std::make_unique<PhysicsWorldImpl>(*world_)) {}

    PhysicsBackend physics_backend() const override {
        switch (world_->backend_kind()) {
            case Engine::Physics::PhysicsBackendKind::Jolt: return PhysicsBackend::Jolt;
            case Engine::Physics::PhysicsBackendKind::Bullet: return PhysicsBackend::Bullet;
            case Engine::Physics::PhysicsBackendKind::Builtin: break;
        }
        return PhysicsBackend::Builtin;
    }

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
        // Provider ownership (FALTANTES §17 item 10): the vehicle solver
        // claims the chassis — a second vehicle (or any other provider) on
        // the same body is refused. Same-provider re-claim is idempotent for
        // MANUAL claims, but a NEW vehicle factory call always means another
        // simulator, so the chassis must be unclaimed.
        if (!physics_->provider_of(chassis).empty()) return nullptr;
        std::string claimError;
        if (!physics_->claim_provider(chassis, "vehicle:jolt", claimError)) {
            return nullptr;
        }
        auto impl = std::make_unique<VehicleImpl>(*world_, chassis, wheels);
        if (!impl->valid()) return nullptr;
        return impl;
    }

    std::unique_ptr<IBeamVehicle> create_beam_vehicle(
        const vehicles::BeamGraphAsset& asset) override {
        // The XPBD chassis is a DEFORMABLE body solved by the deformable
        // provider — it owns no physics-world BodyId, so there is no claim to
        // make (the entity namespace is the provider's own). Provider
        // exclusivity is enforced on physics-world entities by the claims
        // below (create_vehicle / create_vehicle_from_asset).
        //
        // Provider gate (FALTANTES §17 items 5/6): the asset selects EXACTLY
        // ONE physics provider; a non-vendored plugin (chrono/jsbsim) is
        // refused here with a diagnostic — never a silent fallback. The beam
        // solver itself is the provider (item 10), so only jolt is valid for
        // the current beam runtime.
        std::string providerError;
        auto provider = vehicles::create_vehicle_provider(asset.provider, providerError);
        if (!provider || !provider->available()) {
            return nullptr;
        }
        auto impl = std::make_unique<BeamVehicleImpl>(*world_, asset);
        if (impl && impl->valid()) impl->set_power(asset.power);
        return impl;
    }

    std::unique_ptr<IVehicle> create_vehicle_from_asset(
        const vehicles::VehicleAsset& asset) override {
        std::string error;
        if (!asset.validate(error)) return nullptr;
        // Provider gate (FALTANTES §17 items 5/6): the asset selects EXACTLY
        // ONE physics provider. Only jolt is vendored today; chrono/jsbsim
        // are refused with a diagnostic — never a silent fallback (the
        // provider ownership claim below also names the provider).
        std::string providerError;
        auto provider = vehicles::create_vehicle_provider(asset.provider, providerError);
        if (!provider || !provider->available()) {
            return nullptr;
        }
        // Assemble the chassis body from the public ChassisComponent.
        Engine::Physics::BodyDesc body;
        body.motion = Engine::Physics::MotionType::Dynamic;
        body.position = asset.position;
        body.rotation = glm::normalize(asset.rotation);
        body.mass = asset.chassis.mass;
        body.collider.friction = asset.chassis.friction;
        body.collider.restitution = asset.chassis.restitution;
        switch (asset.chassis.shape) {
            case vehicles::ChassisShape::Sphere:
                body.collider.shape = Engine::Physics::SphereShape{ asset.chassis.radius };
                break;
            case vehicles::ChassisShape::Capsule:
                body.collider.shape = Engine::Physics::CapsuleShape{
                    asset.chassis.radius, asset.chassis.halfHeight };
                break;
            case vehicles::ChassisShape::Box:
                body.collider.shape = Engine::Physics::BoxShape{ asset.chassis.halfExtents };
                break;
        }
        const auto chassis = world_->create_body(body);
        if (chassis == Engine::Physics::InvalidBody) return nullptr;

        std::vector<Engine::Gameplay::WheelDesc> descs;
        descs.reserve(asset.wheels.size());
        for (const vehicles::WheelComponent& wheel : asset.wheels) {
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
        Engine::Gameplay::VehicleDrivetrain drivetrain;
        drivetrain.engineMaxTorque = asset.drivetrain.engineMaxTorque;
        drivetrain.engineMinRPM = asset.drivetrain.engineMinRPM;
        drivetrain.engineMaxRPM = asset.drivetrain.engineMaxRPM;
        drivetrain.differentialRatio = asset.drivetrain.differentialRatio;
        drivetrain.gearRatios = asset.drivetrain.gearRatios;

        Engine::Physics::VehicleKind kind = Engine::Physics::VehicleKind::Wheeled;
        switch (asset.kind) {
            case vehicles::VehicleKind::Motorcycle:
                kind = Engine::Physics::VehicleKind::Motorcycle;
                break;
            case vehicles::VehicleKind::Tracked:
                kind = Engine::Physics::VehicleKind::Tracked;
                break;
            case vehicles::VehicleKind::Wheeled: break;
        }

        std::vector<Engine::Physics::PropulsionModule> modules;
        modules.reserve(asset.propulsion.size());
        for (const vehicles::PropulsionModule& module : asset.propulsion) {
            Engine::Physics::PropulsionModule internal;
            switch (module.kind) {
                case vehicles::PropulsionKind::Wing:
                    internal.kind = Engine::Physics::PropulsionKind::Wing;
                    break;
                case vehicles::PropulsionKind::Buoyancy:
                    internal.kind = Engine::Physics::PropulsionKind::Buoyancy;
                    break;
                case vehicles::PropulsionKind::Thruster: break;
            }
            internal.localPosition = module.localPosition;
            internal.axis = module.axis;
            internal.maxForce = module.maxForce;
            internal.area = module.area;
            internal.liftCoefficient = module.liftCoefficient;
            internal.fluidDensity = module.fluidDensity;
            internal.waterLevel = module.waterLevel;
            modules.push_back(internal);
        }

        std::vector<Engine::Physics::VehicleSeat> seats;
        seats.reserve(asset.seats.size());
        for (const vehicles::VehicleSeat& seat : asset.seats) {
            Engine::Physics::VehicleSeat internal;
            internal.name = seat.name;
            internal.localPosition = seat.localPosition;
            internal.exitOffset = seat.exitOffset;
            seats.push_back(internal);
        }

        auto impl = std::make_unique<VehicleImpl>(
            *world_, BodyId{ chassis }, descs, drivetrain, kind, std::move(modules));
        if (!impl->valid()) return nullptr;
        // The freshly assembled chassis is claimed by the vehicle provider
        // (exclusive — FALTANTES §17 item 10).
        std::string claimError;
        physics_->claim_provider(BodyId{ chassis }, "vehicle:jolt", claimError);
        impl->set_seats(std::move(seats));
        impl->set_power(asset.power);
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
    static Engine::Physics::PhysicsBackendKind to_internal_backend(
        PhysicsBackend backend) {
        switch (backend) {
            case PhysicsBackend::Jolt: return Engine::Physics::PhysicsBackendKind::Jolt;
            case PhysicsBackend::Bullet: return Engine::Physics::PhysicsBackendKind::Bullet;
            case PhysicsBackend::Builtin: break;
        }
        return Engine::Physics::PhysicsBackendKind::Builtin;
    }

    std::unique_ptr<Engine::Physics::PhysicsRuntime> world_;
    std::unique_ptr<PhysicsWorldImpl> physics_;
};

}  // namespace

std::unique_ptr<IGameplayRuntime> create_gameplay_runtime(
    PhysicsBackend backend) {
    return std::make_unique<GameplayRuntimeImpl>(backend);
}

}  // namespace gameplay
}  // namespace engine

#pragma once

// Public gameplay runtime contract (SDK, META section 20 / FALTANTES item 9 —
// destruction, vehicles, weapons/abilities, ragdolls). Consolidates the
// engine's internal gameplay runtimes behind a self-contained public surface:
// one runtime owns a physics world and exposes destruction / vehicle / weapon
// / ragdoll subsystems with deterministic behavior. The only implementation
// lives in src/engine/sdk/GameplayRuntime.cpp (it composes the internal
// physics/gameplay layers; nothing internal leaks through this header).
//
// This header is self-contained (glm only, no renderer details, no Vulkan).

#include "engine/vehicles/IVehicleAsset.hpp"
#include "engine/vehicles/IBeamGraphAsset.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace engine {
namespace gameplay {

// ---- Minimal public physics surface (what the gameplay subsystems need) ----
struct SphereShape {
    float radius{ 0.5f };
};
struct BoxShape {
    glm::vec3 halfExtents{ 0.5f };
};
struct CapsuleShape {
    float radius{ 0.35f };
    float halfHeight{ 0.65f };
};
using Shape = std::variant<SphereShape, BoxShape, CapsuleShape>;

enum class MotionType : std::uint8_t { Static, Dynamic, Kinematic };

struct BodySpec {
    MotionType motion{ MotionType::Dynamic };
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 linearVelocity{ 0.0f };
    float mass{ 1.0f };
    float friction{ 0.55f };
    float restitution{ 0.05f };
    Shape shape{ BoxShape{} };
    bool continuous{ false };
};

struct BodyId {
    std::uint32_t id{ 0 };
    bool valid() const { return id != 0; }
};

struct BodyState {
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 linearVelocity{ 0.0f };
    glm::vec3 angularVelocity{ 0.0f };
};

struct RaycastHit {
    BodyId body;
    glm::vec3 point{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    float distance{ 0.0f };
};

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;

    virtual BodyId create_body(const BodySpec& spec) = 0;
    virtual bool destroy_body(BodyId body) = 0;
    virtual bool body_state(BodyId body, BodyState& out) const = 0;
    // Instantly moves a body (teleport / kinematic drive — the vehicle
    // replication reconcile snaps the predicted chassis to the authoritative
    // pose through this).
    virtual void set_transform(BodyId body, const glm::vec3& position,
                               const glm::quat& rotation) = 0;
    // Instantly sets a body's linear and angular velocity (the vehicle
    // replication reconcile snaps the predicted velocities to the
    // authoritative ones so the corrected prediction does not coast on the
    // stale momentum).
    virtual void set_velocity(BodyId body, const glm::vec3& linearVelocity,
                              const glm::vec3& angularVelocity) = 0;
    virtual void apply_impulse(BodyId body, const glm::vec3& impulse) = 0;
    virtual void add_force(BodyId body, const glm::vec3& force) = 0;
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                         float maxDistance, RaycastHit& out) const = 0;
    // Advances the physics world by deltaTime (deterministic fixed-step solver).
    virtual void step(float deltaTime) = 0;

    // ---- Provider ownership (FALTANTES §17 item 10) ------------------------
    // Each physical entity (body) is simulated by EXACTLY ONE provider at a
    // time — a vehicle solver (Jolt), a deformable chassis (XPBD), a ragdoll,
    // a destruction debris runtime, etc. Claims are all-or-nothing: claiming a
    // body that ANOTHER provider already owns is refused with a diagnostic
    // (the game never silently ends up with two simulators on one entity).
    // The vehicle factories (create_vehicle_from_asset / create_beam_vehicle)
    // claim the chassis automatically; a second vehicle on the same chassis
    // body is refused. Claiming with the SAME provider again is idempotent.
    virtual bool claim_provider(BodyId body, const std::string& provider,
                                std::string& errorOut) = 0;
    // Releases the claim. Only the owning provider may release; returns false
    // for an unclaimed body or a non-owning provider.
    virtual bool release_provider(BodyId body, const std::string& provider) = 0;
    // The provider currently simulating `body` (empty string when unclaimed).
    virtual std::string provider_of(BodyId body) const = 0;
};

// ---- Destruction (FALTANTES item 9 / META section 20) ----------------------
struct DestructionChunk {
    glm::vec3 localPosition{ 0.0f };
    glm::quat localRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 halfExtents{ 0.25f };
    float mass{ 1.0f };
    float health{ 25.0f };
    float damageResistance{ 0.0f };
};

struct DestructionSpec {
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    std::vector<DestructionChunk> chunks;
};

struct DestructionEvent {
    std::size_t chunkIndex{ 0 };
    BodyId body;
    glm::vec3 position{ 0.0f };
    glm::vec3 impulse{ 0.0f };
};

class IDestruction {
public:
    virtual ~IDestruction() = default;

    virtual std::size_t chunk_count() const = 0;
    virtual bool fully_destroyed() const = 0;
    // Detaches chunks within `radius` of `origin`, damaging them and applying
    // an impulse. Returns the detach events.
    virtual std::vector<DestructionEvent> apply_radial_damage(
        const glm::vec3& origin, float radius, float damage,
        float impulseStrength) = 0;
    virtual bool detach_chunk(std::size_t index,
                              const glm::vec3& impulse = glm::vec3(0.0f)) = 0;
    virtual bool chunk_health(std::size_t index, float& out) const = 0;
    virtual bool chunk_detached(std::size_t index) const = 0;
    virtual BodyId chunk_body(std::size_t index) const = 0;
};

// ---- Vehicle (FALTANTES item 9) --------------------------------------------
struct WheelSpec {
    glm::vec3 localPosition{ 0.0f };
    float radius{ 0.36f };
    float suspensionRestLength{ 0.45f };
    float suspensionTravel{ 0.18f };
    float springStrength{ 26000.0f };
    float damperStrength{ 3200.0f };
    float tireGrip{ 1.35f };
    float maxDriveForce{ 4200.0f };
    float maxBrakeForce{ 6000.0f };
    float maxSteerAngle{ 0.55f };
    bool steering{ false };
    bool driven{ true };
};

struct WheelState {
    bool grounded{ false };
    BodyId groundBody;
    glm::vec3 contactPoint{ 0.0f };
    glm::vec3 contactNormal{ 0.0f, 1.0f, 0.0f };
    float suspensionLength{ 0.0f };
    float compression{ 0.0f };
    float rotation{ 0.0f };
    float angularSpeed{ 0.0f };
    float steerAngle{ 0.0f };
};

struct VehicleInput {
    float throttle{ 0.0f };
    float steering{ 0.0f };
    float brake{ 0.0f };
    float handbrake{ 0.0f };
};

// Damage part (FALTANTES §17 item 9): parts are AUTO-DERIVED from the
// vehicle's components — Chassis + Drivetrain for every vehicle, Wheel per
// wheel, Beam per beam (beam chassis only). componentIndex names the wheel /
// beam for the Wheel/Beam kinds (0 for Chassis/Drivetrain).
enum class VehiclePartKind : std::uint8_t { Chassis, Drivetrain, Wheel, Beam };

struct VehiclePartInfo {
    std::string name;
    VehiclePartKind kind{ VehiclePartKind::Chassis };
    float maxHealth{ 100.0f };
    float health{ 100.0f };
    bool separated{ false };        // health reached 0 (part popped off)
    std::size_t componentIndex{ 0 };
};

// FALTANTES §17 item 8: occupants, entry and exit. An occupant is a physics
// body the project creates through IPhysicsWorld::create_body (a driver /
// passenger capsule). enter() attaches it to a seat — the body RIDES the
// vehicle (the runtime drives it to the seat pose every update); exit() flips
// it back to dynamic and spawns it at the seat's world exit offset (the
// occupant stands outside). Every vehicle exposes these methods.
class IVehicleOccupants {
public:
    virtual ~IVehicleOccupants() = default;

    virtual std::size_t seat_count() const = 0;
    virtual std::string seat_name(std::size_t seatIndex) const = 0;
    virtual bool seat_occupied(std::size_t seatIndex) const = 0;
    virtual BodyId occupant(std::size_t seatIndex) const = 0;
    // Current WORLD pose of the seat (chassis transform * local position; the
    // deformed node frame for a beam chassis).
    virtual glm::vec3 seat_position(std::size_t seatIndex) const = 0;
    // Attaches `occupant` to seat `seatIndex` (kinematic ride). Fails (with a
    // diagnostic) for an out-of-range/taken seat or a non-dynamic body.
    virtual bool enter(BodyId occupant, std::size_t seatIndex,
                       std::string& errorOut) = 0;
    // Spawns the occupant at the seat's exit offset, dynamic again. Fails if
    // the seat is empty. The caller reads the body's new state via
    // IPhysicsWorld::body_state.
    virtual bool exit(std::size_t seatIndex, std::string& errorOut) = 0;
};

// FALTANTES §17 item 9: damage and part separation. Parts are auto-derived
// from the vehicle's components; damage degrades their behavior (a damaged
// wheel loses grip/drive, a damaged drivetrain loses torque, a damaged beam
// degrades the deformable chassis stiffness) and at 0 health a part
// SEPARATES (a wheel pops off — stops contributing; a beam deactivates — no
// longer holds the mesh together). Deterministic and data-free (no JSON).
class IVehicleDamage {
public:
    virtual ~IVehicleDamage() = default;

    virtual std::size_t part_count() const = 0;
    virtual VehiclePartInfo part_info(std::size_t partIndex) const = 0;
    // Damages the part by `amount` (> 0), clamping at 0 and separating it.
    // Returns false (with a diagnostic) for an out-of-range part or a
    // non-positive amount.
    virtual bool apply_damage(std::size_t partIndex, float amount,
                              std::string& errorOut) = 0;
    virtual bool repair(std::size_t partIndex, float amount,
                        std::string& errorOut) = 0;
    virtual bool is_separated(std::size_t partIndex) const = 0;
};

// FALTANTES §17 item 7: energy, fuel and data-driven controls. The vehicle
// carries a fuel tank (burned by throttle) and a battery (drawn by throttle,
// regenerated by braking) — each ENABLED system (capacity > 0) cuts the drive
// when its level falls below the configured threshold, so the vehicle coasts
// to a stop on empty. The control mapping transforms the RAW input (deadzone
// + sensitivity curve + inversion) before it reaches the physics. Levels are
// 0..1 fractions of capacity (1 when the system is disabled).
class IVehiclePower {
public:
    virtual ~IVehiclePower() = default;

    virtual float fuel_level() const = 0;
    virtual float charge_level() const = 0;
    // true when every ENABLED system is above its cut-out threshold.
    virtual bool powered() const = 0;
    // Adds `fraction` * capacity to the live level (clamped; no-op when the
    // system is disabled).
    virtual void refuel(float fraction) = 0;
    virtual void recharge(float fraction) = 0;
};

class IVehicle : public IVehicleOccupants, public IVehicleDamage,
                 public IVehiclePower {
public:
    virtual ~IVehicle() = default;

    virtual void set_input(const VehicleInput& input) = 0;
    // Applies suspension/drive/brake impulses for this step (raycast against
    // the runtime's physics world).
    virtual void update(float deltaTime) = 0;
    virtual BodyId chassis() const = 0;
    virtual std::vector<WheelState> wheel_states() const = 0;
    virtual float speed() const = 0;
    virtual bool valid() const = 0;
};

// ---- Beam chassis (FALTANTES §17 item 4) -----------------------------------
// A DEFORMABLE node/beam vehicle chassis: nodes connected by beams with
// per-beam stiffness, wheels mounted on nodes, solved by XPBD. The chassis
// bends under load (node positions are exposed).
class IBeamVehicle : public IVehicleOccupants, public IVehicleDamage,
                     public IVehiclePower {
public:
    virtual ~IBeamVehicle() = default;

    virtual void set_input(const VehicleInput& input) = 0;
    // Applies wheel suspension/drive/brake forces to the mount nodes, steps
    // the XPBD solver, and reads the deformed node state.
    virtual void update(float deltaTime) = 0;
    virtual bool valid() const = 0;
    virtual std::size_t node_count() const = 0;
    virtual glm::vec3 node_position(std::size_t index) const = 0;
    virtual glm::vec3 chassis_position() const = 0;
    virtual float speed() const = 0;
    // Max |node - rest| over the free nodes: the deformability observable.
    virtual float deformation() const = 0;
    virtual std::vector<WheelState> wheel_states() const = 0;
};

// ---- Weapon / abilities (FALTANTES item 9) ---------------------------------
struct WeaponSpec {
    std::string id;      // stable project id (mapped to a UUID by the runtime)
    std::string name;
    enum class FireMode : std::uint8_t { Single, Burst, Automatic };
    FireMode fireMode{ FireMode::Single };
    std::uint32_t magazineSize{ 30 };
    std::uint32_t reserveAmmo{ 90 };
    std::uint32_t burstCount{ 3 };
    float roundsPerMinute{ 600.0f };
    float reloadSeconds{ 2.0f };
    float damage{ 20.0f };
    float range{ 100.0f };
    float spreadDegrees{ 1.0f };
    bool hitscan{ true };
};

struct WeaponHit {
    std::string entity;  // stable entity id (string form of the UUID)
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    float distance{ 0.0f };
    float damage{ 0.0f };
};

class IWeapon {
public:
    virtual ~IWeapon() = default;

    // The project resolves hits (its own raycast into the world/scene).
    using RaycastFn = std::function<std::optional<WeaponHit>(
        const glm::vec3& origin, const glm::vec3& direction, float maxDistance)>;
    virtual void set_raycast(RaycastFn callback) = 0;
    virtual void set_projectile_spawn(
        std::function<void(const glm::vec3&, const glm::vec3&, float)> callback) = 0;

    virtual bool trigger_pressed(const glm::vec3& origin,
                                 const glm::vec3& direction) = 0;
    virtual void trigger_released() = 0;
    virtual void update(float deltaTime, const glm::vec3& origin,
                        const glm::vec3& direction) = 0;
    virtual bool reload() = 0;
    virtual std::uint32_t ammo() const = 0;
    virtual std::uint32_t reserve() const = 0;
    virtual bool reloading() const = 0;
    virtual std::vector<WeaponHit> hits() const = 0;
    virtual void clear_hits() = 0;
};

// ---- Ragdoll (FALTANTES item 9 / META section 20) --------------------------
struct RagdollBone {
    std::string name;
    std::string parent;
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    float length{ 0.5f };
    float radius{ 0.12f };
    float mass{ 1.0f };
};

struct RagdollPoseBone {
    std::string name;
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
};

class IRagdoll {
public:
    virtual ~IRagdoll() = default;

    virtual std::size_t bone_count() const = 0;
    virtual void apply_impulse(const std::string& bone,
                               const glm::vec3& impulse) = 0;
    virtual void set_awake(bool awake) = 0;
    virtual std::vector<RagdollPoseBone> pose() const = 0;
    virtual BodyId bone_body(const std::string& bone) const = 0;
};

// ---- Runtime (owns the physics world) --------------------------------------
// The physics backend of the standard world. Jolt is the production authority
// (FALTANTES item 1 / META section 20); Builtin and Bullet remain selectable
// for specialized or fallback use.
enum class PhysicsBackend : std::uint8_t { Jolt, Builtin, Bullet };

class IGameplayRuntime {
public:
    virtual ~IGameplayRuntime() = default;

    // The backend this runtime actually advances (matches the factory argument;
    // "jolt" for the default factory call).
    virtual PhysicsBackend physics_backend() const = 0;

    virtual IPhysicsWorld& physics() = 0;
    virtual void step(float deltaTime) = 0;

    virtual std::unique_ptr<IDestruction> create_destruction(
        const DestructionSpec& spec) = 0;
    virtual std::unique_ptr<IVehicle> create_vehicle(
        BodyId chassis, const std::vector<WheelSpec>& wheels) = 0;
    // FALTANTES §17 item 2: assembles a vehicle from a VehicleAsset (the
    // public composable components). The chassis body is created from
    // asset.chassis at asset.position/rotation; the returned IVehicle::chassis()
    // is that body (owned by the runtime's physics world).
    virtual std::unique_ptr<IVehicle> create_vehicle_from_asset(
        const vehicles::VehicleAsset& asset) = 0;
    // FALTANTES §17 item 4: assembles a DEFORMABLE node/beam chassis from a
    // BeamGraphAsset (nodes + beams with per-beam stiffness + wheel mounts),
    // solved by XPBD. The returned vehicle exposes the deformed node
    // positions; the chassis bends under load instead of staying rigid.
    virtual std::unique_ptr<IBeamVehicle> create_beam_vehicle(
        const vehicles::BeamGraphAsset& asset) = 0;
    virtual std::unique_ptr<IWeapon> create_weapon(const WeaponSpec& spec) = 0;
    virtual std::unique_ptr<IRagdoll> create_ragdoll(
        const std::vector<RagdollBone>& bones, const glm::vec3& rootPosition) = 0;
};

// The only implementation of IGameplayRuntime (src/engine/sdk/GameplayRuntime.cpp).
// The standard world defaults to Jolt as the single physics authority.
std::unique_ptr<IGameplayRuntime> create_gameplay_runtime(
    PhysicsBackend backend = PhysicsBackend::Jolt);

}  // namespace gameplay
}  // namespace engine

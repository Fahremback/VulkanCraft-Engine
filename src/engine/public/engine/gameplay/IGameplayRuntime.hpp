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
    virtual void apply_impulse(BodyId body, const glm::vec3& impulse) = 0;
    virtual void add_force(BodyId body, const glm::vec3& force) = 0;
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                         float maxDistance, RaycastHit& out) const = 0;
    // Advances the physics world by deltaTime (deterministic fixed-step solver).
    virtual void step(float deltaTime) = 0;
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

class IVehicle {
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

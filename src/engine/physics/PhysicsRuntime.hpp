#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Engine::Physics {

// Production physics backends selectable per world. Builtin is the internal
// solver; Jolt and Bullet are real external libraries (see PhysicsBackend.hpp).
enum class PhysicsBackendKind : std::uint8_t { Builtin, Jolt, Bullet };

class PhysicsBackend;

using BodyHandle = std::uint32_t;
using ConstraintHandle = std::uint32_t;
inline constexpr BodyHandle InvalidBody = 0;
inline constexpr ConstraintHandle InvalidConstraint = 0;

struct SphereShape { float radius{0.5f}; };
struct BoxShape { glm::vec3 halfExtents{0.5f}; };
struct CapsuleShape { float radius{0.35f}; float halfHeight{0.65f}; };
using ColliderShape = std::variant<SphereShape, BoxShape, CapsuleShape>;

enum class MotionType : std::uint8_t { Static, Dynamic, Kinematic };

struct CollisionFilter {
    std::uint32_t layer{1u};
    std::uint32_t mask{~0u};
    bool allows(const CollisionFilter& other) const noexcept {
        return (mask & other.layer) != 0u && (other.mask & layer) != 0u;
    }
};

struct Collider {
    ColliderShape shape{BoxShape{}};
    glm::vec3 localPosition{0.0f};
    glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float friction{0.55f};
    float restitution{0.05f};
    bool trigger{false};
    CollisionFilter filter{};
};

struct BodyDesc {
    MotionType motion{MotionType::Dynamic};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
    float mass{1.0f};
    float linearDamping{0.04f};
    float angularDamping{0.08f};
    float gravityScale{1.0f};
    bool continuous{false};
    bool allowSleep{true};
    Collider collider{};
    std::uint64_t userData{0};
};

struct RigidBody {
    BodyHandle handle{InvalidBody};
    MotionType motion{MotionType::Dynamic};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 previousPosition{0.0f};
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
    glm::vec3 accumulatedForce{0.0f};
    glm::vec3 accumulatedTorque{0.0f};
    float inverseMass{1.0f};
    float linearDamping{0.04f};
    float angularDamping{0.08f};
    float gravityScale{1.0f};
    float sleepTimer{0.0f};
    bool continuous{false};
    bool allowSleep{true};
    bool sleeping{false};
    Collider collider{};
    std::uint64_t userData{0};

    bool dynamic() const noexcept { return motion == MotionType::Dynamic; }
};

struct Aabb {
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
    bool overlaps(const Aabb& other) const noexcept;
    bool contains(const glm::vec3& point) const noexcept;
};

struct Contact {
    BodyHandle bodyA{InvalidBody};
    BodyHandle bodyB{InvalidBody};
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; // A toward B
    float penetration{0.0f};
    float restitution{0.0f};
    float friction{0.0f};
};

struct TriggerEvent {
    enum class Type : std::uint8_t { Enter, Stay, Exit };
    Type type{Type::Enter};
    BodyHandle trigger{InvalidBody};
    BodyHandle other{InvalidBody};
};

struct RaycastHit {
    BodyHandle body{InvalidBody};
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance{0.0f};
};

struct DebugLine {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    glm::vec4 color{1.0f};
};

struct DistanceConstraintDesc {
    BodyHandle bodyA{InvalidBody};
    BodyHandle bodyB{InvalidBody};
    glm::vec3 localAnchorA{0.0f};
    glm::vec3 localAnchorB{0.0f};
    float restLength{1.0f};
    float stiffness{0.8f};
    float damping{0.15f};
    float breakImpulse{0.0f};
};

struct DistanceConstraint : DistanceConstraintDesc {
    ConstraintHandle handle{InvalidConstraint};
    float lastImpulse{0.0f};
    bool broken{false};
};

struct WorldSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    std::uint32_t solverIterations{8};
    std::uint32_t maxCcdSubsteps{8};
    float fixedStep{1.0f / 60.0f};
    float sleepLinearThreshold{0.06f};
    float sleepAngularThreshold{0.08f};
    float sleepDelay{0.75f};
    float contactSlop{0.005f};
};

class PhysicsRuntime final {
public:
    explicit PhysicsRuntime(WorldSettings settings = {}, PhysicsBackendKind backend = PhysicsBackendKind::Builtin);
    ~PhysicsRuntime();

    bool uses_external_backend() const noexcept { return external_ != nullptr; }
    PhysicsBackendKind backend_kind() const noexcept { return backendKind_; }
    const char* backend_name() const noexcept;

    BodyHandle create_body(const BodyDesc& description);
    bool destroy_body(BodyHandle body);
    RigidBody* body(BodyHandle handle);
    const RigidBody* body(BodyHandle handle) const;

    ConstraintHandle create_distance_constraint(const DistanceConstraintDesc& description);
    bool destroy_constraint(ConstraintHandle constraint);

    void add_force(BodyHandle body, const glm::vec3& force);
    void add_torque(BodyHandle body, const glm::vec3& torque);
    void apply_impulse(BodyHandle body, const glm::vec3& impulse);
    void apply_impulse_at_point(BodyHandle body, const glm::vec3& impulse, const glm::vec3& worldPoint);
    void wake(BodyHandle body);

    void step(float deltaTime);
    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction,
                                      float maxDistance, std::uint32_t layerMask = ~0u,
                                      BodyHandle ignoredBody = InvalidBody) const;
    std::vector<BodyHandle> overlap_aabb(const Aabb& bounds, std::uint32_t layerMask = ~0u) const;

    const std::vector<Contact>& contacts() const noexcept { return contacts_; }
    const std::vector<TriggerEvent>& trigger_events() const noexcept { return triggerEvents_; }
    std::vector<DebugLine> debug_geometry(bool includeSleeping = true) const;
    const WorldSettings& settings() const noexcept { return settings_; }
    void set_settings(const WorldSettings& settings);

private:
    struct Pair { BodyHandle a; BodyHandle b; };
    struct TriggerPair { BodyHandle trigger; BodyHandle other; };

    Aabb compute_aabb(const RigidBody& body, bool swept = false) const;
    std::vector<Pair> broadphase() const;
    std::optional<Contact> generate_contact(const RigidBody& a, const RigidBody& b) const;
    void integrate(float dt);
    void solve_constraints(float dt);
    void solve_contacts(float dt);
    void update_sleep(float dt);
    void update_triggers(const std::vector<TriggerPair>& current);
    void step_substep(float dt);

    BodyHandle to_engine_handle(BodyHandle backendHandle) const;
    BodyHandle to_backend_handle(BodyHandle engineHandle) const;

    WorldSettings settings_{};
    PhysicsBackendKind backendKind_{PhysicsBackendKind::Builtin};
    std::unique_ptr<PhysicsBackend> external_;
    std::vector<std::optional<RigidBody>> bodies_{1};
    std::vector<std::optional<DistanceConstraint>> constraints_{1};
    std::vector<BodyHandle> freeBodies_;
    std::vector<ConstraintHandle> freeConstraints_;
    std::vector<BodyHandle> backendBodies_;      // engine handle -> backend handle (external only)
    std::vector<ConstraintHandle> backendConstraints_;
    std::unordered_map<BodyHandle, BodyHandle> backendToEngine_;
    std::vector<Contact> contacts_;
    std::vector<TriggerPair> triggerPairs_;
    std::vector<TriggerEvent> triggerEvents_;
    float accumulator_{0.0f};
};

} // namespace Engine::Physics

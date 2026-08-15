#pragma once
// Advanced physics: convex hull, triangle mesh, hinge/slider joints,
// multi-threaded island solver, robust stacking & friction.
#include "PhysicsRuntime.hpp"
#include <span>
#include <array>
#include <functional>
#include <memory>
#include <thread>

namespace Engine::Physics {

// ---------- Convex Hull ----------
struct ConvexHull {
    std::vector<glm::vec3> vertices;
    std::vector<std::array<uint32_t,3>> faces;
    std::vector<glm::vec3> faceNormals;
    glm::vec3 centroid{0.0f};
    static ConvexHull from_points(std::span<const glm::vec3> cloud);
    Aabb aabb() const;
};

// ---------- Triangle Mesh ----------
struct TriangleMesh {
    std::vector<glm::vec3> vertices;
    std::vector<std::array<uint32_t,3>> triangles;
    Aabb aabb() const;
};

// New collision shapes
struct ConvexHullShape { std::shared_ptr<ConvexHull> hull; };
struct TriangleMeshShape { std::shared_ptr<TriangleMesh> mesh; };

using ExtendedColliderShape = std::variant<SphereShape, BoxShape, CapsuleShape,
                                            ConvexHullShape, TriangleMeshShape>;

// ---------- Joints ----------
enum class JointType : uint8_t { Distance, Hinge, Slider, BallSocket, Fixed };

struct HingeJointDesc {
    BodyHandle bodyA{InvalidBody};
    BodyHandle bodyB{InvalidBody};
    glm::vec3 anchorA{0.0f};
    glm::vec3 anchorB{0.0f};
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float lowerLimit{-3.14159f};
    float upperLimit{3.14159f};
    bool enableLimits{false};
    float motorSpeed{0.0f};
    float maxMotorTorque{0.0f};
    bool enableMotor{false};
    float breakTorque{0.0f};
};

struct SliderJointDesc {
    BodyHandle bodyA{InvalidBody};
    BodyHandle bodyB{InvalidBody};
    glm::vec3 axis{1.0f, 0.0f, 0.0f};
    float lowerLimit{-1.0f};
    float upperLimit{1.0f};
    bool enableLimits{true};
    float breakForce{0.0f};
};

struct BallSocketJointDesc {
    BodyHandle bodyA{InvalidBody};
    BodyHandle bodyB{InvalidBody};
    glm::vec3 anchorA{0.0f};
    glm::vec3 anchorB{0.0f};
    float coneAngle{0.0f}; // 0 = no limit
    float breakForce{0.0f};
};

struct FixedJointDesc {
    BodyHandle bodyA{InvalidBody};
    BodyHandle bodyB{InvalidBody};
    glm::vec3 anchorA{0.0f};
    glm::vec3 anchorB{0.0f};
    float breakForce{0.0f};
};

using JointDesc = std::variant<DistanceConstraintDesc, HingeJointDesc,
                                SliderJointDesc, BallSocketJointDesc, FixedJointDesc>;

using JointHandle = uint32_t;
inline constexpr JointHandle InvalidJoint = 0;

struct Joint {
    JointHandle handle{InvalidJoint};
    JointType type{JointType::Distance};
    JointDesc desc;
    float accumulatedImpulse{0.0f};
    bool broken{false};
};

// ---------- Island ----------
struct Island {
    std::vector<BodyHandle> bodies;
    std::vector<uint32_t> contactIndices;
    std::vector<JointHandle> joints;
    bool sleeping{false};
};

// ---------- Advanced Physics World ----------
class PhysicsWorld final {
public:
    explicit PhysicsWorld(WorldSettings settings = {});

    // Bodies
    BodyHandle create_body(const BodyDesc& desc);
    bool destroy_body(BodyHandle h);
    RigidBody* body(BodyHandle h);
    const RigidBody* body(BodyHandle h) const;

    // Joints
    JointHandle create_joint(JointDesc desc);
    bool destroy_joint(JointHandle h);
    const Joint* joint(JointHandle h) const;

    // Forces
    void add_force(BodyHandle h, const glm::vec3& force);
    void add_torque(BodyHandle h, const glm::vec3& torque);
    void apply_impulse(BodyHandle h, const glm::vec3& impulse);
    void apply_impulse_at_point(BodyHandle h, const glm::vec3& impulse, const glm::vec3& worldPoint);
    void wake(BodyHandle h);

    // Simulation
    void step(float deltaTime);

    // Queries
    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction,
                                       float maxDistance, uint32_t layerMask = ~0u,
                                       BodyHandle ignore = InvalidBody) const;
    std::vector<BodyHandle> overlap_aabb(const Aabb& bounds, uint32_t layerMask = ~0u) const;
    std::vector<BodyHandle> overlap_sphere(const glm::vec3& center, float radius,
                                            uint32_t layerMask = ~0u) const;

    // Results
    const std::vector<Contact>& contacts() const noexcept { return contacts_; }
    const std::vector<TriggerEvent>& trigger_events() const noexcept { return triggerEvents_; }
    std::vector<DebugLine> debug_geometry(bool includeSleeping = true) const;

    // Settings
    const WorldSettings& settings() const noexcept { return settings_; }
    void set_settings(const WorldSettings& s) { settings_ = s; }

    // Threading
    void set_thread_count(uint32_t count);

    // Scene integration
    void sync_from_scene(const std::unordered_map<uint64_t, BodyHandle>& mapping,
                          const std::function<glm::vec3(uint64_t)>& getPosition,
                          const std::function<glm::quat(uint64_t)>& getRotation);
    void sync_to_scene(const std::unordered_map<uint64_t, BodyHandle>& mapping,
                        const std::function<void(uint64_t, const glm::vec3&, const glm::quat&)>& setTransform) const;

private:
    // Broadphase
    struct BroadPair { BodyHandle a; BodyHandle b; };
    struct TriggerPair { BodyHandle trigger; BodyHandle other; };
    std::vector<BroadPair> broadphase() const;

    // Collision
    std::optional<Contact> narrow_phase(const RigidBody& a, const RigidBody& b) const;
    std::optional<Contact> collide_convex_convex(const ConvexHull& a, const glm::vec3& posA,
                                                   const ConvexHull& b, const glm::vec3& posB) const;
    void update_triggers(const std::vector<TriggerPair>& current);
    [[nodiscard]] Aabb compute_aabb(const RigidBody& body, bool swept = false) const;

    // Islands
    std::vector<Island> build_islands() const;
    void solve_island(Island& island, float dt);

    // Integration & solving
    void integrate(float dt);
    void solve_contacts_pgs(float dt);
    void solve_joints(float dt);
    void update_sleep(float dt);
    void step_substep(float dt);

    // State
    WorldSettings settings_{};
    std::vector<std::optional<RigidBody>> bodies_{1};
    std::vector<std::optional<Joint>> joints_{1};
    std::vector<BodyHandle> freeBodies_;
    std::vector<JointHandle> freeJoints_;
    std::vector<Contact> contacts_;
    std::vector<TriggerPair> triggerPairs_;
    std::vector<TriggerEvent> triggerEvents_;
    float accumulator_{0.0f};
    uint32_t threadCount_{1};
    std::vector<std::thread> workers_;
};

} // namespace Engine::Physics

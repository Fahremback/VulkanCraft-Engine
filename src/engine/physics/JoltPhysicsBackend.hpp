#pragma once

#include "PhysicsBackend.hpp"

namespace Engine::Physics {

// Production backend backed by the real Jolt Physics library (third_party/jolt).
// Collision filtering honors the engine CollisionFilter semantics by mapping
// each unique (layer, mask) combo to a Jolt object layer.
class JoltPhysicsBackend final : public PhysicsBackend {
public:
    explicit JoltPhysicsBackend(const WorldSettings& settings);
    ~JoltPhysicsBackend() override;

    const char* name() const noexcept override { return "jolt"; }

    void set_gravity(const glm::vec3& gravity) override;
    void step(float deltaTime) override;

    BodyHandle create_body(const BodyDesc& description) override;
    bool destroy_body(BodyHandle body) override;
    bool set_motion_type(BodyHandle body, MotionType motion, float mass) override;

    void set_transform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) override;
    bool get_state(BodyHandle body, glm::vec3& position, glm::quat& rotation,
                   glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool& sleeping) const override;
    void set_linear_velocity(BodyHandle body, const glm::vec3& velocity) override;
    void set_velocity(BodyHandle body, const glm::vec3& linearVelocity,
                      const glm::vec3& angularVelocity) override;
    void set_gravity_scale(BodyHandle body, float scale) override;
    void set_linear_damping(BodyHandle body, float damping) override;

    void add_force(BodyHandle body, const glm::vec3& force) override;
    void add_torque(BodyHandle body, const glm::vec3& torque) override;
    void apply_impulse(BodyHandle body, const glm::vec3& impulse) override;
    void apply_impulse_at_point(BodyHandle body, const glm::vec3& impulse, const glm::vec3& worldPoint) override;
    void wake(BodyHandle body) override;

    ConstraintHandle create_distance_constraint(const DistanceConstraintDesc& description) override;
    ConstraintHandle create_swing_twist_constraint(const SwingTwistConstraintDesc& description) override;
    bool supports_swing_twist() const override { return true; }
    bool set_swing_twist_motor(ConstraintHandle constraint, bool motorOn,
                               float frequency, float damping,
                               const glm::quat& target) override;
    bool destroy_constraint(ConstraintHandle constraint) override;

    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction,
                                      float maxDistance, std::uint32_t layerMask,
                                      BodyHandle ignoredBody) const override;
    std::vector<BodyHandle> overlap_aabb(const Aabb& bounds, std::uint32_t layerMask) const override;

    VehicleHandle create_vehicle(const VehicleDesc& description) override;
    bool destroy_vehicle(VehicleHandle vehicle) override;
    bool set_vehicle_input(VehicleHandle vehicle, const VehicleInput& input) override;
    bool vehicle_wheel_state(VehicleHandle vehicle, std::size_t index, WheelState& out) const override;
    bool supports_vehicles() const override { return true; }

    std::vector<Contact> contacts() override;
    std::vector<TriggerEvent> trigger_events() override;
    std::vector<DebugLine> debug_geometry() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Engine::Physics

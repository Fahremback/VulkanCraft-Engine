#pragma once

#include "PhysicsRuntime.hpp"
#include "Vehicle.hpp"

#include <memory>
#include <string>

namespace Engine::Physics {

// PhysicsBackendKind lives in PhysicsRuntime.hpp (it is part of the runtime
// surface). Names used by config/env: "builtin", "jolt", "bullet".
PhysicsBackendKind backend_kind_from_string(const std::string& name);
std::string backend_kind_to_string(PhysicsBackendKind kind);

// The seam PhysicsRuntime delegates to when an external backend is selected.
// Handles are opaque and 1-based (InvalidBody/InvalidConstraint = 0), owned by
// the backend. Collision filtering uses the engine CollisionFilter semantics
// (mask & other.layer both ways).
class PhysicsBackend {
public:
    virtual ~PhysicsBackend() = default;

    virtual const char* name() const noexcept = 0;

    virtual void set_gravity(const glm::vec3& gravity) = 0;
    virtual void step(float deltaTime) = 0;

    virtual BodyHandle create_body(const BodyDesc& description) = 0;
    virtual bool destroy_body(BodyHandle body) = 0;
    // Kinematic <-> Dynamic flip (fracture/destruction debris). Default:
    // backends that mutate the engine RigidBody directly (builtin) need no
    // extra work; external backends must push the change through (Jolt) AND
    // restore real mass properties for the dynamic body (a kinematic body
    // carries zero inverse mass — without it gravity has no effect). `mass`
    // is the target dynamic mass.
    virtual bool set_motion_type(BodyHandle body, MotionType motion, float mass) {
        (void)body;
        (void)motion;
        (void)mass;
        return true;
    }

    virtual void set_transform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) = 0;
    virtual bool get_state(BodyHandle body, glm::vec3& position, glm::quat& rotation,
                           glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool& sleeping) const = 0;
    virtual void set_linear_velocity(BodyHandle body, const glm::vec3& velocity) = 0;
    // Sets both velocities at once (vehicle replication reconcile snaps the
    // predicted body to the authoritative state, including momentum).
    virtual void set_velocity(BodyHandle body, const glm::vec3& linearVelocity,
                              const glm::vec3& angularVelocity) {
        set_linear_velocity(body, linearVelocity);
        (void)angularVelocity;
    }
    virtual void set_gravity_scale(BodyHandle body, float scale) = 0;
    virtual void set_linear_damping(BodyHandle body, float damping) = 0;

    virtual void add_force(BodyHandle body, const glm::vec3& force) = 0;
    virtual void apply_impulse(BodyHandle body, const glm::vec3& impulse) = 0;
    virtual void apply_impulse_at_point(BodyHandle body, const glm::vec3& impulse, const glm::vec3& worldPoint) = 0;
    virtual void wake(BodyHandle body) = 0;

    virtual ConstraintHandle create_distance_constraint(const DistanceConstraintDesc& description) = 0;
    // Swing-twist ragdoll joint. Backends without a specialized solver return
    // InvalidConstraint and the caller falls back (Ragdoll uses distance
    // constraints on builtin/bullet).
    virtual ConstraintHandle create_swing_twist_constraint(const SwingTwistConstraintDesc&) {
        return InvalidConstraint;
    }
    virtual bool supports_swing_twist() const { return false; }
    // Updates the motor of an existing swing-twist constraint (active ragdoll —
    // FALTANTES §18 item 8): toggles the Position-state motors, sets the spring
    // (frequency/damping) and the target relative orientation of body B in body
    // A's constraint frame (the same convention as
    // SwingTwistConstraintDesc::motorTarget). Backends without the seam return
    // false (no-op).
    virtual bool set_swing_twist_motor(ConstraintHandle constraint, bool motorOn,
                                       float frequency, float damping,
                                       const glm::quat& target) {
        (void)constraint; (void)motorOn; (void)frequency; (void)damping;
        (void)target;
        return false;
    }
    virtual bool destroy_constraint(ConstraintHandle constraint) = 0;

    virtual    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction,
                                      float maxDistance, std::uint32_t layerMask,
                                      BodyHandle ignoredBody) const = 0;
    virtual std::vector<BodyHandle> overlap_aabb(const Aabb& bounds, std::uint32_t layerMask) const = 0;

    // Vehicle seam (Jolt Vehicles adapter). Backends without a vehicle solver
    // return InvalidVehicle / false and the caller falls back to the legacy
    // raycast model.
    virtual VehicleHandle create_vehicle(const VehicleDesc& description) {
        (void)description;
        return InvalidVehicle;
    }
    virtual bool destroy_vehicle(VehicleHandle vehicle) {
        (void)vehicle;
        return false;
    }
    virtual bool set_vehicle_input(VehicleHandle vehicle, const VehicleInput& input) {
        (void)vehicle;
        (void)input;
        return false;
    }
    virtual bool vehicle_wheel_state(VehicleHandle vehicle, std::size_t index, WheelState& out) const {
        (void)vehicle;
        (void)index;
        (void)out;
        return false;
    }
    virtual bool supports_vehicles() const { return false; }

    virtual std::vector<Contact> contacts() = 0;
    virtual std::vector<TriggerEvent> trigger_events() = 0;
    virtual std::vector<DebugLine> debug_geometry() const = 0;
};

// Returns nullptr for Builtin (PhysicsRuntime keeps its internal solver).
std::unique_ptr<PhysicsBackend> create_backend(PhysicsBackendKind kind, const WorldSettings& settings);

} // namespace Engine::Physics

#pragma once

#include "../physics/PhysicsRuntime.hpp"
#include "../physics/Vehicle.hpp"

#include <vector>

namespace Engine::Gameplay {

// The canonical vehicle types live in the physics layer (Vehicle.hpp) so the
// backend seam can use them without depending on gameplay. These aliases keep
// the gameplay-facing API (and every existing consumer) source-compatible.
using WheelDesc = Physics::WheelDesc;
using WheelState = Physics::WheelState;
using VehicleInput = Physics::VehicleInput;

// Optional drivetrain override (FALTANTES §17 item 2): engine torque / RPM /
// transmission / differential. engineMaxTorque == 0 derives the torque from
// the wheels' maxDriveForce (first gear delivers it) — the legacy behavior.
struct VehicleDrivetrain {
    float engineMaxTorque{0.0f};
    float engineMinRPM{1000.0f};
    float engineMaxRPM{6000.0f};
    float differentialRatio{3.42f};
    std::vector<float> gearRatios{2.66f, 1.78f, 1.3f, 1.0f, 0.74f};
};

class VehicleRuntime final {
public:
    VehicleRuntime(Physics::BodyHandle chassis, std::vector<WheelDesc> wheels,
                  VehicleDrivetrain drivetrain = {});
    // Kind + propulsion modules (FALTANTES §17 item 3): selects the Jolt
    // controller family and adds wing/thruster/buoyancy force modules.
    VehicleRuntime(Physics::BodyHandle chassis, std::vector<WheelDesc> wheels,
                  VehicleDrivetrain drivetrain, Physics::VehicleKind kind,
                  std::vector<Physics::PropulsionModule> propulsion);
    void set_input(const VehicleInput& input);
    // Replaces the propulsion modules (wings/thrusters/buoyancy) after
    // construction — used by tests to compare module on/off on one chassis.
    void set_propulsion(std::vector<Physics::PropulsionModule> propulsion) {
        propulsion_ = std::move(propulsion);
    }

    // --- Power / controls (FALTANTES §17 item 7) ---------------------------
    // Sets the fuel tank + battery + control mapping. The LIVE levels start
    // at the initial fractions and move with burn/draw/regen/refuel/recharge.
    void set_power(const Physics::VehiclePower& power) { power_ = power; }
    const Physics::VehiclePower& power() const noexcept { return power_; }
    // Fuel/energy fractions (0..1 of capacity; 1 when the system is off).
    float fuel_level() const noexcept { return power_.fuelLevel; }
    float charge_level() const noexcept { return power_.chargeLevel; }
    // true when the drivetrain is allowed to deliver power: every ENABLED
    // system (capacity > 0) is above its cut-out threshold.
    bool powered() const noexcept;
    // Adds fraction * capacity to the live level (clamped to 1). No-op when
    // the system is disabled (capacity == 0).
    void refuel(float fraction);
    void recharge(float fraction);
    // Applies the control mapping to the raw input (deadzone/curve/invert).
    void set_raw_input(const VehicleInput& raw);

    // --- Occupants (FALTANTES §17 item 8) -----------------------------------
    // An occupant is a physics body that rides a seat (kinematic follow of the
    // seat pose every update) and exits at the seat's world exit offset
    // (flipped back to dynamic). The body is owned by the caller's world.
    void set_seats(std::vector<Physics::VehicleSeat> seats);
    std::size_t seat_count() const noexcept { return seats_.size(); }
    const Physics::VehicleSeat& seat(std::size_t index) const;
    // World pose of the seat (chassis transform * local position).
    glm::vec3 seat_position(const Physics::PhysicsRuntime& world, std::size_t index) const;
    bool seat_occupied(std::size_t index) const noexcept;
    Physics::BodyHandle occupant(std::size_t index) const noexcept;
    // Attaches `body` to seat `index` (kinematic ride). Fails if the seat is
    // taken or the body is not a valid dynamic body. Restores the body's mass
    // on exit (read from its pre-ride inverseMass).
    bool enter_occupant(Physics::PhysicsRuntime& world, Physics::BodyHandle body,
                        std::size_t index, std::string& errorOut);
    bool exit_occupant(Physics::PhysicsRuntime& world, std::size_t index,
                       std::string& errorOut);

    // --- Damage / parts (FALTANTES §17 item 9) -------------------------------
    // Parts are AUTO-DERIVED: [0]=Chassis, [1]=Drivetrain, [2+i]=Wheel i.
    std::size_t part_count() const noexcept { return parts_.size(); }
    const Physics::VehiclePartInfo& part(std::size_t index) const;
    bool apply_damage(std::size_t partIndex, float amount, std::string& errorOut);
    bool repair(std::size_t partIndex, float amount, std::string& errorOut);
    bool is_separated(std::size_t partIndex) const;
    // Effective drive scale from damage: drivetrain health * average driven-
    // wheel health (0 when every driven wheel separated).
    float drive_scale() const;
    // Drives the vehicle. On worlds with a vehicle solver (Jolt) this pushes
    // the input through the backend constraint and reads the real wheel
    // states; on builtin/bullet worlds it keeps the legacy raycast model.
    // Propulsion modules apply their forces every update on every backend.
    void update(Physics::PhysicsRuntime& world, float deltaTime, std::uint32_t drivableLayers = ~0u);

    Physics::BodyHandle chassis() const noexcept { return chassis_; }
    const std::vector<WheelDesc>& wheels() const noexcept { return wheels_; }
    const std::vector<WheelState>& wheel_states() const noexcept { return states_; }
    float speed(const Physics::PhysicsRuntime& world) const;
    bool valid(const Physics::PhysicsRuntime& world) const;
    // The adapter descriptor derived from this runtime's wheels (exposed for
    // diagnostics/tests).
    Physics::VehicleDesc make_vehicle_desc() const;

private:
    void apply_propulsion(Physics::PhysicsRuntime& world, float deltaTime);
    void update_raycast(Physics::PhysicsRuntime& world, float deltaTime, std::uint32_t drivableLayers);

    Physics::BodyHandle chassis_{Physics::InvalidBody};
    std::vector<WheelDesc> wheels_;
    std::vector<WheelState> states_;
    VehicleDrivetrain drivetrain_;
    Physics::VehicleKind kind_{ Physics::VehicleKind::Wheeled };
    std::vector<Physics::PropulsionModule> propulsion_;
    VehicleInput input_{};
    // Jolt adapter state (lazily created on the first update against a world
    // that supports vehicles).
    Physics::VehicleHandle vehicle_{Physics::InvalidVehicle};
    bool adapter_ready_{false};

    std::vector<Physics::VehicleSeat> seats_;
    std::vector<Physics::BodyHandle> occupants_;  // InvalidBody when empty
    std::vector<float> occupantMass_;             // mass restored on exit
    std::vector<Physics::VehiclePartInfo> parts_;
    Physics::VehiclePower power_;
    void update_occupants(Physics::PhysicsRuntime& world);
    // Burns fuel / draws-or-regens energy for this step (dt) by the current
    // input, then returns the power scale (0 when a system is below its
    // cut-out threshold).
    float consume_power(float deltaTime);
};

} // namespace Engine::Gameplay

#pragma once

// BeamChassisRuntime (FALTANTES §17 item 4): a deformable node/beam vehicle
// chassis solved by XPBD. The chassis is a graph of nodes + beams (distance
// constraints with per-beam stiffness); wheels hang from mount nodes and the
// suspension/drive/brake forces are applied to THOSE nodes, so the chassis
// BENDS under load instead of staying rigid. The solver is the public
// IDeformableProvider (Xpbd, §16 item 3); this runtime is the vehicle
// integration: it builds the deformable body from the BeamGraphAsset, raycasts
// the wheels against the physics world, applies forces at the mount nodes,
// steps the solver, and exposes the deformed node positions.

#include "../physics/PhysicsRuntime.hpp"
#include "../physics/Vehicle.hpp"
#include "../public/engine/vehicles/IBeamGraphAsset.hpp"
#include "../public/engine/deformable/IDeformableProvider.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Engine::Gameplay {

class BeamChassisRuntime final {
public:
    // Builds the XPBD deformable body from the asset (nodes/beams/wheels).
    // Fails with a diagnostic for an invalid asset/body (all-or-nothing).
    BeamChassisRuntime(Physics::PhysicsRuntime& world,
                       const engine::vehicles::BeamGraphAsset& asset,
                       std::string& errorOut);

    void set_input(const Physics::VehicleInput& input);

    // --- Power / controls (FALTANTES §17 item 7) ---------------------------
    void set_power(const Physics::VehiclePower& power) { power_ = power; }
    const Physics::VehiclePower& power() const noexcept { return power_; }
    float fuel_level() const noexcept { return power_.fuelLevel; }
    float charge_level() const noexcept { return power_.chargeLevel; }
    bool powered() const noexcept;
    void refuel(float fraction);
    void recharge(float fraction);
    void set_raw_input(const Physics::VehicleInput& raw);

    // Applies the wheel forces to the mount nodes (raycast against the world),
    // steps the XPBD solver, and reads the resulting deformed state. The
    // chassis orientation is derived from the CURRENT node positions (two
    // reference nodes by rest Z/X), so the vehicle drives where it is tilted.
    void update(float deltaTime, std::uint32_t drivableLayers = ~0u);

    bool valid() const noexcept { return valid_; }
    std::size_t node_count() const noexcept { return nodeCount_; }
    // Deformed (current solved) node positions in world space.
    glm::vec3 node_position(std::size_t index) const;
    glm::vec3 chassis_position() const;   // centroid of the free nodes
    glm::vec3 chassis_forward() const;    // current forward (reference nodes)
    glm::vec3 chassis_up() const;         // current up (right x forward)
    // Max |node - rest| over the free nodes: the deformability observable.
    float deformation() const;
    float speed(const Physics::PhysicsRuntime& world) const;
    const std::vector<Physics::WheelState>& wheel_states() const noexcept {
        return states_;
    }

    // --- Occupants (FALTANTES §17 item 8) -----------------------------------
    // Seats derive their pose from the DEFORMED node frame; the occupant body
    // rides the seat (kinematic follow every update) and exits at the seat's
    // world exit offset.
    void set_seats(std::vector<Physics::VehicleSeat> seats);
    std::size_t seat_count() const noexcept { return seats_.size(); }
    const Physics::VehicleSeat& seat(std::size_t index) const;
    glm::vec3 seat_position(std::size_t index) const;
    bool seat_occupied(std::size_t index) const noexcept;
    Physics::BodyHandle occupant(std::size_t index) const noexcept;
    bool enter_occupant(Physics::PhysicsRuntime& world, Physics::BodyHandle body,
                        std::size_t index, std::string& errorOut);
    bool exit_occupant(Physics::PhysicsRuntime& world, std::size_t index,
                       std::string& errorOut);

    // --- Damage / parts (FALTANTES §17 item 9) -------------------------------
    // Parts are AUTO-DERIVED: [0]=Chassis, [1+i]=Wheel i,
    // [1+nWheels+j]=Beam j. Beam damage degrades that beam's stiffness;
    // chassis damage degrades EVERY beam; a separated beam is deactivated
    // (the constraint no longer holds the mesh together).
    std::size_t part_count() const noexcept { return parts_.size(); }
    const Physics::VehiclePartInfo& part(std::size_t index) const;
    bool apply_damage(Physics::PhysicsRuntime& world, std::size_t partIndex,
                      float amount, std::string& errorOut);
    bool repair(Physics::PhysicsRuntime& world, std::size_t partIndex,
                float amount, std::string& errorOut);
    bool is_separated(std::size_t partIndex) const;

private:
    Physics::PhysicsRuntime& world_;
    std::unique_ptr<Deformable::IDeformableProvider> solver_;
    Deformable::DeformableBodyHandle body_{Deformable::InvalidDeformableBody};
    std::vector<glm::vec3> restNodes_;   // rest positions (for deformation)
    std::vector<float> beamStiffness_;   // rest stiffness per beam (damage base)
    std::vector<std::size_t> wheelNodes_;
    std::vector<Physics::WheelDesc> wheels_;
    std::vector<Physics::WheelState> states_;
    Physics::VehicleInput input_{};
    std::size_t nodeCount_{ 0 };
    float forceScale_{ 1.0f };  // nodeCount / mass: unit-mass nodes -> kg nodes
    std::uint32_t forwardNode_{ 0 };  // rest min-Z node (front)
    std::uint32_t backNode_{ 0 };     // rest max-Z node (back)
    std::uint32_t leftNode_{ 0 };     // rest min-X node
    std::uint32_t rightNode_{ 0 };    // rest max-X node
    bool valid_{ false };

    std::vector<Physics::VehicleSeat> seats_;
    std::vector<Physics::BodyHandle> occupants_;  // InvalidBody when empty
    std::vector<float> occupantMass_;
    std::vector<Physics::VehiclePartInfo> parts_;
    Physics::VehiclePower power_;
    std::size_t wheelPartOffset_{ 1 };   // parts index of the first wheel
    std::size_t beamPartOffset_{ 1 };    // parts index of the first beam
    void refresh_beam_stiffness();
    void update_occupants(Physics::PhysicsRuntime& world);
    float consume_power(float deltaTime);
};

}  // namespace Engine::Gameplay

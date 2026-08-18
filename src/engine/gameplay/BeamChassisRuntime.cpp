// BeamChassisRuntime.cpp — the XPBD node/beam vehicle chassis (FALTANTES §17
// item 4). Builds a deformable body from the BeamGraphAsset through the public
// IDeformableProvider (Xpbd), mounts the wheels on their nodes, applies
// suspension/drive/brake forces AT THE NODES (so the chassis deforms under
// load), and steps the solver. Deterministic: the solver iterates nodes/edges
// in creation order and the wheel raycasts are deterministic per backend.

#include "BeamChassisRuntime.hpp"

#include <algorithm>
#include <cmath>

namespace Engine::Gameplay {

namespace {

glm::vec3 normalized_or(const glm::vec3& v, const glm::vec3& fallback) {
    const float len = glm::length(v);
    return len > 1.0e-6f ? v / len : fallback;
}

}  // namespace

BeamChassisRuntime::BeamChassisRuntime(Physics::PhysicsRuntime& world,
                                       const engine::vehicles::BeamGraphAsset& asset,
                                       std::string& errorOut)
    : world_(world) {
    std::string validationError;
    if (!asset.validate(validationError)) {
        errorOut = validationError;
        return;
    }

    // Map the asset solver config into the XPBD provider config. The wheel
    // suspension handles the ground — the solver's plane is disabled so it
    // never fights the raycast support.
    Deformable::DeformableConfig config;
    config.substeps = asset.solver.substeps;
    config.solverIterations = asset.solver.solverIterations;
    config.stiffness = asset.solver.stiffness;
    config.damping = asset.solver.damping;
    config.gravity = asset.solver.gravity;
    config.groundCollision = false;
    solver_ = Deformable::create_deformable_provider(
        Deformable::DeformableProviderKind::Xpbd, config, errorOut);
    if (!solver_) return;

    // Assemble the deformable mesh: nodes at asset transform, beams with
    // per-edge stiffness, fixed flags from the asset.
    Deformable::DeformableMeshDesc desc;
    desc.nodes.reserve(asset.nodes.size());
    desc.fixed.reserve(asset.nodes.size());
    const glm::mat3 basis = glm::mat3_cast(glm::normalize(asset.rotation));
    for (const engine::vehicles::BeamNode& node : asset.nodes) {
        desc.nodes.push_back(asset.position + basis * node.position);
        desc.fixed.push_back(node.fixed);
    }
    desc.edges.reserve(asset.beams.size());
    desc.stiffness.reserve(asset.beams.size());
    beamStiffness_.reserve(asset.beams.size());
    for (const engine::vehicles::Beam& beam : asset.beams) {
        desc.edges.emplace_back(beam.a, beam.b);
        desc.stiffness.push_back(beam.stiffness);
        beamStiffness_.push_back(beam.stiffness);
    }

    body_ = solver_->create_body(desc, errorOut);
    if (body_ == Deformable::InvalidDeformableBody) return;

    // The XPBD solver treats every node as unit mass. To make the wheel
    // forces (tuned for a ~1000+ kg rigid chassis) act on a chassis whose
    // mass is distributed across the nodes, scale them by nodeCount / mass:
    // each node then accelerates as if it carried mass / nodeCount kg.
    forceScale_ = static_cast<float>(desc.nodes.size()) / asset.mass;

    // Reference nodes by rest geometry: front = min-Z, back = max-Z,
    // left = min-X, right = max-X (the engine forward convention is -Z).
    restNodes_ = desc.nodes;
    nodeCount_ = desc.nodes.size();
    forwardNode_ = backNode_ = 0;
    leftNode_ = rightNode_ = 0;
    for (std::uint32_t i = 0; i < nodeCount_; ++i) {
        if (desc.nodes[i].z < desc.nodes[forwardNode_].z) forwardNode_ = i;
        if (desc.nodes[i].z > desc.nodes[backNode_].z) backNode_ = i;
        if (desc.nodes[i].x < desc.nodes[leftNode_].x) leftNode_ = i;
        if (desc.nodes[i].x > desc.nodes[rightNode_].x) rightNode_ = i;
    }

    // Wheels: mount node + wheel descriptor.
    wheelNodes_.clear();
    wheels_.clear();
    wheelNodes_.reserve(asset.wheels.size());
    wheels_.reserve(asset.wheels.size());
    for (const engine::vehicles::BeamWheelMount& mount : asset.wheels) {
        wheelNodes_.push_back(mount.node);
        Physics::WheelDesc wheel;
        wheel.localPosition = mount.wheel.localPosition;
        wheel.radius = mount.wheel.radius;
        wheel.suspensionRestLength = mount.wheel.suspensionRestLength;
        wheel.suspensionTravel = mount.wheel.suspensionTravel;
        wheel.springStrength = mount.wheel.springStrength;
        wheel.damperStrength = mount.wheel.damperStrength;
        wheel.tireGrip = mount.wheel.tireGrip;
        wheel.maxDriveForce = mount.wheel.maxDriveForce;
        wheel.maxBrakeForce = mount.wheel.maxBrakeForce;
        wheel.maxSteerAngle = mount.wheel.maxSteerAngle;
        wheel.steering = mount.steering;
        wheel.driven = mount.driven;
        wheels_.push_back(wheel);
    }
    states_.assign(wheels_.size(), Physics::WheelState{});
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        states_[i].suspensionLength = wheels_[i].suspensionRestLength;
    }
    // Damage parts auto-derived (FALTANTES §17 item 9): [0]=Chassis,
    // [1+i]=Wheel i, [1+nWheels+j]=Beam j.
    Physics::VehiclePartInfo chassisPart;
    chassisPart.name = "chassis";
    chassisPart.kind = Physics::VehiclePartKind::Chassis;
    parts_.push_back(chassisPart);
    wheelPartOffset_ = parts_.size();
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        Physics::VehiclePartInfo wheelPart;
        wheelPart.name = "wheel_" + std::to_string(i);
        wheelPart.kind = Physics::VehiclePartKind::Wheel;
        wheelPart.componentIndex = i;
        parts_.push_back(wheelPart);
    }
    beamPartOffset_ = parts_.size();
    for (std::size_t j = 0; j < beamStiffness_.size(); ++j) {
        Physics::VehiclePartInfo beamPart;
        beamPart.name = "beam_" + std::to_string(j);
        beamPart.kind = Physics::VehiclePartKind::Beam;
        beamPart.componentIndex = j;
        parts_.push_back(beamPart);
    }
    valid_ = true;
}

void BeamChassisRuntime::set_seats(std::vector<Physics::VehicleSeat> seats) {
    seats_ = std::move(seats);
    occupants_.assign(seats_.size(), Physics::InvalidBody);
    occupantMass_.assign(seats_.size(), 1.0f);
}

const Physics::VehicleSeat& BeamChassisRuntime::seat(std::size_t index) const {
    static const Physics::VehicleSeat kEmpty;
    return index < seats_.size() ? seats_[index] : kEmpty;
}

glm::vec3 BeamChassisRuntime::seat_position(std::size_t index) const {
    if (index >= seats_.size() || !valid_) return glm::vec3(0.0f);
    const glm::vec3 forward = chassis_forward();
    const glm::vec3 up = chassis_up();
    const glm::vec3 right = normalized_or(glm::cross(up, forward),
                                          glm::vec3(1.0f, 0.0f, 0.0f));
    const Physics::VehicleSeat& seat = seats_[index];
    return chassis_position() + right * seat.localPosition.x +
           up * seat.localPosition.y + forward * seat.localPosition.z;
}

bool BeamChassisRuntime::seat_occupied(std::size_t index) const noexcept {
    return index < occupants_.size() && occupants_[index] != Physics::InvalidBody;
}

Physics::BodyHandle BeamChassisRuntime::occupant(std::size_t index) const noexcept {
    return index < occupants_.size() ? occupants_[index] : Physics::InvalidBody;
}

bool BeamChassisRuntime::enter_occupant(Physics::PhysicsRuntime& world,
                                        Physics::BodyHandle body,
                                        std::size_t index, std::string& errorOut) {
    if (!valid_) { errorOut = "chassis not valid"; return false; }
    if (index >= seats_.size()) { errorOut = "seat index out of range"; return false; }
    if (occupants_[index] != Physics::InvalidBody) {
        errorOut = "seat already occupied";
        return false;
    }
    Physics::RigidBody* rb = world.body(body);
    if (!rb || !rb->dynamic()) { errorOut = "occupant must be a valid dynamic body"; return false; }
    const float mass = rb->inverseMass > 0.0f ? 1.0f / rb->inverseMass : 1.0f;
    world.set_motion(body, Physics::MotionType::Kinematic, mass);
    occupants_[index] = body;
    occupantMass_[index] = mass;
    return true;
}

bool BeamChassisRuntime::exit_occupant(Physics::PhysicsRuntime& world,
                                       std::size_t index, std::string& errorOut) {
    if (!valid_ || index >= seats_.size() ||
        occupants_[index] == Physics::InvalidBody) {
        errorOut = "seat not occupied";
        return false;
    }
    const Physics::BodyHandle body = occupants_[index];
    const glm::vec3 forward = chassis_forward();
    const glm::vec3 up = chassis_up();
    const glm::vec3 right = normalized_or(glm::cross(up, forward),
                                          glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat3 basis(right, up, forward);
    const Physics::VehicleSeat& seat = seats_[index];
    const glm::vec3 exitPosition =
        chassis_position() + basis * seat.exitOffset;
    world.set_motion(body, Physics::MotionType::Dynamic, occupantMass_[index]);
    world.set_transform(body, exitPosition, glm::quat_cast(basis));
    world.wake(body);
    occupants_[index] = Physics::InvalidBody;
    return true;
}

void BeamChassisRuntime::update_occupants(Physics::PhysicsRuntime& world) {
    if (!valid_) return;
    const glm::vec3 forward = chassis_forward();
    const glm::vec3 up = chassis_up();
    const glm::vec3 right = normalized_or(glm::cross(up, forward),
                                          glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat3 basis(right, up, forward);
    const glm::quat rotation = glm::quat_cast(basis);
    const glm::vec3 origin = chassis_position();
    for (std::size_t i = 0; i < occupants_.size(); ++i) {
        if (occupants_[i] == Physics::InvalidBody) continue;
        world.set_transform(occupants_[i], origin + basis * seats_[i].localPosition,
                            rotation);
    }
}

const Physics::VehiclePartInfo& BeamChassisRuntime::part(std::size_t index) const {
    static const Physics::VehiclePartInfo kEmpty;
    return index < parts_.size() ? parts_[index] : kEmpty;
}

void BeamChassisRuntime::refresh_beam_stiffness() {
    if (!valid_ || beamStiffness_.empty()) return;
    const float chassisScale = parts_[0].maxHealth > 0.0f
        ? parts_[0].health / parts_[0].maxHealth : 0.0f;
    for (std::size_t j = 0; j < beamStiffness_.size(); ++j) {
        const Physics::VehiclePartInfo& beamPart = parts_[beamPartOffset_ + j];
        const float beamScale = beamPart.maxHealth > 0.0f
            ? beamPart.health / beamPart.maxHealth : 0.0f;
        const float effective =
            beamPart.separated ? 0.0f
                               : beamStiffness_[j] * chassisScale * beamScale;
        solver_->set_edge_stiffness(body_, static_cast<std::uint32_t>(j),
                                    effective);
    }
}

bool BeamChassisRuntime::apply_damage(Physics::PhysicsRuntime& world,
                                      std::size_t partIndex, float amount,
                                      std::string& errorOut) {
    (void)world;
    if (partIndex >= parts_.size()) { errorOut = "part index out of range"; return false; }
    if (!(amount > 0.0f)) { errorOut = "damage amount must be positive"; return false; }
    Physics::VehiclePartInfo& part = parts_[partIndex];
    part.health = std::max(0.0f, part.health - amount);
    part.separated = part.health <= 0.0f;
    refresh_beam_stiffness();  // chassis/beam damage changes the solver
    return true;
}

bool BeamChassisRuntime::repair(Physics::PhysicsRuntime& world,
                                std::size_t partIndex, float amount,
                                std::string& errorOut) {
    (void)world;
    if (partIndex >= parts_.size()) { errorOut = "part index out of range"; return false; }
    if (!(amount > 0.0f)) { errorOut = "repair amount must be positive"; return false; }
    Physics::VehiclePartInfo& part = parts_[partIndex];
    part.health = std::min(part.maxHealth, part.health + amount);
    part.separated = false;
    refresh_beam_stiffness();
    return true;
}

bool BeamChassisRuntime::is_separated(std::size_t partIndex) const {
    return partIndex < parts_.size() && parts_[partIndex].separated;
}

bool BeamChassisRuntime::powered() const noexcept {
    if (power_.fuel.capacity > 0.0f &&
        power_.fuelLevel <= power_.fuel.minLevelToRun) {
        return false;
    }
    if (power_.energy.capacity > 0.0f &&
        power_.chargeLevel <= power_.energy.minChargeToRun) {
        return false;
    }
    return true;
}

void BeamChassisRuntime::refuel(float fraction) {
    if (!(power_.fuel.capacity > 0.0f) || !(fraction > 0.0f)) return;
    power_.fuelLevel = std::min(1.0f, power_.fuelLevel + fraction);
}

void BeamChassisRuntime::recharge(float fraction) {
    if (!(power_.energy.capacity > 0.0f) || !(fraction > 0.0f)) return;
    power_.chargeLevel = std::min(1.0f, power_.chargeLevel + fraction);
}

void BeamChassisRuntime::set_raw_input(const Physics::VehicleInput& raw) {
    set_input(Physics::map_controls(power_.controls, raw));
}

void BeamChassisRuntime::set_input(const Physics::VehicleInput& input) {
    input_.throttle = glm::clamp(input.throttle, -1.0f, 1.0f);
    input_.steering = glm::clamp(input.steering, -1.0f, 1.0f);
    input_.brake = glm::clamp(input.brake, 0.0f, 1.0f);
    input_.handbrake = glm::clamp(input.handbrake, 0.0f, 1.0f);
}

float BeamChassisRuntime::consume_power(float deltaTime) {
    if (deltaTime <= 0.0f) return 1.0f;
    if (power_.fuel.capacity > 0.0f) {
        const float burn =
            power_.fuel.burnPerSecond * std::abs(input_.throttle) +
            power_.fuel.idleBurnPerSecond;
        power_.fuelLevel = std::max(0.0f, power_.fuelLevel - burn * deltaTime / power_.fuel.capacity);
    }
    if (power_.energy.capacity > 0.0f) {
        const float draw = power_.energy.drawPerSecond * std::abs(input_.throttle);
        const float regen = power_.energy.regenPerSecond * input_.brake;
        power_.chargeLevel = glm::clamp(
            power_.chargeLevel + (regen - draw) * deltaTime / power_.energy.capacity,
            0.0f, 1.0f);
    }
    return powered() ? 1.0f : 0.0f;
}

glm::vec3 BeamChassisRuntime::chassis_forward() const {
    if (!valid_ || nodeCount_ == 0) return glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 front = solver_->node_position(body_, forwardNode_);
    const glm::vec3 back = solver_->node_position(body_, backNode_);
    return normalized_or(front - back, glm::vec3(0.0f, 0.0f, -1.0f));
}

glm::vec3 BeamChassisRuntime::chassis_up() const {
    if (!valid_ || nodeCount_ == 0) return glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 forward = chassis_forward();
    const glm::vec3 right = normalized_or(
        solver_->node_position(body_, rightNode_) -
            solver_->node_position(body_, leftNode_),
        glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 up = normalized_or(glm::cross(right, forward),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    return up;
}

glm::vec3 BeamChassisRuntime::chassis_position() const {
    if (!valid_ || nodeCount_ == 0) return glm::vec3(0.0f);
    glm::vec3 sum(0.0f);
    std::size_t count = 0;
    for (std::uint32_t i = 0; i < nodeCount_; ++i) {
        sum += solver_->node_position(body_, i);
        ++count;
    }
    return count ? sum / static_cast<float>(count) : glm::vec3(0.0f);
}

glm::vec3 BeamChassisRuntime::node_position(std::size_t index) const {
    if (!valid_ || index >= nodeCount_) return glm::vec3(0.0f);
    return solver_->node_position(body_, static_cast<std::uint32_t>(index));
}

float BeamChassisRuntime::deformation() const {
    if (!valid_ || nodeCount_ == 0 || restNodes_.size() != nodeCount_) return 0.0f;
    float maxDeformation = 0.0f;
    for (std::uint32_t i = 0; i < nodeCount_; ++i) {
        maxDeformation = std::max(
            maxDeformation, glm::length(solver_->node_position(body_, i) - restNodes_[i]));
    }
    return maxDeformation;
}

float BeamChassisRuntime::speed(const Physics::PhysicsRuntime& world) const {
    (void)world;
    if (!valid_ || nodeCount_ == 0) return 0.0f;
    const glm::vec3 forward = chassis_forward();
    glm::vec3 velocitySum(0.0f);
    for (std::uint32_t i = 0; i < nodeCount_; ++i) {
        velocitySum += solver_->node_velocity(body_, i);
    }
    return glm::dot(velocitySum / static_cast<float>(nodeCount_), forward);
}

void BeamChassisRuntime::update(float deltaTime, std::uint32_t drivableLayers) {
    if (!valid_ || deltaTime <= 0.0f) return;

    // Power (FALTANTES §17 item 7): burn fuel / draw-or-regen energy by the
    // input; without power the wheel drive/brake forces die.
    const float powerScale = consume_power(deltaTime);

    // Chassis frame from the CURRENT deformed nodes (drives where tilted).
    const glm::vec3 forward = chassis_forward();
    const glm::vec3 up = chassis_up();
    const glm::vec3 down = -up;
    const glm::vec3 right = normalized_or(glm::cross(up, forward), glm::vec3(1.0f, 0.0f, 0.0f));

    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        const Physics::WheelDesc& wheel = wheels_[i];
        const std::size_t node = wheelNodes_[i];
        Physics::WheelState& state = states_[i];
        // Damage (FALTANTES §17 item 9): a separated wheel pops off (skipped);
        // a damaged wheel scales its forces by the remaining health.
        const Physics::VehiclePartInfo& wheelPart = parts_[wheelPartOffset_ + i];
        const float wheelScale = wheelPart.maxHealth > 0.0f
            ? wheelPart.health / wheelPart.maxHealth : 0.0f;
        const glm::vec3 mount = solver_->node_position(body_, static_cast<std::uint32_t>(node));

        const float steerAngle = wheel.steering ? input_.steering * wheel.maxSteerAngle : 0.0f;
        state.steerAngle = steerAngle;
        const float cosine = std::cos(steerAngle), sine = std::sin(steerAngle);
        const glm::vec3 steeredForward = normalized_or(forward * cosine + right * sine, forward);

        const float castLength = wheel.suspensionRestLength + wheel.suspensionTravel + wheel.radius;
        const auto hit = world_.raycast(mount, down, castLength, drivableLayers, Physics::InvalidBody);
        state.grounded = hit.has_value();
        state.groundBody = hit ? hit->body : Physics::InvalidBody;
        if (!hit || wheelPart.separated) {
            state.suspensionLength = wheel.suspensionRestLength + wheel.suspensionTravel;
            state.compression = 0.0f;
            state.contactPoint = mount + down * state.suspensionLength;
            state.contactNormal = up;
            state.angularSpeed = 0.0f;
            continue;
        }

        state.contactPoint = hit->point;
        state.contactNormal = hit->normal;
        state.suspensionLength = glm::clamp(hit->distance - wheel.radius,
                                            wheel.suspensionRestLength - wheel.suspensionTravel,
                                            wheel.suspensionRestLength + wheel.suspensionTravel);
        state.compression = wheel.suspensionRestLength - state.suspensionLength;

        // Suspension: spring + damper along the contact normal, applied to the
        // mount NODE (the chassis deforms under the load).
        const glm::vec3 nodeVelocity = solver_->node_velocity(body_, static_cast<std::uint32_t>(node));
        const float suspensionVelocity = glm::dot(nodeVelocity, hit->normal);
        const float springForce = std::max(0.0f, state.compression * wheel.springStrength +
                                                  suspensionVelocity * wheel.damperStrength);
        solver_->apply_force(body_, static_cast<std::uint32_t>(node),
                             hit->normal * (springForce * forceScale_ * wheelScale));

        // Longitudinal drive/brake along the steered forward.
        const float lateralSpeed = glm::dot(nodeVelocity, right);
        solver_->apply_force(body_, static_cast<std::uint32_t>(node),
                             -right * (lateralSpeed * wheel.tireGrip * 20.0f * forceScale_ * wheelScale));
        float drive = wheel.driven ? input_.throttle * wheel.maxDriveForce * wheelScale * powerScale : 0.0f;
        if (input_.brake > 0.0f) {
            drive -= input_.brake * wheel.maxBrakeForce * wheelScale * powerScale *
                     (drive > 0.0f ? 1.0f : -1.0f);
        }
        if (drive != 0.0f) {
            solver_->apply_force(body_, static_cast<std::uint32_t>(node),
                                 steeredForward * (drive * forceScale_));
        }
        state.angularSpeed = wheel.radius > 0.001f
            ? glm::dot(nodeVelocity, steeredForward) / wheel.radius
            : 0.0f;
        state.rotation = std::fmod(state.rotation + state.angularSpeed * deltaTime,
                                   glm::two_pi<float>());
    }

    // Step the XPBD solver (gravity + beam constraints).
    solver_->step(deltaTime);
    update_occupants(world_);
}

}  // namespace Engine::Gameplay

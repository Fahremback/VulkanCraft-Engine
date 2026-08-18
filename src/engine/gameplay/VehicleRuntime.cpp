#include "VehicleRuntime.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Engine::Gameplay {
namespace {
glm::vec3 normalized_or(const glm::vec3& value, const glm::vec3& fallback) {
    const float squared = glm::dot(value, value);
    return squared > 1.0e-8f ? value / std::sqrt(squared) : fallback;
}
} // namespace

VehicleRuntime::VehicleRuntime(Physics::BodyHandle chassis, std::vector<WheelDesc> wheels,
                               VehicleDrivetrain drivetrain)
    : VehicleRuntime(chassis, std::move(wheels), std::move(drivetrain),
                     Physics::VehicleKind::Wheeled, {}) {}

VehicleRuntime::VehicleRuntime(Physics::BodyHandle chassis, std::vector<WheelDesc> wheels,
                               VehicleDrivetrain drivetrain, Physics::VehicleKind kind,
                               std::vector<Physics::PropulsionModule> propulsion)
    : chassis_(chassis), wheels_(std::move(wheels)), states_(wheels_.size()),
      drivetrain_(std::move(drivetrain)), kind_(kind), propulsion_(std::move(propulsion)) {
    for (std::size_t i = 0; i < wheels_.size(); ++i) states_[i].suspensionLength = wheels_[i].suspensionRestLength;
    // Damage parts auto-derived from the components (FALTANTES §17 item 9):
    // [0]=Chassis, [1]=Drivetrain, [2+i]=Wheel i.
    Physics::VehiclePartInfo chassisPart;
    chassisPart.name = "chassis";
    chassisPart.kind = Physics::VehiclePartKind::Chassis;
    parts_.push_back(chassisPart);
    Physics::VehiclePartInfo drivetrainPart;
    drivetrainPart.name = "drivetrain";
    drivetrainPart.kind = Physics::VehiclePartKind::Drivetrain;
    parts_.push_back(drivetrainPart);
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        Physics::VehiclePartInfo wheelPart;
        wheelPart.name = "wheel_" + std::to_string(i);
        wheelPart.kind = Physics::VehiclePartKind::Wheel;
        wheelPart.componentIndex = i;
        parts_.push_back(wheelPart);
    }
}

void VehicleRuntime::set_seats(std::vector<Physics::VehicleSeat> seats) {
    seats_ = std::move(seats);
    occupants_.assign(seats_.size(), Physics::InvalidBody);
    occupantMass_.assign(seats_.size(), 1.0f);
}

const Physics::VehicleSeat& VehicleRuntime::seat(std::size_t index) const {
    static const Physics::VehicleSeat kEmpty;
    return index < seats_.size() ? seats_[index] : kEmpty;
}

glm::vec3 VehicleRuntime::seat_position(const Physics::PhysicsRuntime& world,
                                        std::size_t index) const {
    if (index >= seats_.size()) return glm::vec3(0.0f);
    const Physics::RigidBody* chassis = world.body(chassis_);
    if (!chassis) return seats_[index].localPosition;
    return chassis->position + chassis->rotation * seats_[index].localPosition;
}

bool VehicleRuntime::seat_occupied(std::size_t index) const noexcept {
    return index < occupants_.size() && occupants_[index] != Physics::InvalidBody;
}

Physics::BodyHandle VehicleRuntime::occupant(std::size_t index) const noexcept {
    return index < occupants_.size() ? occupants_[index] : Physics::InvalidBody;
}

bool VehicleRuntime::enter_occupant(Physics::PhysicsRuntime& world,
                                    Physics::BodyHandle body,
                                    std::size_t index, std::string& errorOut) {
    if (index >= seats_.size()) { errorOut = "seat index out of range"; return false; }
    if (occupants_[index] != Physics::InvalidBody) { errorOut = "seat already occupied"; return false; }
    Physics::RigidBody* rb = world.body(body);
    if (!rb || !rb->dynamic()) { errorOut = "occupant must be a valid dynamic body"; return false; }
    const float mass = rb->inverseMass > 0.0f ? 1.0f / rb->inverseMass : 1.0f;
    // Kinematic ride: gravity/impulses stop affecting the body; the runtime
    // drives it to the seat pose every update.
    world.set_motion(body, Physics::MotionType::Kinematic, mass);
    occupants_[index] = body;
    occupantMass_[index] = mass;
    return true;
}

bool VehicleRuntime::exit_occupant(Physics::PhysicsRuntime& world,
                                   std::size_t index, std::string& errorOut) {
    if (index >= seats_.size() || occupants_[index] == Physics::InvalidBody) {
        errorOut = "seat not occupied";
        return false;
    }
    const Physics::BodyHandle body = occupants_[index];
    const Physics::RigidBody* chassis = world.body(chassis_);
    const glm::quat rotation =
        chassis ? chassis->rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 exitPosition = chassis
        ? chassis->position + chassis->rotation * seats_[index].exitOffset
        : seats_[index].exitOffset;
    // Flip back to dynamic and spawn at the exit offset (the occupant stands
    // outside the vehicle).
    world.set_motion(body, Physics::MotionType::Dynamic, occupantMass_[index]);
    world.set_transform(body, exitPosition, rotation);
    world.wake(body);
    occupants_[index] = Physics::InvalidBody;
    return true;
}

void VehicleRuntime::update_occupants(Physics::PhysicsRuntime& world) {
    const Physics::RigidBody* chassis = world.body(chassis_);
    const glm::quat rotation =
        chassis ? chassis->rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    for (std::size_t i = 0; i < occupants_.size(); ++i) {
        if (occupants_[i] == Physics::InvalidBody) continue;
        const glm::vec3 position = chassis
            ? chassis->position + chassis->rotation * seats_[i].localPosition
            : seats_[i].localPosition;
        world.set_transform(occupants_[i], position, rotation);
    }
}

const Physics::VehiclePartInfo& VehicleRuntime::part(std::size_t index) const {
    static const Physics::VehiclePartInfo kEmpty;
    return index < parts_.size() ? parts_[index] : kEmpty;
}

bool VehicleRuntime::apply_damage(std::size_t partIndex, float amount,
                                  std::string& errorOut) {
    if (partIndex >= parts_.size()) { errorOut = "part index out of range"; return false; }
    if (!(amount > 0.0f)) { errorOut = "damage amount must be positive"; return false; }
    Physics::VehiclePartInfo& part = parts_[partIndex];
    part.health = std::max(0.0f, part.health - amount);
    part.separated = part.health <= 0.0f;
    return true;
}

bool VehicleRuntime::repair(std::size_t partIndex, float amount,
                            std::string& errorOut) {
    if (partIndex >= parts_.size()) { errorOut = "part index out of range"; return false; }
    if (!(amount > 0.0f)) { errorOut = "repair amount must be positive"; return false; }
    Physics::VehiclePartInfo& part = parts_[partIndex];
    part.health = std::min(part.maxHealth, part.health + amount);
    part.separated = false;
    return true;
}

bool VehicleRuntime::is_separated(std::size_t partIndex) const {
    return partIndex < parts_.size() && parts_[partIndex].separated;
}

float VehicleRuntime::drive_scale() const {
    if (parts_.size() < 2) return 1.0f;
    float scale = parts_[1].maxHealth > 0.0f
        ? parts_[1].health / parts_[1].maxHealth : 0.0f;
    float sum = 0.0f;
    std::size_t count = 0;
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        if (!wheels_[i].driven) continue;
        const Physics::VehiclePartInfo& part = parts_[2 + i];
        sum += part.maxHealth > 0.0f ? part.health / part.maxHealth : 0.0f;
        ++count;
    }
    if (count > 0) scale *= sum / static_cast<float>(count);
    return glm::clamp(scale, 0.0f, 1.0f);
}

// Applies the propulsion modules (FALTANTES §17 item 3) to the chassis:
// Wing lifts with forward speed, Thruster pushes along its axis by throttle,
// Buoyancy floats when the module goes below its water level. All forces are
// computed from the current body state — deterministic per backend.
void VehicleRuntime::apply_propulsion(Physics::PhysicsRuntime& world, float deltaTime) {
    if (propulsion_.empty() || deltaTime <= 0.0f) return;
    Physics::RigidBody* chassis = world.body(chassis_);
    if (!chassis || !chassis->dynamic()) return;

    const glm::mat3 basis = glm::mat3_cast(chassis->rotation);
    const glm::vec3 forward = glm::normalize(basis * glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 up = glm::normalize(basis * glm::vec3(0.0f, 1.0f, 0.0f));
    const float forwardSpeed = glm::dot(chassis->linearVelocity, forward);

    for (const Physics::PropulsionModule& module : propulsion_) {
        const glm::vec3 worldPos = chassis->position + basis * module.localPosition;
        switch (module.kind) {
            case Physics::PropulsionKind::Wing: {
                // Lift = 0.5 * rho * v^2 * area * CL, applied along the wing
                // normal (local up). Only lifts while moving forward.
                const float speed = std::max(0.0f, forwardSpeed);
                const float lift = 0.5f * 1.225f * speed * speed *
                                   module.area * module.liftCoefficient;
                world.apply_impulse_at_point(chassis_, up * lift * deltaTime, worldPos);
                break;
            }
            case Physics::PropulsionKind::Thruster: {
                const float thrust = module.maxForce * std::max(0.0f, input_.throttle);
                world.apply_impulse_at_point(chassis_, basis * module.axis * thrust * deltaTime, worldPos);
                break;
            }
            case Physics::PropulsionKind::Buoyancy: {
                // Archimedes: F = rho * g * V. Ramp in linearly over the last
                // meter of submersion so the surface behaves continuously.
                const float depth = module.waterLevel - worldPos.y;
                const float submerged = glm::clamp(depth, 0.0f, 1.0f);
                const float g = glm::length(world.settings().gravity);
                const float force = module.fluidDensity * g * module.maxForce * submerged;
                world.apply_impulse_at_point(chassis_, glm::vec3(0.0f, force, 0.0f) * deltaTime, worldPos);
                break;
            }
        }
    }
}

bool VehicleRuntime::powered() const noexcept {
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

void VehicleRuntime::refuel(float fraction) {
    if (!(power_.fuel.capacity > 0.0f) || !(fraction > 0.0f)) return;
    power_.fuelLevel = std::min(1.0f, power_.fuelLevel + fraction);
}

void VehicleRuntime::recharge(float fraction) {
    if (!(power_.energy.capacity > 0.0f) || !(fraction > 0.0f)) return;
    power_.chargeLevel = std::min(1.0f, power_.chargeLevel + fraction);
}

void VehicleRuntime::set_raw_input(const VehicleInput& raw) {
    set_input(Physics::map_controls(power_.controls, raw));
}

void VehicleRuntime::set_input(const VehicleInput& input) {
    input_.throttle = glm::clamp(input.throttle, -1.0f, 1.0f);
    input_.steering = glm::clamp(input.steering, -1.0f, 1.0f);
    input_.brake = glm::clamp(input.brake, 0.0f, 1.0f);
    input_.handbrake = glm::clamp(input.handbrake, 0.0f, 1.0f);
}

float VehicleRuntime::consume_power(float deltaTime) {
    if (deltaTime <= 0.0f) return 1.0f;
    // Burn fuel by |throttle| (idle burn when the engine is on) and draw /
    // regen energy by throttle/brake. Levels clamp to [0, 1] fractions.
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

bool VehicleRuntime::valid(const Physics::PhysicsRuntime& world) const {
    return chassis_ != Physics::InvalidBody && world.body(chassis_) != nullptr && !wheels_.empty();
}

float VehicleRuntime::speed(const Physics::PhysicsRuntime& world) const {
    const Physics::RigidBody* chassis = world.body(chassis_);
    return chassis ? glm::dot(chassis->linearVelocity, chassis->rotation * glm::vec3(0.0f, 0.0f, -1.0f)) : 0.0f;
}

Physics::VehicleDesc VehicleRuntime::make_vehicle_desc() const {
    Physics::VehicleDesc desc;
    desc.chassis = chassis_;
    desc.forward = {0.0f, 0.0f, -1.0f}; // engine forward convention
    for (const WheelDesc& wheel : wheels_) {
        Physics::WheelDesc w;
        w.localPosition = wheel.localPosition;
        w.radius = wheel.radius;
        w.suspensionRestLength = wheel.suspensionRestLength;
        w.suspensionTravel = wheel.suspensionTravel;
        w.springStrength = wheel.springStrength;
        w.damperStrength = wheel.damperStrength;
        w.tireGrip = wheel.tireGrip;
        w.maxDriveForce = wheel.maxDriveForce;
        w.maxBrakeForce = wheel.maxBrakeForce;
        w.maxSteerAngle = wheel.maxSteerAngle;
        w.steering = wheel.steering;
        w.driven = wheel.driven;
        desc.wheels.push_back(w);
    }
    // Drivetrain override (FALTANTES §17 item 2): explicit engine torque/RPM /
    // transmission / differential from the asset. engineMaxTorque == 0 derives
    // the torque that delivers the wheel's maxDriveForce in first gear:
    // F = T * gear[0] * differentialRatio / radius  =>  T = F * r / (gear[0] * diff),
    // with a 25% margin for auto-shift losses (the legacy acceleration).
    desc.engineMinRPM = std::max(1.0f, drivetrain_.engineMinRPM);
    desc.engineMaxRPM = std::max(desc.engineMinRPM + 1.0f, drivetrain_.engineMaxRPM);
    desc.differentialRatio = std::max(0.01f, drivetrain_.differentialRatio);
    if (!drivetrain_.gearRatios.empty()) {
        desc.gearRatios.clear();
        for (const float ratio : drivetrain_.gearRatios) {
            if (ratio > 0.0f) desc.gearRatios.push_back(ratio);
        }
    }
    if (desc.gearRatios.empty()) desc.gearRatios.push_back(1.0f);
    const float firstGear = desc.gearRatios.front();
    float maxForce = 0.0f;
    float radius = 0.0f;
    for (const Physics::WheelDesc& w : desc.wheels) {
        maxForce = std::max(maxForce, w.maxDriveForce);
        radius = std::max(radius, w.radius);
    }
    if (drivetrain_.engineMaxTorque > 0.0f) {
        desc.engineMaxTorque = drivetrain_.engineMaxTorque;
    } else if (maxForce > 0.0f && radius > 0.0f && firstGear > 0.0f && desc.differentialRatio > 0.0f) {
        desc.engineMaxTorque = maxForce * radius * 1.25f / (firstGear * desc.differentialRatio);
    }
    desc.kind = kind_;
    desc.propulsion = propulsion_;
    // Tracked: split the wheels into the two tracks by local x sign.
    if (kind_ == Physics::VehicleKind::Tracked) {
        desc.tracks.resize(2);
        for (std::size_t i = 0; i < wheels_.size(); ++i) {
            const std::size_t side = wheels_[i].localPosition.x < 0.0f ? 0u : 1u;
            desc.tracks[side].wheelIndices.push_back(i);
        }
    }
    return desc;
}

void VehicleRuntime::update(Physics::PhysicsRuntime& world, float deltaTime, std::uint32_t drivableLayers) {
    if (deltaTime <= 0.0f) return;
    // Power (FALTANTES §17 item 7): burn fuel / draw-or-regen energy by the
    // input, and compute the power scale (0 when a system is below cut-out).
    const float powerScale = consume_power(deltaTime);
    // Propulsion modules apply on every backend (pure forces through the seam).
    apply_propulsion(world, deltaTime);
    if (world.supports_vehicles()) {
        // Jolt Vehicles adapter: create the constraint lazily (the world is
        // only known at update time), push the input, and pull the wheel
        // states the backend produced during the last physics step.
        if (!adapter_ready_) {
            vehicle_ = world.create_vehicle(make_vehicle_desc());
            adapter_ready_ = true;
        }
        if (vehicle_ != Physics::InvalidVehicle) {
            // Damage scales the input (FALTANTES §17 item 9): the Jolt adapter
            // has no per-wheel force hook after creation, so the damaged
            // drivetrain/wheels reduce the effective throttle/brake. Power
            // (item 7) scales the same input when the fuel/energy runs out.
            VehicleInput scaled = input_;
            const float damageScale = drive_scale() * powerScale;
            scaled.throttle *= damageScale;
            scaled.brake *= damageScale;
            world.set_vehicle_input(vehicle_, scaled);
            for (std::size_t i = 0; i < states_.size(); ++i) {
                world.vehicle_wheel_state(vehicle_, i, states_[i]);
            }
        }
        update_occupants(world);
        return;
    }
    update_raycast(world, deltaTime, drivableLayers);
    update_occupants(world);
}

void VehicleRuntime::update_raycast(Physics::PhysicsRuntime& world, float deltaTime, std::uint32_t drivableLayers) {
    Physics::RigidBody* chassis = world.body(chassis_);
    if (!chassis || !chassis->dynamic() || deltaTime <= 0.0f) return;
    const glm::vec3 chassisDown = chassis->rotation * glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 chassisForward = chassis->rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 chassisRight = chassis->rotation * glm::vec3(1.0f, 0.0f, 0.0f);

    const float drivetrainScale = parts_.size() > 1 && parts_[1].maxHealth > 0.0f
        ? parts_[1].health / parts_[1].maxHealth : 1.0f;
    // Power (FALTANTES §17 item 7): without fuel/energy the drive dies even on
    // the legacy raycast path (same scale the Jolt adapter applies).
    const float powerScale = powered() ? 1.0f : 0.0f;
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        const WheelDesc& wheel = wheels_[i];
        WheelState& state = states_[i];
        // Damage (FALTANTES §17 item 9): a SEPARATED wheel pops off — it stops
        // contributing suspension/drive entirely; a damaged wheel scales its
        // grip and drive by its remaining health.
        const Physics::VehiclePartInfo& wheelPart = parts_[2 + i];
        const float wheelScale = wheelPart.maxHealth > 0.0f
            ? wheelPart.health / wheelPart.maxHealth : 0.0f;
        const glm::vec3 mount = chassis->position + chassis->rotation * wheel.localPosition;
        const float castLength = wheel.suspensionRestLength + wheel.suspensionTravel + wheel.radius;
        const auto hit = world.raycast(mount, chassisDown, castLength, drivableLayers, chassis_);
        state.steerAngle = wheel.steering ? input_.steering * wheel.maxSteerAngle : 0.0f;
        state.grounded = hit.has_value();
        state.groundBody = hit ? hit->body : Physics::InvalidBody;
        if (!hit || wheelPart.separated) {
            state.suspensionLength = wheel.suspensionRestLength + wheel.suspensionTravel;
            state.compression = 0.0f;
            state.angularSpeed *= std::max(0.0f, 1.0f - deltaTime * 0.4f);
            state.rotation += state.angularSpeed * deltaTime;
            continue;
        }

        state.contactPoint = hit->point;
        state.contactNormal = hit->normal;
        state.suspensionLength = glm::clamp(hit->distance - wheel.radius,
                                            wheel.suspensionRestLength - wheel.suspensionTravel,
                                            wheel.suspensionRestLength + wheel.suspensionTravel);
        state.compression = wheel.suspensionRestLength - state.suspensionLength;
        const glm::vec3 r = state.contactPoint - chassis->position;
        glm::vec3 pointVelocity = chassis->linearVelocity + glm::cross(chassis->angularVelocity, r);
        if (const Physics::RigidBody* ground = world.body(hit->body)) {
            pointVelocity -= ground->linearVelocity + glm::cross(ground->angularVelocity, state.contactPoint - ground->position);
        }
        const float suspensionVelocity = glm::dot(pointVelocity, chassisDown);
        const float springForce = std::max(0.0f, state.compression * wheel.springStrength + suspensionVelocity * wheel.damperStrength);
        const glm::vec3 suspensionImpulse = -chassisDown * springForce * deltaTime;
        world.apply_impulse_at_point(chassis_, suspensionImpulse, state.contactPoint);
        if (Physics::RigidBody* ground = world.body(hit->body); ground && ground->dynamic()) {
            world.apply_impulse_at_point(hit->body, -suspensionImpulse, state.contactPoint);
        }

        const float cosine = std::cos(state.steerAngle), sine = std::sin(state.steerAngle);
        const glm::vec3 steeredForward = normalized_or(chassisForward * cosine + chassisRight * sine, chassisForward);
        const glm::vec3 steeredRight = normalized_or(glm::cross(steeredForward, state.contactNormal), chassisRight);
        const float lateralSpeed = glm::dot(pointVelocity, steeredRight);
        const float chassisMass = chassis->inverseMass > 0.0f ? 1.0f / chassis->inverseMass : 0.0f;
        const float grip = wheel.tireGrip * wheelScale *
                           (1.0f - (wheel.driven ? input_.handbrake * 0.85f : 0.0f));
        const float lateralImpulseMagnitude = glm::clamp(-lateralSpeed * chassisMass * grip,
                                                         -springForce * deltaTime * grip,
                                                          springForce * deltaTime * grip);
        world.apply_impulse_at_point(chassis_, steeredRight * lateralImpulseMagnitude, state.contactPoint);

        float longitudinalForce = wheel.driven
            ? input_.throttle * wheel.maxDriveForce * wheelScale * drivetrainScale * powerScale
            : 0.0f;
        const float longitudinalSpeed = glm::dot(pointVelocity, steeredForward);
        const float braking = (input_.brake + (wheel.driven ? input_.handbrake : 0.0f)) *
                              wheel.maxBrakeForce * wheelScale * drivetrainScale * powerScale;
        if (braking > 0.0f) longitudinalForce -= std::copysign(std::min(braking, std::abs(longitudinalSpeed) * chassisMass / deltaTime), longitudinalSpeed);
        const glm::vec3 driveImpulse = steeredForward * longitudinalForce * deltaTime;
        world.apply_impulse_at_point(chassis_, driveImpulse, state.contactPoint);
        if (Physics::RigidBody* ground = world.body(hit->body); ground && ground->dynamic()) {
            world.apply_impulse_at_point(hit->body, -driveImpulse, state.contactPoint);
        }
        state.angularSpeed = wheel.radius > 0.001f ? longitudinalSpeed / wheel.radius : 0.0f;
        state.rotation = std::fmod(state.rotation + state.angularSpeed * deltaTime, glm::two_pi<float>());
    }
}

} // namespace Engine::Gameplay

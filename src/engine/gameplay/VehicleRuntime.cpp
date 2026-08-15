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
}

VehicleRuntime::VehicleRuntime(Physics::BodyHandle chassis, std::vector<WheelDesc> wheels)
    : chassis_(chassis), wheels_(std::move(wheels)), states_(wheels_.size()) {
    for (std::size_t i = 0; i < wheels_.size(); ++i) states_[i].suspensionLength = wheels_[i].suspensionRestLength;
}

void VehicleRuntime::set_input(const VehicleInput& input) {
    input_.throttle = glm::clamp(input.throttle, -1.0f, 1.0f);
    input_.steering = glm::clamp(input.steering, -1.0f, 1.0f);
    input_.brake = glm::clamp(input.brake, 0.0f, 1.0f);
    input_.handbrake = glm::clamp(input.handbrake, 0.0f, 1.0f);
}

bool VehicleRuntime::valid(const Physics::PhysicsRuntime& world) const {
    return chassis_ != Physics::InvalidBody && world.body(chassis_) != nullptr && !wheels_.empty();
}

float VehicleRuntime::speed(const Physics::PhysicsRuntime& world) const {
    const Physics::RigidBody* chassis = world.body(chassis_);
    return chassis ? glm::dot(chassis->linearVelocity, chassis->rotation * glm::vec3(0.0f, 0.0f, -1.0f)) : 0.0f;
}

void VehicleRuntime::update(Physics::PhysicsRuntime& world, float deltaTime, std::uint32_t drivableLayers) {
    Physics::RigidBody* chassis = world.body(chassis_);
    if (!chassis || !chassis->dynamic() || deltaTime <= 0.0f) return;
    const glm::vec3 chassisDown = chassis->rotation * glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 chassisForward = chassis->rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 chassisRight = chassis->rotation * glm::vec3(1.0f, 0.0f, 0.0f);

    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        const WheelDesc& wheel = wheels_[i];
        WheelState& state = states_[i];
        const glm::vec3 mount = chassis->position + chassis->rotation * wheel.localPosition;
        const float castLength = wheel.suspensionRestLength + wheel.suspensionTravel + wheel.radius;
        const auto hit = world.raycast(mount, chassisDown, castLength, drivableLayers, chassis_);
        state.steerAngle = wheel.steering ? input_.steering * wheel.maxSteerAngle : 0.0f;
        state.grounded = hit.has_value();
        state.groundBody = hit ? hit->body : Physics::InvalidBody;
        if (!hit) {
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
        const float grip = wheel.tireGrip * (1.0f - (wheel.driven ? input_.handbrake * 0.85f : 0.0f));
        const float lateralImpulseMagnitude = glm::clamp(-lateralSpeed * chassisMass * grip,
                                                         -springForce * deltaTime * grip,
                                                          springForce * deltaTime * grip);
        world.apply_impulse_at_point(chassis_, steeredRight * lateralImpulseMagnitude, state.contactPoint);

        float longitudinalForce = wheel.driven ? input_.throttle * wheel.maxDriveForce : 0.0f;
        const float longitudinalSpeed = glm::dot(pointVelocity, steeredForward);
        const float braking = (input_.brake + (wheel.driven ? input_.handbrake : 0.0f)) * wheel.maxBrakeForce;
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

#pragma once

#include "../physics/PhysicsRuntime.hpp"

#include <vector>

namespace Engine::Gameplay {

struct WheelDesc {
    glm::vec3 localPosition{0.0f};
    float radius{0.36f};
    float suspensionRestLength{0.45f};
    float suspensionTravel{0.18f};
    float springStrength{26000.0f};
    float damperStrength{3200.0f};
    float tireGrip{1.35f};
    float maxDriveForce{4200.0f};
    float maxBrakeForce{6000.0f};
    float maxSteerAngle{0.55f};
    bool steering{false};
    bool driven{true};
};

struct WheelState {
    bool grounded{false};
    Physics::BodyHandle groundBody{Physics::InvalidBody};
    glm::vec3 contactPoint{0.0f};
    glm::vec3 contactNormal{0.0f, 1.0f, 0.0f};
    float suspensionLength{0.0f};
    float compression{0.0f};
    float rotation{0.0f};
    float angularSpeed{0.0f};
    float steerAngle{0.0f};
};

struct VehicleInput {
    float throttle{0.0f};
    float steering{0.0f};
    float brake{0.0f};
    float handbrake{0.0f};
};

class VehicleRuntime final {
public:
    VehicleRuntime(Physics::BodyHandle chassis, std::vector<WheelDesc> wheels);
    void set_input(const VehicleInput& input);
    void update(Physics::PhysicsRuntime& world, float deltaTime, std::uint32_t drivableLayers = ~0u);

    Physics::BodyHandle chassis() const noexcept { return chassis_; }
    const std::vector<WheelDesc>& wheels() const noexcept { return wheels_; }
    const std::vector<WheelState>& wheel_states() const noexcept { return states_; }
    float speed(const Physics::PhysicsRuntime& world) const;
    bool valid(const Physics::PhysicsRuntime& world) const;

private:
    Physics::BodyHandle chassis_{Physics::InvalidBody};
    std::vector<WheelDesc> wheels_;
    std::vector<WheelState> states_;
    VehicleInput input_{};
};

} // namespace Engine::Gameplay

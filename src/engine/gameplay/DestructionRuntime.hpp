#pragma once

#include "../physics/PhysicsRuntime.hpp"

#include <cstdint>
#include <vector>

namespace Engine::Gameplay {

struct DestructionChunkDesc {
    glm::vec3 localPosition{0.0f};
    glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 halfExtents{0.25f};
    float mass{1.0f};
    float health{25.0f};
    float damageResistance{0.0f};
    std::uint32_t materialIndex{0};
};

struct DestructionChunkState {
    Physics::BodyHandle body{Physics::InvalidBody};
    float health{0.0f};
    float mass{1.0f};
    bool detached{false};
    std::uint32_t materialIndex{0};
};

struct DestructionEvent {
    std::size_t chunkIndex{0};
    Physics::BodyHandle body{Physics::InvalidBody};
    glm::vec3 position{0.0f};
    glm::vec3 impulse{0.0f};
};

class DestructibleRuntime final {
public:
    bool create(Physics::PhysicsRuntime& world, const glm::vec3& position, const glm::quat& rotation,
                const std::vector<DestructionChunkDesc>& chunks, std::uint32_t layer = 1u,
                std::uint32_t mask = ~0u);
    void destroy(Physics::PhysicsRuntime& world);
    std::vector<DestructionEvent> apply_radial_damage(Physics::PhysicsRuntime& world,
                                                      const glm::vec3& origin, float radius,
                                                      float damage, float impulseStrength);
    bool detach_chunk(Physics::PhysicsRuntime& world, std::size_t chunkIndex,
                      const glm::vec3& impulse = glm::vec3(0.0f));
    void reset(Physics::PhysicsRuntime& world);

    const std::vector<DestructionChunkState>& chunks() const noexcept { return states_; }
    bool fully_destroyed() const noexcept;

private:
    glm::vec3 rootPosition_{0.0f};
    glm::quat rootRotation_{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<DestructionChunkDesc> descriptions_;
    std::vector<DestructionChunkState> states_;
};

} // namespace Engine::Gameplay

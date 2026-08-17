#include "DestructionRuntime.hpp"

#include <algorithm>
#include <cmath>

namespace Engine::Gameplay {
namespace {
glm::vec3 direction_or_up(const glm::vec3& value) {
    const float squared = glm::dot(value, value);
    return squared > 1.0e-8f ? value / std::sqrt(squared) : glm::vec3(0.0f, 1.0f, 0.0f);
}
}

bool DestructibleRuntime::create(Physics::PhysicsRuntime& world, const glm::vec3& position,
                                 const glm::quat& rotation, const std::vector<DestructionChunkDesc>& chunks,
                                 std::uint32_t layer, std::uint32_t mask) {
    if (!states_.empty() || chunks.empty()) return false;
    for (const auto& chunk : chunks) {
        if (chunk.mass <= 0.0f || chunk.health <= 0.0f || glm::any(glm::lessThanEqual(chunk.halfExtents, glm::vec3(0.0f)))) return false;
    }
    rootPosition_ = position;
    rootRotation_ = glm::normalize(rotation);
    descriptions_ = chunks;
    states_.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        Physics::BodyDesc body;
        body.motion = Physics::MotionType::Kinematic;
        body.position = rootPosition_ + rootRotation_ * chunk.localPosition;
        body.rotation = rootRotation_ * chunk.localRotation;
        body.mass = chunk.mass;
        body.collider.shape = Physics::BoxShape{chunk.halfExtents};
        body.collider.friction = 0.72f;
        body.collider.restitution = 0.08f;
        body.collider.filter = {layer, mask};
        const auto handle = world.create_body(body);
        if (handle == Physics::InvalidBody) { destroy(world); return false; }
        states_.push_back({handle, chunk.health, chunk.mass, false, chunk.materialIndex});
    }
    return true;
}

void DestructibleRuntime::destroy(Physics::PhysicsRuntime& world) {
    for (const auto& chunk : states_) world.destroy_body(chunk.body);
    descriptions_.clear(); states_.clear();
}

bool DestructibleRuntime::detach_chunk(Physics::PhysicsRuntime& world, std::size_t chunkIndex, const glm::vec3& impulse) {
    if (chunkIndex >= states_.size()) return false;
    DestructionChunkState& chunk = states_[chunkIndex];
    if (chunk.detached) return false;
    Physics::RigidBody* body = world.body(chunk.body);
    if (!body) return false;
    chunk.detached = true;
    body->continuous = true;
    body->allowSleep = true;
    // Kinematic -> Dynamic through the runtime: the mirror AND the external
    // backend (Jolt) both flip, so the debris actually falls under gravity in
    // the standard world (SetMotionType + Activate), then the impulse pushes
    // it out of the fracture.
    world.set_motion(chunk.body, Physics::MotionType::Dynamic, chunk.mass);
    world.apply_impulse(chunk.body, impulse);
    return true;
}

std::vector<DestructionEvent> DestructibleRuntime::apply_radial_damage(Physics::PhysicsRuntime& world,
                                                                       const glm::vec3& origin, float radius,
                                                                       float damage, float impulseStrength) {
    std::vector<DestructionEvent> events;
    if (radius <= 0.0f || damage <= 0.0f) return events;
    for (std::size_t i = 0; i < states_.size(); ++i) {
        DestructionChunkState& state = states_[i];
        Physics::RigidBody* body = world.body(state.body);
        if (!body) continue;
        const glm::vec3 offset = body->position - origin;
        const float distance = std::sqrt(glm::dot(offset, offset));
        if (distance > radius) continue;
        const float falloff = 1.0f - distance / radius;
        const float effectiveDamage = std::max(0.0f, damage * falloff - descriptions_[i].damageResistance);
        state.health = std::max(0.0f, state.health - effectiveDamage);
        if (state.health <= 0.0f && !state.detached) {
            const glm::vec3 impulse = direction_or_up(offset) * impulseStrength * falloff;
            if (detach_chunk(world, i, impulse)) events.push_back({i, state.body, body->position, impulse});
        } else if (state.detached && impulseStrength > 0.0f) {
            world.apply_impulse(state.body, direction_or_up(offset) * impulseStrength * falloff);
        }
    }
    return events;
}

void DestructibleRuntime::reset(Physics::PhysicsRuntime& world) {
    for (std::size_t i = 0; i < states_.size(); ++i) {
        auto& state = states_[i];
        state.health = descriptions_[i].health;
        state.detached = false;
        if (Physics::RigidBody* body = world.body(state.body)) {
            body->motion = Physics::MotionType::Kinematic;
            body->inverseMass = 0.0f;
            body->position = rootPosition_ + rootRotation_ * descriptions_[i].localPosition;
            body->previousPosition = body->position;
            body->rotation = rootRotation_ * descriptions_[i].localRotation;
            body->linearVelocity = {};
            body->angularVelocity = {};
            body->sleeping = false;
        }
    }
}

bool DestructibleRuntime::fully_destroyed() const noexcept {
    return !states_.empty() && std::all_of(states_.begin(), states_.end(), [](const auto& state) { return state.detached; });
}

} // namespace Engine::Gameplay

#include "Physics.hpp"
#include "../core/serialization/Serializer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace Engine {

bool PhysicsMaterialAsset::save_to_file(const std::filesystem::path& path) const {
    return Serializer::serialize_physics_material(*this, path).success;
}

bool PhysicsMaterialAsset::load_from_file(const std::filesystem::path& path) {
    return Serializer::deserialize_physics_material(*this, path).success;
}

void PhysicsWorld::step(Scene& scene, float deltaTime) {
    if (deltaTime <= 0.0f) return;
    for (auto& [id, rb] : scene.rigidbodyComponents) {
        if (rb.isKinematic) continue;
        auto transformIt = scene.transformComponents.find(id);
        if (transformIt == scene.transformComponents.end()) continue;

        auto& transform = transformIt->second;
        if (rb.useGravity) {
            transform.position += m_gravity * (0.5f * deltaTime * deltaTime);
        }
        if (transform.position.y < 0.0f) {
            transform.position.y = 0.0f;
        }
    }
}

RaycastHit PhysicsWorld::raycast(const Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    RaycastHit closestHit;
    closestHit.hit = false;
    closestHit.distance = maxDistance;

    const glm::vec3 dirNorm = glm::length(direction) > 0.0001f ? glm::normalize(direction) : glm::vec3(0.0f, 0.0f, -1.0f);
    for (const auto& [id, transform] : scene.transformComponents) {
        const glm::vec3 pos = transform.position;
        const float radius = 1.0f * std::max({ transform.scale.x, transform.scale.y, transform.scale.z });
        const glm::vec3 L = pos - origin;
        const float tca = glm::dot(L, dirNorm);
        if (tca < 0.0f) continue;
        const float d2 = glm::dot(L, L) - tca * tca;
        const float r2 = radius * radius;
        if (d2 > r2) continue;
        const float thc = std::sqrt(r2 - d2);
        const float t0 = tca - thc;
        if (t0 > 0.0f && t0 < closestHit.distance) {
            closestHit.hit = true;
            closestHit.distance = t0;
            closestHit.point = origin + dirNorm * t0;
            closestHit.normal = glm::length(closestHit.point - pos) > 0.0001f ? glm::normalize(closestHit.point - pos) : glm::vec3(0.0f, 1.0f, 0.0f);
            closestHit.entityID = id;
        }
    }
    return closestHit;
}

} // namespace Engine

#pragma once

#include <glm/glm.hpp>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include "../core/uuid/UUID.hpp"
#include "../scene/Scene.hpp"

namespace Engine {

struct RaycastHit {
    bool hit{ false };
    float distance{ 0.0f };
    glm::vec3 point{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    UUID entityID{ 0, 0 };
};

struct PhysicsMaterialAsset {
    UUID id;
    std::string name{ "Default Physics Material" };
    float friction{ 0.5f };
    float restitution{ 0.1f };
    float density{ 1000.0f };
    float hardness{ 0.5f };

    bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);
};

class PhysicsWorld final {
public:
    PhysicsWorld() = default;
    ~PhysicsWorld() = default;

    void set_gravity(const glm::vec3& gravity) { m_gravity = gravity; }
    glm::vec3 get_gravity() const { return m_gravity; }

    void step(Scene& scene, float deltaTime);

    RaycastHit raycast(const Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;

private:
    glm::vec3 m_gravity{ 0.0f, -9.81f, 0.0f };
};

} // namespace Engine

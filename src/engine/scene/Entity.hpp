#pragma once

#include "../core/uuid/UUID.hpp"
#include <string>

namespace Engine {

class Scene;

class Entity {
public:
    Entity() = default;
    Entity(UUID id, Scene* scene, const std::string& name = "Entity")
        : m_id(id), m_scene(scene), m_name(name) {}

    UUID get_id() const { return m_id; }
    const std::string& get_name() const { return m_name; }
    void set_name(const std::string& name) { m_name = name; }
    Scene* get_scene() const { return m_scene; }

    bool is_valid() const { return m_id.is_valid() && m_scene != nullptr; }

    bool operator==(const Entity& other) const {
        return m_id == other.m_id && m_scene == other.m_scene;
    }

private:
    friend class Scene;
    void bind_scene(Scene* scene) noexcept { m_scene = scene; }
    UUID m_id{ 0, 0 };
    Scene* m_scene{ nullptr };
    std::string m_name{ "Entity" };
};

} // namespace Engine

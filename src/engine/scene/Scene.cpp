// ---------------------------------------------------------------------------
// Scene.cpp
//
// Implementation of Scene: entity management, typed component stores,
// and the new generic (type-erased) component storage layer.
// ---------------------------------------------------------------------------

#include "Scene.hpp"
#include "ComponentRegistry.hpp"
#include "../core/serialization/Serializer.hpp"

#include <algorithm>
#include <cassert>

namespace Engine {

// ---------------------------------------------------------------------------
// Constructors / Assignment
// ---------------------------------------------------------------------------

Scene::Scene(const std::string& name)
    : m_id(), m_name(name) {
    register_builtin_component_reflection();
}

Scene::Scene(Scene&& other) noexcept
    : m_id(other.m_id),
      m_name(std::move(other.m_name)),
      m_entities(std::move(other.m_entities)),
      transformComponents(std::move(other.transformComponents)),
      meshRendererComponents(std::move(other.meshRendererComponents)),
      lightComponents(std::move(other.lightComponents)),
      cameraComponents(std::move(other.cameraComponents)),
      rigidbodyComponents(std::move(other.rigidbodyComponents)),
      weaponComponents(std::move(other.weaponComponents)),
      particleEmitterComponents(std::move(other.particleEmitterComponents)),
      vehicleComponents(std::move(other.vehicleComponents)),
      ragdollComponents(std::move(other.ragdollComponents)),
      missionComponents(std::move(other.missionComponents)),
      dialogueComponents(std::move(other.dialogueComponents)),
      destructionComponents(std::move(other.destructionComponents)),
      navigationComponents(std::move(other.navigationComponents)),
      audioComponents(std::move(other.audioComponents)),
      materialComponents(std::move(other.materialComponents)),
      hierarchyComponents(std::move(other.hierarchyComponents)),
      voxelVolumeComponents(std::move(other.voxelVolumeComponents)),
      colliderComponents(std::move(other.colliderComponents)),
      constraintComponents(std::move(other.constraintComponents)),
      softBodyComponents(std::move(other.softBodyComponents)),
      springComponents(std::move(other.springComponents)),
      decalComponents(std::move(other.decalComponents)),
      splineComponents(std::move(other.splineComponents)),
      forceFieldComponents(std::move(other.forceFieldComponents)),
      envProbeComponents(std::move(other.envProbeComponents)),
      weatherComponents(std::move(other.weatherComponents)),
      hairParticleComponents(std::move(other.hairParticleComponents)),
      m_genericComponents(std::move(other.m_genericComponents)) {
    rebind_entities();
}

Scene& Scene::operator=(Scene&& other) noexcept {
    if (this == &other) return *this;
    m_id                   = other.m_id;
    m_name                 = std::move(other.m_name);
    m_entities             = std::move(other.m_entities);
    transformComponents    = std::move(other.transformComponents);
    meshRendererComponents = std::move(other.meshRendererComponents);
    lightComponents        = std::move(other.lightComponents);
    cameraComponents       = std::move(other.cameraComponents);
    rigidbodyComponents    = std::move(other.rigidbodyComponents);
    weaponComponents       = std::move(other.weaponComponents);
    particleEmitterComponents = std::move(other.particleEmitterComponents);
    vehicleComponents      = std::move(other.vehicleComponents);
    ragdollComponents      = std::move(other.ragdollComponents);
    missionComponents      = std::move(other.missionComponents);
    dialogueComponents     = std::move(other.dialogueComponents);
    destructionComponents  = std::move(other.destructionComponents);
    navigationComponents   = std::move(other.navigationComponents);
    audioComponents        = std::move(other.audioComponents);
    materialComponents     = std::move(other.materialComponents);
    hierarchyComponents    = std::move(other.hierarchyComponents);
    voxelVolumeComponents  = std::move(other.voxelVolumeComponents);
    colliderComponents     = std::move(other.colliderComponents);
    constraintComponents   = std::move(other.constraintComponents);
    softBodyComponents     = std::move(other.softBodyComponents);
    springComponents       = std::move(other.springComponents);
    decalComponents        = std::move(other.decalComponents);
    splineComponents       = std::move(other.splineComponents);
    forceFieldComponents   = std::move(other.forceFieldComponents);
    envProbeComponents     = std::move(other.envProbeComponents);
    weatherComponents      = std::move(other.weatherComponents);
    hairParticleComponents = std::move(other.hairParticleComponents);
    m_genericComponents    = std::move(other.m_genericComponents);
    rebind_entities();
    return *this;
}

void Scene::rebind_entities() noexcept {
    for (auto& [id, entity] : m_entities) entity.bind_scene(this);
}

// ---------------------------------------------------------------------------
// Entity management (unchanged logic, extended to clean up generics)
// ---------------------------------------------------------------------------

Entity Scene::create_entity(const std::string& name) {
    UUID id;
    return create_entity_with_id(id, name);
}

Entity Scene::create_entity_with_id(UUID id, const std::string& name) {
    Entity entity(id, this, name);
    m_entities[id] = entity;
    transformComponents[id] = TransformComponent{};
    return entity;
}

void Scene::destroy_entity(UUID id) {
    // Handle hierarchy reparenting
    UUID parentID = get_parent(id);
    std::vector<UUID> children = get_children(id);
    for (UUID childID : children) {
        set_parent(childID, parentID);
    }
    set_parent(id, UUID{ 0, 0 });

    // Remove from typed component stores
    m_entities.erase(id);
    transformComponents.erase(id);
    meshRendererComponents.erase(id);
    lightComponents.erase(id);
    cameraComponents.erase(id);
    rigidbodyComponents.erase(id);
    weaponComponents.erase(id);
    particleEmitterComponents.erase(id);
    vehicleComponents.erase(id);
    ragdollComponents.erase(id);
    missionComponents.erase(id);
    dialogueComponents.erase(id);
    destructionComponents.erase(id);
    navigationComponents.erase(id);
    audioComponents.erase(id);
    materialComponents.erase(id);
    hierarchyComponents.erase(id);
    voxelVolumeComponents.erase(id);

    // Remove all generic components for this entity
    clear_generic_components(id);
}

void Scene::rename_entity(UUID id, const std::string& name) {
    auto it = m_entities.find(id);
    if (it != m_entities.end()) {
        it->second.set_name(name);
    }
}

// ---------------------------------------------------------------------------
// Hierarchy management (unchanged)
// ---------------------------------------------------------------------------

void Scene::set_parent(UUID childID, UUID newParentID) {
    if (!m_entities.contains(childID)) return;
    if (childID == newParentID) return;

    // Detect cycles
    UUID current = newParentID;
    while (current.is_valid()) {
        if (current == childID) return;
        current = get_parent(current);
    }

    // Remove from old parent's children list
    UUID oldParentID = get_parent(childID);
    if (oldParentID.is_valid() && hierarchyComponents.contains(oldParentID)) {
        auto& oldChildren = hierarchyComponents[oldParentID].childrenIDs;
        std::erase(oldChildren, childID);
    }

    // Add to new parent's children list
    if (newParentID.is_valid() && m_entities.contains(newParentID)) {
        hierarchyComponents[childID].parentID = newParentID;
        auto& newChildren = hierarchyComponents[newParentID].childrenIDs;
        if (std::find(newChildren.begin(), newChildren.end(), childID) == newChildren.end()) {
            newChildren.push_back(childID);
        }
    } else {
        hierarchyComponents[childID].parentID = UUID{ 0, 0 };
    }
}

UUID Scene::get_parent(UUID childID) const {
    auto it = hierarchyComponents.find(childID);
    if (it != hierarchyComponents.end()) {
        return it->second.parentID;
    }
    return UUID{ 0, 0 };
}

std::vector<UUID> Scene::get_children(UUID parentID) const {
    auto it = hierarchyComponents.find(parentID);
    if (it != hierarchyComponents.end()) {
        return it->second.childrenIDs;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Entity lookup (unchanged)
// ---------------------------------------------------------------------------

Entity Scene::find_entity_by_id(UUID id) {
    auto it = m_entities.find(id);
    if (it != m_entities.end()) return it->second;
    return Entity();
}

const Entity* Scene::find_entity_by_id_const(UUID id) const {
    auto it = m_entities.find(id);
    if (it != m_entities.end()) return &it->second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Clone (extended to include generic components)
// ---------------------------------------------------------------------------

Scene Scene::clone_for_play() const {
    Scene clone(m_name + " [PLAY]");
    clone.m_id = m_id;

    // Clone entities
    for (const auto& [id, entity] : m_entities) {
        clone.create_entity_with_id(id, entity.get_name());
    }

    // Clone typed component stores
    clone.transformComponents    = transformComponents;
    clone.meshRendererComponents = meshRendererComponents;
    clone.lightComponents        = lightComponents;
    clone.cameraComponents       = cameraComponents;
    clone.rigidbodyComponents    = rigidbodyComponents;
    clone.weaponComponents       = weaponComponents;
    clone.particleEmitterComponents = particleEmitterComponents;
    clone.vehicleComponents      = vehicleComponents;
    clone.ragdollComponents      = ragdollComponents;
    clone.missionComponents      = missionComponents;
    clone.dialogueComponents     = dialogueComponents;
    clone.destructionComponents  = destructionComponents;
    clone.navigationComponents   = navigationComponents;
    clone.audioComponents        = audioComponents;
    clone.materialComponents     = materialComponents;
    clone.hierarchyComponents    = hierarchyComponents;
    clone.voxelVolumeComponents  = voxelVolumeComponents;
    clone.colliderComponents     = colliderComponents;
    clone.constraintComponents   = constraintComponents;
    clone.softBodyComponents     = softBodyComponents;
    clone.springComponents       = springComponents;
    clone.decalComponents        = decalComponents;
    clone.splineComponents       = splineComponents;
    clone.forceFieldComponents   = forceFieldComponents;
    clone.envProbeComponents     = envProbeComponents;
    clone.weatherComponents      = weatherComponents;
    clone.hairParticleComponents = hairParticleComponents;

    // Clone generic component stores
    for (const auto& [typeId, entityMap] : m_genericComponents) {
        for (const auto& [entityId, entry] : entityMap) {
            if (entry.is_valid()) {
                GenericComponentEntry clonedEntry = entry.clone();
                if (clonedEntry.is_valid()) {
                    clone.m_genericComponents[typeId][entityId] = std::move(clonedEntry);
                }
            }
        }
    }

    return clone;
}

// ---------------------------------------------------------------------------
// Serialization (unchanged)
// ---------------------------------------------------------------------------

bool Scene::save_to_file(const std::string& filePath) {
    return Serializer::serialize_scene(*this, filePath).success;
}

bool Scene::load_from_file(const std::string& filePath) {
    return Serializer::deserialize_scene(*this, filePath).success;
}

// ---------------------------------------------------------------------------
// Generic Component Storage
// ---------------------------------------------------------------------------

void* Scene::add_generic_component(UUID entityId, TypeId typeId) {
    // Check if already exists
    auto typeIt = m_genericComponents.find(typeId);
    if (typeIt != m_genericComponents.end()) {
        auto entityIt = typeIt->second.find(entityId);
        if (entityIt != typeIt->second.end()) {
            // Component already exists — return existing data
            return entityIt->second.data;
        }
    }

    // Look up the descriptor to create a new component
    const auto* desc = ComponentRegistry::get().find(typeId);
    if (!desc || !desc->factory) {
        return nullptr; // Unknown component type
    }

    // Create a new default-constructed component
    void* componentData = desc->factory();
    if (!componentData) {
        return nullptr;
    }

    // Store in the generic map
    GenericComponentEntry entry(componentData, typeId);
    void* rawPtr = entry.data;
    m_genericComponents[typeId][entityId] = std::move(entry);

    // Invoke onAdd callback if registered
    if (desc->onAdd) {
        desc->onAdd(this, entityId, rawPtr);
    }

    return rawPtr;
}

bool Scene::remove_generic_component(UUID entityId, TypeId typeId) {
    auto typeIt = m_genericComponents.find(typeId);
    if (typeIt == m_genericComponents.end()) {
        return false;
    }

    auto entityIt = typeIt->second.find(entityId);
    if (entityIt == typeIt->second.end()) {
        return false;
    }

    // Invoke onRemove callback if registered
    const auto* desc = ComponentRegistry::get().find(typeId);
    if (desc && desc->onRemove && entityIt->second.data) {
        desc->onRemove(this, entityId, entityIt->second.data);
    }

    // Erase the entry (destructor runs via GenericComponentEntry dtor)
    typeIt->second.erase(entityIt);

    // Clean up empty type maps
    if (typeIt->second.empty()) {
        m_genericComponents.erase(typeIt);
    }

    return true;
}

bool Scene::has_generic_component(UUID entityId, TypeId typeId) const {
    auto typeIt = m_genericComponents.find(typeId);
    if (typeIt == m_genericComponents.end()) {
        return false;
    }
    return typeIt->second.count(entityId) > 0;
}

void* Scene::get_generic_component(UUID entityId, TypeId typeId) {
    auto typeIt = m_genericComponents.find(typeId);
    if (typeIt == m_genericComponents.end()) {
        return nullptr;
    }
    auto entityIt = typeIt->second.find(entityId);
    if (entityIt == typeIt->second.end()) {
        return nullptr;
    }
    return entityIt->second.data;
}

const void* Scene::get_generic_component(UUID entityId, TypeId typeId) const {
    auto typeIt = m_genericComponents.find(typeId);
    if (typeIt == m_genericComponents.end()) {
        return nullptr;
    }
    auto entityIt = typeIt->second.find(entityId);
    if (entityIt == typeIt->second.end()) {
        return nullptr;
    }
    return entityIt->second.data;
}

std::vector<std::pair<TypeId, void*>> Scene::get_all_generic_components(UUID entityId) {
    std::vector<std::pair<TypeId, void*>> result;
    for (auto& [typeId, entityMap] : m_genericComponents) {
        auto entityIt = entityMap.find(entityId);
        if (entityIt != entityMap.end() && entityIt->second.data) {
            result.emplace_back(typeId, entityIt->second.data);
        }
    }
    return result;
}

std::vector<std::pair<TypeId, const void*>> Scene::get_all_generic_components(UUID entityId) const {
    std::vector<std::pair<TypeId, const void*>> result;
    for (const auto& [typeId, entityMap] : m_genericComponents) {
        auto entityIt = entityMap.find(entityId);
        if (entityIt != entityMap.end() && entityIt->second.data) {
            result.emplace_back(typeId, entityIt->second.data);
        }
    }
    return result;
}

void Scene::clone_entity_generic_components(UUID source, UUID dest) {
    for (const auto& [typeId, entityMap] : m_genericComponents) {
        auto srcIt = entityMap.find(source);
        if (srcIt == entityMap.end() || !srcIt->second.is_valid()) {
            continue;
        }

        // Skip if dest already has this component type
        auto destTypeIt = m_genericComponents.find(typeId);
        if (destTypeIt != m_genericComponents.end()) {
            if (destTypeIt->second.count(dest) > 0) {
                continue;
            }
        }

        // Clone the component
        GenericComponentEntry clonedEntry = srcIt->second.clone();
        if (clonedEntry.is_valid()) {
            void* clonedPtr = clonedEntry.data;
            m_genericComponents[typeId][dest] = std::move(clonedEntry);

            // Invoke onAdd callback for the cloned component
            const auto* desc = ComponentRegistry::get().find(typeId);
            if (desc && desc->onAdd) {
                desc->onAdd(this, dest, clonedPtr);
            }
        }
    }
}

void Scene::clear_generic_components(UUID entityId) {
    for (auto typeIt = m_genericComponents.begin(); typeIt != m_genericComponents.end(); ) {
        auto entityIt = typeIt->second.find(entityId);
        if (entityIt != typeIt->second.end()) {
            // Invoke onRemove callback
            const auto* desc = ComponentRegistry::get().find(typeIt->first);
            if (desc && desc->onRemove && entityIt->second.data) {
                desc->onRemove(this, entityId, entityIt->second.data);
            }

            typeIt->second.erase(entityIt);

            // Clean up empty type maps
            if (typeIt->second.empty()) {
                typeIt = m_genericComponents.erase(typeIt);
                continue;
            }
        }
        ++typeIt;
    }
}

size_t Scene::get_generic_component_type_count() const {
    return m_genericComponents.size();
}

std::vector<UUID> Scene::get_entities_with_generic_component(TypeId typeId) const {
    std::vector<UUID> result;
    auto typeIt = m_genericComponents.find(typeId);
    if (typeIt != m_genericComponents.end()) {
        result.reserve(typeIt->second.size());
        for (const auto& [entityId, entry] : typeIt->second) {
            result.push_back(entityId);
        }
    }
    return result;
}

} // namespace Engine

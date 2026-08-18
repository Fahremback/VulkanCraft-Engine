#pragma once

// ---------------------------------------------------------------------------
// Scene.hpp
//
// Scene graph with typed component stores (legacy) AND generic type-erased
// component storage keyed by TypeId. The generic storage enables plugins and
// future component types to be added at runtime without recompiling the
// engine core.
// ---------------------------------------------------------------------------

#include "Entity.hpp"
#include "Components.hpp"
#include "ComponentRegistry.hpp"
#include "../core/uuid/UUID.hpp"
#include "../core/reflection/Reflection.hpp"

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <any>
#include <utility>
#include <cassert>

namespace Engine {

class Serializer;

// ---------------------------------------------------------------------------
// GenericComponentEntry: type-erased component stored in the generic map.
// Wraps a void* plus the TypeId so that the correct destructor can be found
// through the ComponentRegistry when the entry is destroyed.
// ---------------------------------------------------------------------------

struct GenericComponentEntry {
    void*  data{ nullptr };
    TypeId typeId{ 0 };

    GenericComponentEntry() = default;

    GenericComponentEntry(void* d, TypeId t)
        : data(d), typeId(t) {}

    // Move-only to prevent double-free
    GenericComponentEntry(GenericComponentEntry&& other) noexcept
        : data(other.data), typeId(other.typeId) {
        other.data   = nullptr;
        other.typeId = 0;
    }

    GenericComponentEntry& operator=(GenericComponentEntry&& other) noexcept {
        if (this != &other) {
            destroy();
            data   = other.data;
            typeId = other.typeId;
            other.data   = nullptr;
            other.typeId = 0;
        }
        return *this;
    }

    GenericComponentEntry(const GenericComponentEntry&) = delete;
    GenericComponentEntry& operator=(const GenericComponentEntry&) = delete;

    ~GenericComponentEntry() {
        destroy();
    }

    bool is_valid() const { return data != nullptr && typeId != 0; }

    /// Deep-clone this entry using the ComponentRegistry's cloner.
    GenericComponentEntry clone() const {
        if (!is_valid()) return {};
        const auto* desc = ComponentRegistry::get().find(typeId);
        if (!desc || !desc->cloner) return {};
        void* cloned = desc->cloner(data);
        return GenericComponentEntry(cloned, typeId);
    }

private:
    void destroy() {
        if (data && typeId != 0) {
            const auto* desc = ComponentRegistry::get().find(typeId);
            if (desc && desc->destructor) {
                desc->destructor(data);
            }
            data   = nullptr;
            typeId = 0;
        }
    }
};

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

class Scene {
public:
    Scene(const std::string& name = "Untitled Scene");
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    ~Scene() = default;

    // -----------------------------------------------------------------------
    // Basic scene properties
    // -----------------------------------------------------------------------

    const std::string& get_name() const { return m_name; }
    void set_name(const std::string& name) { m_name = name; }
    UUID get_id() const { return m_id; }

    // -----------------------------------------------------------------------
    // Entity management (unchanged API)
    // -----------------------------------------------------------------------

    Entity create_entity(const std::string& name = "Entity");
    Entity create_entity_with_id(UUID id, const std::string& name = "Entity");
    void destroy_entity(UUID id);
    void rename_entity(UUID id, const std::string& name);
    void set_parent(UUID childID, UUID parentID);
    UUID get_parent(UUID childID) const;
    std::vector<UUID> get_children(UUID parentID) const;

    Entity find_entity_by_id(UUID id);
    const Entity* find_entity_by_id_const(UUID id) const;
    Scene clone_for_play() const;
    const std::unordered_map<UUID, Entity>& get_entities() const { return m_entities; }

    bool save_to_file(const std::string& filePath);
    bool load_from_file(const std::string& filePath);

    // -----------------------------------------------------------------------
    // Typed Component Stores (legacy — fully preserved for backward compat)
    // -----------------------------------------------------------------------

    std::unordered_map<UUID, TransformComponent>     transformComponents;
    std::unordered_map<UUID, MeshRendererComponent>  meshRendererComponents;
    std::unordered_map<UUID, LightComponent>         lightComponents;
    std::unordered_map<UUID, CameraComponent>        cameraComponents;
    std::unordered_map<UUID, RigidbodyComponent>     rigidbodyComponents;
    std::unordered_map<UUID, WeaponComponent>        weaponComponents;
    std::unordered_map<UUID, ParticleEmitterComponent> particleEmitterComponents;
    std::unordered_map<UUID, VehicleComponent>       vehicleComponents;
    std::unordered_map<UUID, RagdollComponent>       ragdollComponents;
    std::unordered_map<UUID, MissionComponent>       missionComponents;
    std::unordered_map<UUID, DialogueComponent>      dialogueComponents;
    std::unordered_map<UUID, DestructionComponent>   destructionComponents;
    std::unordered_map<UUID, NavigationComponent>    navigationComponents;
    std::unordered_map<UUID, AudioComponent>         audioComponents;
    std::unordered_map<UUID, MaterialComponent>      materialComponents;
    std::unordered_map<UUID, HierarchyComponent>     hierarchyComponents;
    std::unordered_map<UUID, VoxelVolumeComponent>   voxelVolumeComponents;

    // Wicked-port component set (frontend; PORTS.md). Authored in the tool
    // panels; runtime integration status is noted on each struct in
    // Components.hpp (TODO(frontend-port) where the play world does not
    // simulate the feature yet).
    std::unordered_map<UUID, ColliderComponent>     colliderComponents;
    std::unordered_map<UUID, ConstraintComponent>   constraintComponents;
    std::unordered_map<UUID, SoftBodyComponent>     softBodyComponents;
    std::unordered_map<UUID, SpringComponent>       springComponents;
    std::unordered_map<UUID, DecalComponent>        decalComponents;
    std::unordered_map<UUID, SplineComponent>       splineComponents;
    std::unordered_map<UUID, ForceFieldComponent>   forceFieldComponents;
    std::unordered_map<UUID, EnvProbeComponent>     envProbeComponents;
    std::unordered_map<UUID, WeatherComponent>      weatherComponents;
    std::unordered_map<UUID, HairParticleComponent> hairParticleComponents;

    // Runtime-wired component set (added by the frontend port): Layers,
    // vertex Paint, flipbook Video and Gaussian Splat clouds all have REAL
    // runtime behaviour in the editor and in play mode.
    std::unordered_map<UUID, LayerComponent>        layerComponents;
    std::unordered_map<UUID, PaintComponent>        paintComponents;
    std::unordered_map<UUID, VideoComponent>        videoComponents;
    std::unordered_map<UUID, GaussianSplatComponent> gaussianSplatComponents;
    std::unordered_map<UUID, ExpressionComponent>   expressionComponents;

    // Animation stack (Playback): Apply targets of the Animation / Timeline /
    // IK / Retarget specialized editors. Simulated by tick_play_runtime.
    std::unordered_map<UUID, AnimationComponent>    animationComponents;
    std::unordered_map<UUID, TimelineComponent>     timelineComponents;
    std::unordered_map<UUID, IKComponent>           ikComponents;
    std::unordered_map<UUID, RetargetComponent>     retargetComponents;

    // -----------------------------------------------------------------------
    // Generic Component Storage (new — for plugins and future types)
    //
    // Components are stored as void* wrapped in GenericComponentEntry,
    // keyed first by TypeId (component type) then by UUID (entity).
    // The ComponentRegistry manages the lifecycle (create/destroy/clone).
    // -----------------------------------------------------------------------

    /// Add a default-constructed generic component to an entity.
    /// Returns a pointer to the component data, or nullptr if the type is
    /// not registered in the ComponentRegistry or the entity already has
    /// a component of that type.
    void* add_generic_component(UUID entityId, TypeId typeId);

    /// Remove a generic component from an entity.
    /// Returns true if the component was found and removed.
    bool remove_generic_component(UUID entityId, TypeId typeId);

    /// Check whether an entity has a generic component of the given type.
    bool has_generic_component(UUID entityId, TypeId typeId) const;

    /// Get a pointer to a generic component on an entity.
    /// Returns nullptr if the entity does not have a component of that type.
    void* get_generic_component(UUID entityId, TypeId typeId);

    /// Get a const pointer to a generic component on an entity.
    const void* get_generic_component(UUID entityId, TypeId typeId) const;

    /// Get all generic components attached to an entity.
    /// Returns a vector of (TypeId, void*) pairs.
    std::vector<std::pair<TypeId, void*>> get_all_generic_components(UUID entityId);

    /// Get all generic components attached to an entity (const version).
    std::vector<std::pair<TypeId, const void*>> get_all_generic_components(UUID entityId) const;

    /// Clone all generic components from one entity to another.
    /// Existing generic components on the destination are NOT cleared first;
    /// components that already exist on dest with the same TypeId are skipped.
    void clone_entity_generic_components(UUID source, UUID dest);

    /// Remove all generic components from an entity.
    void clear_generic_components(UUID entityId);

    /// Get the number of generic component types that have entries for any entity.
    size_t get_generic_component_type_count() const;

    /// Get all entity IDs that have a generic component of the given type.
    std::vector<UUID> get_entities_with_generic_component(TypeId typeId) const;

    // -----------------------------------------------------------------------
    // Generic component storage (raw access — for integration layer)
    // -----------------------------------------------------------------------

    using GenericComponentMap = std::unordered_map<TypeId,
                                    std::unordered_map<UUID, GenericComponentEntry>>;

    const GenericComponentMap& get_generic_component_map() const {
        return m_genericComponents;
    }

private:
    friend class Serializer;
    void rebind_entities() noexcept;

    UUID        m_id;
    std::string m_name;
    std::unordered_map<UUID, Entity> m_entities;

    // Generic (type-erased) component storage
    // Outer key: TypeId (component type)
    // Inner key: UUID   (entity id)
    GenericComponentMap m_genericComponents;
};

} // namespace Engine

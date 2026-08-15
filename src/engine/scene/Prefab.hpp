#pragma once

#include <any>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/uuid/UUID.hpp"
#include "Entity.hpp"
#include "Scene.hpp"

namespace Engine {

class Serializer;
class Prefab;

enum class PrefabOverrideKind {
    Property,
    ComponentAdded,
    ComponentRemoved,
    EntityAdded,
    EntityRemoved
};

// The first three fields intentionally retain the original aggregate API.
struct PropertyOverride {
    std::string componentName;
    std::string fieldName;
    std::any value;
    UUID localEntityID{0, 0};
    PrefabOverrideKind kind{PrefabOverrideKind::Property};
};

struct PrefabEntityData {
    UUID localID{0, 0};
    UUID sourceEntityID{0, 0};
    UUID parentLocalID{0, 0};
    std::string name{"Entity"};

    bool hasTransform{false};
    bool hasMeshRenderer{false};
    bool hasLight{false};
    bool hasCamera{false};
    bool hasRigidbody{false};
    bool hasWeapon{false};
    bool hasParticleEmitter{false};
    bool hasVehicle{false};
    bool hasRagdoll{false};
    bool hasMission{false};
    bool hasDialogue{false};
    bool hasDestruction{false};
    bool hasNavigation{false};
    bool hasAudio{false};
    bool hasMaterial{false};
    bool hasVoxelVolume{false};
    TransformComponent transform{};
    MeshRendererComponent meshRenderer{};
    LightComponent light{};
    CameraComponent camera{};
    RigidbodyComponent rigidbody{};
    WeaponComponent weapon{};
    ParticleEmitterComponent particleEmitter{};
    VehicleComponent vehicle{};
    RagdollComponent ragdoll{};
    MissionComponent mission{};
    DialogueComponent dialogue{};
    DestructionComponent destruction{};
    NavigationComponent navigation{};
    AudioComponent audio{};
    MaterialComponent material{};
    VoxelVolumeComponent voxelVolume{};
};

struct NestedPrefabReference {
    UUID parentLocalID{0, 0};
    UUID prefabID{0, 0};
    const Prefab* prefab{nullptr}; // Non-owning asset-registry binding.
};

struct PrefabInstance {
    UUID instanceID{0, 0};
    UUID prefabID{0, 0};
    UUID rootEntityID{0, 0};
    std::string rootName;
    Scene* scene{nullptr};
    std::unordered_map<UUID, UUID> entities; // local prefab ID -> scene entity ID
    std::vector<UUID> nestedInstanceIDs;
    std::vector<PropertyOverride> overrides;
    std::unordered_map<UUID, PrefabEntityData> addedEntities;
};

struct PrefabInstantiation {
    UUID instanceID{0, 0};
    UUID rootEntityID{0, 0};
    std::unordered_map<UUID, UUID> entities;
    std::vector<UUID> nestedInstanceIDs;
    std::string error;

    explicit operator bool() const noexcept {
        return instanceID.is_valid() && rootEntityID.is_valid() && error.empty();
    }
};

class Prefab {
public:
    Prefab(UUID id = UUID(), const std::string& name = "Untitled Prefab")
        : m_id(id), m_name(name) {}

    UUID get_id() const noexcept { return m_id; }
    const std::string& get_name() const noexcept { return m_name; }
    UUID root_local_id() const noexcept { return m_rootLocalID; }
    std::size_t entity_count() const noexcept { return m_entities.size(); }
    bool is_captured() const noexcept { return m_captured; }
    const std::vector<PrefabEntityData>& get_entities() const noexcept { return m_entities; }
    const std::vector<NestedPrefabReference>& get_nested_prefabs() const noexcept { return m_nestedPrefabs; }

    // Legacy single-entity entry point now captures the complete descendant hierarchy.
    void capture(const Scene& source, UUID entityID);
    bool capture_hierarchy(const Scene& source, UUID rootEntityID);
    bool capture_entities(const Scene& source, const std::vector<UUID>& entityIDs, UUID rootEntityID);
    UUID local_id_for_source(UUID sourceEntityID) const noexcept;

    // Legacy overrides are defaults applied to every future/current instance.
    void set_override(PropertyOverride propertyOverride);
    bool set_prefab_property(UUID localEntityID, const std::string& componentName,
                             const std::string& fieldName, const std::any& value);

    Entity instantiate(Scene* targetScene, const std::string& instanceName = "") const;
    PrefabInstantiation instantiate_instance(Scene* targetScene,
                                               const std::string& instanceName = "") const;
    const PrefabInstance* find_instance(UUID instanceID) const noexcept;
    UUID instance_id_for_entity(UUID sceneEntityID) const noexcept;

    bool set_instance_override(Scene& scene, UUID instanceID, PropertyOverride propertyOverride) const;
    bool set_component_removed(Scene& scene, UUID instanceID, UUID localEntityID,
                               const std::string& componentName) const;
    bool set_component_added(Scene& scene, UUID instanceID, UUID localEntityID,
                             const std::string& componentName, const std::any& component) const;
    bool set_entity_removed(Scene& scene, UUID instanceID, UUID localEntityID) const;
    UUID add_instance_entity(Scene& scene, UUID instanceID, UUID parentLocalID,
                             const std::string& name) const;

    bool propagate_instance(Scene& scene, UUID instanceID) const;
    bool propagate_all_instances() const;
    bool apply_override(Scene& scene, UUID instanceID, UUID localEntityID,
                        const std::string& componentName, const std::string& fieldName);
    bool apply_component_override_to_prefab(Scene& scene, UUID instanceID, UUID localEntityID,
                                             const std::string& componentName);
    bool revert_component_override(Scene& scene, UUID instanceID, UUID localEntityID,
                                    const std::string& componentName) const;
    bool apply_entity_override(Scene& scene, UUID instanceID, UUID localEntityID);
    bool revert_override(Scene& scene, UUID instanceID, UUID localEntityID,
                         const std::string& componentName, const std::string& fieldName) const;
    bool revert_entity_override(Scene& scene, UUID instanceID, UUID localEntityID) const;
    bool revert_all(Scene& scene, UUID instanceID) const;
    bool has_override(UUID instanceID, UUID localEntityID,
                      const std::string& componentName, const std::string& fieldName) const noexcept;
    bool destroy_instance(Scene& scene, UUID instanceID) const;

    bool add_nested_prefab(UUID parentLocalID, const Prefab& nested);
    bool bind_nested_prefab(UUID prefabID, const Prefab& nested);
    bool would_create_cycle(const Prefab& nested) const noexcept;

    bool save_to_file(const std::string& filePath) const;
    bool load_from_file(const std::string& filePath);

private:
    friend class Serializer;

    PrefabEntityData snapshot_entity(const Scene& source, UUID sourceID, UUID localID) const;
    const PrefabEntityData* find_entity(UUID localID) const noexcept;
    PrefabEntityData* find_entity(UUID localID) noexcept;
    bool expand_into(Scene& scene, PrefabInstance& instance, UUID attachParent,
                     std::unordered_set<UUID>& prefabStack,
                     std::vector<UUID>& createdEntities, bool topLevel,
                     const std::string& instanceName, std::string& error) const;
    bool materialize_entity(Scene& scene, UUID sceneID, const PrefabEntityData& data) const;
    bool apply_property(Scene& scene, UUID sceneID, const PropertyOverride& property) const;
    bool apply_property(PrefabEntityData& entity, const PropertyOverride& property) const;
    bool apply_component_override(Scene& scene, UUID sceneID, const PropertyOverride& property) const;
    bool rebuild_instance(Scene& scene, PrefabInstance& instance) const;
    bool contains_prefab_recursive(UUID prefabID, std::unordered_set<UUID>& visited) const noexcept;
    void prune_dead_instances(Scene& scene) const;
    bool apply_legacy_overrides(Scene& scene, PrefabInstance& instance) const;

    UUID m_id;
    std::string m_name;
    UUID m_rootLocalID{0, 0};
    bool m_captured{false};
    std::vector<PrefabEntityData> m_entities;
    std::vector<NestedPrefabReference> m_nestedPrefabs;
    std::vector<PropertyOverride> m_overrides;
    mutable std::unordered_map<UUID, PrefabInstance> m_instances;
    mutable std::unordered_map<UUID, UUID> m_entityToInstance;
};

} // namespace Engine

#include "Prefab.hpp"
#include "../core/serialization/Serializer.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace Engine {
namespace {

bool same_override_key(const PropertyOverride& left, const PropertyOverride& right) {
    return left.localEntityID == right.localEntityID &&
           left.componentName == right.componentName &&
           left.fieldName == right.fieldName && left.kind == right.kind;
}

bool is_removed(const PrefabInstance& instance, UUID localID) {
    return std::any_of(instance.overrides.begin(), instance.overrides.end(), [localID](const auto& item) {
        return item.localEntityID == localID && item.kind == PrefabOverrideKind::EntityRemoved;
    });
}

PropertyOverride component_override(UUID localID, const std::string& component,
                                    PrefabOverrideKind kind, std::any value = {}) {
    PropertyOverride result;
    result.localEntityID = localID;
    result.componentName = component;
    result.kind = kind;
    result.value = std::move(value);
    return result;
}

} // namespace

void Prefab::capture(const Scene& source, UUID entityID) {
    (void)capture_hierarchy(source, entityID);
}

bool Prefab::capture_hierarchy(const Scene& source, UUID rootEntityID) {
    if (!source.find_entity_by_id_const(rootEntityID)) return false;
    std::vector<UUID> selected;
    std::vector<UUID> pending{rootEntityID};
    std::unordered_set<UUID> visited;
    while (!pending.empty()) {
        const UUID current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        if (!source.find_entity_by_id_const(current)) return false;
        selected.push_back(current);
        const auto children = source.get_children(current);
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return capture_entities(source, selected, rootEntityID);
}

bool Prefab::capture_entities(const Scene& source, const std::vector<UUID>& entityIDs,
                              UUID rootEntityID) {
    if (entityIDs.empty() || !source.find_entity_by_id_const(rootEntityID)) return false;
    std::unordered_set<UUID> selected(entityIDs.begin(), entityIDs.end());
    if (!selected.contains(rootEntityID) || selected.size() != entityIDs.size()) return false;
    for (UUID id : entityIDs) if (!source.find_entity_by_id_const(id)) return false;

    std::unordered_map<UUID, UUID> stableIDs;
    for (const auto& entity : m_entities) {
        if (entity.sourceEntityID.is_valid()) stableIDs[entity.sourceEntityID] = entity.localID;
    }
    std::vector<PrefabEntityData> captured;
    captured.reserve(entityIDs.size());
    for (UUID sourceID : entityIDs) {
        UUID localID = stableIDs.contains(sourceID) ? stableIDs.at(sourceID) : UUID();
        captured.push_back(snapshot_entity(source, sourceID, localID));
    }
    std::unordered_map<UUID, UUID> sourceToLocal;
    for (const auto& entity : captured) sourceToLocal[entity.sourceEntityID] = entity.localID;
    for (auto& entity : captured) {
        const UUID sourceParent = source.get_parent(entity.sourceEntityID);
        entity.parentLocalID = selected.contains(sourceParent) ? sourceToLocal.at(sourceParent) : UUID{0, 0};
    }

    const UUID rootLocal = sourceToLocal.at(rootEntityID);
    const Entity* root = source.find_entity_by_id_const(rootEntityID);
    m_entities = std::move(captured);
    m_rootLocalID = rootLocal;
    m_name = root->get_name();
    m_captured = true;
    return true;
}

PrefabEntityData Prefab::snapshot_entity(const Scene& source, UUID sourceID, UUID localID) const {
    PrefabEntityData data;
    data.localID = localID.is_valid() ? localID : UUID();
    data.sourceEntityID = sourceID;
    if (const Entity* entity = source.find_entity_by_id_const(sourceID)) data.name = entity->get_name();
#define VC_CAPTURE_COMPONENT(store, flag, member) \
    if (auto it = source.store.find(sourceID); it != source.store.end()) { data.flag = true; data.member = it->second; }
    VC_CAPTURE_COMPONENT(transformComponents, hasTransform, transform)
    VC_CAPTURE_COMPONENT(meshRendererComponents, hasMeshRenderer, meshRenderer)
    VC_CAPTURE_COMPONENT(lightComponents, hasLight, light)
    VC_CAPTURE_COMPONENT(cameraComponents, hasCamera, camera)
    VC_CAPTURE_COMPONENT(rigidbodyComponents, hasRigidbody, rigidbody)
    VC_CAPTURE_COMPONENT(weaponComponents, hasWeapon, weapon)
    VC_CAPTURE_COMPONENT(particleEmitterComponents, hasParticleEmitter, particleEmitter)
    VC_CAPTURE_COMPONENT(vehicleComponents, hasVehicle, vehicle)
    VC_CAPTURE_COMPONENT(ragdollComponents, hasRagdoll, ragdoll)
    VC_CAPTURE_COMPONENT(missionComponents, hasMission, mission)
    VC_CAPTURE_COMPONENT(dialogueComponents, hasDialogue, dialogue)
    VC_CAPTURE_COMPONENT(destructionComponents, hasDestruction, destruction)
    VC_CAPTURE_COMPONENT(navigationComponents, hasNavigation, navigation)
    VC_CAPTURE_COMPONENT(audioComponents, hasAudio, audio)
    VC_CAPTURE_COMPONENT(materialComponents, hasMaterial, material)
    VC_CAPTURE_COMPONENT(voxelVolumeComponents, hasVoxelVolume, voxelVolume)
#undef VC_CAPTURE_COMPONENT
    return data;
}

UUID Prefab::local_id_for_source(UUID sourceEntityID) const noexcept {
    for (const auto& entity : m_entities)
        if (entity.sourceEntityID == sourceEntityID) return entity.localID;
    return UUID{0, 0};
}

const PrefabEntityData* Prefab::find_entity(UUID localID) const noexcept {
    const auto it = std::find_if(m_entities.begin(), m_entities.end(), [localID](const auto& entity) {
        return entity.localID == localID;
    });
    return it == m_entities.end() ? nullptr : &*it;
}

PrefabEntityData* Prefab::find_entity(UUID localID) noexcept {
    return const_cast<PrefabEntityData*>(std::as_const(*this).find_entity(localID));
}

void Prefab::set_override(PropertyOverride propertyOverride) {
    if (!propertyOverride.localEntityID.is_valid()) propertyOverride.localEntityID = m_rootLocalID;
    PrefabEntityData* target = find_entity(propertyOverride.localEntityID);
    if (propertyOverride.kind != PrefabOverrideKind::Property || !target) return;
    PrefabEntityData validation = *target;
    if (!apply_property(validation, propertyOverride)) return;
    auto previous = m_overrides;
    auto it = std::find_if(m_overrides.begin(), m_overrides.end(), [&](const auto& existing) {
        return same_override_key(existing, propertyOverride);
    });
    if (it == m_overrides.end()) m_overrides.push_back(std::move(propertyOverride));
    else *it = std::move(propertyOverride);
    if (!propagate_all_instances()) {
        m_overrides = std::move(previous);
        (void)propagate_all_instances();
    }
}

bool Prefab::set_prefab_property(UUID localEntityID, const std::string& componentName,
                                 const std::string& fieldName, const std::any& value) {
    PrefabEntityData* entity = find_entity(localEntityID);
    PropertyOverride property{componentName, fieldName, value, localEntityID};
    if (!entity) return false;
    PrefabEntityData original = *entity;
    if (!apply_property(*entity, property)) return false;
    if (propagate_all_instances()) return true;
    *entity = std::move(original);
    (void)propagate_all_instances();
    return false;
}

Entity Prefab::instantiate(Scene* targetScene, const std::string& instanceName) const {
    const PrefabInstantiation result = instantiate_instance(targetScene, instanceName);
    return result ? targetScene->find_entity_by_id(result.rootEntityID) : Entity{};
}

PrefabInstantiation Prefab::instantiate_instance(Scene* targetScene, const std::string& instanceName) const {
    PrefabInstantiation result;
    if (!targetScene || !m_captured || !m_rootLocalID.is_valid()) {
        result.error = "Prefab is not captured or target scene is null";
        return result;
    }

    PrefabInstance instance;
    instance.instanceID = UUID();
    instance.prefabID = m_id;
    instance.rootName = instanceName.empty() ? m_name : instanceName;
    instance.scene = targetScene;
    std::unordered_set<UUID> stack;
    std::vector<UUID> created;
    std::string error;
    try {
        if (!expand_into(*targetScene, instance, UUID{0, 0}, stack, created, true, instanceName, error)) {
            for (auto it = created.rbegin(); it != created.rend(); ++it) targetScene->destroy_entity(*it);
            prune_dead_instances(*targetScene);
            result.error = error.empty() ? "Prefab expansion failed" : error;
            return result;
        }
        if (!apply_legacy_overrides(*targetScene, instance)) {
            for (auto it = created.rbegin(); it != created.rend(); ++it) targetScene->destroy_entity(*it);
            prune_dead_instances(*targetScene);
            result.error = "A prefab default override is invalid";
            return result;
        }
    } catch (const std::exception& exception) {
        for (auto it = created.rbegin(); it != created.rend(); ++it) targetScene->destroy_entity(*it);
        prune_dead_instances(*targetScene);
        result.error = exception.what();
        return result;
    } catch (...) {
        for (auto it = created.rbegin(); it != created.rend(); ++it) targetScene->destroy_entity(*it);
        prune_dead_instances(*targetScene);
        result.error = "Unknown prefab expansion failure";
        return result;
    }

    result.instanceID = instance.instanceID;
    result.rootEntityID = instance.rootEntityID;
    result.entities = instance.entities;
    result.nestedInstanceIDs = instance.nestedInstanceIDs;
    m_instances[instance.instanceID] = instance;
    for (const auto& [local, sceneID] : instance.entities)
        if (!m_entityToInstance.contains(sceneID)) m_entityToInstance[sceneID] = instance.instanceID;
    return result;
}

bool Prefab::expand_into(Scene& scene, PrefabInstance& instance, UUID attachParent,
                         std::unordered_set<UUID>& prefabStack, std::vector<UUID>& createdEntities,
                         bool topLevel, const std::string& instanceName, std::string& error) const {
    if (!prefabStack.insert(m_id).second) {
        error = "Nested prefab cycle detected";
        return false;
    }
    if (!m_captured || !find_entity(m_rootLocalID)) {
        error = "Nested prefab is not captured";
        prefabStack.erase(m_id);
        return false;
    }

    for (const auto& data : m_entities) {
        if (instance.entities.contains(data.localID)) {
            error = "Duplicate local entity ID across nested prefabs";
            prefabStack.erase(m_id);
            return false;
        }
        Entity entity = scene.create_entity(topLevel && data.localID == m_rootLocalID && !instanceName.empty()
                                                ? instanceName : data.name);
        createdEntities.push_back(entity.get_id());
        instance.entities[data.localID] = entity.get_id();
        if (!materialize_entity(scene, entity.get_id(), data)) {
            error = "Could not materialize prefab entity";
            prefabStack.erase(m_id);
            return false;
        }
        if (data.localID == m_rootLocalID) {
            if (!instance.rootEntityID.is_valid()) instance.rootEntityID = entity.get_id();
            if (topLevel && !instanceName.empty()) scene.rename_entity(entity.get_id(), instanceName);
        }
    }
    for (const auto& data : m_entities) {
        const UUID child = instance.entities.at(data.localID);
        if (data.parentLocalID.is_valid()) scene.set_parent(child, instance.entities.at(data.parentLocalID));
        else if (data.localID == m_rootLocalID && attachParent.is_valid()) scene.set_parent(child, attachParent);
    }

    for (const auto& nested : m_nestedPrefabs) {
        if (!nested.prefab || nested.prefab->get_id() != nested.prefabID) {
            error = "Nested prefab reference is unresolved";
            prefabStack.erase(m_id);
            return false;
        }
        const auto parent = instance.entities.find(nested.parentLocalID);
        if (parent == instance.entities.end()) {
            error = "Nested prefab parent does not exist";
            prefabStack.erase(m_id);
            return false;
        }
        PrefabInstance childInstance;
        childInstance.instanceID = UUID();
        childInstance.prefabID = nested.prefabID;
        childInstance.rootName = nested.prefab->get_name();
        childInstance.scene = &scene;
        if (!nested.prefab->expand_into(scene, childInstance, parent->second, prefabStack,
                                        createdEntities, false, {}, error)) {
            prefabStack.erase(m_id);
            return false;
        }
        if (!nested.prefab->apply_legacy_overrides(scene, childInstance)) {
            error = "A nested prefab default override is invalid";
            prefabStack.erase(m_id);
            return false;
        }
        for (const auto& [localID, sceneID] : childInstance.entities) {
            if (!instance.entities.emplace(localID, sceneID).second) {
                error = "Duplicate local entity ID across nested prefabs";
                prefabStack.erase(m_id);
                return false;
            }
        }
        instance.nestedInstanceIDs.push_back(childInstance.instanceID);
        instance.nestedInstanceIDs.insert(instance.nestedInstanceIDs.end(),
            childInstance.nestedInstanceIDs.begin(), childInstance.nestedInstanceIDs.end());
        nested.prefab->m_instances[childInstance.instanceID] = childInstance;
        for (const auto& [localID, sceneID] : childInstance.entities)
            nested.prefab->m_entityToInstance[sceneID] = childInstance.instanceID;
    }
    prefabStack.erase(m_id);
    return true;
}

bool Prefab::materialize_entity(Scene& scene, UUID id, const PrefabEntityData& data) const {
    if (!scene.find_entity_by_id_const(id)) return false;
    scene.rename_entity(id, data.name);
#define VC_SET_COMPONENT(store, flag, member) \
    scene.store.erase(id); if (data.flag) scene.store[id] = data.member;
    VC_SET_COMPONENT(transformComponents, hasTransform, transform)
    VC_SET_COMPONENT(meshRendererComponents, hasMeshRenderer, meshRenderer)
    VC_SET_COMPONENT(lightComponents, hasLight, light)
    VC_SET_COMPONENT(cameraComponents, hasCamera, camera)
    VC_SET_COMPONENT(rigidbodyComponents, hasRigidbody, rigidbody)
    VC_SET_COMPONENT(weaponComponents, hasWeapon, weapon)
    VC_SET_COMPONENT(particleEmitterComponents, hasParticleEmitter, particleEmitter)
    VC_SET_COMPONENT(vehicleComponents, hasVehicle, vehicle)
    VC_SET_COMPONENT(ragdollComponents, hasRagdoll, ragdoll)
    VC_SET_COMPONENT(missionComponents, hasMission, mission)
    VC_SET_COMPONENT(dialogueComponents, hasDialogue, dialogue)
    VC_SET_COMPONENT(destructionComponents, hasDestruction, destruction)
    VC_SET_COMPONENT(navigationComponents, hasNavigation, navigation)
    VC_SET_COMPONENT(audioComponents, hasAudio, audio)
    VC_SET_COMPONENT(materialComponents, hasMaterial, material)
    VC_SET_COMPONENT(voxelVolumeComponents, hasVoxelVolume, voxelVolume)
#undef VC_SET_COMPONENT
    return true;
}

const PrefabInstance* Prefab::find_instance(UUID instanceID) const noexcept {
    const auto it = m_instances.find(instanceID);
    return it == m_instances.end() ? nullptr : &it->second;
}

UUID Prefab::instance_id_for_entity(UUID sceneEntityID) const noexcept {
    const auto it = m_entityToInstance.find(sceneEntityID);
    return it == m_entityToInstance.end() ? UUID{0, 0} : it->second;
}

bool Prefab::apply_property(Scene& scene, UUID id, const PropertyOverride& property) const {
    try {
        if (property.componentName == "TransformComponent") {
            auto it = scene.transformComponents.find(id); if (it == scene.transformComponents.end()) return false;
            if (property.fieldName == "position") it->second.position = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "rotation") it->second.rotation = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "scale") it->second.scale = std::any_cast<glm::vec3>(property.value); else return false;
        } else if (property.componentName == "LightComponent") {
            auto it = scene.lightComponents.find(id); if (it == scene.lightComponents.end()) return false;
            if (property.fieldName == "color") it->second.color = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "intensity") it->second.intensity = std::any_cast<float>(property.value);
            else if (property.fieldName == "range") it->second.range = std::any_cast<float>(property.value);
            else if (property.fieldName == "castShadows") it->second.castShadows = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "CameraComponent") {
            auto it = scene.cameraComponents.find(id); if (it == scene.cameraComponents.end()) return false;
            if (property.fieldName == "fov") it->second.fov = std::any_cast<float>(property.value);
            else if (property.fieldName == "nearPlane") it->second.nearPlane = std::any_cast<float>(property.value);
            else if (property.fieldName == "farPlane") it->second.farPlane = std::any_cast<float>(property.value);
            else if (property.fieldName == "isPrimary") it->second.isPrimary = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "RigidbodyComponent") {
            auto it = scene.rigidbodyComponents.find(id); if (it == scene.rigidbodyComponents.end()) return false;
            if (property.fieldName == "mass") it->second.mass = std::any_cast<float>(property.value);
            else if (property.fieldName == "friction") it->second.friction = std::any_cast<float>(property.value);
            else if (property.fieldName == "restitution") it->second.restitution = std::any_cast<float>(property.value);
            else if (property.fieldName == "isKinematic") it->second.isKinematic = std::any_cast<bool>(property.value);
            else if (property.fieldName == "useGravity") it->second.useGravity = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "WeaponComponent") {
            auto it = scene.weaponComponents.find(id); if (it == scene.weaponComponents.end()) return false;
            if (property.fieldName == "damage") it->second.damage = std::any_cast<float>(property.value);
            else if (property.fieldName == "roundsPerMinute") it->second.roundsPerMinute = std::any_cast<float>(property.value);
            else if (property.fieldName == "magazineSize") it->second.magazineSize = std::any_cast<uint32_t>(property.value);
            else if (property.fieldName == "reserveAmmo") it->second.reserveAmmo = std::any_cast<uint32_t>(property.value);
            else if (property.fieldName == "automatic") it->second.automatic = std::any_cast<bool>(property.value);
            else if (property.fieldName == "spreadDegrees") it->second.spreadDegrees = std::any_cast<float>(property.value);
            else if (property.fieldName == "hitscan") it->second.hitscan = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "ParticleEmitterComponent") {
            auto it = scene.particleEmitterComponents.find(id); if (it == scene.particleEmitterComponents.end()) return false;
            if (property.fieldName == "position") it->second.position = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "direction") it->second.direction = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "rate") it->second.rate = std::any_cast<float>(property.value);
            else if (property.fieldName == "speedMin") it->second.speedMin = std::any_cast<float>(property.value);
            else if (property.fieldName == "speedMax") it->second.speedMax = std::any_cast<float>(property.value);
            else if (property.fieldName == "sizeStart") it->second.sizeStart = std::any_cast<float>(property.value);
            else if (property.fieldName == "sizeEnd") it->second.sizeEnd = std::any_cast<float>(property.value);
            else if (property.fieldName == "colorStart") it->second.colorStart = std::any_cast<glm::vec4>(property.value);
            else if (property.fieldName == "colorEnd") it->second.colorEnd = std::any_cast<glm::vec4>(property.value);
            else if (property.fieldName == "burstCount") it->second.burstCount = std::any_cast<uint32_t>(property.value);
            else if (property.fieldName == "emitting") it->second.emitting = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "VehicleComponent") {
            auto it = scene.vehicleComponents.find(id); if (it == scene.vehicleComponents.end()) return false;
            if (property.fieldName == "enginePower") it->second.enginePower = std::any_cast<float>(property.value);
            else if (property.fieldName == "maxSteerAngle") it->second.maxSteerAngle = std::any_cast<float>(property.value);
            else if (property.fieldName == "brakeForce") it->second.brakeForce = std::any_cast<float>(property.value);
            else if (property.fieldName == "wheelBase") it->second.wheelBase = std::any_cast<float>(property.value);
            else if (property.fieldName == "trackWidth") it->second.trackWidth = std::any_cast<float>(property.value);
            else if (property.fieldName == "mass") it->second.mass = std::any_cast<float>(property.value);
            else if (property.fieldName == "frontWheelDrive") it->second.frontWheelDrive = std::any_cast<bool>(property.value);
            else if (property.fieldName == "enabled") it->second.enabled = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "RagdollComponent") {
            auto it = scene.ragdollComponents.find(id); if (it == scene.ragdollComponents.end()) return false;
            if (property.fieldName == "enabled") it->second.enabled = std::any_cast<bool>(property.value);
            else if (property.fieldName == "blendWeight") it->second.blendWeight = std::any_cast<float>(property.value);
            else if (property.fieldName == "fromSkeleton") it->second.fromSkeleton = std::any_cast<bool>(property.value);
            else if (property.fieldName == "massPerBone") it->second.massPerBone = std::any_cast<float>(property.value);
            else if (property.fieldName == "spawnOffset") it->second.spawnOffset = std::any_cast<glm::vec3>(property.value); else return false;
        } else if (property.componentName == "MissionComponent") {
            auto it = scene.missionComponents.find(id); if (it == scene.missionComponents.end()) return false;
            if (property.fieldName == "missionId") it->second.missionId = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "objectiveText") it->second.objectiveText = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "objectiveTarget") it->second.objectiveTarget = std::any_cast<uint32_t>(property.value);
            else if (property.fieldName == "completeEvent") it->second.completeEvent = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "autoStart") it->second.autoStart = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "DialogueComponent") {
            auto it = scene.dialogueComponents.find(id); if (it == scene.dialogueComponents.end()) return false;
            if (property.fieldName == "dialogueId") it->second.dialogueId = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "line") it->second.line = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "choiceText") it->second.choiceText = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "nextDialogueId") it->second.nextDialogueId = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "playOnStart") it->second.playOnStart = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "DestructionComponent") {
            auto it = scene.destructionComponents.find(id); if (it == scene.destructionComponents.end()) return false;
            if (property.fieldName == "chunkCount") it->second.chunkCount = std::any_cast<uint32_t>(property.value);
            else if (property.fieldName == "chunkHealth") it->second.chunkHealth = std::any_cast<float>(property.value);
            else if (property.fieldName == "damageRadius") it->second.damageRadius = std::any_cast<float>(property.value);
            else if (property.fieldName == "damageImpulse") it->second.damageImpulse = std::any_cast<float>(property.value);
            else if (property.fieldName == "enabled") it->second.enabled = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "NavigationComponent") {
            auto it = scene.navigationComponents.find(id); if (it == scene.navigationComponents.end()) return false;
            if (property.fieldName == "gridWidth") it->second.gridWidth = std::any_cast<int>(property.value);
            else if (property.fieldName == "gridHeight") it->second.gridHeight = std::any_cast<int>(property.value);
            else if (property.fieldName == "cellSize") it->second.cellSize = std::any_cast<float>(property.value);
            else if (property.fieldName == "agentSpeed") it->second.agentSpeed = std::any_cast<float>(property.value);
            else if (property.fieldName == "enabled") it->second.enabled = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "AudioComponent") {
            auto it = scene.audioComponents.find(id); if (it == scene.audioComponents.end()) return false;
            if (property.fieldName == "clipPath") it->second.clipPath = std::any_cast<std::string>(property.value);
            else if (property.fieldName == "volume") it->second.volume = std::any_cast<float>(property.value);
            else if (property.fieldName == "pitch") it->second.pitch = std::any_cast<float>(property.value);
            else if (property.fieldName == "spatial") it->second.spatial = std::any_cast<bool>(property.value);
            else if (property.fieldName == "looping") it->second.looping = std::any_cast<bool>(property.value);
            else if (property.fieldName == "playOnStart") it->second.playOnStart = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "MaterialComponent") {
            auto it = scene.materialComponents.find(id); if (it == scene.materialComponents.end()) return false;
            if (property.fieldName == "albedo") it->second.albedo = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "roughness") it->second.roughness = std::any_cast<float>(property.value);
            else if (property.fieldName == "metallic") it->second.metallic = std::any_cast<float>(property.value);
            else if (property.fieldName == "emissiveColor") it->second.emissiveColor = std::any_cast<glm::vec3>(property.value);
            else if (property.fieldName == "emissiveIntensity") it->second.emissiveIntensity = std::any_cast<float>(property.value); else return false;
        } else if (property.componentName == "MeshRendererComponent") {
            auto it = scene.meshRendererComponents.find(id); if (it == scene.meshRendererComponents.end()) return false;
            if (property.fieldName == "meshAssetID") it->second.meshAssetID = std::any_cast<UUID>(property.value);
            else if (property.fieldName == "materialAssetID") it->second.materialAssetID = std::any_cast<UUID>(property.value);
            else if (property.fieldName == "isVisible") it->second.isVisible = std::any_cast<bool>(property.value);
            else if (property.fieldName == "castShadows") it->second.castShadows = std::any_cast<bool>(property.value); else return false;
        } else if (property.componentName == "VoxelVolumeComponent") {
            auto it = scene.voxelVolumeComponents.find(id); if (it == scene.voxelVolumeComponents.end()) return false;
            if (property.fieldName == "chunkBudget") it->second.chunkBudget = std::any_cast<int>(property.value);
            else if (property.fieldName == "seed") it->second.seed = std::any_cast<int>(property.value);
            else if (property.fieldName == "seaLevel") it->second.seaLevel = std::any_cast<float>(property.value);
            else if (property.fieldName == "enableFarLod") it->second.enableFarLod = std::any_cast<bool>(property.value); else return false;
        } else return false;
    } catch (const std::bad_any_cast&) { return false; }
    return true;
}

bool Prefab::apply_property(PrefabEntityData& entity, const PropertyOverride& property) const {
    Scene scratch("Prefab Property Transaction");
    const UUID id = scratch.create_entity(entity.name).get_id();
    materialize_entity(scratch, id, entity);
    if (!apply_property(scratch, id, property)) return false;
    PrefabEntityData updated = snapshot_entity(scratch, id, entity.localID);
    updated.sourceEntityID = entity.sourceEntityID;
    updated.parentLocalID = entity.parentLocalID;
    entity = std::move(updated);
    return true;
}

bool Prefab::apply_component_override(Scene& scene, UUID id, const PropertyOverride& property) const {
    if (property.kind == PrefabOverrideKind::ComponentRemoved) {
        if (property.componentName == "TransformComponent") scene.transformComponents.erase(id);
        else if (property.componentName == "MeshRendererComponent") scene.meshRendererComponents.erase(id);
        else if (property.componentName == "LightComponent") scene.lightComponents.erase(id);
        else if (property.componentName == "CameraComponent") scene.cameraComponents.erase(id);
        else        if (property.componentName == "RigidbodyComponent") scene.rigidbodyComponents.erase(id);
        else if (property.componentName == "WeaponComponent") scene.weaponComponents.erase(id);
        else if (property.componentName == "ParticleEmitterComponent") scene.particleEmitterComponents.erase(id);
        else if (property.componentName == "VehicleComponent") scene.vehicleComponents.erase(id);
        else if (property.componentName == "RagdollComponent") scene.ragdollComponents.erase(id);
        else if (property.componentName == "MissionComponent") scene.missionComponents.erase(id);
        else if (property.componentName == "DialogueComponent") scene.dialogueComponents.erase(id);
        else if (property.componentName == "DestructionComponent") scene.destructionComponents.erase(id);
        else if (property.componentName == "NavigationComponent") scene.navigationComponents.erase(id);
        else if (property.componentName == "AudioComponent") scene.audioComponents.erase(id);
        else if (property.componentName == "MaterialComponent") scene.materialComponents.erase(id);
        else if (property.componentName == "VoxelVolumeComponent") scene.voxelVolumeComponents.erase(id);
        else return false;
        return true;
    }
    if (property.kind != PrefabOverrideKind::ComponentAdded) return false;
    try {
        if (property.componentName == "TransformComponent") scene.transformComponents[id] = std::any_cast<TransformComponent>(property.value);
        else if (property.componentName == "MeshRendererComponent") scene.meshRendererComponents[id] = std::any_cast<MeshRendererComponent>(property.value);
        else if (property.componentName == "LightComponent") scene.lightComponents[id] = std::any_cast<LightComponent>(property.value);
        else if (property.componentName == "CameraComponent") scene.cameraComponents[id] = std::any_cast<CameraComponent>(property.value);
        else if (property.componentName == "RigidbodyComponent") scene.rigidbodyComponents[id] = std::any_cast<RigidbodyComponent>(property.value);
        else if (property.componentName == "WeaponComponent") scene.weaponComponents[id] = std::any_cast<WeaponComponent>(property.value);
        else if (property.componentName == "ParticleEmitterComponent") scene.particleEmitterComponents[id] = std::any_cast<ParticleEmitterComponent>(property.value);
        else if (property.componentName == "VehicleComponent") scene.vehicleComponents[id] = std::any_cast<VehicleComponent>(property.value);
        else if (property.componentName == "RagdollComponent") scene.ragdollComponents[id] = std::any_cast<RagdollComponent>(property.value);
        else if (property.componentName == "MissionComponent") scene.missionComponents[id] = std::any_cast<MissionComponent>(property.value);
        else if (property.componentName == "DialogueComponent") scene.dialogueComponents[id] = std::any_cast<DialogueComponent>(property.value);
        else if (property.componentName == "DestructionComponent") scene.destructionComponents[id] = std::any_cast<DestructionComponent>(property.value);
        else if (property.componentName == "NavigationComponent") scene.navigationComponents[id] = std::any_cast<NavigationComponent>(property.value);
        else if (property.componentName == "AudioComponent") scene.audioComponents[id] = std::any_cast<AudioComponent>(property.value);
        else if (property.componentName == "MaterialComponent") scene.materialComponents[id] = std::any_cast<MaterialComponent>(property.value);
        else if (property.componentName == "VoxelVolumeComponent") scene.voxelVolumeComponents[id] = std::any_cast<VoxelVolumeComponent>(property.value);
        else return false;
    } catch (const std::bad_any_cast&) { return false; }
    return true;
}

bool Prefab::set_instance_override(Scene& scene, UUID instanceID, PropertyOverride property) const {
    auto instanceIt = m_instances.find(instanceID);
    if (instanceIt == m_instances.end() || instanceIt->second.scene != &scene) return false;
    if (!property.localEntityID.is_valid()) property.localEntityID = m_rootLocalID;
    const auto entityIt = instanceIt->second.entities.find(property.localEntityID);
    if (entityIt == instanceIt->second.entities.end() || !scene.find_entity_by_id_const(entityIt->second)) return false;
    if (property.kind != PrefabOverrideKind::Property || !apply_property(scene, entityIt->second, property)) return false;
    auto& overrides = instanceIt->second.overrides;
    auto existing = std::find_if(overrides.begin(), overrides.end(), [&](const auto& value) {
        return same_override_key(value, property);
    });
    if (existing == overrides.end()) overrides.push_back(std::move(property)); else *existing = std::move(property);
    return true;
}

bool Prefab::set_component_removed(Scene& scene, UUID instanceID, UUID localEntityID,
                                   const std::string& componentName) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene || !it->second.entities.contains(localEntityID)) return false;
    PropertyOverride override = component_override(localEntityID, componentName, PrefabOverrideKind::ComponentRemoved);
    if (!apply_component_override(scene, it->second.entities.at(localEntityID), override)) return false;
    std::erase_if(it->second.overrides, [&](const auto& value) {
        return value.localEntityID == localEntityID && value.componentName == componentName;
    });
    it->second.overrides.push_back(std::move(override));
    return true;
}

bool Prefab::set_component_added(Scene& scene, UUID instanceID, UUID localEntityID,
                                 const std::string& componentName, const std::any& component) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene || !it->second.entities.contains(localEntityID)) return false;
    PropertyOverride override = component_override(localEntityID, componentName, PrefabOverrideKind::ComponentAdded, component);
    if (!apply_component_override(scene, it->second.entities.at(localEntityID), override)) return false;
    std::erase_if(it->second.overrides, [&](const auto& value) {
        return value.localEntityID == localEntityID && value.componentName == componentName;
    });
    it->second.overrides.push_back(std::move(override));
    return true;
}

bool Prefab::set_entity_removed(Scene& scene, UUID instanceID, UUID localEntityID) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene || localEntityID == m_rootLocalID ||
        !it->second.entities.contains(localEntityID)) return false;
    const UUID sceneID = it->second.entities.at(localEntityID);
    for (const auto& data : m_entities) {
        UUID current = data.localID;
        while (current.is_valid() && current != localEntityID) {
            const PrefabEntityData* currentData = find_entity(current);
            current = currentData ? currentData->parentLocalID : UUID{0, 0};
        }
        if (current == localEntityID && it->second.entities.contains(data.localID)) {
            const UUID descendantID = it->second.entities.at(data.localID);
            if (scene.find_entity_by_id_const(descendantID)) scene.destroy_entity(descendantID);
        }
    }
    if (scene.find_entity_by_id_const(sceneID)) scene.destroy_entity(sceneID);
    PropertyOverride override;
    override.localEntityID = localEntityID;
    override.kind = PrefabOverrideKind::EntityRemoved;
    std::erase_if(it->second.overrides, [localEntityID](const auto& value) {
        return value.localEntityID == localEntityID && value.kind == PrefabOverrideKind::EntityRemoved;
    });
    it->second.overrides.push_back(std::move(override));
    return true;
}

UUID Prefab::add_instance_entity(Scene& scene, UUID instanceID, UUID parentLocalID,
                                 const std::string& name) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene || !it->second.entities.contains(parentLocalID))
        return UUID{0, 0};
    const Entity entity = scene.create_entity(name);
    const UUID localID = UUID();
    PrefabEntityData data = snapshot_entity(scene, entity.get_id(), localID);
    data.sourceEntityID = UUID{0, 0};
    data.parentLocalID = parentLocalID;
    scene.set_parent(entity.get_id(), it->second.entities.at(parentLocalID));
    it->second.entities[localID] = entity.get_id();
    it->second.addedEntities[localID] = data;
    PropertyOverride override;
    override.localEntityID = localID;
    override.kind = PrefabOverrideKind::EntityAdded;
    it->second.overrides.push_back(std::move(override));
    m_entityToInstance[entity.get_id()] = instanceID;
    return localID;
}

bool Prefab::apply_legacy_overrides(Scene& scene, PrefabInstance& instance) const {
    for (auto property : m_overrides) {
        if (!property.localEntityID.is_valid()) property.localEntityID = m_rootLocalID;
        const auto entity = instance.entities.find(property.localEntityID);
        if (entity == instance.entities.end() || !apply_property(scene, entity->second, property)) return false;
    }
    return true;
}

bool Prefab::rebuild_instance(Scene& scene, PrefabInstance& instance) const {
    struct Backup { PrefabEntityData data; UUID sceneID; UUID parentSceneID; };
    const auto oldMap = instance.entities;
    std::vector<Backup> backup;
    for (const auto& [local, sceneID] : oldMap) {
        if (!scene.find_entity_by_id_const(sceneID)) continue;
        PrefabEntityData data = snapshot_entity(scene, sceneID, local);
        data.parentLocalID = UUID{0, 0};
        backup.push_back({std::move(data), sceneID, scene.get_parent(sceneID)});
    }
    auto rollback = [&] {
        for (const auto& [local, sceneID] : instance.entities)
            if (scene.find_entity_by_id_const(sceneID)) scene.destroy_entity(sceneID);
        instance.entities = oldMap;
        for (const auto& item : backup) {
            scene.create_entity_with_id(item.sceneID, item.data.name);
            materialize_entity(scene, item.sceneID, item.data);
        }
        for (const auto& item : backup) if (item.parentSceneID.is_valid()) scene.set_parent(item.sceneID, item.parentSceneID);
    };

    auto effectivelyRemoved = [&](UUID localID) {
        UUID current = localID;
        while (current.is_valid()) {
            if (is_removed(instance, current)) return true;
            const PrefabEntityData* data = find_entity(current);
            current = data ? data->parentLocalID : UUID{0, 0};
        }
        return false;
    };
    try {
        std::unordered_set<UUID> validLocals;
        for (const auto& data : m_entities) {
            validLocals.insert(data.localID);
            UUID sceneID;
            if (auto mapped = instance.entities.find(data.localID); mapped != instance.entities.end()) sceneID = mapped->second;
            else { sceneID = UUID(); instance.entities[data.localID] = sceneID; }
            if (effectivelyRemoved(data.localID)) {
                if (scene.find_entity_by_id_const(sceneID)) scene.destroy_entity(sceneID);
                continue;
            }
            if (!scene.find_entity_by_id_const(sceneID)) scene.create_entity_with_id(sceneID, data.name);
            if (!materialize_entity(scene, sceneID, data)) { rollback(); return false; }
            if (data.localID == m_rootLocalID && !instance.rootName.empty())
                scene.rename_entity(sceneID, instance.rootName);
        }
        for (const auto& [local, data] : instance.addedEntities) {
            validLocals.insert(local);
            UUID sceneID = instance.entities.at(local);
            if (!scene.find_entity_by_id_const(sceneID)) scene.create_entity_with_id(sceneID, data.name);
            if (!materialize_entity(scene, sceneID, data)) { rollback(); return false; }
        }
        for (auto it = instance.entities.begin(); it != instance.entities.end();) {
            if (!validLocals.contains(it->first) && find_entity(it->first)) { // Nested entities are retained.
                if (scene.find_entity_by_id_const(it->second)) scene.destroy_entity(it->second);
                m_entityToInstance.erase(it->second);
                it = instance.entities.erase(it);
            } else ++it;
        }
        for (const auto& data : m_entities) {
            if (effectivelyRemoved(data.localID)) continue;
            const UUID sceneID = instance.entities.at(data.localID);
            if (data.parentLocalID.is_valid() && instance.entities.contains(data.parentLocalID))
                scene.set_parent(sceneID, instance.entities.at(data.parentLocalID));
        }
        for (const auto& [local, data] : instance.addedEntities)
            if (data.parentLocalID.is_valid() && instance.entities.contains(data.parentLocalID))
                scene.set_parent(instance.entities.at(local), instance.entities.at(data.parentLocalID));

        if (!apply_legacy_overrides(scene, instance)) { rollback(); return false; }
        for (const auto& item : instance.overrides) {
            if (item.kind == PrefabOverrideKind::EntityAdded || item.kind == PrefabOverrideKind::EntityRemoved) continue;
            const auto mapped = instance.entities.find(item.localEntityID);
            if (mapped == instance.entities.end() || !scene.find_entity_by_id_const(mapped->second)) continue;
            const bool ok = item.kind == PrefabOverrideKind::Property
                ? apply_property(scene, mapped->second, item)
                : apply_component_override(scene, mapped->second, item);
            if (!ok) { rollback(); return false; }
        }
    } catch (...) { rollback(); return false; }
    return true;
}

bool Prefab::propagate_instance(Scene& scene, UUID instanceID) const {
    auto it = m_instances.find(instanceID);
    return it != m_instances.end() && it->second.scene == &scene && rebuild_instance(scene, it->second);
}

bool Prefab::propagate_all_instances() const {
    for (auto& [id, instance] : m_instances) {
        if (!instance.scene || !rebuild_instance(*instance.scene, instance)) return false;
    }
    return true;
}

bool Prefab::apply_override(Scene& scene, UUID instanceID, UUID localEntityID,
                            const std::string& componentName, const std::string& fieldName) {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene) return false;
    auto overrideIt = std::find_if(it->second.overrides.begin(), it->second.overrides.end(), [&](const auto& item) {
        return item.kind == PrefabOverrideKind::Property && item.localEntityID == localEntityID &&
               item.componentName == componentName && item.fieldName == fieldName;
    });
    PrefabEntityData* entity = find_entity(localEntityID);
    if (overrideIt == it->second.overrides.end() || !entity) return false;
    PrefabEntityData original = *entity;
    const PropertyOverride saved = *overrideIt;
    if (!apply_property(*entity, saved)) return false;
    it->second.overrides.erase(overrideIt);
    if (propagate_all_instances()) return true;
    *entity = std::move(original);
    it->second.overrides.push_back(saved);
    (void)propagate_all_instances();
    return false;
}

bool Prefab::apply_component_override_to_prefab(Scene& scene, UUID instanceID, UUID localEntityID,
                                                 const std::string& componentName) {
    auto instanceIt = m_instances.find(instanceID);
    PrefabEntityData* target = find_entity(localEntityID);
    if (instanceIt == m_instances.end() || instanceIt->second.scene != &scene || !target ||
        !instanceIt->second.entities.contains(localEntityID)) return false;
    auto overrideIt = std::find_if(instanceIt->second.overrides.begin(), instanceIt->second.overrides.end(),
        [&](const auto& item) { return item.localEntityID == localEntityID &&
            item.componentName == componentName && (item.kind == PrefabOverrideKind::ComponentAdded ||
                                                     item.kind == PrefabOverrideKind::ComponentRemoved); });
    if (overrideIt == instanceIt->second.overrides.end()) return false;
    const PrefabEntityData original = *target;
    const UUID sceneID = instanceIt->second.entities.at(localEntityID);
    const bool removed = overrideIt->kind == PrefabOverrideKind::ComponentRemoved;
    const PrefabEntityData current = snapshot_entity(scene, sceneID, localEntityID);
#define VC_APPLY_COMPONENT(name, flag, store, member) \
    if (componentName == name) { if (!removed && !current.flag) return false; target->flag = !removed; if (!removed) target->member = current.member; }
    VC_APPLY_COMPONENT("TransformComponent", hasTransform, transformComponents, transform)
    else VC_APPLY_COMPONENT("MeshRendererComponent", hasMeshRenderer, meshRendererComponents, meshRenderer)
    else VC_APPLY_COMPONENT("LightComponent", hasLight, lightComponents, light)
    else VC_APPLY_COMPONENT("CameraComponent", hasCamera, cameraComponents, camera)
    else VC_APPLY_COMPONENT("RigidbodyComponent", hasRigidbody, rigidbodyComponents, rigidbody)
    else VC_APPLY_COMPONENT("WeaponComponent", hasWeapon, weaponComponents, weapon)
    else VC_APPLY_COMPONENT("ParticleEmitterComponent", hasParticleEmitter, particleEmitterComponents, particleEmitter)
    else VC_APPLY_COMPONENT("VehicleComponent", hasVehicle, vehicleComponents, vehicle)
    else VC_APPLY_COMPONENT("RagdollComponent", hasRagdoll, ragdollComponents, ragdoll)
    else VC_APPLY_COMPONENT("MissionComponent", hasMission, missionComponents, mission)
    else VC_APPLY_COMPONENT("DialogueComponent", hasDialogue, dialogueComponents, dialogue)
    else VC_APPLY_COMPONENT("DestructionComponent", hasDestruction, destructionComponents, destruction)
    else VC_APPLY_COMPONENT("NavigationComponent", hasNavigation, navigationComponents, navigation)
    else VC_APPLY_COMPONENT("AudioComponent", hasAudio, audioComponents, audio)
    else VC_APPLY_COMPONENT("MaterialComponent", hasMaterial, materialComponents, material)
    else VC_APPLY_COMPONENT("VoxelVolumeComponent", hasVoxelVolume, voxelVolumeComponents, voxelVolume)
    else return false;
#undef VC_APPLY_COMPONENT
    const PropertyOverride saved = *overrideIt;
    instanceIt->second.overrides.erase(overrideIt);
    if (propagate_all_instances()) return true;
    *target = original;
    instanceIt->second.overrides.push_back(saved);
    (void)propagate_all_instances();
    return false;
}

bool Prefab::revert_component_override(Scene& scene, UUID instanceID, UUID localEntityID,
                                        const std::string& componentName) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene) return false;
    const auto previous = it->second.overrides;
    const auto oldSize = it->second.overrides.size();
    std::erase_if(it->second.overrides, [&](const auto& item) {
        return item.localEntityID == localEntityID && item.componentName == componentName &&
               (item.kind == PrefabOverrideKind::ComponentAdded || item.kind == PrefabOverrideKind::ComponentRemoved);
    });
    if (oldSize == it->second.overrides.size()) return false;
    if (rebuild_instance(scene, it->second)) return true;
    it->second.overrides = previous;
    (void)rebuild_instance(scene, it->second);
    return false;
}

bool Prefab::apply_entity_override(Scene& scene, UUID instanceID, UUID localEntityID) {
    auto instanceIt = m_instances.find(instanceID);
    if (instanceIt == m_instances.end() || instanceIt->second.scene != &scene || localEntityID == m_rootLocalID)
        return false;
    auto overrideIt = std::find_if(instanceIt->second.overrides.begin(), instanceIt->second.overrides.end(),
        [localEntityID](const auto& item) { return item.localEntityID == localEntityID &&
            (item.kind == PrefabOverrideKind::EntityAdded || item.kind == PrefabOverrideKind::EntityRemoved); });
    if (overrideIt == instanceIt->second.overrides.end()) return false;
    const auto oldEntities = m_entities;
    const auto oldNested = m_nestedPrefabs;
    const auto kind = overrideIt->kind;
    if (kind == PrefabOverrideKind::EntityAdded) {
        auto added = instanceIt->second.addedEntities.find(localEntityID);
        if (added == instanceIt->second.addedEntities.end()) return false;
        m_entities.push_back(added->second);
        instanceIt->second.addedEntities.erase(added);
    } else {
        std::unordered_set<UUID> removedLocals{localEntityID};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& data : m_entities)
                if (removedLocals.contains(data.parentLocalID) && removedLocals.insert(data.localID).second) changed = true;
        }
        std::erase_if(m_entities, [&](const auto& data) { return removedLocals.contains(data.localID); });
        std::erase_if(m_nestedPrefabs, [&](const auto& nested) { return removedLocals.contains(nested.parentLocalID); });
        for (auto& [otherID, other] : m_instances) {
            for (UUID removedLocal : removedLocals) {
                if (auto mapped = other.entities.find(removedLocal); mapped != other.entities.end()) {
                    if (other.scene && other.scene->find_entity_by_id_const(mapped->second)) other.scene->destroy_entity(mapped->second);
                    m_entityToInstance.erase(mapped->second);
                    other.entities.erase(mapped);
                }
            }
            std::erase_if(other.overrides, [&](const auto& item) { return removedLocals.contains(item.localEntityID); });
        }
    }
    std::erase_if(instanceIt->second.overrides, [localEntityID](const auto& item) {
        return item.localEntityID == localEntityID &&
               (item.kind == PrefabOverrideKind::EntityAdded || item.kind == PrefabOverrideKind::EntityRemoved);
    });
    if (propagate_all_instances()) return true;
    m_entities = oldEntities;
    m_nestedPrefabs = oldNested;
    (void)propagate_all_instances();
    return false;
}

bool Prefab::revert_override(Scene& scene, UUID instanceID, UUID localEntityID,
                             const std::string& componentName, const std::string& fieldName) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene) return false;
    const auto previous = it->second.overrides;
    const auto oldSize = it->second.overrides.size();
    std::erase_if(it->second.overrides, [&](const auto& item) {
        return item.kind == PrefabOverrideKind::Property && item.localEntityID == localEntityID &&
               item.componentName == componentName && item.fieldName == fieldName;
    });
    if (oldSize == it->second.overrides.size()) return false;
    if (rebuild_instance(scene, it->second)) return true;
    it->second.overrides = previous;
    (void)rebuild_instance(scene, it->second);
    return false;
}

bool Prefab::revert_entity_override(Scene& scene, UUID instanceID, UUID localEntityID) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene) return false;
    const auto oldSize = it->second.overrides.size();
    std::erase_if(it->second.overrides, [localEntityID](const auto& item) {
        return item.localEntityID == localEntityID &&
               (item.kind == PrefabOverrideKind::EntityRemoved || item.kind == PrefabOverrideKind::EntityAdded);
    });
    if (it->second.addedEntities.erase(localEntityID)) {
        const UUID sceneID = it->second.entities.at(localEntityID);
        if (scene.find_entity_by_id_const(sceneID)) scene.destroy_entity(sceneID);
        it->second.entities.erase(localEntityID);
        m_entityToInstance.erase(sceneID);
    }
    return oldSize != it->second.overrides.size() && rebuild_instance(scene, it->second);
}

bool Prefab::revert_all(Scene& scene, UUID instanceID) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene) return false;
    for (const auto& [local, data] : it->second.addedEntities) {
        const UUID sceneID = it->second.entities.at(local);
        if (scene.find_entity_by_id_const(sceneID)) scene.destroy_entity(sceneID);
        it->second.entities.erase(local);
        m_entityToInstance.erase(sceneID);
    }
    it->second.addedEntities.clear();
    it->second.overrides.clear();
    return rebuild_instance(scene, it->second);
}

bool Prefab::has_override(UUID instanceID, UUID localEntityID, const std::string& componentName,
                          const std::string& fieldName) const noexcept {
    const auto* instance = find_instance(instanceID);
    return instance && std::any_of(instance->overrides.begin(), instance->overrides.end(), [&](const auto& item) {
        return item.localEntityID == localEntityID && item.componentName == componentName &&
               item.fieldName == fieldName;
    });
}

bool Prefab::destroy_instance(Scene& scene, UUID instanceID) const {
    auto it = m_instances.find(instanceID);
    if (it == m_instances.end() || it->second.scene != &scene) return false;
    for (const auto& [local, sceneID] : it->second.entities) {
        if (scene.find_entity_by_id_const(sceneID)) scene.destroy_entity(sceneID);
        m_entityToInstance.erase(sceneID);
    }
    m_instances.erase(it);
    prune_dead_instances(scene);
    return true;
}

void Prefab::prune_dead_instances(Scene& scene) const {
    for (auto it = m_instances.begin(); it != m_instances.end();) {
        if (it->second.scene == &scene && !scene.find_entity_by_id_const(it->second.rootEntityID)) {
            for (const auto& [local, sceneID] : it->second.entities) m_entityToInstance.erase(sceneID);
            it = m_instances.erase(it);
        } else ++it;
    }
    for (const auto& nested : m_nestedPrefabs)
        if (nested.prefab) nested.prefab->prune_dead_instances(scene);
}

bool Prefab::contains_prefab_recursive(UUID prefabID, std::unordered_set<UUID>& visited) const noexcept {
    if (m_id == prefabID) return true;
    if (!visited.insert(m_id).second) return false;
    for (const auto& nested : m_nestedPrefabs)
        if (nested.prefab && nested.prefab->contains_prefab_recursive(prefabID, visited)) return true;
    return false;
}

bool Prefab::would_create_cycle(const Prefab& nested) const noexcept {
    std::unordered_set<UUID> visited;
    return nested.contains_prefab_recursive(m_id, visited);
}

bool Prefab::add_nested_prefab(UUID parentLocalID, const Prefab& nested) {
    if (!find_entity(parentLocalID) || !nested.is_captured() || would_create_cycle(nested)) return false;
    auto duplicate = std::find_if(m_nestedPrefabs.begin(), m_nestedPrefabs.end(), [&](const auto& value) {
        return value.parentLocalID == parentLocalID && value.prefabID == nested.get_id();
    });
    if (duplicate != m_nestedPrefabs.end()) return true;
    m_nestedPrefabs.push_back({parentLocalID, nested.get_id(), &nested});
    return true;
}

bool Prefab::bind_nested_prefab(UUID prefabID, const Prefab& nested) {
    if (prefabID != nested.get_id() || would_create_cycle(nested)) return false;
    bool bound = false;
    for (auto& reference : m_nestedPrefabs) {
        if (reference.prefabID == prefabID) { reference.prefab = &nested; bound = true; }
    }
    return bound;
}

bool Prefab::save_to_file(const std::string& filePath) const {
    return Serializer::serialize_prefab(*this, filePath).success;
}

bool Prefab::load_from_file(const std::string& filePath) {
    return Serializer::deserialize_prefab(*this, filePath).success;
}

} // namespace Engine

// ---------------------------------------------------------------------------
// SceneComponentIntegration.cpp
//
// Implementation of the high-level integration layer that bridges Scene's
// typed component stores, the generic (type-erased) component storage, and
// the ComponentRegistry / Reflection system.
//
// This file contains:
//   - Builtin component registration (typed stores -> descriptors)
//   - Plugin component registration
//   - Unified add/remove/has/get by TypeId
//   - Full entity serialization / deserialization
//   - Entity cloning with all component types
//   - Introspection helpers
// ---------------------------------------------------------------------------

#include "SceneComponentIntegration.hpp"
#include "ComponentRegistry.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>

namespace Engine {

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void SceneComponentIntegration::register_builtin_components() {
    static std::once_flag s_registered;
    std::call_once(s_registered, [] {
        // Ensure reflection data is available
        register_builtin_component_reflection();

        auto& registry = ComponentRegistry::get();

        // TransformComponent
        {
            ComponentDescriptor desc = make_component_descriptor<TransformComponent>("TransformComponent");
            desc.typeId = BuiltinTypeIds::TransformComponent;
            registry.register_component(desc);
        }

        // MeshRendererComponent
        {
            ComponentDescriptor desc;
            desc.typeId    = BuiltinTypeIds::MeshRendererComponent;
            desc.name      = "MeshRendererComponent";
            desc.sizeBytes = sizeof(MeshRendererComponent);
            desc.factory   = []() -> void* { return new MeshRendererComponent{}; };
            desc.destructor = [](void* p) { delete static_cast<MeshRendererComponent*>(p); };
            desc.cloner    = [](const void* s) -> void* {
                return new MeshRendererComponent(*static_cast<const MeshRendererComponent*>(s));
            };
            // MeshRendererComponent has no REFLECT fields, but we can still
            // provide a manual serializer for its key fields
            desc.serializer = [](const void* ptr) -> std::unordered_map<std::string, std::any> {
                const auto* c = static_cast<const MeshRendererComponent*>(ptr);
                std::unordered_map<std::string, std::any> result;
                result["meshAssetID"]    = c->meshAssetID;
                result["materialAssetID"] = c->materialAssetID;
                result["isVisible"]      = c->isVisible;
                result["castShadows"]    = c->castShadows;
                return result;
            };
            desc.deserializer = [](void* ptr, const std::unordered_map<std::string, std::any>& data) {
                auto* c = static_cast<MeshRendererComponent*>(ptr);
                auto it = data.find("meshAssetID");
                if (it != data.end()) c->meshAssetID = std::any_cast<UUID>(it->second);
                it = data.find("materialAssetID");
                if (it != data.end()) c->materialAssetID = std::any_cast<UUID>(it->second);
                it = data.find("isVisible");
                if (it != data.end()) c->isVisible = std::any_cast<bool>(it->second);
                it = data.find("castShadows");
                if (it != data.end()) c->castShadows = std::any_cast<bool>(it->second);
            };
            registry.register_component(desc);
        }

        // LightComponent
        {
            ComponentDescriptor desc = make_component_descriptor<LightComponent>("LightComponent");
            desc.typeId = BuiltinTypeIds::LightComponent;
            registry.register_component(desc);
        }

        // CameraComponent
        {
            ComponentDescriptor desc = make_component_descriptor<CameraComponent>("CameraComponent");
            desc.typeId = BuiltinTypeIds::CameraComponent;
            registry.register_component(desc);
        }

        // RigidbodyComponent
        {
            ComponentDescriptor desc = make_component_descriptor<RigidbodyComponent>("RigidbodyComponent");
            desc.typeId = BuiltinTypeIds::RigidbodyComponent;
            registry.register_component(desc);
        }

        // MaterialComponent
        {
            ComponentDescriptor desc = make_component_descriptor<MaterialComponent>("MaterialComponent");
            desc.typeId = BuiltinTypeIds::MaterialComponent;
            registry.register_component(desc);
        }

        // HierarchyComponent
        {
            ComponentDescriptor desc;
            desc.typeId    = BuiltinTypeIds::HierarchyComponent;
            desc.name      = "HierarchyComponent";
            desc.sizeBytes = sizeof(HierarchyComponent);
            desc.factory   = []() -> void* { return new HierarchyComponent{}; };
            desc.destructor = [](void* p) { delete static_cast<HierarchyComponent*>(p); };
            desc.cloner    = [](const void* s) -> void* {
                return new HierarchyComponent(*static_cast<const HierarchyComponent*>(s));
            };
            desc.serializer = [](const void* ptr) -> std::unordered_map<std::string, std::any> {
                const auto* c = static_cast<const HierarchyComponent*>(ptr);
                std::unordered_map<std::string, std::any> result;
                result["parentID"]    = c->parentID;
                result["childrenIDs"] = c->childrenIDs;
                return result;
            };
            desc.deserializer = [](void* ptr, const std::unordered_map<std::string, std::any>& data) {
                auto* c = static_cast<HierarchyComponent*>(ptr);
                auto it = data.find("parentID");
                if (it != data.end()) c->parentID = std::any_cast<UUID>(it->second);
                it = data.find("childrenIDs");
                if (it != data.end()) c->childrenIDs = std::any_cast<std::vector<UUID>>(it->second);
            };
            registry.register_component(desc);
        }

        // VoxelVolumeComponent
        {
            ComponentDescriptor desc;
            desc.typeId    = BuiltinTypeIds::VoxelVolumeComponent;
            desc.name      = "VoxelVolumeComponent";
            desc.sizeBytes = sizeof(VoxelVolumeComponent);
            desc.factory   = []() -> void* { return new VoxelVolumeComponent{}; };
            desc.destructor = [](void* p) { delete static_cast<VoxelVolumeComponent*>(p); };
            desc.cloner    = [](const void* s) -> void* {
                return new VoxelVolumeComponent(*static_cast<const VoxelVolumeComponent*>(s));
            };
            desc.serializer = [](const void* ptr) -> std::unordered_map<std::string, std::any> {
                const auto* c = static_cast<const VoxelVolumeComponent*>(ptr);
                std::unordered_map<std::string, std::any> result;
                result["chunkBudget"]  = c->chunkBudget;
                result["seed"]         = c->seed;
                result["seaLevel"]     = c->seaLevel;
                result["enableFarLod"] = c->enableFarLod;
                return result;
            };
            desc.deserializer = [](void* ptr, const std::unordered_map<std::string, std::any>& data) {
                auto* c = static_cast<VoxelVolumeComponent*>(ptr);
                auto it = data.find("chunkBudget");
                if (it != data.end()) c->chunkBudget = std::any_cast<int>(it->second);
                it = data.find("seed");
                if (it != data.end()) c->seed = std::any_cast<int>(it->second);
                it = data.find("seaLevel");
                if (it != data.end()) c->seaLevel = std::any_cast<float>(it->second);
                it = data.find("enableFarLod");
                if (it != data.end()) c->enableFarLod = std::any_cast<bool>(it->second);
            };
            registry.register_component(desc);
        }
    });
}

bool SceneComponentIntegration::register_plugin_component(const ComponentDescriptor& descriptor) {
    if (!descriptor.is_valid()) {
        return false;
    }

    // Ensure builtin components are registered first
    register_builtin_components();

    ComponentRegistry::get().register_component(descriptor);
    return true;
}

bool SceneComponentIntegration::unregister_plugin_component(TypeId typeId) {
    // Don't allow unregistering builtin types
    if (is_builtin_type(typeId)) {
        return false;
    }
    return ComponentRegistry::get().unregister_component(typeId);
}

// ---------------------------------------------------------------------------
// Builtin typed component helpers (private)
// ---------------------------------------------------------------------------

void* SceneComponentIntegration::get_builtin_typed_component(
    Scene& scene, UUID entityId, TypeId typeId)
{
    if (typeId == BuiltinTypeIds::TransformComponent) {
        auto it = scene.transformComponents.find(entityId);
        return (it != scene.transformComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::MeshRendererComponent) {
        auto it = scene.meshRendererComponents.find(entityId);
        return (it != scene.meshRendererComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::LightComponent) {
        auto it = scene.lightComponents.find(entityId);
        return (it != scene.lightComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::CameraComponent) {
        auto it = scene.cameraComponents.find(entityId);
        return (it != scene.cameraComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::RigidbodyComponent) {
        auto it = scene.rigidbodyComponents.find(entityId);
        return (it != scene.rigidbodyComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::MaterialComponent) {
        auto it = scene.materialComponents.find(entityId);
        return (it != scene.materialComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::HierarchyComponent) {
        auto it = scene.hierarchyComponents.find(entityId);
        return (it != scene.hierarchyComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::VoxelVolumeComponent) {
        auto it = scene.voxelVolumeComponents.find(entityId);
        return (it != scene.voxelVolumeComponents.end()) ? &it->second : nullptr;
    }
    return nullptr; // Not a builtin type
}

const void* SceneComponentIntegration::get_builtin_typed_component(
    const Scene& scene, UUID entityId, TypeId typeId)
{
    if (typeId == BuiltinTypeIds::TransformComponent) {
        auto it = scene.transformComponents.find(entityId);
        return (it != scene.transformComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::MeshRendererComponent) {
        auto it = scene.meshRendererComponents.find(entityId);
        return (it != scene.meshRendererComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::LightComponent) {
        auto it = scene.lightComponents.find(entityId);
        return (it != scene.lightComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::CameraComponent) {
        auto it = scene.cameraComponents.find(entityId);
        return (it != scene.cameraComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::RigidbodyComponent) {
        auto it = scene.rigidbodyComponents.find(entityId);
        return (it != scene.rigidbodyComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::MaterialComponent) {
        auto it = scene.materialComponents.find(entityId);
        return (it != scene.materialComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::HierarchyComponent) {
        auto it = scene.hierarchyComponents.find(entityId);
        return (it != scene.hierarchyComponents.end()) ? &it->second : nullptr;
    }
    if (typeId == BuiltinTypeIds::VoxelVolumeComponent) {
        auto it = scene.voxelVolumeComponents.find(entityId);
        return (it != scene.voxelVolumeComponents.end()) ? &it->second : nullptr;
    }
    return nullptr;
}

void* SceneComponentIntegration::add_builtin_typed_component(
    Scene& scene, UUID entityId, TypeId typeId)
{
    if (typeId == BuiltinTypeIds::TransformComponent) {
        auto [it, inserted] = scene.transformComponents.try_emplace(entityId, TransformComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::MeshRendererComponent) {
        auto [it, inserted] = scene.meshRendererComponents.try_emplace(entityId, MeshRendererComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::LightComponent) {
        auto [it, inserted] = scene.lightComponents.try_emplace(entityId, LightComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::CameraComponent) {
        auto [it, inserted] = scene.cameraComponents.try_emplace(entityId, CameraComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::RigidbodyComponent) {
        auto [it, inserted] = scene.rigidbodyComponents.try_emplace(entityId, RigidbodyComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::MaterialComponent) {
        auto [it, inserted] = scene.materialComponents.try_emplace(entityId, MaterialComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::HierarchyComponent) {
        auto [it, inserted] = scene.hierarchyComponents.try_emplace(entityId, HierarchyComponent{});
        return &it->second;
    }
    if (typeId == BuiltinTypeIds::VoxelVolumeComponent) {
        auto [it, inserted] = scene.voxelVolumeComponents.try_emplace(entityId, VoxelVolumeComponent{});
        return &it->second;
    }
    return nullptr; // Not a builtin type
}

bool SceneComponentIntegration::remove_builtin_typed_component(
    Scene& scene, UUID entityId, TypeId typeId)
{
    if (typeId == BuiltinTypeIds::TransformComponent) {
        return scene.transformComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::MeshRendererComponent) {
        return scene.meshRendererComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::LightComponent) {
        return scene.lightComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::CameraComponent) {
        return scene.cameraComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::RigidbodyComponent) {
        return scene.rigidbodyComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::MaterialComponent) {
        return scene.materialComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::HierarchyComponent) {
        return scene.hierarchyComponents.erase(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::VoxelVolumeComponent) {
        return scene.voxelVolumeComponents.erase(entityId) > 0;
    }
    return false;
}

bool SceneComponentIntegration::has_builtin_typed_component(
    const Scene& scene, UUID entityId, TypeId typeId)
{
    if (typeId == BuiltinTypeIds::TransformComponent) {
        return scene.transformComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::MeshRendererComponent) {
        return scene.meshRendererComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::LightComponent) {
        return scene.lightComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::CameraComponent) {
        return scene.cameraComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::RigidbodyComponent) {
        return scene.rigidbodyComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::MaterialComponent) {
        return scene.materialComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::HierarchyComponent) {
        return scene.hierarchyComponents.count(entityId) > 0;
    }
    if (typeId == BuiltinTypeIds::VoxelVolumeComponent) {
        return scene.voxelVolumeComponents.count(entityId) > 0;
    }
    return false;
}

std::vector<std::pair<TypeId, void*>> SceneComponentIntegration::get_all_builtin_typed_components(
    Scene& scene, UUID entityId)
{
    std::vector<std::pair<TypeId, void*>> result;
    result.reserve(8); // Max 8 builtin types

    auto tryAdd = [&](TypeId tid, void* ptr) {
        if (ptr) result.emplace_back(tid, ptr);
    };

    tryAdd(BuiltinTypeIds::TransformComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::TransformComponent));
    tryAdd(BuiltinTypeIds::MeshRendererComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::MeshRendererComponent));
    tryAdd(BuiltinTypeIds::LightComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::LightComponent));
    tryAdd(BuiltinTypeIds::CameraComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::CameraComponent));
    tryAdd(BuiltinTypeIds::RigidbodyComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::RigidbodyComponent));
    tryAdd(BuiltinTypeIds::MaterialComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::MaterialComponent));
    tryAdd(BuiltinTypeIds::HierarchyComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::HierarchyComponent));
    tryAdd(BuiltinTypeIds::VoxelVolumeComponent,
           get_builtin_typed_component(scene, entityId, BuiltinTypeIds::VoxelVolumeComponent));

    return result;
}

std::vector<TypeId> SceneComponentIntegration::get_builtin_typed_component_ids(
    const Scene& scene, UUID entityId)
{
    std::vector<TypeId> result;
    result.reserve(8);

    if (scene.transformComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::TransformComponent);
    if (scene.meshRendererComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::MeshRendererComponent);
    if (scene.lightComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::LightComponent);
    if (scene.cameraComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::CameraComponent);
    if (scene.rigidbodyComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::RigidbodyComponent);
    if (scene.materialComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::MaterialComponent);
    if (scene.hierarchyComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::HierarchyComponent);
    if (scene.voxelVolumeComponents.count(entityId) > 0)
        result.push_back(BuiltinTypeIds::VoxelVolumeComponent);

    return result;
}

// ---------------------------------------------------------------------------
// Component operations by TypeId (public API)
// ---------------------------------------------------------------------------

void* SceneComponentIntegration::add_component(Scene& scene, UUID entityId, TypeId typeId) {
    // Try builtin typed stores first
    if (is_builtin_type(typeId)) {
        return add_builtin_typed_component(scene, entityId, typeId);
    }

    // Fall through to generic store
    return scene.add_generic_component(entityId, typeId);
}

bool SceneComponentIntegration::remove_component(Scene& scene, UUID entityId, TypeId typeId) {
    // Try builtin typed stores first
    if (is_builtin_type(typeId)) {
        return remove_builtin_typed_component(scene, entityId, typeId);
    }

    // Fall through to generic store
    return scene.remove_generic_component(entityId, typeId);
}

bool SceneComponentIntegration::has_component(const Scene& scene, UUID entityId, TypeId typeId) {
    // Check builtin typed stores
    if (is_builtin_type(typeId)) {
        return has_builtin_typed_component(scene, entityId, typeId);
    }

    // Check generic store
    return scene.has_generic_component(entityId, typeId);
}

void* SceneComponentIntegration::get_component(Scene& scene, UUID entityId, TypeId typeId) {
    // Try builtin typed stores first
    void* builtin = get_builtin_typed_component(scene, entityId, typeId);
    if (builtin) return builtin;

    // Try generic store
    return scene.get_generic_component(entityId, typeId);
}

const void* SceneComponentIntegration::get_component(
    const Scene& scene, UUID entityId, TypeId typeId)
{
    // Try builtin typed stores first
    const void* builtin = get_builtin_typed_component(scene, entityId, typeId);
    if (builtin) return builtin;

    // Try generic store
    return scene.get_generic_component(entityId, typeId);
}

std::vector<std::pair<TypeId, void*>> SceneComponentIntegration::get_all_components(
    Scene& scene, UUID entityId)
{
    // Start with builtin typed components
    auto result = get_all_builtin_typed_components(scene, entityId);

    // Add generic components
    auto generics = scene.get_all_generic_components(entityId);
    result.insert(result.end(), generics.begin(), generics.end());

    return result;
}

std::vector<TypeId> SceneComponentIntegration::get_component_type_ids(
    const Scene& scene, UUID entityId)
{
    // Start with builtin typed component IDs
    auto result = get_builtin_typed_component_ids(scene, entityId);

    // Add generic component IDs
    auto genericComps = scene.get_all_generic_components(entityId);
    for (const auto& [typeId, ptr] : genericComps) {
        result.push_back(typeId);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Entity cloning
// ---------------------------------------------------------------------------

void SceneComponentIntegration::clone_all_components(
    Scene& scene, UUID source, UUID dest)
{
    // Clone builtin typed components
    {
        auto it = scene.transformComponents.find(source);
        if (it != scene.transformComponents.end()) {
            scene.transformComponents[dest] = it->second;
        }
    }
    {
        auto it = scene.meshRendererComponents.find(source);
        if (it != scene.meshRendererComponents.end()) {
            scene.meshRendererComponents[dest] = it->second;
        }
    }
    {
        auto it = scene.lightComponents.find(source);
        if (it != scene.lightComponents.end()) {
            scene.lightComponents[dest] = it->second;
        }
    }
    {
        auto it = scene.cameraComponents.find(source);
        if (it != scene.cameraComponents.end()) {
            scene.cameraComponents[dest] = it->second;
        }
    }
    {
        auto it = scene.rigidbodyComponents.find(source);
        if (it != scene.rigidbodyComponents.end()) {
            scene.rigidbodyComponents[dest] = it->second;
        }
    }
    {
        auto it = scene.materialComponents.find(source);
        if (it != scene.materialComponents.end()) {
            scene.materialComponents[dest] = it->second;
        }
    }
    {
        auto it = scene.hierarchyComponents.find(source);
        if (it != scene.hierarchyComponents.end()) {
            // Clone hierarchy but clear parent/children to avoid
            // creating invalid hierarchy references
            HierarchyComponent clonedHierarchy;
            clonedHierarchy.parentID = UUID{ 0, 0 };
            scene.hierarchyComponents[dest] = clonedHierarchy;
        }
    }
    {
        auto it = scene.voxelVolumeComponents.find(source);
        if (it != scene.voxelVolumeComponents.end()) {
            scene.voxelVolumeComponents[dest] = it->second;
        }
    }

    // Clone generic components
    scene.clone_entity_generic_components(source, dest);
}

Entity SceneComponentIntegration::clone_entity(Scene& scene, UUID sourceId) {
    // Verify source entity exists
    auto sourceEntity = scene.find_entity_by_id(sourceId);
    if (!sourceEntity.is_valid()) {
        return Entity{};
    }

    // Create new entity with a new UUID
    std::string cloneName = sourceEntity.get_name() + " (Clone)";
    Entity newEntity = scene.create_entity(cloneName);

    // Clone all components from source to new entity
    clone_all_components(scene, sourceId, newEntity.get_id());

    return newEntity;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

SerializedEntity SceneComponentIntegration::serialize_entity(
    const Scene& scene, UUID entityId)
{
    SerializedEntity result;
    result.entityId = entityId;

    // Get entity name
    const Entity* entity = scene.find_entity_by_id_const(entityId);
    if (entity) {
        result.name = entity->get_name();
    }

    // Serialize builtin typed components
    auto builtinTypeIds = get_builtin_typed_component_ids(scene, entityId);
    for (TypeId tid : builtinTypeIds) {
        const void* compPtr = get_builtin_typed_component(scene, entityId, tid);
        if (compPtr) {
            result.components.push_back(serialize_component(tid, compPtr));
        }
    }

    // Serialize generic components
    auto genericComps = scene.get_all_generic_components(entityId);
    for (const auto& [typeId, ptr] : genericComps) {
        if (ptr) {
            result.components.push_back(serialize_component(typeId, ptr));
        }
    }

    return result;
}

SerializedComponent SceneComponentIntegration::serialize_component(
    TypeId typeId, const void* componentData)
{
    SerializedComponent result;
    result.typeId = typeId;

    const auto* desc = ComponentRegistry::get().find(typeId);
    if (desc) {
        result.typeName = desc->name;
        if (desc->serializer && componentData) {
            result.fields = desc->serializer(componentData);
        }
    }

    return result;
}

void SceneComponentIntegration::deserialize_entity(
    Scene& scene, const SerializedEntity& data)
{
    // Ensure entity exists
    Entity entity = scene.find_entity_by_id(data.entityId);
    if (!entity.is_valid()) {
        entity = scene.create_entity_with_id(data.entityId, data.name);
    } else if (!data.name.empty()) {
        scene.rename_entity(data.entityId, data.name);
    }

    // Deserialize each component
    for (const auto& serializedComp : data.components) {
        // Add the component if it doesn't exist
        void* compPtr = get_component(scene, data.entityId, serializedComp.typeId);
        if (!compPtr) {
            compPtr = add_component(scene, data.entityId, serializedComp.typeId);
        }

        if (compPtr) {
            deserialize_component(serializedComp.typeId, compPtr, serializedComp);
        }
    }
}

bool SceneComponentIntegration::deserialize_component(
    TypeId typeId, void* componentData,
    const SerializedComponent& serialized)
{
    if (!componentData) return false;

    const auto* desc = ComponentRegistry::get().find(typeId);
    if (!desc || !desc->deserializer) return false;

    desc->deserializer(componentData, serialized.fields);
    return true;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

std::vector<std::string> SceneComponentIntegration::get_all_component_names() {
    register_builtin_components(); // Ensure registered

    std::vector<std::string> names;
    auto descriptors = ComponentRegistry::get().get_all();
    names.reserve(descriptors.size());
    for (const auto* desc : descriptors) {
        names.push_back(desc->name);
    }
    return names;
}

std::vector<TypeId> SceneComponentIntegration::get_all_registered_type_ids() {
    register_builtin_components(); // Ensure registered
    return ComponentRegistry::get().get_all_type_ids();
}

std::string SceneComponentIntegration::get_type_name(TypeId typeId) {
    return ComponentRegistry::get().get_name(typeId);
}

std::vector<FieldAccessor> SceneComponentIntegration::get_fields(TypeId typeId) {
    return ComponentRegistry::get().get_fields(typeId);
}

bool SceneComponentIntegration::is_builtin_type(TypeId typeId) {
    return typeId == BuiltinTypeIds::TransformComponent
        || typeId == BuiltinTypeIds::MeshRendererComponent
        || typeId == BuiltinTypeIds::LightComponent
        || typeId == BuiltinTypeIds::CameraComponent
        || typeId == BuiltinTypeIds::RigidbodyComponent
        || typeId == BuiltinTypeIds::MaterialComponent
        || typeId == BuiltinTypeIds::HierarchyComponent
        || typeId == BuiltinTypeIds::VoxelVolumeComponent;
}

std::vector<TypeId> SceneComponentIntegration::get_builtin_type_ids() {
    return {
        BuiltinTypeIds::TransformComponent,
        BuiltinTypeIds::MeshRendererComponent,
        BuiltinTypeIds::LightComponent,
        BuiltinTypeIds::CameraComponent,
        BuiltinTypeIds::RigidbodyComponent,
        BuiltinTypeIds::MaterialComponent,
        BuiltinTypeIds::HierarchyComponent,
        BuiltinTypeIds::VoxelVolumeComponent,
    };
}

} // namespace Engine

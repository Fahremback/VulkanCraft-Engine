#pragma once

// ---------------------------------------------------------------------------
// SceneComponentIntegration.hpp
//
// High-level integration layer between Scene, ComponentRegistry, and the
// Reflection system. Provides functions to:
//   - Register all builtin component types as generic descriptors
//   - Register plugin-defined component types at runtime
//   - Create/remove components by TypeId on entities
//   - Serialize/deserialize all components of an entity generically
//   - Clone an entity with all its components (typed + generic)
//   - Query and iterate components through the generic system
//
// This is the main entry point for code that needs to work with components
// without compile-time knowledge of the concrete types (e.g., the editor
// inspector, plugin system, network replication, undo/redo).
// ---------------------------------------------------------------------------

#include "Scene.hpp"
#include "ComponentRegistry.hpp"
#include "../core/reflection/Reflection.hpp"
#include "../core/uuid/UUID.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <any>
#include <functional>
#include <utility>
#include <mutex>
#include <cassert>

namespace Engine {

// ---------------------------------------------------------------------------
// Builtin TypeId constants for core component types.
// These are compile-time constants derived from the component struct names.
// ---------------------------------------------------------------------------

namespace BuiltinTypeIds {
    constexpr TypeId TransformComponent     = type_id("TransformComponent");
    constexpr TypeId MeshRendererComponent   = type_id("MeshRendererComponent");
    constexpr TypeId LightComponent          = type_id("LightComponent");
    constexpr TypeId CameraComponent         = type_id("CameraComponent");
    constexpr TypeId RigidbodyComponent      = type_id("RigidbodyComponent");
    constexpr TypeId MaterialComponent       = type_id("MaterialComponent");
    constexpr TypeId HierarchyComponent      = type_id("HierarchyComponent");
    constexpr TypeId VoxelVolumeComponent    = type_id("VoxelVolumeComponent");
} // namespace BuiltinTypeIds

// ---------------------------------------------------------------------------
// SerializedEntity: a fully serialized entity with all component data.
// Used for clipboard, undo/redo, network replication, etc.
// ---------------------------------------------------------------------------

struct SerializedComponent {
    TypeId      typeId{ 0 };
    std::string typeName;
    std::unordered_map<std::string, std::any> fields;
};

struct SerializedEntity {
    UUID        entityId;
    std::string name;
    std::vector<SerializedComponent> components;

    /// Check if the serialized entity has a component of the given type.
    bool has_component(TypeId typeId) const {
        for (const auto& comp : components) {
            if (comp.typeId == typeId) return true;
        }
        return false;
    }

    /// Find a serialized component by TypeId. Returns nullptr if not found.
    const SerializedComponent* find_component(TypeId typeId) const {
        for (const auto& comp : components) {
            if (comp.typeId == typeId) return &comp;
        }
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// SceneComponentIntegration: static utility class
// ---------------------------------------------------------------------------

class SceneComponentIntegration {
public:
    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /// Register all builtin engine component types as generic descriptors
    /// in the ComponentRegistry. This bridges the legacy typed stores with
    /// the new generic system. Safe to call multiple times (idempotent).
    static void register_builtin_components();

    /// Register a plugin-defined component type. The descriptor must be
    /// fully populated. Returns true on success, false if the descriptor
    /// is invalid. Thread-safe.
    static bool register_plugin_component(const ComponentDescriptor& descriptor);

    /// Unregister a plugin component. Returns true if it was found and removed.
    /// WARNING: Any entities in any Scene that have this component will still
    /// hold the data, but the descriptor will no longer be available for
    /// serialization/cloning/inspection. Clean up entities before unregistering.
    static bool unregister_plugin_component(TypeId typeId);

    /// Register a component type from a C++ struct that has been reflected
    /// with REFLECT_BEGIN/REFLECT_END. Convenience wrapper around
    /// ComponentRegistry::register_component_type<T>.
    template<typename T>
    static void register_reflected_component(const std::string& typeName) {
        ComponentRegistry::get().register_component_type<T>(typeName);
    }

    // -----------------------------------------------------------------------
    // Component operations by TypeId
    // -----------------------------------------------------------------------

    /// Add a component to an entity by TypeId. For builtin types, this
    /// delegates to the typed store if the type has a known mapping.
    /// For generic/plugin types, uses the generic store.
    /// Returns a raw pointer to the component data, or nullptr on failure.
    static void* add_component(Scene& scene, UUID entityId, TypeId typeId);

    /// Remove a component from an entity by TypeId. For builtin types,
    /// removes from the typed store. For generic types, removes from
    /// the generic store.
    /// Returns true if the component was found and removed.
    static bool remove_component(Scene& scene, UUID entityId, TypeId typeId);

    /// Check whether an entity has a component of the given type.
    /// Checks both typed and generic stores.
    static bool has_component(const Scene& scene, UUID entityId, TypeId typeId);

    /// Get a pointer to a component on an entity by TypeId.
    /// Checks typed stores first, then generic.
    /// Returns nullptr if not found.
    static void* get_component(Scene& scene, UUID entityId, TypeId typeId);

    /// Get a const pointer to a component on an entity by TypeId.
    static const void* get_component(const Scene& scene, UUID entityId, TypeId typeId);

    /// Get all components (typed + generic) for an entity.
    /// Returns a vector of (TypeId, void*) pairs.
    static std::vector<std::pair<TypeId, void*>> get_all_components(
        Scene& scene, UUID entityId);

    /// Get all component TypeIds that an entity has.
    static std::vector<TypeId> get_component_type_ids(
        const Scene& scene, UUID entityId);

    // -----------------------------------------------------------------------
    // Entity cloning
    // -----------------------------------------------------------------------

    /// Clone all components (typed + generic) from source entity to dest
    /// entity within the same scene. Both entities must already exist.
    /// Typed components are copied directly; generic components are
    /// deep-cloned through the ComponentRegistry.
    static void clone_all_components(Scene& scene, UUID source, UUID dest);

    /// Clone an entity fully: creates a new entity in the scene with a
    /// new UUID, copies all components (typed + generic), and returns
    /// the new entity. The new entity gets the source's name with
    /// " (Clone)" appended.
    static Entity clone_entity(Scene& scene, UUID sourceId);

    // -----------------------------------------------------------------------
    // Serialization
    // -----------------------------------------------------------------------

    /// Serialize all components of an entity to a SerializedEntity struct.
    /// Includes both typed (builtin) and generic components.
    static SerializedEntity serialize_entity(const Scene& scene, UUID entityId);

    /// Serialize a single component by TypeId.
    /// Returns a SerializedComponent with the field data.
    static SerializedComponent serialize_component(
        TypeId typeId, const void* componentData);

    /// Deserialize a SerializedEntity back into a Scene. If the entity
    /// doesn't exist, it is created. If it exists, its components are
    /// updated/added.
    static void deserialize_entity(Scene& scene, const SerializedEntity& data);

    /// Deserialize a single component from a SerializedComponent into
    /// existing component data.
    static bool deserialize_component(
        TypeId typeId, void* componentData,
        const SerializedComponent& serialized);

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    /// Get all registered component type names (builtin + plugin).
    static std::vector<std::string> get_all_component_names();

    /// Get all registered TypeIds (builtin + plugin).
    static std::vector<TypeId> get_all_registered_type_ids();

    /// Get the human-readable name for a TypeId.
    static std::string get_type_name(TypeId typeId);

    /// Get the reflected fields for a component type.
    static std::vector<FieldAccessor> get_fields(TypeId typeId);

    /// Check if a TypeId corresponds to a builtin component type.
    static bool is_builtin_type(TypeId typeId);

    /// Get the list of all builtin TypeIds.
    static std::vector<TypeId> get_builtin_type_ids();

private:
    /// Internal: get a typed component pointer for builtin types.
    /// Returns nullptr if the TypeId doesn't map to a builtin typed store.
    static void* get_builtin_typed_component(Scene& scene, UUID entityId, TypeId typeId);
    static const void* get_builtin_typed_component(const Scene& scene, UUID entityId, TypeId typeId);

    /// Internal: add a builtin typed component.
    static void* add_builtin_typed_component(Scene& scene, UUID entityId, TypeId typeId);

    /// Internal: remove a builtin typed component.
    static bool remove_builtin_typed_component(Scene& scene, UUID entityId, TypeId typeId);

    /// Internal: check if entity has a builtin typed component.
    static bool has_builtin_typed_component(const Scene& scene, UUID entityId, TypeId typeId);

    /// Internal: collect all builtin typed components for an entity.
    static std::vector<std::pair<TypeId, void*>> get_all_builtin_typed_components(
        Scene& scene, UUID entityId);

    /// Internal: collect all builtin typed component TypeIds for an entity.
    static std::vector<TypeId> get_builtin_typed_component_ids(
        const Scene& scene, UUID entityId);
};

} // namespace Engine

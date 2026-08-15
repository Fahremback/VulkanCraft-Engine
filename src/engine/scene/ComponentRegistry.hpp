#pragma once

// ---------------------------------------------------------------------------
// ComponentRegistry.hpp
//
// Central registry for generic (type-erased) component types. Allows plugins
// and engine code to register new component types at runtime using TypeId
// identifiers derived from FNV-1a hashed names.
//
// The registry stores ComponentDescriptors and provides convenience helpers
// for creating, destroying, cloning, and serializing components without
// compile-time knowledge of the concrete type.
// ---------------------------------------------------------------------------

#include "../core/reflection/Reflection.hpp"
#include "../core/uuid/UUID.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <mutex>
#include <cassert>
#include <cstdint>

namespace Engine {

class Scene;

// ---------------------------------------------------------------------------
// ComponentHandle: a type-erased, RAII-managed owning pointer to a component.
// Stores the raw pointer + a destructor so that the component can be freed
// without knowing its concrete type at the call site.
// ---------------------------------------------------------------------------

class ComponentHandle {
public:
    ComponentHandle() = default;

    ComponentHandle(void* ptr, ComponentDestructor dtor, TypeId typeId)
        : m_ptr(ptr), m_destructor(std::move(dtor)), m_typeId(typeId) {}

    ~ComponentHandle() {
        reset();
    }

    // Move-only
    ComponentHandle(ComponentHandle&& other) noexcept
        : m_ptr(other.m_ptr)
        , m_destructor(std::move(other.m_destructor))
        , m_typeId(other.m_typeId)
    {
        other.m_ptr = nullptr;
        other.m_typeId = 0;
    }

    ComponentHandle& operator=(ComponentHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            m_destructor = std::move(other.m_destructor);
            m_typeId = other.m_typeId;
            other.m_ptr = nullptr;
            other.m_typeId = 0;
        }
        return *this;
    }

    ComponentHandle(const ComponentHandle&) = delete;
    ComponentHandle& operator=(const ComponentHandle&) = delete;

    /// Access the raw component pointer.
    void* get() const { return m_ptr; }

    /// Get the TypeId of the component this handle owns.
    TypeId get_type_id() const { return m_typeId; }

    /// Check if the handle owns a valid component.
    bool is_valid() const { return m_ptr != nullptr; }

    /// Explicit bool conversion.
    explicit operator bool() const { return is_valid(); }

    /// Release ownership without destroying. Caller takes ownership.
    void* release() {
        void* tmp = m_ptr;
        m_ptr = nullptr;
        m_typeId = 0;
        return tmp;
    }

    /// Destroy the owned component and reset to empty.
    void reset() {
        if (m_ptr && m_destructor) {
            m_destructor(m_ptr);
        }
        m_ptr = nullptr;
        m_typeId = 0;
    }

private:
    void*               m_ptr{ nullptr };
    ComponentDestructor m_destructor;
    TypeId              m_typeId{ 0 };
};

// ---------------------------------------------------------------------------
// ComponentRegistry: singleton managing all registered component descriptors
// and providing factory/lifecycle operations.
// ---------------------------------------------------------------------------

class ComponentRegistry {
public:
    /// Get the global singleton instance.
    static ComponentRegistry& get();

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /// Register a component descriptor. The descriptor must be valid
    /// (typeId != 0, factory/destructor/cloner set). Thread-safe.
    void register_component(const ComponentDescriptor& descriptor);

    /// Register a component type using automatic descriptor generation.
    /// Requires that REFLECT_BEGIN/REFLECT_END was invoked for the type
    /// and the reflect function was called before this.
    template<typename T>
    void register_component_type(const std::string& typeName) {
        ComponentDescriptor desc = make_component_descriptor<T>(typeName);
        register_component(desc);
    }

    /// Unregister a component type (e.g., on plugin unload). Thread-safe.
    /// Returns true if the descriptor was found and removed.
    bool unregister_component(TypeId typeId);

    /// Check whether a component type has been registered.
    bool is_registered(TypeId typeId) const;

    // -----------------------------------------------------------------------
    // Querying
    // -----------------------------------------------------------------------

    /// Find a descriptor by TypeId. Returns nullptr if not registered.
    const ComponentDescriptor* find(TypeId typeId) const;

    /// Find a descriptor by name. Returns nullptr if not registered.
    const ComponentDescriptor* find_by_name(const std::string& name) const;

    /// Get all registered descriptors (snapshot). Thread-safe.
    std::vector<const ComponentDescriptor*> get_all() const;

    /// Get the number of registered component types.
    size_t count() const;

    /// Get all registered TypeIds.
    std::vector<TypeId> get_all_type_ids() const;

    /// Get a mapping of TypeId -> component name for all registered types.
    std::unordered_map<TypeId, std::string> get_name_map() const;

    // -----------------------------------------------------------------------
    // Factory operations (create/destroy/clone using type-erased descriptors)
    // -----------------------------------------------------------------------

    /// Create a new default-constructed component of the given type.
    /// Returns an owning ComponentHandle. Handle is invalid if type unknown.
    ComponentHandle create(TypeId typeId) const;

    /// Clone a component. Returns an owning ComponentHandle.
    /// Handle is invalid if type is unknown.
    ComponentHandle clone(TypeId typeId, const void* source) const;

    /// Destroy a component previously created by create() or clone().
    /// This is a convenience method; ComponentHandle::reset() also works.
    void destroy(TypeId typeId, void* component) const;

    // -----------------------------------------------------------------------
    // Serialization operations
    // -----------------------------------------------------------------------

    /// Serialize a component to a key-value map using its descriptor.
    /// Returns empty map if the type has no serializer registered.
    std::unordered_map<std::string, std::any> serialize(
        TypeId typeId, const void* component) const;

    /// Deserialize a component from a key-value map using its descriptor.
    /// Returns false if the type is not registered or has no deserializer.
    bool deserialize(TypeId typeId, void* component,
                     const std::unordered_map<std::string, std::any>& data) const;

    // -----------------------------------------------------------------------
    // Iteration / Introspection
    // -----------------------------------------------------------------------

    /// Iterate over all registered descriptors, invoking the callback for each.
    /// The callback receives a const reference to the descriptor.
    void for_each(const std::function<void(const ComponentDescriptor&)>& callback) const;

    /// Get the reflected fields for a component type.
    /// Returns empty vector if type is not registered.
    std::vector<FieldAccessor> get_fields(TypeId typeId) const;

    /// Get the human-readable name for a TypeId.
    /// Returns empty string if type is not registered.
    std::string get_name(TypeId typeId) const;

    /// Get the size in bytes for a component type.
    size_t get_size(TypeId typeId) const;

private:
    ComponentRegistry() = default;
    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;

    mutable std::shared_mutex m_mutex;

    // Primary storage: TypeId -> descriptor
    std::unordered_map<TypeId, ComponentDescriptor> m_descriptors;

    // Reverse lookup: name -> TypeId
    std::unordered_map<std::string, TypeId> m_nameToTypeId;

    // Track registration order for deterministic iteration
    std::vector<TypeId> m_registrationOrder;
};

} // namespace Engine

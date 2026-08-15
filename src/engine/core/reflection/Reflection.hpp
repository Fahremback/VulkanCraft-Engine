#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <any>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <cassert>
#include <glm/glm.hpp>
#include "../uuid/UUID.hpp"

namespace Engine {

class Scene;

// ---------------------------------------------------------------------------
// TypeId: compile-time type identification via FNV-1a hash
// ---------------------------------------------------------------------------

using TypeId = uint64_t;

/// Compile-time FNV-1a 64-bit hash for type identification.
/// Deterministic across compilations for the same string.
constexpr TypeId type_id(std::string_view name) noexcept {
    constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME        = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET_BASIS;
    for (char c : name) {
        hash ^= static_cast<uint64_t>(c);
        hash *= FNV_PRIME;
    }
    return hash;
}

/// Helper macro to get TypeId from a C++ type at compile-time.
#define ENGINE_TYPE_ID(Type) ::Engine::type_id(#Type)

// ---------------------------------------------------------------------------
// FieldType & FieldAccessor (unchanged from original)
// ---------------------------------------------------------------------------

enum class FieldType {
    Int,
    Float,
    Bool,
    String,
    Vec3,
    Color,
    UUIDRef
};

struct FieldMetaData {
    std::string name;
    std::string displayName;
    float rangeMin{ 0.0f };
    float rangeMax{ 1000.0f };
    bool hasRange{ false };
};

class FieldAccessor {
public:
    FieldType type;
    FieldMetaData metadata;

    std::function<std::any(const void*)> getter;
    std::function<void(void*, const std::any&)> setter;
};

// ---------------------------------------------------------------------------
// ClassMetaData (unchanged from original)
// ---------------------------------------------------------------------------

class ClassMetaData {
public:
    std::string name;
    std::vector<FieldAccessor> fields;

    void add_field(const FieldAccessor& field) {
        fields.push_back(field);
    }
};

// ---------------------------------------------------------------------------
// Component lifecycle function types (type-erased)
// ---------------------------------------------------------------------------

/// Factory: creates a default-constructed component, returns owning pointer.
/// The void* points to heap-allocated memory that must be freed by the destructor.
using ComponentFactory    = std::function<void*()>;

/// Destructor: frees a component previously allocated by the factory.
using ComponentDestructor = std::function<void(void*)>;

/// Clone: deep-copies a component, returns a new heap-allocated copy.
using ComponentCloner     = std::function<void*(const void*)>;

/// Serialize: converts a component to a generic key-value representation.
/// Returns a map of field-name -> std::any for each reflected field.
using ComponentSerializer   = std::function<std::unordered_map<std::string, std::any>(const void*)>;

/// Deserialize: populates a component from a key-value representation.
using ComponentDeserializer = std::function<void(void*, const std::unordered_map<std::string, std::any>&)>;

/// OnAdd callback: invoked when a component is added to an entity.
using ComponentOnAdd    = std::function<void(Scene*, UUID, void*)>;

/// OnRemove callback: invoked just before a component is removed.
using ComponentOnRemove = std::function<void(Scene*, UUID, void*)>;

// ---------------------------------------------------------------------------
// ComponentDescriptor: full description of a component type
// ---------------------------------------------------------------------------

struct ComponentDescriptor {
    TypeId      typeId{ 0 };
    std::string name;
    size_t      sizeBytes{ 0 };

    // Lifecycle
    ComponentFactory      factory;
    ComponentDestructor   destructor;
    ComponentCloner       cloner;

    // Serialization
    ComponentSerializer   serializer;
    ComponentDeserializer deserializer;

    // Callbacks (optional)
    ComponentOnAdd        onAdd;
    ComponentOnRemove     onRemove;

    // Reflection fields from REFLECT macros
    std::vector<FieldAccessor> fields;

    // Validation
    bool is_valid() const {
        return typeId != 0 && factory && destructor && cloner;
    }
};

// ---------------------------------------------------------------------------
// ComponentHandlers (legacy, kept for backward compatibility)
// ---------------------------------------------------------------------------

using ComponentAddHandler    = std::function<void(Scene*, UUID)>;
using ComponentRemoveHandler = std::function<void(Scene*, UUID)>;
using ComponentQueryHandler  = std::function<void*(Scene*, UUID)>;
using ComponentHasHandler    = std::function<bool(const Scene*, UUID)>;

struct ComponentHandlers {
    ComponentAddHandler    add;
    ComponentRemoveHandler remove;
    ComponentQueryHandler  get;
    ComponentHasHandler    has;
};

// ---------------------------------------------------------------------------
// TypeRegistry: global thread-safe registry for types and components
// ---------------------------------------------------------------------------

class TypeRegistry {
public:
    static TypeRegistry& get() {
        static TypeRegistry inst;
        return inst;
    }

    // --- ClassMetaData registration (legacy interface, still fully supported) ---

    void register_class(const ClassMetaData& meta) {
        std::unique_lock lock(m_mutex);
        m_classes[meta.name] = meta;
    }

    const ClassMetaData* find_class(const std::string& name) const {
        std::shared_lock lock(m_mutex);
        auto it = m_classes.find(name);
        if (it != m_classes.end()) return &it->second;
        return nullptr;
    }

    const std::unordered_map<std::string, ClassMetaData>& get_all_classes() const {
        return m_classes;
    }

    // --- ComponentHandlers registration (legacy interface) ---

    void register_component_handlers(const std::string& componentName, ComponentHandlers handlers) {
        std::unique_lock lock(m_mutex);
        m_componentHandlers[componentName] = std::move(handlers);
    }

    const ComponentHandlers* find_component_handlers(const std::string& componentName) const {
        std::shared_lock lock(m_mutex);
        auto it = m_componentHandlers.find(componentName);
        if (it != m_componentHandlers.end()) return &it->second;
        return nullptr;
    }

    const std::unordered_map<std::string, ComponentHandlers>& get_all_component_handlers() const {
        return m_componentHandlers;
    }

    // --- ComponentDescriptor registration (new generic interface) ---

    /// Register a fully-described component type. Thread-safe.
    void register_component_descriptor(const ComponentDescriptor& desc) {
        assert(desc.is_valid() && "ComponentDescriptor must have typeId, factory, destructor, and cloner");
        std::unique_lock lock(m_mutex);
        m_descriptors[desc.typeId] = desc;
        m_nameToTypeId[desc.name] = desc.typeId;
    }

    /// Find a descriptor by TypeId. Returns nullptr if not registered.
    const ComponentDescriptor* find_descriptor(TypeId id) const {
        std::shared_lock lock(m_mutex);
        auto it = m_descriptors.find(id);
        if (it != m_descriptors.end()) return &it->second;
        return nullptr;
    }

    /// Find a descriptor by name. Returns nullptr if not registered.
    const ComponentDescriptor* find_descriptor_by_name(const std::string& name) const {
        std::shared_lock lock(m_mutex);
        auto nit = m_nameToTypeId.find(name);
        if (nit == m_nameToTypeId.end()) return nullptr;
        auto dit = m_descriptors.find(nit->second);
        if (dit != m_descriptors.end()) return &dit->second;
        return nullptr;
    }

    /// Get all registered descriptors. Caller must hold no lock.
    std::vector<const ComponentDescriptor*> get_all_descriptors() const {
        std::shared_lock lock(m_mutex);
        std::vector<const ComponentDescriptor*> result;
        result.reserve(m_descriptors.size());
        for (auto& [id, desc] : m_descriptors) {
            result.push_back(&desc);
        }
        return result;
    }

    /// Get all registered TypeIds mapped to their names.
    std::unordered_map<TypeId, std::string> get_type_id_name_map() const {
        std::shared_lock lock(m_mutex);
        std::unordered_map<TypeId, std::string> result;
        for (auto& [id, desc] : m_descriptors) {
            result[id] = desc.name;
        }
        return result;
    }

    /// Check if a descriptor is registered for a TypeId.
    bool has_descriptor(TypeId id) const {
        std::shared_lock lock(m_mutex);
        return m_descriptors.count(id) > 0;
    }

    /// Unregister a component descriptor (for plugin unloading).
    bool unregister_descriptor(TypeId id) {
        std::unique_lock lock(m_mutex);
        auto it = m_descriptors.find(id);
        if (it == m_descriptors.end()) return false;
        m_nameToTypeId.erase(it->second.name);
        m_descriptors.erase(it);
        return true;
    }

private:
    TypeRegistry() = default;
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    mutable std::shared_mutex m_mutex;

    // Legacy registries
    std::unordered_map<std::string, ClassMetaData>      m_classes;
    std::unordered_map<std::string, ComponentHandlers>   m_componentHandlers;

    // New generic component descriptor registry
    std::unordered_map<TypeId, ComponentDescriptor>      m_descriptors;
    std::unordered_map<std::string, TypeId>              m_nameToTypeId;
};

// ---------------------------------------------------------------------------
// Helper: build a ComponentDescriptor from a reflected C++ struct
// ---------------------------------------------------------------------------

/// Automatically builds a ComponentDescriptor for a type T that has been
/// reflected with REFLECT_BEGIN/REFLECT_END. The ClassMetaData must already
/// be registered in TypeRegistry before calling this.
template<typename T>
ComponentDescriptor make_component_descriptor(const std::string& typeName) {
    ComponentDescriptor desc;
    desc.typeId    = type_id(std::string_view(typeName));
    desc.name      = typeName;
    desc.sizeBytes = sizeof(T);

    // Factory: default-construct on heap
    desc.factory = []() -> void* {
        return static_cast<void*>(new T{});
    };

    // Destructor: delete the heap object
    desc.destructor = [](void* ptr) {
        delete static_cast<T*>(ptr);
    };

    // Cloner: copy-construct on heap
    desc.cloner = [](const void* src) -> void* {
        return static_cast<void*>(new T(*static_cast<const T*>(src)));
    };

    // Serializer: use reflected fields to extract values
    desc.serializer = [typeName](const void* ptr) -> std::unordered_map<std::string, std::any> {
        std::unordered_map<std::string, std::any> result;
        const auto* meta = TypeRegistry::get().find_class(typeName);
        if (meta) {
            for (const auto& field : meta->fields) {
                result[field.metadata.name] = field.getter(ptr);
            }
        }
        return result;
    };

    // Deserializer: use reflected fields to set values
    desc.deserializer = [typeName](void* ptr, const std::unordered_map<std::string, std::any>& data) {
        const auto* meta = TypeRegistry::get().find_class(typeName);
        if (meta) {
            for (const auto& field : meta->fields) {
                auto it = data.find(field.metadata.name);
                if (it != data.end()) {
                    field.setter(ptr, it->second);
                }
            }
        }
    };

    // Copy fields from ClassMetaData if available
    const auto* meta = TypeRegistry::get().find_class(typeName);
    if (meta) {
        desc.fields = meta->fields;
    }

    return desc;
}

} // namespace Engine

// ---------------------------------------------------------------------------
// REFLECT macros (unchanged API, same behavior as original)
// ---------------------------------------------------------------------------

#define REFLECT_BEGIN(Type) \
    inline void reflect_##Type() { \
        ::Engine::ClassMetaData meta; \
        meta.name = #Type; \
        using ClassType = Type;

#define REFLECT_FIELD(FieldName, FieldEnum, DisplayName) \
    { \
        ::Engine::FieldAccessor accessor; \
        accessor.type = FieldEnum; \
        accessor.metadata.name = #FieldName; \
        accessor.metadata.displayName = DisplayName; \
        accessor.getter = [](const void* obj) -> std::any { return static_cast<const ClassType*>(obj)->FieldName; }; \
        accessor.setter = [](void* obj, const std::any& val) { static_cast<ClassType*>(obj)->FieldName = std::any_cast<decltype(ClassType::FieldName)>(val); }; \
        meta.add_field(accessor); \
    }

#define REFLECT_FIELD_RANGE(FieldName, FieldEnum, DisplayName, MinVal, MaxVal) \
    { \
        ::Engine::FieldAccessor accessor; \
        accessor.type = FieldEnum; \
        accessor.metadata.name = #FieldName; \
        accessor.metadata.displayName = DisplayName; \
        accessor.metadata.hasRange = true; \
        accessor.metadata.rangeMin = MinVal; \
        accessor.metadata.rangeMax = MaxVal; \
        accessor.getter = [](const void* obj) -> std::any { return static_cast<const ClassType*>(obj)->FieldName; }; \
        accessor.setter = [](void* obj, const std::any& val) { static_cast<ClassType*>(obj)->FieldName = std::any_cast<decltype(ClassType::FieldName)>(val); }; \
        meta.add_field(accessor); \
    }

#define REFLECT_END(Type) \
        ::Engine::TypeRegistry::get().register_class(meta); \
    }

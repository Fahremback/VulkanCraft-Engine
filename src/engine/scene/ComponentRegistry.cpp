// ---------------------------------------------------------------------------
// ComponentRegistry.cpp
//
// Implementation of the generic component registry. Provides thread-safe
// registration, lookup, factory, serialization, and introspection for
// type-erased components identified by TypeId.
// ---------------------------------------------------------------------------

#include "ComponentRegistry.hpp"
#include <algorithm>
#include <stdexcept>
#include <cassert>

namespace Engine {

// ---------------------------------------------------------------------------
// Singleton accessor
// ---------------------------------------------------------------------------

ComponentRegistry& ComponentRegistry::get() {
    static ComponentRegistry instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void ComponentRegistry::register_component(const ComponentDescriptor& descriptor) {
    assert(descriptor.is_valid() &&
           "ComponentDescriptor must have a valid typeId, factory, destructor, and cloner");

    if (!descriptor.is_valid()) {
        return; // Silently ignore invalid descriptors in release builds
    }

    std::unique_lock lock(m_mutex);

    // Check for duplicate registration (same TypeId)
    auto existingIt = m_descriptors.find(descriptor.typeId);
    if (existingIt != m_descriptors.end()) {
        // Update existing descriptor (allows re-registration / hot-reload)
        existingIt->second = descriptor;
        // Update name mapping in case name changed
        // Remove old name mapping if it pointed to this TypeId
        for (auto it = m_nameToTypeId.begin(); it != m_nameToTypeId.end(); ) {
            if (it->second == descriptor.typeId && it->first != descriptor.name) {
                it = m_nameToTypeId.erase(it);
            } else {
                ++it;
            }
        }
        m_nameToTypeId[descriptor.name] = descriptor.typeId;
        return;
    }

    // New registration
    m_descriptors[descriptor.typeId] = descriptor;
    m_nameToTypeId[descriptor.name]  = descriptor.typeId;
    m_registrationOrder.push_back(descriptor.typeId);

    // Also register into the global TypeRegistry for backward compatibility
    TypeRegistry::get().register_component_descriptor(descriptor);
}

bool ComponentRegistry::unregister_component(TypeId typeId) {
    std::unique_lock lock(m_mutex);

    auto it = m_descriptors.find(typeId);
    if (it == m_descriptors.end()) {
        return false;
    }

    // Remove name mapping
    m_nameToTypeId.erase(it->second.name);

    // Remove from registration order
    m_registrationOrder.erase(
        std::remove(m_registrationOrder.begin(), m_registrationOrder.end(), typeId),
        m_registrationOrder.end()
    );

    // Remove descriptor
    m_descriptors.erase(it);

    // Also unregister from global TypeRegistry
    TypeRegistry::get().unregister_descriptor(typeId);

    return true;
}

bool ComponentRegistry::is_registered(TypeId typeId) const {
    std::shared_lock lock(m_mutex);
    return m_descriptors.count(typeId) > 0;
}

// ---------------------------------------------------------------------------
// Querying
// ---------------------------------------------------------------------------

const ComponentDescriptor* ComponentRegistry::find(TypeId typeId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it != m_descriptors.end()) {
        return &it->second;
    }
    return nullptr;
}

const ComponentDescriptor* ComponentRegistry::find_by_name(const std::string& name) const {
    std::shared_lock lock(m_mutex);
    auto nameIt = m_nameToTypeId.find(name);
    if (nameIt == m_nameToTypeId.end()) {
        return nullptr;
    }
    auto descIt = m_descriptors.find(nameIt->second);
    if (descIt != m_descriptors.end()) {
        return &descIt->second;
    }
    return nullptr;
}

std::vector<const ComponentDescriptor*> ComponentRegistry::get_all() const {
    std::shared_lock lock(m_mutex);
    std::vector<const ComponentDescriptor*> result;
    result.reserve(m_registrationOrder.size());
    for (TypeId id : m_registrationOrder) {
        auto it = m_descriptors.find(id);
        if (it != m_descriptors.end()) {
            result.push_back(&it->second);
        }
    }
    return result;
}

size_t ComponentRegistry::count() const {
    std::shared_lock lock(m_mutex);
    return m_descriptors.size();
}

std::vector<TypeId> ComponentRegistry::get_all_type_ids() const {
    std::shared_lock lock(m_mutex);
    return m_registrationOrder; // copy
}

std::unordered_map<TypeId, std::string> ComponentRegistry::get_name_map() const {
    std::shared_lock lock(m_mutex);
    std::unordered_map<TypeId, std::string> result;
    result.reserve(m_descriptors.size());
    for (const auto& [id, desc] : m_descriptors) {
        result[id] = desc.name;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Factory operations
// ---------------------------------------------------------------------------

ComponentHandle ComponentRegistry::create(TypeId typeId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it == m_descriptors.end() || !it->second.factory) {
        return ComponentHandle{}; // Invalid handle
    }

    const auto& desc = it->second;
    void* ptr = desc.factory();
    if (!ptr) {
        return ComponentHandle{};
    }

    return ComponentHandle(ptr, desc.destructor, typeId);
}

ComponentHandle ComponentRegistry::clone(TypeId typeId, const void* source) const {
    if (!source) {
        return ComponentHandle{};
    }

    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it == m_descriptors.end() || !it->second.cloner) {
        return ComponentHandle{};
    }

    const auto& desc = it->second;
    void* cloned = desc.cloner(source);
    if (!cloned) {
        return ComponentHandle{};
    }

    return ComponentHandle(cloned, desc.destructor, typeId);
}

void ComponentRegistry::destroy(TypeId typeId, void* component) const {
    if (!component) {
        return;
    }

    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it != m_descriptors.end() && it->second.destructor) {
        it->second.destructor(component);
    }
}

// ---------------------------------------------------------------------------
// Serialization operations
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::any> ComponentRegistry::serialize(
    TypeId typeId, const void* component) const
{
    if (!component) {
        return {};
    }

    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it == m_descriptors.end() || !it->second.serializer) {
        return {};
    }

    return it->second.serializer(component);
}

bool ComponentRegistry::deserialize(TypeId typeId, void* component,
                                     const std::unordered_map<std::string, std::any>& data) const
{
    if (!component) {
        return false;
    }

    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it == m_descriptors.end() || !it->second.deserializer) {
        return false;
    }

    it->second.deserializer(component, data);
    return true;
}

// ---------------------------------------------------------------------------
// Iteration / Introspection
// ---------------------------------------------------------------------------

void ComponentRegistry::for_each(
    const std::function<void(const ComponentDescriptor&)>& callback) const
{
    if (!callback) return;

    std::shared_lock lock(m_mutex);
    for (TypeId id : m_registrationOrder) {
        auto it = m_descriptors.find(id);
        if (it != m_descriptors.end()) {
            callback(it->second);
        }
    }
}

std::vector<FieldAccessor> ComponentRegistry::get_fields(TypeId typeId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it != m_descriptors.end()) {
        return it->second.fields;
    }
    return {};
}

std::string ComponentRegistry::get_name(TypeId typeId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it != m_descriptors.end()) {
        return it->second.name;
    }
    return {};
}

size_t ComponentRegistry::get_size(TypeId typeId) const {
    std::shared_lock lock(m_mutex);
    auto it = m_descriptors.find(typeId);
    if (it != m_descriptors.end()) {
        return it->second.sizeBytes;
    }
    return 0;
}

} // namespace Engine

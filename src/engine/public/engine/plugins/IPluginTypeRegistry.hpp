#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::plugins {

/// What kind of thing a plugin is registering.
enum class PluginRegistrationKind {
    Type,           ///< A new type (e.g. component, resource)
    Component,      ///< An ECS component
    Asset,          ///< An asset type
    Importer,       ///< An asset importer
    Panel,          ///< An editor panel
    McpTool,        ///< An MCP tool
    Command,        ///< A CLI command
    Node,           ///< A visual scripting node
};

/// Describes one registration from a plugin.
struct PluginRegistration {
    PluginRegistrationKind kind;
    std::string id;             ///< stable unique id (e.g. "my_company.health_bar")
    std::string name;           ///< display name
    std::string description;    ///< human-readable description
    std::string pluginName;     ///< which plugin owns this
    std::string version;        ///< version of the registered thing
};

/// Allows plugins to register types, components, assets, importers,
/// panels and MCP tools without modifying the engine. The host creates
/// one registry and passes it to plugins at on_load time. Registrations
/// are validated (no duplicates, no empty ids) and stored deterministically.
///
/// Headless-testable: register, query, unregister — no GPU, no filesystem.
class IPluginTypeRegistry {
public:
    virtual ~IPluginTypeRegistry() = default;

    /// Register something. Returns false if id is empty or already registered.
    virtual bool register_item(PluginRegistration reg, std::string* error = nullptr) = 0;

    /// Unregister by id. Returns false if not found.
    virtual bool unregister(const std::string& id, std::string* error = nullptr) = 0;

    /// Check if an id is registered.
    [[nodiscard]] virtual bool is_registered(const std::string& id) const = 0;

    /// Get a registration by id. Returns nullptr if not found.
    [[nodiscard]] virtual const PluginRegistration* get(const std::string& id) const = 0;

    /// List all registrations, optionally filtered by kind.
    [[nodiscard]] virtual std::vector<PluginRegistration> list(
        PluginRegistrationKind kindFilter = static_cast<PluginRegistrationKind>(-1)) const = 0;

    /// List all registrations from a specific plugin.
    [[nodiscard]] virtual std::vector<PluginRegistration> list_by_plugin(
        const std::string& pluginName) const = 0;

    /// Clear all registrations (for testing/reset).
    virtual void clear() = 0;

    /// Number of registered items.
    [[nodiscard]] virtual std::size_t count() const = 0;
};

/// Default implementation of IPluginTypeRegistry.
class PluginTypeRegistry final : public IPluginTypeRegistry {
public:
    bool register_item(PluginRegistration reg, std::string* error = nullptr) override {
        if (reg.id.empty()) {
            if (error) *error = "registration id must not be empty";
            return false;
        }
        if (registrations_.count(reg.id)) {
            if (error) *error = "duplicate registration: " + reg.id;
            return false;
        }
        const std::string id = reg.id;
        registrations_.emplace(id, std::move(reg));
        order_.push_back(id);
        return true;
    }

    bool unregister(const std::string& id, std::string* error = nullptr) override {
        auto it = registrations_.find(id);
        if (it == registrations_.end()) {
            if (error) *error = "unknown registration: " + id;
            return false;
        }
        registrations_.erase(it);
        order_.erase(
            std::remove(order_.begin(), order_.end(), id), order_.end());
        return true;
    }

    [[nodiscard]] bool is_registered(const std::string& id) const override {
        return registrations_.count(id) > 0;
    }

    [[nodiscard]] const PluginRegistration* get(const std::string& id) const override {
        auto it = registrations_.find(id);
        return it != registrations_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::vector<PluginRegistration> list(
        PluginRegistrationKind kindFilter = static_cast<PluginRegistrationKind>(-1)) const override {
        std::vector<PluginRegistration> result;
        for (const auto& id : order_) {
            auto it = registrations_.find(id);
            if (it != registrations_.end()) {
                if (static_cast<int>(kindFilter) == -1 ||
                    it->second.kind == kindFilter) {
                    result.push_back(it->second);
                }
            }
        }
        return result;
    }

    [[nodiscard]] std::vector<PluginRegistration> list_by_plugin(
        const std::string& pluginName) const override {
        std::vector<PluginRegistration> result;
        for (const auto& id : order_) {
            auto it = registrations_.find(id);
            if (it != registrations_.end() && it->second.pluginName == pluginName) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    void clear() override {
        registrations_.clear();
        order_.clear();
    }

    [[nodiscard]] std::size_t count() const override { return registrations_.size(); }

private:
    std::unordered_map<std::string, PluginRegistration> registrations_;
    std::vector<std::string> order_;  ///< insertion order for determinism
};

} // namespace engine::plugins

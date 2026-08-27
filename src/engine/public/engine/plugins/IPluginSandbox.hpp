#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace Engine::Plugins {

/// Defines what a plugin is allowed to do. Each permission is a string
/// (e.g. "filesystem:read", "network:connect", "process:spawn") that the
/// host checks before granting access. Plugins declare their required
/// permissions in the manifest; the host grants or denies them at load time.
///
/// Denials are logged but do NOT crash the plugin — the plugin simply
/// receives an error/empty result when it tries the denied operation.
/// This is the same "opt-in by declaration, enforced by the host" pattern
/// used by ILuauSandbox and ISignatureVerifier.
struct PluginPermissions {
    /// Permissions this plugin declares it needs (from manifest).
    std::unordered_set<std::string> required;

    /// Permissions the host actually grants (computed at load time).
    std::unordered_set<std::string> granted;

    /// Check if a specific permission is granted.
    [[nodiscard]] bool has(const std::string& perm) const noexcept {
        return granted.count(perm) > 0;
    }

    /// Check all required permissions are granted.
    [[nodiscard]] bool all_granted() const noexcept {
        for (const auto& r : required) {
            if (granted.count(r) == 0) return false;
        }
        return true;
    }
};

/// Permission constants used across the engine.
namespace Permissions {
    constexpr const char* kFileRead       = "filesystem:read";
    constexpr const char* kFileWrite      = "filesystem:write";
    constexpr const char* kNetworkConnect = "network:connect";
    constexpr const char* kNetworkListen  = "network:listen";
    constexpr const char* kProcessSpawn   = "process:spawn";
    constexpr const char* kAssetRead      = "assets:read";
    constexpr const char* kAssetWrite     = "assets:write";
    constexpr const char* kWorldRead      = "world:read";
    constexpr const char* kWorldWrite     = "world:write";
    constexpr const char* kMcpAccess      = "mcp:access";
    constexpr const char* kEditorAccess   = "editor:access";
} // namespace Permissions

/// Sandbox that wraps a plugin's execution context. The host creates
/// one sandbox per plugin and checks permissions before each privileged
/// operation. Plugins receive the sandbox reference at on_load time.
///
/// Headless-testable: create a sandbox, grant/deny permissions, and
/// verify the host's check paths — no GPU, no filesystem, no network.
class IPluginSandbox {
public:
    virtual ~IPluginSandbox() = default;

    /// Returns the permissions granted to this sandbox's plugin.
    [[nodiscard]] virtual const PluginPermissions& permissions() const = 0;

    /// Check if a specific permission is granted. Returns true if allowed.
    [[nodiscard]] virtual bool check(const std::string& permission) const = 0;

    /// Check multiple permissions at once. Returns true if ALL are granted.
    [[nodiscard]] virtual bool check_all(
        const std::vector<std::string>& permissions) const = 0;

    /// Get the plugin name this sandbox protects.
    [[nodiscard]] virtual const std::string& plugin_name() const = 0;
};

/// Default implementation of IPluginSandbox. Constructed by the host
/// with the granted permissions computed from manifest + host policy.
class PluginSandbox final : public IPluginSandbox {
public:
    explicit PluginSandbox(std::string pluginName, PluginPermissions perms)
        : pluginName_(std::move(pluginName)), perms_(std::move(perms)) {}

    [[nodiscard]] const PluginPermissions& permissions() const override {
        return perms_;
    }

    [[nodiscard]] bool check(const std::string& permission) const override {
        return perms_.has(permission);
    }

    [[nodiscard]] bool check_all(
        const std::vector<std::string>& permissions) const override {
        for (const auto& p : permissions) {
            if (!perms_.has(p)) return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& plugin_name() const override {
        return pluginName_;
    }

private:
    std::string pluginName_;
    PluginPermissions perms_;
};

} // namespace Engine::Plugins

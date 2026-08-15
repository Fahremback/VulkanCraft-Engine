#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Plugins {

// Plugin manifest (README §34): identity, API version, dependencies and load
// order metadata. Parsed from a small stable file format.
struct PluginManifest {
    std::string name;
    std::string version{"0.1.0"};
    std::uint32_t apiVersion{1};                 // engine API the plugin targets
    std::vector<std::string> dependencies;       // plugin names that must load first
    std::vector<std::string> optionalDependencies;
    bool editorOnly{false};
    std::string description;

    // Validation: name non-empty, apiVersion within a sane range, no self-deps.
    struct Validation {
        std::vector<std::string> errors;
        [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
    };
    [[nodiscard]] Validation validate() const;

    [[nodiscard]] bool save_to_file(const std::filesystem::path& path) const;
    [[nodiscard]] bool load_from_file(const std::filesystem::path& path);
};

// Lifecycle hooks mirroring the README contract plus an update tick.
class IPluginRuntime {
public:
    virtual ~IPluginRuntime() = default;
    virtual const std::string& name() const = 0;
    virtual const std::string& version() const = 0;
    virtual std::uint32_t api_version() const noexcept { return 1; }

    virtual bool on_load(std::string& error) = 0;
    virtual void on_unload() = 0;
    virtual void update(double deltaSeconds) { (void)deltaSeconds; }
};

// A plugin loaded from a manifest + runtime implementation.
struct LoadedPlugin {
    PluginManifest manifest;
    std::shared_ptr<IPluginRuntime> runtime;
    bool loaded{false};
    std::string loadError;
};

// Manages plugin lifecycle with dependency-ordered startup, API-version
// compatibility checks, unload in reverse order and hot reload.
class PluginRuntime final {
public:
    using Factory = std::function<std::shared_ptr<IPluginRuntime>(const PluginManifest&)>;

    // Registers a factory keyed by plugin name (host-provided implementations).
    bool register_factory(std::string name, Factory factory);

    // Loads a manifest (file or in-memory) and, if a factory exists, loads the
    // plugin. Dependencies are loaded first (topological order).
    bool load_manifest(const PluginManifest& manifest, std::string* error = nullptr);
    bool load_manifest_file(const std::filesystem::path& path, std::string* error = nullptr);

    // Unloads a plugin and everything that depends on it (reverse order).
    bool unload(const std::string& name, std::string* error = nullptr);

    // Hot reload: unload + reload keeping the same manifest.
    bool reload(const std::string& name, std::string* error = nullptr);

    void update_all(double deltaSeconds);

    [[nodiscard]] std::vector<std::string> loaded_names() const;
    [[nodiscard]] bool is_loaded(const std::string& name) const;
    [[nodiscard]] const LoadedPlugin* plugin(const std::string& name) const;
    [[nodiscard]] std::size_t count() const noexcept { return records_.size(); }

    // Dependency order computed from manifests (cycle-safe; returns false on cycle).
    [[nodiscard]] bool compute_load_order(std::vector<std::string>& order,
                                          std::string* error = nullptr) const;

    // A plugin whose API version is unsupported is rejected at load time.
    [[nodiscard]] static bool api_compatible(std::uint32_t pluginApi,
                                             std::uint32_t engineApi) noexcept;

private:
    struct Record {
        LoadedPlugin plugin;
        Factory factory;
    };

    [[nodiscard]] Record* find(const std::string& name);
    [[nodiscard]] const Record* find(const std::string& name) const;
    // load_record may recurse through dependencies; loading_ tracks the plugin
    // names currently being loaded on this call chain so dependency cycles are
    // rejected instead of recursing forever.
    bool load_record(Record& record, std::string& error);
    bool begin_load(Record& record, std::string& error);
    void end_load(Record& record) noexcept;

    std::unordered_map<std::string, Record> records_;
    std::vector<std::string> loading_;
};

} // namespace Engine::Plugins

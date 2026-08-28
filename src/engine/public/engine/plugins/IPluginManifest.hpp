#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::plugins {

struct PluginVersion {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};

    int compare(const PluginVersion& other) const noexcept {
        if (major != other.major) return major < other.major ? -1 : 1;
        if (minor != other.minor) return minor < other.minor ? -1 : 1;
        if (patch != other.patch) return patch < other.patch ? -1 : 1;
        return 0;
    }

    std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    static PluginVersion parse(const std::string& value) {
        PluginVersion result;
        std::uint32_t* fields[] = {&result.major, &result.minor, &result.patch};
        std::size_t field = 0;
        std::uint32_t number = 0;
        bool has_digit = false;
        for (const char character : value) {
            if (character == '.') {
                if (!has_digit || field >= 3) return {};
                *fields[field++] = number;
                number = 0;
                has_digit = false;
            } else if (character >= '0' && character <= '9') {
                number = number * 10u + static_cast<std::uint32_t>(character - '0');
                has_digit = true;
            } else {
                return {};
            }
        }
        if (!has_digit || field != 2) return {};
        *fields[field] = number;
        return result;
    }

    bool operator==(const PluginVersion& other) const noexcept { return compare(other) == 0; }
    bool operator!=(const PluginVersion& other) const noexcept { return !(*this == other); }
    bool operator<(const PluginVersion& other) const noexcept { return compare(other) < 0; }
    bool operator>(const PluginVersion& other) const noexcept { return compare(other) > 0; }
    bool operator<=(const PluginVersion& other) const noexcept { return compare(other) <= 0; }
    bool operator>=(const PluginVersion& other) const noexcept { return compare(other) >= 0; }
};

struct VersionConstraint {
    std::string raw;

    bool satisfies(const PluginVersion& version) const {
        if (raw == "*") return true;
        const auto parse_and_match = [&](const std::string& text, bool exact) {
            const auto required = PluginVersion::parse(text);
            if (required.to_string() != text) return false;
            return exact ? version == required : version >= required;
        };
        if (raw.rfind(">=", 0) == 0) return parse_and_match(raw.substr(2), false);
        if (raw.rfind("==", 0) == 0) return parse_and_match(raw.substr(2), true);
        return false;
    }
};

struct PluginDependency {
    std::string name;
    VersionConstraint constraint;
    bool required{true};
};

enum class PluginAbi : std::uint8_t { Cpp, Luau, Wasm, VisualScript };

struct PluginCapabilities {
    bool provides_types{false};
    bool provides_components{false};
    bool provides_assets{false};
    bool provides_importers{false};
    bool provides_panels{false};
    bool provides_mcp_tools{false};
    bool provides_commands{false};
    bool provides_nodes{false};
    bool provides_events{false};
    bool provides_ui{false};
};

struct PluginManifest {
    std::string name;
    std::string display_name;
    std::string description;
    std::string author;
    PluginVersion version{0, 0, 1};
    PluginAbi abi{PluginAbi::Cpp};
    std::vector<PluginDependency> dependencies;
    std::vector<std::string> permissions;
    PluginCapabilities capabilities;
    std::unordered_map<std::string, std::string> metadata;

    [[nodiscard]] bool validate(std::string& error) const {
        if (name.empty()) { error = "plugin name must not be empty"; return false; }
        if (display_name.empty()) { error = "display_name must not be empty"; return false; }
        if (version.major == 0 && version.minor == 0 && version.patch == 0) { error = "version must not be 0.0.0"; return false; }
        std::unordered_set<std::string> seen;
        for (const auto& dependency : dependencies) {
            if (dependency.name.empty() || dependency.constraint.raw.empty()) { error = "invalid dependency"; return false; }
            if (!seen.insert(dependency.name).second) { error = "duplicate dependency"; return false; }
        }
        return true;
    }

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static PluginManifest from_json(const std::string&, std::string&);
};

enum class PluginState : std::uint8_t { Unloaded, Loading, Loaded, Error, Disabled, Reloading };

struct PluginRuntimeInfo {
    PluginManifest manifest;
    PluginState state{PluginState::Unloaded};
    std::string error_message;
    std::uint64_t load_time_ms{0};
    std::uint64_t memory_bytes{0};
    std::string library_path;
    std::string script_path;
};

class IPluginManifestManager {
public:
    virtual ~IPluginManifestManager() = default;
    virtual bool register_manifest(const PluginManifest&, std::string&) = 0;
    virtual bool unregister(const std::string&, std::string&) = 0;
    virtual const PluginManifest* get(const std::string&) const = 0;
    virtual std::vector<PluginManifest> list() const = 0;
    virtual std::vector<std::string> resolve_dependencies(std::string&) const = 0;
    virtual bool can_load(const std::string&, std::string&) const = 0;
    virtual const PluginRuntimeInfo* get_runtime_info(const std::string&) const = 0;
    virtual bool set_state(const std::string&, PluginState, std::string&) = 0;
};

} // namespace engine::plugins

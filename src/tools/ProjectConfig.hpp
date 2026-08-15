#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Tools {

enum class BuildProfile : uint8_t { Debug, Development, Shipping, Server, Editor };
enum class TargetPlatform : uint8_t { Windows, Linux, MacOS, DedicatedServer };

[[nodiscard]] const char* profile_name(BuildProfile profile) noexcept;
[[nodiscard]] const char* platform_name(TargetPlatform platform) noexcept;
[[nodiscard]] std::optional<BuildProfile> profile_from_string(std::string_view value) noexcept;

// Project-level configuration (README §39): initial scene, plugin list,
// target platform and the validation rules that gate a build.
struct ProjectConfig {
    std::string name{"MyProject"};
    std::string version{"0.1.0"};
    std::filesystem::path projectDirectory;
    std::filesystem::path enginePath;

    // Content layout (mirrors the README project structure).
    std::filesystem::path sourceAssets{"Assets"};
    std::filesystem::path scenesDirectory{"Scenes"};
    std::filesystem::path intermediateDirectory{"Intermediate"};
    std::filesystem::path buildDirectory{"Build"};

    std::string initialScene{"Scenes/Main.scene"};
    std::vector<std::string> enabledPlugins{"VoxelWorld", "Vehicles", "Weapons", "Missions"};
    TargetPlatform targetPlatform{TargetPlatform::Windows};
    BuildProfile activeProfile{BuildProfile::Shipping};

    // ── Resolved absolute paths ──
    [[nodiscard]] std::filesystem::path assets_path() const;
    [[nodiscard]] std::filesystem::path scenes_path() const;
    [[nodiscard]] std::filesystem::path intermediate_path() const;
    [[nodiscard]] std::filesystem::path build_path() const;
    [[nodiscard]] std::filesystem::path config_file_path() const { return projectDirectory / "ProjectConfig.json"; }

    // Validation: every check must pass for a build to proceed.
    struct ValidationReport {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
        [[nodiscard]] size_t error_count() const noexcept { return errors.size(); }
        [[nodiscard]] size_t warning_count() const noexcept { return warnings.size(); }
    };

    [[nodiscard]] ValidationReport validate() const;
    [[nodiscard]] bool is_plugin_enabled(std::string_view name) const;
    bool set_plugin_enabled(std::string_view name, bool enabled);

    // JSON-ish persistence (stable line format, same family as other assets).
    [[nodiscard]] bool save_to_file() const;
    [[nodiscard]] bool load_from_file();
};

} // namespace Engine::Tools

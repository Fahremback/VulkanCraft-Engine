#include "ProjectConfig.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace Engine::Tools {

const char* profile_name(BuildProfile profile) noexcept {
    switch (profile) {
        case BuildProfile::Debug: return "Debug";
        case BuildProfile::Development: return "Development";
        case BuildProfile::Shipping: return "Shipping";
        case BuildProfile::Server: return "Server";
        case BuildProfile::Editor: return "Editor";
    }
    return "Unknown";
}

const char* platform_name(TargetPlatform platform) noexcept {
    switch (platform) {
        case TargetPlatform::Windows: return "windows-x64";
        case TargetPlatform::Linux: return "linux-x64";
        case TargetPlatform::MacOS: return "macos-universal";
        case TargetPlatform::DedicatedServer: return "dedicated-server";
    }
    return "unknown";
}

std::optional<BuildProfile> profile_from_string(std::string_view value) noexcept {
    std::string lowered;
    for (char c : value) lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowered == "debug") return BuildProfile::Debug;
    if (lowered == "development" || lowered == "dev") return BuildProfile::Development;
    if (lowered == "shipping") return BuildProfile::Shipping;
    if (lowered == "server") return BuildProfile::Server;
    if (lowered == "editor") return BuildProfile::Editor;
    return std::nullopt;
}

std::filesystem::path ProjectConfig::assets_path() const {
    return (projectDirectory / sourceAssets).lexically_normal();
}

std::filesystem::path ProjectConfig::scenes_path() const {
    return (projectDirectory / scenesDirectory).lexically_normal();
}

std::filesystem::path ProjectConfig::intermediate_path() const {
    return (projectDirectory / intermediateDirectory).lexically_normal();
}

std::filesystem::path ProjectConfig::build_path() const {
    return (projectDirectory / buildDirectory / profile_name(activeProfile)).lexically_normal();
}

ProjectConfig::ValidationReport ProjectConfig::validate() const {
    ValidationReport report;
    if (name.empty()) report.errors.emplace_back("Project name must not be empty");
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        report.errors.emplace_back("Project name contains invalid path characters");
    if (projectDirectory.empty())
        report.errors.emplace_back("Project directory is not set");
    else if (!std::filesystem::is_directory(projectDirectory))
        report.warnings.emplace_back("Project directory does not exist yet: " + projectDirectory.string());
    if (enginePath.empty())
        report.errors.emplace_back("Engine path is not set");
    else if (!std::filesystem::is_directory(enginePath))
        report.errors.emplace_back("Engine path does not exist: " + enginePath.string());
    if (initialScene.empty())
        report.warnings.emplace_back("No initial scene configured; builds will produce an empty game");
    if (enabledPlugins.empty())
        report.warnings.emplace_back("No plugins enabled");
    // The initial scene should exist unless we are generating a fresh project.
    if (!projectDirectory.empty() && std::filesystem::is_directory(projectDirectory)) {
        const std::filesystem::path scene = projectDirectory / initialScene;
        if (!std::filesystem::exists(scene))
            report.warnings.emplace_back("Initial scene not found: " + scene.string());
    }
    // Duplicate plugin names.
    std::vector<std::string> sorted = enabledPlugins;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        report.errors.emplace_back("Duplicate plugin entries in enabledPlugins");
    return report;
}

bool ProjectConfig::is_plugin_enabled(std::string_view name) const {
    return std::find(enabledPlugins.begin(), enabledPlugins.end(), std::string(name)) != enabledPlugins.end();
}

bool ProjectConfig::set_plugin_enabled(std::string_view name, bool enabled) {
    const std::string value(name);
    const auto it = std::find(enabledPlugins.begin(), enabledPlugins.end(), value);
    if (enabled && it == enabledPlugins.end()) {
        enabledPlugins.push_back(value);
        return true;
    }
    if (!enabled && it != enabledPlugins.end()) {
        enabledPlugins.erase(it);
        return true;
    }
    return false;
}

bool ProjectConfig::save_to_file() const {
    std::ofstream out(config_file_path(), std::ios::trunc);
    if (!out) return false;
    out << "VCPROJECT 1\n";
    out << "name " << name << "\n";
    out << "version " << version << "\n";
    out << "initialScene " << initialScene << "\n";
    out << "profile " << profile_name(activeProfile) << "\n";
    out << "platform " << platform_name(targetPlatform) << "\n";
    out << "plugins " << enabledPlugins.size() << "\n";
    for (const std::string& plugin : enabledPlugins) out << "  plugin " << plugin << "\n";
    return out.good();
}

bool ProjectConfig::load_from_file() {
    std::ifstream in(config_file_path());
    if (!in) return false;
    std::string magic;
    int version{};
    in >> magic >> version;
    if (magic != "VCPROJECT" || version != 1) return false;
    std::string key;
    while (in >> key) {
        if (key == "name") in >> name;
        else if (key == "version") in >> version;
        else if (key == "initialScene") in >> initialScene;
        else if (key == "profile") {
            std::string value;
            in >> value;
            if (const auto p = profile_from_string(value)) activeProfile = *p;
        } else if (key == "platform") {
            std::string value;
            in >> value;
            if (value == "linux-x64") targetPlatform = TargetPlatform::Linux;
            else if (value == "macos-universal") targetPlatform = TargetPlatform::MacOS;
            else if (value == "dedicated-server") targetPlatform = TargetPlatform::DedicatedServer;
            else targetPlatform = TargetPlatform::Windows;
        } else if (key == "plugins") {
            size_t count{};
            in >> count;
            enabledPlugins.clear();
            for (size_t i = 0; i < count; ++i) {
                std::string tag, plugin;
                in >> tag >> plugin;
                if (tag == "plugin") enabledPlugins.push_back(plugin);
            }
        }
    }
    return true;
}

} // namespace Engine::Tools

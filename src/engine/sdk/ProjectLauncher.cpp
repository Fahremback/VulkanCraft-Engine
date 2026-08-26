// ProjectLauncher.cpp — the ONLY TU with the project-launcher session
// behavior (agente 2 §C l.66). Pure session model: hub mode + active
// project/scene + dirty flag + ordered recents. All mutations are
// all-or-nothing (refused with a reason, session untouched). No
// clocks/RNG/globals. Deterministic JSON.

#include "engine/editor/IProjectLauncher.hpp"

#include <sstream>

namespace engine {
namespace editor {

namespace {

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

class ProjectLauncherImpl final : public IProjectLauncher {
public:
    ProjectLauncherImpl() = default;

    ProjectLauncherSnapshot snapshot() const override {
        return { in_launcher_mode_, project_, scene_, dirty_, recents_ };
    }

    bool add_recent(const std::string& projectPath,
                    std::string& errorOut) override {
        errorOut.clear();
        if (projectPath.empty()) {
            errorOut = "project path must not be empty";
            return false;
        }
        for (size_t i = 0; i < recents_.size(); ++i) {
            if (recents_[i] == projectPath) {
                // Move to front (most recent first).
                recents_.erase(recents_.begin() + static_cast<long>(i));
                recents_.insert(recents_.begin(), projectPath);
                return true;
            }
        }
        recents_.insert(recents_.begin(), projectPath);
        return true;
    }

    bool open_project(const std::string& projectPath,
                      std::string& errorOut) override {
        errorOut.clear();
        if (projectPath.empty()) {
            errorOut = "project path must not be empty";
            return false;
        }
        // The project must be known (recents or previously open).
        bool known = false;
        for (const auto& r : recents_) {
            if (r == projectPath) { known = true; break; }
        }
        if (!known && project_ != projectPath) {
            errorOut = "unknown project (call add_recent first): " + projectPath;
            return false;
        }
        project_ = projectPath;
        scene_.clear();
        dirty_ = false;
        in_launcher_mode_ = false;
        add_recent(projectPath, errorOut);
        return true;
    }

    bool open_scene(const std::string& scenePath,
                    std::string& errorOut) override {
        errorOut.clear();
        if (in_launcher_mode_) {
            errorOut = "no project open (launcher hub is showing)";
            return false;
        }
        if (scenePath.empty()) {
            errorOut = "scene path must not be empty";
            return false;
        }
        if (!scene_.empty()) {
            errorOut = "a scene is already open: " + scene_;
            return false;
        }
        scene_ = scenePath;
        dirty_ = false;
        return true;
    }

    void set_dirty(bool dirty) override {
        if (in_launcher_mode_ || scene_.empty()) return;
        dirty_ = dirty;
    }

    bool close_scene(std::string& errorOut) override {
        errorOut.clear();
        if (in_launcher_mode_) {
            errorOut = "launcher hub is showing (no scene)";
            return false;
        }
        if (scene_.empty()) {
            errorOut = "no scene is open";
            return false;
        }
        scene_.clear();
        dirty_ = false;
        return true;
    }

    void back_to_launcher() override {
        in_launcher_mode_ = true;
        project_.clear();
        scene_.clear();
        dirty_ = false;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"mode\":\""
            << (in_launcher_mode_ ? "launcher" : "editor")
            << "\",\"project\":\"" << json_escape(project_)
            << "\",\"scene\":\"" << json_escape(scene_)
            << "\",\"dirty\":" << (dirty_ ? "true" : "false")
            << ",\"recents\":[";
        for (size_t i = 0; i < recents_.size(); ++i) {
            if (i) out << ",";
            out << "\"" << json_escape(recents_[i]) << "\"";
        }
        out << "]}";
        return out.str();
    }

private:
    bool in_launcher_mode_{ true };
    std::string project_;
    std::string scene_;
    bool dirty_{ false };
    std::vector<std::string> recents_;
};

}  // namespace

std::unique_ptr<IProjectLauncher> create_project_launcher() {
    return std::make_unique<ProjectLauncherImpl>();
}

}  // namespace editor
}  // namespace engine

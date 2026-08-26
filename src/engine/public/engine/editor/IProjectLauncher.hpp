#pragma once

// IProjectLauncher (agente 2 §C l.66): the PUBLIC, deterministic model of
// the "open project / launcher hub" flow — the editor must open a project,
// import assets, edit the scene, run the game and package it without the
// user hand-editing files. This contract owns the SESSION STATE only: which
// project is open, which scene is active, whether it is dirty, whether the
// launcher hub is showing, and the ordered recent-projects list. It is a pure
// state machine:
//   - UNEQUIVOCAL: open_project/open_scene validate every input (path
//     non-empty, project known to the recents list or created); invalid
//     commands are REFUSED with a reason and leave the session untouched
//     (all-or-nothing).
//   - DETERMINISM: no clocks/RNG/globals; same command sequence -> identical
//     state.
//   - OBSERVABLE: to_json() serializes {mode, project, scene, dirty, recents}
//     deterministically (editor exposes it via the Control API, e.g.
//     GET /launcher).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/ProjectLauncher.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

struct ProjectLauncherSnapshot {
    bool in_launcher_mode{ true };    // hub showing (no project open yet)
    std::string project;              // active project path ("" when hub)
    std::string scene;                // active scene path ("" when none)
    bool dirty{ false };              // scene has unsaved edits
    std::vector<std::string> recents; // ordered, most-recent first

    bool operator==(const ProjectLauncherSnapshot& other) const {
        return in_launcher_mode == other.in_launcher_mode &&
               project == other.project && scene == other.scene &&
               dirty == other.dirty && recents == other.recents;
    }
    bool operator!=(const ProjectLauncherSnapshot& other) const {
        return !(*this == other);
    }
};

class IProjectLauncher {
public:
    virtual ~IProjectLauncher() = default;

    virtual ProjectLauncherSnapshot snapshot() const = 0;

    // Registers a project path in the recents list (moves to front when
    // already known; deduplicates). REFUSED on empty path.
    virtual bool add_recent(const std::string& projectPath,
                            std::string& errorOut) = 0;

    // Opens a project: leaves the hub, sets the active project, adds it to
    // recents, clears scene/dirty. REFUSED on empty path (project must be
    // known — add_recent or a prior open created it).
    virtual bool open_project(const std::string& projectPath,
                              std::string& errorOut) = 0;

    // Opens a scene inside the active project. REFUSED when the hub is
    // showing (no project), the path is empty, or a scene is already open.
    virtual bool open_scene(const std::string& scenePath,
                            std::string& errorOut) = 0;

    // Marks the session dirty / clean. open_scene resets dirty to false.
    virtual void set_dirty(bool dirty) = 0;

    // Closes the scene (keeps the project open; back to no scene, clean).
    // REFUSED when the hub is showing or no scene is open.
    virtual bool close_scene(std::string& errorOut) = 0;

    // Returns to the launcher hub (project/scene cleared, dirty cleared).
    virtual void back_to_launcher() = 0;

    // Deterministic JSON of the snapshot (bit-exact).
    virtual std::string to_json() const = 0;
};

// Factory: the SDK adapter is the only TU with behavior.
std::unique_ptr<IProjectLauncher> create_project_launcher();

}  // namespace editor
}  // namespace engine

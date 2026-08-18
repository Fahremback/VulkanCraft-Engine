// TimeTravel.cpp
//
// The only implementation of engine::world::ITimeTravel (SDK, META §19 /
// FALTANTES §15 "branches/estados temporais persistentes para viagem no
// tempo"). Pure composition of the public layers (IWorldManager + the save v5
// it already delegates to) — no backend, headless, deterministic.
//
// Model: a temporal state is a full world snapshot (save v5) persisted to a
// named path and registered on a timeline. A branch forks an existing state by
// copying its snapshot file, so source and branch are independent persistent
// timelines. `travel_to` rewinds the LIVE world to a state: the current live
// state is saved to a rollback path first, the world is unloaded and reloaded
// from the state's snapshot; any failure restores the pre-travel state
// (transactional). Futures are never destroyed — other states/branches keep
// existing and remain travelable.

#include "engine/world/ITimeTravel.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace engine {
namespace world {

namespace {

// Path used to snapshot the live state before a travel, so a failure can
// restore it exactly. Derived from the target state's path (deterministic).
std::string rollback_path(const std::string& statePath) {
    return statePath + ".travel_rollback";
}

bool copy_path(const std::filesystem::path& from,
               const std::filesystem::path& to, std::string& errorOut) {
    std::error_code ec;
    if (std::filesystem::is_directory(from, ec)) {
        std::filesystem::copy(from, to,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::
                                      overwrite_existing,
                              ec);
    } else {
        std::filesystem::copy_file(from, to,
                                   std::filesystem::copy_options::
                                       overwrite_existing,
                                   ec);
    }
    if (ec) {
        errorOut = "time travel: cannot copy snapshot to '" + to.string() +
                   "' (" + ec.message() + ")";
        return false;
    }
    return true;
}

}  // namespace

class TimeTravel final : public ITimeTravel {
public:
    explicit TimeTravel(IWorldManager& manager) : manager_(manager) {}

    bool capture_state(const std::string& name, const std::string& worldName,
                       const std::string& path,
                       std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "time travel: empty state name";
            return false;
        }
        if (states_.count(name) != 0) {
            errorOut = "time travel: duplicate state name '" + name + "'";
            return false;
        }
        if (path.empty()) {
            errorOut = "time travel: empty snapshot path";
            return false;
        }
        if (manager_.world(worldName) == nullptr) {
            errorOut = "time travel: unknown world '" + worldName + "'";
            return false;
        }
        if (!manager_.save_world(worldName, path, errorOut)) {
            return false;  // errorOut carries the save diagnostic
        }
        TimelineStateInfo info;
        info.name = name;
        info.worldName = worldName;
        info.path = path;
        states_[name] = info;
        errorOut.clear();
        return true;
    }

    bool branch_state(const std::string& name, const std::string& fromState,
                      const std::string& path,
                      std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "time travel: empty branch name";
            return false;
        }
        if (states_.count(name) != 0) {
            errorOut = "time travel: duplicate branch name '" + name + "'";
            return false;
        }
        const auto it = states_.find(fromState);
        if (it == states_.end()) {
            errorOut = "time travel: unknown source state '" + fromState + "'";
            return false;
        }
        if (path.empty()) {
            errorOut = "time travel: empty branch snapshot path";
            return false;
        }
        if (!copy_path(it->second.path, path, errorOut)) {
            return false;
        }
        TimelineStateInfo info;
        info.name = name;
        info.worldName = it->second.worldName;
        info.path = path;
        states_[name] = info;
        errorOut.clear();
        return true;
    }

    bool state_exists(const std::string& name) const override {
        return states_.count(name) != 0;
    }

    std::vector<TimelineStateInfo> states() const override {
        std::vector<TimelineStateInfo> out;
        out.reserve(states_.size());
        for (const auto& [name, info] : states_) {
            (void)name;
            out.push_back(info);
        }
        return out;
    }

    TimelineStateInfo state(const std::string& name) const override {
        const auto it = states_.find(name);
        return it == states_.end() ? TimelineStateInfo{} : it->second;
    }

    bool travel_to(const std::string& name,
                   std::string& errorOut) override {
        const auto it = states_.find(name);
        if (it == states_.end()) {
            errorOut = "time travel: unknown state '" + name + "'";
            return false;
        }
        const TimelineStateInfo& target = it->second;
        if (manager_.world(target.worldName) == nullptr) {
            errorOut = "time travel: world '" + target.worldName +
                       "' is not loaded";
            return false;
        }

        // Phase 1: snapshot the LIVE state to the rollback path. The manager's
        // save_world keeps the world untouched, so this cannot fail midway.
        const std::string rollback = rollback_path(target.path);
        if (!manager_.save_world(target.worldName, rollback, errorOut)) {
            return false;
        }

        // Phase 2: preserve the world's identity (seed/rules) for the reload.
        const WorldInfo liveInfo = manager_.world_info(target.worldName);
        WorldSpec spec;
        spec.name = target.worldName;
        spec.seed = liveInfo.seed;
        spec.rulesJson = liveInfo.rulesJson;
        spec.savePath = target.path;

        // Phase 3: unload + reload from the state's snapshot.
        if (!manager_.unload_world(target.worldName)) {
            std::string ignore;
            manager_.load_world(WorldSpec{ spec.name, spec.seed,
                                           spec.rulesJson, rollback },
                                ignore);
            errorOut = "time travel: failed to unload world '" +
                       target.worldName + "' (restored from rollback)";
            return false;
        }
        if (!manager_.load_world(spec, errorOut)) {
            // Transactional rollback: restore the pre-travel live state.
            std::string ignore;
            if (!manager_.load_world(WorldSpec{ spec.name, spec.seed,
                                                spec.rulesJson, rollback },
                                     ignore)) {
                errorOut = "time travel: travel to '" + name +
                           "' failed AND rollback failed (" + errorOut + "; " +
                           ignore + ")";
                return false;
            }
            errorOut = "time travel: travel to '" + name +
                       "' failed; world restored from rollback (" + errorOut +
                       ")";
            return false;
        }

        // Phase 4: success — drop the rollback snapshot (best-effort).
        std::error_code ec;
        std::filesystem::remove_all(rollback, ec);
        errorOut.clear();
        return true;
    }

private:
    IWorldManager& manager_;
    std::map<std::string, TimelineStateInfo> states_;  // sorted by name
};

std::unique_ptr<ITimeTravel> create_time_travel(IWorldManager& manager) {
    return std::make_unique<TimeTravel>(manager);
}

}  // namespace world
}  // namespace engine

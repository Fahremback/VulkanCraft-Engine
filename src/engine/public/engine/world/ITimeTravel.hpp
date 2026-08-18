#pragma once

// Public time-travel contract (SDK, META §19 / FALTANTES §15 item "branches/
// estados temporais persistentes para viagem no tempo"). Time travel is built
// on the SAME foundation as everything else in this engine: the per-world save
// v5. A TEMPORAL STATE is a full snapshot of one world (blocks + entities +
// components) persisted to a named path and registered on a timeline; a BRANCH
// is a state forked from another state (its snapshot file is copied, so the
// branch and its source are independent persistent timelines).
//
// `travel_to` REWINDS the live world to a captured state — the current world
// is unloaded and reloaded from the state's snapshot. Consequences are
// PERSISTENT: states and branches never disappear, so the "future" keeps
// existing as a branch you can travel back to (a divergent multiverse, not a
// single undoable line). The travel is transactional: the live state is saved
// to a rollback path first, and any failure restores it exactly.
//
// This header is self-contained; the only implementation lives in
// src/engine/sdk/TimeTravel.cpp.

#include "engine/world/IWorldManager.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace world {

// Metadata of one registered temporal state / branch.
struct TimelineStateInfo {
    std::string name;       // unique timeline entry name
    std::string worldName;  // the world it snapshots
    std::string path;       // persisted snapshot path (save v5)
};

class ITimeTravel {
public:
    virtual ~ITimeTravel() = default;

    // Captures the FULL current state of a world (save v5 — blocks, block
    // entities, fluids/light, world entities with components and stable ids)
    // to `path` and registers it on the timeline as `name`. All-or-nothing:
    // duplicate name, empty path, unknown world, or a failed save are refused
    // without registering.
    virtual bool capture_state(const std::string& name,
                               const std::string& worldName,
                               const std::string& path,
                               std::string& errorOut) = 0;

    // Forks an existing state into a NEW independent timeline entry: the
    // source snapshot file is copied to `path` (a directory is copied
    // recursively) and registered as `name`. Both the source and the branch
    // keep existing — traveling to either one later rewinds the world to that
    // divergence point. All-or-nothing: duplicate name, unknown source state,
    // empty path, or a failed copy are refused.
    virtual bool branch_state(const std::string& name,
                              const std::string& fromState,
                              const std::string& path,
                              std::string& errorOut) = 0;

    virtual bool state_exists(const std::string& name) const = 0;
    virtual std::vector<TimelineStateInfo> states() const = 0;
    // Metadata of one entry; empty fields when unknown.
    virtual TimelineStateInfo state(const std::string& name) const = 0;

    // TIME TRAVEL: rewinds the LIVE world to the captured state. The current
    // live state is first saved to a rollback path; then the world is
    // unloaded and reloaded from the state's snapshot. Transactional: any
    // failure restores the pre-travel state (via the rollback snapshot) and
    // returns false with a diagnostic. The "future" is never destroyed —
    // other states/branches continue to exist and can be traveled to.
    virtual bool travel_to(const std::string& name,
                           std::string& errorOut) = 0;
};

// The only implementation of ITimeTravel (src/engine/sdk/TimeTravel.cpp).
std::unique_ptr<ITimeTravel> create_time_travel(IWorldManager& manager);

}  // namespace world
}  // namespace engine

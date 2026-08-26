#pragma once

// ISimulationFarm — FALTANTES differential "HeadlessSimulationFarm": run
// THOUSANDS of seeds/profiles headlessly to find SOFTLOCKS and REPETITION
// (META §32 — engine-own code; nothing in external/solutions resolves it).
//
// A headless farm sweeps a deterministic grid of (seed, profile) TRIALS
// through a PURE simulation driver and reports, bit-exactly, which trials
// got stuck (SOFTLOCK: progress stalled for a consecutive window) and which
// fell into a loop (REPEAT: a state re-occurred — the simulation would cycle
// forever). The same config + the same driver reproduce a BIT-IDENTICAL
// report on every machine and every run.
//
// The farm is driver-agnostic: the caller registers the pure `SimAdvance`
// step function (a deterministic state transition), so the engine never
// hardcodes a simulation kind. The natural composition (and the gate's
// proof) is EpisodeCompiler content: an episode is COMPILED and PUBLISHED
// (validated/simulated/tested/signed), and the farm then drives each entry
// headlessly across thousands of seeds — softlocks and repetition are found
// BEFORE anything interactive ever runs.
//
// Detection semantics (deterministic):
//   - SOFTLOCK — the progress oracle reports NO progress for
//     `maxStallSteps` CONSECUTIVE steps. The default oracle: the state must
//     change between steps (consecutive identical states = stall). A caller
//     may register a stricter oracle (e.g. an oscillation A<->B is "no
//     progress" even though the raw state changes).
//   - REPEAT — `detectCycles` keeps a bounded memory of recent state hashes
//     (FIFO, `cycleMemory` entries, deterministic eviction of the oldest);
//     a re-occurring state hash is a cycle -> REPEAT at that step. The
//     memory is bounded ON PURPOSE (documented): a cycle longer than the
//     memory is not flagged (it looks like progress).
//   - A driver ERROR (non-empty diagnostic from `advance`) refuses the
//     WHOLE run all-or-nothing: a broken driver is a broken farm, never a
//     simulation finding.
//
// The sweep order is fixed: seeds ascending in [seedStart, seedEnd), then
// profiles in config order — results land in the report in that exact order.
// `maxTrials` caps the total (0 = unlimited). All refusals are all-or-nothing
// with a diagnostic and the report untouched.
//
// PURE and DETERMINISTIC — the farm never touches a concrete world; it only
// runs the registered pure driver and assembles the report. Self-contained
// (std).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace simfarm {

// One trial's outcome.
enum class FarmOutcome : std::uint8_t {
    Ok = 0,        // ran the full step budget without a finding
    Softlock = 1,  // progress stalled for the consecutive window
    Repeat = 2     // a state re-occurred within the cycle memory
};

// One (seed, profile) trial result. Deterministic fields.
struct FarmTrialResult {
    std::uint64_t seed{ 0 };
    std::string profile;  // "" = the default profile
    FarmOutcome outcome{ FarmOutcome::Ok };
    std::uint64_t stepsRun{ 0 };   // steps actually executed
    std::uint64_t stoppedAt{ 0 };  // step where the finding fired (0 = n/a)
    std::string stateHash;         // final (or repeating) deterministic state hash
};

// The deterministic aggregate of a sweep.
struct FarmReport {
    std::uint64_t trialsRun{ 0 };
    std::uint64_t softlocks{ 0 };
    std::uint64_t repeats{ 0 };
    // In sweep order (seed asc, profile asc). Deterministic.
    std::vector<FarmTrialResult> results;
};

// The PURE simulation driver. `advance` evolves the opaque serialized state
// one deterministic step; the same (state, seed, profile, step) always
// yields the same next state. A non-empty `errorOut` is a driver error and
// refuses the WHOLE run (all-or-nothing) — it is never a softlock.
using SimAdvance = std::function<std::string(
    const std::string& state, std::uint64_t seed, const std::string& profile,
    std::uint64_t step, std::string& errorOut)>;

// Progress oracle: true while the trial is still making progress. Default:
// the state must differ from the previous state (consecutive identical
// states = stall). Must be PURE and deterministic.
using ProgressFn =
    std::function<bool(const std::string& state, const std::string& prevState)>;

// The deterministic sweep configuration. Validated all-or-nothing.
struct FarmConfig {
    std::uint64_t maxStepsPerTrial{ 4096 };  // step budget per trial (> 0)
    std::uint64_t maxStallSteps{ 16 };       // consecutive no-progress => SOFTLOCK (> 0)
    std::uint64_t maxTrials{ 0 };            // 0 = unlimited (all seeds x profiles)
    bool detectCycles{ true };               // state-hash repetition => REPEAT
    std::uint64_t cycleMemory{ 4096 };       // FIFO recent-state memory (>= 2 when cycles on)
    std::uint64_t seedStart{ 0 };            // sweep range [seedStart, seedEnd)
    std::uint64_t seedEnd{ 256 };            // must be > seedStart
    std::vector<std::string> profiles{ "" }; // profiles to sweep (non-empty; "" = default)
};

class ISimulationFarm {
public:
    virtual ~ISimulationFarm() = default;

    // Registers the pure simulation driver (REQUIRED). Refuses a null hook.
    virtual bool set_driver(SimAdvance advance, std::string& errorOut) = 0;

    // Optional progress oracle. Refuses a null hook; the default (state must
    // change between steps) applies until one is registered.
    virtual bool set_progress(ProgressFn progress, std::string& errorOut) = 0;

    // Runs the deterministic sweep. `report` is cleared at entry and filled
    // on success. All-or-nothing refusals (null driver, empty profiles,
    // seedEnd <= seedStart, maxStepsPerTrial 0, maxStallSteps 0,
    // cycleMemory < 2 with cycles enabled) refuse BEFORE running — the
    // report stays empty. A driver error mid-run also refuses the whole run.
    // The same config + the same hooks reproduce a bit-identical report.
    virtual bool run(const FarmConfig& config, FarmReport& report,
                     std::string& errorOut) = 0;

    // Deterministic canonical JSON summary of a report (counts + results in
    // sweep order) — the machine surface for surfacing farm findings.
    virtual std::string report_to_json(const FarmReport& report) const = 0;
};

// The only implementation (src/engine/sdk/SimulationFarm.cpp).
std::unique_ptr<ISimulationFarm> create_simulation_farm();

}  // namespace simfarm
}  // namespace engine

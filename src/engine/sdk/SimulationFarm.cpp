// SimulationFarm.cpp — SDK adapter for the public ISimulationFarm contract
// (FALTANTES differential: run thousands of seeds/profiles headlessly to find
// softlocks and repetition — META §32). Single TU, pure, deterministic,
// all-or-nothing refusals. Detection semantics:
//   - SOFTLOCK: the progress oracle reports no progress for maxStallSteps
//     CONSECUTIVE steps (default oracle: the state must change);
//   - REPEAT: only on PROGRESSING steps — a re-occurring state hash within
//     the bounded FIFO memory is a cycle; a stalled run never hits the cycle
//     check, so a fixed state is a SOFTLOCK and an oscillation is a REPEAT;
//   - driver error: refuses the WHOLE run (a broken driver is not a finding).
// The sweep is deterministic: seeds ascending x profiles in config order, and
// the same config + driver reproduce a bit-identical report.
#include "engine/simfarm/ISimulationFarm.hpp"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine {
namespace simfarm {
namespace {

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Deterministic content digest (splitmix64 over the bytes; 16 hex chars) —
// the state hash and the default "progress" ground truth.
std::string state_digest(const std::string& data) {
    std::uint64_t h = 0xE9E3779B97F4A7C1ULL;
    for (const char c : data) {
        h = splitmix64(h ^ static_cast<std::uint64_t>(
                               static_cast<unsigned char>(c)));
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buffer);
}

bool default_progress(const std::string& state, const std::string& prevState) {
    return state != prevState;
}

std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string outcome_name(FarmOutcome outcome) {
    switch (outcome) {
        case FarmOutcome::Ok:
            return "ok";
        case FarmOutcome::Softlock:
            return "softlock";
        case FarmOutcome::Repeat:
            return "repeat";
    }
    return "ok";
}

class SimulationFarm final : public ISimulationFarm {
public:
    bool set_driver(SimAdvance advance, std::string& errorOut) override {
        if (!advance) {
            errorOut = "driver must not be null";
            return false;
        }
        driver_ = std::move(advance);
        return true;
    }

    bool set_progress(ProgressFn progress, std::string& errorOut) override {
        if (!progress) {
            errorOut = "progress oracle must not be null";
            return false;
        }
        progress_ = std::move(progress);
        return true;
    }

    bool run(const FarmConfig& config, FarmReport& report,
             std::string& errorOut) override {
        report = FarmReport{};
        // The sweep is independent of the caller's prior diagnostic: a
        // stale non-empty error must never poison a good run.
        errorOut.clear();
        if (!driver_) {
            errorOut = "no simulation driver registered";
            return false;
        }
        if (config.profiles.empty()) {
            errorOut = "profiles must be non-empty";
            return false;
        }
        if (config.seedEnd <= config.seedStart) {
            errorOut = "seedEnd must be greater than seedStart";
            return false;
        }
        if (config.maxStepsPerTrial == 0) {
            errorOut = "maxStepsPerTrial must be > 0";
            return false;
        }
        if (config.maxStallSteps == 0) {
            errorOut = "maxStallSteps must be > 0";
            return false;
        }
        if (config.detectCycles && config.cycleMemory < 2) {
            errorOut = "cycleMemory must be >= 2 when cycle detection is enabled";
            return false;
        }

        for (std::uint64_t seed = config.seedStart; seed < config.seedEnd;
             ++seed) {
            for (const std::string& profile : config.profiles) {
                if (config.maxTrials != 0 &&
                    report.trialsRun >= config.maxTrials) {
                    return true;
                }
                FarmTrialResult result =
                    run_trial(config, seed, profile, errorOut);
                if (errorOut.empty()) {
                    report.results.push_back(std::move(result));
                    ++report.trialsRun;
                    if (result.outcome == FarmOutcome::Softlock) {
                        ++report.softlocks;
                    } else if (result.outcome == FarmOutcome::Repeat) {
                        ++report.repeats;
                    }
                } else {
                    // Driver error: all-or-nothing — refuse the whole run.
                    report = FarmReport{};
                    return false;
                }
            }
        }
        return true;
    }

    std::string report_to_json(const FarmReport& report) const override {
        std::string out = "{\"trialsRun\":";
        out += uint64_str(report.trialsRun);
        out += ",\"softlocks\":";
        out += uint64_str(report.softlocks);
        out += ",\"repeats\":";
        out += uint64_str(report.repeats);
        out += ",\"results\":[";
        for (std::size_t i = 0; i < report.results.size(); ++i) {
            if (i != 0) out += ",";
            const FarmTrialResult& result = report.results[i];
            out += "{\"seed\":";
            out += uint64_str(result.seed);
            out += ",\"profile\":\"";
            out += escape_json(result.profile);
            out += "\",\"outcome\":\"";
            out += outcome_name(result.outcome);
            out += "\",\"stepsRun\":";
            out += uint64_str(result.stepsRun);
            out += ",\"stoppedAt\":";
            out += uint64_str(result.stoppedAt);
            out += ",\"stateHash\":\"";
            out += result.stateHash;
            out += "\"}";
        }
        out += "]}";
        return out;
    }

private:
    std::string uint64_str(std::uint64_t value) const {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%llu",
                      static_cast<unsigned long long>(value));
        return std::string(buffer);
    }

    // Runs one (seed, profile) trial against the budget. On a driver error
    // `errorOut` is set (the caller refuses the whole run); otherwise the
    // result is deterministic.
    FarmTrialResult run_trial(const FarmConfig& config, std::uint64_t seed,
                              const std::string& profile,
                              std::string& errorOut) {
        FarmTrialResult result;
        result.seed = seed;
        result.profile = profile;

        std::string state;
        std::uint64_t stall = 0;
        std::deque<std::string> order;              // FIFO of recent hashes
        std::unordered_set<std::string> seen;       // membership (lookup only)

        for (std::uint64_t step = 0; step < config.maxStepsPerTrial; ++step) {
            std::string hookError;
            const std::string next =
                driver_(state, seed, profile, step, hookError);
            if (!hookError.empty()) {
                errorOut = "driver error at seed " + uint64_str(seed) +
                           " profile '" + profile + "' step " +
                           uint64_str(step) + ": " + hookError;
                return result;
            }
            if (!progress_(next, state)) {
                ++stall;
                if (stall >= config.maxStallSteps) {
                    result.outcome = FarmOutcome::Softlock;
                    result.stepsRun = step + 1;
                    result.stoppedAt = step;
                    result.stateHash = state_digest(next);
                    return result;
                }
            } else {
                stall = 0;
                if (config.detectCycles) {
                    const std::string hash = state_digest(next);
                    if (seen.find(hash) != seen.end()) {
                        result.outcome = FarmOutcome::Repeat;
                        result.stepsRun = step + 1;
                        result.stoppedAt = step;
                        result.stateHash = hash;
                        return result;
                    }
                    if (order.size() >= config.cycleMemory) {
                        seen.erase(order.front());
                        order.pop_front();
                    }
                    order.push_back(hash);
                    seen.insert(hash);
                }
            }
            state = next;
        }

        result.outcome = FarmOutcome::Ok;
        result.stepsRun = config.maxStepsPerTrial;
        result.stoppedAt = 0;
        result.stateHash = state_digest(state);
        return result;
    }

    SimAdvance driver_;
    ProgressFn progress_{ default_progress };
};

}  // namespace

std::unique_ptr<ISimulationFarm> create_simulation_farm() {
    return std::unique_ptr<ISimulationFarm>(new SimulationFarm());
}

}  // namespace simfarm
}  // namespace engine

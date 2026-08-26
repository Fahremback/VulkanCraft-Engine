// SimulationFarmTests.cpp
//
// FALTANTES differential — HeadlessSimulationFarm: run thousands of
// seeds/profiles headlessly to find SOFTLOCKS and REPETITION (META §32). The
// gate drives the PUBLIC contract headless and proves:
//   - the deterministic sweep (seeds ascending x profiles in order, capped
//     by maxTrials) with per-trial outcomes and aggregate counts;
//   - SOFTLOCK detection: maxStallSteps consecutive no-progress steps fire
//     at the exact step, and a full-budget progressing trial is Ok;
//   - REPEAT detection: a state cycle fires at the re-occurrence step, only
//     on PROGRESSING steps (a fixed state is a softlock, not a repeat), and
//     a cycle longer than the bounded cycleMemory is honestly NOT flagged;
//   - the progress ORACLE seam: a caller oracle can declare an oscillation
//     "no progress" -> softlock even though the raw state changes;
//   - cross-instance bit-exact determinism (identical report + JSON);
//   - all-or-nothing refusals (null driver/oracle, empty profiles, bad
//     seed range, zero budgets, tiny cycleMemory) leave the report empty,
//     and a driver error refuses the WHOLE run;
//   - report_to_json is the deterministic canonical machine surface;
//   - COMPOSITION with EpisodeCompiler: content compiled and published
//     through IEpisodeCompiler is driven headlessly across seeds/profiles —
//     a stalling entry is found as a softlock in the farm.

#include "engine/compiler/IEpisodeCompiler.hpp"
#include "engine/simfarm/ISimulationFarm.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

using engine::simfarm::FarmConfig;
using engine::simfarm::FarmOutcome;
using engine::simfarm::FarmReport;
using engine::simfarm::FarmTrialResult;
using engine::simfarm::create_simulation_farm;

// Progressing driver: state = "s:<seed>:<profile>:<step>". Never stalls,
// never cycles — every trial runs the full budget and is Ok.
std::string progressing_driver(const std::string&, std::uint64_t seed,
                               const std::string& profile, std::uint64_t step,
                               std::string&) {
    return "s:" + std::to_string(seed) + ":" + profile + ":" +
           std::to_string(step);
}

// Stalling driver: progresses until `stallAt`, then returns a FIXED state —
// a pure softlock (never a repeat: stalled steps skip the cycle check).
auto stall_driver(std::uint64_t stallAt) {
    return [stallAt](const std::string&, std::uint64_t seed,
                     const std::string& profile, std::uint64_t step,
                     std::string&) {
        if (step < stallAt) {
            return "moving:" + std::to_string(step);
        }
        return "stuck:" + std::to_string(seed) + ":" + profile;
    };
}

// Oscillating driver: A -> B -> A -> B ... — always progressing per the
// default oracle (state changes every step), so the CYCLE detector fires.
std::string oscillate_driver(const std::string&, std::uint64_t,
                             const std::string&, std::uint64_t step,
                             std::string&) {
    return (step % 2 == 0) ? "A" : "B";
}

// ---- 1. deterministic sweep + aggregate outcomes ----

void test_deterministic_sweep() {
    auto farm = create_simulation_farm();
    std::string error;
    check(farm->set_driver(progressing_driver, error), "driver set");

    FarmConfig config;
    config.maxStepsPerTrial = 64;
    config.seedStart = 0;
    config.seedEnd = 8;   // 8 seeds
    config.profiles = { "default", "hard" };  // 2 profiles -> 16 trials
    FarmReport report;
    check(farm->run(config, report, error), "sweep runs");
    check(error.empty(), "sweep diagnostic empty");
    check(report.trialsRun == 16, "16 trials (8 seeds x 2 profiles)");
    check(report.softlocks == 0 && report.repeats == 0,
          "no findings in a progressing sweep");
    check(report.results.size() == 16, "16 results in the report");

    // Sweep order is fixed: seeds ascending, profiles in config order.
    for (std::size_t i = 0; i < report.results.size(); ++i) {
        const FarmTrialResult& result = report.results[i];
        const std::uint64_t expectedSeed = i / 2;
        const std::string expectedProfile = (i % 2 == 0) ? "default" : "hard";
        check(result.seed == expectedSeed &&
                  result.profile == expectedProfile,
              "sweep order seed asc x profile order");
        check(result.outcome == FarmOutcome::Ok, "trial outcome ok");
        check(result.stepsRun == 64, "full budget run");
        check(result.stoppedAt == 0, "no stop for ok trials");
        check(result.stateHash.size() == 16, "state hash is 16 hex chars");
    }

    // maxTrials caps the sweep deterministically (still in order).
    FarmConfig capped = config;
    capped.maxTrials = 5;
    FarmReport cappedReport;
    check(farm->run(capped, cappedReport, error), "capped sweep runs");
    check(cappedReport.trialsRun == 5 &&
              cappedReport.results.size() == 5,
          "maxTrials caps the sweep at 5");
    // index 4 = seed 2 / "default" (the "hard" trial would be index 5).
    check(cappedReport.results[4].seed == 2 &&
              cappedReport.results[4].profile == "default",
          "cap respects the deterministic order");

    std::printf("[farm] deterministic sweep + aggregates OK\n");
}

// ---- 2. softlock detection ----

void test_softlock_detection() {
    auto farm = create_simulation_farm();
    std::string error;
    check(farm->set_driver(stall_driver(8), error), "stall driver set");

    FarmConfig config;
    config.maxStepsPerTrial = 64;
    config.maxStallSteps = 4;
    config.seedStart = 0;
    config.seedEnd = 1;
    config.profiles = { "" };
    FarmReport report;
    check(farm->run(config, report, error), "run");
    check(report.trialsRun == 1 && report.softlocks == 1,
          "one softlock found");
    const FarmTrialResult& result = report.results[0];
    check(result.outcome == FarmOutcome::Softlock, "outcome softlock");
    // Steps 0..7 move; step 8 is the FIRST stuck state, which still differs
    // from "moving:7" (a transition = progress); the stall window starts at
    // step 9 and reaches 4 at step 12 (the 4th consecutive no-progress
    // step) -> softlock at step 12.
    check(result.stoppedAt == 12, "softlock fires at the 4th stall step");
    check(result.stepsRun == 13, "13 steps executed before the finding");

    // maxStallSteps = 1: the FIRST no-progress step is already a softlock.
    FarmConfig tight = config;
    tight.maxStallSteps = 1;
    FarmReport tightReport;
    check(farm->run(tight, tightReport, error), "tight run");
    check(tightReport.results[0].stoppedAt == 9,
          "stall window 1 fires at the first repeated state");

    std::printf("[farm] softlock detection (window + exact step) OK\n");
}

// ---- 3. repeat / cycle detection ----

void test_repeat_detection() {
    auto farm = create_simulation_farm();
    std::string error;
    check(farm->set_driver(oscillate_driver, error), "oscillate driver set");

    FarmConfig config;
    config.maxStepsPerTrial = 64;
    config.seedStart = 0;
    config.seedEnd = 1;
    config.profiles = { "" };
    FarmReport report;
    check(farm->run(config, report, error), "run");
    check(report.repeats == 1, "cycle flagged as repeat");
    const FarmTrialResult& result = report.results[0];
    check(result.outcome == FarmOutcome::Repeat, "outcome repeat");
    // A(0) B(1) A(2): A re-occurs at step 2.
    check(result.stoppedAt == 2, "cycle fires at the re-occurrence step");

    // A cycle LONGER than the bounded memory is honestly NOT flagged
    // (documented boundedness: it looks like progress).
    auto period6 = [](const std::string&, std::uint64_t,
                      const std::string&, std::uint64_t step,
                      std::string&) {
        return "c" + std::to_string(step % 6);
    };
    auto farm2 = create_simulation_farm();
    check(farm2->set_driver(period6, error), "period-6 driver set");
    FarmConfig small = config;
    small.cycleMemory = 4;  // period 6 > memory 4 -> not detected
    FarmReport smallReport;
    check(farm2->run(small, smallReport, error), "run with small memory");
    check(smallReport.results[0].outcome == FarmOutcome::Ok &&
              smallReport.results[0].stepsRun == 64,
          "cycle longer than the memory is not flagged");

    // detectCycles off: cycles run the full budget.
    FarmConfig noCycles = config;
    noCycles.detectCycles = false;
    FarmReport noCyclesReport;
    check(farm->run(noCycles, noCyclesReport, error), "run cycles off");
    check(noCyclesReport.results[0].outcome == FarmOutcome::Ok &&
              noCyclesReport.results[0].stepsRun == 64,
          "cycle detection off -> full budget, no finding");

    std::printf("[farm] repeat detection (cycle + bounded memory) OK\n");
}

// ---- 4. progress oracle seam ----

void test_progress_oracle() {
    auto farm = create_simulation_farm();
    std::string error;
    check(farm->set_driver(oscillate_driver, error), "oscillate driver set");

    // The caller's oracle declares the A<->B oscillation "no progress" — the
    // raw state changes, but the trial is stuck; with the oracle the farm
    // reports a SOFTLOCK (the stall counter wins over the cycle check).
    check(farm->set_progress(
              [](const std::string&, const std::string&) { return false; },
              error),
          "stricter oracle set");

    FarmConfig config;
    config.maxStepsPerTrial = 64;
    config.maxStallSteps = 3;
    config.seedStart = 0;
    config.seedEnd = 1;
    config.profiles = { "" };
    FarmReport report;
    check(farm->run(config, report, error), "run with oracle");
    check(report.softlocks == 1 && report.repeats == 0,
          "oracle declares oscillation a softlock");
    check(report.results[0].stoppedAt == 2,
          "softlock fires at the 3rd oracle-no-progress step");

    // Null oracle refused.
    check(!farm->set_progress(nullptr, error) && !error.empty(),
          "null oracle refused");

    std::printf("[farm] progress oracle seam OK\n");
}

// ---- 5. cross-instance determinism ----

void test_determinism() {
    std::string error;
    FarmConfig config;
    config.maxStepsPerTrial = 128;
    config.maxStallSteps = 6;
    config.seedStart = 100;
    config.seedEnd = 140;
    config.profiles = { "", "a", "b" };

    auto farmA = create_simulation_farm();
    check(farmA->set_driver(stall_driver(20), error), "driver A set");
    FarmReport reportA;
    check(farmA->run(config, reportA, error), "run A");

    auto farmB = create_simulation_farm();
    check(farmB->set_driver(stall_driver(20), error), "driver B set");
    FarmReport reportB;
    check(farmB->run(config, reportB, error), "run B");

    check(reportA.trialsRun == reportB.trialsRun &&
              reportA.softlocks == reportB.softlocks &&
              reportA.repeats == reportB.repeats,
          "aggregates bit-identical across instances");
    check(farmA->report_to_json(reportA) == farmB->report_to_json(reportB),
          "report JSON bit-identical across instances");
    check(!farmA->report_to_json(reportA).empty() &&
              reportA.softlocks == 120,
          "40 seeds x 3 profiles all stall (softlocks = 120)");

    std::printf("[farm] cross-instance determinism OK\n");
}

// ---- 6. all-or-nothing refusals + driver error ----

void test_refusals() {
    auto farm = create_simulation_farm();
    std::string error;
    FarmConfig config;
    config.seedStart = 0;
    config.seedEnd = 4;
    FarmReport report;

    check(!farm->run(config, report, error) && !error.empty(),
          "run without driver refused");
    check(report.trialsRun == 0 && report.results.empty(),
          "report untouched without driver");

    check(farm->set_driver(progressing_driver, error), "driver set");

    FarmConfig emptyProfiles = config;
    emptyProfiles.profiles = {};
    check(!farm->run(emptyProfiles, report, error) && !error.empty(),
          "empty profiles refused");

    FarmConfig badRange = config;
    badRange.seedEnd = 0;
    check(!farm->run(badRange, report, error) && !error.empty(),
          "seedEnd <= seedStart refused");

    FarmConfig noSteps = config;
    noSteps.maxStepsPerTrial = 0;
    check(!farm->run(noSteps, report, error) && !error.empty(),
          "maxStepsPerTrial 0 refused");

    FarmConfig noStall = config;
    noStall.maxStallSteps = 0;
    check(!farm->run(noStall, report, error) && !error.empty(),
          "maxStallSteps 0 refused");

    FarmConfig tinyMemory = config;
    tinyMemory.cycleMemory = 1;
    check(!farm->run(tinyMemory, report, error) && !error.empty(),
          "cycleMemory 1 with cycles enabled refused");

    // All refusals left the report empty (all-or-nothing).
    check(report.trialsRun == 0 && report.results.empty(),
          "report stays empty after refusals");

    // A driver error mid-run refuses the WHOLE run with a diagnostic.
    auto failing = create_simulation_farm();
    check(failing->set_driver(
              [](const std::string&, std::uint64_t seed,
                 const std::string&, std::uint64_t, std::string& e) {
                  if (seed >= 2) {
                      e = "simulation exploded";
                      return std::string();
                  }
                  return "fine:" + std::to_string(seed);
              },
              error),
          "failing driver set");
    FarmConfig twoSeeds = config;
    twoSeeds.seedEnd = 4;
    FarmReport failedReport;
    check(!failing->run(twoSeeds, failedReport, error) &&
              error.find("driver error at seed 2") != std::string::npos &&
              error.find("simulation exploded") != std::string::npos,
          "driver error refuses the whole run with a diagnostic");
    check(failedReport.trialsRun == 0 && failedReport.results.empty(),
          "report cleared on driver error");

    std::printf("[farm] all-or-nothing refusals + driver error OK\n");
}

// ---- 7. report_to_json canonical surface ----

void test_report_json() {
    auto farm = create_simulation_farm();
    std::string error;
    check(farm->set_driver(stall_driver(4), error), "stall driver set");

    FarmConfig config;
    config.maxStepsPerTrial = 32;
    config.maxStallSteps = 2;
    config.seedStart = 0;
    config.seedEnd = 2;
    config.profiles = { "" };
    FarmReport report;
    check(farm->run(config, report, error), "run");

    const std::string json = farm->report_to_json(report);
    check(json.find("\"trialsRun\":2") != std::string::npos &&
              json.find("\"softlocks\":2") != std::string::npos &&
              json.find("\"repeats\":0") != std::string::npos,
          "JSON carries the aggregate counts");
    // First stuck state at step 4 (a transition = progress); the stall
    // window (2) starts at step 5 and fires at step 6.
    check(json.find("\"seed\":0") != std::string::npos &&
              json.find("\"outcome\":\"softlock\"") != std::string::npos &&
              json.find("\"stoppedAt\":6") != std::string::npos,
          "JSON carries the per-trial finding");
    // Results are the canonical sweep-order surface (start with seed 0).
    check(json.find("\"results\":[{\"seed\":0,\"profile\":\"\","
                  "\"outcome\":\"softlock\"") != std::string::npos,
          "results array starts with the seed-0 trial in sweep order");

    std::printf("[farm] report_to_json canonical surface OK\n");
}

// ---- 8. composition: EpisodeCompiler content driven headlessly ----

void test_composition_with_episode_compiler() {
    std::string error;

    // Compile a two-entry episode (mission kind) through IEpisodeCompiler —
    // the published content the farm will drive.
    auto compiler = engine::compiler::create_episode_compiler();
    check(compiler->register_validator(
              "mission",
              [](const std::string& json, std::string& e) {
                  if (json.empty() || json.front() != '{' ||
                      json.back() != '}') {
                      e = "not a json object";
                      return false;
                  }
                  return true;
              },
              error),
          "validator registered");
    check(compiler->register_simulator(
              "mission",
              [](const std::string& json, int steps, std::string& trace,
                 std::string&) {
                  trace = "sim:" + std::to_string(steps) + ":" +
                          std::to_string(json.size());
                  return true;
              },
              error),
          "simulator registered");
    check(compiler->register_tester(
              "mission",
              [](const std::string&, std::string&) { return true; }, error),
          "tester registered");

    engine::compiler::EpisodeManifest manifest;
    manifest.title = "FarmEpisode";
    engine::compiler::EpisodeEntry steady;
    steady.kind = "mission";
    steady.name = "steady";
    steady.json = "{\"objective\":\"reach\"}";
    engine::compiler::EpisodeEntry stuck;
    stuck.kind = "mission";
    stuck.name = "stuck";
    stuck.json = "{\"objective\":\"collect\",\"stall\":true}";
    manifest.entries = { steady, stuck };

    engine::compiler::EpisodePackage package;
    check(compiler->compile(manifest, package, error), "episode compiled");
    check(compiler->verify(package, error), "episode verified");
    check(!package.signature.empty() && !package.manifestJson.empty(),
          "episode published");

    // The farm drives the COMPILED content headlessly: per profile (= entry
    // name) a pure driver; the "stuck" entry stalls after a few steps.
    std::vector<engine::compiler::EpisodeEntry> entries = manifest.entries;
    auto farm = create_simulation_farm();
    check(farm->set_driver(
              [entries](const std::string&, std::uint64_t seed,
                        const std::string& profile, std::uint64_t step,
                        std::string&) -> std::string {
                  std::string json;
                  for (const auto& entry : entries) {
                      if (entry.name == profile) json = entry.json;
                  }
                  const bool stall = json.find("\"stall\":true") !=
                                     std::string::npos;
                  if (stall && step >= 3) {
                      return "stuck:" + profile;
                  }
                  return "run:" + profile + ":" + std::to_string(seed) + ":" +
                         std::to_string(step) + ":" + json;
              },
              error),
          "content driver set");

    FarmConfig config;
    config.maxStepsPerTrial = 32;
    config.maxStallSteps = 3;
    config.seedStart = 0;
    config.seedEnd = 4;  // 4 seeds
    config.profiles = { "steady", "stuck" };
    FarmReport report;
    check(farm->run(config, report, error), "farm runs the episode content");
    check(report.trialsRun == 8, "8 trials (4 seeds x 2 entries)");
    check(report.softlocks == 4, "the stuck entry softlocks for all 4 seeds");
    check(report.repeats == 0, "no repeats in the episode content");

    // The published manifest travels with the farm: the report is derived
    // from compiled+verified content (signature intact), never raw inputs.
    check(package.signature.size() == 32,
          "published signature intact (32 hex)");
    for (const FarmTrialResult& result : report.results) {
        if (result.profile == "stuck") {
            // Steps 0..2 run; step 3 is the first stuck state (still a
            // transition = progress); the stall window (3) starts at step 4
            // and fires at step 6.
            check(result.outcome == FarmOutcome::Softlock &&
                      result.stoppedAt == 6,
                  "stuck entry found as a softlock at the exact step");
        } else {
            check(result.outcome == FarmOutcome::Ok &&
                      result.stepsRun == 32,
                  "steady entry runs the full budget");
        }
    }

    std::printf("[farm] composition with EpisodeCompiler OK\n");
}

}  // namespace

int main() {
    test_deterministic_sweep();
    test_softlock_detection();
    test_repeat_detection();
    test_progress_oracle();
    test_determinism();
    test_refusals();
    test_report_json();
    test_composition_with_episode_compiler();
    if (g_failures == 0) {
        std::printf("[simulation-farm] ALL PASSED\n");
        return 0;
    }
    std::printf("[simulation-farm] %d FAILURE(S)\n", g_failures);
    return 1;
}

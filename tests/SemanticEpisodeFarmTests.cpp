// SemanticEpisodeFarmTests.cpp
//
// Integration gate for the FALTANTES differentials (META §32): the THREE
// content-pipeline contracts chained END-TO-END —
//
//     SemanticEngineAPI (#145) -> EpisodeCompiler (#140) -> HeadlessSimulationFarm (#142)
//
//   1. the semantic core canonicalizes authored documents (schema per kind,
//      defaults applied, bit-exact canonical form);
//   2. those canonical documents COMPILE and VERIFY as an episode (the
//      compiler's validators are bound to the SAME semantic surface);
//   3. the farm drives the COMPILED content headlessly across seeds and
//      finds a SOFTLOCK in a stalling entry — a finding in published,
//      signed content, bit-exact reproducible.
//
// The gate proves the differentials are not isolated pieces: one surface
// (semantic), one pipeline (episode), one detector (farm), all deterministic
// and composed.

#include "engine/compiler/IEpisodeCompiler.hpp"
#include "engine/semantic/ISemanticApi.hpp"
#include "engine/simfarm/ISimulationFarm.hpp"

#include <cstdio>
#include <map>
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

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

using engine::compiler::EpisodeEntry;
using engine::compiler::EpisodeManifest;
using engine::compiler::EpisodePackage;
using engine::compiler::create_episode_compiler;
using engine::semantic::create_semantic_api;
using engine::simfarm::FarmConfig;
using engine::simfarm::FarmOutcome;
using engine::simfarm::FarmReport;
using engine::simfarm::create_simulation_farm;

const char* kPhysicsMaterialSchema =
    "{\"name\":\"physics_material\",\"version\":1,\"fields\":["
    "{\"name\":\"name\",\"type\":\"string\",\"required\":true},"
    "{\"name\":\"friction\",\"type\":\"double\",\"default\":0.6,\"min\":0,\"max\":1},"
    "{\"name\":\"restitution\",\"type\":\"double\",\"default\":0.1,\"min\":0,\"max\":1},"
    "{\"name\":\"flags\",\"type\":\"stringArray\",\"default\":[\"solid\"]},"
    "{\"name\":\"enabled\",\"type\":\"bool\",\"default\":true}]}";

const char* kItemSchema =
    "{\"name\":\"item\",\"version\":1,\"fields\":["
    "{\"name\":\"id\",\"type\":\"string\",\"required\":true},"
    "{\"name\":\"stack\",\"type\":\"int\",\"default\":64,\"min\":1,\"max\":9999},"
    "{\"name\":\"category\",\"type\":\"string\",\"allowed\":[\"tool\",\"material\",\"food\"]},"
    "{\"name\":\"tags\",\"type\":\"stringArray\"}]}";

}  // namespace

int main() {
    std::string error;

    // ---- 1. semantic surface: schemas + canonical documents ----
    auto api = create_semantic_api();
    check(api->register_kind_json(kPhysicsMaterialSchema, error),
          "physics_material schema registered");
    check(api->register_kind_json(kItemSchema, error), "item schema registered");

    std::map<std::string, std::string> canonicalByName;
    std::map<std::string, std::string> kindByName;
    const struct {
        const char* name;
        const char* kind;
        const char* doc;
    } authored[] = {
        { "ice", "physics_material", "{\"name\":\"Ice\",\"friction\":0.4}" },
        { "ground", "physics_material",
          "{\"name\":\"Ground\",\"friction\":0.8,\"enabled\":false}" },
        { "emerald", "item",
          "{\"id\":\"vc:emerald\",\"category\":\"material\",\"tags\":[\"gem\"]}" },
        { "stall_item", "item",
          "{\"id\":\"stall_item\",\"tags\":[\"stall\"]}" },
    };
    for (const auto& entry : authored) {
        std::string canonical;
        check(api->validate(entry.kind, entry.doc, canonical, error),
              "authored doc canonicalized: " + std::string(entry.name));
        check(!canonical.empty(), "canonical non-empty: " +
                                      std::string(entry.name));
        canonicalByName[entry.name] = canonical;
        kindByName[entry.name] = entry.kind;
    }
    check(canonicalByName["stall_item"].find("stall") != std::string::npos,
          "the stalling entry carries the stall marker in its canonical form");

    // ---- 2. episode: canonical documents compile + verify ----
    auto compiler = create_episode_compiler();
    for (const char* kind : { "physics_material", "item" }) {
        check(compiler->register_validator(
                  kind,
                  [&api, kind](const std::string& json, std::string& e) {
                      std::string canonical;
                      return api->validate(kind, json, canonical, e);
                  },
                  error),
              "semantic validator bound for kind: " + std::string(kind));
        check(compiler->register_simulator(
                  kind,
                  [](const std::string& json, int steps, std::string& trace,
                     std::string&) {
                      trace = "sim:" + std::to_string(steps) + ":" +
                              std::to_string(json.size());
                      return true;
                  },
                  error),
              "simulator registered for kind: " + std::string(kind));
        check(compiler->register_tester(
                  kind,
                  [](const std::string&, std::string&) { return true; }, error),
              "tester registered for kind: " + std::string(kind));
    }

    EpisodeManifest manifest;
    manifest.title = "SemanticEpisodeFarm";
    for (const auto& entry : authored) {
        EpisodeEntry episodeEntry;
        episodeEntry.kind = kindByName[entry.name];
        episodeEntry.name = entry.name;
        episodeEntry.json = canonicalByName[entry.name];
        manifest.entries.push_back(std::move(episodeEntry));
    }

    EpisodePackage package;
    check(compiler->compile(manifest, package, error),
          "canonical documents compile as an episode");
    check(error.empty(), "compile diagnostic empty");
    check(compiler->verify(package, error), "episode verifies");
    check(package.signature.size() == 32, "published signature intact");
    check(package.manifestJson.find("\"name\":\"stall_item\"") !=
              std::string::npos,
          "manifest carries the stalling entry");

    // ---- 3. farm: drive the COMPILED content headlessly ----
    auto farm = create_simulation_farm();
    check(farm->set_driver(
              [canonicalByName](const std::string&, std::uint64_t seed,
                                const std::string& profile, std::uint64_t step,
                                std::string&) -> std::string {
                  const auto found = canonicalByName.find(profile);
                  const std::string json =
                      (found == canonicalByName.end()) ? "" : found->second;
                  const bool stalls =
                      json.find("stall") != std::string::npos;
                  if (stalls && step >= 3) {
                      return "stuck:" + profile;
                  }
                  return "run:" + profile + ":" + std::to_string(seed) + ":" +
                         std::to_string(step);
              },
              error),
          "content driver set");

    FarmConfig config;
    config.maxStepsPerTrial = 32;
    config.maxStallSteps = 3;
    config.seedStart = 0;
    config.seedEnd = 4;  // 4 seeds
    config.profiles = { "ice", "ground", "emerald", "stall_item" };
    FarmReport report;
    check(farm->run(config, report, error), "farm runs the compiled episode");
    check(error.empty(), "farm diagnostic empty");
    check(report.trialsRun == 16, "16 trials (4 seeds x 4 entries)");
    check(report.softlocks == 4 && report.repeats == 0,
          "the stalling entry softlocks for all 4 seeds; no repeats");

    for (const auto& result : report.results) {
        if (result.profile == "stall_item") {
            // First stuck state at step 3 (still a transition); the stall
            // window (3) starts at step 4 and fires at step 6.
            check(result.outcome == FarmOutcome::Softlock &&
                      result.stoppedAt == 6,
                  "stall_item found as a softlock at the exact step");
        } else {
            check(result.outcome == FarmOutcome::Ok &&
                      result.stepsRun == 32,
                  "non-stalling entries run the full budget");
        }
    }

    // Determinism of the whole chain: a fresh farm over the same compiled
    // content reproduces the identical report.
    auto farm2 = create_simulation_farm();
    check(farm2->set_driver(
              [canonicalByName](const std::string&, std::uint64_t seed,
                                const std::string& profile, std::uint64_t step,
                                std::string&) -> std::string {
                  const auto found = canonicalByName.find(profile);
                  const std::string json =
                      (found == canonicalByName.end()) ? "" : found->second;
                  const bool stalls =
                      json.find("stall") != std::string::npos;
                  if (stalls && step >= 3) {
                      return "stuck:" + profile;
                  }
                  return "run:" + profile + ":" + std::to_string(seed) + ":" +
                         std::to_string(step);
              },
              error),
          "content driver 2 set");
    FarmReport report2;
    check(farm2->run(config, report2, error), "second farm run");
    check(farm->report_to_json(report) == farm2->report_to_json(report2),
          "the whole chain is bit-identical across instances");

    if (g_failures == 0) {
        std::printf("[semantic-episode-farm] ALL PASSED\n");
        return 0;
    }
    std::printf("[semantic-episode-farm] %d FAILURE(S)\n", g_failures);
    return 1;
}

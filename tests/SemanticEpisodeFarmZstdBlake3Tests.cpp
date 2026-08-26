// SemanticEpisodeFarmZstdBlake3Tests.cpp
//
// Production-integration gate (AGENT-5-DOCS-20260825, claim 01:50, 2026-08-26):
// the FULL content-pipeline chain with the PRODUCTION seams wired —
//
//     SemanticEngineAPI (#145) -> EpisodeCompiler (#140, zstd+blake3)
//                              -> HeadlessSimulationFarm (#142)
//
// Why this gate exists: #147 (SemanticEpisodeFarmTests) proved the chain with
// the DEFAULT providers (identity codec + splitmix64 digest), and #151
// (EpisodeCompilerZstdBlake3Tests) proved the production seams on the
// EpisodeCompiler ALONE. The chain END-TO-END with the REAL promoted adapters
// (create_zstd_compression_provider / create_blake3_hash_provider — the same
// TUs backing the persistent world save) had never been proven together.
//
// This gate proves:
//   1. semantic canonicalizes authored documents (bit-exact);
//   2. those canonical documents COMPILE and VERIFY as an episode through the
//      REAL zstd codec + REAL BLAKE3-256 hasher (signature == 64-char hex ==
//      hash_hex(manifest + payload); payload is a real zstd frame);
//   3. the farm drives the PUBLISHED, SIGNED content headlessly across
//      4 seeds x 4 entries and finds the SOFTLOCK in the stalling entry,
//      bit-exact reproducible across instances;
//   4. tampering with the signed content is detected under BLAKE3 even after
//      the whole chain has run.

#include "engine/compiler/IEpisodeCompiler.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "engine/hashing/IHashProvider.hpp"
#include "engine/semantic/ISemanticApi.hpp"
#include "engine/simfarm/ISimulationFarm.hpp"

#include <cstdint>
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
using engine::compression::create_zstd_compression_provider;
using engine::hashing::create_blake3_hash_provider;
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

    const auto zstd = create_zstd_compression_provider();
    const auto blake3 = create_blake3_hash_provider();
    check(zstd != nullptr && blake3 != nullptr, "production providers created");

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
        check(!canonical.empty(),
              "canonical non-empty: " + std::string(entry.name));
        canonicalByName[entry.name] = canonical;
        kindByName[entry.name] = entry.kind;
    }

    // ---- 2. episode: compile + verify through the PRODUCTION seams ----
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

    // Wire the REAL promoted adapters through the codec/hash seams.
    check(compiler->set_codec(
              [zstd](const std::string& data, std::string& e) {
                  const std::string out = zstd->compress(data);
                  if (out.empty()) {
                      e = "zstd compression failed";
                      return std::string();
                  }
                  return out;
              },
              [zstd](const std::string& data, std::string& e) {
                  if (!data.empty() && !zstd->is_compressed(data)) {
                      e = "zstd decompression failed";
                      return std::string();
                  }
                  return zstd->decompress(data);
              },
              error),
          "zstd codec wired through the seam");
    check(compiler->set_hasher(
              [blake3](const std::string& data) {
                  return blake3->hash_hex(data);
              },
              error),
          "blake3 hasher wired through the seam");

    EpisodeManifest manifest;
    manifest.title = "SemanticEpisodeFarmZstdBlake3";
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
    check(compiler->verify(package, error), "episode verifies through zstd");
    check(package.signature.size() == 64 &&
              package.signature == blake3->hash_hex(package.manifestJson +
                                                    package.payload),
          "signature is the real BLAKE3-256 digest of (manifest + payload)");
    check(zstd->is_compressed(package.payload),
          "the published payload is a real zstd frame");

    // ---- 3. farm: drive the PUBLISHED, SIGNED content headlessly ----
    auto farm = create_simulation_farm();
    check(farm->set_driver(
              [canonicalByName](const std::string&, std::uint64_t seed,
                                const std::string& profile, std::uint64_t step,
                                std::string&) -> std::string {
                  const auto found = canonicalByName.find(profile);
                  const std::string json =
                      (found == canonicalByName.end()) ? "" : found->second;
                  const bool stalls = json.find("stall") != std::string::npos;
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
    check(farm->run(config, report, error),
          "farm runs the signed episode content");
    check(error.empty(), "farm diagnostic empty");
    check(report.trialsRun == 16, "16 trials (4 seeds x 4 entries)");
    check(report.softlocks == 4 && report.repeats == 0,
          "the stalling entry softlocks for all 4 seeds; no repeats");

    for (const auto& result : report.results) {
        if (result.profile == "stall_item") {
            check(result.outcome == FarmOutcome::Softlock &&
                      result.stoppedAt == 6,
                  "stall_item found as a softlock at the exact step");
        } else {
            check(result.outcome == FarmOutcome::Ok && result.stepsRun == 32,
                  "non-stalling entries run the full budget");
        }
    }

    // ---- 4. tamper detection under BLAKE3 AFTER the full chain ----
    EpisodePackage tampered = package;
    tampered.payload[0] ^= 0x01;
    check(!compiler->verify(tampered, error) &&
              error.find("signature mismatch") != std::string::npos,
          "flipped payload byte detected under BLAKE3");

    // ---- 5. determinism of the WHOLE chain across instances ----
    auto compiler2 = create_episode_compiler();
    for (const char* kind : { "physics_material", "item" }) {
        compiler2->register_validator(
            kind,
            [&api, kind](const std::string& json, std::string& e) {
                std::string canonical;
                return api->validate(kind, json, canonical, e);
            },
            error);
        compiler2->register_simulator(
            kind,
            [](const std::string& json, int steps, std::string& trace,
               std::string&) {
                trace = "sim:" + std::to_string(steps) + ":" +
                        std::to_string(json.size());
                return true;
            },
            error);
        compiler2->register_tester(
            kind, [](const std::string&, std::string&) { return true; },
            error);
    }
    check(compiler2->set_codec(
              [zstd](const std::string& data, std::string& e) {
                  const std::string out = zstd->compress(data);
                  if (out.empty()) {
                      e = "zstd compression failed";
                      return std::string();
                  }
                  return out;
              },
              [zstd](const std::string& data, std::string& e) {
                  if (!data.empty() && !zstd->is_compressed(data)) {
                      e = "zstd decompression failed";
                      return std::string();
                  }
                  return zstd->decompress(data);
              },
              error),
          "compiler2 zstd codec set");
    check(compiler2->set_hasher(
              [blake3](const std::string& data) {
                  return blake3->hash_hex(data);
              },
              error),
          "compiler2 blake3 hasher set");
    EpisodePackage package2;
    check(compiler2->compile(manifest, package2, error), "compile 2 succeeds");
    check(package2.manifestJson == package.manifestJson &&
              package2.payload == package.payload &&
              package2.signature == package.signature,
          "the production chain is bit-identical across instances");

    auto farm2 = create_simulation_farm();
    check(farm2->set_driver(
              [canonicalByName](const std::string&, std::uint64_t seed,
                                const std::string& profile, std::uint64_t step,
                                std::string&) -> std::string {
                  const auto found = canonicalByName.find(profile);
                  const std::string json =
                      (found == canonicalByName.end()) ? "" : found->second;
                  const bool stalls = json.find("stall") != std::string::npos;
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
        std::printf("[semantic-episode-farm-zstd-blake3] ALL PASSED\n");
        return 0;
    }
    std::printf("[semantic-episode-farm-zstd-blake3] %d FAILURE(S)\n",
                g_failures);
    return 1;
}

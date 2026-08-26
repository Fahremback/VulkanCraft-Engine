#pragma once

// IEpisodeCompiler — FALTANTES differential "EpisodeCompiler": VALIDATE,
// SIMULATE, TEST, COMPRESS, SIGN AND PUBLISH CONTENT (META section 32 —
// engine-own code; nothing in external/solutions resolves it).
//
// An EPISODE is a coherent bundle of authored content (missions, abilities,
// vehicles, world profiles, gaits...). Before it can ship it must pass a
// deterministic gate: every entry is VALIDATED, SIMULATED (a deterministic
// dry-run) and TESTED; the validated content is packed, COMPRESSED, SIGNED
// (content-addressed hash) and PUBLISHED as a self-describing package. The
// whole pipeline must be reproducible: the same episode + the same registered
// hooks produce a BIT-IDENTICAL package on every machine and every run, and a
// single failing entry fails the entire compile (all-or-nothing — nothing
// partial is ever published).
//
// The compiler is kind-agnostic: per-KIND stage hooks are registered by the
// project (validators / simulators / testers), so the engine never hard-codes
// a content type. The compression and hashing stages are SEAMS with
// deterministic defaults (identity codec + a deterministic content digest);
// production wires the promoted Zstandard (engine/compression) and BLAKE3
// (engine/hashing) adapters through those seams. The differential is the
// deterministic, signed, all-or-nothing PIPELINE itself.
//
// Stages (fixed order, deterministic):
//   1. VALIDATE  — every entry through its kind's validator; an unknown kind
//      or a failing validator refuses the whole compile.
//   2. SIMULATE  — every entry through its kind's simulator (declared step
//      count); the deterministic trace is folded into a per-entry digest that
//      lands in the published manifest (simulation is observable).
//   3. TEST      — every entry through its kind's tester; a failure refuses
//      the compile.
//   4. PACK      — entries are serialized in manifest order into one content
//      blob (length-prefixed, deterministic).
//   5. COMPRESS  — the content blob through the codec seam.
//   6. SIGN      — signature = hash(manifestCanonical + compressedPayload).
//   7. PUBLISH   — the package {manifest, payload, signature}; the manifest
//      carries every entry's validation/simulation/test outcome + digest.
//
// verify() re-derives the signature from the manifest + payload and checks
// the payload integrity (a single flipped byte changes the signature).
//
// PURE and DETERMINISTIC — the compiler never touches a concrete world; it
// only runs the registered pure hooks and assembles the package. All
// refusals are all-or-nothing with a diagnostic. Self-contained (std).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace compiler {

// One authored content entry of an episode. `kind` selects the registered
// stage hooks; `name` must be unique within the episode; `json` is the
// content document (opaque to the compiler — the hooks interpret it).
struct EpisodeEntry {
    std::string kind;  // "mission", "ability", "vehicle", "profile", ...
    std::string name;  // unique within the episode
    std::string json;  // the content document
};

// The episode to compile. `title` identifies the episode; entries keep their
// author order (deterministic pipeline order).
struct EpisodeManifest {
    int version{ 1 };
    std::string title;
    std::vector<EpisodeEntry> entries;
};

// ---- stage hooks (caller-registered; must be PURE and deterministic) ----

// Structural validation of one entry's content. Returns false with a
// diagnostic on refusal.
using EntryValidator = std::function<bool(const std::string& json,
                                          std::string& errorOut)>;
// Deterministic dry-run of one entry's content for `steps` iterations.
// Returns false with a diagnostic on failure; on success `traceOut` receives
// a deterministic trace (folded into the published per-entry digest).
using EntrySimulator = std::function<bool(const std::string& json, int steps,
                                          std::string& traceOut,
                                          std::string& errorOut)>;
// Extra checks over one entry's content. Returns false with a diagnostic on
// failure.
using EntryTester = std::function<bool(const std::string& json,
                                       std::string& errorOut)>;

// Codec seam (production: Zstandard). Default = identity. compress and
// decompress must be inverse for every input the pipeline produces.
using CompressFn = std::function<std::string(const std::string& data,
                                             std::string& errorOut)>;
using DecompressFn = std::function<std::string(const std::string& data,
                                               std::string& errorOut)>;
// Hash seam (production: BLAKE3). Default = deterministic content digest.
using HashFn = std::function<std::string(const std::string& data)>;

// One entry's published result (inside the manifest).
struct EpisodeEntryResult {
    std::string kind;
    std::string name;
    bool validated{ false };
    bool simulated{ false };
    bool tested{ false };
    std::string digest;  // hash(json + '|' + simulation trace)
};

// The published package. `manifestJson` is the canonical deterministic
// manifest (bit-exact for the same inputs); `payload` is the compressed
// content blob; `signature` is hash(manifestCanonical + payload).
struct EpisodePackage {
    std::string manifestJson;
    std::string payload;
    std::string signature;
};

class IEpisodeCompiler {
public:
    virtual ~IEpisodeCompiler() = default;

    // ---- per-kind hook registry (all-or-nothing per kind) ----
    // Registers the validator for `kind`. Refuses an empty kind or a null
    // hook. Re-registering replaces the previous hook.
    virtual bool register_validator(const std::string& kind,
                                    EntryValidator validator,
                                    std::string& errorOut) = 0;
    virtual bool register_simulator(const std::string& kind,
                                    EntrySimulator simulator,
                                    std::string& errorOut) = 0;
    virtual bool register_tester(const std::string& kind, EntryTester tester,
                                 std::string& errorOut) = 0;

    // ---- seams (defaults: identity codec + deterministic digest) ----
    // Sets the compression/decompression pair. Refuses a null hook. Must be
    // set together.
    virtual bool set_codec(CompressFn compress, DecompressFn decompress,
                           std::string& errorOut) = 0;
    virtual bool set_hasher(HashFn hash, std::string& errorOut) = 0;

    // ---- the pipeline ----
    // Compiles the episode through the fixed stage order. `out` is cleared at
    // entry and only filled on success. All-or-nothing: an unknown kind, a
    // failing validator/simulator/tester, a duplicate entry name, a null
    // required hook or a failing codec refuses the WHOLE compile (nothing is
    // published, `out` stays empty). The same manifest + the same hooks
    // reproduce a bit-identical package. Refuses an invalid manifest version
    // or a null title.
    virtual bool compile(const EpisodeManifest& manifest,
                         EpisodePackage& out, std::string& errorOut) = 0;

    // ---- verification ----
    // Re-derives the signature from the manifest + payload and checks that
    // the payload is the decompression of the published blob. Refuses
    // all-or-nothing on a malformed manifest or a failed decompression;
    // returns false with "signature mismatch" on tampering.
    virtual bool verify(const EpisodePackage& package,
                        std::string& errorOut) const = 0;

    // Decompresses a published payload back to the original content blob
    // (the verify/round-trip helper the consumer uses before loading).
    virtual bool unpack(const EpisodePackage& package, std::string& contentOut,
                        std::string& errorOut) const = 0;
};

// The only implementation (src/engine/sdk/EpisodeCompiler.cpp).
std::unique_ptr<IEpisodeCompiler> create_episode_compiler();

}  // namespace compiler
}  // namespace engine

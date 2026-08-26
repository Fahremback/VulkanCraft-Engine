// EpisodeCompilerTests.cpp
//
// FALTANTES differential — EpisodeCompiler: VALIDATE, SIMULATE, TEST,
// COMPRESS, SIGN and PUBLISH CONTENT (META §32). The gate drives the PUBLIC
// contract headless and proves:
//   - the fixed stage pipeline (validate -> simulate -> test -> pack ->
//     compress -> sign -> publish) runs in order and is all-or-nothing;
//   - the same manifest + the same hooks reproduce a BIT-IDENTICAL package
//     on every instance (cross-instance determinism);
//   - the simulation trace is OBSERVABLE: it is folded into the per-entry
//     digest that lands in the published manifest (a different trace ->
//     a different digest/signature);
//   - packing is deterministic length-prefixed content in manifest (author)
//     order; unpack() returns exactly that content blob;
//   - the compression and hashing SEAMS are wired: a custom codec and a
//     custom hasher change the payload/signature accordingly, and a failing
//     codec refuses the whole compile;
//   - verify() re-derives the signature and detects tampering (a flipped
//     byte in the payload or in the manifest, a truncated payload, a
//     malformed manifest);
//   - all-or-nothing refusals: unknown kind, missing hook, failing stage,
//     duplicate entry name, empty title, bad version, null registrations —
//     nothing is published (`out` stays empty) and the diagnostic is set.

#include "engine/compiler/IEpisodeCompiler.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

using engine::compiler::EpisodeEntry;
using engine::compiler::EpisodeManifest;
using engine::compiler::EpisodePackage;
using engine::compiler::create_episode_compiler;

// ---- deterministic hooks used across the gate ----

// Structural validator: the content must be a non-empty JSON object.
bool validate_json(const std::string& json, std::string& error) {
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') {
        error = "not a json object";
        return false;
    }
    return true;
}

// Deterministic dry-run: folds the step count + the number of 'a' chars into
// the trace (the trace lands in the published per-entry digest).
bool simulate_json(const std::string& json, int steps, std::string& trace,
                   std::string& error) {
    std::size_t count = 0;
    for (const char c : json) {
        if (c == 'a') ++count;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "steps=%d:as=%zu", steps, count);
    trace = buffer;
    error.clear();
    return true;
}

// Extra check: the entry must declare itself ready.
bool test_json(const std::string& json, std::string& error) {
    if (json.find("\"ready\":true") == std::string::npos) {
        error = "entry not ready";
        return false;
    }
    return true;
}

void register_mission_hooks(engine::compiler::IEpisodeCompiler& compiler,
                            std::string& error) {
    check(compiler.register_validator("mission", validate_json, error),
          "mission validator registered");
    check(compiler.register_simulator("mission", simulate_json, error),
          "mission simulator registered");
    check(compiler.register_tester("mission", test_json, error),
          "mission tester registered");
}

EpisodeManifest two_mission_episode() {
    EpisodeManifest manifest;
    manifest.version = 1;
    manifest.title = "MyEpisode";
    EpisodeEntry first;
    first.kind = "mission";
    first.name = "m1";
    first.json = "{\"hp\":10,\"ready\":true}";
    EpisodeEntry second;
    second.kind = "mission";
    second.name = "m2";
    second.json = "{\"hp\":5,\"ready\":true,\"flags\":\"aaa\"}";
    manifest.entries = { first, second };
    return manifest;
}

// Reconstructs the expected length-prefixed packed bytes for one entry
// ([u32 LE kindLen][kind][0x00][name][0x00][json]).
std::string pack_one(const std::string& kind, const std::string& name,
                     const std::string& json) {
    std::string out;
    const std::uint32_t kindSize = static_cast<std::uint32_t>(kind.size());
    out.push_back(static_cast<char>(kindSize & 0xFFu));
    out.push_back(static_cast<char>((kindSize >> 8) & 0xFFu));
    out.push_back(static_cast<char>((kindSize >> 16) & 0xFFu));
    out.push_back(static_cast<char>((kindSize >> 24) & 0xFFu));
    out += kind;
    out.push_back('\0');
    out += name;
    out.push_back('\0');
    out += json;
    return out;
}

// ---- 1. full pipeline: happy path, publish + verify + unpack ----

void test_happy_path_pipeline() {
    auto compiler = create_episode_compiler();
    std::string error;
    register_mission_hooks(*compiler, error);

    const EpisodeManifest manifest = two_mission_episode();
    EpisodePackage package;
    check(compiler->compile(manifest, package, error), "compile succeeds");
    check(error.empty(), "compile diagnostic empty");
    check(!package.manifestJson.empty() && !package.payload.empty() &&
              !package.signature.empty(),
          "package fully populated");
    check(package.signature.size() == 32, "default digest is 32 hex chars");

    // The manifest is canonical and carries the fixed deterministic dry-run
    // length plus per-entry stage outcomes + digests.
    check(package.manifestJson.find("\"title\":\"MyEpisode\"") !=
              std::string::npos,
          "manifest carries the title");
    check(package.manifestJson.find("\"stepCount\":16") != std::string::npos,
          "manifest carries the fixed simulation step count");
    check(package.manifestJson.find("\"validated\":true") != std::string::npos &&
              package.manifestJson.find("\"simulated\":true") != std::string::npos &&
              package.manifestJson.find("\"tested\":true") != std::string::npos,
          "manifest carries every entry's stage outcomes");
    check(package.manifestJson.find("\"digest\":\"") != std::string::npos,
          "manifest carries per-entry digests");

    // Payload is exactly the deterministic packed content blob (identity
    // codec by default): entries packed in author order.
    const std::string expectedPacked =
        pack_one("mission", "m1", "{\"hp\":10,\"ready\":true}") +
        pack_one("mission", "m2", "{\"hp\":5,\"ready\":true,\"flags\":\"aaa\"}");
    check(package.payload == expectedPacked,
          "payload is the exact length-prefixed packed content");
    const std::size_t m1pos = package.manifestJson.find("\"name\":\"m1\"");
    const std::size_t m2pos = package.manifestJson.find("\"name\":\"m2\"");
    check(m1pos != std::string::npos && m2pos != std::string::npos &&
              m1pos < m2pos,
          "manifest entries keep author order");

    // verify() re-derives the signature and checks the payload.
    check(compiler->verify(package, error), "verify passes");
    check(error.empty(), "verify diagnostic empty");

    // unpack() restores the original content blob.
    std::string content;
    check(compiler->unpack(package, content, error), "unpack succeeds");
    check(content == expectedPacked, "unpack returns the original content");

    std::printf("[episode] happy-path pipeline (publish/verify/unpack) OK\n");
}

// ---- 2. cross-instance bit-exact determinism ----

void test_bit_exact_determinism() {
    std::string error;
    const EpisodeManifest manifest = two_mission_episode();

    auto compilerA = create_episode_compiler();
    register_mission_hooks(*compilerA, error);
    EpisodePackage packageA;
    check(compilerA->compile(manifest, packageA, error), "compile A ok");

    auto compilerB = create_episode_compiler();
    register_mission_hooks(*compilerB, error);
    EpisodePackage packageB;
    check(compilerB->compile(manifest, packageB, error), "compile B ok");

    check(packageA.manifestJson == packageB.manifestJson,
          "manifest bit-identical across instances");
    check(packageA.payload == packageB.payload,
          "payload bit-identical across instances");
    check(packageA.signature == packageB.signature,
          "signature bit-identical across instances");

    std::printf("[episode] cross-instance bit-exact determinism OK\n");
}

// ---- 3. stage order + simulation observability ----

void test_stage_order_and_observability() {
    std::string error;
    EpisodeManifest manifest = two_mission_episode();

    // Unknown kind -> refused before any hook runs.
    {
        auto compiler = create_episode_compiler();
        register_mission_hooks(*compiler, error);
        EpisodeManifest bad = manifest;
        bad.entries[0].kind = "dungeon";
        EpisodePackage out;
        check(!compiler->compile(bad, out, error) &&
                  error.find("no validator registered for kind: dungeon") !=
                      std::string::npos,
              "unknown kind refused with diagnostic");
        check(out.manifestJson.empty() && out.payload.empty() &&
                  out.signature.empty(),
              "nothing published on unknown kind");
    }

    // Missing tester hook -> refused.
    {
        auto compiler = create_episode_compiler();
        check(compiler->register_validator("mission", validate_json, error),
              "validator registered");
        check(compiler->register_simulator("mission", simulate_json, error),
              "simulator registered");
        EpisodePackage out;
        check(!compiler->compile(manifest, out, error) &&
                  error.find("no tester registered for kind: mission") !=
                      std::string::npos,
              "missing tester hook refused");
    }

    // Failing validator -> refused with the stage named.
    {
        auto compiler = create_episode_compiler();
        register_mission_hooks(*compiler, error);
        EpisodeManifest bad = manifest;
        bad.entries[0].json = "not json";
        EpisodePackage out;
        check(!compiler->compile(bad, out, error) &&
                  error.find("validation failed for m1") != std::string::npos,
              "failing validator refuses the compile");
    }

    // Failing simulator -> refused with the stage named.
    {
        auto compiler = create_episode_compiler();
        register_mission_hooks(*compiler, error);
        check(compiler->register_simulator(
                  "mission",
                  [](const std::string&, int, std::string&, std::string& e) {
                      e = "dry-run blew up";
                      return false;
                  },
                  error),
              "broken simulator registered");
        EpisodePackage out;
        check(!compiler->compile(manifest, out, error) &&
                  error.find("simulation failed for m1") != std::string::npos,
              "failing simulator refuses the compile");
    }

    // Failing tester -> refused with the stage named.
    {
        auto compiler = create_episode_compiler();
        register_mission_hooks(*compiler, error);
        EpisodeManifest bad = manifest;
        bad.entries[1].json = "{\"hp\":5}";
        EpisodePackage out;
        check(!compiler->compile(bad, out, error) &&
                  error.find("test failed for m2") != std::string::npos,
              "failing tester refuses the compile");
    }

    // Simulation is OBSERVABLE: a different trace -> a different per-entry
    // digest -> a different manifest and signature.
    {
        auto compilerA = create_episode_compiler();
        register_mission_hooks(*compilerA, error);
        EpisodePackage packageA;
        check(compilerA->compile(manifest, packageA, error), "compile A ok");

        auto compilerB = create_episode_compiler();
        register_mission_hooks(*compilerB, error);
        check(compilerB->register_simulator(
                  "mission",
                  [](const std::string&, int, std::string& trace,
                     std::string&) {
                      trace = "different-dry-run";
                      return true;
                  },
                  error),
              "trace-differing simulator registered");
        EpisodePackage packageB;
        check(compilerB->compile(manifest, packageB, error), "compile B ok");

        check(packageA.manifestJson != packageB.manifestJson,
              "different trace changes the published manifest (digest)");
        check(packageA.signature != packageB.signature,
              "different trace changes the signature");
        check(compilerB->verify(packageB, error),
              "the re-derived signature still verifies");
    }

    // Structural refusals: duplicate name, empty kind, empty title, bad
    // version.
    {
        auto compiler = create_episode_compiler();
        register_mission_hooks(*compiler, error);
        EpisodePackage out;

        EpisodeManifest dup = manifest;
        dup.entries[1].name = "m1";
        check(!compiler->compile(dup, out, error) &&
                  error.find("duplicate entry name: m1") != std::string::npos,
              "duplicate entry name refused");

        EpisodeManifest emptyKind = manifest;
        emptyKind.entries[0].kind = "";
        check(!compiler->compile(emptyKind, out, error) &&
                  !error.empty(),
              "empty entry kind refused");

        EpisodeManifest emptyName = manifest;
        emptyName.entries[0].name = "";
        check(!compiler->compile(emptyName, out, error) && !error.empty(),
              "empty entry name refused");

        EpisodeManifest noTitle = manifest;
        noTitle.title = "";
        check(!compiler->compile(noTitle, out, error) &&
                  error.find("manifest title must be non-empty") !=
                      std::string::npos,
              "empty title refused");

        EpisodeManifest badVersion = manifest;
        badVersion.version = 2;
        check(!compiler->compile(badVersion, out, error) &&
                  error.find("unsupported manifest version") !=
                      std::string::npos,
              "unsupported manifest version refused");
    }

    std::printf("[episode] stage order + simulation observability OK\n");
}

// ---- 4. all-or-nothing + registration refusals ----

void test_all_or_nothing_and_registrations() {
    std::string error;
    auto compiler = create_episode_compiler();

    // Registration refusals.
    check(!compiler->register_validator("", validate_json, error) &&
              !error.empty(),
          "empty kind validator refused");
    check(!compiler->register_validator("mission", nullptr, error) &&
              !error.empty(),
          "null validator refused");
    check(!compiler->register_simulator("", simulate_json, error) &&
              !error.empty(),
          "empty kind simulator refused");
    check(!compiler->register_tester("", test_json, error) && !error.empty(),
          "empty kind tester refused");
    check(!compiler->set_codec(nullptr, nullptr, error) && !error.empty(),
          "null codec refused");
    check(!compiler->set_hasher(nullptr, error) && !error.empty(),
          "null hasher refused");

    // A failing compile leaves `out` untouched (default empty), and the same
    // compiler still compiles once the input is fixed.
    register_mission_hooks(*compiler, error);
    EpisodeManifest bad = two_mission_episode();
    bad.entries[0].json = "broken";
    EpisodePackage out;
    check(!compiler->compile(bad, out, error) && !error.empty(),
          "failing compile refused");
    check(out.manifestJson.empty() && out.payload.empty() &&
              out.signature.empty(),
          "out untouched after refusal");

    const EpisodeManifest good = two_mission_episode();
    check(compiler->compile(good, out, error), "fixed manifest compiles");
    check(!out.manifestJson.empty(), "publish happened after the fix");

    std::printf("[episode] all-or-nothing + registration refusals OK\n");
}

// ---- 5. signature tamper detection ----

void test_tamper_detection() {
    auto compiler = create_episode_compiler();
    std::string error;
    register_mission_hooks(*compiler, error);
    const EpisodeManifest manifest = two_mission_episode();
    EpisodePackage package;
    check(compiler->compile(manifest, package, error), "compile ok");
    check(compiler->verify(package, error), "verify ok");

    // Flip a byte in the payload: size is unchanged, the re-derived
    // signature over the tampered payload differs.
    EpisodePackage tamperedPayload = package;
    tamperedPayload.payload[0] ^= 0x01;
    check(!compiler->verify(tamperedPayload, error) &&
              error.find("signature mismatch") != std::string::npos,
          "flipped payload byte detected as signature mismatch");

    // Change the manifest (title) while keeping it valid JSON: the signature
    // no longer covers the manifest.
    EpisodePackage tamperedManifest = package;
    const std::size_t titlePos =
        tamperedManifest.manifestJson.find("MyEpisode");
    check(titlePos != std::string::npos, "title found");
    tamperedManifest.manifestJson[titlePos] = 'X';
    check(!compiler->verify(tamperedManifest, error) &&
              error.find("signature mismatch") != std::string::npos,
          "tampered manifest detected as signature mismatch");

    // Truncated payload: length mismatch caught before the signature check.
    EpisodePackage truncated = package;
    truncated.payload = truncated.payload.substr(0, truncated.payload.size() - 1);
    check(!compiler->verify(truncated, error) &&
              error.find("payload length mismatch") != std::string::npos,
          "truncated payload detected as length mismatch");

    // Malformed manifest JSON.
    EpisodePackage malformed = package;
    malformed.manifestJson = "{not json";
    check(!compiler->verify(malformed, error) &&
              error.find("malformed package manifest") != std::string::npos,
          "malformed manifest refused");

    std::printf("[episode] signature tamper detection OK\n");
}

// ---- 6. codec seam ----

void test_codec_seam() {
    auto compiler = create_episode_compiler();
    std::string error;
    register_mission_hooks(*compiler, error);

    // Reverse-string codec: compress and decompress are inverse, so the
    // pipeline still publishes, verifies and unpacks to the original blob.
    check(compiler->set_codec(
              [](const std::string& data, std::string&) {
                  return std::string(data.rbegin(), data.rend());
              },
              [](const std::string& data, std::string&) {
                  return std::string(data.rbegin(), data.rend());
              },
              error),
          "reverse codec set");
    const EpisodeManifest manifest = two_mission_episode();
    EpisodePackage package;
    check(compiler->compile(manifest, package, error), "compile with codec");
    check(!package.payload.empty() &&
              package.payload.size() ==
                  pack_one("mission", "m1", "{\"hp\":10,\"ready\":true}").size() +
                      pack_one("mission", "m2",
                               "{\"hp\":5,\"ready\":true,\"flags\":\"aaa\"}")
                          .size(),
          "payload is the compressed (reversed) packed blob");
    check(compiler->verify(package, error), "verify with codec");
    std::string content;
    check(compiler->unpack(package, content, error), "unpack with codec");
    const std::string expectedPacked =
        pack_one("mission", "m1", "{\"hp\":10,\"ready\":true}") +
        pack_one("mission", "m2", "{\"hp\":5,\"ready\":true,\"flags\":\"aaa\"}");
    check(content == expectedPacked,
          "unpack restores the original content through the codec");

    // A failing codec refuses the whole compile (all-or-nothing).
    auto failing = create_episode_compiler();
    register_mission_hooks(*failing, error);
    check(failing->set_codec(
              [](const std::string&, std::string& e) {
                  e = "codec exploded";
                  return std::string();
              },
              [](const std::string& data, std::string&) { return data; },
              error),
          "failing codec set");
    EpisodePackage out;
    check(!failing->compile(manifest, out, error) &&
              error.find("codec exploded") != std::string::npos,
          "failing codec refuses the compile");
    check(out.manifestJson.empty() && out.payload.empty() &&
              out.signature.empty(),
          "nothing published on codec failure");

    std::printf("[episode] codec seam (custom + failing) OK\n");
}

// Content-sensitive deterministic hasher for the seam test: "H:" + length +
// ":x" + XOR of every byte. Flipping ANY byte changes the XOR sum, so the
// signature binds to the content (a length-only hash could not detect a
// same-length byte flip).
std::string content_hash(const std::string& data) {
    unsigned char x = 0;
    for (const char c : data) {
        x ^= static_cast<unsigned char>(c);
    }
    return "H:" + std::to_string(data.size()) + ":x" +
           std::to_string(static_cast<int>(x));
}

// ---- 7. hasher seam ----

void test_hasher_seam() {
    auto compiler = create_episode_compiler();
    std::string error;
    register_mission_hooks(*compiler, error);

    // Custom deterministic hasher. Both the per-entry digests and the
    // package signature must flow through the seam.
    check(compiler->set_hasher(
              [](const std::string& data) { return content_hash(data); },
              error),
          "custom hasher set");

    const EpisodeManifest manifest = two_mission_episode();
    EpisodePackage package;
    check(compiler->compile(manifest, package, error), "compile with hasher");
    const std::string expectedSignature =
        content_hash(package.manifestJson + package.payload);
    check(package.signature == expectedSignature,
          "signature flows through the hasher seam");
    check(package.manifestJson.find("\"digest\":\"H:") != std::string::npos,
          "entry digests flow through the hasher seam");
    check(compiler->verify(package, error), "verify with custom hasher");

    // Tampering is still detected under the custom hasher (the XOR sum
    // changes, so the re-derived signature differs).
    EpisodePackage tampered = package;
    tampered.payload[0] ^= 0x01;
    check(!compiler->verify(tampered, error) &&
              error.find("signature mismatch") != std::string::npos,
          "tampering detected under the custom hasher");

    std::printf("[episode] hasher seam OK\n");
}

}  // namespace

int main() {
    test_happy_path_pipeline();
    test_bit_exact_determinism();
    test_stage_order_and_observability();
    test_all_or_nothing_and_registrations();
    test_tamper_detection();
    test_codec_seam();
    test_hasher_seam();
    if (g_failures == 0) {
        std::printf("[episode-compiler] ALL PASSED\n");
        return 0;
    }
    std::printf("[episode-compiler] %d FAILURE(S)\n", g_failures);
    return 1;
}

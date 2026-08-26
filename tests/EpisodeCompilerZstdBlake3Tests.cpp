// EpisodeCompilerZstdBlake3Tests.cpp
//
// Production-wiring gate for the EpisodeCompiler differential (#140): the
// contract spec says the codec/hash SEAMS are "production wired" through the
// promoted Zstandard (engine/compression) and BLAKE3 (engine/hashing)
// adapters. This gate drives the pipeline with the REAL providers
// (create_zstd_compression_provider / create_blake3_hash_provider — the same
// TUs that back the persistent world save) and proves:
//   - the payload is a REAL zstd frame that SHRINKS repetitive content;
//   - the signature is a REAL BLAKE3-256 hex digest (64 chars) over the
//     exact canonical (manifest + payload) concatenation;
//   - the published episode verifies and unpacks bit-exactly through the
//     real codec (round-trip through zstd);
//   - tampering is still detected under BLAKE3 (a flipped payload byte ->
//     signature mismatch);
//   - the whole package is bit-identical across instances;
//   - the EMPTY episode still round-trips through the real codec (zstd frame
//     of empty content decompresses to empty — a boundary the identity codec
//     masked).

#include "engine/compiler/IEpisodeCompiler.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "engine/hashing/IHashProvider.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

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
using engine::compression::create_zstd_compression_provider;
using engine::hashing::create_blake3_hash_provider;

bool is_hex(const std::string& text) {
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

}  // namespace

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

EpisodeManifest two_repetitive_entries() {
    EpisodeManifest manifest;
    manifest.title = "ZstdEpisode";
    // Highly repetitive content: zstd must shrink the packed blob.
    const std::string blob = std::string(400, 'a');
    EpisodeEntry first;
    first.kind = "mission";
    first.name = "m1";
    first.json = "{\"data\":\"" + blob + "\"}";
    EpisodeEntry second;
    second.kind = "mission";
    second.name = "m2";
    second.json = "{\"data\":\"" + blob + "\",\"flags\":[\"x\",\"x\",\"x\"]}";
    manifest.entries = { first, second };
    return manifest;
}

void register_mission_hooks(engine::compiler::IEpisodeCompiler& compiler,
                            std::string& error) {
    check(compiler.register_validator(
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
          "mission validator registered");
    check(compiler.register_simulator(
              "mission",
              [](const std::string& json, int steps, std::string& trace,
                 std::string&) {
                  trace = "sim:" + std::to_string(steps) + ":" +
                          std::to_string(json.size());
                  return true;
              },
              error),
          "mission simulator registered");
    check(compiler.register_tester(
              "mission",
              [](const std::string&, std::string&) { return true; }, error),
          "mission tester registered");
}

int main() {
    std::string error;

    const auto zstd = create_zstd_compression_provider();
    const auto blake3 = create_blake3_hash_provider();
    check(zstd != nullptr && blake3 != nullptr, "providers created");

    // Wire the REAL promoted adapters through the codec/hash seams.
    auto compiler = create_episode_compiler();
    register_mission_hooks(*compiler, error);
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

    // ---- compile with the production pipeline ----
    const EpisodeManifest manifest = two_repetitive_entries();
    EpisodePackage package;
    check(compiler->compile(manifest, package, error), "compile succeeds");
    check(error.empty(), "compile diagnostic empty");

    const std::string expectedPacked =
        pack_one("mission", "m1",
                 "{\"data\":\"" + std::string(400, 'a') + "\"}") +
        pack_one("mission", "m2",
                 "{\"data\":\"" + std::string(400, 'a') +
                     "\",\"flags\":[\"x\",\"x\",\"x\"]}");
    check(package.payload.size() < expectedPacked.size(),
          "zstd SHRINKS the repetitive packed content");
    check(zstd->is_compressed(package.payload),
          "the payload is a real zstd frame");

    // The signature is a REAL BLAKE3-256 hex digest (64 chars) over the
    // exact canonical (manifest + payload) concatenation.
    check(package.signature.size() == 64 && is_hex(package.signature),
          "signature is a 64-char hex BLAKE3 digest");
    check(package.signature == blake3->hash_hex(package.manifestJson +
                                                package.payload),
          "signature equals BLAKE3(manifest + payload) — the seam is BLAKE3");
    check(package.manifestJson.find("\"digest\":\"") != std::string::npos,
          "manifest carries per-entry digests");

    // verify + unpack round-trip through the real codec.
    check(compiler->verify(package, error), "verify passes through zstd");
    check(error.empty(), "verify diagnostic empty");
    std::string content;
    check(compiler->unpack(package, content, error), "unpack passes");
    check(content == expectedPacked,
          "unpack restores the original content through zstd");

    // ---- tamper detection under BLAKE3 ----
    EpisodePackage tampered = package;
    tampered.payload[0] ^= 0x01;
    check(!compiler->verify(tampered, error) &&
              error.find("signature mismatch") != std::string::npos,
          "flipped payload byte detected under BLAKE3");

    // ---- cross-instance bit-exact determinism ----
    auto compiler2 = create_episode_compiler();
    register_mission_hooks(*compiler2, error);
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
          "the production pipeline is bit-identical across instances");

    // ---- empty episode boundary through the real codec ----
    EpisodeManifest emptyManifest;
    emptyManifest.title = "Empty";
    EpisodePackage emptyPackage;
    check(compiler->compile(emptyManifest, emptyPackage, error),
          "empty episode compiles");
    check(compiler->verify(emptyPackage, error),
          "empty episode verifies through zstd");
    std::string emptyContent;
    check(compiler->unpack(emptyPackage, emptyContent, error) &&
              emptyContent.empty(),
          "empty episode unpacks to empty through zstd");

    if (g_failures == 0) {
        std::printf("[episode-zstd-blake3] ALL PASSED\n");
        return 0;
    }
    std::printf("[episode-zstd-blake3] %d FAILURE(S)\n", g_failures);
    return 1;
}

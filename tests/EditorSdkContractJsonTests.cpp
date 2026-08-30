// EditorSdkContractJsonTests
//
// Agente 3 (fechamento_solidacao) — A3-SDK-JSON-INVALIDO. The editor's
// /sdk-contracts observable used to be serialized by hand; hostile strings
// (quotes / backslashes / newlines / Unicode control characters) or a failing
// factory produced invalid JSON (leading `,`, unescaped quotes, missing keys).
// This test feeds those exact conditions into the real serializer
// (serialize_sdk_contract_json) through nlohmann::json and requires that
// nlohmann::json::parse() NEVER throws and the round-tripped values match what
// was serialized. Standalone main() + CHECK, no Vulkan / full editor target.
//
// Fails WITHOUT the nlohmann-based serializer (manual concatenation breaks on
// the hostile strings or the all-factories-failed `{` case) and passes WITH it.

#include "EditorSdkContractJson.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

using Engine::SdkContractStats;
using Engine::serialize_sdk_contract_json;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "EditorSdkContractJsonTests failure at line " << __LINE__ \
              << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

// Every string field carries hostile characters: a double-quote, a backslash,
// a literal newline, a carriage return and a tab. Any serializer that does not
// escape these emits an unparseable document.
SdkContractStats full_stats_with_hostile_strings() {
    SdkContractStats s;
    const std::string hostile = "a\"b\\c\nd\re\tf\"\\\n";
    s.hasJobs = true;
    s.jobsSubmitted = 7;
    s.jobsCompleted = 3;
    s.jobsPending = 1;
    s.jobsHandle = 42;
    s.hasProcgen = true;
    s.procgenResult = hostile;
    s.procgenUnits = 12;
    s.procgenProgress = 8;
    s.procgenCancelled = false;
    s.procgenError = hostile;
    s.hasPreview = true;
    s.previewOk = true;
    s.previewTitle = hostile;
    s.previewLines = { hostile, "second\nline", "tab\tok" };
    s.hasFarm = true;
    s.farmKind = hostile;
    s.farmSignature = 99;
    s.farmVerified = true;
    s.hasHilbert = true;
    s.hilCellId = 1000;
    s.hilParent = 2;
    s.hilCoverCells = 5;
    s.hilDecodedX = 3;
    s.hilDecodedY = -4;
    s.hilJsonVariant = true;
    s.hilContains = true;
    s.hasBlockEntity = true;
    s.blockBooted = true;
    s.blockActive = 1;
    s.blockCompletedRuns = 6;
    s.blockFailedRuns = 0;
    s.blockLastError = hostile;
    s.blockScriptVar = 1.5;
    s.blockHasScript = true;
    s.hasParticle = true;
    s.particleBooted = true;
    s.particleHandle = 7;
    s.particleAlive = 11;
    s.hasAudio = true;
    s.audioMaster = 0.75;
    s.audioMusic = 0.5;
    s.audioSfx = 0.25;
    s.audioGainDbMaster = -3.0;
    return s;
}

bool always_parses() {
    // 1) Empty stats — models EVERY factory failing at once. Manual
    //    concatenation produced `{` with no keys (invalid); nlohmann yields `{}`.
    {
        const SdkContractStats empty;
        const std::string doc = serialize_sdk_contract_json(empty);
        try {
            const nlohmann::json parsed = nlohmann::json::parse(doc);
            CHECK(parsed.is_object());
            CHECK(parsed.empty());  // no leading-comma artifact, no partial keys
        } catch (const std::exception& e) {
            std::cerr << "empty stats unparseable: " << e.what() << "\n  doc=" << doc << "\n";
            return false;
        }
    }

    // 2) Partial failure — jobs + audio present, everything else missing.
    //    The old code wrote `,"audio":...}` with a leading comma into an empty
    //    `{` (invalid). nlohmann emits a clean 2-key object.
    {
        SdkContractStats partial;
        partial.hasJobs = true;
        partial.jobsSubmitted = 1;
        partial.jobsCompleted = 1;
        partial.jobsPending = 0;
        partial.jobsHandle = 9;
        partial.hasAudio = true;
        partial.audioMaster = 0.1;
        partial.audioMusic = 0.1;
        partial.audioSfx = 0.1;
        partial.audioGainDbMaster = 0.0;
        const std::string doc = serialize_sdk_contract_json(partial);
        try {
            const nlohmann::json parsed = nlohmann::json::parse(doc);
            CHECK(parsed.is_object());
            CHECK(parsed.size() == 2);
            CHECK(parsed.contains("jobs"));
            CHECK(parsed.contains("audio"));
            CHECK(!parsed.contains("procgen"));
            CHECK(!parsed.contains("particle"));
        } catch (const std::exception& e) {
            std::cerr << "partial stats unparseable: " << e.what() << "\n  doc=" << doc << "\n";
            return false;
        }
    }

    // 3) Every subsystem present with hostile strings — asserts BOTH that the
    //    document always parses AND that every field round-trips (escape or
    //    the parse is fine but the value is garbage).
    {
        const std::string doc = serialize_sdk_contract_json(full_stats_with_hostile_strings());
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(doc);
        } catch (const std::exception& e) {
            std::cerr << "hostile strings unparseable: " << e.what() << "\n  doc=" << doc << "\n";
            return false;
        }
        CHECK(parsed.is_object());
        CHECK(parsed.contains("jobs"));
        CHECK(parsed.contains("procgen"));
        CHECK(parsed.contains("preview"));
        CHECK(parsed.contains("farm"));
        CHECK(parsed.contains("hilbert"));
        CHECK(parsed.contains("blockEntity"));
        CHECK(parsed.contains("particle"));
        CHECK(parsed.contains("audio"));

        // Round-trip the hostile strings unchanged (nlohmann dumps escaped,
        // parse() unescapes back to the original bytes).
        const std::string hostile = "a\"b\\c\nd\re\tf\"\\\n";
        CHECK(parsed["procgen"]["error"].get<std::string>() == hostile);
        CHECK(parsed["preview"]["title"].get<std::string>() == hostile);
        CHECK(parsed["preview"]["firstLine"].get<std::string>() == hostile);
        CHECK(parsed["farm"]["kind"].get<std::string>() == hostile);
        CHECK(parsed["blockEntity"]["lastError"].get<std::string>() == hostile);
        CHECK(parsed["preview"]["rows"] == 3);
        CHECK(parsed["preview"]["firstLine"] == parsed["preview"]["title"]);
        CHECK(parsed["jobs"]["submitted"] == 7);
        CHECK(parsed["jobs"]["handle"] == 42);
        CHECK(parsed["hilbert"]["decoded"][0] == 3);
        CHECK(parsed["hilbert"]["decoded"][1] == -4);
        CHECK(parsed["hilbert"]["jsonVariant"] == true);
        CHECK(parsed["blockEntity"]["scriptVar_count"] == 1.5);
        CHECK(parsed["blockEntity"]["hasScript"] == true);
        CHECK(parsed["particle"]["handle"] == 7);
        CHECK(parsed["particle"]["alive"] == 11);
        CHECK(parsed["particle"]["booted"] == true);
        CHECK(parsed["audio"]["master"] == 0.75);
        CHECK(parsed["audio"]["music"] == 0.5);
        CHECK(parsed["audio"]["sfx"] == 0.25);
        CHECK(parsed["audio"]["gain_db_master"] == -3.0);
        CHECK(parsed["farm"]["verified"] == true);
        CHECK(parsed["farm"]["signature"] == 99);
        CHECK(parsed["blockEntity"]["completedRuns"] == 6);
    }

    return true;
}

}  // namespace

int main() {
    if (!always_parses()) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cerr << "EditorSdkContractJsonTests: all checks passed\n";
    return 0;
}
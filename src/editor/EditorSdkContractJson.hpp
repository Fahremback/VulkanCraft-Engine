// EditorSdkContractJson.hpp
//
// Agente 3 (fechamento_solidacao) — BUG A3-SDK-JSON-INVALIDO: the editor's
// `/sdk-contracts` observable was serialized by hand (`out << "{"` +
// string concatenation). If a factory fails partway (leading comma) or a
// string field contains a quote / backslash / newline / Unicode control, the
// result is objectively invalid JSON. This standalone, headless-testable
// serializer is the single place that builds the document: it feeds a plain
// aggregate of observable values through nlohmann::json and dumps() it, so
// the whole class of malformed-document bugs is removed.
//
// It intentionally depends ONLY on the standard library + nlohmann, so the
// unit test (EditorSdkContractJsonTests) compiles and runs without Vulkan /
// the full editor target.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace Engine {

struct SdkContractStats {
    // jobs
    bool hasJobs = false;
    std::uint64_t jobsSubmitted = 0;
    std::uint64_t jobsCompleted = 0;
    std::uint64_t jobsPending = 0;
    std::uint64_t jobsHandle = 0;
    // procgen
    bool hasProcgen = false;
    std::string procgenResult;      // Completed | Cancelled | Failed
    std::size_t procgenUnits = 0;
    std::size_t procgenProgress = 0;
    bool procgenCancelled = false;
    std::string procgenError;
    // preview
    bool hasPreview = false;
    bool previewOk = false;
    std::string previewTitle;
    std::vector<std::string> previewLines;  // firstLine observable derived from this
    // farm cooker
    bool hasFarm = false;
    std::string farmKind;           // RuntimeCooker | other
    std::uint64_t farmSignature = 0;
    bool farmVerified = false;
    // hilbert cell index (plain + JSON variants)
    bool hasHilbert = false;
    std::uint64_t hilCellId = 0;
    std::uint64_t hilParent = 0;
    std::size_t hilCoverCells = 0;
    std::int32_t hilDecodedX = 0;
    std::int32_t hilDecodedY = 0;
    bool hilJsonVariant = false;
    bool hilContains = false;
    // block-entity scripting
    bool hasBlockEntity = false;
    bool blockBooted = false;
    std::size_t blockActive = 0;
    std::uint64_t blockCompletedRuns = 0;
    std::uint64_t blockFailedRuns = 0;
    std::string blockLastError;
    double blockScriptVar = 0.0;
    bool blockHasScript = false;
    // particle system (same provider as the game)
    bool hasParticle = false;
    bool particleBooted = false;
    std::int32_t particleHandle = 0;
    std::int32_t particleAlive = 0;
    // audio mixer
    bool hasAudio = false;
    double audioMaster = 0.0;
    double audioMusic = 0.0;
    double audioSfx = 0.0;
    double audioGainDbMaster = 0.0;
};

// Builds the `/sdk-contracts` document exactly as the editor publishes it.
// Every present subsystem (hasJobs / hasProcgen / ...) contributes an object
// key; absent subsystems are omitted (never an empty `{` or a leading comma).
// Strings are escaped by nlohmann::json::dump(), so any user/title/error text
// — quotes, backslashes, newlines, control chars — cannot break the document.
std::string serialize_sdk_contract_json(const SdkContractStats& stats);

}  // namespace Engine
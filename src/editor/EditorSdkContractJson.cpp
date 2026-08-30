// EditorSdkContractJson.cpp
//
// Agente 3 (fechamento_solidacao) — A3-SDK-JSON-INVALIDO. The editor used to
// publish `/sdk-contracts` by hand-concatenating strings; any failing factory
// or hostile string produced invalid JSON (`{,`, missing fields, unescaped
// quotes). This TU is the single serializer: it builds a nlohmann::json tree
// (omitting whatever subsystem is absent) and dumps() it, so the output is
// ALWAYS a valid JSON document regardless of runtime failure paths.

#include "EditorSdkContractJson.hpp"

#include <nlohmann/json.hpp>

namespace Engine {

std::string serialize_sdk_contract_json(const SdkContractStats& s) {
    // Explicit object so even a fully-failed frame (every subsystem absent)
    // serializes to "{}" — a valid, empty document — never "null".
    nlohmann::json root = nlohmann::json::object();

    if (s.hasJobs) {
        root["jobs"] = {
            { "submitted", s.jobsSubmitted },
            { "completed", s.jobsCompleted },
            { "pending",   s.jobsPending },
            { "handle",    s.jobsHandle },
        };
    }
    if (s.hasProcgen) {
        root["procgen"] = {
            { "jobs",      s.procgenResult },
            { "units",     s.procgenUnits },
            { "progress",  s.procgenProgress },
            { "cancelled", s.procgenCancelled },
            { "error",     s.procgenError },
        };
    }
    if (s.hasPreview) {
        root["preview"] = {
            { "ok",        s.previewOk },
            { "title",     s.previewTitle },
            { "rows",      s.previewLines.size() },
        };
        if (s.previewLines.empty()) {
            root["preview"]["firstLine"] = std::string{};
        } else {
            root["preview"]["firstLine"] = s.previewLines.front();
        }
    }
    if (s.hasFarm) {
        root["farm"] = {
            { "kind",      s.farmKind },
            { "signature", s.farmSignature },
            { "verified",  s.farmVerified },
        };
    }
    if (s.hasHilbert) {
        root["hilbert"] = {
            { "cellId",     s.hilCellId },
            { "parent",     s.hilParent },
            { "coverCells", s.hilCoverCells },
            { "decoded",    { s.hilDecodedX, s.hilDecodedY } },
            { "jsonVariant", s.hilJsonVariant },
            { "contains",   s.hilContains },
        };
    }
    if (s.hasBlockEntity) {
        root["blockEntity"] = {
            { "booted",        s.blockBooted },
            { "active",        s.blockActive },
            { "completedRuns", s.blockCompletedRuns },
            { "failedRuns",    s.blockFailedRuns },
            { "lastError",     s.blockLastError },
            { "scriptVar_count", s.blockScriptVar },
            { "hasScript",     s.blockHasScript },
        };
    }
    if (s.hasParticle) {
        root["particle"] = {
            { "booted", s.particleBooted },
            { "handle", s.particleHandle },
            { "alive",  s.particleAlive },
        };
    }
    if (s.hasAudio) {
        root["audio"] = {
            { "master",         s.audioMaster },
            { "music",          s.audioMusic },
            { "sfx",            s.audioSfx },
            { "gain_db_master", s.audioGainDbMaster },
        };
    }

    return root.dump();
}

}  // namespace Engine
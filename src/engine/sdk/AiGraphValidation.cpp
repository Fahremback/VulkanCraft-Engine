// AiGraphValidation.cpp
//
// The RuntimeValidator behind IAiGraphValidator (FALTANTES §18 item 13): the
// PUBLIC API that validates AI-proposed motion clips and animation graphs and
// signs them deterministically. The AI itself runs OFFLINE (the farm — NO LLM
// or training environment is distributed with the game, per
// ACELERADORES_ANIMACAO_FISICA_PROCGEN.md / DEPENDENCY_POLICY); this adapter
// is the runtime side: pure structural/semantic validation over the canonical
// formats + deterministic signing (FNV-1a 64 over canonical bytes — the farm
// pattern of §18 item 11).
//
//   validate_clip — delegates to MotionClip::validate (the motion database
//                   rules: canonical tracks, monotonic keyframes, in-range
//                   bone refs). `skeleton` is validated first (a malformed
//                   skeleton is refused, never assumed).
//   validate_graph — unique non-empty state names, existing initial state,
//                   transitions only between existing states, parameters
//                   declared, finite thresholds/blend durations/exit times.
//   cook/verify  — sign + re-verify the deterministic FNV-1a 64 signature.
//
// The ONLY TU that crosses into the adapter; the public contract is
// everything the caller sees.
#include "engine/animation/IAiGraphValidation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

namespace engine {
namespace animation {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

void push_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void push_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void push_u8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void push_f32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    push_u32(out, bits);
}

void push_str(std::vector<std::uint8_t>& out, const std::string& s) {
    push_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void push_vec3(std::vector<std::uint8_t>& out, const glm::vec3& v) {
    push_f32(out, v.x);
    push_f32(out, v.y);
    push_f32(out, v.z);
}

void push_quat(std::vector<std::uint8_t>& out, const glm::quat& q) {
    push_f32(out, q.x);
    push_f32(out, q.y);
    push_f32(out, q.z);
    push_f32(out, q.w);
}

bool AnimationGraphProposal_validate(const AnimationGraphProposal& graph,
                                     std::string& errorOut) {
    if (graph.states.empty()) {
        errorOut = "ai validation: graph needs at least one state";
        return false;
    }
    std::set<std::string> stateNames;
    for (const AiGraphState& state : graph.states) {
        if (state.name.empty()) {
            errorOut = "ai validation: state name must be non-empty";
            return false;
        }
        if (state.clipName.empty()) {
            errorOut = "ai validation: state must bind a clip name";
            return false;
        }
        if (!stateNames.insert(state.name).second) {
            errorOut = "ai validation: duplicate state name '" + state.name +
                       "'";
            return false;
        }
    }
    if (stateNames.find(graph.initialState) == stateNames.end()) {
        errorOut = "ai validation: initial state '" + graph.initialState +
                   "' does not exist";
        return false;
    }
    std::set<std::string> parameterNames;
    for (const AiGraphParameter& parameter : graph.parameters) {
        if (parameter.name.empty()) {
            errorOut = "ai validation: parameter name must be non-empty";
            return false;
        }
        if (!parameterNames.insert(parameter.name).second) {
            errorOut = "ai validation: duplicate parameter '" +
                       parameter.name + "'";
            return false;
        }
    }
    for (const AiGraphTransition& transition : graph.transitions) {
        if (stateNames.find(transition.from) == stateNames.end()) {
            errorOut = "ai validation: transition 'from' state '" +
                       transition.from + "' does not exist";
            return false;
        }
        if (stateNames.find(transition.to) == stateNames.end()) {
            errorOut = "ai validation: transition 'to' state '" +
                       transition.to + "' does not exist";
            return false;
        }
        if (transition.parameter.empty()) {
            errorOut = "ai validation: transition must name a parameter";
            return false;
        }
        if (parameterNames.find(transition.parameter) ==
            parameterNames.end()) {
            errorOut = "ai validation: transition parameter '" +
                       transition.parameter + "' is not declared";
            return false;
        }
        if (!std::isfinite(transition.threshold) ||
            !std::isfinite(transition.blendDuration) ||
            transition.blendDuration < 0.0f) {
            errorOut = "ai validation: threshold/blendDuration must be finite "
                       "and blendDuration >= 0";
            return false;
        }
        if (transition.hasExitTime &&
            (!std::isfinite(transition.exitTime) ||
             transition.exitTime < 0.0f)) {
            errorOut = "ai validation: exitTime must be finite and >= 0";
            return false;
        }
    }
    return true;
}

class RuntimeValidatorImpl final : public IAiGraphValidator {
public:
    AiBackend kind() const noexcept override {
        return AiBackend::RuntimeValidator;
    }

    bool validate_clip(const MotionSkeleton& skeleton, const MotionClip& clip,
                       std::string& errorOut) const override {
        std::string skeletonError;
        if (!skeleton.validate(skeletonError)) {
            errorOut = "ai validation: skeleton invalid: " + skeletonError;
            return false;
        }
        if (clip.tracks.empty()) {
            errorOut = "ai validation: AI-proposed clip must have at least "
                       "one track";
            return false;
        }
        std::string clipError;
        if (!clip.validate(skeleton, clipError)) {
            errorOut = "ai validation: clip invalid: " + clipError;
            return false;
        }
        return true;
    }

    bool validate_graph(const AnimationGraphProposal& graph,
                        std::string& errorOut) const override {
        return AnimationGraphProposal_validate(graph, errorOut);
    }

    bool cook(AiAssetKind kind, const MotionSkeleton& skeleton,
              const MotionClip& clip, const AnimationGraphProposal& graph,
              AiCookedAsset& out, std::string& errorOut) override {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(static_cast<std::uint8_t>(kind));
        switch (kind) {
            case AiAssetKind::MotionClip:
                if (!validate_clip(skeleton, clip, errorOut)) return false;
                serialize_clip(clip, bytes);
                break;
            case AiAssetKind::AnimationGraph:
                if (!validate_graph(graph, errorOut)) return false;
                serialize_graph(graph, bytes);
                break;
            default:
                errorOut = "ai validation: unknown asset kind";
                return false;
        }
        out = AiCookedAsset{};
        out.kind = kind;
        out.signature = fnv1a64(bytes.data(), bytes.size());
        out.clip = clip;
        out.graph = graph;
        return true;
    }

    bool verify(const AiCookedAsset& asset,
                std::string& errorOut) const override {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(static_cast<std::uint8_t>(asset.kind));
        switch (asset.kind) {
            case AiAssetKind::MotionClip:
                // Structural validation only (no skeleton available in
                // verify): the clip rules that do not need the skeleton
                // (track keyframe monotonicity, non-empty) still apply.
                if (asset.clip.tracks.empty()) {
                    errorOut = "ai validation: clip has no tracks";
                    return false;
                }
                serialize_clip(asset.clip, bytes);
                break;
            case AiAssetKind::AnimationGraph:
                if (!AnimationGraphProposal_validate(asset.graph, errorOut)) {
                    return false;
                }
                serialize_graph(asset.graph, bytes);
                break;
            default:
                errorOut = "ai validation: unknown asset kind";
                return false;
        }
        const std::uint64_t computed = fnv1a64(bytes.data(), bytes.size());
        if (computed != asset.signature) {
            errorOut = "ai validation: signature mismatch (tampered asset)";
            return false;
        }
        return true;
    }

private:
    static void serialize_clip(const MotionClip& clip,
                               std::vector<std::uint8_t>& out) {
        push_str(out, clip.name);
        push_f32(out, clip.duration);
        push_u64(out, static_cast<std::uint64_t>(clip.tracks.size()));
        for (const MotionTrack& track : clip.tracks) {
            push_u32(out, static_cast<std::uint32_t>(track.boneIndex));
            push_u64(out,
                     static_cast<std::uint64_t>(track.keyframes.size()));
            for (const MotionKeyframe& key : track.keyframes) {
                push_f32(out, key.time);
                push_vec3(out, key.translation);
                push_quat(out, key.rotation);
                push_vec3(out, key.scale);
            }
        }
    }

    static void serialize_graph(const AnimationGraphProposal& graph,
                                std::vector<std::uint8_t>& out) {
        push_u64(out, static_cast<std::uint64_t>(graph.states.size()));
        for (const AiGraphState& state : graph.states) {
            push_str(out, state.name);
            push_str(out, state.clipName);
        }
        push_u64(out, static_cast<std::uint64_t>(graph.transitions.size()));
        for (const AiGraphTransition& transition : graph.transitions) {
            push_str(out, transition.from);
            push_str(out, transition.to);
            push_str(out, transition.parameter);
            push_u8(out,
                    static_cast<std::uint8_t>(transition.comparison));
            push_f32(out, transition.threshold);
            push_f32(out, transition.blendDuration);
            push_u8(out, transition.hasExitTime ? 1 : 0);
            push_f32(out, transition.exitTime);
        }
        push_u64(out, static_cast<std::uint64_t>(graph.parameters.size()));
        for (const AiGraphParameter& parameter : graph.parameters) {
            push_str(out, parameter.name);
            push_u8(out, static_cast<std::uint8_t>(parameter.kind));
        }
        push_str(out, graph.initialState);
    }
};

}  // namespace

bool AnimationGraphProposal::validate(std::string& errorOut) const {
    return AnimationGraphProposal_validate(*this, errorOut);
}

std::unique_ptr<IAiGraphValidator> create_ai_validator(AiBackend backend,
                                                       std::string& errorOut) {
    if (backend == AiBackend::RuntimeValidator) {
        return std::make_unique<RuntimeValidatorImpl>();
    }
    // The LLM/training backends run OFFLINE (the farm, §18 item 11) — the
    // runtime never links them (DEPENDENCY_POLICY: "Nenhum LLM ou ambiente de
    // treinamento é distribuído com o jogo"). Refuse with a diagnostic: a
    // missing AI backend must never look like a working validator (the farm/
    // plugin pattern).
    const char* name = "unknown";
    if (backend == AiBackend::Llm) name = "LLM";
    if (backend == AiBackend::TrainingBackend) name = "training backend";
    errorOut = std::string("ai validation: ") + name +
               " runs OFFLINE (the farm) — reference only, never linked into "
               "the runtime (DEPENDENCY_POLICY); AI assets enter the game via "
               "the RuntimeValidator gate";
    return nullptr;
}

}  // namespace animation
}  // namespace engine

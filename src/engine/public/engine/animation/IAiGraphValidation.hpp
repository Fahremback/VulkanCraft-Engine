#pragma once

// IAiGraphValidation (FALTANTES §18 item 13): the PUBLIC API for generating
// and validating animation clips and graphs BY AI. The rule of the engine
// (ACELERADORES_ANIMACAO_FISICA_PROCGEN.md / DEPENDENCY_POLICY) is: NO LLM or
// training environment is ever distributed with the game — AI runs OUTSIDE
// the runtime (offline, the farm pattern of §18 item 11), and what the game
// consumes is the VALIDATED, SIGNED asset.
//
// This contract is the runtime side of that pipeline: a public API that takes
// an AI-PROPOSED clip or animation graph (data-driven state machine) and
// either accepts it (validated all-or-nothing + deterministically signed) or
// refuses it with a diagnostic. The AI itself is a seam: `create_ai_validator`
// REFUSES any LLM/training backend with a diagnostic — the runtime never
// links one (a missing AI backend must never look like a working validator).
// `RuntimeValidator` is implemented: pure structural/semantic validation over
// the canonical formats + deterministic signing (FNV-1a 64 over canonical
// bytes — the farm pattern of §18 item 11).
//
// ANIMATION GRAPH: the data-driven state machine the engine already drives
// (AnimationStateMachine in the runtime facades) — states (each bound to a
// clip), transitions (from/to + parameter + comparison + threshold + blend),
// and declared parameters (float/bool/trigger). The validator checks the
// structure is sane BEFORE the runtime ever evaluates it: unique state names,
// a valid initial state, transitions only between existing states, parameters
// that are actually declared, finite thresholds and blend durations.
//
// CLIP: validated against a cooked skeleton (the ozz/ACL motion database
// contract — MotionClip::validate), so AI-proposed clips that reference
// unknown/out-of-order bones or malformed tracks are refused before they
// reach the sampler.
//
// Self-contained (std + glm + the public motion database types).
// Deterministic. Headless.

#include "engine/animation/IMotionDatabase.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// What the AI produced.
enum class AiAssetKind : std::uint8_t {
    MotionClip = 0,       // a canonical motion clip (IMotionDatabase types)
    AnimationGraph = 1    // a data-driven state machine
};

// The AI backend seam. RuntimeValidator is implemented; the LLM/training
// backends run OFFLINE (the farm) and are REFUSED inside the runtime.
enum class AiBackend : std::uint8_t {
    RuntimeValidator = 0,  // implemented: validate + sign (pure, headless)
    Llm = 1,               // offline LLM that drafts clips/graphs (reference only)
    TrainingBackend = 2    // offline training backend (MuJoCo/DeepMimic style)
};

// One state of the animation graph, bound to a clip by name.
struct AiGraphState {
    std::string name;
    std::string clipName;  // must reference a clip known to the caller
};

// A transition between two graph states, mirroring the runtime
// AnimationTransition (parameter must be declared in the graph).
struct AiGraphTransition {
    std::string from;
    std::string to;
    std::string parameter;  // the declared float/bool/trigger driving it
    enum class Comparison : std::uint8_t { Greater, Less, Equal, NotEqual };
    Comparison comparison{ Comparison::Greater };
    float threshold{ 0.0f };
    float blendDuration{ 0.2f };
    bool hasExitTime{ false };
    float exitTime{ 1.0f };
};

// A declared animation parameter the AI graph may reference.
enum class AiParameterKind : std::uint8_t { Float, Bool, Trigger };

struct AiGraphParameter {
    std::string name;
    AiParameterKind kind{ AiParameterKind::Float };
};

// The AI-proposed animation graph (data-driven state machine). Validated
// all-or-nothing: unique non-empty state names, an existing initial state,
// transitions only between existing states, parameters that are declared,
// finite thresholds / blend durations / exit times.
struct AnimationGraphProposal {
    std::vector<AiGraphState> states;
    std::vector<AiGraphTransition> transitions;
    std::vector<AiGraphParameter> parameters;
    std::string initialState;

    // All-or-nothing structural validation (does not require the clips to
    // exist — the caller binds them). Refuses with a diagnostic.
    bool validate(std::string& errorOut) const;
};

// A validated, signed AI asset as the runtime consumes it.
struct AiCookedAsset {
    AiAssetKind kind{ AiAssetKind::MotionClip };
    std::uint64_t signature{ 0 };
    MotionClip clip;                     // valid when kind == MotionClip
    AnimationGraphProposal graph;        // valid when kind == AnimationGraph
};

class IAiGraphValidator {
public:
    virtual ~IAiGraphValidator() = default;

    virtual AiBackend kind() const noexcept = 0;

    // Validates an AI-proposed clip against the skeleton (canonical track
    // structure, monotonic keyframes, in-range bone refs — the motion
    // database clip rules). Refuses with a diagnostic.
    virtual bool validate_clip(const MotionSkeleton& skeleton,
                               const MotionClip& clip,
                               std::string& errorOut) const = 0;

    // Validates an AI-proposed animation graph (structural rules above).
    virtual bool validate_graph(const AnimationGraphProposal& graph,
                                std::string& errorOut) const = 0;

    // Validates AND signs exactly one AI asset (the payload family matching
    // `kind`): deterministic FNV-1a 64 over the canonical kind+payload bytes.
    // Refuses (false + diagnostic) invalid graphs/clips or a clip that does
    // not pass validate_clip against `skeleton`.
    virtual bool cook(AiAssetKind kind, const MotionSkeleton& skeleton,
                      const MotionClip& clip,
                      const AnimationGraphProposal& graph,
                      AiCookedAsset& out, std::string& errorOut) = 0;

    // Verifies a cooked asset: recomputes the signature over the payload and
    // compares it to `asset.signature` (tamper/corruption gate). Also re-runs
    // the structural validation.
    virtual bool verify(const AiCookedAsset& asset,
                        std::string& errorOut) const = 0;
};

// The seam. RuntimeValidator is implemented (`src/engine/sdk/
// AiGraphValidation.cpp`). Llm / TrainingBackend run OFFLINE and are REFUSED
// with a diagnostic — an AI backend must never appear inside the runtime (the
// farm/plugin pattern: a missing backend never looks like a working
// validator).
std::unique_ptr<IAiGraphValidator> create_ai_validator(AiBackend backend,
                                                       std::string& errorOut);

}  // namespace animation
}  // namespace engine

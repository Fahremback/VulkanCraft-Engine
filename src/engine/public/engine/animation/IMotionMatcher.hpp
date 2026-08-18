#pragma once

// Public motion-matching contract (FALTANTES §18 item 9): motion matching a
// partir do código MIT (a abordagem do `sample_motion_matching` do ozz —
// referência MIT, nunca compilada, como shape-ml/minecraft-spider) e de dados
// próprios/licenciados (o gate gera clips sintéticos próprios). The matcher
// ingests annotated clips (pre-sampled local poses + per-frame root
// positions/orientations) and, given a live query (root state + future
// trajectory + current pose), returns the clip frame with the lowest weighted
// feature cost.
//
// Features (all computed RELATIVE to the character's facing at the frame, the
// classic motion-matching design):
//   - TRAJECTORY: the future root path (spec.trajectoryPoints samples at
//     (i+1)*horizon/points seconds ahead), compared in the facing frame.
//   - VELOCITY: the root linear velocity in the facing frame.
//   - POSE: the local rotations of the selected bones (poseBones; empty =
//     every bone) vs the query's current pose.
// Each feature cost is the MEAN of the per-term squared errors, so the spec
// weights are comparable across features with different term counts.
//
// PURE and DETERMINISTIC: identical (clips, spec, query) produce identical
// results (no hidden state, no RNG). The search is exhaustive with a
// deterministic tie-break (lowest clip index, then lowest frame).
//
// Self-contained (glm + the public animation headers — MotionPose/MotionSkeleton
// from IMotionDatabase.hpp). The ONLY TU that implements the contract is the
// SDK adapter (src/engine/sdk/MotionMatcher.cpp) — the adapter rule.

#include "engine/animation/IMotionDatabase.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// Matching policy. Validated all-or-nothing (refuse, never clamp).
struct MotionMatcherSpec {
    // Feature weights (>= 0).
    float trajectoryWeight{1.0f};
    float velocityWeight{1.0f};
    float poseWeight{1.0f};
    // Trajectory: number of future points (>= 1) and the horizon they span
    // (seconds, > 0). The k-th point sits (k+1)*horizon/points seconds ahead.
    int trajectoryPoints{3};
    float trajectoryHorizon{0.5f};
    // Bones used by the pose feature (canonical indices). Empty = all bones.
    std::vector<int> poseBones;

    bool validate(std::string& errorOut) const;
};

// One annotated clip fed to the matcher: pre-sampled local poses plus the
// per-frame root state (world position + orientation) used by the
// trajectory/velocity features. All vectors must be frame-sized (==
// frames.size()).
struct MotionClipEntry {
    std::string name;
    // Per-frame local poses (bone order == the canonical skeleton).
    std::vector<MotionPose> frames;
    // Per-frame root world position.
    std::vector<glm::vec3> rootPositions;
    // Per-frame root world orientation (facing; forward = +Z local).
    std::vector<glm::quat> rootOrientations;
    // Sampling rate of the frames (Hz, > 0).
    float frameRate{30.0f};
    // Whether the clip loops (trajectory wraps past the end).
    bool loop{true};

    // All-or-nothing: non-empty frames, position/orientation arrays sized to
    // the frames, frameRate > 0, every pose sized to the skeleton.
    bool validate(const MotionSkeleton& skeleton, std::string& errorOut) const;
};

// The live character state to match.
struct MotionMatchQuery {
    glm::vec3 rootPosition{0.0f};
    glm::vec3 rootVelocity{0.0f};
    glm::quat rootOrientation{1.0f, 0.0f, 0.0f, 0.0f};
    // The current local pose (sized to the skeleton; empty = pose feature
    // contributes 0).
    MotionPose pose;
    // The future root path in WORLD space, exactly
    // spec.trajectoryPoints samples at (k+1)*horizon/points seconds ahead.
    std::vector<glm::vec3> trajectory;

    bool validate(const MotionSkeleton& skeleton, const MotionMatcherSpec& spec,
                  std::string& errorOut) const;
};

// The match result: the clip index (into the build() clip order) and frame
// minimizing the weighted feature cost.
struct MotionMatchResult {
    int clip{ -1 };
    int frame{ 0 };
    float cost{ 0.0f };
};

// The motion matcher: ingests annotated clips and answers live queries with
// the best-matching clip frame. Transport- and render-free — pure animation
// data, deterministic.
class IMotionMatcher {
public:
    virtual ~IMotionMatcher() = default;

    // Ingests the clips (copied). All-or-nothing: an invalid skeleton, spec,
    // or ANY invalid clip entry refuses the whole set with a diagnostic.
    virtual bool build(const MotionSkeleton& skeleton,
                       const std::vector<MotionClipEntry>& clips,
                       const MotionMatcherSpec& spec,
                       std::string& errorOut) = 0;

    // Exhaustive search over every frame of every clip: returns the (clip,
    // frame) with the lowest weighted cost. All-or-nothing: refuses an
    // invalid query (or an empty clip set).
    virtual bool match(const MotionMatchQuery& query, MotionMatchResult& out,
                       std::string& errorOut) const = 0;

    virtual std::size_t clip_count() const noexcept = 0;
    virtual std::size_t frame_count(std::size_t clip) const noexcept = 0;
};

// The factory: builds the motion matcher (the ONLY adapter TU).
std::unique_ptr<IMotionMatcher> create_motion_matcher();

}  // namespace animation
}  // namespace engine

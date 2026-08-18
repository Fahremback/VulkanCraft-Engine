#pragma once

// IAnimationLod (FALTANTES §18 item 12): the PUBLIC animation LOD contract.
// Animation is the most expensive per-entity cost after physics, and most of
// it is spent on entities the camera barely sees. This contract turns
// RELEVANCE (0 = off-screen/trivial, 1 = on-screen/primary) into an animation
// budget:
//   - TIER SELECTION: relevance maps to one of the configured tiers (the tier
//     with the highest minRelevance <= relevance).
//   - UPDATE FREQUENCY: each tier has an updateInterval — the LOD only asks
//     the caller to re-sample the pose when the interval has elapsed. Between
//     samples the last pose is HELD (the frequency IS the LOD). This is the
//     per-entity tick-rate reduction.
//   - BONE SUBSET: each tier may list the bones it updates (empty = all).
//     Bones OUTSIDE the subset are FROZEN at their last value — the caller
//     blends the freshly-sampled subset over the held pose. This is the
//     spatial LOD (a far entity stops updating fingers/face, keeps the root
//     and the main spine/legs).
//
// The composition with the motion database (IMotionDatabase / the façades) is
// caller-side: sample() only when should_sample() says so, then apply() to
// merge the subset into the held pose. The LOD itself is PURE and
// DETERMINISTIC — it never samples, it only decides budgets — so the gate is
// a light target (no ozz/ACL needed).
//
// State is caller-owned and explicit (the IProceduralLegs::prevLegs pattern):
// the caller keeps the `AnimationLodState` (last tier, last sample time) and
// the held `MotionPose`; the LOD never hides state inside the adapter. Same
// spec + same relevance/time sequence -> identical state transitions and
// identical held poses, bit-exact, across instances.
//
// Self-contained (std + glm + the public MotionPose). Deterministic. Headless.

#include "engine/animation/IMotionDatabase.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// One animation budget tier.
struct AnimationLodTier {
    // The lowest relevance that selects this tier. Tiers must be sorted by
    // minRelevance DESCENDING (the first tier with minRelevance <= relevance
    // wins; a relevance below every minRelevance uses the last/cheapest tier).
    float minRelevance{ 0.0f };
    // Seconds between re-samples in this tier. Must be > 0. Lower relevance
    // tiers get LONGER intervals (fewer samples per second).
    float updateInterval{ 1.0f / 60.0f };
    // Canonical bone indices updated by this tier (empty = ALL bones). Bones
    // outside the subset are FROZEN at their last value by apply().
    std::vector<int> boneSubset;
};

// The full LOD configuration, validated all-or-nothing (never clamped).
struct AnimationLodSpec {
    std::vector<AnimationLodTier> tiers;

    // All-or-nothing: refuses empty tiers, non-finite/out-of-range relevance
    // thresholds or intervals, non-descending minRelevance order, and
    // out-of-range/duplicate bone indices in a subset (needs boneCount).
    bool validate(std::size_t boneCount, std::string& errorOut) const;
};

// Caller-owned LOD state (explicit, like IProceduralLegs::prevLegs).
struct AnimationLodState {
    // The active tier index (0 = highest relevance tier). The caller seeds it
    // with -1 before the first update; the LOD fills it in.
    int tierIndex{ -1 };
    // Time of the last sample (seconds). -1 = never sampled.
    float lastSampleTime{ -1.0f };
};

class IAnimationLod {
public:
    virtual ~IAnimationLod() = default;

    // Maps `relevance` in [0, 1] to a tier index: the first tier (in the
    // descending minRelevance order) with minRelevance <= relevance; the last
    // tier when relevance is below every threshold. Refuses an invalid spec
    // or relevance outside [0, 1] (all-or-nothing, never clamps).
    virtual bool select_tier(const AnimationLodSpec& spec, float relevance,
                             int& tierIndexOut, std::string& errorOut) const = 0;

    // True when the tier's updateInterval has elapsed since the last sample
    // (or nothing was sampled yet) — i.e. the caller should re-sample the
    // pose this tick. Refuses an invalid spec/state/tierIndex or a non-
    // increasing time (the LOD is deterministic: time must move forward).
    virtual bool should_sample(const AnimationLodSpec& spec,
                               const AnimationLodState& state, int tierIndex,
                               float time, std::string& errorOut) const = 0;

    // Merges a freshly-sampled `sample` into the caller-held `held` pose for
    // the tier: bones IN the tier's subset are replaced by the sample (empty
    // subset = the whole sample), bones OUTSIDE keep their held value
    // (frozen). Updates the state (tierIndex + lastSampleTime). `out` is
    // sized to the sample and written deterministically. When the tier has
    // NOT been sampled this tick (should_sample was false), apply() is a
    // no-op that returns the held pose unchanged (the caller keeps holding —
    // the frequency IS the LOD). Refuses invalid specs/states or size
    // mismatches between sample/held (all-or-nothing).
    virtual bool apply(const AnimationLodSpec& spec, int tierIndex, float time,
                       const MotionPose& sample, const MotionPose& held,
                       AnimationLodState& state, MotionPose& out,
                       std::string& errorOut) const = 0;
};

// The factory: builds the pure animation LOD adapter (src/engine/sdk/
// AnimationLod.cpp — the only TU that crosses into the adapter).
std::unique_ptr<IAnimationLod> create_animation_lod();

}  // namespace animation
}  // namespace engine

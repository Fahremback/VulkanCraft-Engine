// AnimationLod.cpp
//
// The pure animation LOD adapter behind IAnimationLod (FALTANTES §18 item 12):
// relevance -> tier budget (update frequency + bone subset). The adapter
// NEVER samples — it only decides budgets — so it is PURE and DETERMINISTIC:
// same spec + same relevance/time sequence -> identical tier transitions and
// identical held poses, bit-exact, across instances. State is caller-owned
// and explicit (the IProceduralLegs::prevLegs pattern): the caller keeps
// `AnimationLodState` and the held pose.
//
//   select_tier  — the first tier (descending minRelevance order) with
//                  minRelevance <= relevance; the last tier when relevance is
//                  below every threshold.
//   should_sample — the tier's updateInterval has elapsed since the last
//                  sample (or nothing was sampled yet).
//   apply        — merges the freshly-sampled subset into the held pose:
//                  bones IN the subset are replaced by the sample (empty
//                  subset = whole sample), bones OUTSIDE keep their held
//                  value (frozen). When the tier was not sampled this tick,
//                  apply() returns the held pose unchanged (hold — the
//                  frequency IS the LOD).
//
// The ONLY TU that crosses into the adapter; the public contract is
// everything the caller sees.
#include "engine/animation/IAnimationLod.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace engine {
namespace animation {

namespace {

constexpr float kEpsilon = 1.0e-6f;

bool AnimationLodSpec_validate(const AnimationLodSpec& spec,
                               std::size_t boneCount,
                               std::string& errorOut) {
    if (spec.tiers.empty()) {
        errorOut = "animation lod: at least one tier is required";
        return false;
    }
    for (std::size_t i = 0; i < spec.tiers.size(); ++i) {
        const AnimationLodTier& tier = spec.tiers[i];
        if (!std::isfinite(tier.minRelevance) ||
            tier.minRelevance < 0.0f || tier.minRelevance > 1.0f) {
            errorOut = "animation lod: minRelevance must be in [0, 1]";
            return false;
        }
        if (!std::isfinite(tier.updateInterval) || tier.updateInterval <= 0.0f) {
            errorOut = "animation lod: updateInterval must be finite and > 0";
            return false;
        }
        if (i > 0 && tier.minRelevance > spec.tiers[i - 1].minRelevance) {
            errorOut = "animation lod: tiers must be sorted by minRelevance "
                       "DESCENDING";
            return false;
        }
        std::set<int> seen;
        for (int bone : tier.boneSubset) {
            if (boneCount > 0 &&
                (bone < 0 || static_cast<std::size_t>(bone) >= boneCount)) {
                errorOut = "animation lod: bone subset index out of range";
                return false;
            }
            if (!seen.insert(bone).second) {
                errorOut = "animation lod: duplicate bone in the subset";
                return false;
            }
        }
    }
    return true;
}

bool AnimationLodState_valid(const AnimationLodState& state,
                             std::string& errorOut) {
    if (state.tierIndex < -1) {
        errorOut = "animation lod: tierIndex must be >= -1";
        return false;
    }
    if (state.lastSampleTime < -1.0f ||
        !std::isfinite(state.lastSampleTime)) {
        errorOut = "animation lod: invalid lastSampleTime";
        return false;
    }
    return true;
}

class AnimationLodImpl final : public IAnimationLod {
public:
    bool select_tier(const AnimationLodSpec& spec, float relevance,
                     int& tierIndexOut, std::string& errorOut) const override {
        if (!AnimationLodSpec_validate(spec, 0, errorOut)) return false;
        if (!std::isfinite(relevance) || relevance < 0.0f ||
            relevance > 1.0f) {
            errorOut = "animation lod: relevance must be in [0, 1]";
            return false;
        }
        // First tier (descending minRelevance) with minRelevance <= relevance.
        int selected = static_cast<int>(spec.tiers.size()) - 1;
        for (std::size_t i = 0; i < spec.tiers.size(); ++i) {
            if (spec.tiers[i].minRelevance <= relevance) {
                selected = static_cast<int>(i);
                break;
            }
        }
        tierIndexOut = selected;
        return true;
    }

    bool should_sample(const AnimationLodSpec& spec,
                       const AnimationLodState& state, int tierIndex,
                       float time, std::string& errorOut) const override {
        if (!AnimationLodSpec_validate(spec, 0, errorOut)) return false;
        if (!AnimationLodState_valid(state, errorOut)) return false;
        if (tierIndex < 0 ||
            static_cast<std::size_t>(tierIndex) >= spec.tiers.size()) {
            errorOut = "animation lod: tierIndex out of range";
            return false;
        }
        if (!std::isfinite(time) || time < 0.0f) {
            errorOut = "animation lod: time must be finite and >= 0";
            return false;
        }
        if (state.lastSampleTime >= 0.0f && time < state.lastSampleTime) {
            errorOut = "animation lod: time must not move backwards";
            return false;
        }
        if (state.lastSampleTime < 0.0f) return true;  // never sampled
        return (time - state.lastSampleTime) >=
               spec.tiers[tierIndex].updateInterval - kEpsilon;
    }

    bool apply(const AnimationLodSpec& spec, int tierIndex, float time,
               const MotionPose& sample, const MotionPose& held,
               AnimationLodState& state, MotionPose& out,
               std::string& errorOut) const override {
        if (!AnimationLodSpec_validate(spec, sample.translations.size(),
                                       errorOut)) {
            return false;
        }
        if (!AnimationLodState_valid(state, errorOut)) return false;
        if (tierIndex < 0 ||
            static_cast<std::size_t>(tierIndex) >= spec.tiers.size()) {
            errorOut = "animation lod: tierIndex out of range";
            return false;
        }
        if (!std::isfinite(time) || time < 0.0f) {
            errorOut = "animation lod: time must be finite and >= 0";
            return false;
        }
        if (sample.translations.size() != sample.rotations.size() ||
            sample.translations.size() != sample.scales.size()) {
            errorOut = "animation lod: sample pose must be sized to the "
                       "skeleton (trans/rot/scale)";
            return false;
        }
        if (held.translations.size() != sample.translations.size()) {
            errorOut = "animation lod: held pose size must match the sample";
            return false;
        }
        if (state.lastSampleTime >= 0.0f && time < state.lastSampleTime) {
            errorOut = "animation lod: time must not move backwards";
            return false;
        }

        const AnimationLodTier& tier = spec.tiers[tierIndex];
        const bool shouldSample = state.lastSampleTime < 0.0f ||
                                  (time - state.lastSampleTime) >=
                                      tier.updateInterval - kEpsilon;

        if (tier.boneSubset.empty()) {
            // Whole-pose tier: the fresh sample replaces the held pose.
            out = sample;
        } else if (!shouldSample) {
            // Not sampled this tick: hold the last pose (frequency IS the LOD).
            out = held;
        } else {
            // Sampled with a subset: replace the subset bones, freeze the rest.
            out = sample;
            for (std::size_t i = 0; i < sample.translations.size(); ++i) {
                const int bone = static_cast<int>(i);
                const bool inSubset =
                    std::find(tier.boneSubset.begin(), tier.boneSubset.end(),
                              bone) != tier.boneSubset.end();
                if (!inSubset) {
                    out.translations[i] = held.translations[i];
                    out.rotations[i] = held.rotations[i];
                    out.scales[i] = held.scales[i];
                }
            }
        }

        // Advance the state only when this tick actually sampled.
        if (shouldSample) {
            state.tierIndex = tierIndex;
            state.lastSampleTime = time;
        }
        return true;
    }
};

}  // namespace

bool AnimationLodSpec::validate(std::size_t boneCount,
                                std::string& errorOut) const {
    return AnimationLodSpec_validate(*this, boneCount, errorOut);
}

std::unique_ptr<IAnimationLod> create_animation_lod() {
    return std::make_unique<AnimationLodImpl>();
}

}  // namespace animation
}  // namespace engine

// AnimationLodTests (FALTANTES §18 item 12): proves the animation LOD by
// relevance — tier selection (relevance -> tier), update frequency (each tier
// re-samples at its own interval; between samples the pose is HELD), bone
// subset (bones outside the tier's subset are FROZEN at their last value),
// determinism (same spec + relevance/time sequence -> identical state and held
// poses, bit-exact, across instances) and all-or-nothing refusals. The
// adapter is PURE (it never samples — it only decides budgets), so the gate
// is a light target that feeds it synthetic MotionPoses.
#include "engine/animation/IAnimationLod.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace engine::animation;

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[anilod] FAIL: %s\n", message);
        ++g_failures;
    }
}

// A 3-bone pose whose per-bone values are derived from `time` (each call with
// a different time produces a different sample, deterministically).
MotionPose make_pose(float time, int bones = 3) {
    MotionPose pose;
    for (int b = 0; b < bones; ++b) {
        pose.translations.push_back(
            glm::vec3(time * 0.1f + static_cast<float>(b), 0.0f, 0.0f));
        pose.rotations.push_back(glm::quat(
            1.0f, 0.0f, static_cast<float>(b) * 0.01f + time * 0.001f, 0.0f));
        pose.scales.push_back(glm::vec3(1.0f));
    }
    return pose;
}

// The classic 3-tier budget: close (all bones, 60 Hz), mid (bones 0..1,
// 10 Hz), far (bone 0 only, 2 Hz).
AnimationLodSpec make_spec() {
    AnimationLodSpec spec;
    spec.tiers.push_back({0.8f, 1.0f / 60.0f, {}});          // close: all bones
    spec.tiers.push_back({0.3f, 1.0f / 10.0f, {0, 1}});      // mid: subset 0..1
    spec.tiers.push_back({0.0f, 1.0f / 2.0f, {0}});          // far: bone 0 only
    return spec;
}

void test_select_tier() {
    std::string err;
    auto lod = create_animation_lod();
    check(lod != nullptr, "tier: adapter created");
    const AnimationLodSpec spec = make_spec();
    int tier = -1;
    check(lod->select_tier(spec, 1.0f, tier, err), "tier: relevance 1 selects");
    check(tier == 0, "tier: relevance 1 -> tier 0 (close)");
    check(lod->select_tier(spec, 0.8f, tier, err), "tier: relevance 0.8 selects");
    check(tier == 0, "tier: relevance 0.8 -> tier 0 (boundary inclusive)");
    check(lod->select_tier(spec, 0.79f, tier, err), "tier: relevance 0.79 selects");
    check(tier == 1, "tier: relevance 0.79 -> tier 1 (mid)");
    check(lod->select_tier(spec, 0.3f, tier, err), "tier: relevance 0.3 selects");
    check(tier == 1, "tier: relevance 0.3 -> tier 1 (boundary inclusive)");
    check(lod->select_tier(spec, 0.29f, tier, err), "tier: relevance 0.29 selects");
    check(tier == 2, "tier: relevance 0.29 -> tier 2 (far)");
    check(lod->select_tier(spec, 0.0f, tier, err), "tier: relevance 0 selects");
    check(tier == 2, "tier: relevance 0 -> tier 2 (cheapest)");

    // Refusals: relevance out of range / invalid spec.
    std::string serr;
    check(!lod->select_tier(spec, 1.5f, tier, serr),
          "tier: relevance > 1 refused");
    check(!lod->select_tier(spec, -0.1f, tier, serr),
          "tier: negative relevance refused");
    check(!lod->select_tier(spec, std::nanf(""), tier, serr),
          "tier: NaN relevance refused");
    AnimationLodSpec empty;
    check(!lod->select_tier(empty, 0.5f, tier, serr),
          "tier: empty spec refused");
    std::printf("[anilod] tier selection + refusals OK\n");
}

void test_update_frequency() {
    std::string err;
    auto lod = create_animation_lod();
    const AnimationLodSpec spec = make_spec();

    // Close tier (60 Hz): at 60 fps every tick samples.
    {
        AnimationLodState state;
        int samples = 0;
        for (int tick = 0; tick < 60; ++tick) {
            const float t = static_cast<float>(tick) / 60.0f;
            bool sample = false;
            int tier = -1;
            check(lod->select_tier(spec, 1.0f, tier, err), "freq: select");
            check(lod->should_sample(spec, state, tier, t, err), "freq: sample");
            sample = true;
            if (sample) {
                ++samples;
                MotionPose held = make_pose(t);
                MotionPose out;
                check(lod->apply(spec, tier, t, held, held, state, out, err),
                      "freq: apply");
            }
        }
        check(samples == 60, "freq: close tier samples every tick (60/60)");
    }

    // Mid tier (10 Hz): at 60 fps samples ~6 ticks (every 6th).
    {
        AnimationLodState state;
        int samples = 0;
        int lastSampleTick = -1;
        for (int tick = 0; tick < 60; ++tick) {
            const float t = static_cast<float>(tick) / 60.0f;
            int tier = -1;
            check(lod->select_tier(spec, 0.5f, tier, err), "freq: select mid");
            if (lod->should_sample(spec, state, tier, t, err)) {
                ++samples;
                if (lastSampleTick >= 0) {
                    check(tick - lastSampleTick >= 6,
                          "freq: mid samples at most every 6 ticks");
                }
                lastSampleTick = tick;
                MotionPose sample = make_pose(t);
                MotionPose held = make_pose(t);
                MotionPose out;
                check(lod->apply(spec, tier, t, sample, held, state, out, err),
                      "freq: apply mid");
            }
        }
        // 10 Hz over 1 second = ~10 samples (first + every 6 ticks).
        check(samples >= 9 && samples <= 11,
              "freq: mid tier samples ~10x over a second");
    }

    // Far tier (2 Hz): at 60 fps samples ~2-3 ticks.
    {
        AnimationLodState state;
        int samples = 0;
        for (int tick = 0; tick < 60; ++tick) {
            const float t = static_cast<float>(tick) / 60.0f;
            int tier = -1;
            check(lod->select_tier(spec, 0.05f, tier, err), "freq: select far");
            if (lod->should_sample(spec, state, tier, t, err)) {
                ++samples;
                MotionPose sample = make_pose(t);
                MotionPose held = make_pose(t);
                MotionPose out;
                check(lod->apply(spec, tier, t, sample, held, state, out, err),
                      "freq: apply far");
            }
        }
        check(samples >= 2 && samples <= 4,
              "freq: far tier samples ~2-3x over a second");
    }
    std::printf("[anilod] update frequency by tier OK\n");
}

void test_bone_subset_freezes() {
    std::string err;
    auto lod = create_animation_lod();
    const AnimationLodSpec spec = make_spec();

    // Mid tier (subset {0,1}): bone 2 is FROZEN at its first value while the
    // tier re-samples bones 0..1. Over 10 samples the held bone 2 never moves.
    AnimationLodState state;
    MotionPose held = make_pose(0.0f);
    const glm::vec3 frozenT = held.translations[2];
    const glm::quat frozenR = held.rotations[2];
    int samples = 0;
    for (int tick = 0; tick < 120; ++tick) {
        const float t = static_cast<float>(tick) / 60.0f;
        int tier = -1;
        check(lod->select_tier(spec, 0.5f, tier, err), "subset: select mid");
        if (lod->should_sample(spec, state, tier, t, err)) {
            const MotionPose sample = make_pose(t);
            MotionPose out;
            check(lod->apply(spec, tier, t, sample, held, state, out, err),
                  "subset: apply");
            held = out;
            ++samples;
        }
    }
    check(samples >= 9, "subset: mid tier sampled several times");
    check(held.translations[2] == frozenT,
          "subset: out-of-subset bone translation FROZEN");
    check(held.rotations[2] == frozenR,
          "subset: out-of-subset bone rotation FROZEN");
    // Bones IN the subset updated: bone 0's translation tracks the last
    // sample time.
    check(held.translations[0] != frozenT,
          "subset: in-subset bone updated by the samples");

    // Far tier (subset {0}): bones 1 and 2 stay frozen.
    {
        AnimationLodState farState;
        MotionPose farHeld = make_pose(0.0f);
        const glm::vec3 frozen1 = farHeld.translations[1];
        const glm::vec3 frozen2 = farHeld.translations[2];
        for (int tick = 0; tick < 120; ++tick) {
            const float t = static_cast<float>(tick) / 60.0f;
            int tier = -1;
            check(lod->select_tier(spec, 0.05f, tier, err), "subset: select far");
            if (lod->should_sample(spec, farState, tier, t, err)) {
                const MotionPose sample = make_pose(t);
                MotionPose out;
                check(lod->apply(spec, tier, t, sample, farHeld, farState, out,
                                 err),
                      "subset: apply far");
                farHeld = out;
            }
        }
        check(farHeld.translations[1] == frozen1 &&
                  farHeld.translations[2] == frozen2,
              "subset: far tier freezes bones 1..2");
        check(farHeld.translations[0] != frozen1,
              "subset: far tier updates bone 0");
    }

    // Close tier (empty subset = ALL bones): everything updates.
    {
        AnimationLodState closeState;
        MotionPose closeHeld = make_pose(0.0f);
        for (int tick = 0; tick < 60; ++tick) {
            const float t = static_cast<float>(tick) / 60.0f;
            int tier = -1;
            check(lod->select_tier(spec, 1.0f, tier, err), "subset: select close");
            if (lod->should_sample(spec, closeState, tier, t, err)) {
                const MotionPose sample = make_pose(t);
                MotionPose out;
                check(lod->apply(spec, tier, t, sample, closeHeld, closeState,
                                 out, err),
                      "subset: apply close");
                closeHeld = out;
            }
        }
        check(closeHeld.translations[2].x > 1.0f,
              "subset: close tier updates ALL bones (bone 2 tracks time)");
    }
    std::printf("[anilod] bone-subset freeze + whole-pose tier OK\n");
}

void test_hold_between_samples() {
    std::string err;
    auto lod = create_animation_lod();
    const AnimationLodSpec spec = make_spec();

    // Between samples the pose is HELD: with a mid tier (10 Hz), the held
    // pose stays identical for the ticks between samples (frequency IS the
    // LOD — the caller stops sampling and keeps rendering the last pose).
    AnimationLodState state;
    MotionPose held = make_pose(0.0f);
    int distinctPoses = 0;
    MotionPose last;
    bool hasLast = false;
    for (int tick = 0; tick < 120; ++tick) {
        const float t = static_cast<float>(tick) / 60.0f;
        int tier = -1;
        check(lod->select_tier(spec, 0.5f, tier, err), "hold: select mid");
        if (lod->should_sample(spec, state, tier, t, err)) {
            const MotionPose sample = make_pose(t);
            MotionPose out;
            check(lod->apply(spec, tier, t, sample, held, state, out, err),
                  "hold: apply");
            held = out;
            if (hasLast && out.translations[0] != last.translations[0]) {
                ++distinctPoses;
            }
            hasLast = true;
            last = out;
        }
    }
    // ~20 samples over 2 seconds -> several distinct poses, but far fewer
    // than 120 (the intermediate ticks were held).
    check(distinctPoses >= 10 && distinctPoses <= 25,
          "hold: between samples the pose is held (not re-sampled)");
    std::printf("[anilod] hold between samples (frequency IS the LOD) OK\n");
}

void test_determinism() {
    std::string err;
    auto a = create_animation_lod();
    auto b = create_animation_lod();
    const AnimationLodSpec spec = make_spec();

    AnimationLodState stateA, stateB;
    MotionPose heldA = make_pose(0.0f);
    MotionPose heldB = make_pose(0.0f);
    bool identical = true;
    for (int tick = 0; tick < 120; ++tick) {
        const float t = static_cast<float>(tick) / 60.0f;
        int tierA = -1, tierB = -1;
        a->select_tier(spec, 0.5f, tierA, err);
        b->select_tier(spec, 0.5f, tierB, err);
        if (a->should_sample(spec, stateA, tierA, t, err)) {
            const MotionPose sample = make_pose(t);
            MotionPose outA, outB;
            a->apply(spec, tierA, t, sample, heldA, stateA, outA, err);
            b->apply(spec, tierB, t, sample, heldB, stateB, outB, err);
            heldA = outA;
            heldB = outB;
            if (outA.translations != outB.translations ||
                outA.rotations != outB.rotations ||
                stateA.tierIndex != stateB.tierIndex ||
                stateA.lastSampleTime != stateB.lastSampleTime) {
                identical = false;
            }
        }
    }
    check(identical, "determinism: two instances, same sequence -> identical");
    std::printf("[anilod] cross-instance determinism OK\n");
}

void test_validation_refusals() {
    std::string err;
    auto lod = create_animation_lod();

    // Invalid specs.
    AnimationLodSpec empty;
    check(!empty.validate(3, err), "refusal: empty spec validate fails");
    AnimationLodSpec badOrder;
    badOrder.tiers.push_back({0.3f, 0.1f, {}});
    badOrder.tiers.push_back({0.8f, 0.1f, {}});  // ascending: wrong
    check(!badOrder.validate(3, err), "refusal: non-descending order refused");
    AnimationLodSpec badInterval;
    badInterval.tiers.push_back({0.5f, 0.0f, {}});
    check(!badInterval.validate(3, err), "refusal: zero interval refused");
    AnimationLodSpec badBone;
    badBone.tiers.push_back({0.5f, 0.1f, {0, 5}});  // bone 5 > boneCount 3
    check(!badBone.validate(3, err), "refusal: out-of-range bone refused");
    AnimationLodSpec dupBone;
    dupBone.tiers.push_back({0.5f, 0.1f, {0, 0}});
    check(!dupBone.validate(3, err), "refusal: duplicate bone refused");

    // Valid spec passes.
    check(make_spec().validate(3, err), "refusal: valid spec passes validate");

    // should_sample / apply refusals.
    const AnimationLodSpec spec = make_spec();
    AnimationLodState state;
    check(!lod->should_sample(spec, state, 9, 0.1f, err),
          "refusal: out-of-range tier refused");
    check(!lod->should_sample(spec, state, 0, -1.0f, err),
          "refusal: negative time refused");
    AnimationLodState badState;
    badState.lastSampleTime = 5.0f;
    check(!lod->should_sample(spec, badState, 0, 0.5f, err),
          "refusal: time moving backwards refused");
    MotionPose sample = make_pose(0.1f, 3);
    MotionPose wrongSize = make_pose(0.1f, 2);
    MotionPose out;
    check(!lod->apply(spec, 1, 0.1f, wrongSize, sample, state, out, err),
          "refusal: sample/held size mismatch refused");
    std::printf("[anilod] all-or-nothing validation refusals OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_select_tier();
    test_update_frequency();
    test_bone_subset_freezes();
    test_hold_between_samples();
    test_determinism();
    test_validation_refusals();
    if (g_failures == 0) {
        std::printf("[anilod] ALL PASSED\n");
        return 0;
    }
    std::printf("[anilod] %d FAILURE(S)\n", g_failures);
    return 1;
}

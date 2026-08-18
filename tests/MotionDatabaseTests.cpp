// MotionDatabaseTests (FALTANTES §18 item 1): proves the fixed animation
// pipeline `ufbx/fastgltf -> ozz -> ACL -> motion database` behind the public
// IMotionDatabase façade. Canonical skeleton + clip are cooked through ozz
// (SkeletonBuilder/AnimationBuilder) and sampled; the same clip is
// uniform-resampled and compressed with ACL, and its decompressed samples must
// agree with the ozz path within compression tolerance. Also proves the
// all-or-nothing validation, the compression win (cooked bytes < source
// keyframe bytes), and restart-safety (clear()).
#include "engine/animation/IMotionDatabase.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[motion-db] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[motion-db] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

// A 5-bone armature: root -> upperArm -> forearm -> hand, plus a second child
// (root -> spine) to exercise branching.
engine::animation::MotionSkeleton make_skeleton() {
    engine::animation::MotionSkeleton sk;
    sk.name = "canonical_humanoid";
    auto addBone = [&sk](const std::string& name, int parent, glm::vec3 t,
                         glm::quat r) {
        engine::animation::MotionBone b;
        b.name = name;
        b.parent = parent;
        b.localTranslation = t;
        b.localRotation = r;
        sk.bones.push_back(b);
    };
    addBone("root", -1, glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0, 0, 0));
    addBone("upperArm", 0, glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0, 0, 0));
    addBone("forearm", 1, glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0, 0, 0));
    addBone("hand", 2, glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0, 0, 0));
    addBone("spine", 0, glm::vec3(0.0f, 1.5f, 0.0f), glm::quat(1.0f, 0, 0, 0));
    return sk;
}

// A 2-second swing clip: root translates/rotates, forearm bends, hand follows.
engine::animation::MotionClip make_clip() {
    engine::animation::MotionClip clip;
    clip.name = "swing";
    clip.duration = 2.0f;

    auto makeTrack = [&clip](int bone, std::vector<float> times,
                             std::vector<glm::vec3> tr,
                             std::vector<glm::quat> rot,
                             std::vector<glm::vec3> sc) {
        engine::animation::MotionTrack t;
        t.boneIndex = bone;
        for (std::size_t i = 0; i < times.size(); ++i) {
            engine::animation::MotionKeyframe k;
            k.time = times[i];
            k.translation = tr[i];
            k.rotation = rot[i];
            k.scale = sc[i];
            t.keyframes.push_back(k);
        }
        clip.tracks.push_back(t);
    };

    const glm::quat id = glm::quat(1.0f, 0, 0, 0);
    const glm::quat rot90Z =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    // Root: walks forward 4 units over the clip, no rotation.
    makeTrack(0, {0.0f, 1.0f, 2.0f},
              {glm::vec3(0, 0, 0), glm::vec3(2, 0, 0), glm::vec3(4, 0, 0)},
              {id, id, id}, {glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)});
    // Forearm: bends from straight to 90° around Z (a 2-second arm swing).
    makeTrack(2, {0.0f, 2.0f}, {glm::vec3(0, 0, 0), glm::vec3(0, 0, 0)},
              {id, rot90Z}, {glm::vec3(1.0f), glm::vec3(1.0f)});
    // Hand: follows the forearm bend (so ACL has rotation to compress).
    makeTrack(3, {0.0f, 2.0f}, {glm::vec3(0, 0, 0), glm::vec3(0, 0, 0)},
              {id, rot90Z}, {glm::vec3(1.0f), glm::vec3(1.0f)});
    // Spine: stretches (scale 1 -> 1.5) to give the compressor a scale track.
    makeTrack(4, {0.0f, 2.0f}, {glm::vec3(0, 0, 0), glm::vec3(0, 0, 0)},
              {id, id}, {glm::vec3(1.0f), glm::vec3(1.5f)});
    return clip;
}

// Direct keyframe interpolation (the "legacy sampler" reference: linear
// translation/scale, slerp rotation).
engine::animation::MotionPose referencePose(const engine::animation::MotionSkeleton& sk,
                                            const engine::animation::MotionClip& clip,
                                            float time) {
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    for (const auto& track : clip.tracks) {
        const auto& keys = track.keyframes;
        if (keys.empty()) {
            continue;
        }
        const int idx = track.boneIndex;
        if (time <= keys.front().time) {
            pose.translations[idx] = keys.front().translation;
            pose.rotations[idx] = keys.front().rotation;
            pose.scales[idx] = keys.front().scale;
            continue;
        }
        if (time >= keys.back().time) {
            pose.translations[idx] = keys.back().translation;
            pose.rotations[idx] = keys.back().rotation;
            pose.scales[idx] = keys.back().scale;
            continue;
        }
        for (std::size_t i = 1; i < keys.size(); ++i) {
            if (time <= keys[i].time) {
                const float span = keys[i].time - keys[i - 1].time;
                const float t =
                    span > 0.0f ? (time - keys[i - 1].time) / span : 0.0f;
                pose.translations[idx] =
                    glm::mix(keys[i - 1].translation, keys[i].translation, t);
                pose.rotations[idx] =
                    glm::slerp(keys[i - 1].rotation, keys[i].rotation, t);
                pose.scales[idx] =
                    glm::mix(keys[i - 1].scale, keys[i].scale, t);
                break;
            }
        }
    }
    return pose;
}

void test_validation() {
    auto db = engine::animation::create_motion_database();
    check(db != nullptr, "factory creates the database");

    std::string err;
    // Empty skeleton refused.
    engine::animation::MotionSkeleton empty;
    check(!db->cook_skeleton(empty, err), "empty skeleton refused");
    check(!err.empty(), "empty skeleton refusal carries a diagnostic");

    // Misordered parent refused.
    engine::animation::MotionSkeleton bad = make_skeleton();
    bad.bones[0].parent = 3;  // root referencing a later bone
    check(!db->cook_skeleton(bad, err), "misordered parent refused");

    // Cook a good skeleton; cook_clip before skeleton is refused.
    engine::animation::MotionSkeleton sk = make_skeleton();
    check(db->cook_skeleton(sk, err), "valid skeleton cooks");
    check(db->bone_count() == sk.bones.size(), "bone_count matches");

    auto db2 = engine::animation::create_motion_database();
    engine::animation::MotionClip clip = make_clip();
    check(!db2->cook_clip(clip, err), "clip refused without cooked skeleton");

    // Invalid clip: negative duration.
    engine::animation::MotionClip neg = make_clip();
    neg.duration = -1.0f;
    check(!db->cook_clip(neg, err), "negative duration refused");

    // Invalid clip: non-monotonic keyframe time (equal to the previous one).
    engine::animation::MotionClip mono = make_clip();
    mono.tracks[0].keyframes[1].time = 0.0f;
    check(!db->cook_clip(mono, err), "non-monotonic keyframes refused");

    // Invalid clip: out-of-range bone ref.
    engine::animation::MotionClip oor = make_clip();
    oor.tracks[0].boneIndex = 99;
    check(!db->cook_clip(oor, err), "out-of-range bone ref refused");

    std::printf("[motion-db] all-or-nothing validation (skeleton/clip) OK\n");
}

void test_ozz_cook_and_sample() {
    auto db = engine::animation::create_motion_database();
    std::string err;
    engine::animation::MotionSkeleton sk = make_skeleton();
    check(db->cook_skeleton(sk, err), "skeleton cooks");
    engine::animation::MotionClip clip = make_clip();
    check(db->cook_clip(clip, err), "clip cooks through ozz");

    const engine::animation::CookedMotion* motion = db->cooked("swing");
    check(motion != nullptr, "cooked() finds the clip");
    check(motion != nullptr && motion->kind ==
              engine::animation::CookedMotion::Kind::OzzAnimation,
          "cooked clip is the ozz runtime animation");
    check(motion != nullptr && motion->bytes > 0,
          "cooked clip reports bytes");

    // Unknown clip refused.
    check(db->cooked("nope") == nullptr, "unknown clip returns nullptr");

    // Sample at t=0: equals the first keyframes exactly.
    engine::animation::MotionPose pose0;
    check(db->sample(*motion, 0.0f, pose0), "sample at t=0");
    check(pose0.translations.size() == sk.bones.size(),
          "pose has one transform per bone");
    checkClose(pose0.translations[0].x, 0.0f, 1e-5f, "t=0 root x");
    checkClose(pose0.rotations[2].w, 1.0f, 1e-5f, "t=0 forearm identity");

    // Sample at t=2: root at x=4 (moves along X), forearm rotated 90°.
    engine::animation::MotionPose pose2;
    check(db->sample(*motion, 2.0f, pose2), "sample at t=duration");
    // ozz stores keyframes as 16-bit halves: 4.0 quantizes to ~3.9995.
    checkClose(pose2.translations[0].x, 4.0f, 1e-3f, "t=2 root x=4");
    const glm::quat rot90Z =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const float dot = glm::dot(pose2.rotations[2], rot90Z);
    check(dot > 0.999f, "t=2 forearm rotated 90°");

    // Midpoint interpolation vs the reference (legacy) sampler.
    const float times[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f};
    bool equivalent = true;
    for (float t : times) {
        engine::animation::MotionPose p;
        check(db->sample(*motion, t, p), "midpoint sample");
        const engine::animation::MotionPose ref =
            referencePose(sk, clip, t);
        for (std::size_t b = 0; b < sk.bones.size(); ++b) {
            const float te = glm::length(p.translations[b] - ref.translations[b]);
            const float se = glm::length(p.scales[b] - ref.scales[b]);
            if (te > 1e-3f || se > 1e-3f) {
                equivalent = false;
            }
        }
    }
    check(equivalent, "ozz sampling matches the reference interpolation");

    // Determinism: sampling the same time twice gives bit-identical poses.
    engine::animation::MotionPose again;
    check(db->sample(*motion, 0.5f, again), "sample again");
    engine::animation::MotionPose third;
    check(db->sample(*motion, 0.5f, third), "sample third");
    bool same = true;
    for (std::size_t b = 0; b < sk.bones.size(); ++b) {
        if (again.rotations[b] != third.rotations[b] ||
            again.translations[b] != third.translations[b] ||
            again.scales[b] != third.scales[b]) {
            same = false;
        }
    }
    check(same, "sampling is deterministic (bit-exact)");
    (void)pose0;

    std::printf("[motion-db] ozz cook + sample (reference equivalence, "
                "determinism) OK\n");
}

void test_acl_compression_equivalence() {
    auto db = engine::animation::create_motion_database();
    std::string err;
    engine::animation::MotionSkeleton sk = make_skeleton();
    check(db->cook_skeleton(sk, err), "skeleton cooks");
    engine::animation::MotionClip clip = make_clip();
    check(db->cook_clip(clip, err), "clip cooks through ozz");
    const engine::animation::CookedMotion* ozz = db->cooked("swing");
    check(ozz != nullptr &&
              ozz->kind == engine::animation::CookedMotion::Kind::OzzAnimation,
          "ozz clip cooked");

    // Compression win: the uncompressed uniform-resampled source (the same
    // resample the adapter feeds ACL) vs the ACL cooked bytes. The resample
    // is deterministic: rate = max(30, ceil((maxKeys-1)/duration)), samples =
    // floor(duration*rate)+1 (mirrors the adapter contract).
    std::size_t maxKeys = 1;
    for (const auto& t : clip.tracks) {
        maxKeys = std::max(maxKeys, t.keyframes.size());
    }
    const float rate =
        std::max(30.0f, std::ceil(static_cast<float>(maxKeys - 1) /
                                  std::max(clip.duration, 0.0001f)));
    const std::size_t numSamples =
        static_cast<std::size_t>(std::floor(clip.duration * rate) + 1);
    const std::size_t sourceBytes =
        sk.bones.size() * numSamples * sizeof(glm::mat4);  // >= qvvf size
    // Compress under a distinct name so both pipeline paths are addressable
    // through cooked() (which returns the most recently cooked version).
    engine::animation::MotionClip aclClip = clip;
    aclClip.name = "swing_acl";
    check(db->compress_clip(aclClip, err), "clip compresses with ACL");
    const engine::animation::CookedMotion* acl =
        db->cooked("swing_acl");
    check(acl != nullptr && acl->kind ==
              engine::animation::CookedMotion::Kind::AclCompressed,
          "compressed clip is the ACL-compressed version");
    check(acl != nullptr && acl->bytes > 0, "compressed clip reports bytes");
    check(acl != nullptr && acl->bytes < sourceBytes,
          "compression win: ACL bytes < source keyframe bytes");

    // Equivalence: ACL decompressed samples agree with the ozz path within
    // compression tolerance.
    const float times[] = {0.0f, 0.1f, 0.25f, 0.5f, 1.0f, 1.5f, 1.9f, 2.0f};
    bool equivalent = true;
    float maxPosErr = 0.0f;
    float maxRotErr = 0.0f;
    for (float t : times) {
        engine::animation::MotionPose pAcl;
        check(db->sample(*acl, t, pAcl), "ACL sample");
        engine::animation::MotionPose pOzz;
        check(db->sample(*ozz, t, pOzz), "ozz sample");
        for (std::size_t b = 0; b < sk.bones.size(); ++b) {
            const float pe =
                glm::length(pAcl.translations[b] - pOzz.translations[b]);
            const float se =
                glm::length(pAcl.scales[b] - pOzz.scales[b]);
            maxPosErr = std::max(maxPosErr, std::max(pe, se));
            const float d = glm::dot(pAcl.rotations[b], pOzz.rotations[b]);
            maxRotErr = std::max(maxRotErr, 1.0f - std::fabs(d));
            if (pe > 1e-2f || se > 1e-2f || (1.0f - std::fabs(d)) > 1e-3f) {
                equivalent = false;
            }
        }
    }
    check(equivalent, "ACL decompressed samples match ozz within tolerance");
    std::printf("[motion-db] ACL equivalence max pos err %.5f, max rot err "
                "%.6f (tolerance 1e-2 / 1e-3)\n",
                maxPosErr, maxRotErr);
    (void)maxPosErr;
    (void)maxRotErr;
}

void test_restart_safety() {
    auto db = engine::animation::create_motion_database();
    std::string err;
    engine::animation::MotionSkeleton sk = make_skeleton();
    check(db->cook_skeleton(sk, err), "skeleton cooks");
    engine::animation::MotionClip clip = make_clip();
    check(db->cook_clip(clip, err), "clip cooks");
    check(db->cooked("swing") != nullptr, "clip present");

    // A second skeleton replaces the first; the old clip is still there but
    // sampling with mismatched bone counts must not crash.
    db->clear();
    check(db->bone_count() == 0, "clear resets bone count");
    check(db->cooked("swing") == nullptr, "clear drops cooked clips");
    check(db->cook_skeleton(sk, err), "skeleton cooks again after clear");
    check(db->cook_clip(clip, err), "clip cooks again after clear");

    // Third run behaves identically (restart-safety).
    engine::animation::MotionPose p;
    const engine::animation::CookedMotion* m = db->cooked("swing");
    check(m != nullptr && db->sample(*m, 1.0f, p), "post-clear sample works");
    checkClose(p.translations[0].x, 2.0f, 1e-4f, "post-clear sample value");

    std::printf("[motion-db] restart-safety (clear + recook) OK\n");
}

// FALTANTES §18 item 2: the database also performs 2-pose blending through
// the ozz BlendingJob (SoA; lerp translations/scales, nlerp rotations). The
// engine's AnimationBlender::blend routes here. `count` 6 exercises the
// 2-group SoA packing (5 real + 1 padded lane).
void test_blend_poses() {
    auto db = engine::animation::create_motion_database();
    // Blending needs no cooked skeleton — the poses carry their own size.
    const std::size_t count = 6;
    engine::animation::MotionPose a, b;
    a.translations.resize(count);
    a.rotations.resize(count);
    a.scales.resize(count);
    b.translations.resize(count);
    b.rotations.resize(count);
    b.scales.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        a.translations[i] = glm::vec3(static_cast<float>(i), 0.0f, 0.0f);
        a.rotations[i] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        a.scales[i] = glm::vec3(1.0f);
        b.translations[i] =
            glm::vec3(2.0f * static_cast<float>(i), 3.0f, 4.0f);
        b.rotations[i] =
            glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        b.scales[i] = glm::vec3(2.0f);
    }
    for (float w : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        engine::animation::MotionPose out;
        check(db->blend_poses(a, b, w, out), "blend succeeds");
        check(out.translations.size() == count, "blend output size");
        for (std::size_t i = 0; i < count; ++i) {
            const glm::vec3 et = glm::mix(a.translations[i], b.translations[i], w);
            checkClose(out.translations[i].x, et.x, 1e-4f, "blend translation x");
            checkClose(out.translations[i].y, et.y, 1e-4f, "blend translation y");
            checkClose(out.translations[i].z, et.z, 1e-4f, "blend translation z");
            // ozz BlendingJob normalizes the lerped quaternion (nlerp).
            const glm::quat er = glm::normalize(glm::lerp(a.rotations[i], b.rotations[i], w));
            const float dot = glm::dot(out.rotations[i], er);
            checkClose(std::fabs(dot), 1.0f, 1e-4f, "blend rotation (nlerp)");
            const glm::vec3 es = glm::mix(a.scales[i], b.scales[i], w);
            checkClose(out.scales[i].x, es.x, 1e-4f, "blend scale");
        }
    }
    // Refusals (all-or-nothing): empty and size-mismatched poses.
    engine::animation::MotionPose empty, emptyOut;
    check(!db->blend_poses(empty, b, 0.5f, emptyOut), "empty pose refused");
    engine::animation::MotionPose shortB;
    shortB.translations.resize(1);
    shortB.rotations.resize(1);
    shortB.scales.resize(1);
    check(!db->blend_poses(a, shortB, 0.5f, emptyOut), "mismatched poses refused");
    // Determinism: identical inputs produce bit-identical output.
    engine::animation::MotionPose out1, out2;
    check(db->blend_poses(a, b, 0.3f, out1) && db->blend_poses(a, b, 0.3f, out2),
          "blend determinism runs");
    check(out1.translations == out2.translations &&
              out1.rotations == out2.rotations && out1.scales == out2.scales,
          "blend deterministic bit-exact");
    std::printf("[motion-db] blend_poses (ozz BlendingJob) OK\n");
}

// FALTANTES §18 item 3: IK through the ozz jobs (IKTwoBoneJob / IKAimJob)
// behind the database. The jobs take model-space matrices and output
// local-space correction quaternions applied as `local * correction`.
glm::vec3 modelPosition(const engine::animation::MotionPose& pose,
                        const engine::animation::MotionSkeleton& sk,
                        int bone) {
    std::vector<glm::mat4> models(sk.bones.size(), glm::mat4(1.0f));
    for (std::size_t i = 0; i < sk.bones.size(); ++i) {
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0f), pose.translations[i]) *
            glm::mat4_cast(pose.rotations[i]) *
            glm::scale(glm::mat4(1.0f), pose.scales[i]);
        const int p = sk.bones[i].parent;
        models[i] = p >= 0 ? models[static_cast<std::size_t>(p)] * local : local;
    }
    return glm::vec3(models[static_cast<std::size_t>(bone)][3]);
}

glm::quat modelRotation(const engine::animation::MotionPose& pose,
                        const engine::animation::MotionSkeleton& sk,
                        int bone) {
    std::vector<glm::quat> rots(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    for (std::size_t i = 0; i < sk.bones.size(); ++i) {
        const int p = sk.bones[i].parent;
        rots[i] = p >= 0 ? rots[static_cast<std::size_t>(p)] * pose.rotations[i]
                         : pose.rotations[i];
    }
    return rots[static_cast<std::size_t>(bone)];
}

void test_ik() {
    auto db = engine::animation::create_motion_database();
    std::string err;
    const engine::animation::MotionSkeleton sk = make_skeleton();
    check(db->cook_skeleton(sk, err), "IK: skeleton cooks");

    engine::animation::MotionPose pose;
    pose.translations = {{0,0,0}, {0,1,0}, {0,1,0}, {0,0.5f,0}, {0,1.5f,0}};
    pose.rotations.resize(5, glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(5, glm::vec3(1.0f));
    // Model positions: root (0,0,0), upperArm (0,1,0), forearm (0,2,0),
    // hand (0,2.5,0), spine (0,1.5,0).
    const glm::vec3 restHand = glm::vec3(0.0f, 2.5f, 0.0f);
    check(glm::distance(modelPosition(pose, sk, 3), restHand) < 1e-4f,
          "IK: rest hand position");

    // Two-bone chain root(0) -> upperArm(1) -> hand(3): weight 1 reaches the
    // model-space target analytically.
    const glm::vec3 target(1.5f, 1.0f, 0.0f);
    engine::animation::MotionPose out;
    check(db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 1.0f, out),
          "IK: two-bone runs");
    check(glm::distance(modelPosition(out, sk, 3), target) < 0.02f,
          "IK: hand reaches target");

    // Weight 0 leaves the pose untouched.
    engine::animation::MotionPose zero;
    check(db->ik_two_bone(pose, 0, 1, 3, glm::vec3(3, 0, 0), glm::vec3(0, 0, 1),
                          0.0f, zero),
          "IK: weight 0 runs");
    check(zero.translations == pose.translations &&
              zero.rotations == pose.rotations && zero.scales == pose.scales,
          "IK: weight 0 unchanged");

    // Weight 0.5 moves the hand toward (but not onto) the target.
    engine::animation::MotionPose half;
    check(db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 0.5f, half),
          "IK: weight 0.5 runs");
    const glm::vec3 halfHand = modelPosition(half, sk, 3);
    check(glm::distance(halfHand, target) <
              glm::distance(restHand, target) - 0.05f,
          "IK: weight 0.5 moves toward target");

    // Pole vector controls the bend side: +Z vs -Z flips the mid joint side.
    engine::animation::MotionPose poleP, poleN;
    check(db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 1.0f, poleP) &&
              db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, -1), 1.0f, poleN),
          "IK: pole runs");
    check(modelPosition(poleP, sk, 1).z > 0.05f && modelPosition(poleN, sk, 1).z < -0.05f,
          "IK: pole flips bend side");

    // Determinism: identical inputs -> bit-identical output.
    engine::animation::MotionPose d1, d2;
    check(db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 0.7f, d1) &&
              db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 0.7f, d2),
          "IK: determinism runs");
    check(d1.translations == d2.translations && d1.rotations == d2.rotations &&
              d1.scales == d2.scales,
          "IK: deterministic bit-exact");

    // Aim: rotate the hand so its local +X points at a model-space target.
    const glm::vec3 aimTarget(2.5f, 3.0f, 0.0f);
    engine::animation::MotionPose aimed;
    check(db->ik_aim(pose, 3, aimTarget, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                     glm::vec3(0, 1, 0), 1.0f, aimed),
          "IK: aim runs");
    const glm::vec3 forward =
        glm::normalize(modelRotation(aimed, sk, 3) * glm::vec3(1, 0, 0));
    const glm::vec3 wantDir =
        glm::normalize(aimTarget - restHand);
    check(glm::distance(forward, wantDir) < 0.05f, "IK: aim direction");
    // Aim weight 0 leaves the pose untouched.
    engine::animation::MotionPose aimedZero;
    check(db->ik_aim(pose, 3, aimTarget, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                     glm::vec3(0, 1, 0), 0.0f, aimedZero),
          "IK: aim weight 0 runs");
    // Weight 0 -> identity correction. ozz normalizes the lerp with an
    // estimate (NormalizeEst4 -> x86 rsqrtps), whose max relative error
    // (~1.5e-4) makes the identity come back as w = 1 - 2^-13 — a real ozz
    // property, so compare with a 1e-3 tolerance, not bit-exact.
    bool aimUnchanged = true;
    for (std::size_t i = 0; i < pose.rotations.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(aimedZero.rotations[i][c] - pose.rotations[i][c]) >
                1e-3f) {
                aimUnchanged = false;
            }
        }
    }
    check(aimUnchanged, "IK: aim weight 0 unchanged");

    // Refusals (all-or-nothing).
    auto fresh = engine::animation::create_motion_database();
    engine::animation::MotionPose dummy;
    check(!fresh->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 1.0f, dummy),
          "IK: no cooked skeleton refused");
    engine::animation::MotionPose empty;
    check(!db->ik_two_bone(empty, 0, 1, 3, target, glm::vec3(0, 0, 1), 1.0f, dummy),
          "IK: empty pose refused");
    engine::animation::MotionPose mism;
    mism.translations.resize(4);
    mism.rotations.resize(4);
    mism.scales.resize(4);
    check(!db->ik_two_bone(mism, 0, 1, 3, target, glm::vec3(0, 0, 1), 1.0f, dummy),
          "IK: mismatched pose refused");
    check(!db->ik_two_bone(pose, 0, 0, 3, target, glm::vec3(0, 0, 1), 1.0f, dummy),
          "IK: root==mid refused");
    check(!db->ik_two_bone(pose, 0, 1, 9, target, glm::vec3(0, 0, 1), 1.0f, dummy),
          "IK: out-of-range refused");
    // spine (4) is NOT a descendant of upperArm (1): non-ancestor chain.
    check(!db->ik_two_bone(pose, 0, 1, 4, target, glm::vec3(0, 0, 1), 1.0f, dummy),
          "IK: non-ancestor chain refused");
    check(!db->ik_two_bone(pose, 0, 1, 3, target, glm::vec3(0, 0, 1), 1.5f, dummy),
          "IK: weight > 1 refused");
    check(!db->ik_aim(pose, 9, aimTarget, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                      glm::vec3(0, 1, 0), 1.0f, dummy),
          "IK: aim out-of-range refused");
    check(!db->ik_aim(pose, 3, aimTarget, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
                      glm::vec3(0, 1, 0), 1.0f, dummy),
          "IK: aim degenerate forward refused");
    std::printf("[motion-db] IK (ozz IKTwoBoneJob/IKAimJob) OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_validation();
    test_ozz_cook_and_sample();
    test_acl_compression_equivalence();
    test_restart_safety();
    test_blend_poses();
    test_ik();
    if (g_failures == 0) {
        std::printf("[motion-db] ALL PASSED\n");
        return 0;
    }
    std::printf("[motion-db] %d FAILURE(S)\n", g_failures);
    return 1;
}

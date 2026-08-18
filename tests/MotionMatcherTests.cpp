// MotionMatcherTests (FALTANTES §18 item 9): proves the public motion
// matching contract `IMotionMatcher` — feature vector (trajectory + velocity +
// pose, all relative to the facing) with an exhaustive deterministic search.
// The approach follows the MIT-licensed ozz `sample_motion_matching` as a
// REFERENCE only (never compiled, like shape-ml/minecraft-spider); the clips
// are OWN synthetic data (closed-form, no RNG) — the "dados próprios" of the
// item. The matcher is pure and deterministic: identical inputs produce
// bit-identical results.
#include "engine/animation/IMotionMatcher.hpp"

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
        std::printf("[mm] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[mm] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

// Canonical skeleton: root -> spine -> head; root -> thighL -> shinL;
// root -> thighR -> shinR (children after parents).
MotionSkeleton make_skeleton() {
    MotionSkeleton sk;
    sk.name = "humanoid";
    auto add = [&sk](const std::string& name, int parent, glm::vec3 t) {
        MotionBone b;
        b.name = name;
        b.parent = parent;
        b.localTranslation = t;
        sk.bones.push_back(b);
    };
    add("root", -1, glm::vec3(0));
    add("spine", 0, glm::vec3(0, 1.0f, 0));
    add("head", 1, glm::vec3(0, 0.5f, 0));
    add("thighL", 0, glm::vec3(0.15f, 0.9f, 0));
    add("shinL", 3, glm::vec3(0, -0.5f, 0));
    add("thighR", 0, glm::vec3(-0.15f, 0.9f, 0));
    add("shinR", 5, glm::vec3(0, -0.5f, 0));
    return sk;
}

const MotionSkeleton& skeleton() {
    static const MotionSkeleton sk = make_skeleton();
    return sk;
}

// One pose with the skeleton's rest translations and the given per-bone
// rotations (identity unless animated).
MotionPose make_pose(const glm::quat& spine, const glm::quat& head,
                     const glm::quat& thighL, const glm::quat& shinL,
                     const glm::quat& thighR, const glm::quat& shinR) {
    MotionPose pose;
    pose.translations.resize(7, glm::vec3(0.0f));
    pose.rotations.resize(7, glm::quat(1, 0, 0, 0));
    pose.scales.resize(7, glm::vec3(1.0f));
    for (std::size_t i = 0; i < skeleton().bones.size(); ++i) {
        pose.translations[i] = skeleton().bones[i].localTranslation;
    }
    pose.rotations[1] = spine;
    pose.rotations[2] = head;
    pose.rotations[3] = thighL;
    pose.rotations[4] = shinL;
    pose.rotations[5] = thighR;
    pose.rotations[6] = shinR;
    return pose;
}

// A locomotive clip: the root advances along `axis` at `speed` m/s with a
// vertical bob; the legs swing at `stepHz` with `swing` amplitude (left/right
// out of phase). Closed-form — deterministic, no RNG.
MotionClipEntry make_lococlip(const std::string& name, int frames, float rate,
                              const glm::vec3& axis, float speed, float bob,
                              float stepHz, float swing) {
    MotionClipEntry clip;
    clip.name = name;
    clip.frameRate = rate;
    clip.loop = true;
    clip.frames.reserve(static_cast<std::size_t>(frames));
    clip.rootPositions.reserve(static_cast<std::size_t>(frames));
    clip.rootOrientations.assign(static_cast<std::size_t>(frames),
                                 glm::quat(1, 0, 0, 0));
    const float pi = glm::pi<float>();
    for (int f = 0; f < frames; ++f) {
        const float t = static_cast<float>(f) / rate;
        const glm::vec3 pos = axis * (speed * t) +
                              glm::vec3(0, bob * std::sin(2 * pi * 2 * stepHz * t), 0);
        clip.rootPositions.push_back(pos);
        const glm::quat thighL =
            glm::angleAxis(swing * std::sin(2 * pi * stepHz * t), glm::vec3(1, 0, 0));
        const glm::quat shinL =
            glm::angleAxis(-0.8f * swing * std::sin(2 * pi * stepHz * t),
                           glm::vec3(1, 0, 0));
        const glm::quat thighR =
            glm::angleAxis(swing * std::sin(2 * pi * stepHz * t + pi),
                           glm::vec3(1, 0, 0));
        const glm::quat shinR =
            glm::angleAxis(-0.8f * swing * std::sin(2 * pi * stepHz * t + pi),
                           glm::vec3(1, 0, 0));
        clip.frames.push_back(
            make_pose(glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0), thighL,
                      shinL, thighR, shinR));
    }
    return clip;
}

// An idle clip: stationary root, optional leg bend (crouch).
MotionClipEntry make_idle(const std::string& name, int frames, float rate,
                          float bend) {
    MotionClipEntry clip;
    clip.name = name;
    clip.frameRate = rate;
    clip.loop = true;
    clip.frames.reserve(static_cast<std::size_t>(frames));
    clip.rootPositions.assign(static_cast<std::size_t>(frames), glm::vec3(0));
    clip.rootOrientations.assign(static_cast<std::size_t>(frames),
                                 glm::quat(1, 0, 0, 0));
    const glm::quat thigh = glm::angleAxis(bend, glm::vec3(1, 0, 0));
    const glm::quat shin = glm::angleAxis(-1.8f * bend, glm::vec3(1, 0, 0));
    for (int f = 0; f < frames; ++f) {
        clip.frames.push_back(make_pose(glm::quat(1, 0, 0, 0),
                                        glm::quat(1, 0, 0, 0), thigh, shin,
                                        thigh, shin));
    }
    return clip;
}

// The trajectory a clip frame implies: root positions at (k+1)*horizon/points
// seconds ahead (wrapped), world space — what a caller feeds as the query.
std::vector<glm::vec3> clip_trajectory(const MotionClipEntry& clip, int f,
                                       int points, float horizon) {
    std::vector<glm::vec3> traj(static_cast<std::size_t>(points));
    const int frames = static_cast<int>(clip.frames.size());
    for (int i = 0; i < points; ++i) {
        const int idx = static_cast<int>(std::lround(
            static_cast<float>(i + 1) * horizon / static_cast<float>(points) *
            clip.frameRate));
        traj[static_cast<std::size_t>(i)] =
            clip.rootPositions[static_cast<std::size_t>((f + idx) % frames)];
    }
    return traj;
}

void test_validation() {
    std::string err;
    MotionMatcherSpec spec;

    spec.trajectoryWeight = -1.0f;
    check(!spec.validate(err), "validation: negative trajectoryWeight refused");
    spec = MotionMatcherSpec();
    spec.velocityWeight = -0.5f;
    check(!spec.validate(err), "validation: negative velocityWeight refused");
    spec = MotionMatcherSpec();
    spec.trajectoryPoints = 0;
    check(!spec.validate(err), "validation: zero trajectoryPoints refused");
    spec = MotionMatcherSpec();
    spec.trajectoryHorizon = 0.0f;
    check(!spec.validate(err), "validation: zero horizon refused");
    spec = MotionMatcherSpec();
    spec.poseBones = {-1};
    check(!spec.validate(err), "validation: negative poseBone refused");
    spec = MotionMatcherSpec();
    check(spec.validate(err), "validation: default spec is valid");

    MotionClipEntry clip = make_idle("bad", 30, 30.0f, 0.0f);
    clip.rootPositions.clear();  // size mismatch
    auto matcher = create_motion_matcher();
    std::vector<MotionClipEntry> clips = {clip};
    check(!matcher->build(skeleton(), clips, spec, err),
          "validation: root array size mismatch refuses");
    clips = {make_idle("bad2", 30, 0.0f, 0.0f)};
    check(!matcher->build(skeleton(), clips, spec, err),
          "validation: zero frameRate refuses");
    clips = {make_idle("bad3", 30, 30.0f, 0.0f)};
    clips[0].frames[0].rotations.resize(3);  // not skeleton-sized
    check(!matcher->build(skeleton(), clips, spec, err),
          "validation: pose not sized to skeleton refuses");
    check(!matcher->build(skeleton(), {}, spec, err),
          "validation: empty clip set refuses");
    MotionSkeleton emptySkeleton;
    check(!matcher->build(emptySkeleton,
                          {make_idle("ok", 30, 30.0f, 0.0f)}, spec, err),
          "validation: empty skeleton refuses");

    // A valid build, then query validation.
    check(matcher->build(skeleton(), {make_idle("ok", 30, 30.0f, 0.0f)}, spec,
                         err),
          "validation: valid build");
    MotionMatchQuery q;
    MotionMatchResult result;
    check(!matcher->match(q, result, err),
          "validation: wrong trajectory size refuses");
    q.trajectory.resize(spec.trajectoryPoints, glm::vec3(0));
    check(matcher->match(q, result, err),
          "validation: query without a pose matches (pose optional)");
    q.pose = make_pose(glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0),
                       glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0),
                       glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0));
    check(matcher->match(q, result, err),
          "validation: valid query matches");
    q.trajectory.clear();
    check(!matcher->match(q, result, err),
          "validation: wrong trajectory size refuses");
    q.trajectory.assign(spec.trajectoryPoints, glm::vec3(0));
    q.rootVelocity = glm::vec3(NAN, 0, 0);
    check(!matcher->match(q, result, err),
          "validation: non-finite velocity refuses");
    std::printf("[mm] validation all-or-nothing OK\n");
}

void test_self_match() {
    // A query built FROM a walk frame must find exactly that frame with cost 0.
    MotionMatcherSpec spec;
    const float rate = 30.0f;
    std::vector<MotionClipEntry> clips = {
        make_lococlip("walk", 120, rate, glm::vec3(0, 0, 1), 1.0f, 0.05f, 0.7f,
                      0.4f),
        make_lococlip("run", 120, rate, glm::vec3(0, 0, 1), 3.0f, 0.1f, 1.3f,
                      0.7f),
        make_lococlip("strafe", 120, rate, glm::vec3(1, 0, 0), 1.0f, 0.05f,
                      0.7f, 0.4f),
        make_idle("idleA", 60, rate, 0.0f),
        make_idle("idleB", 60, rate, 0.5f),
    };
    auto matcher = create_motion_matcher();
    std::string err;
    check(matcher->build(skeleton(), clips, spec, err), "self: build succeeds");
    check(matcher->clip_count() == 5, "self: five clips ingested");

    const int f0 = 5;
    const MotionClipEntry& walk = clips[0];
    MotionMatchQuery q;
    q.rootPosition = walk.rootPositions[static_cast<std::size_t>(f0)];
    q.rootVelocity =
        (walk.rootPositions[static_cast<std::size_t>(f0 + 1)] -
         walk.rootPositions[static_cast<std::size_t>(f0)]) *
        walk.frameRate;
    q.rootOrientation = glm::quat(1, 0, 0, 0);
    q.pose = walk.frames[static_cast<std::size_t>(f0)];
    q.trajectory = clip_trajectory(walk, f0, spec.trajectoryPoints,
                                   spec.trajectoryHorizon);
    MotionMatchResult out;
    check(matcher->match(q, out, err), "self: match succeeds");
    check(out.clip == 0, "self: matches the walk clip");
    check(out.frame == f0, "self: matches the exact walk frame");
    // Near-zero, not exactly 0: the pose feature uses (1 - |dot|) and
    // angleAxis quats are unit only to float precision (dot(q,q) = 1 +- 1e-7),
    // so a self-match costs ~1e-7 — still the unique minimum.
    checkClose(out.cost, 0.0f, 1e-5f, "self: near-zero cost (unique minimum)");
    std::printf("[mm] self-match exact (clip=%d frame=%d cost=%.9f)\n",
                out.clip, out.frame, out.cost);
}

void test_discrimination() {
    // Velocity + trajectory discriminate locomotion state: a walking query
    // matches walk, an idle query matches idle, a strafing query matches
    // strafe. Pose feature off (weight 0) — pure locomotion discrimination.
    MotionMatcherSpec spec;
    spec.poseWeight = 0.0f;
    const float rate = 30.0f;
    std::vector<MotionClipEntry> clips = {
        make_lococlip("walk", 120, rate, glm::vec3(0, 0, 1), 1.0f, 0.05f, 0.7f,
                      0.4f),
        make_lococlip("run", 120, rate, glm::vec3(0, 0, 1), 3.0f, 0.1f, 1.3f,
                      0.7f),
        make_lococlip("strafe", 120, rate, glm::vec3(1, 0, 0), 1.0f, 0.05f,
                      0.7f, 0.4f),
        make_idle("idleA", 60, rate, 0.0f),
    };
    auto matcher = create_motion_matcher();
    std::string err;
    check(matcher->build(skeleton(), clips, spec, err), "disc: build");

    // Walking query (velocity +Z, trajectory ahead).
    MotionMatchQuery walk;
    walk.rootVelocity = glm::vec3(0, 0, 1);
    walk.trajectory = {glm::vec3(0, 0, 1.0f / 6.0f), glm::vec3(0, 0, 2.0f / 6.0f),
                       glm::vec3(0, 0, 0.5f)};
    MotionMatchResult out;
    check(matcher->match(walk, out, err), "disc: walk query matches");
    check(out.clip == 0, "disc: walking query -> walk clip");

    // Idle query (velocity 0, stationary trajectory).
    MotionMatchQuery idle;
    idle.trajectory.assign(spec.trajectoryPoints, glm::vec3(0));
    check(matcher->match(idle, out, err), "disc: idle query matches");
    check(out.clip == 3, "disc: idle query -> idle clip");

    // Strafing query (velocity +X, trajectory to the side).
    MotionMatchQuery strafe;
    strafe.rootVelocity = glm::vec3(1, 0, 0);
    strafe.trajectory = {glm::vec3(1.0f / 6.0f, 0, 0),
                         glm::vec3(2.0f / 6.0f, 0, 0), glm::vec3(0.5f, 0, 0)};
    check(matcher->match(strafe, out, err), "disc: strafe query matches");
    check(out.clip == 2, "disc: strafing query -> strafe clip");
    std::printf("[mm] locomotion discrimination (walk/idle/strafe) OK\n");
}

void test_pose_feature() {
    // Two clips with IDENTICAL root motion (idle) but different leg poses:
    // with the trajectory/velocity features neutral, the pose feature must
    // pick the clip whose frame pose matches the query pose.
    MotionMatcherSpec spec;
    spec.trajectoryWeight = 0.0f;
    spec.velocityWeight = 0.0f;
    spec.poseWeight = 1.0f;
    spec.poseBones = {3, 4, 5, 6};  // thighs + shins
    const float rate = 30.0f;
    std::vector<MotionClipEntry> clips = {
        make_idle("idleA", 60, rate, 0.0f),
        make_idle("idleB", 60, rate, 0.5f),
    };
    auto matcher = create_motion_matcher();
    std::string err;
    check(matcher->build(skeleton(), clips, spec, err), "pose: build");

    MotionMatchQuery q;
    q.trajectory.assign(spec.trajectoryPoints, glm::vec3(0));
    // Query pose = a crouched (idleB) pose.
    q.pose = clips[1].frames[10];
    MotionMatchResult out;
    check(matcher->match(q, out, err), "pose: match succeeds");
    check(out.clip == 1, "pose: crouched query -> idleB clip");
    checkClose(out.cost, 0.0f, 1e-5f,
               "pose: near-zero cost on the exact frame");

    // Query pose = the straight (idleA) pose.
    q.pose = clips[0].frames[10];
    check(matcher->match(q, out, err), "pose: match succeeds");
    check(out.clip == 0, "pose: straight query -> idleA clip");
    std::printf("[mm] pose feature discriminates (idleA straight vs idleB "
                "crouch) OK\n");
}

void test_determinism() {
    MotionMatcherSpec spec;
    const float rate = 30.0f;
    std::vector<MotionClipEntry> clips = {
        make_lococlip("walk", 120, rate, glm::vec3(0, 0, 1), 1.0f, 0.05f, 0.7f,
                      0.4f),
        make_lococlip("run", 120, rate, glm::vec3(0, 0, 1), 3.0f, 0.1f, 1.3f,
                      0.7f),
        make_idle("idleA", 60, rate, 0.0f),
        make_idle("idleB", 60, rate, 0.5f),
    };
    std::string err;
    auto a = create_motion_matcher();
    auto b = create_motion_matcher();
    check(a->build(skeleton(), clips, spec, err) &&
              b->build(skeleton(), clips, spec, err),
          "determinism: both matchers build");

    MotionMatchQuery q;
    q.rootPosition = glm::vec3(0, 0.1f, 12.5f);
    q.rootVelocity = glm::vec3(0.2f, 0, 0.9f);
    q.trajectory = {glm::vec3(0.1f, 0, 12.9f), glm::vec3(0.15f, 0, 13.4f),
                    glm::vec3(0.2f, 0, 14.0f)};
    q.pose = clips[0].frames[30];
    MotionMatchResult ra, rb;
    for (int i = 0; i < 2; ++i) {
        check(a->match(q, ra, err), "determinism: match A");
        check(a->match(q, rb, err), "determinism: repeat match A");
        check(ra.clip == rb.clip && ra.frame == rb.frame && ra.cost == rb.cost,
              "determinism: repeated queries bit-identical");
    }
    check(b->match(q, rb, err), "determinism: match B");
    check(ra.clip == rb.clip && ra.frame == rb.frame && ra.cost == rb.cost,
          "determinism: independent matchers bit-identical");
    check(a->frame_count(0) == 120 && a->frame_count(2) == 60,
          "determinism: frame counts exposed");
    std::printf("[mm] cross-query + cross-instance determinism OK\n");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_validation();
    test_self_match();
    test_discrimination();
    test_pose_feature();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[mm] ALL PASSED\n");
        return 0;
    }
    std::printf("[mm] %d FAILURE(S)\n", g_failures);
    return 1;
}

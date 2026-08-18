// PoseWarpTests (FALTANTES §18 item 6): proves the public pose-warping
// contract. The warper adapts a sampled pose to the creature's LIVE
// locomotion state: the root snaps to (body position, yaw, terrain surface
// height), planted feet are pinned toward the item-5 placed targets (clamped
// per warp, never teleported), swinging feet keep their arc, an optional
// speed-based forward lean is applied, and the whole thing is deterministic.
// The gate also proves the full locomotion composition planner -> placement
// -> warp -> IK: a warped pose's planted feet are then solved EXACTLY by
// IMotionDatabase::ik_two_bone, landing on the placed terrain targets.
#include "engine/animation/IPoseWarper.hpp"
#include "engine/animation/IGaitPlanner.hpp"
#include "engine/animation/IFootPlacement.hpp"
#include "engine/animation/IMotionDatabase.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[warp] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[warp] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

void checkVec(const glm::vec3& a, const glm::vec3& b, float eps,
              const char* message) {
    if (glm::distance(a, b) > eps) {
        std::printf("[warp] FAIL: %s (%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f)\n",
                    message, a.x, a.y, a.z, b.x, b.y, b.z);
        ++g_failures;
    }
}

// A quadruped skeleton (canonical order: children after parents), matching
// the leg-chain geometry used by the gait/placement tests. Bones:
// 0 root, 1/4/7/10 hips, 2/5/8/11 knees, 3/6/9/12 feet.
engine::animation::MotionSkeleton make_quadruped_skeleton() {
    engine::animation::MotionSkeleton sk;
    sk.name = "quad";
    auto add = [&sk](const std::string& name, int parent, glm::vec3 t) {
        engine::animation::MotionBone b;
        b.name = name;
        b.parent = parent;
        b.localTranslation = t;
        sk.bones.push_back(b);
    };
    add("root", -1, glm::vec3(0));
    const glm::vec3 hips[4] = {{0.3f, 0.8f, 0.5f}, {-0.3f, 0.8f, 0.5f},
                               {0.3f, 0.8f, -0.5f}, {-0.3f, 0.8f, -0.5f}};
    for (int i = 0; i < 4; ++i) {
        add("hip" + std::to_string(i), 0, hips[i]);
        add("knee" + std::to_string(i), 1 + i * 3, glm::vec3(0, -1.0f, 0));
        add("foot" + std::to_string(i), 2 + i * 3, glm::vec3(0, -1.0f, 0));
    }
    return sk;
}

engine::animation::GaitAsset make_quadruped() {
    engine::animation::GaitAsset gait;
    gait.name = "quad";
    gait.cycleDuration = 1.0f;
    gait.stanceFraction = 0.6f;
    gait.stepHeight = 0.2f;
    gait.maxStride = 0.5f;
    const glm::vec3 hips[4] = {{0.3f, 0.8f, 0.5f}, {-0.3f, 0.8f, 0.5f},
                               {0.3f, 0.8f, -0.5f}, {-0.3f, 0.8f, -0.5f}};
    const char* names[4] = {"FL", "FR", "BL", "BR"};
    for (int i = 0; i < 4; ++i) {
        engine::animation::LegChainAsset leg;
        leg.name = names[i];
        leg.hipOffset = hips[i];
        leg.upperLength = 1.0f;
        leg.lowerLength = 1.0f;
        leg.restOffset = glm::vec3(0.0f, -0.8f, 0.0f);
        leg.hipBone = 1 + i * 3;
        leg.kneeBone = 2 + i * 3;
        leg.footBone = 3 + i * 3;
        gait.legs.push_back(leg);
    }
    gait.legPhases = {0.0f, 0.5f, 0.25f, 0.75f};
    return gait;
}

// A flat terrain sampler (surface = 130, matching the world convention: the
// gait/placement tests use FlatGenerator(130) whose top face is 131).
class FlatTerrain final : public engine::animation::IFootTerrainSampler {
public:
    explicit FlatTerrain(float surface) : surface_(surface) {}
    engine::animation::SurfaceSample sample(float, float) const override {
        engine::animation::SurfaceSample s;
        s.known = true;
        s.height = surface_;
        return s;
    }

private:
    float surface_;
};

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
        models[i] =
            p >= 0 ? models[static_cast<std::size_t>(p)] * local : local;
    }
    return glm::vec3(models[static_cast<std::size_t>(bone)][3]);
}

// 1. Root snap: the root lands at (body.x, surface, body.z) with the body
//    yaw; a speed-based lean pitches it forward.
void test_root_warp() {
    const engine::animation::MotionSkeleton sk = make_quadruped_skeleton();
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    for (const auto& b : sk.bones) {
        if (b.parent >= 0) pose.translations[b.parent + 1] = b.localTranslation;
    }

    auto warper = engine::animation::create_pose_warper();
    engine::animation::PoseWarpSpec spec;
    spec.rootWeight = 1.0f;
    spec.footWeight = 0.0f;  // isolate the root warp
    engine::animation::WarpInput input;
    input.bodyPosition = glm::vec3(10.0f, 0.0f, 20.0f);
    input.bodyYaw = 0.5f;
    input.surfaceHeight = 130.0f;
    input.speed = 0.0f;
    engine::animation::MotionPose out;
    std::string err;
    check(warper->warp(sk, pose, spec, input, out, err), "root: warp succeeds");
    checkVec(glm::vec3(out.translations[0].x, out.translations[0].y,
                       out.translations[0].z),
             glm::vec3(10.0f, 130.0f, 20.0f), 1e-5f,
             "root: snapped to (body.x, surface, body.z)");
    // Yaw: forward (+Z local) points toward yaw 0.5 after the root warp.
    const glm::vec3 forward =
        glm::mat3_cast(out.rotations[0]) * glm::vec3(0, 0, 1);
    checkClose(std::atan2(forward.x, forward.z), 0.5f, 1e-4f,
               "root: heading follows body yaw");

    // Lean: a 10 m/s body with leanFactor 0.02 pitches the root forward
    // (the +Z forward dips toward -Y).
    engine::animation::PoseWarpSpec leanSpec;
    leanSpec.rootWeight = 1.0f;
    leanSpec.footWeight = 0.0f;
    leanSpec.leanFactor = 0.02f;
    engine::animation::WarpInput leanInput = input;
    leanInput.speed = 10.0f;
    engine::animation::MotionPose leanOut;
    check(warper->warp(sk, pose, leanSpec, leanInput, leanOut, err),
          "root: lean warp succeeds");
    const glm::vec3 leanForward =
        glm::mat3_cast(leanOut.rotations[0]) * glm::vec3(0, 0, 1);
    check(leanForward.y < -1e-4f, "root: lean pitches forward (down)");
    std::printf("[warp] root warp (snap + yaw + lean) OK\n");
}

// 2. Foot pin: a planted foot approaches its placed target (clamped per
//    warp); swinging feet keep their arc; rootWeight=0 leaves the root.
void test_foot_pin() {
    const engine::animation::MotionSkeleton sk = make_quadruped_skeleton();
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    for (const auto& b : sk.bones) {
        if (b.parent >= 0) pose.translations[b.parent + 1] = b.localTranslation;
    }

    auto warper = engine::animation::create_pose_warper();
    engine::animation::PoseWarpSpec spec;
    spec.rootWeight = 0.0f;  // isolate the foot pins
    spec.footWeight = 1.0f;
    spec.maxFootMove = 5.0f;  // permissive: the target is ~1.2 m away
    engine::animation::WarpInput input;
    input.bodyPosition = glm::vec3(0.0f);
    input.bodyYaw = 0.0f;
    input.surfaceHeight = 130.0f;
    input.speed = 0.0f;
    // FL (bone 3) rests at world y = 0.8 - 1 - 1 = -1.2 below the origin;
    // pin it to a target 1.2 m up (within maxFootMove) -> the foot lands on it.
    engine::animation::WarpFootTarget fl;
    fl.footBone = 3;
    fl.targetWorld = glm::vec3(0.3f, 0.0f, 0.5f);
    fl.stance = true;
    input.feet.push_back(fl);
    engine::animation::MotionPose out;
    std::string err;
    check(warper->warp(sk, pose, spec, input, out, err), "pin: warp succeeds");
    checkClose(modelPosition(out, sk, 3).y, 0.0f, 1e-3f,
               "pin: planted foot reaches the pin target");
    // Root untouched when rootWeight = 0.
    checkVec(out.translations[0], glm::vec3(0.0f), 1e-6f,
             "pin: root untouched with rootWeight 0");

    // Swing foot: no pin (keeps its arc).
    engine::animation::WarpFootTarget swing;
    swing.footBone = 6;  // FR foot
    swing.targetWorld = glm::vec3(-0.3f, 200.0f, 0.5f);  // far above
    swing.stance = false;
    engine::animation::WarpInput swingInput = input;
    swingInput.feet.clear();
    swingInput.feet.push_back(swing);
    engine::animation::MotionPose swingOut;
    check(warper->warp(sk, pose, spec, swingInput, swingOut, err),
          "pin: swing warp succeeds");
    checkVec(modelPosition(swingOut, sk, 6), modelPosition(pose, sk, 6), 1e-5f,
             "pin: swinging foot keeps its arc");

    // Clamp: a far target is approached up to maxFootMove, not teleported.
    engine::animation::PoseWarpSpec clampSpec;
    clampSpec.rootWeight = 0.0f;
    clampSpec.footWeight = 1.0f;
    clampSpec.maxFootMove = 0.25f;
    engine::animation::WarpInput clampInput = input;
    engine::animation::WarpFootTarget far;
    far.footBone = 3;
    far.targetWorld = glm::vec3(0.3f, 1000.0f, 0.5f);  // ~870 m away
    far.stance = true;
    clampInput.feet.clear();
    clampInput.feet.push_back(far);
    engine::animation::MotionPose clampOut;
    check(warper->warp(sk, pose, clampSpec, clampInput, clampOut, err),
          "pin: clamp warp succeeds");
    const float moved = std::fabs(modelPosition(clampOut, sk, 3).y -
                                  modelPosition(pose, sk, 3).y);
    checkClose(moved, 0.25f, 1e-3f, "pin: far target clamped to maxFootMove");
    std::printf("[warp] foot pin (pin + swing + clamp) OK\n");
}

// 3. Refusals: all-or-nothing validation.
void test_refusals() {
    const engine::animation::MotionSkeleton sk = make_quadruped_skeleton();
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    auto warper = engine::animation::create_pose_warper();
    engine::animation::PoseWarpSpec spec;
    engine::animation::WarpInput input;
    engine::animation::MotionPose out;
    std::string err;

    // Empty skeleton.
    engine::animation::MotionSkeleton empty;
    check(!warper->warp(empty, pose, spec, input, out, err) && !err.empty(),
          "refuse: empty skeleton");
    err.clear();
    // Pose size mismatch.
    engine::animation::MotionPose badPose;
    badPose.translations.resize(1);
    badPose.rotations.resize(1);
    badPose.scales.resize(1);
    check(!warper->warp(sk, badPose, spec, input, out, err) && !err.empty(),
          "refuse: pose size mismatch");
    err.clear();
    // Invalid spec.
    engine::animation::PoseWarpSpec badSpec;
    badSpec.maxFootMove = 0.0f;
    check(!warper->warp(sk, pose, badSpec, input, out, err) && !err.empty(),
          "refuse: bad spec");
    err.clear();
    // Out-of-range foot bone.
    engine::animation::WarpInput badInput = input;
    engine::animation::WarpFootTarget bad;
    bad.footBone = 99;
    bad.stance = true;
    badInput.feet.push_back(bad);
    check(!warper->warp(sk, pose, spec, badInput, out, err) && !err.empty(),
          "refuse: foot bone out of range");
    err.clear();
    // Non-finite input.
    engine::animation::WarpInput nanInput = input;
    nanInput.surfaceHeight = std::nanf("");
    check(!warper->warp(sk, pose, spec, nanInput, out, err) && !err.empty(),
          "refuse: non-finite input");
    std::printf("[warp] refusals all-or-nothing OK\n");
}

// 4. Determinism: two warper instances, identical inputs -> identical poses.
void test_determinism() {
    const engine::animation::MotionSkeleton sk = make_quadruped_skeleton();
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    for (const auto& b : sk.bones) {
        if (b.parent >= 0) pose.translations[b.parent + 1] = b.localTranslation;
    }
    auto a = engine::animation::create_pose_warper();
    auto b = engine::animation::create_pose_warper();
    engine::animation::PoseWarpSpec spec;
    engine::animation::WarpInput input;
    input.bodyPosition = glm::vec3(3.0f, 0.0f, 4.0f);
    input.bodyYaw = 0.7f;
    input.surfaceHeight = 130.0f;
    input.speed = 2.5f;
    for (int i = 0; i < 4; ++i) {
        engine::animation::WarpFootTarget f;
        f.footBone = 3 + i * 3;
        f.targetWorld = glm::vec3(0.3f, 130.0f, 0.5f);
        f.stance = (i % 2 == 0);
        input.feet.push_back(f);
    }
    engine::animation::MotionPose pa, pb;
    std::string err;
    check(a->warp(sk, pose, spec, input, pa, err) &&
              b->warp(sk, pose, spec, input, pb, err),
          "determinism: warps succeed");
    bool same = true;
    for (std::size_t i = 0; i < sk.bones.size(); ++i) {
        if (pa.translations[i] != pb.translations[i] ||
            pa.rotations[i] != pb.rotations[i] ||
            pa.scales[i] != pb.scales[i]) {
            same = false;
        }
    }
    check(same, "determinism: bit-exact between instances");
    std::printf("[warp] determinism bit-exact OK\n");
}

// 5. Full composition: planner -> placement -> warp -> IK. The warped pose's
//    planted feet are solved exactly by ik_two_bone, landing on the placed
//    terrain targets.
void test_composition() {
    const engine::animation::MotionSkeleton sk = make_quadruped_skeleton();
    const engine::animation::GaitAsset gait = make_quadruped();
    auto planner = engine::animation::create_contact_planner();
    auto placer = engine::animation::create_foot_placer();
    auto warper = engine::animation::create_pose_warper();
    auto db = engine::animation::create_motion_database();
    std::string err;
    check(db->cook_skeleton(sk, err), "compose: skeleton cooks");

    // A pose with the body at the terrain surface (root y = 131 like the
    // flat world) and feet resting on it.
    const float surface = 130.0f;
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    pose.translations[0] = glm::vec3(0.0f, surface + 0.8f, 0.0f);  // body
    for (const auto& b : sk.bones) {
        if (b.parent >= 0 && b.parent < static_cast<int>(sk.bones.size())) {
            // hips are root-relative; knees/feet chain down.
        }
    }
    pose.translations[1] = glm::vec3(0.3f, 0.8f, 0.5f);
    pose.translations[2] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[3] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[4] = glm::vec3(-0.3f, 0.8f, 0.5f);
    pose.translations[5] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[6] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[7] = glm::vec3(0.3f, 0.8f, -0.5f);
    pose.translations[8] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[9] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[10] = glm::vec3(-0.3f, 0.8f, -0.5f);
    pose.translations[11] = glm::vec3(0.0f, -1.0f, 0.0f);
    pose.translations[12] = glm::vec3(0.0f, -1.0f, 0.0f);

    // Planner -> placement.
    engine::animation::GaitPlan plan;
    check(planner->plan(gait, 0.0f, glm::vec3(0.0f, 0.0f, 0.0f), 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "compose: plan succeeds");
    FlatTerrain terrain(surface);
    engine::animation::FootPlacementSpec placeSpec;
    engine::animation::FootPlacementResult placed, empty;
    check(placer->place(placeSpec, terrain, plan, empty, placed, err),
          "compose: placement succeeds");

    // Placement -> warp: the warper adapts the ROOT to the body state on the
    // terrain (footWeight 0 — the feet are the IK's exact job, see below).
    engine::animation::PoseWarpSpec warpSpec;
    warpSpec.rootWeight = 1.0f;
    warpSpec.footWeight = 0.0f;
    engine::animation::WarpInput warpInput;
    warpInput.bodyPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    warpInput.bodyYaw = 0.0f;
    warpInput.surfaceHeight = surface;
    warpInput.speed = 1.0f;
    engine::animation::MotionPose warped;
    check(warper->warp(sk, pose, warpSpec, warpInput, warped, err),
          "compose: warp succeeds");
    checkClose(warped.translations[0].y, surface, 1e-5f,
               "compose: root on the surface");

    // Warp -> IK: solve each planted chain exactly to its placed target.
    for (const engine::animation::PlacedFoot& pf : placed.feet) {
        if (!pf.stance) continue;
        const int root = 1 + pf.legIndex * 3;
        const int mid = 2 + pf.legIndex * 3;
        const int end = 3 + pf.legIndex * 3;
        engine::animation::MotionPose solved;
        check(db->ik_two_bone(warped, root, mid, end, pf.targetWorld,
                              glm::vec3(0, 0, 1), 1.0f, solved),
              "compose: IK solves the planted chain");
        checkVec(modelPosition(solved, sk, end), pf.targetWorld, 0.02f,
                 "compose: foot lands on the placed target");
    }
    std::printf("[warp] planner -> placement -> warp -> IK OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_root_warp();
    test_foot_pin();
    test_refusals();
    test_determinism();
    test_composition();
    if (g_failures == 0) {
        std::printf("[warp] ALL PASSED\n");
        return 0;
    }
    std::printf("[warp] %d FAILURE(S)\n", g_failures);
    return 1;
}

// SkeletonPoseMapper (FALTANTES item 2): binds the animation skeleton to the
// ragdoll bodies through Jolt's SkeletonMapper. Runs once (not per backend) —
// the mapper is a pure Jolt seam independent of the PhysicsRuntime backend.
#include "engine/physics/SkeletonPoseMapper.hpp"
#include "engine/animation/AnimationSystem.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Engine;
using namespace Engine::Physics;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// A humanoid-ish animation skeleton: Hips -> Spine -> Head with an extra
// "LeftArm" off the Spine that is NOT part of the ragdoll (tests chains and
// unmapped joints). localTransform carries the parent-relative transform.
SkeletonAsset make_animation_skeleton() {
    SkeletonAsset skeleton;
    BoneNode hips;
    hips.name = "Hips";
    hips.parentIndex = -1;
    hips.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    BoneNode spine;
    spine.name = "Spine";
    spine.parentIndex = 0;
    spine.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    BoneNode head;
    head.name = "Head";
    head.parentIndex = 1;
    head.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
    BoneNode arm;
    arm.name = "LeftArm";
    arm.parentIndex = 1;
    arm.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 0.1f, 0.0f));
    skeleton.bones = {hips, spine, head, arm};
    return skeleton;
}

std::vector<RagdollBoneDesc> make_ragdoll_bones() {
    std::vector<RagdollBoneDesc> bones;
    RagdollBoneDesc hips;
    hips.name = "Hips";
    hips.position = {0.0f, 0.0f, 0.0f};
    hips.length = 1.0f;
    hips.radius = 0.12f;
    hips.mass = 3.0f;
    bones.push_back(hips);
    RagdollBoneDesc spine;
    spine.name = "Spine";
    spine.parent = "Hips";
    spine.position = {0.0f, 1.0f, 0.0f};
    spine.length = 0.5f;
    spine.radius = 0.12f;
    spine.mass = 2.0f;
    bones.push_back(spine);
    RagdollBoneDesc head;
    head.name = "Head";
    head.parent = "Spine";
    head.position = {0.0f, 1.5f, 0.0f};
    head.length = 0.3f;
    head.radius = 0.15f;
    head.mass = 1.0f;
    bones.push_back(head);
    return bones;
}

// Identity animation pose (model space): Hips at origin, Spine 1m up, Head
// 1.5m up, LeftArm off the spine.
std::vector<glm::mat4> neutral_animation_pose() {
    std::vector<glm::mat4> pose(4, glm::mat4(1.0f));
    pose[0] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    pose[1] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    pose[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.5f, 0.0f));
    pose[3] = glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 1.1f, 0.0f));
    return pose;
}

// An animation pose with the head tilted: rotation applied around the spine
// joint, plus a translation on the head (model space).
std::vector<glm::mat4> tilted_animation_pose() {
    std::vector<glm::mat4> pose = neutral_animation_pose();
    const glm::quat tilt = glm::angleAxis(0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
    // Head in model space: spine model position, rotated around the spine.
    pose[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
              glm::mat4_cast(tilt) *
              glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
    return pose;
}

float translation_error(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b);
}

void test_mapper_mapping() {
    const SkeletonAsset skeleton = make_animation_skeleton();
    const std::vector<RagdollBoneDesc> bones = make_ragdoll_bones();

    SkeletonPoseMapper mapper;
    check(mapper.initialize(skeleton, bones), "mapper initializes");
    check(mapper.valid(), "mapper valid");
    check(mapper.mapped_bone_count() == 3, "three ragdoll bones map 1-on-1 into the animation skeleton");
    check(mapper.ragdoll_bone_names().size() == 3, "ragdoll bone names exposed");

    // Neutral pose round-trip: mapping the neutral animation pose onto the
    // ragdoll must reproduce the ragdoll bone positions (Hips 0, Spine 1,
    // Head 1.5 in Y).
    std::vector<glm::mat4> ragdollPose;
    check(mapper.map_to_ragdoll(neutral_animation_pose(), ragdollPose), "map_to_ragdoll");
    check(ragdollPose.size() == 3, "ragdoll pose has one matrix per bone");
    check(translation_error(glm::vec3(ragdollPose[0][3]), glm::vec3(0.0f, 0.0f, 0.0f)) < 0.01f,
          "Hips maps to the origin");
    check(translation_error(glm::vec3(ragdollPose[1][3]), glm::vec3(0.0f, 1.0f, 0.0f)) < 0.05f,
          "Spine maps to y=1");
    check(translation_error(glm::vec3(ragdollPose[2][3]), glm::vec3(0.0f, 1.5f, 0.0f)) < 0.05f,
          "Head maps to y=1.5");

    // The tilt is preserved: the head ragdoll matrix must carry a non-identity
    // rotation after mapping the tilted pose.
    std::vector<glm::mat4> tilted;
    check(mapper.map_to_ragdoll(tilted_animation_pose(), tilted), "map_to_ragdoll tilted");
    const glm::quat headRotation = glm::quat_cast(tilted[2]);
    const float angle = std::fabs(glm::angle(headRotation));
    check(angle > 0.3f, "head tilt survives the mapping (rotation preserved)");

    // Reverse: mapping the ragdoll pose back onto the animation skeleton
    // produces the animation pose again (round-trip through the mapper).
    std::vector<glm::mat4> backToAnimation;
    check(mapper.map_reverse_to_animation(ragdollPose, backToAnimation), "map_reverse_to_animation");
    check(backToAnimation.size() == 4, "animation pose has one matrix per bone");
    check(translation_error(glm::vec3(backToAnimation[1][3]), glm::vec3(0.0f, 1.0f, 0.0f)) < 0.05f,
          "reverse maps Spine back to y=1");
    check(translation_error(glm::vec3(backToAnimation[2][3]), glm::vec3(0.0f, 1.5f, 0.0f)) < 0.05f,
          "reverse maps Head back to y=1.5");
    std::printf("  [mapper] mapped=%zu hip=(%.2f,%.2f,%.2f) spine=(%.2f,%.2f,%.2f) head=(%.2f,%.2f,%.2f) tilt=%.3f\n",
                mapper.mapped_bone_count(),
                ragdollPose[0][3].x, ragdollPose[0][3].y, ragdollPose[0][3].z,
                ragdollPose[1][3].x, ragdollPose[1][3].y, ragdollPose[1][3].z,
                ragdollPose[2][3].x, ragdollPose[2][3].y, ragdollPose[2][3].z,
                angle);
}

void test_mapper_determinism_and_validation() {
    const SkeletonAsset skeleton = make_animation_skeleton();
    const std::vector<RagdollBoneDesc> bones = make_ragdoll_bones();

    // Determinism: two identical mappers produce identical ragdoll poses.
    SkeletonPoseMapper a, b;
    check(a.initialize(skeleton, bones) && b.initialize(skeleton, bones), "two mappers initialize");
    std::vector<glm::mat4> poseA, poseB;
    check(a.map_to_ragdoll(tilted_animation_pose(), poseA), "mapper A maps");
    check(b.map_to_ragdoll(tilted_animation_pose(), poseB), "mapper B maps");
    bool identical = poseA.size() == poseB.size();
    for (std::size_t i = 0; identical && i < poseA.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(poseA[i][c].x - poseB[i][c].x) > 1.0e-6f ||
                std::fabs(poseA[i][c].y - poseB[i][c].y) > 1.0e-6f ||
                std::fabs(poseA[i][c].z - poseB[i][c].z) > 1.0e-6f) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "identical mappers produce identical poses (determinism)");

    // Validation: wrong-size inputs are refused.
    std::vector<glm::mat4> out;
    check(!a.map_to_ragdoll(std::vector<glm::mat4>(2, glm::mat4(1.0f)), out),
          "wrong animation pose size refused");
    check(!a.map_reverse_to_animation(std::vector<glm::mat4>(1, glm::mat4(1.0f)), out),
          "wrong ragdoll pose size refused");

    // Empty skeleton: initialize fails cleanly.
    SkeletonPoseMapper empty;
    check(!empty.initialize(SkeletonAsset{}, bones), "empty animation skeleton refused");
    check(!empty.valid(), "empty mapper not valid");

    // Unmapped ragdoll (bone names absent from the animation skeleton): the
    // mapper must refuse rather than silently produce garbage. All three names
    // must be missing — a partial rename is a legitimate sparse mapping.
    std::vector<RagdollBoneDesc> bad = bones;
    bad[0].name = "NoSuchBone";
    bad[1].name = "AlsoMissing";
    bad[2].name = "GoneToo";
    SkeletonPoseMapper badMapper;
    check(!badMapper.initialize(skeleton, bad), "ragdoll with zero mapped joints refused");
    std::printf("  [mapper] determinism=%s validation=ok\n", identical ? "yes" : "no");
}

void test_ragdoll_set_pose() {
    // Drives a real ragdoll through the mapper: build the ragdoll, map a pose,
    // apply it to the bodies, and verify the bodies moved to the mapped pose.
    WorldSettings settings;
    settings.gravity = {0.0f, 0.0f, 0.0f};
    PhysicsRuntime world(settings, PhysicsBackendKind::Jolt);
    const std::vector<RagdollBoneDesc> bones = make_ragdoll_bones();
    Ragdoll ragdoll;
    check(ragdoll.create(world, bones, {10.0f, 20.0f, 30.0f}), "ragdoll created");
    check(ragdoll.uses_swing_twist_joints(), "Jolt ragdoll uses swing-twist joints");

    SkeletonPoseMapper mapper;
    check(mapper.initialize(make_animation_skeleton(), bones), "mapper initialized");
    std::vector<glm::mat4> pose;
    check(mapper.map_to_ragdoll(tilted_animation_pose(), pose), "pose mapped");
    check(ragdoll.set_pose(world, {10.0f, 20.0f, 30.0f}, pose), "pose applied to bodies");

    const auto current = ragdoll.pose(world);
    check(current.size() == 3, "pose read back");
    const float spineY = current[1].position.y;
    check(std::fabs(spineY - (20.0f + 1.0f)) < 0.05f, "Spine body moved to mapped y=21");
    const glm::quat headRot = current[2].rotation;
    const float angle = std::fabs(glm::angle(headRot));
    check(angle > 0.3f, "Head body carries the mapped tilt");
    std::printf("  [set_pose] spine=(%.2f,%.2f,%.2f) headTilt=%.3f\n",
                current[1].position.x, current[1].position.y, current[1].position.z, angle);
    ragdoll.destroy(world);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_mapper_mapping();
    test_mapper_determinism_and_validation();
    test_ragdoll_set_pose();
    if (failures == 0) {
        std::printf("ALL SKELETON MAPPER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}

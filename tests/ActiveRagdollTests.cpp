// ActiveRagdollTests (FALTANTES §18 item 8): proves the active/partial ragdoll
// with recovery driven by the Jolt swing-twist constraint motors, built on the
// §16 item 2 infrastructure (Ragdoll + SkeletonPoseMapper). ACTIVE = the
// motors hold a live animation pose (drive_to_pose); PARTIAL = only the joints
// authored with jointMotorOn are driven (a disabled joint stays displaced
// under perturbation while the driven ones pull back); RECOVERY = the
// caller-owned blend weight advances at the rate and the OUTPUT pose converges
// to the mapped animation pose exactly at weight 1, regardless of how
// perturbed the physical state was. The physics-convergence gates use
// tolerances (the Jolt solver is multi-threaded / non-deterministic, documented
// in docs/DETERMINISMO_PROVIDERS.md); the output-blend endpoints are exact.
#include "engine/physics/ActiveRagdoll.hpp"
#include "engine/animation/AnimationSystem.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
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

// A humanoid-ish animation skeleton: Hips -> Spine -> Head plus a LeftArm that
// is NOT part of the ragdoll (unmapped joint, like the §16 item 2 test).
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

// Ragdoll chain: Hips -> Spine -> Head. `headMotor` = the head joint's
// jointMotorOn flag (the partial-activation mask).
std::vector<RagdollBoneDesc> make_ragdoll_bones(bool headMotor) {
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
    spine.jointMotorOn = true;
    spine.jointMotorFrequency = 40.0f;
    spine.jointMotorDamping = 10.0f;
    bones.push_back(spine);
    RagdollBoneDesc head;
    head.name = "Head";
    head.parent = "Spine";
    head.position = {0.0f, 1.5f, 0.0f};
    head.length = 0.3f;
    head.radius = 0.15f;
    head.mass = 1.0f;
    head.jointMotorOn = headMotor;
    head.jointMotorFrequency = 40.0f;
    head.jointMotorDamping = 10.0f;
    bones.push_back(head);
    return bones;
}

// Neutral animation pose (model space).
std::vector<glm::mat4> neutral_pose() {
    std::vector<glm::mat4> pose(4, glm::mat4(1.0f));
    pose[0] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    pose[1] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    pose[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.5f, 0.0f));
    pose[3] = glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, 1.1f, 0.0f));
    return pose;
}

// Tilted pose: the head rotated 0.5 rad about X around the spine.
std::vector<glm::mat4> tilted_pose() {
    std::vector<glm::mat4> pose = neutral_pose();
    const glm::quat tilt = glm::angleAxis(0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
    pose[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
              glm::mat4_cast(tilt) *
              glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
    return pose;
}

// Angle between two unit quaternions (radians), in [0, pi].
float quat_angle(const glm::quat& a, const glm::quat& b) {
    const float d = std::fabs(glm::dot(glm::normalize(a), glm::normalize(b)));
    return 2.0f * std::acos(std::min(1.0f, d));
}

// Relative orientation of the child bone in the parent's frame, compared to
// the mapped target (radians).
float relative_angle(const std::vector<RagdollPoseBone>& pose,
                     const std::vector<glm::mat4>& mapped, std::size_t parent,
                     std::size_t child) {
    const glm::quat physical =
        glm::inverse(pose[parent].rotation) * pose[child].rotation;
    const glm::quat target = glm::inverse(glm::quat_cast(mapped[parent])) *
                             glm::quat_cast(mapped[child]);
    return quat_angle(physical, target);
}

void test_api_refusals() {
    // Builtin backend has no swing-twist seam: the motor API refuses.
    WorldSettings settings;
    settings.gravity = {0.0f, 0.0f, 0.0f};
    PhysicsRuntime builtin(settings, PhysicsBackendKind::Builtin);
    check(!builtin.supports_swing_twist(), "builtin has no swing-twist seam");
    check(!builtin.set_swing_twist_motor(1, true, 4.0f, 2.0f, glm::quat(1, 0, 0, 0)),
          "builtin set_swing_twist_motor refuses");

    // Jolt: invalid handles refuse (zero, unknown high-space, distance space).
    PhysicsRuntime world(settings, PhysicsBackendKind::Jolt);
    check(world.supports_swing_twist(), "jolt has the swing-twist seam");
    check(!world.set_swing_twist_motor(0, true, 4.0f, 2.0f, glm::quat(1, 0, 0, 0)),
          "zero handle refuses");
    check(!world.set_swing_twist_motor(kSwingTwistHandleOffset + 999, true, 4.0f,
                                       2.0f, glm::quat(1, 0, 0, 0)),
          "unknown swing-twist handle refuses");
    const BodyHandle a = world.create_body(BodyDesc{});
    const BodyHandle b = world.create_body(BodyDesc{});
    DistanceConstraintDesc dc;
    dc.bodyA = a;
    dc.bodyB = b;
    const ConstraintHandle dh = world.create_distance_constraint(dc);
    check(dh != InvalidConstraint, "distance constraint created");
    check(!world.set_swing_twist_motor(dh, true, 4.0f, 2.0f, glm::quat(1, 0, 0, 0)),
          "distance-constraint handle refuses");

    // ActiveRagdoll driver refusals.
    ActiveRagdoll rag;
    std::vector<glm::mat4> out;
    float w = 0.0f;
    check(!rag.drive_to_pose(world, neutral_pose()), "drive before create refuses");
    check(!rag.recover(world, 1.0f / 60.0f, w, neutral_pose(), 1.0f, out),
          "recover before create refuses");
    check(!rag.recover(world, -1.0f, w, neutral_pose(), 1.0f, out),
          "negative dt refuses");
    check(!rag.recover(world, 1.0f / 60.0f, w, neutral_pose(), -1.0f, out),
          "negative rate refuses");
    std::printf("  [api] refusals all-or-nothing OK\n");
}

void test_active_drive() {
    WorldSettings settings;
    settings.gravity = {0.0f, 0.0f, 0.0f};
    PhysicsRuntime world(settings, PhysicsBackendKind::Jolt);
    ActiveRagdoll rag;
    const glm::vec3 rootPos(10.0f, 20.0f, 30.0f);
    check(rag.create(world, make_animation_skeleton(), make_ragdoll_bones(true),
                     rootPos),
          "active ragdoll created");
    check(rag.valid(), "active ragdoll valid");
    check(rag.joint_count() == 2, "two joints (hips->spine, spine->head)");
    check(rag.joint_driven(0) && rag.joint_driven(1), "both joints driven");

    std::vector<glm::mat4> mapped;
    check(rag.mapper().map_to_ragdoll(tilted_pose(), mapped), "pose mapped");

    // Drive to the tilted pose: the motors hold the head tilt.
    check(rag.drive_to_pose(world, tilted_pose()), "drive_to_pose succeeds");
    const float initial =
        relative_angle(rag.ragdoll().pose(world), mapped, 1, 2);
    for (int i = 0; i < 180; ++i) world.step(1.0f / 60.0f);
    const float settled =
        relative_angle(rag.ragdoll().pose(world), mapped, 1, 2);
    check(settled < 0.25f, "motors hold the head near the tilted target");
    check(settled < initial * 0.5f,
          "the joint converged toward the target from the neutral start");
    std::printf("  [active] head relative error: %.4f -> %.4f rad\n", initial,
                settled);

    // A hit (off-COM impulse -> torque) knocks the head off; the motors pull
    // it back.
    const BodyHandle head = rag.ragdoll().bone_body("Head");
    const glm::vec3 headPos = rag.ragdoll().pose(world)[2].position;
    world.apply_impulse_at_point(head, glm::vec3(0.0f, 0.0f, -2.0f),
                                 headPos + glm::vec3(0.15f, 0.0f, 0.0f));
    world.step(1.0f / 60.0f);
    world.step(1.0f / 60.0f);
    const float knocked =
        relative_angle(rag.ragdoll().pose(world), mapped, 1, 2);
    for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
    const float recovered =
        relative_angle(rag.ragdoll().pose(world), mapped, 1, 2);
    check(recovered < knocked, "motors pull the head back after the hit");
    check(recovered < 0.25f, "head back near the target after recovery");
    std::printf("  [active] after hit: %.4f -> %.4f rad\n", knocked, recovered);
    rag.destroy(world);
}

void test_partial() {
    // Full ragdoll vs partial (head joint motor OFF): same drive, same hit.
    // The driven joints pull back; the free joint stays displaced.
    WorldSettings settings;
    settings.gravity = {0.0f, 0.0f, 0.0f};
    const glm::vec3 rootPos(10.0f, 20.0f, 30.0f);

    PhysicsRuntime worldFull(settings, PhysicsBackendKind::Jolt);
    ActiveRagdoll full;
    check(full.create(worldFull, make_animation_skeleton(),
                      make_ragdoll_bones(true), rootPos),
          "full ragdoll created");
    check(full.joint_driven(0) && full.joint_driven(1),
          "full: both joints driven");

    PhysicsRuntime worldPart(settings, PhysicsBackendKind::Jolt);
    ActiveRagdoll part;
    check(part.create(worldPart, make_animation_skeleton(),
                      make_ragdoll_bones(false), rootPos),
          "partial ragdoll created");
    check(part.joint_driven(0) && !part.joint_driven(1),
          "partial: head joint NOT driven (hips->spine still driven)");

    std::vector<glm::mat4> mappedF;
    std::vector<glm::mat4> mappedP;
    check(full.mapper().map_to_ragdoll(tilted_pose(), mappedF), "full mapped");
    check(part.mapper().map_to_ragdoll(tilted_pose(), mappedP), "partial mapped");

    check(full.drive_to_pose(worldFull, tilted_pose()), "full drive");
    check(part.drive_to_pose(worldPart, tilted_pose()), "partial drive");
    for (int i = 0; i < 120; ++i) {
        worldFull.step(1.0f / 60.0f);
        worldPart.step(1.0f / 60.0f);
    }
    const BodyHandle headF = full.ragdoll().bone_body("Head");
    const BodyHandle headP = part.ragdoll().bone_body("Head");
    const glm::vec3 headPosF = full.ragdoll().pose(worldFull)[2].position;
    const glm::vec3 headPosP = part.ragdoll().pose(worldPart)[2].position;
    // Off-COM impulse -> a yaw torque (orthogonal to the tilted pitch target),
    // so the free joint's displacement reliably moves AWAY from the target.
    worldFull.apply_impulse_at_point(headF, glm::vec3(0.0f, 0.0f, -2.0f),
                                     headPosF + glm::vec3(0.15f, 0.0f, 0.0f));
    worldPart.apply_impulse_at_point(headP, glm::vec3(0.0f, 0.0f, -2.0f),
                                     headPosP + glm::vec3(0.15f, 0.0f, 0.0f));
    for (int i = 0; i < 120; ++i) {
        worldFull.step(1.0f / 60.0f);
        worldPart.step(1.0f / 60.0f);
    }
    const float errFull =
        relative_angle(full.ragdoll().pose(worldFull), mappedF, 1, 2);
    const float errPart =
        relative_angle(part.ragdoll().pose(worldPart), mappedP, 1, 2);
    check(errFull < 0.3f, "full: driven head joint pulled back to the target");
    check(errPart > errFull + 0.1f,
          "partial: the free head joint stayed displaced (no motor)");
    std::printf("  [partial] head error: full=%.4f partial=%.4f rad\n", errFull,
                errPart);
    full.destroy(worldFull);
    part.destroy(worldPart);
}

void test_recovery() {
    WorldSettings settings;
    settings.gravity = {0.0f, 0.0f, 0.0f};
    // Root at the origin so the blend endpoints are exact.
    const glm::vec3 rootPos(0.0f, 0.0f, 0.0f);

    // Two ragdolls, perturbed DIFFERENTLY: one settled, one hit hard.
    PhysicsRuntime worldA(settings, PhysicsBackendKind::Jolt);
    ActiveRagdoll a;
    check(a.create(worldA, make_animation_skeleton(), make_ragdoll_bones(true),
                   rootPos),
          "ragdoll A created");
    check(a.drive_to_pose(worldA, neutral_pose()), "A neutral drive");
    for (int i = 0; i < 60; ++i) worldA.step(1.0f / 60.0f);

    PhysicsRuntime worldB(settings, PhysicsBackendKind::Jolt);
    ActiveRagdoll b;
    check(b.create(worldB, make_animation_skeleton(), make_ragdoll_bones(true),
                   rootPos),
          "ragdoll B created");
    // Deterministic physical perturbation: kinematically drive B's bodies to a
    // yawed pose (head rotated 0.9 rad about Y) — no dependence on solver
    // timing, and clearly off the neutral A is settled at.
    std::vector<glm::mat4> perturbed = neutral_pose();
    const glm::quat yaw = glm::angleAxis(0.9f, glm::vec3(0.0f, 1.0f, 0.0f));
    perturbed[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                   glm::mat4_cast(yaw) *
                   glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
    std::vector<glm::mat4> perturbedRagdoll;
    check(b.mapper().map_to_ragdoll(perturbed, perturbedRagdoll),
          "B perturbed pose mapped");
    check(b.ragdoll().set_pose(worldB, rootPos, perturbedRagdoll),
          "B physically perturbed via set_pose");

    std::vector<glm::mat4> mapped;
    check(a.mapper().map_to_ragdoll(tilted_pose(), mapped), "mapped pose");

    // B is clearly more perturbed than A before recovery.
    check(relative_angle(b.ragdoll().pose(worldB), mapped, 1, 2) >
              relative_angle(a.ragdoll().pose(worldA), mapped, 1, 2) + 0.1f,
          "B is more perturbed than A before recovery");

    // At weight 0 the output IS the physical pose (model space).
    float wA = 0.0f;
    std::vector<glm::mat4> outA;
    check(a.recover(worldA, 1.0f / 60.0f, wA, tilted_pose(), 0.0f, outA),
          "A recover (rate 0)");
    check(wA == 0.0f, "rate 0 keeps the weight at 0");
    const auto physA = a.ragdoll().pose(worldA);
    check(outA.size() == 3, "output has one matrix per ragdoll bone");
    const glm::vec3 rootPhys = physA[0].position;
    check(glm::length(glm::vec3(outA[1][3]) - (physA[1].position - rootPhys)) <
                  1e-4f &&
              quat_angle(glm::quat_cast(outA[1]), physA[1].rotation) < 1e-4f,
          "weight 0 output equals the physical pose (model space)");

    // Converge both to weight 1: the output becomes the mapped animation pose
    // EXACTLY, regardless of how different the physical start was.
    float wB = 0.0f;
    std::vector<glm::mat4> outB;
    for (int i = 0; i < 60; ++i) {
        check(a.recover(worldA, 1.0f / 60.0f, wA, tilted_pose(), 3.0f, outA),
              "A recover advances");
        check(b.recover(worldB, 1.0f / 60.0f, wB, tilted_pose(), 3.0f, outB),
              "B recover advances");
    }
    check(wA == 1.0f && wB == 1.0f, "both weights clamp to 1");
    for (std::size_t i = 0; i < mapped.size(); ++i) {
        const float posErrA =
            glm::length(glm::vec3(outA[i][3]) - glm::vec3(mapped[i][3]));
        const float posErrB =
            glm::length(glm::vec3(outB[i][3]) - glm::vec3(mapped[i][3]));
        const float rotErrA =
            quat_angle(glm::quat_cast(outA[i]), glm::quat_cast(mapped[i]));
        const float rotErrB =
            quat_angle(glm::quat_cast(outB[i]), glm::quat_cast(mapped[i]));
        check(posErrA < 1e-4f && posErrB < 1e-4f,
              "recovered output position matches the animation pose");
        check(rotErrA < 1e-4f && rotErrB < 1e-4f,
              "recovered output rotation matches the animation pose");
        check(glm::length(glm::vec3(outA[i][3]) - glm::vec3(outB[i][3])) <
                      1e-6f &&
                  quat_angle(glm::quat_cast(outA[i]),
                             glm::quat_cast(outB[i])) < 1e-6f,
              "A and B converge to the SAME output (start-independent)");
    }
    // The root body was driven back to the animation root position.
    const glm::vec3 rootNow = a.ragdoll().pose(worldA)[0].position;
    check(glm::length(rootNow - rootPos) < 1e-3f,
          "recovery drove the root body back to the animation root");
    std::printf("  [recover] output converged to the animation pose at w=1\n");
    a.destroy(worldA);
    b.destroy(worldB);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_api_refusals();
    test_active_drive();
    test_partial();
    test_recovery();
    if (failures == 0) {
        std::printf("ALL ACTIVE RAGDOLL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}

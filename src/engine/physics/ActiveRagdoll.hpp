#pragma once

#include "Ragdoll.hpp"
#include "SkeletonPoseMapper.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Physics {

// Active/partial ragdoll with recovery (FALTANTES §18 item 8), built on the
// §16 item 2 infrastructure (Ragdoll swing-twist joints + SkeletonPoseMapper).
//
// - ACTIVE: the joint constraint motors drive the ragdoll toward a live
//   animation pose (drive_to_pose) instead of the kinematic set_pose path —
//   physics can still be perturbed (impulse/hit) and the motors pull back.
// - PARTIAL: only the bones authored with jointMotorOn are driven; the others
//   stay free under physics (e.g. a stunned limb droops under gravity).
// - RECOVERY: after a perturbation, recover() advances a caller-owned blend
//   weight and per frame (a) maps the animation pose onto the ragdoll,
//   (b) drives every joint motor toward the blended relative orientation and
//   the root body toward the blended root transform, and (c) writes the
//   blended OUTPUT pose the caller renders. At weight 1 the output equals the
//   animation pose EXACTLY (lerp/slerp endpoints are bit-exact).
//
// The motor-math and the output blend are deterministic given the input poses;
// the Jolt solver itself is multi-threaded and non-deterministic (documented
// in docs/DETERMINISMO_PROVIDERS.md), so physics-convergence gates use
// tolerances while the OUTPUT blend uses exact endpoints.
class ActiveRagdoll final {
public:
    ActiveRagdoll() = default;

    // Builds the Ragdoll (per-bone jointMotorOn = the partial-activation
    // mask) and the SkeletonPoseMapper. `bones` must be non-empty with unique
    // names and present parents (Ragdoll::create validates).
    bool create(PhysicsRuntime& world,
                const Engine::SkeletonAsset& animationSkeleton,
                const std::vector<RagdollBoneDesc>& bones,
                const glm::vec3& rootPosition);
    void destroy(PhysicsRuntime& world);

    bool valid() const noexcept {
        return !ragdoll_.empty() && mapper_ && mapper_->valid();
    }
    Ragdoll& ragdoll() noexcept { return ragdoll_; }
    const Ragdoll& ragdoll() const noexcept { return ragdoll_; }
    const SkeletonPoseMapper& mapper() const noexcept {
        return *mapper_;
    }

    // Number of joints and whether the k-th joint is motor-driven (authored
    // jointMotorOn) — the partial-activation surface the gate asserts on.
    std::size_t joint_count() const noexcept { return joints_.size(); }
    bool joint_driven(std::size_t k) const noexcept {
        return k < joints_.size() ? joints_[k].driven : false;
    }

    // Active drive: maps the animation model-space pose onto the ragdoll and
    // sets each DRIVEN joint's motor target to the child's relative
    // orientation under the mapped pose (same convention as creation:
    // inverse(parent) * child). Non-driven joints stay free. Returns false
    // when invalid, not on swing-twist joints, or the pose size mismatches.
    bool drive_to_pose(PhysicsRuntime& world,
                       const std::vector<glm::mat4>& animationModelPose);

    // Recovery: advances `blendWeight` by dt * recoveryRate (clamped to 1),
    // drives every joint motor (all forced on during recovery) toward the
    // blended relative orientation and the root body toward the blended root
    // transform, and writes the blended OUTPUT pose (model space relative to
    // the ragdoll root, one matrix per ragdoll bone in `bones` order) the
    // caller renders. weight 0 = fully physical, 1 = exactly the animation
    // pose. Returns false when invalid, dt < 0, rate < 0 or size mismatch.
    bool recover(PhysicsRuntime& world, float dt, float& blendWeight,
                 const std::vector<glm::mat4>& animationModelPose,
                 float recoveryRate, std::vector<glm::mat4>& outBlendedPose);

private:
    struct JointDrive {
        std::size_t parentIndex{0};
        std::size_t childIndex{0};
        bool driven{false};
        float frequency{4.0f};
        float damping{2.0f};
        ConstraintHandle handle{InvalidConstraint};
    };

    Ragdoll ragdoll_;
    // unique_ptr: SkeletonPoseMapper is non-movable (deleted copy + user
    // destructor), so the mapper is heap-allocated and rebuilt per create().
    std::unique_ptr<SkeletonPoseMapper> mapper_;
    std::vector<std::string> boneNames_;
    std::vector<JointDrive> joints_;
    glm::vec3 rootPosition_{0.0f};
    BodyHandle rootBody_{InvalidBody};
};

} // namespace Engine::Physics

// ActiveRagdoll (FALTANTES §18 item 8): active/partial ragdoll with recovery
// driven by the Jolt swing-twist constraint motors, built on the §16 item 2
// infrastructure (Ragdoll + SkeletonPoseMapper). Active = motors hold a live
// animation pose (drive_to_pose); partial = only joints authored with
// jointMotorOn are driven; recovery = blend the physical state back to the
// animation pose at a caller-chosen rate (all motors forced on, root body
// driven toward the blended root) while writing the blended OUTPUT pose the
// caller renders.
#include "ActiveRagdoll.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Engine::Physics {

namespace {
glm::vec3 model_position(const glm::mat4& m) { return glm::vec3(m[3]); }
glm::quat model_rotation(const glm::mat4& m) { return glm::quat_cast(m); }
} // namespace

bool ActiveRagdoll::create(PhysicsRuntime& world,
                           const Engine::SkeletonAsset& animationSkeleton,
                           const std::vector<RagdollBoneDesc>& bones,
                           const glm::vec3& rootPosition) {
    destroy(world);
    if (bones.empty() || animationSkeleton.bones.empty()) return false;
    rootPosition_ = rootPosition;
    boneNames_.clear();
    std::unordered_map<std::string, std::size_t> nameToIndex;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        boneNames_.push_back(bones[i].name);
        nameToIndex.emplace(bones[i].name, i);
    }
    if (!ragdoll_.create(world, bones, rootPosition)) return false;
    mapper_ = std::make_unique<SkeletonPoseMapper>();
    if (!mapper_->initialize(animationSkeleton, bones)) {
        destroy(world);
        return false;
    }
    const std::vector<ConstraintHandle>& handles = ragdoll_.joint_handles();
    joints_.clear();
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].parent.empty()) continue;
        const auto parentIt = nameToIndex.find(bones[i].parent);
        if (parentIt == nameToIndex.end()) {
            destroy(world);
            return false;
        }
        if (joints_.size() >= handles.size() ||
            handles[joints_.size()] == InvalidConstraint) {
            destroy(world);
            return false;
        }
        JointDrive drive;
        drive.parentIndex = parentIt->second;
        drive.childIndex = i;
        drive.driven = bones[i].jointMotorOn;
        drive.frequency = bones[i].jointMotorFrequency;
        drive.damping = bones[i].jointMotorDamping;
        drive.handle = handles[joints_.size()];
        joints_.push_back(drive);
    }
    rootBody_ = ragdoll_.bone_body(bones[0].name);
    return valid();
}

void ActiveRagdoll::destroy(PhysicsRuntime& world) {
    ragdoll_.destroy(world);
    mapper_.reset();
    boneNames_.clear();
    joints_.clear();
    rootBody_ = InvalidBody;
    rootPosition_ = glm::vec3(0.0f);
}

bool ActiveRagdoll::drive_to_pose(PhysicsRuntime& world,
                                  const std::vector<glm::mat4>& animationModelPose) {
    if (!valid() || !ragdoll_.uses_swing_twist_joints()) return false;
    std::vector<glm::mat4> mapped;
    if (!mapper_->map_to_ragdoll(animationModelPose, mapped)) return false;
    if (mapped.size() != boneNames_.size()) return false;
    for (const JointDrive& joint : joints_) {
        if (!joint.driven) continue;  // partial ragdoll: joint stays free
        const glm::quat parentRot = model_rotation(mapped[joint.parentIndex]);
        const glm::quat childRot = model_rotation(mapped[joint.childIndex]);
        const glm::quat target = glm::inverse(parentRot) * childRot;
        if (!world.set_swing_twist_motor(joint.handle, true, joint.frequency,
                                         joint.damping, target)) {
            return false;
        }
    }
    return true;
}

bool ActiveRagdoll::recover(PhysicsRuntime& world, float dt,
                            float& blendWeight,
                            const std::vector<glm::mat4>& animationModelPose,
                            float recoveryRate,
                            std::vector<glm::mat4>& outBlendedPose) {
    if (!valid() || !ragdoll_.uses_swing_twist_joints()) return false;
    if (dt < 0.0f || recoveryRate < 0.0f || !std::isfinite(dt) ||
        !std::isfinite(recoveryRate)) {
        return false;
    }
    std::vector<glm::mat4> mapped;
    if (!mapper_->map_to_ragdoll(animationModelPose, mapped)) return false;
    if (mapped.size() != boneNames_.size()) return false;
    const std::vector<RagdollPoseBone> physical = ragdoll_.pose(world);
    if (physical.size() != boneNames_.size()) return false;

    blendWeight = std::clamp(blendWeight + dt * recoveryRate, 0.0f, 1.0f);
    const float w = blendWeight;

    // Blended world-space pose: mix(physical, desiredWorld, w), where
    // desiredWorld = rootPosition_ + mapped position, mapped rotation.
    std::vector<glm::vec3> blendedPos(boneNames_.size());
    std::vector<glm::quat> blendedRot(boneNames_.size());
    for (std::size_t i = 0; i < boneNames_.size(); ++i) {
        const glm::vec3 desiredPos = rootPosition_ + model_position(mapped[i]);
        const glm::quat desiredRot = model_rotation(mapped[i]);
        blendedPos[i] = glm::mix(physical[i].position, desiredPos, w);
        blendedRot[i] = glm::slerp(physical[i].rotation, desiredRot, w);
    }

    // Drive: every joint motor toward the blended relative orientation (all
    // forced on during recovery) and the root body toward the blended root.
    for (const JointDrive& joint : joints_) {
        const glm::quat target =
            glm::inverse(blendedRot[joint.parentIndex]) *
            blendedRot[joint.childIndex];
        if (!world.set_swing_twist_motor(joint.handle, true, joint.frequency,
                                         joint.damping, target)) {
            return false;
        }
    }
    if (rootBody_ != InvalidBody && !boneNames_.empty()) {
        world.set_transform(rootBody_, blendedPos[0], blendedRot[0]);
    }

    // Output pose in model space relative to the ragdoll root.
    outBlendedPose.clear();
    outBlendedPose.reserve(boneNames_.size());
    const glm::vec3 rootWorld = blendedPos[0];
    for (std::size_t i = 0; i < boneNames_.size(); ++i) {
        const glm::mat4 m =
            glm::translate(glm::mat4(1.0f), blendedPos[i] - rootWorld) *
            glm::mat4_cast(blendedRot[i]);
        outBlendedPose.push_back(m);
    }
    return true;
}

} // namespace Engine::Physics

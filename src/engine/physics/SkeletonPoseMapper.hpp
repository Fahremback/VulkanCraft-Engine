#pragma once

#include "Ragdoll.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Physics {

// Maps poses between the ragdoll skeleton (the RagdollBoneDesc hierarchy) and
// an animation skeleton (SkeletonAsset) using Jolt's SkeletonMapper
// (third_party/jolt/Jolt/Skeleton/SkeletonMapper.h). This is the "SkeletonMapper"
// piece of FALTANTES item 2: it binds a skinned animation skeleton to the
// ragdoll bodies so a pose can be driven from animation (DriveToPose) and the
// physical pose can be read back for the skinned mesh.
//
// The only TU that includes the Jolt skeleton headers is
// src/engine/physics/SkeletonPoseMapper.cpp; this header stays glm-only.
class SkeletonPoseMapper final {
public:
    SkeletonPoseMapper();
    ~SkeletonPoseMapper();
    SkeletonPoseMapper(const SkeletonPoseMapper&) = delete;
    SkeletonPoseMapper& operator=(const SkeletonPoseMapper&) = delete;

    // Builds the Jolt skeletons (animation + ragdoll) and the neutral poses,
    // then initializes the Jolt SkeletonMapper. Joints map 1-on-1 by name
    // (Jolt's default CanMapJoint). Returns false when either hierarchy is
    // empty or no joint of the ragdoll skeleton maps into the animation
    // skeleton.
    bool initialize(const Engine::SkeletonAsset& animationSkeleton,
                    const std::vector<RagdollBoneDesc>& ragdollBones);

    bool valid() const noexcept { return valid_; }

    // Number of ragdoll bones that mapped 1-on-1 into the animation skeleton.
    std::size_t mapped_bone_count() const noexcept { return mappedBoneCount_; }

    // Maps an animation pose (model-space joint matrices, one per animation
    // bone) onto the ragdoll skeleton (model space). `outRagdollPose` receives
    // one matrix per ragdoll bone, in the same order as `ragdollBones` passed
    // to initialize(). Returns false when not initialized or the input size
    // does not match the animation bone count.
    bool map_to_ragdoll(const std::vector<glm::mat4>& animationModelPose,
                        std::vector<glm::mat4>& outRagdollPose) const;

    // Reverse: maps a ragdoll pose (model space, one matrix per ragdoll bone)
    // back onto the animation skeleton. Returns false when not initialized or
    // the input size does not match the ragdoll bone count.
    bool map_reverse_to_animation(const std::vector<glm::mat4>& ragdollPose,
                                  std::vector<glm::mat4>& outAnimationPose) const;

    const std::vector<std::string>& ragdoll_bone_names() const noexcept { return boneNames_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool valid_{ false };
    std::size_t mappedBoneCount_{ 0 };
    std::vector<std::string> boneNames_;
};

} // namespace Engine::Physics

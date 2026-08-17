#include "SkeletonPoseMapper.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonMapper.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <mutex>
#include <string>

namespace Engine::Physics {

namespace {

std::once_flag gJoltSkeletonOnce;
void ensure_jolt_skeleton_registered() {
    std::call_once(gJoltSkeletonOnce, [] {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    });
}

JPH::Vec3 to_jolt_vec(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat to_jolt_quat(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
JPH::Mat44 to_jolt_mat(const glm::mat4& m) {
    return JPH::Mat44::sRotationTranslation(to_jolt_quat(glm::normalize(glm::quat_cast(m))),
                                            to_jolt_vec(glm::vec3(m[3])));
}
glm::vec3 to_glm_vec(const JPH::Vec3& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat to_glm_quat(const JPH::Quat& q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }
glm::mat4 to_glm_mat(const JPH::Mat44& m) {
    glm::mat4 r(1.0f);
    for (int c = 0; c < 4; ++c) {
        const JPH::Vec4 column = m.GetColumn4(c);
        r[c] = glm::vec4(column.GetX(), column.GetY(), column.GetZ(), column.GetW());
    }
    return r;
}

// Builds the Jolt skeleton + model-space neutral pose from the engine skeleton
// hierarchy (local transforms chained up to the root).
struct BuiltSkeleton {
    JPH::Skeleton* skeleton{ nullptr };
    std::vector<JPH::Mat44> neutralModel;  // one per joint
    std::vector<JPH::Mat44> neutralLocal;  // one per joint (animation only)
    bool valid{ false };
};

BuiltSkeleton build_animation_skeleton(const Engine::SkeletonAsset& animation) {
    BuiltSkeleton built;
    built.skeleton = new JPH::Skeleton();
    const std::size_t count = animation.bones.size();
    if (count == 0) return built;
    std::vector<int> indexByBone(animation.bones.size(), -1);
    for (std::size_t i = 0; i < animation.bones.size(); ++i) {
        indexByBone[i] = static_cast<int>(i);
    }
    // Jolt requires parents before children; the engine skeleton is authored
    // parent-before-child, but be defensive: emit in order and fix indices.
    std::vector<int> remap(animation.bones.size(), -1);
    for (std::size_t i = 0; i < animation.bones.size(); ++i) {
        const int parent = animation.bones[i].parentIndex;
        // Jolt requires parents before children; the engine skeleton is
        // authored parent-before-child, but keep a name-based fallback.
        if (parent >= 0 && parent < static_cast<int>(i)) {
            remap[i] = built.skeleton->AddJoint(animation.bones[i].name.c_str(), remap[parent]);
        } else if (parent >= 0 && parent < static_cast<int>(animation.bones.size())) {
            remap[i] = built.skeleton->AddJoint(animation.bones[i].name.c_str(),
                                                animation.bones[parent].name.c_str());
        } else {
            remap[i] = built.skeleton->AddJoint(animation.bones[i].name.c_str());
        }
    }
    built.skeleton->CalculateParentJointIndices();

    // Model-space neutral pose: chain local transforms up to the root.
    std::vector<glm::mat4> model(count, glm::mat4(1.0f));
    std::vector<glm::mat4> local(count, glm::mat4(1.0f));
    for (std::size_t i = 0; i < count; ++i) {
        const glm::mat4 localMat = animation.bones[i].localTransform;
        local[i] = localMat;
        const int parent = animation.bones[i].parentIndex;
        model[i] = (parent >= 0 && parent < static_cast<int>(count)) ? model[parent] * localMat : localMat;
    }
    built.neutralModel.reserve(count);
    built.neutralLocal.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        built.neutralModel.push_back(to_jolt_mat(model[i]));
        built.neutralLocal.push_back(to_jolt_mat(local[i]));
    }
    built.valid = built.skeleton->GetJointCount() == static_cast<int>(count);
    return built;
}

BuiltSkeleton build_ragdoll_skeleton(const std::vector<RagdollBoneDesc>& bones) {
    BuiltSkeleton built;
    built.skeleton = new JPH::Skeleton();
    if (bones.empty()) return built;
    std::vector<int> jointIndex(bones.size(), -1);
    for (std::size_t i = 0; i < bones.size(); ++i) {
        jointIndex[i] = built.skeleton->AddJoint(bones[i].name.c_str(), -1);
    }
    // Parents must exist and come before children; the ragdoll bones are
    // ordered parent-before-child by build_ragdoll_bones, but wire parents by
    // name here for robustness.
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].parent.empty()) continue;
        int parent = -1;
        for (std::size_t j = 0; j < bones.size(); ++j) {
            if (bones[j].name == bones[i].parent) { parent = static_cast<int>(j); break; }
        }
        if (parent >= 0 && parent < static_cast<int>(i)) {
            built.skeleton->GetJoints()[i].mParentJointIndex = parent;
            built.skeleton->GetJoints()[i].mParentName = bones[parent].name.c_str();
        }
    }
    built.skeleton->CalculateParentJointIndices();

    built.neutralModel.reserve(bones.size());
    for (const auto& bone : bones) {
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), bone.position) *
                                glm::mat4_cast(glm::normalize(bone.rotation));
        built.neutralModel.push_back(to_jolt_mat(model));
    }
    built.valid = built.skeleton->GetJointCount() == static_cast<int>(bones.size());
    return built;
}

} // namespace

struct SkeletonPoseMapper::Impl {
    JPH::Skeleton* animationSkeleton{ nullptr };
    JPH::Skeleton* ragdollSkeleton{ nullptr };
    JPH::SkeletonMapper* mapper{ nullptr };
    std::vector<JPH::Mat44> animationNeutralLocal;
    std::vector<JPH::Mat44> ragdollNeutralModel;
    std::size_t animationCount{ 0 };
    std::size_t ragdollCount{ 0 };
    ~Impl() {
        delete mapper;
        delete animationSkeleton;
        delete ragdollSkeleton;
    }
};

SkeletonPoseMapper::SkeletonPoseMapper() = default;
SkeletonPoseMapper::~SkeletonPoseMapper() = default;

bool SkeletonPoseMapper::initialize(const Engine::SkeletonAsset& animationSkeleton,
                                    const std::vector<RagdollBoneDesc>& ragdollBones) {
    impl_ = std::make_unique<Impl>();
    valid_ = false;
    mappedBoneCount_ = 0;
    boneNames_.clear();
    if (animationSkeleton.bones.empty() || ragdollBones.empty()) return false;

    ensure_jolt_skeleton_registered();

    const BuiltSkeleton animation = build_animation_skeleton(animationSkeleton);
    const BuiltSkeleton ragdoll = build_ragdoll_skeleton(ragdollBones);
    if (!animation.valid || !ragdoll.valid) return false;

    impl_->animationSkeleton = animation.skeleton;
    impl_->ragdollSkeleton = ragdoll.skeleton;
    impl_->animationNeutralLocal = animation.neutralLocal;
    impl_->ragdollNeutralModel = ragdoll.neutralModel;
    impl_->animationCount = animationSkeleton.bones.size();
    impl_->ragdollCount = ragdollBones.size();

    impl_->mapper = new JPH::SkeletonMapper();
    impl_->mapper->Initialize(impl_->ragdollSkeleton, ragdoll.neutralModel.data(),
                              impl_->animationSkeleton, animation.neutralModel.data(),
                              JPH::SkeletonMapper::sDefaultCanMapJoint);

    // Count 1-on-1 mappings (ragdoll bone -> animation joint).
    for (const auto& bone : ragdollBones) {
        const int mapped = impl_->mapper->GetMappedJointIdx(static_cast<int>(boneNames_.size()));
        if (mapped >= 0) ++mappedBoneCount_;
        boneNames_.push_back(bone.name);
    }
    valid_ = mappedBoneCount_ > 0;
    return valid_;
}

bool SkeletonPoseMapper::map_to_ragdoll(const std::vector<glm::mat4>& animationModelPose,
                                        std::vector<glm::mat4>& outRagdollPose) const {
    if (!valid_ || !impl_ || animationModelPose.size() != impl_->animationCount) return false;
    std::vector<JPH::Mat44> input;
    input.reserve(animationModelPose.size());
    for (const auto& mat : animationModelPose) input.push_back(to_jolt_mat(mat));
    std::vector<JPH::Mat44> output(impl_->ragdollCount);
    impl_->mapper->MapReverse(input.data(), output.data());
    outRagdollPose.clear();
    outRagdollPose.reserve(impl_->ragdollCount);
    for (const auto& mat : output) outRagdollPose.push_back(to_glm_mat(mat));
    return true;
}

bool SkeletonPoseMapper::map_reverse_to_animation(const std::vector<glm::mat4>& ragdollPose,
                                                  std::vector<glm::mat4>& outAnimationPose) const {
    if (!valid_ || !impl_ || ragdollPose.size() != impl_->ragdollCount) return false;
    std::vector<JPH::Mat44> input;
    input.reserve(ragdollPose.size());
    for (const auto& mat : ragdollPose) input.push_back(to_jolt_mat(mat));
    std::vector<JPH::Mat44> output(impl_->animationCount);
    // Map needs the local-space pose of the animation skeleton for joints that
    // are not 1-on-1 mapped; the neutral local pose is the reference.
    impl_->mapper->Map(input.data(), impl_->animationNeutralLocal.data(), output.data());
    outAnimationPose.clear();
    outAnimationPose.reserve(impl_->animationCount);
    for (const auto& mat : output) outAnimationPose.push_back(to_glm_mat(mat));
    return true;
}

} // namespace Engine::Physics

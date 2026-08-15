#pragma once

#include "AnimationSystem.hpp"
#include <functional>
#include <optional>
#include <string_view>

namespace Engine {

struct TransformPose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct Pose {
    std::vector<TransformPose> local;
};

struct RootMotionDelta {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

class AnimationSampler final {
public:
    static Pose bind_pose(const SkeletonAsset& skeleton);
    static Pose sample(const SkeletonAsset& skeleton, const AnimationClip& clip, float time);
    static RootMotionDelta root_motion(const AnimationClip& clip, float previousTime, float currentTime);
    static std::vector<glm::mat4> global_matrices(const SkeletonAsset& skeleton, const Pose& pose);
};

class AnimationBlender final {
public:
    static Pose blend(const Pose& a, const Pose& b, float weight);
    static Pose additive(const Pose& base, const Pose& additivePose, float weight,
                         const std::vector<float>& boneMask = {});
};

enum class Comparison { Greater, Less, Equal, NotEqual };

struct AnimationTransition {
    std::string from;
    std::string to;
    std::string parameter;
    Comparison comparison{Comparison::Greater};
    float threshold{};
    float blendDuration{0.2f};
    bool hasExitTime{};
    float exitTime{1.0f};
};

class AnimationStateMachine final {
public:
    void add_state(std::string name, const AnimationClip* clip);
    void add_transition(AnimationTransition transition);
    bool set_initial_state(std::string_view name);
    void set_float(std::string name, float value);
    void set_bool(std::string name, bool value);
    void set_trigger(std::string name);
    void update(const SkeletonAsset& skeleton, float deltaTime);
    const Pose& pose() const noexcept { return pose_; }
    const std::string& current_state() const noexcept { return currentState_; }
    float normalized_time() const noexcept;

private:
    bool transition_satisfied(const AnimationTransition& transition) const;
    std::unordered_map<std::string, const AnimationClip*> states_;
    std::vector<AnimationTransition> transitions_;
    std::unordered_map<std::string, float> floats_;
    std::unordered_map<std::string, bool> bools_;
    std::unordered_map<std::string, bool> triggers_;
    std::string currentState_;
    std::string previousState_;
    float stateTime_{};
    float previousStateTime_{};
    float blendTime_{};
    float blendDuration_{};
    Pose pose_;
};

struct BlendTreePoint {
    float threshold{};
    const AnimationClip* clip{};
};

class BlendTree1D final {
public:
    void add(float threshold, const AnimationClip* clip);
    Pose sample(const SkeletonAsset& skeleton, float parameter, float time) const;
private:
    std::vector<BlendTreePoint> points_;
};

class AnimationSyncGroup final {
public:
    void set_normalized_time(float normalized) noexcept;
    float time_for(const AnimationClip& clip) const noexcept;
private:
    float normalizedTime_{};
};

class AnimationRetargeter final {
public:
    static Pose retarget(const SkeletonAsset& sourceSkeleton, const SkeletonAsset& targetSkeleton,
                         const Pose& sourcePose, const HumanoidRigDefinition& mapping);
};

class IKSolver final {
public:
    static bool solve_two_bone(Pose& pose, int rootBone, int endBone,
                               const glm::vec3& target, float weight);
    static bool look_at(Pose& pose, int bone, const glm::vec3& bonePosition,
                        const glm::vec3& target, const glm::vec3& forwardAxis,
                        float weight);
};

struct ProceduralAnimationLayer {
    std::vector<float> boneMask;
    float weight{1.0f};
    std::function<void(Pose&, float)> evaluate;
};

class ProceduralAnimationStack final {
public:
    void add_layer(ProceduralAnimationLayer layer);
    Pose evaluate(Pose basePose, float deltaTime) const;
private:
    std::vector<ProceduralAnimationLayer> layers_;
};

struct RagdollBody {
    int boneIndex{-1};
    glm::mat4 worldTransform{1.0f};
    float blendWeight{1.0f};
};

class RagdollPoseBridge final {
public:
    static Pose blend_physics_pose(const SkeletonAsset& skeleton, const Pose& animated,
                                   const std::vector<RagdollBody>& bodies, float globalWeight);
};

} // namespace Engine

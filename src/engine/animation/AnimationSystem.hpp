#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "../core/uuid/UUID.hpp"

namespace Engine {

struct BoneNode {
    std::string name;
    int parentIndex{ -1 };
    glm::mat4 localTransform{ 1.0f };
    glm::mat4 inverseBindMatrix{ 1.0f };
};

class SkeletonAsset {
public:
    UUID id;
    std::string name{ "Skeleton" };
    std::vector<BoneNode> bones;

    int find_bone_index(const std::string& boneName) const {
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].name == boneName) return static_cast<int>(i);
        }
        return -1;
    }
};

struct KeyFrame {
    float timeStamp{ 0.0f };
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
};

struct BoneTrack {
    int boneIndex{ 0 };
    std::vector<KeyFrame> keyFrames;
};

class AnimationClip {
public:
    UUID id;
    std::string name{ "Clip" };
    float duration{ 1.0f };
    float ticksPerSecond{ 30.0f };
    bool looping{ true };
    int rootMotionBone{ -1 };
    std::vector<BoneTrack> tracks;
};

struct HumanoidRigDefinition {
    std::unordered_map<std::string, std::string> boneMapping; // e.g. "Hips" -> "Pelvis"

    void map_bone(const std::string& sourceBone, const std::string& targetBone) {
        boneMapping[sourceBone] = targetBone;
    }
};

class AnimationGraph {
public:
    void configure(const SkeletonAsset* skeleton, const AnimationClip* idle,
                   const AnimationClip* walk, const AnimationClip* run) {
        skeleton_ = skeleton; idle_ = idle; walk_ = walk; run_ = run;
    }
    void reset() noexcept { time_ = 0.0f; }
    void update(float deltaTime, float movementSpeed, std::vector<glm::mat4>& outPose);
private:
    const SkeletonAsset* skeleton_{};
    const AnimationClip* idle_{};
    const AnimationClip* walk_{};
    const AnimationClip* run_{};
    float time_{};
};

} // namespace Engine

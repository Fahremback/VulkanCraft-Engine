#pragma once

#include "PhysicsRuntime.hpp"
#include "../animation/AnimationSystem.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Physics {

struct RagdollBoneDesc {
    std::string name;
    std::string parent;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float length{0.5f};
    float radius{0.12f};
    float mass{1.0f};
    glm::vec3 jointAnchor{0.0f};
};

struct RagdollPoseBone {
    std::string name;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Builds RagdollBoneDesc from a real skeleton: each bone becomes a capsule at
// its world position (local transforms chained up the hierarchy), length from
// the parent->child offset, and parent pointers from parentIndex. This is how
// the editor play world and the game turn an authored skin into ragdoll
// physics (Fase 6).
std::vector<RagdollBoneDesc> build_ragdoll_bones(const SkeletonAsset& skeleton, float massPerBone = 1.0f);

class Ragdoll final {
public:
    bool create(PhysicsRuntime& world, const std::vector<RagdollBoneDesc>& bones,
                const glm::vec3& rootPosition, std::uint32_t collisionLayer = 1u,
                std::uint32_t collisionMask = ~0u);
    void destroy(PhysicsRuntime& world);
    void apply_impulse(PhysicsRuntime& world, const std::string& bone, const glm::vec3& impulse);
    void set_awake(PhysicsRuntime& world, bool awake);
    std::vector<RagdollPoseBone> pose(const PhysicsRuntime& world) const;
    BodyHandle bone_body(const std::string& bone) const;
    bool empty() const noexcept { return bodies_.empty(); }

private:
    std::vector<std::string> order_;
    std::unordered_map<std::string, BodyHandle> bodies_;
    std::vector<ConstraintHandle> joints_;
};

} // namespace Engine::Physics

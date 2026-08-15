#include "Ragdoll.hpp"

#include <algorithm>
#include <unordered_set>

namespace Engine::Physics {

std::vector<RagdollBoneDesc> build_ragdoll_bones(const SkeletonAsset& skeleton, float massPerBone) {
    std::vector<RagdollBoneDesc> bones;
    if (skeleton.bones.empty()) return bones;
    std::vector<glm::vec3> worldPositions(skeleton.bones.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        glm::vec3 world = glm::vec3(skeleton.bones[i].localTransform[3]);
        int parent = skeleton.bones[i].parentIndex;
        // Chain up: accumulate world offsets from ancestors' local translations.
        while (parent >= 0 && parent < static_cast<int>(skeleton.bones.size())) {
            world += glm::vec3(skeleton.bones[parent].localTransform[3]);
            parent = skeleton.bones[parent].parentIndex;
        }
        worldPositions[i] = world;
    }
    bones.reserve(skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        RagdollBoneDesc desc;
        desc.name = skeleton.bones[i].name.empty() ? ("Bone" + std::to_string(i)) : skeleton.bones[i].name;
        const int parentIndex = skeleton.bones[i].parentIndex;
        desc.parent = (parentIndex >= 0 && parentIndex < static_cast<int>(skeleton.bones.size()))
                          ? skeleton.bones[parentIndex].name
                          : std::string();
        desc.position = worldPositions[i];
        const glm::quat rotation = glm::quat_cast(skeleton.bones[i].localTransform);
        desc.rotation = glm::isnan(rotation.w) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : rotation;
        // Capsule length: distance to the first child (or a unit fallback).
        desc.length = 0.5f;
        for (std::size_t j = 0; j < skeleton.bones.size(); ++j) {
            if (skeleton.bones[j].parentIndex != static_cast<int>(i)) continue;
            const float childDistance = glm::length(worldPositions[j] - worldPositions[i]);
            if (childDistance > 0.01f) { desc.length = childDistance; break; }
        }
        desc.radius = 0.12f;
        desc.mass = massPerBone;
        bones.push_back(std::move(desc));
    }
    return bones;
}

bool Ragdoll::create(PhysicsRuntime& world, const std::vector<RagdollBoneDesc>& bones,
                     const glm::vec3& rootPosition, std::uint32_t collisionLayer,
                     std::uint32_t collisionMask) {
    if (!bodies_.empty() || bones.empty()) return false;
    std::unordered_set<std::string> names;
    for (const auto& bone : bones) {
        if (bone.name.empty() || !names.insert(bone.name).second || bone.length <= 0.0f || bone.radius <= 0.0f || bone.mass <= 0.0f) return false;
    }
    for (const auto& bone : bones) if (!bone.parent.empty() && !names.contains(bone.parent)) return false;

    for (const auto& bone : bones) {
        BodyDesc body;
        body.position = rootPosition + bone.position;
        body.rotation = bone.rotation;
        body.mass = bone.mass;
        body.linearDamping = 0.12f;
        body.angularDamping = 0.2f;
        body.allowSleep = true;
        body.continuous = true;
        body.collider.shape = CapsuleShape{bone.radius, std::max(0.0f, bone.length * 0.5f - bone.radius)};
        body.collider.friction = 0.65f;
        body.collider.restitution = 0.02f;
        // Each bone gets its own collision layer (parent/child pairs are masked
        // out below so joints don't fight the capsule collisions).
        body.collider.filter = {collisionLayer + static_cast<std::uint32_t>(order_.size()), collisionMask};
        const BodyHandle handle = world.create_body(body);
        if (handle == InvalidBody) { destroy(world); return false; }
        order_.push_back(bone.name);
        bodies_.emplace(bone.name, handle);
    }
    // Disable collisions between directly connected bones (ragdoll joints keep
    // the separation; colliding capsules + a distance constraint is unstable).
    for (size_t i = 0; i < bones.size(); ++i) {
        const std::string& name = bones[i].name;
        RigidBody* body = world.body(bodies_.at(name));
        if (!body) { destroy(world); return false; }
        std::uint32_t mask = collisionMask;
        for (size_t j = 0; j < bones.size(); ++j) {
            if (j == i) continue;
            const bool connected = bones[j].name == bones[i].parent || bones[i].name == bones[j].parent;
            if (connected) mask &= ~(collisionLayer + static_cast<std::uint32_t>(j));
        }
        body->collider.filter = {collisionLayer + static_cast<std::uint32_t>(i), mask};
    }
    for (const auto& bone : bones) if (!bone.parent.empty()) {
        const RigidBody* child = world.body(bodies_.at(bone.name));
        const RigidBody* parent = world.body(bodies_.at(bone.parent));
        if (!child || !parent) { destroy(world); return false; }
        const glm::vec3 worldAnchor = rootPosition + bone.position + bone.rotation * bone.jointAnchor;
        DistanceConstraintDesc joint;
        joint.bodyA = parent->handle;
        joint.bodyB = child->handle;
        joint.localAnchorA = glm::inverse(parent->rotation) * (worldAnchor - parent->position);
        joint.localAnchorB = glm::inverse(child->rotation) * (worldAnchor - child->position);
        // Rest length = the initial bone separation, so the joint holds the
        // chain together without fighting zero-distance capsules.
        joint.restLength = glm::length(bone.position - [&]() -> glm::vec3 {
            for (const auto& other : bones) if (other.name == bone.parent) return other.position;
            return glm::vec3(0.0f);
        }());
        joint.stiffness = 0.6f;
        joint.damping = 0.4f;
        const ConstraintHandle handle = world.create_distance_constraint(joint);
        if (handle == InvalidConstraint) { destroy(world); return false; }
        joints_.push_back(handle);
    }
    return true;
}

void Ragdoll::destroy(PhysicsRuntime& world) {
    for (ConstraintHandle joint : joints_) world.destroy_constraint(joint);
    for (const auto& [name, handle] : bodies_) world.destroy_body(handle);
    joints_.clear(); bodies_.clear(); order_.clear();
}

void Ragdoll::apply_impulse(PhysicsRuntime& world, const std::string& bone, const glm::vec3& impulse) {
    if (const auto found = bodies_.find(bone); found != bodies_.end()) world.apply_impulse(found->second, impulse);
}

void Ragdoll::set_awake(PhysicsRuntime& world, bool awake) {
    for (const auto& [name, handle] : bodies_) if (RigidBody* body = world.body(handle)) {
        body->sleeping = !awake;
        body->sleepTimer = 0.0f;
        if (awake) world.wake(handle); else { body->linearVelocity = {}; body->angularVelocity = {}; }
    }
}

std::vector<RagdollPoseBone> Ragdoll::pose(const PhysicsRuntime& world) const {
    std::vector<RagdollPoseBone> result;
    result.reserve(order_.size());
    for (const std::string& name : order_) if (const RigidBody* body = world.body(bodies_.at(name))) {
        result.push_back({name, body->position, body->rotation});
    }
    return result;
}

BodyHandle Ragdoll::bone_body(const std::string& bone) const {
    const auto found = bodies_.find(bone);
    return found == bodies_.end() ? InvalidBody : found->second;
}

} // namespace Engine::Physics

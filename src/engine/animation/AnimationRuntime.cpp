#include "AnimationRuntime.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

namespace Engine {
namespace {
float clip_time(const AnimationClip& clip, float time) {
    if (clip.duration <= 0.0f) return 0.0f;
    if (clip.looping) {
        time = std::fmod(time, clip.duration);
        if (time < 0.0f) time += clip.duration;
        return time;
    }
    return std::clamp(time, 0.0f, clip.duration);
}

TransformPose decompose(const glm::mat4& matrix) {
    TransformPose result;
    result.translation = glm::vec3(matrix[3]);
    result.scale = {glm::length(glm::vec3(matrix[0])), glm::length(glm::vec3(matrix[1])), glm::length(glm::vec3(matrix[2]))};
    glm::mat3 rotation(matrix);
    if (result.scale.x > 0) rotation[0] /= result.scale.x;
    if (result.scale.y > 0) rotation[1] /= result.scale.y;
    if (result.scale.z > 0) rotation[2] /= result.scale.z;
    result.rotation = glm::normalize(glm::quat_cast(rotation));
    return result;
}

glm::quat rotation_between(glm::vec3 from, glm::vec3 to) {
    from = glm::normalize(from); to = glm::normalize(to);
    const float cosine = glm::dot(from, to);
    if (cosine > 0.9999f) return glm::quat(1,0,0,0);
    if (cosine < -0.9999f) {
        glm::vec3 axis = glm::cross(from, glm::vec3(1,0,0));
        if (glm::dot(axis, axis) < 0.0001f) axis = glm::cross(from, glm::vec3(0,1,0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

glm::mat4 compose(const TransformPose& transform) {
    return glm::translate(glm::mat4(1.0f), transform.translation) * glm::mat4_cast(transform.rotation) *
           glm::scale(glm::mat4(1.0f), transform.scale);
}

TransformPose sample_track(const BoneTrack& track, float time) {
    if (track.keyFrames.empty()) return {};
    if (time <= track.keyFrames.front().timeStamp) {
        const auto& key = track.keyFrames.front();
        return {key.position, key.rotation, key.scale};
    }
    if (time >= track.keyFrames.back().timeStamp) {
        const auto& key = track.keyFrames.back();
        return {key.position, key.rotation, key.scale};
    }
    auto upper = std::upper_bound(track.keyFrames.begin(), track.keyFrames.end(), time,
        [](float value, const KeyFrame& key) { return value < key.timeStamp; });
    const KeyFrame& b = *upper;
    const KeyFrame& a = *(upper - 1);
    const float range = b.timeStamp - a.timeStamp;
    const float alpha = range > 0.0f ? (time - a.timeStamp) / range : 0.0f;
    return {glm::mix(a.position, b.position, alpha), glm::normalize(glm::slerp(a.rotation, b.rotation, alpha)),
            glm::mix(a.scale, b.scale, alpha)};
}
} // namespace

Pose AnimationSampler::bind_pose(const SkeletonAsset& skeleton) {
    Pose pose;
    pose.local.reserve(skeleton.bones.size());
    for (const BoneNode& bone : skeleton.bones) pose.local.push_back(decompose(bone.localTransform));
    return pose;
}

Pose AnimationSampler::sample(const SkeletonAsset& skeleton, const AnimationClip& clip, float time) {
    Pose pose = bind_pose(skeleton);
    const float localTime = clip_time(clip, time);
    for (const BoneTrack& track : clip.tracks)
        if (track.boneIndex >= 0 && static_cast<size_t>(track.boneIndex) < pose.local.size())
            pose.local[track.boneIndex] = sample_track(track, localTime);
    return pose;
}

RootMotionDelta AnimationSampler::root_motion(const AnimationClip& clip, float previousTime, float currentTime) {
    if (clip.rootMotionBone < 0) return {};
    const auto found = std::find_if(clip.tracks.begin(), clip.tracks.end(), [&](const BoneTrack& track) {
        return track.boneIndex == clip.rootMotionBone;
    });
    if (found == clip.tracks.end()) return {};
    const TransformPose previous = sample_track(*found, clip_time(clip, previousTime));
    const TransformPose current = sample_track(*found, clip_time(clip, currentTime));
    return {current.translation - previous.translation, glm::normalize(current.rotation * glm::inverse(previous.rotation))};
}

std::vector<glm::mat4> AnimationSampler::global_matrices(const SkeletonAsset& skeleton, const Pose& pose) {
    std::vector<glm::mat4> result(pose.local.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < pose.local.size(); ++i) {
        const glm::mat4 local = compose(pose.local[i]);
        const int parent = i < skeleton.bones.size() ? skeleton.bones[i].parentIndex : -1;
        result[i] = parent >= 0 && static_cast<size_t>(parent) < i ? result[parent] * local : local;
    }
    return result;
}

Pose AnimationBlender::blend(const Pose& a, const Pose& b, float weight) {
    weight = std::clamp(weight, 0.0f, 1.0f);
    Pose result;
    const size_t count = std::min(a.local.size(), b.local.size());
    result.local.resize(count);
    for (size_t i = 0; i < count; ++i) result.local[i] = {
        glm::mix(a.local[i].translation, b.local[i].translation, weight),
        glm::normalize(glm::slerp(a.local[i].rotation, b.local[i].rotation, weight)),
        glm::mix(a.local[i].scale, b.local[i].scale, weight)};
    return result;
}

Pose AnimationBlender::additive(const Pose& base, const Pose& additivePose, float weight, const std::vector<float>& mask) {
    Pose result = base;
    for (size_t i = 0; i < std::min(result.local.size(), additivePose.local.size()); ++i) {
        const float w = std::clamp(weight * (i < mask.size() ? mask[i] : 1.0f), 0.0f, 1.0f);
        result.local[i].translation += additivePose.local[i].translation * w;
        result.local[i].rotation = glm::normalize(result.local[i].rotation * glm::slerp(glm::quat(1,0,0,0), additivePose.local[i].rotation, w));
        result.local[i].scale += (additivePose.local[i].scale - glm::vec3(1.0f)) * w;
    }
    return result;
}

void AnimationStateMachine::add_state(std::string name, const AnimationClip* clip) { if (clip) states_[std::move(name)] = clip; }
void AnimationStateMachine::add_transition(AnimationTransition transition) { transitions_.push_back(std::move(transition)); }
bool AnimationStateMachine::set_initial_state(std::string_view name) {
    if (!states_.contains(std::string(name))) return false;
    currentState_ = name; stateTime_ = 0.0f; previousState_.clear(); return true;
}
void AnimationStateMachine::set_float(std::string name, float value) { floats_[std::move(name)] = value; }
void AnimationStateMachine::set_bool(std::string name, bool value) { bools_[std::move(name)] = value; }
void AnimationStateMachine::set_trigger(std::string name) { triggers_[std::move(name)] = true; }
float AnimationStateMachine::normalized_time() const noexcept {
    const auto found = states_.find(currentState_);
    return found == states_.end() || found->second->duration <= 0 ? 0.0f : stateTime_ / found->second->duration;
}
bool AnimationStateMachine::transition_satisfied(const AnimationTransition& transition) const {
    if (transition.hasExitTime && normalized_time() < transition.exitTime) return false;
    if (auto trigger = triggers_.find(transition.parameter); trigger != triggers_.end() && trigger->second) return true;
    if (auto boolean = bools_.find(transition.parameter); boolean != bools_.end())
        return transition.comparison == Comparison::Equal ? boolean->second == (transition.threshold != 0) : boolean->second != (transition.threshold != 0);
    const float value = floats_.contains(transition.parameter) ? floats_.at(transition.parameter) : 0.0f;
    switch (transition.comparison) {
        case Comparison::Greater: return value > transition.threshold;
        case Comparison::Less: return value < transition.threshold;
        case Comparison::Equal: return std::abs(value - transition.threshold) < 0.0001f;
        case Comparison::NotEqual: return std::abs(value - transition.threshold) >= 0.0001f;
    }
    return false;
}
void AnimationStateMachine::update(const SkeletonAsset& skeleton, float deltaTime) {
    if (currentState_.empty() || !states_.contains(currentState_)) { pose_ = AnimationSampler::bind_pose(skeleton); return; }
    stateTime_ += std::max(deltaTime, 0.0f);
    if (previousState_.empty()) {
        for (const auto& transition : transitions_) if (transition.from == currentState_ && states_.contains(transition.to) && transition_satisfied(transition)) {
            previousState_ = currentState_; previousStateTime_ = stateTime_; currentState_ = transition.to;
            stateTime_ = 0.0f; blendTime_ = 0.0f; blendDuration_ = std::max(transition.blendDuration, 0.0f);
            triggers_[transition.parameter] = false; break;
        }
    }
    pose_ = AnimationSampler::sample(skeleton, *states_.at(currentState_), stateTime_);
    if (!previousState_.empty()) {
        blendTime_ += std::max(deltaTime, 0.0f);
        const Pose previous = AnimationSampler::sample(skeleton, *states_.at(previousState_), previousStateTime_ + blendTime_);
        pose_ = AnimationBlender::blend(previous, pose_, blendDuration_ > 0 ? blendTime_ / blendDuration_ : 1.0f);
        if (blendTime_ >= blendDuration_) previousState_.clear();
    }
}

void BlendTree1D::add(float threshold, const AnimationClip* clip) {
    if (!clip) return; points_.push_back({threshold, clip});
    std::sort(points_.begin(), points_.end(), [](auto& a, auto& b) { return a.threshold < b.threshold; });
}
Pose BlendTree1D::sample(const SkeletonAsset& skeleton, float parameter, float time) const {
    if (points_.empty()) return AnimationSampler::bind_pose(skeleton);
    if (parameter <= points_.front().threshold) return AnimationSampler::sample(skeleton, *points_.front().clip, time);
    if (parameter >= points_.back().threshold) return AnimationSampler::sample(skeleton, *points_.back().clip, time);
    auto upper = std::upper_bound(points_.begin(), points_.end(), parameter, [](float value, const BlendTreePoint& p) { return value < p.threshold; });
    const auto& b = *upper; const auto& a = *(upper - 1);
    return AnimationBlender::blend(AnimationSampler::sample(skeleton, *a.clip, time), AnimationSampler::sample(skeleton, *b.clip, time),
                                   (parameter - a.threshold) / (b.threshold - a.threshold));
}
void AnimationSyncGroup::set_normalized_time(float normalized) noexcept { normalizedTime_ = normalized - std::floor(normalized); }
float AnimationSyncGroup::time_for(const AnimationClip& clip) const noexcept { return normalizedTime_ * clip.duration; }

Pose AnimationRetargeter::retarget(const SkeletonAsset& source, const SkeletonAsset& target, const Pose& sourcePose,
                                   const HumanoidRigDefinition& mapping) {
    Pose result = AnimationSampler::bind_pose(target);
    for (const auto& [sourceName, targetName] : mapping.boneMapping) {
        const int sourceIndex = source.find_bone_index(sourceName), targetIndex = target.find_bone_index(targetName);
        if (sourceIndex >= 0 && targetIndex >= 0 && static_cast<size_t>(sourceIndex) < sourcePose.local.size()) result.local[targetIndex] = sourcePose.local[sourceIndex];
    }
    return result;
}

bool IKSolver::solve_two_bone(Pose& pose, int root, int end, const glm::vec3& target, float weight) {
    if (root < 0 || end < 0 || static_cast<size_t>(root) >= pose.local.size() || static_cast<size_t>(end) >= pose.local.size()) return false;
    weight = std::clamp(weight, 0.0f, 1.0f);
    pose.local[end].translation = glm::mix(pose.local[end].translation, target, weight);
    const glm::vec3 direction = target - pose.local[root].translation;
    if (glm::dot(direction, direction) > 0.000001f) pose.local[root].rotation = glm::normalize(glm::slerp(pose.local[root].rotation,
        rotation_between(glm::vec3(0,1,0), glm::normalize(direction)), weight));
    return true;
}
bool IKSolver::look_at(Pose& pose, int bone, const glm::vec3& position, const glm::vec3& target, const glm::vec3& forward, float weight) {
    if (bone < 0 || static_cast<size_t>(bone) >= pose.local.size() || glm::dot(target-position, target-position) < 0.000001f) return false;
    pose.local[bone].rotation = glm::normalize(glm::slerp(pose.local[bone].rotation, rotation_between(glm::normalize(forward), glm::normalize(target-position)), std::clamp(weight,0.0f,1.0f)));
    return true;
}
void ProceduralAnimationStack::add_layer(ProceduralAnimationLayer layer) { layers_.push_back(std::move(layer)); }
Pose ProceduralAnimationStack::evaluate(Pose base, float deltaTime) const {
    for (const auto& layer : layers_) if (layer.evaluate && layer.weight > 0) { Pose modified = base; layer.evaluate(modified, deltaTime); base = AnimationBlender::blend(base, modified, layer.weight); }
    return base;
}
Pose RagdollPoseBridge::blend_physics_pose(const SkeletonAsset& skeleton, const Pose& animated, const std::vector<RagdollBody>& bodies, float globalWeight) {
    Pose physics = animated;
    for (const auto& body : bodies) if (body.boneIndex >= 0 && static_cast<size_t>(body.boneIndex) < physics.local.size()) physics.local[body.boneIndex] = decompose(body.worldTransform);
    return AnimationBlender::blend(animated, physics, globalWeight);
}

void AnimationGraph::update(float deltaTime, float movementSpeed, std::vector<glm::mat4>& outPose) {
    if (!skeleton_ || !idle_) { outPose.clear(); return; }
    time_ += std::max(deltaTime, 0.0f);
    Pose pose;
    if (movementSpeed <= 0.0f || !walk_) {
        pose = AnimationSampler::sample(*skeleton_, *idle_, time_);
    } else if (movementSpeed < 1.0f || !run_) {
        pose = AnimationBlender::blend(AnimationSampler::sample(*skeleton_, *idle_, time_),
                                       AnimationSampler::sample(*skeleton_, *walk_, time_),
                                       std::clamp(movementSpeed, 0.0f, 1.0f));
    } else {
        pose = AnimationBlender::blend(AnimationSampler::sample(*skeleton_, *walk_, time_),
                                       AnimationSampler::sample(*skeleton_, *run_, time_),
                                       std::clamp(movementSpeed - 1.0f, 0.0f, 1.0f));
    }
    outPose = AnimationSampler::global_matrices(*skeleton_, pose);
    for (size_t i = 0; i < outPose.size() && i < skeleton_->bones.size(); ++i)
        outPose[i] *= skeleton_->bones[i].inverseBindMatrix;
}

} // namespace Engine

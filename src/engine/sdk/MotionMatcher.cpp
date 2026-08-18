// MotionMatcher (FALTANTES §18 item 9): the ONLY TU implementing the public
// IMotionMatcher contract. Motion matching inspired by the MIT-licensed ozz
// `sample_motion_matching` (reference only, never compiled) and the classic
// feature-vector design: the future TRAJECTORY, the root VELOCITY and the POSE
// are compared RELATIVE to the character's facing at each frame; the search is
// exhaustive with a deterministic tie-break. PURE and DETERMINISTIC: identical
// (clips, spec, query) produce identical results — no hidden state, no RNG.
#include "engine/animation/IMotionMatcher.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace engine {
namespace animation {

bool MotionMatcherSpec::validate(std::string& errorOut) const {
    if (trajectoryWeight < 0.0f || velocityWeight < 0.0f || poseWeight < 0.0f ||
        !std::isfinite(trajectoryWeight) || !std::isfinite(velocityWeight) ||
        !std::isfinite(poseWeight)) {
        errorOut = "motion matcher refused: feature weights must be >= 0";
        return false;
    }
    if (trajectoryPoints < 1) {
        errorOut = "motion matcher refused: trajectoryPoints must be >= 1";
        return false;
    }
    if (!std::isfinite(trajectoryHorizon) || trajectoryHorizon <= 0.0f) {
        errorOut = "motion matcher refused: trajectoryHorizon must be > 0";
        return false;
    }
    for (const int bone : poseBones) {
        if (bone < 0) {
            errorOut = "motion matcher refused: negative poseBone index";
            return false;
        }
    }
    return true;
}

bool MotionClipEntry::validate(const MotionSkeleton& skeleton,
                               std::string& errorOut) const {
    if (name.empty()) {
        errorOut = "motion matcher refused: clip name empty";
        return false;
    }
    if (frames.empty()) {
        errorOut = "motion matcher refused: clip has no frames";
        return false;
    }
    if (rootPositions.size() != frames.size() ||
        rootOrientations.size() != frames.size()) {
        errorOut = "motion matcher refused: clip root arrays must be "
                   "frame-sized";
        return false;
    }
    if (!std::isfinite(frameRate) || frameRate <= 0.0f) {
        errorOut = "motion matcher refused: frameRate must be > 0";
        return false;
    }
    for (const MotionPose& pose : frames) {
        if (pose.rotations.size() != skeleton.bones.size()) {
            errorOut = "motion matcher refused: clip pose not sized to the "
                       "skeleton";
            return false;
        }
    }
    return true;
}

bool MotionMatchQuery::validate(const MotionSkeleton& skeleton,
                                const MotionMatcherSpec& spec,
                                std::string& errorOut) const {
    if (!std::isfinite(rootPosition.x) || !std::isfinite(rootPosition.y) ||
        !std::isfinite(rootPosition.z) || !std::isfinite(rootVelocity.x) ||
        !std::isfinite(rootVelocity.y) || !std::isfinite(rootVelocity.z)) {
        errorOut = "motion matcher refused: non-finite query root";
        return false;
    }
    if (trajectory.size() != static_cast<std::size_t>(spec.trajectoryPoints)) {
        errorOut = "motion matcher refused: query trajectory must have "
                   "spec.trajectoryPoints samples";
        return false;
    }
    for (const glm::vec3& p : trajectory) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            errorOut = "motion matcher refused: non-finite trajectory point";
            return false;
        }
    }
    if (!pose.translations.empty() &&
        pose.rotations.size() != skeleton.bones.size()) {
        errorOut = "motion matcher refused: query pose not sized to the "
                   "skeleton";
        return false;
    }
    return true;
}

namespace {

// Rotates a world vector into the character's facing frame (forward = +Z).
glm::vec3 to_facing(const glm::vec3& v, const glm::quat& facing) {
    return glm::inverse(facing) * v;
}

// Query trajectory relative to the query root (position + facing).
std::vector<glm::vec3> query_trajectory_rel(const MotionMatchQuery& q) {
    std::vector<glm::vec3> rel(q.trajectory.size());
    for (std::size_t i = 0; i < q.trajectory.size(); ++i) {
        rel[i] = to_facing(q.trajectory[i] - q.rootPosition, q.rootOrientation);
    }
    return rel;
}

// Clip trajectory at frame `f`: the root positions at (k+1)*horizon/points
// seconds ahead (wrapped when the clip loops), relative to the frame's root
// position and facing.
std::vector<glm::vec3> clip_trajectory_rel(const MotionClipEntry& clip, int f,
                                           int points, float horizon) {
    std::vector<glm::vec3> rel(static_cast<std::size_t>(points));
    const int frames = static_cast<int>(clip.frames.size());
    for (int i = 0; i < points; ++i) {
        const float offsetSeconds =
            static_cast<float>(i + 1) * horizon / static_cast<float>(points);
        const int idx = static_cast<int>(
            std::lround(offsetSeconds * clip.frameRate));
        int target = f + idx;
        if (clip.loop) {
            target %= frames;
        } else {
            target = std::min(target, frames - 1);
        }
        rel[static_cast<std::size_t>(i)] = to_facing(
            clip.rootPositions[static_cast<std::size_t>(target)] -
                clip.rootPositions[static_cast<std::size_t>(f)],
            clip.rootOrientations[static_cast<std::size_t>(f)]);
    }
    return rel;
}

// Mean squared per-point error of two equal-sized relative trajectories.
float trajectory_cost(const std::vector<glm::vec3>& a,
                      const std::vector<glm::vec3>& b) {
    float sum = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const glm::vec3 d = a[i] - b[i];
        sum += glm::dot(d, d);
    }
    return sum / static_cast<float>(a.size());
}

// Mean squared per-component error of two facing-frame velocities.
float velocity_cost(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = a - b;
    return (d.x * d.x + d.y * d.y + d.z * d.z) / 3.0f;
}

// Mean over the selected bones of (1 - |dot|) between the query pose and a
// clip-frame pose (local rotations; 0 = identical, 1 = orthogonal).
float pose_cost(const MotionPose& query, const MotionPose& clip,
                const std::vector<int>& poseBones) {
    const float boneCount =
        poseBones.empty()
            ? static_cast<float>(query.rotations.size())
            : static_cast<float>(poseBones.size());
    float sum = 0.0f;
    if (poseBones.empty()) {
        for (std::size_t i = 0; i < query.rotations.size(); ++i) {
            const float d =
                std::fabs(glm::dot(query.rotations[i], clip.rotations[i]));
            sum += 1.0f - std::min(1.0f, d);
        }
    } else {
        for (const int bone : poseBones) {
            const float d = std::fabs(
                glm::dot(query.rotations[static_cast<std::size_t>(bone)],
                         clip.rotations[static_cast<std::size_t>(bone)]));
            sum += 1.0f - std::min(1.0f, d);
        }
    }
    return sum / boneCount;
}

class MotionMatcherImpl final : public IMotionMatcher {
public:
    bool build(const MotionSkeleton& skeleton,
               const std::vector<MotionClipEntry>& clips,
               const MotionMatcherSpec& spec,
               std::string& errorOut) override {
        std::string err;
        if (!skeleton.validate(err)) {
            errorOut = err;
            return false;
        }
        if (!spec.validate(err)) {
            errorOut = err;
            return false;
        }
        for (const int bone : spec.poseBones) {
            if (static_cast<std::size_t>(bone) >= skeleton.bones.size()) {
                errorOut = "motion matcher refused: poseBone index out of "
                           "range";
                return false;
            }
        }
        if (clips.empty()) {
            errorOut = "motion matcher refused: no clips";
            return false;
        }
        for (const MotionClipEntry& clip : clips) {
            if (!clip.validate(skeleton, err)) {
                errorOut = err;
                return false;
            }
        }
        skeleton_ = skeleton;
        spec_ = spec;
        clips_ = clips;
        return true;
    }

    bool match(const MotionMatchQuery& query, MotionMatchResult& out,
               std::string& errorOut) const override {
        std::string err;
        if (clips_.empty()) {
            errorOut = "motion matcher refused: no clips built";
            return false;
        }
        if (!query.validate(skeleton_, spec_, err)) {
            errorOut = err;
            return false;
        }
        const bool usePose =
            !query.pose.translations.empty() &&
            query.pose.rotations.size() == skeleton_.bones.size();
        const std::vector<glm::vec3> qTraj = query_trajectory_rel(query);
        const glm::vec3 qVel =
            to_facing(query.rootVelocity, query.rootOrientation);

        int bestClip = -1;
        int bestFrame = 0;
        float bestCost = std::numeric_limits<float>::infinity();
        for (std::size_t c = 0; c < clips_.size(); ++c) {
            const MotionClipEntry& clip = clips_[c];
            const int frames = static_cast<int>(clip.frames.size());
            for (int f = 0; f < frames; ++f) {
                const std::vector<glm::vec3> cTraj = clip_trajectory_rel(
                    clip, f, spec_.trajectoryPoints, spec_.trajectoryHorizon);
                const std::size_t fNext =
                    clip.loop
                        ? static_cast<std::size_t>((f + 1) % frames)
                        : static_cast<std::size_t>(std::min(f + 1, frames - 1));
                const glm::vec3 cVel = to_facing(
                    (clip.rootPositions[fNext] -
                     clip.rootPositions[static_cast<std::size_t>(f)]) *
                        clip.frameRate,
                    clip.rootOrientations[static_cast<std::size_t>(f)]);
                const float cost =
                    spec_.trajectoryWeight *
                        trajectory_cost(qTraj, cTraj) +
                    spec_.velocityWeight *
                        velocity_cost(qVel, cVel) +
                    (usePose
                         ? spec_.poseWeight *
                               pose_cost(query.pose,
                                         clip.frames[static_cast<std::size_t>(f)],
                                         spec_.poseBones)
                         : 0.0f);
                if (cost < bestCost) {  // strict: deterministic tie-break
                    bestCost = cost;
                    bestClip = static_cast<int>(c);
                    bestFrame = f;
                }
            }
        }
        out.clip = bestClip;
        out.frame = bestFrame;
        out.cost = bestCost;
        return bestClip >= 0;
    }

    std::size_t clip_count() const noexcept override { return clips_.size(); }

    std::size_t frame_count(std::size_t clip) const noexcept override {
        return clip < clips_.size() ? clips_[clip].frames.size() : 0;
    }

private:
    MotionSkeleton skeleton_;
    MotionMatcherSpec spec_;
    std::vector<MotionClipEntry> clips_;
};

}  // namespace

std::unique_ptr<IMotionMatcher> create_motion_matcher() {
    return std::make_unique<MotionMatcherImpl>();
}

}  // namespace animation
}  // namespace engine

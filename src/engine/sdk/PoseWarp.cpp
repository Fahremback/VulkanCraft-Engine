// PoseWarp (FALTANTES §18 item 6): the ONLY TU implementing the public
// IPoseWarper contract. Adapts a sampled pose to the creature's live
// locomotion state — root snapped to (body position, yaw, terrain surface),
// planted feet pinned toward the item-5 placed targets (local-space delta in
// the parent frame, clamped per warp), optional speed-based forward lean.
// Pure + deterministic (glm only).
#include "engine/animation/IPoseWarper.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace engine {
namespace animation {

bool PoseWarpSpec::validate(std::string& errorOut) const {
    if (!std::isfinite(maxFootMove) || maxFootMove <= 0.0f) {
        errorOut = "pose warp refused: maxFootMove must be > 0";
        return false;
    }
    if (!std::isfinite(rootWeight) || rootWeight < 0.0f || rootWeight > 1.0f ||
        !std::isfinite(footWeight) || footWeight < 0.0f || footWeight > 1.0f) {
        errorOut = "pose warp refused: rootWeight/footWeight must be in [0,1]";
        return false;
    }
    if (!std::isfinite(leanFactor) || leanFactor < 0.0f) {
        errorOut = "pose warp refused: leanFactor must be >= 0";
        return false;
    }
    return true;
}

namespace {

class PoseWarperImpl final : public IPoseWarper {
public:
    bool warp(const MotionSkeleton& skeleton, const MotionPose& pose,
              const PoseWarpSpec& spec, const WarpInput& input,
              MotionPose& out, std::string& errorOut) override {
        out.translations.clear();
        out.rotations.clear();
        out.scales.clear();
        std::string err;
        if (!spec.validate(err)) {
            errorOut = err;
            return false;
        }
        if (skeleton.bones.empty()) {
            errorOut = "pose warp refused: empty skeleton";
            return false;
        }
        if (pose.translations.size() != skeleton.bones.size() ||
            pose.rotations.size() != skeleton.bones.size() ||
            pose.scales.size() != skeleton.bones.size()) {
            errorOut = "pose warp refused: pose size mismatch with skeleton";
            return false;
        }
        if (!std::isfinite(input.bodyPosition.x) ||
            !std::isfinite(input.bodyPosition.y) ||
            !std::isfinite(input.bodyPosition.z) ||
            !std::isfinite(input.bodyYaw) || !std::isfinite(input.surfaceHeight) ||
            !std::isfinite(input.speed)) {
            errorOut = "pose warp refused: non-finite input";
            return false;
        }
        for (const WarpFootTarget& f : input.feet) {
            if (f.footBone < 0 ||
                f.footBone >= static_cast<int>(skeleton.bones.size())) {
                out.translations.clear();
                out.rotations.clear();
                out.scales.clear();
                errorOut = "pose warp refused: foot bone out of range";
                return false;
            }
            if (!std::isfinite(f.targetWorld.x) ||
                !std::isfinite(f.targetWorld.y) ||
                !std::isfinite(f.targetWorld.z)) {
                out.translations.clear();
                out.rotations.clear();
                out.scales.clear();
                errorOut = "pose warp refused: non-finite foot target";
                return false;
            }
        }

        out = pose;

        // World transforms of the ORIGINAL pose (for the foot pins) and of
        // the warped root (parents above the root are unaffected).
        const std::size_t n = skeleton.bones.size();
        std::vector<glm::mat4> originalWorld(n, glm::mat4(1.0f));
        for (std::size_t i = 0; i < n; ++i) {
            const glm::mat4 local =
                glm::translate(glm::mat4(1.0f), pose.translations[i]) *
                glm::mat4_cast(pose.rotations[i]) *
                glm::scale(glm::mat4(1.0f), pose.scales[i]);
            const int p = skeleton.bones[i].parent;
            originalWorld[i] =
                p >= 0 ? originalWorld[static_cast<std::size_t>(p)] * local
                       : local;
        }

        // Root snap: position (x/z from the body, y from the surface),
        // heading, and optional speed-based forward lean. The lean rotates
        // the root about the local X axis (forward = +Z after yaw): a point
        // ahead of the body dips toward the ground, i.e. a forward lean.
        {
            const float leanAngle = spec.leanFactor * input.speed;
            const glm::quat yaw = glm::angleAxis(input.bodyYaw, glm::vec3(0, 1, 0));
            const glm::quat lean = glm::angleAxis(leanAngle, glm::vec3(1, 0, 0));
            const glm::quat targetRot = yaw * lean;
            out.rotations[0] = glm::slerp(pose.rotations[0], targetRot,
                                          spec.rootWeight);
            out.translations[0].x = glm::mix(pose.translations[0].x,
                                             input.bodyPosition.x,
                                             spec.rootWeight);
            out.translations[0].z = glm::mix(pose.translations[0].z,
                                             input.bodyPosition.z,
                                             spec.rootWeight);
            out.translations[0].y = glm::mix(pose.translations[0].y,
                                             input.surfaceHeight,
                                             spec.rootWeight);
        }

        // Warped world transforms of the root (parents of the root are
        // unchanged; only the root itself moved).
        std::vector<glm::mat4> warpedWorld(n, glm::mat4(1.0f));
        for (std::size_t i = 0; i < n; ++i) {
            const glm::mat4 local =
                glm::translate(glm::mat4(1.0f), out.translations[i]) *
                glm::mat4_cast(out.rotations[i]) *
                glm::scale(glm::mat4(1.0f), out.scales[i]);
            const int p = skeleton.bones[i].parent;
            warpedWorld[i] =
                p >= 0 ? warpedWorld[static_cast<std::size_t>(p)] * local
                       : local;
        }

        // Foot pins: translate the foot bone in its PARENT frame so the foot
        // approaches the placed target (clamped per warp). The parent's
        // world rotation maps the world delta into the parent's local frame.
        for (const WarpFootTarget& f : input.feet) {
            if (!f.stance) continue;  // swinging feet keep their arc
            const std::size_t bi = static_cast<std::size_t>(f.footBone);
            const glm::vec3 current =
                glm::vec3(warpedWorld[bi][3]);
            glm::vec3 delta = f.targetWorld - current;
            const float dist = glm::length(delta);
            if (dist > spec.maxFootMove) {
                delta *= spec.maxFootMove / dist;
            }
            const int parent = skeleton.bones[bi].parent;
            glm::vec3 localDelta = delta;
            if (parent >= 0) {
                // Rotate the delta into the parent's local frame (ignore
                // scale — the parent transform may carry one).
                const glm::mat4& pw = warpedWorld[static_cast<std::size_t>(parent)];
                const glm::mat3 rot = glm::mat3(pw);
                localDelta = glm::inverse(rot) * delta;
            }
            out.translations[bi] += localDelta * spec.footWeight;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<IPoseWarper> create_pose_warper() {
    return std::make_unique<PoseWarperImpl>();
}

}  // namespace animation
}  // namespace engine

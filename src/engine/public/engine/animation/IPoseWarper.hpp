#pragma once

// Public pose-warping contract (FALTANTES §18 item 6): adapts a sampled
// animation pose to the creature's ACTUAL locomotion state. The foot
// placement contract (IFootPlacement) re-anchors the gait plan to the live
// terrain; this contract warps a pose (e.g. a clip sampled from the motion
// database) to match the body's real position/heading/surface: the root is
// snapped to the body state (position + yaw + terrain surface height), the
// planted feet are pinned toward the placed targets (a pure local-space
// translation in the parent frame, clamped per warp so the foot never
// teleports), and an optional speed-based forward lean is applied. The IK
// (IMotionDatabase::ik_two_bone) remains the exact solver for a chain; this
// contract is the cheap frame-level adaptation between sampling and solving.
//
// Pure and DETERMINISTIC: identical (skeleton, pose, spec, input) produce
// identical warped poses — no hidden state. Self-contained (glm + the motion
// database header only). The ONLY TU implementing the contract is the SDK
// adapter (src/engine/sdk/PoseWarp.cpp) — the adapter rule.

#include "engine/animation/IMotionDatabase.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// Warping policy. Validated all-or-nothing (refuse, never clamp).
struct PoseWarpSpec {
    // Max distance a planted foot may move per warp (m). A placed target
    // farther than this is approached up to the limit — never teleported.
    // > 0.
    float maxFootMove{ 0.25f };
    // Blends the root snap: 1 = root fully at (body, yaw, surface), 0 = pose
    // root untouched. [0, 1].
    float rootWeight{ 1.0f };
    // Blends each foot pin toward its placed target. [0, 1].
    float footWeight{ 1.0f };
    // Forward lean per unit of horizontal speed (radians per m/s); 0 = no
    // lean. >= 0.
    float leanFactor{ 0.0f };

    bool validate(std::string& errorOut) const;
};

// One foot to pin: the canonical foot bone and the placed target (world
// space, from IFootPlacement). Stance feet are pinned fully; swinging feet
// keep their arc (their targets already arc above the surface — the pin is a
// no-op within tolerance).
struct WarpFootTarget {
    int footBone{ -1 };
    glm::vec3 targetWorld{ 0.0f };
    bool stance{ false };
};

// The body state the pose is warped to (world space, Y-up).
struct WarpInput {
    // Body origin position (world). The root's x/z snap to this; the root's
    // y snaps to `surfaceHeight` (the body is carried by the terrain).
    glm::vec3 bodyPosition{ 0.0f };
    // Body heading (radians about +Y).
    float bodyYaw{ 0.0f };
    // Terrain surface height under the body (world Y) — the root rests on it.
    float surfaceHeight{ 0.0f };
    // Horizontal body speed (m/s) — drives the lean.
    float speed{ 0.0f };
    // Feet to pin toward their placed targets.
    std::vector<WarpFootTarget> feet;
};

// The warper: adapts a pose to live locomotion state. Pure + deterministic.
class IPoseWarper {
public:
    virtual ~IPoseWarper() = default;

    // Warps `pose` (local-space, sized to `skeleton`) into `out`. All-or-
    // nothing: an invalid spec, an empty/mismatched skeleton/pose, an
    // out-of-range foot bone or non-finite input is refused with a
    // diagnostic and `out` is cleared.
    virtual bool warp(const MotionSkeleton& skeleton, const MotionPose& pose,
                      const PoseWarpSpec& spec, const WarpInput& input,
                      MotionPose& out, std::string& errorOut) = 0;
};

// The factory: builds the pure warper (the ONLY adapter TU).
std::unique_ptr<IPoseWarper> create_pose_warper();

}  // namespace animation
}  // namespace engine

#pragma once

// Public procedural-locomotion contract (FALTANTES §18 item 7): spider-style
// leg locomotion INSPIRED by the `minecraft-spider` plugin's CONCEPTS, with
// no dependency on the plugin (it is a reference only, like shape-ml — never
// compiled/included). The distinguishing idea vs the phase-locked gait
// planner (IGaitPlanner): legs are driven by EMERGENT timing — each leg
// plants a foot, and independently re-targets (swings to a new landing) when
// the body carries the planted foot beyond the leg's reach. There is NO gait
// cycle clock or per-leg phase offset: the gait emerges from the geometry
// (hip travel vs reach), exactly the spider feel of legs stepping when the
// body moves them out of range.
//
// The creature is an arbitrary set of leg chains (the SAME LegChainAsset
// shape the gait planner consumes), plus a body height. The driver samples
// the terrain through the IFootTerrainSampler seam (item 5) and produces
// per-leg targets/landings that the caller feeds to the item-6 pose warper
// and item-3 IK. Pure and DETERMINISTIC: the per-leg state is carried in the
// caller-owned `ProceduralLegState` list (never hidden in the adapter).
//
// Self-contained (glm + the public animation headers). The ONLY TU that
// implements the contract is the SDK adapter (src/engine/sdk/ProceduralLegs.cpp)
// — the adapter rule.

#include "engine/animation/IFootPlacement.hpp"
#include "engine/animation/IGaitPlanner.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// One leg's runtime state (caller-owned; the driver reads and updates it).
struct ProceduralLegState {
    // Current planted/swing target in world space. While swinging, this
    // interpolates planted -> landing; while planted it stays fixed.
    glm::vec3 targetWorld{ 0.0f };
    // True while the leg is swinging (moving to a new landing).
    bool swinging{ false };
    // Swing progress in [0, 1) while swinging — derived deterministically
    // from (time - swingStartTime) / swingDuration, never a per-call delta.
    float swingProgress{ 0.0f };
    // The locomotion clock time the swing started (deterministic progress).
    float swingStartTime{ 0.0f };
    // The landing this swing aims for (world space).
    glm::vec3 landing{ 0.0f };
    // The surface height under the CURRENT target column (known or not).
    float surfaceHeight{ 0.0f };
    bool surfaceKnown{ false };
};

// Locomotion policy. Validated all-or-nothing (refuse, never clamp).
struct ProceduralLocomotionSpec {
    // The creature's legs (the same chain shape the gait planner uses).
    // Non-empty.
    std::vector<LegChainAsset> legs;
    // Body height above the terrain surface (m) — the body rides the ground.
    // > 0.
    float bodyHeight{ 0.8f };
    // A leg re-targets when the hip-to-planted horizontal distance exceeds
    // reach * retargetFactor. reach = leg.maxReach or upper+lower when 0.
    // > 0.
    float retargetFactor{ 0.75f };
    // Swing duration (seconds); > 0.
    float swingDuration{ 0.35f };
    // Vertical lift during swing (m); >= 0.
    float stepHeight{ 0.2f };
    // How far ahead of the current planted foot the new landing is placed,
    // along the body's forward (+Z local) direction, scaled by the current
    // speed (m of landing distance per m/s of body speed). >= 0.
    float landingDistancePerSpeed{ 0.15f };
    // Minimum landing distance regardless of speed (m). >= 0.
    float minLandingDistance{ 0.0f };

    bool validate(std::string& errorOut) const;
};

// The driver result: the per-leg states plus the body's adapted state.
struct ProceduralLocomotionResult {
    std::vector<ProceduralLegState> legs;
    // The body rides the terrain: y = surface under the body + bodyHeight.
    glm::vec3 bodyPosition{ 0.0f };
    float surfaceKnown{ false };
};

// The spider-style leg driver. Pure + deterministic: identical
// (spec, terrain, body, prevLegs) produce identical outputs.
class IProceduralLocomotion {
public:
    virtual ~IProceduralLocomotion() = default;

    // Advances the leg states by one step. `bodyPosition`/`bodyYaw` are the
    // CURRENT body state (world, Y-up), `velocity` the horizontal body
    // velocity (m/s, drives the landing distance), `time` the locomotion
    // clock (>= 0, drives swing progress). `prevLegs` must be empty (first
    // step — all legs plant at their rest positions on the terrain) or sized
    // to match the spec's legs. All-or-nothing: an invalid spec, a size
    // mismatch or non-finite input is refused with a diagnostic.
    virtual bool step(const ProceduralLocomotionSpec& spec,
                      const IFootTerrainSampler& terrain,
                      float time, const glm::vec3& bodyPosition,
                      float bodyYaw, const glm::vec2& velocity,
                      const std::vector<ProceduralLegState>& prevLegs,
                      ProceduralLocomotionResult& out,
                      std::string& errorOut) = 0;
};

// The factory: builds the procedural driver (the ONLY adapter TU).
std::unique_ptr<IProceduralLocomotion> create_procedural_locomotion();

}  // namespace animation
}  // namespace engine

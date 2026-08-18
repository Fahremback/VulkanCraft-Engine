#pragma once

// Public foot-placement contract (FALTANTES §18 item 5): robust placement of
// a creature's feet on DYNAMIC voxel terrain and MOVING surfaces. The gait
// planner (IGaitPlanner) produces foot targets assuming flat ground; this
// contract re-anchors them to the live terrain surface (height at the foot's
// column), clamps vertical steps (feet never teleport through walls/slabs),
// and reports the placement facts (surface height, known/unknown terrain,
// step-limited, within reach) so the caller can drive the animation IK.
//
// The terrain is a SEAM: the placer takes an `IFootTerrainSampler` and never
// touches a voxel world itself. The SDK ships a voxel-backed sampler
// (create_voxel_foot_terrain_sampler over the public IVoxelWorld) and the
// gate proves moving surfaces by feeding a platform that moves between
// placements. The placer is PURE and DETERMINISTIC: the only variability is
// the (spec, sampler, plan, prev) inputs — the caller owns any frame-to-frame
// state (passing the previous result enables the step clamp).
//
// Self-contained (glm + the public voxel header only). The ONLY TU that
// implements the contract is the SDK adapter (src/engine/sdk/FootPlacement.cpp)
// — the adapter rule.

#include "engine/animation/IGaitPlanner.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// The terrain surface at one world column. `known=false` means the terrain
// is not available right now (e.g. the chunk is not loaded) — the placer
// keeps the plan's own y instead of inventing a height.
struct SurfaceSample {
    bool known{ false };
    // World Y of the terrain surface (the top face a foot rests on).
    float height{ 0.0f };
};

// The terrain seam: queries the surface height at a world column (X, Z).
// Implementations: the SDK's voxel sampler (IVoxelWorld top-down scan) and
// any caller-provided sampler (moving platforms, heightmaps, physics proxies).
class IFootTerrainSampler {
public:
    virtual ~IFootTerrainSampler() = default;
    virtual SurfaceSample sample(float worldX, float worldZ) const = 0;
};

// Placement policy. Validated all-or-nothing (refuse, never clamp).
struct FootPlacementSpec {
    // Max vertical distance a foot may MOVE between consecutive placements
    // (m). A surface that jumps more than this is reported as `stepLimited`
    // instead of teleporting the foot. > 0.
    float maxStepHeight{ 0.5f };
    // Vertical rest offset above the surface for a PLANTED foot (the foot's
    // contact height minus the surface top face). >= 0.
    float footRestHeight{ 0.0f };

    bool validate(std::string& errorOut) const;
};

// One placed foot: the gait plan's target re-anchored to the terrain.
struct PlacedFoot {
    int legIndex{ 0 };
    // The surface-adjusted target (world space). For a planted foot this is
    // the surface height + rest offset at the plan's (x, z); for a swinging
    // foot the plan's target keeps its lift arc (it already arcs above the
    // ground) — the placer does NOT pull the swing target down to the surface
    // (the foot must clear obstacles).
    glm::vec3 targetWorld{ 0.0f };
    // The predicted next-stance landing point, re-anchored to the surface at
    // the plan's landing column (so the next stance starts on the terrain).
    glm::vec3 landing{ 0.0f };
    // The terrain surface height sampled at the plan's column.
    float surfaceHeight{ 0.0f };
    // True when the terrain sample was available. When false, the placer kept
    // the plan's own y (the caller decides the fallback, e.g. last known).
    bool surfaceKnown{ false };
    // True when the surface moved more than maxStepHeight since the previous
    // placement AND the foot was clamped to the step window. The caller may
    // then e.g. shorten the stride or flag the animation.
    bool stepLimited{ false };
    // True when the foot is planted (stance) per the gait plan.
    bool stance{ false };
};

struct FootPlacementResult {
    std::vector<PlacedFoot> feet;
};

// The placer: re-anchors a gait plan to live terrain. Pure and deterministic
// for the same (spec, sampler, plan, prev) inputs.
class IFootPlacer {
public:
    virtual ~IFootPlacer() = default;

    // Re-anchors every foot of `plan` to the terrain sampled through
    // `terrain`. All-or-nothing: an invalid spec or an empty/mismatched plan
    // is refused with a diagnostic and `out` is cleared. Planted feet are
    // placed ON the surface (clamped to the step window vs the previous
    // result, reported as `stepLimited` when clamped); swinging feet keep
    // their arc. `prev` is the previous placement result (may be empty — the
    // first placement is never step-limited).
    virtual bool place(const FootPlacementSpec& spec,
                       const IFootTerrainSampler& terrain,
                       const GaitPlan& plan, const FootPlacementResult& prev,
                       FootPlacementResult& out, std::string& errorOut) = 0;
};

// The factory: builds the pure placer (the ONLY adapter TU).
std::unique_ptr<IFootPlacer> create_foot_placer();

// Voxel-backed terrain sampler: top-down scan of the public IVoxelWorld at
// each column. `scanTop` is the world Y where the scan starts (>= the highest
// possible surface; the surface below `scanBottom` is treated as unknown).
// Returns `known=false` when the chunk holding the column is not loaded or no
// block is found in the scan window — never a guessed height. Thread-safety:
// reads only, safe for concurrent calls into a streamed world.
std::unique_ptr<IFootTerrainSampler> create_voxel_foot_terrain_sampler(
    const class engine::voxel::IVoxelWorld& world, float scanTop,
    float scanBottom);

}  // namespace animation
}  // namespace engine

// FootPlacement (FALTANTES §18 item 5): the ONLY TU implementing the public
// IFootPlacer contract. Re-anchors a gait plan (IGaitPlanner) to the live
// terrain surface sampled through an IFootTerrainSampler seam: planted feet
// rest ON the surface, the next-stance landing is re-anchored too, vertical
// steps are clamped vs the previous placement (never teleport), and every
// placement reports the surface facts. Pure + deterministic; the voxel
// sampler ships here too (top-down scan of the public IVoxelWorld — the only
// voxel coupling in this TU).
#include "engine/animation/IFootPlacement.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace engine {
namespace animation {

bool FootPlacementSpec::validate(std::string& errorOut) const {
    if (!std::isfinite(maxStepHeight) || maxStepHeight <= 0.0f) {
        errorOut = "foot placement refused: maxStepHeight must be > 0";
        return false;
    }
    if (!std::isfinite(footRestHeight) || footRestHeight < 0.0f) {
        errorOut = "foot placement refused: footRestHeight must be >= 0";
        return false;
    }
    return true;
}

namespace {

class FootPlacerImpl final : public IFootPlacer {
public:
    bool place(const FootPlacementSpec& spec, const IFootTerrainSampler& terrain,
               const GaitPlan& plan, const FootPlacementResult& prev,
               FootPlacementResult& out, std::string& errorOut) override {
        out.feet.clear();
        std::string err;
        if (!spec.validate(err)) {
            errorOut = err;
            return false;
        }
        if (plan.feet.empty()) {
            errorOut = "foot placement refused: empty gait plan";
            return false;
        }
        if (!prev.feet.empty() && prev.feet.size() != plan.feet.size()) {
            errorOut = "foot placement refused: previous result size "
                       "mismatch";
            return false;
        }
        out.feet.resize(plan.feet.size());
        for (std::size_t i = 0; i < plan.feet.size(); ++i) {
            const FootPlan& f = plan.feet[i];
            if (f.legIndex != static_cast<int>(i)) {
                out.feet.clear();
                errorOut = "foot placement refused: plan feet not in leg "
                           "index order";
                return false;
            }
            const SurfaceSample surface =
                terrain.sample(f.targetWorld.x, f.targetWorld.z);
            PlacedFoot& placed = out.feet[i];
            placed.legIndex = f.legIndex;
            placed.stance = f.stance;
            placed.surfaceHeight = surface.known ? surface.height : 0.0f;
            placed.surfaceKnown = surface.known;

            // The y the foot had in the previous placement (for the step
            // clamp) — only for the same stance phase of the same leg.
            float prevY = f.targetWorld.y;
            bool havePrev = false;
            if (i < prev.feet.size() && prev.feet[i].legIndex == f.legIndex &&
                prev.feet[i].stance == f.stance) {
                prevY = prev.feet[i].targetWorld.y;
                havePrev = true;
            }

            // Planted feet rest ON the surface. Swinging feet re-base their
            // arc to the surface: the plan's swing target arcs above the
            // FLAT baseline (the plan assumes flat ground at the body's
            // height), so the lift offset above that baseline is preserved
            // and added to the real surface. The baseline is the plan's own
            // stance-foot y (constant across stance feet: bodyY + rest). If
            // no stance foot exists or the surface is unknown, the plan y is
            // kept untouched. The next-stance landing is re-anchored on both
            // paths (the plan's landing column).
            float flatBaseline = f.targetWorld.y;
            bool haveBaseline = false;
            for (const FootPlan& other : plan.feet) {
                if (other.stance) {
                    flatBaseline = other.targetWorld.y;
                    haveBaseline = true;
                    break;
                }
            }
            if (f.stance && surface.known) {
                placed.targetWorld = f.targetWorld;
                placed.targetWorld.y = surface.height + spec.footRestHeight;
                if (havePrev &&
                    std::fabs(placed.targetWorld.y - prevY) >
                        spec.maxStepHeight) {
                    placed.targetWorld.y =
                        prevY + (placed.targetWorld.y > prevY
                                     ? spec.maxStepHeight
                                     : -spec.maxStepHeight);
                    placed.stepLimited = true;
                }
            } else if (f.stance) {
                placed.targetWorld = f.targetWorld;  // unknown terrain
            } else if (surface.known && haveBaseline) {
                // Swing arc re-based to the surface: lift above the flat
                // baseline, applied to the real surface.
                const float lift = f.targetWorld.y - flatBaseline;
                placed.targetWorld = f.targetWorld;
                placed.targetWorld.y =
                    surface.height + spec.footRestHeight + lift;
            } else {
                placed.targetWorld = f.targetWorld;  // unknown or no baseline
            }

            // The landing column's surface, when known: the next stance
            // starts ON the terrain. The step clamp references the PREVIOUS
            // landing y (the last predicted contact), never the current
            // target (a swinging foot's mid-air target is not a contact
            // anchor — clamping against it would false-flag every swing).
            placed.landing = f.landing;
            const SurfaceSample landingSurface =
                terrain.sample(f.landing.x, f.landing.z);
            if (landingSurface.known) {
                placed.landing.y =
                    landingSurface.height + spec.footRestHeight;
                float prevLandingY = placed.landing.y;
                bool havePrevLanding = false;
                if (i < prev.feet.size() &&
                    prev.feet[i].legIndex == f.legIndex) {
                    prevLandingY = prev.feet[i].landing.y;
                    havePrevLanding = true;
                }
                if (havePrevLanding &&
                    std::fabs(placed.landing.y - prevLandingY) >
                        spec.maxStepHeight) {
                    placed.landing.y =
                        prevLandingY +
                        (placed.landing.y > prevLandingY
                             ? spec.maxStepHeight
                             : -spec.maxStepHeight);
                    placed.stepLimited = true;
                }
            }
        }
        return true;
    }
};

// Voxel terrain sampler: top-down scan of the world column. The chunk is
// known only when loaded — an unloaded chunk never invents a height.
class VoxelFootTerrainSampler final : public IFootTerrainSampler {
public:
    VoxelFootTerrainSampler(const engine::voxel::IVoxelWorld& world,
                            float scanTop, float scanBottom)
        : world_(world), scanTop_(scanTop), scanBottom_(scanBottom) {}

    SurfaceSample sample(float worldX, float worldZ) const override {
        SurfaceSample result;
        const int x = static_cast<int>(std::floor(worldX));
        const int z = static_cast<int>(std::floor(worldZ));
        const int chunkX = x >> 4;
        const int chunkZ = z >> 4;
        if (!world_.is_chunk_loaded(chunkX, chunkZ)) {
            return result;  // known=false
        }
        const int top = static_cast<int>(std::ceil(scanTop_));
        const int bottom = static_cast<int>(std::floor(scanBottom_));
        for (int y = top; y >= bottom; --y) {
            const uint32_t block = world_.get_block(x, y, z);
            if (block != 0) {  // air = 0
                result.known = true;
                // The surface top face is one unit above the highest solid
                // block in the column.
                result.height = static_cast<float>(y + 1);
                return result;
            }
        }
        return result;  // no block in the scan window: unknown
    }

private:
    const engine::voxel::IVoxelWorld& world_;
    float scanTop_;
    float scanBottom_;
};

}  // namespace

std::unique_ptr<IFootPlacer> create_foot_placer() {
    return std::make_unique<FootPlacerImpl>();
}

std::unique_ptr<IFootTerrainSampler> create_voxel_foot_terrain_sampler(
    const engine::voxel::IVoxelWorld& world, float scanTop, float scanBottom) {
    return std::make_unique<VoxelFootTerrainSampler>(world, scanTop, scanBottom);
}

}  // namespace animation
}  // namespace engine

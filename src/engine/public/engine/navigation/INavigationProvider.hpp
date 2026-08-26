#pragma once

// Public navigation provider (SDK, META section 16 / FALTANTES item 12). The
// default implementation is backed by Recast + Detour (the navigation
// authority, DEPENDENCY_POLICY); this header is self-contained and never leaks
// external types. A navmesh is baked from voxel columns (walkable surface) for
// a configured bounds, queried for paths and walkability, and carries a
// revision that bumps on every rebuild so consumers can invalidate cached
// paths after terrain edits.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace navigation {

// Agent + baking configuration (Recast parameters, world units).
struct NavmeshConfig {
    float boundsMinX{ -64.0f };
    float boundsMinZ{ -64.0f };
    float boundsMaxX{ 64.0f };
    float boundsMaxZ{ 64.0f };
    float boundsMinY{ -8.0f };
    float boundsMaxY{ 200.0f };
    float cellSize{ 0.3f };          // voxel footprint
    float cellHeight{ 0.2f };        // voxel height
    float agentRadius{ 0.4f };       // erosion radius
    float agentHeight{ 1.8f };       // clearance
    float agentMaxClimb{ 1.0f };     // max step up (stairs/edges)
    float agentMaxSlope{ 45.0f };    // degrees; steeper surfaces are unwalkable
    float maxEdgeLen{ 12.0f };
    float maxSimplificationError{ 1.3f };
    int minRegionArea{ 8 };          // regions smaller than this are discarded
    int mergeRegionArea{ 20 };
    float detailSampleDist{ 6.0f };
    float detailSampleMaxError{ 0.4f };   // < 1 block so single-block steps survive
    int maxPolys{ 2048 };            // path search budget
    // Tiled navmesh mode (FALTANTES item 12 — local updates). 0 = legacy
    // single navmesh baked from the whole bounds (every build() is a full
    // rebake). > 0 = the bounds are split into square tiles of this size in
    // world units; build() bakes every tile once and update() re-bakes ONLY
    // the tiles overlapping a block change, so a block transaction does not
    // require a full rebake. Tiles are linked by Detour at their shared
    // borders, so paths cross tiles transparently.
    float tileSize{ 0.0f };
};

// One solid voxel column fed to the baker. The walkable surface is the TOP of
// the span (solidMaxY); the baker erodes by agentRadius and filters by slope
// and clearance. Columns whose footprint falls outside the config bounds are
// ignored by the baker.
struct VoxelColumn {
    float x{ 0.0f };         // column center (world)
    float z{ 0.0f };
    float solidMinY{ 0.0f };  // bottom of the solid span
    float solidMaxY{ 0.0f };  // top of the solid span (>= solidMinY)
    bool solid{ false };
};

struct PathResult {
    bool found{ false };
    std::vector<float> waypoints;  // flat x,y,z triplets, world space
    float totalLength{ 0.0f };
    uint64_t revision{ 0 };        // navmesh revision that produced the path
};

class INavigationProvider {
public:
    virtual ~INavigationProvider() = default;

    // Bakes/rebakes the navmesh for config.bounds from the given voxel
    // columns. Returns false with a diagnostic when baking fails (e.g. no
    // walkable surface). Bumps revision() on success. In tiled mode
    // (config.tileSize > 0) this bakes every tile and is the reference
    // result that update() must match locally.
    virtual bool build(const NavmeshConfig& config,
                       const std::vector<VoxelColumn>& columns,
                       std::string& errorOut) = 0;

    // Incremental local update after a block transaction (FALTANTES item 12):
    // replaces the given columns in the provider's column set and re-bakes
    // ONLY the navmesh tiles overlapping the changed columns (tiled mode,
    // NavmeshConfig::tileSize > 0). The result is equivalent to build() over
    // the full updated column set, but tiles outside the change are never
    // re-baked. Bumps revision() and the re-baked tiles' per-tile revisions
    // (see tile_revision). All-or-nothing: if any affected tile fails to
    // bake, no tile is swapped and a diagnostic is returned. Refused with a
    // diagnostic in single-navmesh mode, for an empty change set or before
    // build().
    virtual bool update(const std::vector<VoxelColumn>& changedColumns,
                        std::string& errorOut) = 0;

    // Path search between two world points (projected onto the navmesh).
    virtual bool find_path(float startX, float startY, float startZ,
                           float goalX, float goalY, float goalZ,
                           PathResult& out) const = 0;

    // True when the point is on/near a walkable surface of the navmesh.
    virtual bool is_walkable(float x, float y, float z) const = 0;

    // Bumps on every successful build/update; consumers use it to invalidate
    // cached paths after terrain edits.
    virtual uint64_t revision() const = 0;

    // Revision of the navmesh tile containing the world point (0 when in
    // single-navmesh mode, before build(), or outside the tiled navmesh).
    // Consumers cache paths per tile and invalidate only those crossing a
    // tile whose revision changed after update() — the locality signal for
    // cached path invalidation.
    virtual uint64_t tile_revision(float x, float z) const = 0;
    virtual bool valid() const = 0;
};

// Recast + Detour-backed implementation (the only TU with Recast headers).
std::unique_ptr<INavigationProvider> create_recast_navigation_provider();

}  // namespace navigation
}  // namespace engine

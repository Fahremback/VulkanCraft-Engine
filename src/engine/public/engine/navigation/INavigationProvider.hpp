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
    // Slope cost (FALTANTES item 12 — "inclinação"): when slopeCostArea != 0,
    // walkable spans whose local surface slope is in
    // [slopeCostStartDegrees, agentMaxSlope) are tagged with slopeCostArea at
    // bake time (heightfield level — the per-cell slope is exact, and
    // rcBuildRegions keeps different areas separate), so the caller can cost
    // steep terrain via set_area_cost (a ramp is traversable but expensive).
    // Slopes >= agentMaxSlope remain rejected (RC_NULL_AREA). 0 = disabled
    // (every walkable poly is the default walkable area — current behavior).
    uint8_t slopeCostArea{ 0 };
    float slopeCostStartDegrees{ 20.0f };
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
    // Surface cost area (FALTANTES item 12 — costs by material/danger): 0 =
    // the default walkable area; 1..62 = a custom cost area whose traversal
    // multiplier is set with set_area_cost. Columns of expensive/dangerous
    // material (water, lava, gravel, etc.) are tagged by the caller and the
    // baker preserves the area onto the produced polygons, so find_path
    // prefers cheaper terrain.
    uint8_t area{ 0 };
};

struct PathResult {
    bool found{ false };
    std::vector<float> waypoints;  // flat x,y,z triplets, world space
    float totalLength{ 0.0f };
    uint64_t revision{ 0 };        // navmesh revision that produced the path
};

// Lifecycle of an asynchronous path request (FALTANTES item 12 — async and
// cancellable queries). Queued -> Running -> Succeeded | Failed | Cancelled.
// A request cancelled via cancel_async_path is guaranteed to reach Cancelled
// before cancel_async_path returns (join semantics — see the provider).
// Invalid = the request id is unknown to the provider.
enum class PathRequestStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Invalid,
};

// A dynamic obstacle (FALTANTES item 12 — doors, platforms, moving
// obstacles): a named set of solid voxel columns whose footprint blocks the
// navmesh while ACTIVE and is passable while INACTIVE. Toggling an obstacle
// re-bakes ONLY the navmesh tiles overlapping its columns (tiled mode), so a
// door opening/closing or a platform appearing/disappearing never triggers a
// full rebake; the resulting navmesh is equivalent to build() over the full
// column set with the obstacle's columns applied (active) or not (inactive).
// The id is caller-chosen and must be unique per provider.
struct DynamicObstacle {
    // Solid footprint columns. While ACTIVE these columns override the
    // terrain columns at the same grid positions (blocking); while INACTIVE
    // the terrain columns are restored (passable).
    std::vector<VoxelColumn> columns;
};

// An off-mesh link (FALTANTES item 12 — jump/climb): a point-to-point edge
// in the navigation graph that is NOT part of the walkable surface — a jump
// across a gap, a climb up a ledge, a ladder. Traversing a link teleports the
// agent from start to end in one step (both endpoints must be near a walkable
// poly within `radius` horizontally and agentMaxClimb vertically). Links are
// baked into the tile that contains their midpoint (tiled mode), so setting
// links re-bakes only the affected tiles; the result is equivalent to baking
// the links with build() from the start.
struct OffMeshLink {
    float startX{ 0.0f };
    float startY{ 0.0f };
    float startZ{ 0.0f };
    float endX{ 0.0f };
    float endY{ 0.0f };
    float endZ{ 0.0f };
    float radius{ 0.5f };  // horizontal snap radius around each endpoint
    bool bidirectional{ true };  // false = only start -> end
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

    // Registers (or replaces) a dynamic obstacle and activates it: the
    // obstacle's columns override the terrain in their footprint and ONLY
    // the overlapping tiles are re-baked (tiled mode). Refused with a
    // diagnostic in single-navmesh mode, before build(), for an empty
    // footprint, or when a tile bake fails (all-or-nothing: the previous
    // obstacle state is preserved). A registered id can be toggled with
    // set_obstacle_active.
    virtual bool set_dynamic_obstacle(uint64_t id,
                                      const DynamicObstacle& obstacle,
                                      std::string& errorOut) = 0;

    // Toggles a registered obstacle: active re-applies its columns over the
    // terrain (re-baking only the overlapping tiles), inactive restores the
    // terrain columns underneath. Refused with a diagnostic for an unknown
    // id or in single-navmesh mode.
    virtual bool set_obstacle_active(uint64_t id, bool active,
                                     std::string& errorOut) = 0;

    // Sets the traversal cost multiplier of a surface cost area (FALTANTES
    // item 12 — costs by material/inclination/danger): find_path minimizes
    // (path length * area cost), so an area with cost 3.0 is three times as
    // expensive to cross and the query routes around it when a cheaper route
    // exists. area 0 = the default walkable area; 1..62 = custom areas tagged
    // on VoxelColumn::area / NavmeshConfig::slopeCostArea. The default cost
    // of every area is 1.0. Refused for an out-of-range area or a
    // negative/non-finite cost. Applies to subsequent find_path/is_walkable
    // queries.
    virtual bool set_area_cost(int area, float cost,
                               std::string& errorOut) = 0;

    // Current cost multiplier of a surface cost area (1.0 when never set).
    virtual float area_cost(int area) const = 0;

    // Replaces the off-mesh links of the navmesh (FALTANTES item 12 — jump/
    // climb): each link is baked into the tile containing its midpoint and
    // ONLY the overlapping tiles are re-baked (tiled mode). A path that
    // cannot cross a gap/wall without a link becomes routable once the link
    // exists. Refused with a diagnostic in single-navmesh mode, before
    // build(), for a non-finite endpoint, a non-positive/non-finite radius,
    // or when a tile bake fails (all-or-nothing: the previous link set is
    // preserved). Links are independent of the column set — obstacles and
    // terrain updates do not clear them.
    virtual bool set_off_mesh_links(const std::vector<OffMeshLink>& links,
                                    std::string& errorOut) = 0;

    // Asynchronous and cancellable path queries (FALTANTES item 12): the
    // query runs on the provider's background worker, so the caller never
    // blocks. The result of a Succeeded request is bit-identical to
    // find_path for the same points at the same navmesh revision.
    //
    // begin_async_path enqueues the request and returns its id (0 with a
    // diagnostic when refused: before build(), non-finite points, or no
    // worker available). poll_async_path returns the current status and
    // fills `out` only on Succeeded (and `errorOut` on Failed); it never
    // blocks. cancel_async_path marks the request cancelled and BLOCKS
    // until it reaches a terminal state — after it returns, poll reports
    // Cancelled (join semantics, no race); returns false for an unknown id.
    // Requests are processed in FIFO order; a cancelled request that is
    // still queued is dropped without running. Terrain edits, obstacle
    // toggles and link changes serialize with in-flight queries, so a query
    // never observes a half-swapped tile. The provider joins its worker in
    // the destructor.
    virtual uint64_t begin_async_path(float startX, float startY,
                                      float startZ, float goalX, float goalY,
                                      float goalZ, std::string& errorOut) = 0;

    // Non-blocking status check; fills `out` only on Succeeded (and
    // `errorOut` on Failed). Invalid for an unknown request id.
    virtual PathRequestStatus poll_async_path(uint64_t requestId,
                                              PathResult& out,
                                              std::string& errorOut) = 0;

    // Cancels a request with join semantics (see begin_async_path); false
    // for an unknown request id.
    virtual bool cancel_async_path(uint64_t requestId) = 0;
};

// Recast + Detour-backed implementation (the only TU with Recast headers).
std::unique_ptr<INavigationProvider> create_recast_navigation_provider();

}  // namespace navigation
}  // namespace engine

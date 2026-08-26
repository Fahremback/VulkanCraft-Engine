// RecastNavigation.cpp — Recast/Detour-backed navigation provider (META
// section 16 / FALTANTES item 12). This is the ONLY translation unit that
// includes the Recast and Detour headers; the public contract lives in
// engine/navigation/* and never leaks them.
//
// Two modes (NavmeshConfig::tileSize):
//   0    — legacy single navmesh baked from the whole bounds (every build()
//          is a full rebake).
//   > 0  — tiled navmesh: the bounds are split into square tiles; each tile
//          is baked with the SoloMesh pipeline over a border-expanded
//          heightfield (rcBuildRegions clips the border ring away, so the
//          produced polys cover exactly the nominal tile) and added to a
//          multi-tile dtNavMesh, which links adjacent tiles at their shared
//          edges. update() re-bakes ONLY the tiles overlapping a block
//          change, so a block transaction does not require a full rebake.
//
// The per-tile bake follows the canonical TileMesh sample (RecastDemo
// Sample_TileMesh.cpp): border = walkableRadius + 3 cells, regions built with
// the border so no polys form on it, dtCreateNavMeshData with the tile's own
// bounds and tile indices, addTile/removeTile for swapping.

#include "engine/navigation/INavigationProvider.hpp"
#include "engine/navigation/VoxelNavigation.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include "Recast.h"
#include "RecastAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {
namespace navigation {

namespace {

constexpr unsigned char kWalkableArea = RC_WALKABLE_AREA;
constexpr unsigned short kWalkFlag = 0x0001;  // our walk flag (user-defined)

int world_to_cell(float world, float origin, float cell) {
    return static_cast<int>(std::floor((world - origin) / cell));
}

// Quantized grid key of a column (the sampler emits columns on the
// config.cellSize grid; exact float equality is unsafe across rebuilds).
struct ColumnKey {
    int x{ 0 };
    int z{ 0 };
    bool operator==(const ColumnKey&) const = default;
};
struct ColumnKeyHash {
    std::size_t operator()(const ColumnKey& k) const {
        return (static_cast<std::size_t>(static_cast<std::uint32_t>(k.x)) << 32) ^
               static_cast<std::size_t>(static_cast<std::uint32_t>(k.z));
    }
};
ColumnKey column_key(const VoxelColumn& column, float cellSize) {
    return { static_cast<int>(std::llround(column.x / cellSize)),
             static_cast<int>(std::llround(column.z / cellSize)) };
}

// Tile grid key (tile indices on the tiled navmesh).
struct TileKey {
    int x{ 0 };
    int z{ 0 };
    bool operator==(const TileKey&) const = default;
};
struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const {
        return (static_cast<std::size_t>(static_cast<std::uint32_t>(k.x)) << 32) ^
               static_cast<std::size_t>(static_cast<std::uint32_t>(k.z));
    }
};

// Bakes the navmesh for one rectangular region [minX,maxX]x[minZ,maxZ] (world
// units). `borderCells` expands the rasterized heightfield on x/z so geometry
// from neighbor tiles is present for connectivity; rcBuildRegions clips the
// border ring away, so the produced polys cover exactly the nominal region.
// `exactCells` must be true for tiled mode: the heightfield then spans EXACTLY
// [bmin,bmax] (floor((bmax-bmin)/cs) cells), keeping the border ring
// symmetric so Recast's portal edges land on the nominal tile border and
// adjacent tiles link; with the legacy +1 cell the interior becomes
// asymmetric and portals never coincide. The legacy single navmesh keeps the
// +1 cell convention (exactCells = false) for unchanged behavior.
//
// Returns true with `outData` EMPTY when the region has no walkable surface
// (a valid empty tile — the caller decides what that means for the mode);
// returns false with a diagnostic only on a real bake failure.
bool bake_navmesh_data(const NavmeshConfig& config,
                       float minX, float minZ, float maxX, float maxZ,
                       int borderCells, bool exactCells,
                       const std::vector<VoxelColumn>& columns,
                       std::vector<std::byte>& outData,
                       int outTileX, int outTileY,
                       std::string& errorOut) {
    outData.clear();
    const float bmin[3] = { minX - borderCells * config.cellSize,
                            config.boundsMinY,
                            minZ - borderCells * config.cellSize };
    const float bmax[3] = { maxX + borderCells * config.cellSize,
                            config.boundsMaxY,
                            maxZ + borderCells * config.cellSize };
    const int spanX = world_to_cell(bmax[0], bmin[0], config.cellSize);
    const int spanZ = world_to_cell(bmax[2], bmin[2], config.cellSize);
    const int sizeX = spanX + (exactCells ? 0 : 1);
    const int sizeZ = spanZ + (exactCells ? 0 : 1);
    const int walkableHeight = static_cast<int>(
        std::ceil(config.agentHeight / config.cellHeight));
    const int walkableClimb = static_cast<int>(
        std::ceil(config.agentMaxClimb / config.cellHeight));
    const int walkableRadius = static_cast<int>(
        std::ceil(config.agentRadius / config.cellSize));
    const int maxEdgeLen = static_cast<int>(config.maxEdgeLen / config.cellSize);

    rcContext context(false);  // no logging/timers (stack context, new API)
    rcHeightfield solid;
    if (!rcCreateHeightfield(&context, solid, sizeX, sizeZ, bmin, bmax,
                             config.cellSize, config.cellHeight)) {
        errorOut = "navigation: rcCreateHeightfield failed";
        return false;
    }

    // Rasterize the walkable surface: each solid column becomes a span whose
    // TOP face is the walkable surface. Columns outside the (border-expanded)
    // grid are skipped by the cell check.
    int rasterized = 0;
    for (const VoxelColumn& column : columns) {
        if (!column.solid || column.solidMaxY <= column.solidMinY) continue;
        const int x = world_to_cell(column.x, bmin[0], config.cellSize);
        const int z = world_to_cell(column.z, bmin[2], config.cellSize);
        if (x < 0 || x >= sizeX || z < 0 || z >= sizeZ) continue;
        const int spanMin = world_to_cell(column.solidMinY, bmin[1],
                                          config.cellHeight);
        const int spanMax = world_to_cell(column.solidMaxY, bmin[1],
                                          config.cellHeight);
        if (spanMax <= spanMin) continue;
        if (rcAddSpan(&context, solid, x, z,
                      static_cast<unsigned short>(std::max(spanMin, 0)),
                      static_cast<unsigned short>(spanMax), kWalkableArea,
                      1000)) {
            ++rasterized;
        }
    }
    if (rasterized == 0) {
        // No walkable surface in this region: an empty tile, not an error.
        errorOut.clear();
        return true;
    }

    // Standard filtering: remove unreachable low-hanging obstacles, ledges
    // and spans that do not leave enough headroom for the agent.
    rcFilterLowHangingWalkableObstacles(&context, walkableClimb, solid);
    rcFilterLedgeSpans(&context, walkableHeight, walkableClimb, solid);
    rcFilterWalkableLowHeightSpans(&context, walkableHeight, solid);

    rcCompactHeightfield chf;
    if (!rcBuildCompactHeightfield(&context, walkableHeight, walkableClimb,
                                   solid, chf)) {
        errorOut = "navigation: rcBuildCompactHeightfield failed";
        return false;
    }

    if (!rcErodeWalkableArea(&context, walkableRadius, chf)) {
        errorOut = "navigation: rcErodeWalkableArea failed";
        return false;
    }
    if (!rcBuildDistanceField(&context, chf)) {
        errorOut = "navigation: rcBuildDistanceField failed";
        return false;
    }
    // The border region is clipped here: rcBuildRegions marks cells within
    // `borderCells` of the expanded heightfield edge as border (null area),
    // so no polygons form on the border ring and the tile's polys end
    // exactly at its nominal bounds — adjacent tiles share the same world
    // vertices and Detour links them.
    if (!rcBuildRegions(&context, chf, borderCells, config.minRegionArea,
                        config.mergeRegionArea)) {
        errorOut = "navigation: rcBuildRegions failed";
        return false;
    }

    rcContourSet contours;
    if (!rcBuildContours(&context, chf, config.maxSimplificationError,
                         maxEdgeLen, contours)) {
        errorOut = "navigation: rcBuildContours failed";
        return false;
    }
    rcPolyMesh mesh;
    if (!rcBuildPolyMesh(&context, contours, 6, mesh)) {
        errorOut = "navigation: rcBuildPolyMesh failed";
        return false;
    }

    rcPolyMeshDetail detail;
    if (!rcBuildPolyMeshDetail(&context, mesh, chf, config.detailSampleDist,
                               config.detailSampleMaxError, detail)) {
        errorOut = "navigation: rcBuildPolyMeshDetail failed";
        return false;
    }

    if (mesh.npolys == 0) {
        // No polygons in this region: an empty tile, not an error.
        errorOut.clear();
        return true;
    }

    // Slope rejection: this Recast build (new API) keeps no triangle list
    // on the poly mesh, so decompose each polygon into a fan and test the
    // face slope against agentMaxSlope; steeper polygons are marked
    // RC_NULL_AREA (not walkable) so they never appear in queries.
    const float walkableLimitY = std::cos(config.agentMaxSlope * (RC_PI / 180.0f));
    for (int i = 0; i < mesh.npolys; ++i) {
        const unsigned short* p = &mesh.polys[i * mesh.nvp * 2];
        int nv = 0;
        while (nv < mesh.nvp && p[nv] != RC_MESH_NULL_IDX) ++nv;
        if (nv < 3) {
            mesh.areas[i] = RC_NULL_AREA;
            continue;
        }
        const unsigned short* v0 = &mesh.verts[p[0] * 3];
        const float v0x = static_cast<float>(v0[0]);
        const float v0y = static_cast<float>(v0[1]);
        const float v0z = static_cast<float>(v0[2]);
        bool steep = false;
        for (int j = 1; j + 1 < nv && !steep; ++j) {
            const unsigned short* va = &mesh.verts[p[j] * 3];
            const unsigned short* vb = &mesh.verts[p[j + 1] * 3];
            // World-space edges: x/z scale by cs, y by ch.
            const float e0x = (static_cast<float>(va[0]) - v0x) * mesh.cs;
            const float e0y = (static_cast<float>(va[1]) - v0y) * mesh.ch;
            const float e0z = (static_cast<float>(va[2]) - v0z) * mesh.cs;
            const float e1x = (static_cast<float>(vb[0]) - v0x) * mesh.cs;
            const float e1y = (static_cast<float>(vb[1]) - v0y) * mesh.ch;
            const float e1z = (static_cast<float>(vb[2]) - v0z) * mesh.cs;
            const float nx = e0y * e1z - e0z * e1y;
            const float ny = e0z * e1x - e0x * e1z;
            const float nz = e0x * e1y - e0y * e1x;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len <= 0.0f) continue;
            // |ny|/len: faces are wound CCW viewed from above; taking the
            // absolute keeps the slope test correct for either winding.
            if (std::fabs(ny) / len <= walkableLimitY) steep = true;
        }
        if (steep) mesh.areas[i] = RC_NULL_AREA;
    }

    // Package the poly mesh into Detour tile data. Only walkable-area
    // polygons get the walk flag; RC_NULL_AREA (steep/unwalkable) polys
    // keep flag 0 and are excluded by the query filter.
    std::vector<unsigned short> polyFlags(mesh.npolys, 0);
    for (int i = 0; i < mesh.npolys; ++i) {
        if (mesh.areas[i] == RC_WALKABLE_AREA) polyFlags[i] = kWalkFlag;
    }
    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));
    params.verts = mesh.verts;
    params.vertCount = mesh.nverts;
    params.polys = mesh.polys;
    params.polyAreas = mesh.areas;
    params.polyFlags = polyFlags.data();
    params.polyCount = mesh.npolys;
    params.nvp = mesh.nvp;
    params.detailMeshes = detail.meshes;
    params.detailVerts = detail.verts;
    params.detailVertsCount = detail.nverts;
    params.detailTris = detail.tris;
    params.detailTriCount = detail.ntris;
    params.offMeshConCount = 0;
    params.buildBvTree = true;
    params.walkableHeight = config.agentHeight;
    params.walkableRadius = config.agentRadius;
    params.walkableClimb = config.agentMaxClimb;
    params.tileX = outTileX;
    params.tileY = outTileY;
    params.tileLayer = 0;
    std::memcpy(params.bmin, mesh.bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, mesh.bmax, sizeof(params.bmax));
    params.cs = mesh.cs;
    params.ch = mesh.ch;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        errorOut = "navigation: dtCreateNavMeshData failed";
        return false;
    }
    outData.resize(static_cast<std::size_t>(navDataSize));
    std::memcpy(outData.data(), navData, static_cast<std::size_t>(navDataSize));
    dtFree(navData);
    errorOut.clear();
    return true;
}

}  // namespace

class RecastNavigationProvider final : public INavigationProvider {
public:
    RecastNavigationProvider() : query_(dtAllocNavMeshQuery()) {}

    ~RecastNavigationProvider() override {
        if (navmesh_) dtFreeNavMesh(navmesh_);
        if (query_) dtFreeNavMeshQuery(query_);
    }

    bool build(const NavmeshConfig& config, const std::vector<VoxelColumn>& columns,
               std::string& errorOut) override {
        if (config.cellSize <= 0.0f || config.cellHeight <= 0.0f ||
            config.boundsMaxX <= config.boundsMinX ||
            config.boundsMaxZ <= config.boundsMinZ) {
            errorOut = "navigation: invalid config (bounds/cell size)";
            return false;
        }
        if (config.tileSize != 0.0f) {
            if (config.tileSize < config.cellSize) {
                errorOut = "navigation: tileSize must be zero (single navmesh) "
                           "or >= cellSize";
                return false;
            }
            return build_tiled(config, columns, errorOut);
        }
        return build_single(config, columns, errorOut);
    }

    bool update(const std::vector<VoxelColumn>& changedColumns,
                std::string& errorOut) override {
        if (tileSize_ <= 0.0f) {
            errorOut = "navigation: update() requires tiled mode "
                       "(NavmeshConfig::tileSize > 0)";
            return false;
        }
        if (changedColumns.empty()) {
            errorOut = "navigation: update() requires at least one changed column";
            return false;
        }
        if (columnsByKey_.empty()) {
            errorOut = "navigation: update() before build()";
            return false;
        }

        // Apply the delta to the stored column set, remembering the previous
        // values so a bake failure can roll the stored set back (all-or-nothing).
        struct OldColumn {
            bool present{ false };
            VoxelColumn value;
        };
        std::unordered_map<ColumnKey, OldColumn, ColumnKeyHash> oldColumns;
        oldColumns.reserve(changedColumns.size());
        for (const VoxelColumn& column : changedColumns) {
            const ColumnKey key = column_key(column, config_.cellSize);
            const auto it = columnsByKey_.find(key);
            OldColumn old;
            old.present = it != columnsByKey_.end();
            if (old.present) old.value = it->second;
            oldColumns.emplace(key, old);
            columnsByKey_[key] = column;
        }

        // Affected tiles: every tile whose EXPANDED rasterization (border
        // included) overlaps a changed column. A column near a tile corner
        // can therefore touch up to four tiles — all of them re-baked.
        const float borderWorld = static_cast<float>(borderCells_) * config_.cellSize;
        std::unordered_set<TileKey, TileKeyHash> affected;
        for (const VoxelColumn& column : changedColumns) {
            const int txMin = tile_index(column.x - borderWorld);
            const int txMax = tile_index(column.x + borderWorld);
            const int tzMin = tile_index(column.z - borderWorld);
            const int tzMax = tile_index(column.z + borderWorld);
            for (int tz = tzMin; tz <= tzMax; ++tz) {
                for (int tx = txMin; tx <= txMax; ++tx) {
                    if (tx >= 0 && tx < tileCountX_ && tz >= 0 && tz < tileCountZ_) {
                        affected.insert({ tx, tz });
                    }
                }
            }
        }
        if (affected.empty()) {
            errorOut = "navigation: changed columns outside the navmesh bounds";
            return false;
        }

        // Deterministic bake order (z-major, like build).
        std::vector<TileKey> order(affected.begin(), affected.end());
        std::sort(order.begin(), order.end(), [](const TileKey& a, const TileKey& b) {
            return a.z != b.z ? a.z < b.z : a.x < b.x;
        });

        // Bake every affected tile FIRST (all-or-nothing): no tile is swapped
        // unless every bake succeeds; an empty bake removes the tile.
        std::vector<VoxelColumn> fullColumns;
        fullColumns.reserve(columnsByKey_.size());
        for (const auto& entry : columnsByKey_) fullColumns.push_back(entry.second);
        struct Baked {
            TileKey key;
            std::vector<std::byte> data;
        };
        std::vector<Baked> baked;
        baked.reserve(order.size());
        for (const TileKey& key : order) {
            std::vector<std::byte> data;
            std::string bakeError;
            if (!bake_navmesh_data(config_, tile_min_x(key.x), tile_min_z(key.z),
                                   tile_max_x(key.x), tile_max_z(key.z),
                                   borderCells_, true, fullColumns, data, key.x,
                                   key.z, bakeError)) {
                // Roll the stored set back and report — nothing was swapped.
                for (const auto& entry : oldColumns) {
                    if (entry.second.present) {
                        columnsByKey_[entry.first] = entry.second.value;
                    } else {
                        columnsByKey_.erase(entry.first);
                    }
                }
                errorOut = bakeError.empty() ? "navigation: local tile bake failed"
                                             : bakeError;
                return false;
            }
            baked.push_back({ key, std::move(data) });
        }

        // Swap: remove the old tiles, add the freshly baked ones.
        for (Baked& b : baked) {
            const auto it = tiles_.find(b.key);
            if (it != tiles_.end()) {
                navmesh_->removeTile(it->second.ref, nullptr, nullptr);
                tiles_.erase(it);
            }
            if (b.data.empty()) continue;  // the tile became empty (e.g. dug out)
            TileState state;
            state.data = std::move(b.data);
            if (dtStatusFailed(navmesh_->addTile(
                    reinterpret_cast<unsigned char*>(state.data.data()),
                    static_cast<int>(state.data.size()), 0, 0, &state.ref))) {
                errorOut = "navigation: addTile failed during local update";
                return false;
            }
            tiles_[b.key] = std::move(state);
        }
        ++revision_;
        for (Baked& b : baked) {
            const auto it = tiles_.find(b.key);
            if (it != tiles_.end()) it->second.revision = revision_;
        }
        errorOut.clear();
        return true;
    }

    bool find_path(float startX, float startY, float startZ, float goalX,
                   float goalY, float goalZ, PathResult& out) const override {
        out = PathResult{};
        if (!navmesh_ || !query_ || revision_ == 0) return false;
        const float start[3] = { startX, startY, startZ };
        const float goal[3] = { goalX, goalY, goalZ };
        const float ext[3] = { config_.agentRadius * 4.0f, config_.agentHeight,
                               config_.agentRadius * 4.0f };
        dtQueryFilter filter;  // default: includes all flags (0xffff)

        dtPolyRef startRef = 0, goalRef = 0;
        float startPt[3], goalPt[3];
        if (dtStatusFailed(query_->findNearestPoly(start, ext, &filter, &startRef,
                                                   startPt)) ||
            dtStatusFailed(query_->findNearestPoly(goal, ext, &filter, &goalRef,
                                                   goalPt))) {
            return false;
        }
        if (startRef == 0 || goalRef == 0) return false;

        dtPolyRef path[2048];
        int pathCount = 0;
        const dtStatus pathStatus =
            query_->findPath(startRef, goalRef, startPt, goalPt, &filter, path,
                             &pathCount, config_.maxPolys);
        if (dtStatusFailed(pathStatus) || (pathStatus & DT_PARTIAL_RESULT)) {
            // Partial result = the query could not reach the goal polygon
            // (e.g. an island cut off by maxClimb); report the goal as
            // unreachable instead of a best-guess path.
            return false;
        }
        if (pathCount == 0) return false;

        float straight[4096];
        unsigned char straightFlags[2048];
        dtPolyRef straightPolys[2048];
        int straightCount = 0;
        if (dtStatusFailed(query_->findStraightPath(
                startPt, goalPt, path, pathCount, straight, straightFlags,
                straightPolys, &straightCount, 4096))) {
            return false;
        }
        if (straightCount == 0) return false;

        out.waypoints.reserve(static_cast<std::size_t>(straightCount) * 3);
        float previous[3] = { straight[0], straight[1], straight[2] };
        for (int i = 0; i < straightCount; ++i) {
            const float* point = &straight[i * 3];
            out.waypoints.push_back(point[0]);
            out.waypoints.push_back(point[1]);
            out.waypoints.push_back(point[2]);
            if (i > 0) {
                const float dx = point[0] - previous[0];
                const float dy = point[1] - previous[1];
                const float dz = point[2] - previous[2];
                out.totalLength += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            previous[0] = point[0];
            previous[1] = point[1];
            previous[2] = point[2];
        }
        out.found = true;
        out.revision = revision_;
        return true;
    }

    bool is_walkable(float x, float y, float z) const override {
        if (!navmesh_ || !query_ || revision_ == 0) return false;
        const float pos[3] = { x, y, z };
        const float ext[3] = { config_.agentRadius * 2.0f, config_.agentHeight,
                               config_.agentRadius * 2.0f };
        dtQueryFilter filter;
        dtPolyRef ref = 0;
        float nearest[3];
        if (dtStatusFailed(query_->findNearestPoly(pos, ext, &filter, &ref,
                                                   nearest))) {
            return false;
        }
        return ref != 0;
    }

    uint64_t revision() const override { return revision_; }

    uint64_t tile_revision(float x, float z) const override {
        if (tileSize_ <= 0.0f) return 0;
        const int tx = static_cast<int>(std::floor((x - config_.boundsMinX) / tileSize_));
        const int tz = static_cast<int>(std::floor((z - config_.boundsMinZ) / tileSize_));
        if (tx < 0 || tx >= tileCountX_ || tz < 0 || tz >= tileCountZ_) return 0;
        const auto it = tiles_.find({ tx, tz });
        return it == tiles_.end() ? 0 : it->second.revision;
    }

    bool valid() const override { return navmesh_ != nullptr && revision_ > 0; }

private:
    struct TileState {
        dtTileRef ref{ 0 };
        std::vector<std::byte> data;  // owned buffer (addTile without DT_TILE_FREE_DATA)
        uint64_t revision{ 0 };
    };

    // ---- legacy single-navmesh mode (tileSize == 0) ----
    bool build_single(const NavmeshConfig& config,
                      const std::vector<VoxelColumn>& columns,
                      std::string& errorOut) {
        std::vector<std::byte> data;
        std::string bakeError;
        if (!bake_navmesh_data(config, config.boundsMinX, config.boundsMinZ,
                               config.boundsMaxX, config.boundsMaxZ, 0, false,
                               columns, data, 0, 0, bakeError)) {
            errorOut = bakeError.empty() ? "navigation: bake failed" : bakeError;
            return false;
        }
        if (data.empty()) {
            errorOut = "navigation: no walkable surface rasterized";
            return false;
        }
        if (navmesh_) {
            dtFreeNavMesh(navmesh_);
            navmesh_ = nullptr;
        }
        navmesh_ = dtAllocNavMesh();
        if (navmesh_ == nullptr ||
            dtStatusFailed(navmesh_->init(reinterpret_cast<unsigned char*>(data.data()),
                                          static_cast<int>(data.size()), 0))) {
            errorOut = "navigation: navmesh init failed";
            return false;
        }
        singleData_ = std::move(data);  // keep the buffer alive (flags 0)
        if (dtStatusFailed(query_->init(navmesh_, config.maxPolys))) {
            errorOut = "navigation: query init failed";
            return false;
        }
        config_ = config;
        tileSize_ = 0.0f;
        tileCountX_ = 0;
        tileCountZ_ = 0;
        borderCells_ = 0;
        tiles_.clear();
        columnsByKey_.clear();
        ++revision_;
        errorOut.clear();
        return true;
    }

    // ---- tiled mode (tileSize > 0) ----
    bool build_tiled(const NavmeshConfig& config,
                     const std::vector<VoxelColumn>& columns,
                     std::string& errorOut) {
        const float ts = config.tileSize;
        const int tileCountX = std::max(
            1, static_cast<int>(std::ceil((config.boundsMaxX - config.boundsMinX) / ts)));
        const int tileCountZ = std::max(
            1, static_cast<int>(std::ceil((config.boundsMaxZ - config.boundsMinZ) / ts)));
        const int borderCells = static_cast<int>(
            std::ceil(config.agentRadius / config.cellSize)) + 3;

        // Bake every tile FIRST (all-or-nothing): no tile is added unless
        // every bake succeeds; an empty tile bakes to empty data.
        struct Baked {
            TileKey key;
            std::vector<std::byte> data;
        };
        std::vector<Baked> baked;
        baked.reserve(static_cast<std::size_t>(tileCountX) * tileCountZ);
        for (int tz = 0; tz < tileCountZ; ++tz) {
            for (int tx = 0; tx < tileCountX; ++tx) {
                const float minX = config.boundsMinX + tx * ts;
                const float minZ = config.boundsMinZ + tz * ts;
                const float maxX = std::min(config.boundsMaxX, minX + ts);
                const float maxZ = std::min(config.boundsMaxZ, minZ + ts);
                std::vector<std::byte> data;
                std::string bakeError;
                if (!bake_navmesh_data(config, minX, minZ, maxX, maxZ, borderCells,
                                       true, columns, data, tx, tz, bakeError)) {
                    errorOut = bakeError.empty() ? "navigation: tile bake failed"
                                                 : bakeError;
                    return false;
                }
                baked.push_back({ { tx, tz }, std::move(data) });
            }
        }
        if (std::none_of(baked.begin(), baked.end(),
                         [](const Baked& b) { return !b.data.empty(); })) {
            errorOut = "navigation: no walkable surface rasterized";
            return false;
        }

        if (navmesh_) {
            dtFreeNavMesh(navmesh_);
            navmesh_ = nullptr;
        }
        navmesh_ = dtAllocNavMesh();
        if (navmesh_ == nullptr) {
            errorOut = "navigation: out of memory";
            return false;
        }
        dtNavMeshParams params;
        std::memset(&params, 0, sizeof(params));
        params.orig[0] = config.boundsMinX;
        params.orig[1] = config.boundsMinY;
        params.orig[2] = config.boundsMinZ;
        params.tileWidth = ts;
        params.tileHeight = ts;
        params.maxTiles = tileCountX * tileCountZ;
        params.maxPolys = config.maxPolys;
        if (dtStatusFailed(navmesh_->init(&params))) {
            errorOut = "navigation: tiled navmesh init failed";
            return false;
        }
        tiles_.clear();
        for (Baked& b : baked) {
            if (b.data.empty()) continue;
            TileState state;
            state.data = std::move(b.data);
            if (dtStatusFailed(navmesh_->addTile(
                    reinterpret_cast<unsigned char*>(state.data.data()),
                    static_cast<int>(state.data.size()), 0, 0, &state.ref))) {
                errorOut = "navigation: addTile failed";
                return false;
            }
            tiles_[b.key] = std::move(state);
        }
        if (dtStatusFailed(query_->init(navmesh_, config.maxPolys))) {
            errorOut = "navigation: query init failed";
            return false;
        }
        config_ = config;
        tileSize_ = ts;
        tileCountX_ = tileCountX;
        tileCountZ_ = tileCountZ;
        borderCells_ = borderCells;
        ++revision_;
        for (auto& entry : tiles_) entry.second.revision = revision_;
        // Store the full column set for incremental updates.
        columnsByKey_.clear();
        columnsByKey_.reserve(columns.size());
        for (const VoxelColumn& column : columns) {
            columnsByKey_[column_key(column, config.cellSize)] = column;
        }
        errorOut.clear();
        return true;
    }

    int tile_index(float world) const {
        return static_cast<int>(std::floor((world - config_.boundsMinX) / tileSize_));
    }
    float tile_min_x(int tx) const { return config_.boundsMinX + tx * tileSize_; }
    float tile_min_z(int tz) const { return config_.boundsMinZ + tz * tileSize_; }
    float tile_max_x(int tx) const {
        return std::min(config_.boundsMaxX, tile_min_x(tx) + tileSize_);
    }
    float tile_max_z(int tz) const {
        return std::min(config_.boundsMaxZ, tile_min_z(tz) + tileSize_);
    }

    dtNavMesh* navmesh_{ nullptr };
    dtNavMeshQuery* query_{ nullptr };
    NavmeshConfig config_;
    uint64_t revision_{ 0 };

    // Tiled mode state.
    float tileSize_{ 0.0f };
    int tileCountX_{ 0 };
    int tileCountZ_{ 0 };
    int borderCells_{ 0 };
    std::vector<std::byte> singleData_;  // owned buffer in single mode
    std::unordered_map<TileKey, TileState, TileKeyHash> tiles_;
    std::unordered_map<ColumnKey, VoxelColumn, ColumnKeyHash> columnsByKey_;
};

std::unique_ptr<INavigationProvider> create_recast_navigation_provider() {
    return std::make_unique<RecastNavigationProvider>();
}

std::vector<VoxelColumn> sample_voxel_columns(
    const engine::voxel::IVoxelWorld& world, const NavmeshConfig& config,
    std::string& errorOut) {
    std::vector<VoxelColumn> columns;
    if (config.cellSize <= 0.0f) {
        errorOut = "navigation: invalid cell size";
        return columns;
    }
    const float step = config.cellSize;
    const int topY = static_cast<int>(std::floor(config.boundsMaxY));
    const int bottomY = static_cast<int>(std::floor(config.boundsMinY));
    const float startX = std::floor(config.boundsMinX / step) * step;
    const float startZ = std::floor(config.boundsMinZ / step) * step;
    for (float x = startX; x <= config.boundsMaxX; x += step) {
        for (float z = startZ; z <= config.boundsMaxZ; z += step) {
            const int ix = static_cast<int>(std::floor(x));
            const int iz = static_cast<int>(std::floor(z));
            int surfaceY = -1;
            for (int y = topY; y >= bottomY; --y) {
                if (world.get_block(ix, y, iz) != 0) {  // non-air = solid
                    surfaceY = y;
                    break;
                }
            }
            if (surfaceY < 0) continue;  // air column: nothing to walk on
            VoxelColumn column;
            column.x = x;
            column.z = z;
            column.solidMinY = static_cast<float>(surfaceY);
            column.solidMaxY = static_cast<float>(surfaceY) + 1.0f;
            column.solid = true;
            columns.push_back(column);
        }
    }
    errorOut.clear();
    return columns;
}

}  // namespace navigation
}  // namespace engine

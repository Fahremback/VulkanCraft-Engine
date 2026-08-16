// RecastNavigation.cpp — Recast/Detour-backed navigation provider (META
// section 16). This is the ONLY translation unit that includes the Recast and
// Detour headers; the public contract lives in engine/navigation/* and never
// leaks them. The baker follows the standard SoloMesh pipeline: rasterize the
// voxel surface into a heightfield, filter walkable spans, build
// compact heightfield / regions / contours / poly mesh / detail mesh, then
// create a Detour navmesh and query object.

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
#include <cstdint>
#include <cstring>
#include <memory>
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

}  // namespace

class RecastNavigationProvider final : public INavigationProvider {
public:
    RecastNavigationProvider() : query_(dtAllocNavMeshQuery()) {}

    ~RecastNavigationProvider() override {
        if (query_) dtFreeNavMeshQuery(query_);
        if (navmesh_) dtFreeNavMesh(navmesh_);
    }

    bool build(const NavmeshConfig& config, const std::vector<VoxelColumn>& columns,
               std::string& errorOut) override {
        if (config.cellSize <= 0.0f || config.cellHeight <= 0.0f ||
            config.boundsMaxX <= config.boundsMinX ||
            config.boundsMaxZ <= config.boundsMinZ) {
            errorOut = "navigation: invalid config (bounds/cell size)";
            return false;
        }
        const float bmin[3] = { config.boundsMinX, config.boundsMinY,
                                config.boundsMinZ };
        const float bmax[3] = { config.boundsMaxX, config.boundsMaxY,
                                config.boundsMaxZ };
        const int sizeX = world_to_cell(config.boundsMaxX, config.boundsMinX,
                                        config.cellSize) + 1;
        const int sizeZ = world_to_cell(config.boundsMaxZ, config.boundsMinZ,
                                        config.cellSize) + 1;
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

        // Rasterize the walkable surface: each solid column becomes a span
        // whose TOP face is the walkable surface.
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
            errorOut = "navigation: no walkable surface rasterized";
            return false;
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
        if (!rcBuildRegions(&context, chf, 0, config.minRegionArea,
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
            errorOut = "navigation: navmesh has no polygons";
            return false;
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

        if (navmesh_) dtFreeNavMesh(navmesh_);
        navmesh_ = dtAllocNavMesh();
        if (navmesh_ == nullptr ||
            dtStatusFailed(navmesh_->init(navData, navDataSize,
                                          DT_TILE_FREE_DATA))) {
            if (navData) dtFree(navData);
            errorOut = "navigation: navmesh init failed";
            return false;
        }
        if (dtStatusFailed(query_->init(navmesh_, config.maxPolys))) {
            errorOut = "navigation: query init failed";
            return false;
        }
        config_ = config;
        ++revision_;
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
    bool valid() const override { return navmesh_ != nullptr && revision_ > 0; }

private:
    dtNavMesh* navmesh_{ nullptr };
    dtNavMeshQuery* query_{ nullptr };
    NavmeshConfig config_;
    uint64_t revision_{ 0 };
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

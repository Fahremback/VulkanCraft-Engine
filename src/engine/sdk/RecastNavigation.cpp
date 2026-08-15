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
#include <iostream>
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
        std::cerr << "[nav-build] create hf\n";
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
        std::cerr << "[nav-build] filter lowhang\n";
        rcFilterLedgeSpans(&context, walkableHeight, walkableClimb, solid);
        rcFilterWalkableLowHeightSpans(&context, walkableHeight, solid);
        std::cerr << "[nav-build] filters done\n";

        rcCompactHeightfield chf;
        if (!rcBuildCompactHeightfield(&context, walkableHeight, walkableClimb,
                                       solid, chf)) {
            errorOut = "navigation: rcBuildCompactHeightfield failed";
            return false;
        }

        std::cerr << "[nav-build] chf built (" << chf.width << "x" << chf.height
                  << ")\n";
        if (!rcErodeWalkableArea(&context, walkableRadius, chf)) {
            errorOut = "navigation: rcErodeWalkableArea failed";
            return false;
        }
        std::cerr << "[nav-build] erode done\n";
        if (!rcBuildDistanceField(&context, chf)) {
            errorOut = "navigation: rcBuildDistanceField failed";
            return false;
        }
        std::cerr << "[nav-build] distance done\n";
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

        std::cerr << "[nav-build] regions done\n";
        rcPolyMesh mesh;
        if (!rcBuildPolyMesh(&context, contours, 6, mesh)) {
            errorOut = "navigation: rcBuildPolyMesh failed";
            return false;
        }
        std::cerr << "[nav-build] polymesh done (" << mesh.nverts << "v "
                  << mesh.npolys << "p) bmin=(" << mesh.bmin[0] << ","
                  << mesh.bmin[1] << "," << mesh.bmin[2] << ") bmax=("
                  << mesh.bmax[0] << "," << mesh.bmax[1] << "," << mesh.bmax[2]
                  << ")\n";
        for (int vi = 0; vi < mesh.nverts && vi < 8; ++vi) {
            std::cerr << "[nav-build]   v" << vi << "=(" << mesh.verts[vi * 3]
                      << "," << mesh.verts[vi * 3 + 1] << ","
                      << mesh.verts[vi * 3 + 2] << ")\n";
        }

        rcPolyMeshDetail detail;
        if (!rcBuildPolyMeshDetail(&context, mesh, chf, config.detailSampleDist,
                                   config.detailSampleMaxError, detail)) {
            errorOut = "navigation: rcBuildPolyMeshDetail failed";
            return false;
        }

        std::cerr << "[nav-build] detail done\n";
        if (mesh.npolys == 0) {
            errorOut = "navigation: navmesh has no polygons";
            return false;
        }

        // Package the poly mesh into Detour tile data.
        std::vector<unsigned short> polyFlags(mesh.npolys, kWalkFlag);
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
        if (dtStatusFailed(query_->findPath(startRef, goalRef, startPt, goalPt,
                                            &filter, path, &pathCount,
                                            config_.maxPolys))) {
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

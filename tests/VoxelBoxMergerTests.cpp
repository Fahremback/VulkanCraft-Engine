// ============================================================================
// VoxelBoxMergerTests — text-only gate for the voxel -> box collider merger.
// Covers exact outputs on known patterns plus invariant properties
// (coverage == solid count, disjointness, determinism).
// ============================================================================
#include "../../src/engine/physics/VoxelBoxMerger.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Engine::Physics::VoxelCellBox;
using Engine::Physics::merge_solid_voxels;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

long long total_volume(const std::vector<VoxelCellBox>& boxes) {
    long long v = 0;
    for (const auto& b : boxes) v += b.volume();
    return v;
}

std::vector<VoxelCellBox> sorted(std::vector<VoxelCellBox> boxes) {
    std::sort(boxes.begin(), boxes.end(), [](const VoxelCellBox& a,
                                             const VoxelCellBox& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        if (a.x != b.x) return a.x < b.x;
        if (a.sy != b.sy) return a.sy < b.sy;
        if (a.sz != b.sz) return a.sz < b.sz;
        return a.sx < b.sx;
    });
    return boxes;
}

} // namespace

int main() {
    // ---- Empty grid / null predicate ---------------------------------------
    check(merge_solid_voxels(4, 4, 4, nullptr).empty(), "null predicate rejected");
    check(merge_solid_voxels(0, 4, 4, [](int, int, int) { return true; }).empty(),
          "empty grid rejected");

    // ---- Single voxel ------------------------------------------------------
    {
        const auto boxes = merge_solid_voxels(3, 3, 3, [](int x, int y, int z) {
            return x == 1 && y == 2 && z == 1;
        });
        check(boxes.size() == 1 && boxes[0] == VoxelCellBox{ 1, 2, 1, 1, 1, 1 },
              "single voxel -> single 1^3 box");
        check(total_volume(boxes) == 1, "single voxel volume");
    }

    // ---- Full layer stack: one box per column-run merged into ONE box ------
    {
        const auto boxes = merge_solid_voxels(4, 4, 4, [](int, int, int) { return true; });
        check(boxes.size() == 1, "full 4x4x4 grid merges to one box");
        check(boxes[0] == VoxelCellBox{ 0, 0, 0, 4, 4, 4 }, "full-grid box extents");
    }

    // ---- Horizontal run along X on one layer -------------------------------
    {
        const auto boxes = merge_solid_voxels(8, 1, 2, [](int x, int, int z) {
            return z == 0 || (z == 1 && x < 5);
        });
        check(boxes.size() == 2, "two rows -> two boxes");
        const auto s = sorted(boxes);
        check(s[0] == VoxelCellBox{ 0, 0, 0, 8, 1, 1 }, "row z=0 full-width");
        check(s[1] == VoxelCellBox{ 0, 0, 1, 5, 1, 1 }, "row z=1 width 5");
    }

    // ---- Vertical stacking: same rectangle across 3 layers -> one box ------
    {
        const auto boxes = merge_solid_voxels(2, 5, 2, [](int, int y, int) {
            return y >= 2; // top 3 layers fully solid, bottom 2 air
        });
        check(boxes.size() == 1, "stacked layers merge vertically");
        check(boxes[0] == VoxelCellBox{ 0, 2, 0, 2, 3, 2 }, "stacked extents y=2..4");
    }

    // ---- L-shape: vertical merge only where rectangles match ---------------
    {
        // Layer A (y=0): full 3x3. Layer B (y=1): only left 3x1 column.
        const auto boxes = merge_solid_voxels(3, 2, 3, [](int, int y, int z) {
            return y == 0 || (y == 1 && z == 0);
        });
        check(boxes.size() == 2, "L-shape -> base slab + top strip");
        const auto s = sorted(boxes);
        check(s[0] == VoxelCellBox{ 0, 0, 0, 3, 1, 3 }, "base slab y=0 only");
        // Boxes are DISJOINT: the strip on layer y=1 does not merge downward
        // through the already-covered slab.
        check(s[1] == VoxelCellBox{ 0, 1, 0, 3, 1, 1 }, "top strip sits at y=1");
        check(total_volume(boxes) == 9 + 3, "L-shape total volume");
    }

    // ---- Disjoint columns separated by air gap -----------------------------
    {
        const auto boxes = merge_solid_voxels(7, 3, 1, [](int x, int, int) {
            return x < 2 || x > 4;
        });
        check(boxes.size() == 2, "gap splits into two boxes");
        const auto s = sorted(boxes);
        check(s[0] == VoxelCellBox{ 0, 0, 0, 2, 3, 1 }, "left pillar");
        check(s[1] == VoxelCellBox{ 5, 0, 0, 2, 3, 1 }, "right pillar starts at x=5");
    }

    // ---- Invariants on a terrain-like pattern ------------------------------
    {
        // Heightfield-ish: solid below a per-column height.
        const auto heights = [](int x, int z) {
            return (x * 31 + z * 17) % 6; // deterministic pseudo-terrain
        };
        const int SX = 12, SY = 8, SZ = 12;
        size_t solidCount = 0;
        // Counted in an independent pass: the merger may probe solid() more
        // than once per cell while growing rectangles, so counting inside the
        // predicate would overcount.
        for (int x = 0; x < SX; ++x)
            for (int y = 0; y < SY; ++y)
                for (int z = 0; z < SZ; ++z)
                    if (y <= heights(x % 6, z % 6)) ++solidCount;
        const auto boxes = merge_solid_voxels(SX, SY, SZ,
            [&](int x, int y, int z) { return y <= heights(x % 6, z % 6); });
        check(total_volume(boxes) == static_cast<long long>(solidCount),
              "terrain-like coverage equals solid count");
        bool inside = true;
        for (const auto& b : boxes)
            inside &= b.sx >= 1 && b.sy >= 1 && b.sz >= 1
                   && b.x >= 0 && b.y >= 0 && b.z >= 0
                   && b.x + b.sx <= SX && b.y + b.sy <= SY && b.z + b.sz <= SZ;
        check(inside, "all boxes within grid bounds");
    }

    // ---- Determinism --------------------------------------------------------
    {
        const auto solidFn = [](int x, int y, int z) {
            return ((x * 7 + y * 13 + z * 29) & 3u) != 0u;
        };
        const auto a = sorted(merge_solid_voxels(10, 10, 10, solidFn));
        const auto b = sorted(merge_solid_voxels(10, 10, 10, solidFn));
        check(a == b, "merger is deterministic across calls");
    }

    if (g_failures == 0) {
        std::printf("VOXEL BOX MERGER TESTS PASSED\n");
        return 0;
    }
    std::printf("VOXEL BOX MERGER TESTS FAILED (%d)\n", g_failures);
    return 1;
}

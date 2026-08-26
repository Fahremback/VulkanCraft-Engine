#pragma once
// ============================================================================
// VoxelBoxMerger — dense voxel grid -> near-minimal set of axis-aligned boxes.
//
// Pure math, no engine/GPU/physics-backend dependencies: consumable by the
// editor play runtime, the server bridge, tests and external SDK projects.
// Deterministic by construction (fixed scan order), O(cells) time.
// ============================================================================
#include <cstddef>
#include <functional>
#include <vector>

namespace Engine::Physics {

// Axis-aligned box in voxel CELL units: inclusive min corner (x,y,z) and
// extent (sx,sy,sz) >= 1. Convert to world space at the call site (cell size
// + volume origin belong to the caller).
struct VoxelCellBox {
    int x{ 0 }, y{ 0 }, z{ 0 };
    int sx{ 0 }, sy{ 0 }, sz{ 0 };
    [[nodiscard]] long long volume() const noexcept {
        return static_cast<long long>(sx) * static_cast<long long>(sy)
             * static_cast<long long>(sz);
    }
    [[nodiscard]] bool operator==(const VoxelCellBox&) const = default;
};

// Merge the solid voxels of a dense grid (sx × sy × sz) into boxes:
//   1. Per Y layer, cover the solid mask with greedy rectangles (extend
//      width along X, then depth along Z while the full row stays solid).
//   2. Stack vertically: an identical rectangle on consecutive layers grows
//      the same box instead of emitting a new one.
// `solid(x, y, z)` returns true for a solid cell; out-of-range calls are the
// caller's contract violation (the merger never probes outside the grid).
// Emission order follows first-creation order of each box's bottom rectangle.
[[nodiscard]] std::vector<VoxelCellBox> merge_solid_voxels(
    int sx, int sy, int sz,
    const std::function<bool(int, int, int)>& solid);

} // namespace Engine::Physics

namespace std {
template <> struct hash<Engine::Physics::VoxelCellBox> {
    size_t operator()(const Engine::Physics::VoxelCellBox& b) const noexcept {
        size_t h = 1469598103934665603ull;
        const auto mix = [&h](int v) {
            h ^= static_cast<size_t>(static_cast<unsigned>(v));
            h *= 1099511628211ull;
        };
        mix(b.x); mix(b.y); mix(b.z);
        mix(b.sx); mix(b.sy); mix(b.sz);
        return h;
    }
};
} // namespace std

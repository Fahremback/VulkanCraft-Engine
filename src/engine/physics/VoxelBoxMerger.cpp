// ============================================================================
// VoxelBoxMerger — implementation. See header for the contract.
// ============================================================================
#include "VoxelBoxMerger.hpp"

#include <unordered_map>
#include <unordered_set>

namespace Engine::Physics {
namespace {

struct RectKey {
    int x, z, w, d;
    bool operator==(const RectKey&) const = default;
};

struct RectHash {
    size_t operator()(const RectKey& r) const noexcept {
        size_t h = 1469598103934665603ull;
        const auto mix = [&h](int v) {
            h ^= static_cast<size_t>(static_cast<unsigned>(v));
            h *= 1099511628211ull;
        };
        mix(r.x); mix(r.z); mix(r.w); mix(r.d);
        return h;
    }
};

// A greedy rectangle from some layer that is currently growing upward across
// consecutive layers. `alive` runs close when a later layer lacks an identical
// rectangle (or the grid ends). Runs live in a vector so emission order is
// deterministic (first-creation order), independent of hash iteration.
struct BoxRun {
    RectKey key{};
    int y0{ 0 };
    int height{ 0 };
    bool alive{ true };
};

} // namespace

std::vector<VoxelCellBox> merge_solid_voxels(
    int sx, int sy, int sz,
    const std::function<bool(int, int, int)>& solid) {
    std::vector<VoxelCellBox> boxes;
    if (sx <= 0 || sy <= 0 || sz <= 0 || !solid) return boxes;

    std::vector<BoxRun> runs;

    for (int y = 0; y < sy; ++y) {
        // ---- Greedy rectangles over this layer's solid mask ----------------
        std::vector<uint8_t> visited(static_cast<size_t>(sx) * sz, 0);
        std::vector<RectKey> layerRects;
        for (int z = 0; z < sz; ++z) {
            for (int x = 0; x < sx; ++x) {
                if (visited[static_cast<size_t>(z) * sx + x]) continue;
                if (!solid(x, y, z)) continue;
                int w = 1;
                while (x + w < sx && !visited[static_cast<size_t>(z) * sx + (x + w)]
                       && solid(x + w, y, z)) ++w;
                int d = 1;
                bool grew = true;
                while (grew && z + d < sz) {
                    grew = false;
                    for (int i = 0; i < w; ++i) {
                        const int zz = z + d;
                        if (visited[static_cast<size_t>(zz) * sx + x + i]
                            || !solid(x + i, y, zz)) break;
                        if (i == w - 1) { ++d; grew = true; }
                    }
                }
                for (int dz = 0; dz < d; ++dz)
                    for (int dx = 0; dx < w; ++dx)
                        visited[static_cast<size_t>(z + dz) * sx + x + dx] = 1;
                layerRects.push_back(RectKey{ x, z, w, d });
            }
        }

        // ---- Continue runs from the previous layer / open new ones ---------
        std::unordered_set<RectKey, RectHash> present(layerRects.begin(),
                                                      layerRects.end());
        // A run whose rectangle vanished this layer closes HERE — its box is
        // emitted with the height accumulated so far. (Emitting only
        // still-alive runs at the end would drop every box that ever died,
        // i.e. almost all of them.)
        for (BoxRun& run : runs) {
            if (!run.alive) continue;
            if (!present.count(run.key)) {
                boxes.push_back(VoxelCellBox{
                    run.key.x, run.y0, run.key.z,
                    run.key.w, run.height, run.key.d });
                run.alive = false;
            }
        }
        // Index only still-alive runs: rect -> run position.
        std::unordered_map<RectKey, size_t, RectHash> aliveIdx;
        for (size_t i = 0; i < runs.size(); ++i)
            if (runs[i].alive) aliveIdx.emplace(runs[i].key, i);
        for (const RectKey& rect : layerRects) {
            const auto it = aliveIdx.find(rect);
            if (it != aliveIdx.end()) {
                ++runs[it->second].height;
            } else {
                runs.push_back(BoxRun{ rect, y, 1, true });
                aliveIdx.emplace(rect, runs.size() - 1);
            }
        }
    }
    // Grid exhausted: every still-alive run closes at the top.
    for (const BoxRun& run : runs) {
        if (!run.alive) continue;
        boxes.push_back(VoxelCellBox{ run.key.x, run.y0, run.key.z,
                                      run.key.w, run.height, run.key.d });
    }
    return boxes;
}

} // namespace Engine::Physics

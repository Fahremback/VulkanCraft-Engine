#include "ChunkLighting.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {

// Sorted vector of chunk-local lit cells, rebuilt per pass. Deterministic.
struct LitCell {
    uint32_t key;
    uint8_t light;
};

constexpr int kMaxPasses = 16;  // light levels strictly decrease per edge

}  // namespace

bool ChunkLighting::compute(Chunk& chunk, const ChunkLightAccess& access) {
    const int cx = chunk.chunkX;
    const int cz = chunk.chunkZ;
    const int baseX = cx * CHUNK_SIZE_X;
    const int baseZ = cz * CHUNK_SIZE_Z;

    // ---- Skylight: per-column occlusion height ----
    bool skyChanged = false;
    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
        for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
            uint16_t occlusionTop = 0;
            int topY = chunk.highestOccupiedY;
            for (const auto& entry : chunk.upperSections) {
                topY = std::max<int>(
                    topY, entry.first * VERTICAL_SECTION_SIZE + VERTICAL_SECTION_SIZE - 1);
            }
            topY = std::min<int>(topY, CHUNK_SIZE_Y - 1);
            for (int y = topY; y >= 0; --y) {
                if (access.absorption(access.blockAt(baseX + x, y, baseZ + z)) >= kMaxLight) {
                    occlusionTop = static_cast<uint16_t>(y);
                    break;
                }
            }
            if (chunk.skyOcclusionTop[x][z] != occlusionTop) {
                chunk.skyOcclusionTop[x][z] = occlusionTop;
                skyChanged = true;
            }
        }
    }

    // ---- Block light: emitters seed, relaxation to fixed point ----
    std::unordered_map<uint32_t, uint8_t> next;
    const auto in_domain = [&](int lx, int ly, int lz) {
        return lx >= 0 && lx < CHUNK_SIZE_X && ly >= 0 && ly < CHUNK_SIZE_Y &&
               lz >= 0 && lz < CHUNK_SIZE_Z;
    };
    const auto put = [&](std::unordered_map<uint32_t, uint8_t>& map,
                         int lx, int ly, int lz, uint8_t value) {
        if (!in_domain(lx, ly, lz)) return;
        if (value == 0) return;
        const uint32_t key = Chunk::light_key(lx, ly, lz);
        const auto found = map.find(key);
        if (found == map.end() || found->second < value) map[key] = value;
    };

    // (a) Seed with the chunk's own emitters.
    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
        for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
            const int topY = std::min<int>(
                chunk.highestOccupiedY + 16, CHUNK_SIZE_Y - 1);  // emitters near content
            for (int y = 0; y <= topY; ++y) {
                const uint8_t emit = access.emission(
                    access.blockAt(baseX + x, y, baseZ + z));
                if (emit > 0) put(next, x, y, z, emit);
            }
        }
    }
    // (b) Halo inflow: boundary cells adjacent to a lit neighbor chunk cell.
    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
        for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
            const int topY = std::min<int>(
                chunk.highestOccupiedY + 16, CHUNK_SIZE_Y - 1);
            for (int y = 0; y <= topY; ++y) {
                if (x != 0 && x != CHUNK_SIZE_X - 1 && z != 0 && z != CHUNK_SIZE_Z - 1) {
                    continue;  // only the boundary ring can see the halo
                }
                const int wx = baseX + x;
                const int wz = baseZ + z;
                const int neighbors[6][3] = {
                    { wx + 1, y, wz }, { wx - 1, y, wz },
                    { wx, y + 1, wz }, { wx, y - 1, wz },
                    { wx, y, wz + 1 }, { wx, y, wz - 1 } };
                for (const auto& n : neighbors) {
                    const uint8_t halo = access.blockLightAt(n[0], n[1], n[2]);
                    if (halo == 0) continue;
                    // Destination-cost, matching the relaxation below: the
                    // incoming light is reduced by the LIT cell's absorption
                    // (an opaque emitter still lights its surroundings).
                    // int arithmetic: a negative cost (opaque cell) must
                    // terminate, not wrap through uint8_t into a huge value.
                    const int rawCost = static_cast<int>(halo) - 1 -
                        static_cast<int>(access.absorption(access.blockAt(wx, y, wz)));
                    if (rawCost <= 0) continue;
                    put(next, x, y, z, static_cast<uint8_t>(rawCost));
                }
            }
        }
    }

    // Relax to fixed point: iterate lit cells in deterministic (y, z, x) order
    // and push light into their 6 neighbors (cost paid by the source cell).
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        bool changed = false;
        std::vector<LitCell> active;
        active.reserve(next.size());
        for (const auto& entry : next) active.push_back({ entry.first, entry.second });
        std::sort(active.begin(), active.end(), [](const LitCell& a, const LitCell& b) {
            return a.key < b.key;
        });
        for (const LitCell& cell : active) {
            if (cell.light <= 1) continue;  // cannot illuminate anything further
            const int lx = static_cast<int>(cell.key & 0xFu);
            const int lz = static_cast<int>((cell.key >> 4) & 0xFu);
            const int ly = static_cast<int>(cell.key >> 8);
            const int wx = baseX + lx;
            const int wz = baseZ + lz;
            const int neighbors[6][3] = {
                { wx + 1, ly, wz }, { wx - 1, ly, wz },
                { wx, ly + 1, wz }, { wx, ly - 1, wz },
                { wx, ly, wz + 1 }, { wx, ly, wz - 1 } };
            for (const auto& n : neighbors) {
                const int nl = n[0] - baseX;
                const int nly = n[1];
                const int nlz = n[2] - baseZ;
                if (!in_domain(nl, nly, nlz)) continue;  // light leaves the chunk
                // int arithmetic: opaque neighbors must STOP the light, not
                // wrap through uint8_t into a huge value that keeps spreading.
                const int rawCost = static_cast<int>(cell.light) - 1 -
                    static_cast<int>(access.absorption(access.blockAt(n[0], n[1], n[2])));
                if (rawCost <= 0) continue;
                const uint32_t nkey = Chunk::light_key(nl, nly, nlz);
                const auto found = next.find(nkey);
                const uint8_t cost = static_cast<uint8_t>(rawCost);
                if (found == next.end() || found->second < cost) {
                    next[nkey] = cost;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }

    const bool blockChanged = (next != chunk.blockLight);
    chunk.blockLight = std::move(next);
    return skyChanged || blockChanged;
}

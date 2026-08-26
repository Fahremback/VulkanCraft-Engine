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

bool ChunkLighting::compute(Chunk& chunk, const ChunkLightAccess& access,
                            bool skipSkylight,
                            const std::vector<uint32_t>* editedCells) {
    const int cx = chunk.chunkX;
    const int cz = chunk.chunkZ;
    const int baseX = cx * CHUNK_SIZE_X;
    const int baseZ = cz * CHUNK_SIZE_Z;

    // ---- C.1 passo 2: bounded region for content edits ----
    // Light attenuates by >= 1 per step and never exceeds kMaxLight, so no
    // light path from an edited cell reaches beyond kMaxLight steps. Cells
    // farther than that from EVERY edit are provably unchanged (any path from
    // the old state to them had cost > kMaxLight => contributed 0), so their
    // stored values are kept verbatim and only the AABB [edit +- kMaxLight]
    // is recomputed, seeded from the stored outside values as a fixed halo.
    // The emitter cache is NEVER replayed from a bounded pass (partial scan),
    // and the sky rescan covers only the edited columns.
    const bool bounded = !skipSkylight && editedCells != nullptr &&
                         !editedCells->empty();
    int ax0 = 0, ax1 = CHUNK_SIZE_X - 1;
    int az0 = 0, az1 = CHUNK_SIZE_Z - 1;
    bool editedColumn[CHUNK_SIZE_X][CHUNK_SIZE_Z] = {};
    if (bounded) {
        int minX = CHUNK_SIZE_X, maxX = -1, minZ = CHUNK_SIZE_Z, maxZ = -1;
        for (const uint32_t key : *editedCells) {
            const int lx = static_cast<int>(key & 0xFu);
            const int lz = static_cast<int>((key >> 4) & 0xFu);
            minX = std::min(minX, lx);
            maxX = std::max(maxX, lx);
            minZ = std::min(minZ, lz);
            maxZ = std::max(maxZ, lz);
            editedColumn[lx][lz] = true;
        }
        ax0 = std::max(0, minX - static_cast<int>(kMaxLight));
        ax1 = std::min(CHUNK_SIZE_X - 1, maxX + static_cast<int>(kMaxLight));
        az0 = std::max(0, minZ - static_cast<int>(kMaxLight));
        az1 = std::min(CHUNK_SIZE_Z - 1, maxZ + static_cast<int>(kMaxLight));
    }

    const int topY = std::min<int>(chunk.highestOccupiedY + 16, CHUNK_SIZE_Y - 1);

    // ---- Skylight: per-column occlusion height ----
    // C.1: when the caller proves the chunk's content is unchanged since the
    // last compute (dataVersion gate), every column's occlusion is identical
    // (occlusion is a pure function of the column's blocks), so the rescan is
    // skipped entirely — skyOcclusionTop is still valid. On a bounded pass
    // only the edited columns' occlusion can differ (each column's occlusion
    // depends only on its own blocks), so only those are rescanned.
    bool skyChanged = false;
    if (skipSkylight) {
        // No sky scan: the stored skyOcclusionTop is provably current.
        skyChanged = false;
    } else {
        for (int x = 0; x < CHUNK_SIZE_X; ++x) {
            for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                if (bounded && !editedColumn[x][z]) continue;
                uint16_t occlusionTop = 0;
                int colTop = chunk.highestOccupiedY;
                for (const auto& entry : chunk.upperSections) {
                    colTop = std::max<int>(
                        colTop,
                        entry.first * VERTICAL_SECTION_SIZE + VERTICAL_SECTION_SIZE - 1);
                }
                colTop = std::min<int>(colTop, CHUNK_SIZE_Y - 1);
                for (int y = colTop; y >= 0; --y) {
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
    }

    // ---- Block light: emitters seed, relaxation to fixed point ----
    std::unordered_map<uint32_t, uint8_t> next;
    const auto in_domain = [&](int lx, int ly, int lz) {
        return lx >= 0 && lx < CHUNK_SIZE_X && ly >= 0 && ly < CHUNK_SIZE_Y &&
               lz >= 0 && lz < CHUNK_SIZE_Z;
    };
    const auto in_aabb = [&](int lx, int lz) {
        return lx >= ax0 && lx <= ax1 && lz >= az0 && lz <= az1;
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
    // C.1 incremental: on a content-unchanged re-dirty (skipSkylight), the
    // emitter set is provably IDENTICAL (emission is a pure function of the
    // block; every block/fluid/state write bumps dataVersion and every
    // emission/absorption table change clears lightContentRevision_, so a
    // matching revision means both content AND tables are unchanged). Instead
    // of rescanning the 16x16x(topY+1) emitter grid, replay the cached seed
    // list built by the last full pass — same seeds, same deterministic
    // relaxation, bit-identical fixed point. The cache is keyed by revision
    // and never replayed when it does not match. A BOUNDED pass never rebuilds
    // the cache (it scanned only the AABB) — it invalidates it instead, so the
    // next full pass rescans completely.
    const uint64_t revision = chunk.revision();
    const bool canReplayEmitters =
        !bounded && skipSkylight && chunk.emitterCacheRevision == revision;
    if (canReplayEmitters) {
        for (const auto& entry : chunk.emitterCache) {
            if (entry.second == 0) continue;
            const auto found = next.find(entry.first);
            if (found == next.end() || found->second < entry.second) {
                next[entry.first] = entry.second;
            }
        }
    } else {
        const int x0 = bounded ? ax0 : 0;
        const int x1 = bounded ? ax1 : CHUNK_SIZE_X - 1;
        const int z0 = bounded ? az0 : 0;
        const int z1 = bounded ? az1 : CHUNK_SIZE_Z - 1;
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                for (int y = 0; y <= topY; ++y) {
                    const uint8_t emit = access.emission(
                        access.blockAt(baseX + x, y, baseZ + z));
                    if (emit > 0) put(next, x, y, z, emit);
                }
            }
        }
        if (bounded) {
            // Partial scan: never replay a stale cache from it.
            chunk.emitterCache.clear();
            chunk.emitterCacheRevision = 0;
        } else if (!skipSkylight) {
            // Full pass: rebuild the seed cache for the next content-unchanged
            // re-dirty. The map at this point holds exactly the emitter seeds.
            chunk.emitterCache.clear();
            chunk.emitterCache.reserve(next.size());
            for (const auto& entry : next) {
                if (entry.second > 0) chunk.emitterCache.push_back(entry);
            }
            chunk.emitterCacheRevision = revision;
        }
    }
    // (b) Halo inflow: boundary cells adjacent to a lit neighbor chunk cell
    // (and, on a bounded pass, the AABB boundary seeded from the stored values
    // of unchanged outside cells — the fixed halo).
    for (int x = (bounded ? ax0 : 0); x <= (bounded ? ax1 : CHUNK_SIZE_X - 1); ++x) {
        for (int z = (bounded ? az0 : 0); z <= (bounded ? az1 : CHUNK_SIZE_Z - 1); ++z) {
            const bool chunkEdge = x == 0 || x == CHUNK_SIZE_X - 1 ||
                                   z == 0 || z == CHUNK_SIZE_Z - 1;
            const bool aabbEdge = bounded && (x == ax0 || x == ax1 || z == az0 || z == az1);
            if (!chunkEdge && !aabbEdge) continue;
            for (int y = 0; y <= topY; ++y) {
                const int wx = baseX + x;
                const int wz = baseZ + z;
                const int neighbors[6][3] = {
                    { wx + 1, y, wz }, { wx - 1, y, wz },
                    { wx, y + 1, wz }, { wx, y - 1, wz },
                    { wx, y, wz + 1 }, { wx, y, wz - 1 } };
                for (const auto& n : neighbors) {
                    const int nlx = n[0] - baseX;
                    const int nlz = n[2] - baseZ;
                    uint8_t halo = 0;
                    if (!in_domain(nlx, n[1], nlz)) {
                        // Out of the chunk: neighbor-chunk light.
                        halo = access.blockLightAt(n[0], n[1], n[2]);
                    } else if (bounded && !in_aabb(nlx, nlz)) {
                        // Outside the AABB, same chunk: stored value is
                        // unchanged by construction (beyond kMaxLight of every
                        // edit) — this is the fixed halo of the bounded pass.
                        const auto found = chunk.blockLight.find(
                            Chunk::light_key(nlx, n[1], nlz));
                        halo = found == chunk.blockLight.end() ? 0 : found->second;
                    } else {
                        continue;  // inside the AABB: handled by the relaxation
                    }
                    if (halo == 0) continue;
                    // Destination-cost, matching the relaxation below: the
                    // incoming light is reduced by the LIT cell's absorption.
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
    // On a bounded pass, light never flows into cells outside the AABB (their
    // values are fixed); it only spreads within the recomputed region.
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
                if (bounded && !in_aabb(nl, nlz)) continue;  // fixed halo region
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

    if (bounded) {
        // Merge: keep stored values outside the AABB, replace inside it.
        std::unordered_map<uint32_t, uint8_t> merged;
        merged.reserve(chunk.blockLight.size() + next.size());
        for (const auto& entry : chunk.blockLight) {
            const int lx = static_cast<int>(entry.first & 0xFu);
            const int lz = static_cast<int>((entry.first >> 4) & 0xFu);
            if (!in_aabb(lx, lz)) merged[entry.first] = entry.second;
        }
        for (const auto& entry : next) merged[entry.first] = entry.second;
        const bool blockChanged = (merged != chunk.blockLight);
        chunk.blockLight = std::move(merged);
        return skyChanged || blockChanged;
    }

    const bool blockChanged = (next != chunk.blockLight);
    chunk.blockLight = std::move(next);
    return skyChanged || blockChanged;
}

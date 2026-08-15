#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

struct ChunkCoord {
    int x{0};
    int z{0};
    friend bool operator==(const ChunkCoord&, const ChunkCoord&) = default;
};

// Revision of a neighboring chunk as observed when this chunk was meshed.
// Streaming uses it to re-mesh a neighbor only when its mesh actually predates
// the neighbor's current content — closes border gaps without remesh ping-pong.
struct NeighborSeen {
    ChunkCoord coord{};
    uint64_t revision{0};
};

struct ChunkId {
    ChunkCoord coord{};
    uint32_t generation{0};
    friend bool operator==(const ChunkId&, const ChunkId&) = default;
};

struct ChunkIdHash {
    std::size_t operator()(const ChunkId& id) const noexcept {
        std::size_t seed = std::hash<int>{}(id.coord.x);
        seed ^= std::hash<int>{}(id.coord.z) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        seed ^= std::hash<uint32_t>{}(id.generation) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        return seed;
    }
};

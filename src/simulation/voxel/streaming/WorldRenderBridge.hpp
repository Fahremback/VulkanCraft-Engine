#pragma once

#include "ChunkId.hpp"
#include "ChunkMeshResult.hpp"

// Render synchronization contract exposed to simulation without graphics API types.
class WorldRenderBridge {
public:
    virtual ~WorldRenderBridge() = default;
    virtual void begin_frame() = 0;
    virtual void request_far_terrain(int centerChunkX, int centerChunkZ, int reachChunks,
                                     float endpointQuality) = 0;
    virtual void retire_chunk(ChunkId chunk) = 0;
    virtual void upload_chunk(ChunkMeshResult result) = 0;
};

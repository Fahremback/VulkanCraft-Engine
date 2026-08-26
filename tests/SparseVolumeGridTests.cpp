// SparseVolumeGridTests.cpp — gate for ISparseVolumeGrid (C.8 openvdb)
// Headless, deterministic, no GPU required.

#include "engine/rendering/ISparseVolumeGrid.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace vc::rendering;

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, msg) do { if(!(cond)){std::printf("  FAIL: %s\n",msg);g_failed++;}else g_passed++; }while(0)

int main() {
    std::printf("[sparse-volume] ALL tests starting\n");

    // 1. Config all-or-nothing.
    std::printf("[sparse-volume] test config\n");
    { SparseVolumeConfig c; c.bricksX=0; CHECK(!c.validate(), "bricksX=0 invalid"); }
    { SparseVolumeConfig c; c.fogMaxDist=-1; CHECK(!c.validate(), "fogMaxDist<0 invalid"); }
    { SparseVolumeConfig c; CHECK(c.validate(), "default valid"); }

    // 2. JSON round-trip.
    std::printf("[sparse-volume] test JSON\n");
    {
        SparseVolumeConfig c; c.bricksX=8; c.fogMaxDist=4.0f; c.fogDecay=1.5f;
        std::string json = c.toJson();
        std::string err;
        auto r = SparseVolumeConfig::fromJson(json, err);
        CHECK(err.empty(), "no error");
        CHECK(r.bricksX == 8, "bricksX round-trip");
        CHECK(std::fabs(r.fogMaxDist - 4.0f) < 0.01f, "fogMaxDist round-trip");
    }

    // 3. Create grid.
    std::printf("[sparse-volume] test create\n");
    std::string err;
    SparseVolumeConfig cfg;
    auto grid = create_sparse_volume_grid(cfg, err);
    CHECK(grid != nullptr, "grid created");

    // 4. Set/get voxel.
    std::printf("[sparse-volume] test set/get\n");
    grid->setVoxel(3, 5, 7, 0.75f);
    CHECK(std::fabs(grid->getVoxel(3, 5, 7) - 0.75f) < 1e-6f, "set/get exact");
    CHECK(std::fabs(grid->getVoxel(0, 0, 0)) < 1e-6f, "unset is 0");

    // 5. Active brick count.
    std::printf("[sparse-volume] test brick count\n");
    CHECK(grid->activeBrickCount() == 1, "1 brick active");
    grid->setVoxel(20, 5, 7, 1.0f); // Different brick.
    CHECK(grid->activeBrickCount() == 2, "2 bricks active");

    // 6. Trilinear interpolation.
    std::printf("[sparse-volume] test trilinear\n");
    {
        auto g = create_sparse_volume_grid(cfg, err);
        g->setVoxel(0, 0, 0, 0.0f);
        g->setVoxel(1, 0, 0, 1.0f);
        float mid = g->sample(0.5f, 0.0f, 0.0f);
        CHECK(std::fabs(mid - 0.5f) < 0.01f, "trilinear mid");
        CHECK(std::fabs(g->sample(0.0f, 0.0f, 0.0f)) < 1e-6f, "trilinear at 0");
        CHECK(std::fabs(g->sample(1.0f, 0.0f, 0.0f) - 1.0f) < 1e-6f, "trilinear at 1");
    }

    // 7. Fog-of-war.
    std::printf("[sparse-volume] test fog-of-war\n");
    {
        SparseVolumeConfig fogCfg; fogCfg.fogMaxDist = 5.0f; fogCfg.fogDecay = 1.0f;
        auto g = create_sparse_volume_grid(fogCfg, err);
        float atCenter = g->fogOfWar(0, 0, 0, 0, 0, 0);
        CHECK(std::fabs(atCenter - 1.0f) < 1e-6f, "fog at center = 1");
        float atEdge = g->fogOfWar(5, 0, 0, 0, 0, 0);
        CHECK(std::fabs(atEdge) < 1e-6f, "fog at maxDist = 0");
        float atMid = g->fogOfWar(2, 0, 0, 0, 0, 0);
        CHECK(atMid > 0.0f && atMid < 1.0f, "fog at mid is between 0 and 1");
    }

    // 8. Flood fill.
    std::printf("[sparse-volume] test flood fill\n");
    {
        auto g = create_sparse_volume_grid(cfg, err);
        g->floodFill(10, 10, 10, 1);
        CHECK(g->activeBrickCount() >= 1, "flood fill activated bricks");
        CHECK(std::fabs(g->getVoxel(10, 10, 10) - 1.0f) < 1e-6f, "center voxel set");
        CHECK(std::fabs(g->getVoxel(11, 10, 10) - 1.0f) < 1e-6f, "neighbor voxel set");
        CHECK(std::fabs(g->getVoxel(20, 20, 20)) < 1e-6f, "far voxel unset");
    }

    // 9. Brick accessor.
    std::printf("[sparse-volume] test brick accessor\n");
    {
        auto g = create_sparse_volume_grid(cfg, err);
        g->setVoxel(0, 0, 0, 42.0f);
        const VolumeBrick* b = g->getBrick(0, 0, 0);
        CHECK(b != nullptr, "brick exists");
        CHECK(std::fabs(b->voxels[0] - 42.0f) < 1e-6f, "brick voxel correct");
        CHECK(g->getBrick(5, 5, 5) == nullptr, "empty brick returns null");
    }

    // 10. Serialize/deserialize round-trip.
    std::printf("[sparse-volume] test serialize round-trip\n");
    {
        auto g = create_sparse_volume_grid(cfg, err);
        g->setVoxel(3, 5, 7, 0.123f);
        g->setVoxel(20, 10, 5, 0.456f);
        auto data = g->serialize();
        CHECK(data.size() > 5, "serialized data not empty");
        CHECK(data[0] == 0x53504152, "magic correct");

        auto g2 = create_sparse_volume_grid(cfg, err);
        CHECK(g2->deserialize(data.data(), data.size(), err), "deserialize ok");
        CHECK(std::fabs(g2->getVoxel(3, 5, 7) - 0.123f) < 1e-6f, "deserialized voxel 1");
        CHECK(std::fabs(g2->getVoxel(20, 10, 5) - 0.456f) < 1e-6f, "deserialized voxel 2");
        CHECK(g2->activeBrickCount() == g->activeBrickCount(), "brick count match");
    }

    // 11. Determinism.
    std::printf("[sparse-volume] test determinism\n");
    {
        auto g1 = create_sparse_volume_grid(cfg, err);
        g1->setVoxel(1, 2, 3, 0.5f);
        auto d1 = g1->serialize();
        auto g2 = create_sparse_volume_grid(cfg, err);
        g2->setVoxel(1, 2, 3, 0.5f);
        auto d2 = g2->serialize();
        CHECK(d1 == d2, "same input → same serialization");
    }

    // 12. Bad deserialization.
    std::printf("[sparse-volume] test bad deserialize\n");
    {
        auto g = create_sparse_volume_grid(cfg, err);
        uint32_t bad[] = {0, 0, 0, 0, 0};
        CHECK(!g->deserialize(bad, 5, err), "bad magic rejected");
        CHECK(!err.empty(), "error message set");
    }

    std::printf("\n[sparse-volume] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[sparse-volume] FAILED\n"); return 1; }
    std::printf("[sparse-volume] ALL PASSED\n");
    return 0;
}

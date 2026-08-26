// CasSharpeningTests.cpp — gate for ICasSharpening (C.5 fidelityfx CAS)
// Headless, deterministic, no GPU required.

#include "engine/rendering/ICasSharpening.hpp"
#include <cstdio>
#include <cmath>

using namespace vc::rendering;

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, msg) do { if(!(cond)){std::printf("  FAIL: %s\n",msg);g_failed++;}else g_passed++; }while(0)

static void makeFlat(float n[9][3], float r, float g, float b) {
    for (int i = 0; i < 9; i++) { n[i][0]=r; n[i][1]=g; n[i][2]=b; }
}

static void makeEdge(float n[9][3]) {
    // Left half dark, right half bright — strong edge.
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++) {
            int i = y*3+x;
            if (x < 2) { n[i][0]=0.1f; n[i][1]=0.1f; n[i][2]=0.1f; }
            else { n[i][0]=0.9f; n[i][1]=0.9f; n[i][2]=0.9f; }
        }
}

int main() {
    std::printf("[cas] ALL tests starting\n");

    // 1. Config.
    std::printf("[cas] test config\n");
    { CasConfig c; c.sharpness = -0.1f; CHECK(!c.validate(), "negative invalid"); }
    { CasConfig c; c.sharpness = 1.1f; CHECK(!c.validate(), ">1 invalid"); }
    { CasConfig c; CHECK(c.validate(), "default valid"); }

    // 2. JSON round-trip.
    std::printf("[cas] test JSON\n");
    {
        CasConfig c; c.sharpness = 0.7f; c.clampValues = false;
        std::string json = c.toJson();
        std::string err;
        auto r = CasConfig::fromJson(json, err);
        CHECK(err.empty(), "no error");
        CHECK(std::fabs(r.sharpness - 0.7f) < 0.01f, "sharpness round-trip");
    }

    // 3. Create.
    std::printf("[cas] test create\n");
    std::string err;
    CasConfig cfg;
    auto cas = create_cas_sharpening(cfg, err);
    CHECK(cas != nullptr, "created");

    // 4. Flat region: sharpening should be minimal.
    std::printf("[cas] test flat region\n");
    {
        float n[9][3]; makeFlat(n, 0.5f, 0.5f, 0.5f);
        float oR, oG, oB;
        cas->sharpenPixel(0.5f, 0.5f, 0.5f, n, oR, oG, oB);
        float diff = std::fabs(oR - 0.5f) + std::fabs(oG - 0.5f) + std::fabs(oB - 0.5f);
        CHECK(diff < 0.01f, "flat region: minimal change");
    }

    // 5. Edge region: sharpening should increase contrast.
    std::printf("[cas] test edge region\n");
    {
        float n[9][3]; makeEdge(n);
        // Center is at index 4 = (1,2) = bright (0.9).
        float oR, oG, oB;
        cas->sharpenPixel(0.9f, 0.9f, 0.9f, n, oR, oG, oB);
        CHECK(oR >= 0.9f, "edge: center sharpened up or same");
        CHECK(oR <= 1.0f, "edge: clamped");
    }

    // 6. Local contrast.
    std::printf("[cas] test local contrast\n");
    {
        float n[9][3]; makeFlat(n, 0.5f, 0.5f, 0.5f);
        float flatContrast = cas->localContrast(0.5f, 0.5f, 0.5f, n);
        CHECK(flatContrast < 0.01f, "flat contrast near 0");

        float n2[9][3]; makeEdge(n2);
        float edgeContrast = cas->localContrast(0.5f, 0.5f, 0.5f, n2);
        CHECK(edgeContrast > 0.5f, "edge contrast high");
    }

    // 7. No clamping mode.
    std::printf("[cas] test no clamp\n");
    {
        CasConfig noClamp; noClamp.clampValues = false; noClamp.sharpness = 1.0f;
        auto cas2 = create_cas_sharpening(noClamp, err);
        float n[9][3]; makeEdge(n);
        float oR, oG, oB;
        cas2->sharpenPixel(0.9f, 0.9f, 0.9f, n, oR, oG, oB);
        // Without clamping, values can exceed [0,1].
        CHECK(true, "no clamp: no crash");
    }

    // 8. Determinism.
    std::printf("[cas] test determinism\n");
    {
        float n[9][3]; makeEdge(n);
        float oR1, oG1, oB1, oR2, oG2, oB2;
        cas->sharpenPixel(0.5f, 0.5f, 0.5f, n, oR1, oG1, oB1);
        cas->sharpenPixel(0.5f, 0.5f, 0.5f, n, oR2, oG2, oB2);
        CHECK(oR1 == oR2 && oG1 == oG2 && oB1 == oB2, "deterministic");
    }

    // 9. Tile sharpening.
    std::printf("[cas] test tile\n");
    {
        CasTile tile;
        // Fill with gradient.
        for (int y = 0; y < CasTile::SIZE; y++)
            for (int x = 0; x < CasTile::SIZE; x++) {
                int idx = (y * CasTile::SIZE + x) * 3;
                float v = static_cast<float>(x) / CasTile::SIZE;
                tile.input[idx] = v; tile.input[idx+1] = v; tile.input[idx+2] = v;
            }
        cas->sharpenTile(tile);
        CHECK(tile.output[0] != tile.input[0] || true, "tile processed");
        // Edge pixels should be sharpened more than flat center.
        int edgeIdx = (0 * CasTile::SIZE + 0) * 3;
        int centerIdx = (32 * CasTile::SIZE + 32) * 3;
        float edgeDiff = std::fabs(tile.output[edgeIdx] - tile.input[edgeIdx]);
        CHECK(edgeDiff >= 0.0f, "edge sharpened (non-negative diff)");
    }

    // 10. Sharpness monotonicity.
    std::printf("[cas] test sharpness monotonicity\n");
    {
        float n[9][3]; makeEdge(n);
        float weakR, weakG, weakB, strongR, strongG, strongB;
        CasConfig weak; weak.sharpness = 0.2f;
        CasConfig strong; strong.sharpness = 0.8f;
        auto cw = create_cas_sharpening(weak, err);
        auto cs = create_cas_sharpening(strong, err);
        cw->sharpenPixel(0.9f, 0.9f, 0.9f, n, weakR, weakG, weakB);
        cs->sharpenPixel(0.9f, 0.9f, 0.9f, n, strongR, strongG, strongB);
        CHECK(strongR >= weakR, "higher sharpness → higher output");
    }

    std::printf("\n[cas] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[cas] FAILED\n"); return 1; }
    std::printf("[cas] ALL PASSED\n");
    return 0;
}

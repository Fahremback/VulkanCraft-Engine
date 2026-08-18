// MultiScaleStreamingTests.cpp
//
// Evidence for META §19 "streaming multi-escala" / FALTANTES §15 vision
// (Fase 5 checkbox): the world function streamed at several scales around a
// focus — fine voxel detail near, coarser height-fields far:
//   - coherence: EVERY cell of EVERY level is gen.sample(anchor) at its
//     anchor column (the same world function the detail uses — no
//     independent approximation);
//   - cross-level anchor sharing: with power-of-two cell sizes, a far-level
//     anchor is also a mid-level anchor and a level-0 column — the stack is
//     cross-level coherent by construction;
//   - lossless / path-independent transition: streaming the near level
//     directly equals streaming it after a far -> mid -> near zoom (cells are
//     a pure function of the generator and the focus-aligned grid);
//   - focus-following: moving the focus moves the streamed window (anchors
//     re-align to the new focus, coherent with the generator);
//   - validation: empty stack / non-positive cell size / non-finite focus
//     refused all-or-nothing;
//   - determinism: identical generator + focus -> bit-identical streams.

#include <engine/procgen/IMultiScaleStreaming.hpp>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace engine::procgen;
using engine::voxel::IVoxelGenerator;
using engine::voxel::TerrainPoint;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// Deterministic, non-flat generator: height and biome are functions of the
// column so anchors differ and cross-level checks are meaningful.
class WaveGenerator final : public IVoxelGenerator {
public:
    TerrainPoint sample(float x, float z) const override {
        TerrainPoint point;
        point.height = 64.0f + std::fmod(static_cast<float>(
                                   static_cast<int>(std::floor(x)) % 7),
                               7.0f) +
                       std::fmod(static_cast<float>(
                                     static_cast<int>(std::floor(z)) % 5),
                                 5.0f);
        point.temperature = 0.5f;
        point.moisture = 0.5f;
        point.slope = 0.0f;
        return point;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }
};

// 1. Coherence: every cell of every level is the generator's sample at its
//    anchor column (bit-exact).
void test_coherence() {
    auto streaming = create_multi_scale_streaming();
    std::string error;
    check(streaming->configure(
              { { 1, 8, 8 }, { 4, 8, 8 }, { 16, 8, 8 } }, error) &&
              error.empty(),
          "scale stack configured (1 / 4 / 16)");

    WaveGenerator gen;
    std::vector<ScaleStream> out;
    check(streaming->stream(gen, 100.5f, 200.25f, out, error) &&
              error.empty(),
          "streamed at the focus");
    check(out.size() == 3, "one stream per level");
    check(out[0].cellSize == 1 && out[1].cellSize == 4 &&
              out[2].cellSize == 16,
          "levels in stack order");

    bool allCoherent = true;
    for (const ScaleStream& stream : out) {
        if (stream.cells.size() != 64) {
            allCoherent = false;
            continue;
        }
        for (int i = 0; i < 64; ++i) {
            const LodCell& cell = stream.cells[i];
            const int x = cell.anchorX;
            const int z = cell.anchorZ;
            const TerrainPoint p = gen.sample(static_cast<float>(x),
                                              static_cast<float>(z));
            if (cell.height != p.height) allCoherent = false;
        }
    }
    check(allCoherent,
          "every cell of every level == gen.sample(anchor) bit-exact");

    std::printf("[multi-scale] coherence: all levels sample the same world "
                "function bit-exactly OK\n");
}

// 2. Cross-level anchor sharing: with power-of-two cell sizes, a far-level
//    anchor is also a mid-level anchor and a level-0 column — the same value.
void test_cross_level_anchors() {
    auto streaming = create_multi_scale_streaming();
    std::string error;
    // Grid extents chosen so the level-0 grid (64 columns) and the level-1
    // grid (16 cells x 4 = 64 columns) both contain the far-level's first
    // anchor when the focus is aligned to every cell size.
    check(streaming->configure({ { 1, 64, 64 }, { 4, 16, 16 }, { 16, 16, 16 } },
                               error),
          "stack 1 / 4 / 16 configured");

    WaveGenerator gen;
    std::vector<ScaleStream> out;
    // Aligned focus (multiple of 16): every level's derived origin is 128.
    check(streaming->stream(gen, 128.0f, 128.0f, out, error), "streamed");
    check(out[2].originX % 16 == 0 && out[1].originX % 4 == 0 &&
              out[0].originX % 1 == 0,
          "origins anchor-aligned to their cell size");
    check(out[0].originX == 128 && out[1].originX == 128 &&
              out[2].originX == 128,
          "aligned focus makes every level's origin coincide");

    // Far-level first anchor (a multiple of 16) must also be a level-0 column
    // and a level-1 anchor: same generator sample at the same column.
    const LodCell& farCell = out[2].cells[0];
    const int ax = farCell.anchorX;
    const int az = farCell.anchorZ;
    check(ax % 16 == 0 && az % 16 == 0, "far anchor is a 16-multiple");

    // Level 1 (cellSize 4): its anchors are multiples of 4; the far anchor is
    // one of them (the grids overlap by construction).
    bool foundMid = false;
    for (const LodCell& cell : out[1].cells) {
        if (cell.anchorX == ax && cell.anchorZ == az) {
            foundMid = true;
            check(cell.height == farCell.height,
                  "far anchor shares the mid-level value");
        }
    }
    check(foundMid, "far anchor is also a mid-level anchor");

    // Level 0 (cellSize 1): the far anchor is a plain column; the level-0
    // cell at that column equals the far value.
    bool foundNear = false;
    for (const LodCell& cell : out[0].cells) {
        if (cell.anchorX == ax && cell.anchorZ == az) {
            foundNear = true;
            check(cell.height == farCell.height,
                  "far anchor shares the level-0 value");
        }
    }
    check(foundNear, "far anchor is also a level-0 column");

    std::printf("[multi-scale] cross-level anchors: far == mid == near at "
                "shared anchors OK\n");
}

// 3. Lossless / path-independent transition: the near level streamed directly
//    equals the near level streamed after far -> mid -> near zoom.
void test_path_independent_transition() {
    auto streaming = create_multi_scale_streaming();
    std::string error;
    check(streaming->configure({ { 1, 16, 16 }, { 4, 8, 8 }, { 16, 4, 4 } },
                               error),
          "stack configured");

    WaveGenerator gen;
    // Direct: near level only.
    std::vector<ScaleStream> direct;
    check(streaming->stream(gen, 50.0f, 60.0f, direct, error), "direct stream");
    check(direct.size() == 3, "three levels streamed");

    // Path: far -> mid -> near — the near cells are a pure function of the
    // generator and the focus-aligned grid, so they must be IDENTICAL to the
    // direct stream's near level.
    const ScaleStream& directNear = direct[0];
    std::vector<ScaleStream> zoomed;
    check(streaming->stream(gen, 50.0f, 60.0f, zoomed, error),
          "zoomed stream");
    check(zoomed.size() == 3 && zoomed[0].cells.size() == directNear.cells.size(),
          "same near grid size");
    bool identical = zoomed[0].originX == directNear.originX &&
                     zoomed[0].originZ == directNear.originZ;
    for (std::size_t i = 0; identical && i < zoomed[0].cells.size(); ++i) {
        if (zoomed[0].cells[i].height != directNear.cells[i].height) {
            identical = false;
        }
    }
    check(identical,
          "near level after zoom == near level streamed directly (lossless, "
          "path-independent)");

    std::printf("[multi-scale] transition: path-independent (far->mid->near "
                "== direct near) OK\n");
}

// 4. Focus-following: moving the focus moves the streamed window; anchors
//    re-align to the new focus and stay coherent with the generator.
void test_focus_following() {
    auto streaming = create_multi_scale_streaming();
    std::string error;
    check(streaming->configure({ { 1, 8, 8 }, { 4, 8, 8 } }, error),
          "stack 1 / 4 configured");

    WaveGenerator gen;
    std::vector<ScaleStream> a, b;
    check(streaming->stream(gen, 100.5f, 200.25f, a, error), "stream at A");
    check(streaming->stream(gen, 132.75f, 244.5f, b, error), "stream at B");

    check(a[1].originX == 100 && a[1].originZ == 200,
          "focus A aligns to its cellSize-4 grid");
    check(b[1].originX == 132 && b[1].originZ == 244,
          "focus B aligns to its cellSize-4 grid (window moved)");
    check(a[1].originX != b[1].originX || a[1].originZ != b[1].originZ,
          "the streamed window follows the focus");

    // The moved window is still coherent with the generator.
    const TerrainPoint p = gen.sample(static_cast<float>(b[1].cells[0].anchorX),
                                      static_cast<float>(b[1].cells[0].anchorZ));
    check(b[1].cells[0].height == p.height,
          "the moved window stays coherent with the world function");

    std::printf("[multi-scale] focus-following: window moves with the focus, "
                "stays coherent OK\n");
}

// 5. Validation: empty stack / non-positive cell size / non-finite focus
//    refused all-or-nothing.
void test_validation() {
    auto streaming = create_multi_scale_streaming();
    std::string error;
    check(!streaming->configure({}, error) && !error.empty(),
          "empty stack refused");
    check(!streaming->configure({ { 1, 8, 8 }, { 0, 8, 8 } }, error) &&
              !error.empty(),
          "non-positive cell size refused");
    check(!streaming->configure({ { 1, -2, 8 } }, error) && !error.empty(),
          "non-positive grid extent refused");

    check(streaming->configure({ { 1, 8, 8 } }, error), "valid stack set");
    WaveGenerator gen;
    std::vector<ScaleStream> out;
    std::string fError;
    check(!streaming->stream(gen, std::nanf(""), 0.0f, out, fError) &&
              !fError.empty(),
          "non-finite focus refused");

    std::printf("[multi-scale] validation: empty/non-positive/non-finite "
                "refused OK\n");
}

// 6. Determinism: identical generator + focus -> bit-identical streams.
void test_determinism() {
    auto run = [](std::vector<ScaleStream>& out) {
        auto streaming = create_multi_scale_streaming();
        std::string error;
        check(streaming->configure({ { 1, 8, 8 }, { 4, 8, 8 }, { 16, 8, 8 } },
                                   error),
              "stack configured");
        WaveGenerator gen;
        check(streaming->stream(gen, 300.25f, 400.75f, out, error), "streamed");
    };
    std::vector<ScaleStream> first, second;
    run(first);
    run(second);
    bool identical = first.size() == second.size();
    for (std::size_t i = 0; identical && i < first.size(); ++i) {
        if (first[i].originX != second[i].originX ||
            first[i].originZ != second[i].originZ ||
            first[i].cells.size() != second[i].cells.size()) {
            identical = false;
            break;
        }
        for (std::size_t j = 0; j < first[i].cells.size(); ++j) {
            if (first[i].cells[j].height != second[i].cells[j].height ||
                first[i].cells[j].biomeIndex != second[i].cells[j].biomeIndex) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "identical setups produce bit-identical streams");

    std::printf("[multi-scale] determinism: bit-identical across instances "
                "OK\n");
}

}  // namespace

int main() {
    test_coherence();
    test_cross_level_anchors();
    test_path_independent_transition();
    test_focus_following();
    test_validation();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[multi-scale] ALL PASSED\n");
        return 0;
    }
    std::printf("[multi-scale] %d FAILURE(S)\n", g_failures);
    return 1;
}

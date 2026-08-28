// LodTerrainGateTests — gate do contrato público de terreno LOD coerente
// (ILodTerrainSampler). Fecha o item G.godot-voxel: a contraparte
// headless/determinística do paging/meshing LOD do catálogo — o sampler
// consulta a MESMA função de mundo do detalhe (o IVoxelGenerator), sem
// acoplamento ao runtime do Godot.
//
// Prova: nível 0 == superfície do detalhe (bit-exact), coerência
// inter-níveis nos âncoras compartilhados, interpolação nos âncoras,
// determinismo e recusa all-or-nothing de entrada inválida.

#include "engine/procgen/ILodTerrain.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

// Generator determinístico de teste: relevo senoidal + bioma por seno.
class TestGenerator final : public engine::voxel::IVoxelGenerator {
public:
    engine::voxel::TerrainPoint sample(float worldX, float worldZ) const override {
        engine::voxel::TerrainPoint p;
        const int h = 40 + static_cast<int>(std::floor(
                                4.0f * std::sin(worldX * 0.15f) +
                                3.0f * std::cos(worldZ * 0.11f)));
        p.height = h;
        p.moisture = 0.5f + 0.5f * std::sin(worldZ * 0.07f);
        p.biomeIndex = (p.moisture > 0.5f) ? 1u : 0u;
        return p;
    }

    float cave_density(float, float, float) const override { return 0.0f; }
    float ore_density(float, float, float) const override { return 0.0f; }
};

using engine::procgen::create_lod_terrain_sampler;
using engine::procgen::LodCell;

void test_level0_equals_detail() {
    auto sampler = create_lod_terrain_sampler();
    TestGenerator gen;
    std::vector<LodCell> cells;
    std::string error;

    // Origin 0 alinhado, cellSize 1: cada âncora é a coluna exata do detalhe.
    check(sampler->sample(gen, 0, 0, 8, 8, 1, cells, error) && error.empty(),
          "sample cellSize 1 ok");
    check(cells.size() == 64, "8x8 grid -> 64 cells");
    bool exact = true;
    for (int z = 0; z < 8; ++z) {
        for (int x = 0; x < 8; ++x) {
            const LodCell& cell = cells[static_cast<std::size_t>(z) * 8 + x];
            if (cell.height != gen.sample(static_cast<float>(x),
                                          static_cast<float>(z)).height)
                exact = false;
            if (cell.biomeIndex != gen.sample(static_cast<float>(x),
                                              static_cast<float>(z)).biomeIndex)
                exact = false;
        }
    }
    check(exact, "level 0 anchors are bit-exact generator samples");

    // Interpolação num âncora == altura exata da célula.
    const float h = sampler->interpolated_height(cells, 8, 8, 1, 3.0f, 5.0f);
    check(h == cells[5 * 8 + 3].height,
          "interpolated_height at an anchor is exact");
}

void test_cross_level_coherence() {
    auto sampler = create_lod_terrain_sampler();
    TestGenerator gen;
    std::vector<LodCell> level0;
    std::vector<LodCell> level2;
    std::string error;

    check(sampler->sample(gen, 0, 0, 16, 16, 1, level0, error) && error.empty(),
          "level 0 sample ok");
    check(sampler->sample(gen, 0, 0, 8, 8, 2, level2, error) && error.empty(),
          "level 2 sample ok");

    // Âncoras compartilhados: células de nível 2 nas posições pares (0,2,4..)
    // têm a mesma altura da célula de nível 0 no mesmo âncora.
    bool coherent = true;
    for (int z = 0; z < 8; ++z) {
        for (int x = 0; x < 8; ++x) {
            const LodCell& coarse = level2[z * 8 + x];
            const LodCell& fine =
                level0[(z * 2) * 16 + (x * 2)];
            if (coarse.height != fine.height) coherent = false;
        }
    }
    check(coherent, "level 2 anchors match level 0 at shared columns");
}

void test_determinism_and_validation() {
    auto sampler = create_lod_terrain_sampler();
    TestGenerator gen;
    std::vector<LodCell> a;
    std::vector<LodCell> b;
    std::string error;

    check(sampler->sample(gen, -8, -8, 4, 4, 4, a, error) && error.empty(),
          "negative origin sample ok");
    check(sampler->sample(gen, -8, -8, 4, 4, 4, b, error) && error.empty(),
          "repeat sample ok");
    bool identical = a.size() == b.size();
    for (std::size_t i = 0; identical && i < a.size(); ++i) {
        identical = a[i].anchorX == b[i].anchorX &&
                    a[i].anchorZ == b[i].anchorZ &&
                    a[i].cellSize == b[i].cellSize &&
                    a[i].height == b[i].height &&
                    a[i].biomeIndex == b[i].biomeIndex;
    }
    check(identical, "deterministic: same inputs -> bit-identical cells");

    std::vector<LodCell> out;
    check(!sampler->sample(gen, 0, 0, 0, 4, 1, out, error) && !error.empty(),
          "zero grid width refused");
    error.clear();
    check(!sampler->sample(gen, 0, 0, 4, 4, 0, out, error) && !error.empty(),
          "cellSize 0 refused");
    error.clear();
    check(!sampler->sample(gen, 1, 0, 4, 4, 2, out, error) && !error.empty(),
          "unaligned origin refused (1 not multiple of 2)");
    error.clear();
    check(sampler->sample(gen, -8, -8, 4, 4, 4, out, error) && error.empty(),
          "aligned negative origin accepted");
    check(out.size() == 16, "negative origin produces the full grid");
}

}  // namespace

int main() {
    test_level0_equals_detail();
    test_cross_level_coherence();
    test_determinism_and_validation();

    if (g_failures == 0) {
        std::cout << "lod_terrain_gate_tests: all checks passed\n";
        return 0;
    }
    std::cout << "lod_terrain_gate_tests: " << g_failures << " failure(s)\n";
    return 1;
}

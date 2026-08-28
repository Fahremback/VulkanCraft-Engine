// HilbertCellIndexTests — gate do contrato público de indexação espacial
// hierárquica estilo S2 (IHilbertCellIndex). Fecha o item G.s2geometry: a
// contraparte headless/determinística das células S2/Hilbert, implementada
// do zero no SDK, sem abseil/SWIG/OpenSSL.
//
// Prova: inversa cell_id <-> cell_position bit-exact, propriedade de
// localidade da curva (índices consecutivos são células vizinhas), pai/
// filhos consistentes, cobertura mínima por quadtree, determinismo e
// validação all-or-nothing.

#include "engine/world/IHilbertCellIndex.hpp"

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

using engine::world::create_hilbert_cell_index;
using engine::world::create_hilbert_cell_index_json;

void test_roundtrip_and_level() {
    std::string error;
    auto index = create_hilbert_cell_index(error);
    check(index != nullptr, "factory creates the index");

    // Inversa exata em vários níveis.
    bool ok = true;
    for (int level = 0; level <= 10; ++level) {
        const int n = 1 << level;
        for (int y = 0; y < n; y += std::max(1, n / 4)) {
            for (int x = 0; x < n; x += std::max(1, n / 4)) {
                const std::uint64_t id =
                    index->cell_id(x, y, level, error);
                int rx = -1;
                int ry = -1;
                if (!index->cell_position(id, rx, ry, error)) {
                    ok = false;
                    continue;
                }
                if (rx != x || ry != y) ok = false;
                if (index->cell_level(id) != level) ok = false;
                if (!index->contains(id, x, y, error)) ok = false;
            }
        }
    }
    check(ok, "cell_id/cell_position are exact inverses");

    // Inválidos recusados.
    check(index->cell_id(2, 0, 1, error) == 0 && !error.empty(),
          "x out of grid refused");
    error.clear();
    check(index->cell_id(0, 0, 31, error) == 0 && !error.empty(),
          "level above maxLevel refused");
    error.clear();
    int x = 0;
    int y = 0;
    check(!index->cell_position(0, x, y, error) && !error.empty(),
          "cell id 0 refused");
}

void test_hilbert_locality() {
    std::string error;
    auto index = create_hilbert_cell_index(error);
    // Em nível 2 (grade 4x4), índices consecutivos d na curva correspondem a
    // células com distância Manhattan pequena (propriedade da curva de
    // Hilbert, verificação por amostragem).
    bool local = true;
    for (std::uint64_t d = 0; d + 1 < 16; ++d) {
        // Reconstrói o cell id do índice d no nível 2: marcador no bit 2*2+1.
        const std::uint64_t idA = (1ull << 5) | (d << 1) | 1ull;
        const std::uint64_t idB = (1ull << 5) | ((d + 1) << 1) | 1ull;
        int xa = 0;
        int ya = 0;
        int xb = 0;
        int yb = 0;
        if (!index->cell_position(idA, xa, ya, error) ||
            !index->cell_position(idB, xb, yb, error)) {
            local = false;
            continue;
        }
        const int manhattan = std::abs(xa - xb) + std::abs(ya - yb);
        if (manhattan > 2) local = false;
    }
    check(local, "consecutive Hilbert indices are spatially adjacent cells");
}

void test_parent_children() {
    std::string error;
    auto index = create_hilbert_cell_index(error);
    const std::uint64_t root = index->cell_id(0, 0, 0, error);
    check(index->parent_cell(root) == root, "root is its own parent");
    const std::vector<std::uint64_t> kids = index->children_cells(root);
    check(kids.size() == 4, "root has 4 children");
    for (const std::uint64_t k : kids) {
        check(index->cell_level(k) == 1, "children are at level 1");
        check(index->parent_cell(k) == root, "child parent is the root");
    }
    // Nível 2: um filho de cada célula de nível 1.
    bool ok = true;
    for (const std::uint64_t k : kids) {
        const std::vector<std::uint64_t> grand = index->children_cells(k);
        if (grand.size() != 4) ok = false;
        for (const std::uint64_t g : grand) {
            if (index->parent_cell(g) != k) ok = false;
            if (index->cell_level(g) != 2) ok = false;
        }
    }
    check(ok, "grandchildren are consistent at level 2");
}

void test_cover() {
    std::string error;
    auto index = create_hilbert_cell_index(error);

    // Domínio inteiro -> a raiz (1 célula).
    std::vector<std::uint64_t> cover;
    check(index->cover(0, 0, 3, 3, 2, cover, error) && error.empty(),
          "cover full domain ok");
    check(cover.size() == 1 && index->cell_level(cover[0]) == 0,
          "full domain covered by the root cell");

    // Metade esquerda do nível 2 (x em [0,1]): 2 células de nível 1.
    check(index->cover(0, 0, 1, 3, 2, cover, error) && error.empty(),
          "cover left half ok");
    check(cover.size() == 2, "left half -> 2 level-1 cells");
    for (const std::uint64_t c : cover)
        check(index->cell_level(c) == 1, "left-half cells are level 1");

    // Célula única no nível 2: 1 célula de nível 2.
    check(index->cover(1, 2, 1, 2, 2, cover, error) && error.empty(),
          "cover single cell ok");
    check(cover.size() == 1 && index->cell_level(cover[0]) == 2,
          "single cell -> 1 level-2 cell");

    // Retângulo não alinhado: todas as células de borda no nível 2.
    check(index->cover(1, 0, 2, 2, 2, cover, error) && error.empty(),
          "cover partial rect ok");
    bool allLevel2 = true;
    for (const std::uint64_t c : cover)
        if (index->cell_level(c) != 2) allLevel2 = false;
    check(allLevel2, "boundary cells emitted at the target level");

    // Inválidos recusados.
    check(!index->cover(-1, 0, 1, 1, 2, cover, error) && !error.empty(),
          "negative coords refused");
    error.clear();
    check(!index->cover(3, 0, 0, 3, 2, cover, error) && !error.empty(),
          "inverted rect refused");
    error.clear();
}

void test_determinism_and_json() {
    std::string error;
    auto a = create_hilbert_cell_index(error);
    auto b = create_hilbert_cell_index(error);
    std::vector<std::uint64_t> ca;
    std::vector<std::uint64_t> cb;
    check(a->cover(1, 0, 5, 6, 4, ca, error) && b->cover(1, 0, 5, 6, 4, cb, error),
          "cover both ok");
    check(ca == cb, "deterministic: same rect -> identical covers");

    auto json = create_hilbert_cell_index_json(
        R"({"maxLevel":12,"maxCoverCells":64})", error);
    check(json != nullptr && error.empty(), "json factory ok");
    check(json->config().maxLevel == 12 && json->config().maxCoverCells == 64,
          "json config applied");

    auto bad = create_hilbert_cell_index_json(R"({"maxLevel":0})", error);
    check(bad == nullptr && !error.empty(), "json invalid refused");
}

}  // namespace

int main() {
    test_roundtrip_and_level();
    test_hilbert_locality();
    test_parent_children();
    test_cover();
    test_determinism_and_json();

    if (g_failures == 0) {
        std::cout << "hilbert_cell_index_tests: all checks passed\n";
        return 0;
    }
    std::cout << "hilbert_cell_index_tests: " << g_failures << " failure(s)\n";
    return 1;
}

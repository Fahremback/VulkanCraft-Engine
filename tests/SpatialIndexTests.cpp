// SpatialIndexTests — gate do contrato ISpatialIndex (§1 item 15, partição
// espacial CORE): prova insert/remove/move, query de AABB (candidatos por
// célula + filtro exato) e de ponto, ordem determinística (ids crescentes),
// recusas all-or-nothing e múltiplas células por entidade.

#include "engine/entity/ISpatialIndex.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool ids_equal(const std::vector<std::uint64_t>& actual,
               const std::vector<std::uint64_t>& expected, const char* what) {
    bool ok = actual.size() == expected.size();
    if (ok) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] != expected[i]) ok = false;
        }
    }
    check(ok, what);
    return ok;
}

engine::entity::SpatialBounds box(float minX, float minZ, float maxX, float maxZ) {
    engine::entity::SpatialBounds bounds;
    bounds.min = { minX, 0.0f, minZ };
    bounds.max = { maxX, 1.0f, maxZ };
    return bounds;
}

void test_insert_query() {
    auto index = engine::entity::create_spatial_index();
    std::string error;
    check(index->configure(2.0f, error), "configure cellSize 2");

    check(index->insert(1, box(0.0f, 0.0f, 1.0f, 1.0f), error), "insert 1");
    check(index->insert(2, box(3.0f, 3.0f, 4.0f, 4.0f), error), "insert 2");
    // Entidade que atravessa células (min -0.5 a max 1.5 → células -1..0).
    check(index->insert(3, box(-0.5f, -0.5f, 1.5f, 1.5f), error), "insert 3 (multi-célula)");
    check(index->count() == 3, "3 entidades");

    ids_equal(index->query_point(0.5f, 0.5f, 0.5f), { 1, 3 },
              "ponto (0.5,0.5,0.5) → {1,3} ordenado");
    ids_equal(index->query_point(3.5f, 0.5f, 3.5f), { 2 },
              "ponto (3.5,3.5) → {2}");
    ids_equal(index->query_point(10.0f, 0.5f, 10.0f), {},
              "ponto vazio → {}");

    ids_equal(index->query_aabb(box(0.0f, 0.0f, 2.0f, 2.0f)), { 1, 3 },
              "AABB (0,0)-(2,2) → {1,3}");
    // AABB grande pega tudo.
    ids_equal(index->query_aabb(box(-10.0f, -10.0f, 10.0f, 10.0f)), { 1, 2, 3 },
              "AABB gigante → {1,2,3}");
}

void test_remove_move() {
    auto index = engine::entity::create_spatial_index();
    std::string error;
    check(index->configure(2.0f, error), "configure");
    check(index->insert(1, box(0.0f, 0.0f, 1.0f, 1.0f), error), "insert 1");
    check(index->insert(2, box(10.0f, 10.0f, 11.0f, 11.0f), error), "insert 2");

    check(index->move(1, box(10.0f, 10.0f, 11.0f, 11.0f)), "move 1 p/ perto de 2");
    ids_equal(index->query_point(10.5f, 0.5f, 10.5f), { 1, 2 },
              "após move: {1,2} na célula nova");
    ids_equal(index->query_point(0.5f, 0.5f, 0.5f), {}, "célula antiga vazia");

    check(index->remove(2), "remove 2");
    ids_equal(index->query_point(10.5f, 0.5f, 10.5f), { 1 },
              "após remove: só 1");
    check(!index->remove(2), "remove de desconhecido → false");
    check(!index->move(99, box(0.0f, 0.0f, 1.0f, 1.0f)), "move de desconhecido → false");
}

void test_refusals() {
    auto index = engine::entity::create_spatial_index();
    std::string error;
    check(!index->configure(0.0f, error), "cellSize 0 recusa");
    check(!index->configure(-1.0f, error), "cellSize negativa recusa");
    check(index->configure(1.0f, error), "configure válida");

    check(index->insert(1, box(0.0f, 0.0f, 1.0f, 1.0f), error), "insert 1");
    check(!index->insert(1, box(2.0f, 2.0f, 3.0f, 3.0f), error),
          "insert duplicado recusa");
    check(!index->insert(2, box(5.0f, 5.0f, 1.0f, 1.0f), error),
          "bounds min > max recusa");
    check(index->count() == 1, "estado intacto após recusas");
}

void test_determinism() {
    auto a = engine::entity::create_spatial_index();
    auto b = engine::entity::create_spatial_index();
    std::string error;
    a->configure(1.0f, error);
    b->configure(1.0f, error);
    // Insere em ordem DIFERENTE em b — resultados devem ser iguais (sorted).
    const float coords[][4] = { { 0, 0, 1, 1 }, { 2, 2, 3, 3 }, { -1, -1, 0, 0 } };
    for (int i = 0; i < 3; ++i) {
        a->insert(i + 1, box(coords[i][0], coords[i][1], coords[i][2], coords[i][3]), error);
    }
    for (int i = 2; i >= 0; --i) {
        b->insert(3 - i, box(coords[i][0], coords[i][1], coords[i][2], coords[i][3]), error);
    }
    ids_equal(a->query_aabb(box(-5.0f, -5.0f, 5.0f, 5.0f)),
              b->query_aabb(box(-5.0f, -5.0f, 5.0f, 5.0f)),
              "query determinística independente da ordem de inserção");
    ids_equal(a->query_point(0.5f, 0.5f, 0.5f), { 1 },
              "ponto (0.5,0.5) → {1} em ambos");
}

}  // namespace

int main() {
    test_insert_query();
    test_remove_move();
    test_refusals();
    test_determinism();

    if (failures == 0) {
        std::printf("spatial_index_tests: all checks passed\n");
        return 0;
    }
    std::printf("spatial_index_tests: %d failure(s)\n", failures);
    return 1;
}

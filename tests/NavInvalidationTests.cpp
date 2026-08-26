// NavInvalidationTests — gate do contrato INavInvalidation (§2 item 24,
// invalidação localizada CORE): prova tiles_for (região → tiles em ordem),
// invalidate marca só os intersectantes, rebuild limpa, versão monotônica e
// invalidated_since (retomada do streaming).

#include "engine/navigation/INavInvalidation.hpp"

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

bool tiles_equal(const std::vector<engine::navigation::NavTile>& actual,
                 const std::vector<engine::navigation::NavTile>& expected,
                 const char* what) {
    bool ok = actual.size() == expected.size();
    if (ok) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (!(actual[i] == expected[i])) ok = false;
        }
    }
    check(ok, what);
    return ok;
}

engine::navigation::NavInvalidationRegion region(float minX, float minZ,
                                                 float maxX, float maxZ) {
    engine::navigation::NavInvalidationRegion r;
    r.minX = minX; r.minZ = minZ; r.maxX = maxX; r.maxZ = maxZ;
    return r;
}

void test_tiles_for() {
    auto invalidation = engine::navigation::create_nav_invalidation();
    std::string error;
    check(invalidation->configure(2.0f, error), "configure tileSize 2");

    // Região inteiramente dentro de um tile.
    tiles_equal(invalidation->tiles_for(region(0.0f, 0.0f, 1.0f, 1.0f)), { { 0, 0 } },
                "região pequena → tile (0,0)");
    // Atravessa fronteira (x 1.9..2.1 → tiles 0 e 1).
    tiles_equal(invalidation->tiles_for(region(1.9f, 0.0f, 2.1f, 1.0f)),
                { { 0, 0 }, { 1, 0 } },
                "atravessa fronteira x → (0,0),(1,0)");
    // Região com coordenadas negativas (floor).
    tiles_equal(invalidation->tiles_for(region(-0.5f, -0.5f, 0.5f, 0.5f)),
                { { -1, -1 }, { -1, 0 }, { 0, -1 }, { 0, 0 } },
                "região na origem → 4 tiles (ordem x,z)");
    // Região inválida → vazio (sem mutar).
    check(invalidation->tiles_for(region(5.0f, 5.0f, 1.0f, 1.0f)).empty(),
          "região min > max → vazio");
}

void test_invalidate_rebuild() {
    auto invalidation = engine::navigation::create_nav_invalidation();
    std::string error;
    check(invalidation->configure(2.0f, error), "configure");
    check(invalidation->version() == 0, "versão inicial 0");

    // Região de TILE ÚNICO (max < tileSize 2) para semântica exata.
    invalidation->invalidate(region(0.0f, 0.0f, 1.5f, 1.5f));
    check(invalidation->version() == 1, "versão 1 após 1ª invalidação");
    check(invalidation->is_invalid({ 0, 0 }), "(0,0) inválido");
    check(!invalidation->is_invalid({ 5, 5 }), "(5,5) intacto");
    tiles_equal(invalidation->invalid_tiles(), { { 0, 0 } }, "invalid_tiles");

    // Reconstrução limpa.
    check(invalidation->rebuild({ 0, 0 }), "rebuild (0,0)");
    check(!invalidation->is_invalid({ 0, 0 }), "(0,0) válido após rebuild");
    check(invalidation->invalid_tiles().empty(), "sem tiles inválidos");
    check(!invalidation->rebuild({ 0, 0 }), "rebuild de tile válido → false");
}

void test_invalidated_since() {
    auto invalidation = engine::navigation::create_nav_invalidation();
    std::string error;
    check(invalidation->configure(2.0f, error), "configure");

    // Regiões de tile único: v1 → (0,0); v2 → (1,1) (limite < tileSize).
    invalidation->invalidate(region(0.0f, 0.0f, 1.5f, 1.5f));
    invalidation->invalidate(region(2.0f, 2.0f, 3.5f, 3.5f));

    tiles_equal(invalidation->invalidated_since(0), { { 0, 0 }, { 1, 1 } },
                "desde v0 → ambos");
    tiles_equal(invalidation->invalidated_since(1), { { 1, 1 } },
                "desde v1 → só (1,1)");
    check(invalidation->invalidated_since(2).empty(), "desde v2 → vazio");

    // Rebuild de (0,0) tira do invalidated_since.
    invalidation->rebuild({ 0, 0 });
    tiles_equal(invalidation->invalidated_since(0), { { 1, 1 } },
                "após rebuild de (0,0): só (1,1)");
}

}  // namespace

int main() {
    test_tiles_for();
    test_invalidate_rebuild();
    test_invalidated_since();

    if (failures == 0) {
        std::printf("nav_invalidation_tests: all checks passed\n");
        return 0;
    }
    std::printf("nav_invalidation_tests: %d failure(s)\n", failures);
    return 1;
}

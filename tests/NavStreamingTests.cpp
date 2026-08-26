// NavStreamingTests — gate do contrato INavStreaming (§2 item 29, streaming
// de navegação CORE): prova o gate de região ativa (foco ± raio), as listas
// de load/unload conforme o foco move, o ledger de carregado e a retomada
// segura (tile ativo invalidado só volta a valer após rebuild).

#include "engine/navigation/INavStreaming.hpp"

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

void test_active_gating() {
    auto streaming = engine::navigation::create_nav_streaming();
    std::string error;
    check(streaming->configure(1, error), "configure raio 1");
    streaming->set_focus(0, 0);

    check(streaming->is_tile_active({ 0, 0 }) && streaming->is_tile_active({ 1, 1 }) &&
              streaming->is_tile_active({ -1, -1 }),
          "tiles dentro do raio 1 ativos");
    check(!streaming->is_tile_active({ 2, 0 }) && !streaming->is_tile_active({ 0, 2 }),
          "tiles fora do raio inativos");

    // 3×3 = 9 tiles para carregar.
    const std::vector<engine::navigation::NavTile> toLoad = streaming->tiles_to_load();
    check(toLoad.size() == 9, "9 tiles para carregar");
    check(toLoad[0] == (engine::navigation::NavTile{ -1, -1 }) &&
              toLoad[8] == (engine::navigation::NavTile{ 1, 1 }),
          "tiles_to_load em ordem (x,z)");

    check(streaming->mark_loaded({ 0, 0 }), "mark_loaded (0,0)");
    check(streaming->loaded_count() == 1, "1 carregado");
    check(!streaming->mark_loaded({ 5, 5 }), "mark_loaded de inativo → false");
}

void test_focus_move() {
    auto streaming = engine::navigation::create_nav_streaming();
    std::string error;
    check(streaming->configure(1, error), "configure");
    streaming->set_focus(0, 0);
    for (const auto& tile : streaming->tiles_to_load()) {
        check(streaming->mark_loaded(tile), "carrega todos os 9");
    }
    check(streaming->loaded_count() == 9, "9 carregados");

    streaming->set_focus(2, 0);
    const std::vector<engine::navigation::NavTile> toUnload = streaming->tiles_to_unload();
    check(toUnload.size() == 6, "6 tiles saem da região ativa");
    // A nova região (foco 2,0, raio 1) cobre x em [1,3], z em [-1,1] (9 tiles);
    // a antiga cobria x em [-1,1]. Interseção: x=1 (3 tiles) → 9-3 = 6 fora.
    check(!streaming->is_tile_active({ 0, 0 }) && streaming->is_tile_active({ 3, 1 }),
          "região nova correta");

    const std::vector<engine::navigation::NavTile> toLoad = streaming->tiles_to_load();
    check(toLoad.size() == 6, "6 novos tiles para carregar (x 2..3)");
}

void test_invalidation_retry() {
    auto streaming = engine::navigation::create_nav_streaming();
    std::string error;
    check(streaming->configure(1, error), "configure");
    streaming->set_focus(0, 0);
    for (const auto& tile : streaming->tiles_to_load()) {
        streaming->mark_loaded(tile);
    }

    check(streaming->invalidate_tile({ 0, 0 }), "invalidate_tile (0,0)");
    tiles_equal(streaming->tiles_pending_rebuild(), { { 0, 0 } },
                "pending_rebuild → (0,0)");
    check(!streaming->invalidate_tile({ 5, 5 }), "invalidate de inativo → false");

    // Descarregar zera a marca de inválido (o tile sai da região).
    streaming->set_focus(5, 5);
    for (const auto& tile : streaming->tiles_to_unload()) {
        streaming->mark_unloaded(tile);
    }
    check(streaming->tiles_pending_rebuild().empty(), "sem pending após unload");
    check(streaming->loaded_count() == 0, "nada carregado na região nova ainda");
    check(!streaming->is_loaded({ 0, 0 }), "(0,0) descarregado");

    // Carrega a região nova e verifica a retomada segura: (5,5) limpo
    // (a marca de inválido antiga morreu no unload).
    for (const auto& tile : streaming->tiles_to_load()) {
        streaming->mark_loaded(tile);
    }
    check(streaming->loaded_count() == 9, "9 tiles da nova região carregados");
    check(streaming->is_loaded({ 5, 5 }), "(5,5) carregado e limpo");
    check(streaming->tiles_pending_rebuild().empty(), "sem pending na região nova");
}

}  // namespace

int main() {
    test_active_gating();
    test_focus_move();
    test_invalidation_retry();

    if (failures == 0) {
        std::printf("nav_streaming_tests: all checks passed\n");
        return 0;
    }
    std::printf("nav_streaming_tests: %d failure(s)\n", failures);
    return 1;
}

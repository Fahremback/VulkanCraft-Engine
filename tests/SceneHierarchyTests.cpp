// SceneHierarchyTests — gate headless do contrato engine/editor ISceneHierarchy.
//
// Verifica o modelo de hierarquia da cena: lista plana DFS estável (filhos
// logo após o pai, ordem de inserção), profundidade, raízes, busca por nome
// case-insensitive com pais incluídos, ciclos quebrados e JSON determinístico.

#include "engine/editor/ISceneHierarchy.hpp"

#include <cstdio>
#include <string>
#include <vector>

using engine::editor::create_scene_hierarchy;
using engine::editor::HierarchyEntity;
using engine::editor::HierarchyLink;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::vector<HierarchyEntity> sample_entities() {
    return {
        {"root", "Root"},
        {"a", "Alpha"},
        {"b", "Beta"},
        {"c", "Gamma"},
    };
}

void test_flat_roots_in_order() {
    auto h = create_scene_hierarchy();
    const auto rows = h->build(sample_entities(), {}, "");
    check(rows.size() == 4, "4 raízes na ordem de inserção");
    check(rows[0].name == "Root" && rows[0].depth == 0, "primeira raiz Root");
    check(rows[3].name == "Gamma" && rows[3].depth == 0, "última raiz Gamma");
    check(rows[0].index == 0 && rows[3].index == 3, "índices estáveis");
}

void test_dfs_children_after_parent() {
    auto h = create_scene_hierarchy();
    // root tem filhos a,b; a tem filho c.
    const std::vector<HierarchyLink> links = {
        {"a", "root"},
        {"b", "root"},
        {"c", "a"},
    };
    const auto rows = h->build(sample_entities(), links, "");
    check(rows.size() == 4, "4 linhas");
    check(rows[0].name == "Root" && rows[0].depth == 0, "Root primeiro");
    check(rows[1].name == "Alpha" && rows[1].depth == 1, "Alpha depth 1 logo após Root");
    check(rows[2].name == "Gamma" && rows[2].depth == 2, "Gamma depth 2 (filho de Alpha)");
    check(rows[3].name == "Beta" && rows[3].depth == 1, "Beta depth 1 (segundo filho)");
}

void test_search_case_insensitive() {
    auto h = create_scene_hierarchy();
    const auto rows = h->build(sample_entities(), {}, "ALPHA");
    check(rows.size() == 1, "busca case-insensitive acha Alpha");
    check(rows[0].name == "Alpha", "linha correta");
    // busca sem match → vazio
    check(h->build(sample_entities(), {}, "zzz").empty(), "sem match → vazio");
}

void test_search_includes_matching_parents() {
    auto h = create_scene_hierarchy();
    const std::vector<HierarchyLink> links = {
        {"a", "root"},
        {"c", "a"},
    };
    // busca "gamma": o filho casa → o pai (Alpha) e o avô (Root) entram junto
    const auto rows = h->build(sample_entities(), links, "gamma");
    check(rows.size() == 3, "pais do filho casando entram junto");
    check(rows[0].name == "Root" && rows[0].depth == 0, "avô incluído");
    check(rows[1].name == "Alpha" && rows[1].depth == 1, "pai incluído");
    check(rows[2].name == "Gamma" && rows[2].depth == 2, "filho casando");
}

void test_cycle_broken_as_root() {
    auto h = create_scene_hierarchy();
    // ciclo: a→b, b→a (nenhum é raiz; ambos devem aparecer, determinístico)
    const std::vector<HierarchyLink> links = {
        {"a", "b"},
        {"b", "a"},
    };
    const auto rows = h->build(sample_entities(), links, "");
    check(rows.size() == 4, "ciclo não explode — 4 linhas");
    // nenhuma profundidade além de 0 para os membros do ciclo
    for (const auto& r : rows) {
        check(r.depth == 0, "membros do ciclo tratados como raiz");
    }
}

void test_json_deterministic() {
    auto a = create_scene_hierarchy();
    auto b = create_scene_hierarchy();
    const std::vector<HierarchyLink> links = {{"a", "root"}};
    const auto rows = a->build(sample_entities(), links, "");
    const std::string j1 = a->to_json(rows);
    check(j1.find("\"depth\":1") != std::string::npos, "JSON contém depth");
    const auto rows2 = b->build(sample_entities(), links, "");
    check(b->to_json(rows2) == j1, "JSON determinístico para a mesma entrada");
}

}  // namespace

int main() {
    test_flat_roots_in_order();
    test_dfs_children_after_parent();
    test_search_case_insensitive();
    test_search_includes_matching_parents();
    test_cycle_broken_as_root();
    test_json_deterministic();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}

// HierarchicalPathTests — gate do contrato IHierarchicalPath (§4 item 26,
// pathfinding hierárquico). Prova: caminho na mesma região (A* concreto),
// caminho multi-região via portais (2 níveis), rota inexistente, validação
// all-or-nothing (nó duplicado/aresta desconhecida/custo negativo rejeitam
// e mantêm o grafo anterior), contagens e determinismo.

#include "engine/navigation/IHierarchicalPath.hpp"

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

// Grade 3x3 por região: região 0 = nós 0..8, região 1 = nós 10..18,
// região 2 = nós 20..28. Portais: (8,10) entre R0→R1, (18,20) entre R1→R2.
void build_graph(engine::navigation::IHierarchicalPath& hp) {
    std::vector<engine::navigation::HPathNode> nodes;
    std::vector<engine::navigation::HPathEdge> edges;
    auto add_grid = [&](std::uint32_t base, std::uint32_t region, float ox) {
        for (int i = 0; i < 9; ++i) {
            const int x = i % 3;
            const int z = i / 3;
            engine::navigation::HPathNode node;
            node.id = base + static_cast<std::uint32_t>(i);
            node.x = ox + static_cast<float>(x);
            node.z = static_cast<float>(z);
            node.region = region;
            node.cost = 1.0f;
            nodes.push_back(node);
        }
    };
    add_grid(0, 0, 0.0f);
    add_grid(10, 1, 4.0f);
    add_grid(20, 2, 8.0f);
    auto link = [&](std::uint32_t a, std::uint32_t b, float cost) {
        engine::navigation::HPathEdge edge;
        edge.a = a;
        edge.b = b;
        edge.cost = cost;
        edges.push_back(edge);
    };
    // Intra-região: grade 4-vizinhança.
    auto neighbors = [](int i) {
        const int x = i % 3;
        const int z = i / 3;
        std::vector<int> out;
        if (x > 0) out.push_back(i - 1);
        if (x < 2) out.push_back(i + 1);
        if (z > 0) out.push_back(i - 3);
        if (z < 2) out.push_back(i + 3);
        return out;
    };
    for (int base = 0; base < 30; base += 10) {
        for (int i = 0; i < 9; ++i) {
            for (const int nb : neighbors(i)) {
                if (i < nb) link(base + static_cast<std::uint32_t>(i),
                                 base + static_cast<std::uint32_t>(nb), 1.0f);
            }
        }
    }
    // Portais entre regiões (fronteira): nó canto (2,0) da R0 ↔ (0,0) da R1,
    // e (2,0) da R1 ↔ (0,0) da R2.
    link(8, 10, 1.0f);    // R0 → R1
    link(18, 20, 1.0f);   // R1 → R2

    std::string error;
    check(hp.configure(nodes, edges, error), "configure aceita o grafo");
}

void test_same_region() {
    auto hp = engine::navigation::create_hierarchical_path();
    build_graph(*hp);
    check(hp->node_count() == 27, "27 nós");
    check(hp->region_count() == 3, "3 regiões");

    // Mesma região (0 → 4): A* concreto direto.
    engine::navigation::HPathResult r = hp->find_path(0, 4);
    check(r.found, "caminho na mesma região encontrado");
    if (r.found) {
        check(!r.nodes.empty() && r.nodes.front() == 0 && r.nodes.back() == 4,
              "mesma região: começa em 0 e termina em 4");
    }

    // start == goal.
    r = hp->find_path(4, 4);
    check(r.found && r.nodes.size() == 1 && r.nodes[0] == 4, "start == goal");
}

void test_multi_region() {
    auto hp = engine::navigation::create_hierarchical_path();
    build_graph(*hp);

    // R0 (nó 0) → R2 (nó 24): precisa atravessar 2 portais.
    engine::navigation::HPathResult r = hp->find_path(0, 24);
    check(r.found, "caminho multi-região encontrado");
    if (r.found) {
        check(r.nodes.front() == 0 && r.nodes.back() == 24, "multi-região: extremos corretos");
        // Deve conter o portal 8→10 e 18→20 na sequência.
        bool hasPortal1 = false;
        bool hasPortal2 = false;
        for (std::size_t i = 0; i + 1 < r.nodes.size(); ++i) {
            if ((r.nodes[i] == 8 && r.nodes[i + 1] == 10) ||
                (r.nodes[i] == 10 && r.nodes[i + 1] == 8)) hasPortal1 = true;
            if ((r.nodes[i] == 18 && r.nodes[i + 1] == 20) ||
                (r.nodes[i] == 20 && r.nodes[i + 1] == 18)) hasPortal2 = true;
        }
        check(hasPortal1 && hasPortal2, "multi-região: atravessa os 2 portais");
    }

    // Determinismo: mesmo grafo, mesmo caminho.
    const engine::navigation::HPathResult r2 = hp->find_path(0, 24);
    check(r2.found && r2.nodes == r.nodes && r2.totalCost == r.totalCost,
          "determinístico (mesmo grafo → mesmo caminho)");
}

void test_no_route_and_validation() {
    auto hp = engine::navigation::create_hierarchical_path();
    build_graph(*hp);

    std::vector<engine::navigation::HPathNode> nodes;
    std::vector<engine::navigation::HPathEdge> edges;
    std::string error;
    // Grafo vazio é válido (0 nós/0 regiões); busca com qualquer id falha.
    check(hp->configure(nodes, edges, error), "configure vazio aceito (0 nós)");
    check(hp->node_count() == 0 && hp->region_count() == 0, "vazio: 0 nós, 0 regiões");
    engine::navigation::HPathResult empty = hp->find_path(1, 2);
    check(!empty.found, "grafo vazio: nenhuma rota");
    build_graph(*hp);
    engine::navigation::HPathResult r = hp->find_path(0, 999);
    check(!r.found, "nó desconhecido → não encontrado");
    r = hp->find_path(999, 0);
    check(!r.found, "start desconhecido → não encontrado");

    // Validação all-or-nothing: grafo inválido rejeitado e anterior mantido.
    std::vector<engine::navigation::HPathNode> badNodes;
    engine::navigation::HPathNode n1;
    n1.id = 1; n1.x = 0; n1.z = 0; n1.region = 0; n1.cost = 1.0f;
    badNodes.push_back(n1);
    engine::navigation::HPathNode n2 = n1;
    n2.id = 1;  // duplicado
    badNodes.push_back(n2);
    check(!hp->configure(badNodes, edges, error), "nó duplicado rejeitado");
    check(hp->node_count() == 27, "grafo anterior intacto após rejeição");

    // Custo negativo.
    badNodes.clear();
    badNodes.push_back(n1);
    engine::navigation::HPathNode n3 = n1;
    n3.id = 2;
    badNodes.push_back(n3);
    engine::navigation::HPathEdge bad;
    bad.a = 1; bad.b = 2; bad.cost = -1.0f;
    edges.push_back(bad);
    check(!hp->configure(badNodes, edges, error), "custo negativo rejeitado");
    check(hp->node_count() == 27, "grafo anterior intacto após custo negativo");
}

}  // namespace

int main() {
    test_same_region();
    test_multi_region();
    test_no_route_and_validation();

    if (failures == 0) {
        std::printf("hierarchical_path_tests: all checks passed\n");
        return 0;
    }
    std::printf("hierarchical_path_tests: %d failure(s)\n", failures);
    return 1;
}

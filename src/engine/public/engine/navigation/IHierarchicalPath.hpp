#pragma once
// IHierarchicalPath — pathfinding hierárquico de 2 níveis sobre regiões
// (estilo HPA*). Contrato do §4 item 26 (pathfinding hierárquico entre
// chunks/regiões) — unidade CORE do algoritmo, sem depender do navmesh do
// INavigationProvider (o chamador pode conectar os dois).
//
// Modelo: o mundo é particionado em REGIÕES (ex.: chunks). Cada região tem
// NÓS (pontos de navegação); arestas intra-região conectam nós vizinhos da
// MESMA região; PORTALS conectam nós de regiões DIFERENTES (fronteiras).
// O solver:
//   1. A* abstrato no grafo de regiões: nós abstratos = regiões, custo entre
//      regiões = menor custo portal a portal (pré-computado e cacheado por
//      revision);
//   2. A* concreto dentro da região de origem (até o portal de saída) e da
//      região de destino (do portal de entrada) — o trecho intermediário é o
//      caminho abstrato (não re-expande nós do meio: é isso que dá o ganho
//      de escala).
//
// Determinístico (fila de prioridade com desempate por id — ordem de
// inserção estável), self-contained (std), sem RNG, sem estado global.
// Custo por aresta pode refletir material/terreno (chamador decide).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::navigation {

struct HPathNode {
    std::uint32_t id{ 0 };
    float x{ 0.0f };
    float z{ 0.0f };
    std::uint32_t region{ 0 };   // região a que o nó pertence
    float cost{ 1.0f };          // custo de atravessar o nó (>= 0)
};

// Aresta intra-região (mesma região) ou portal (regiões diferentes).
struct HPathEdge {
    std::uint32_t a{ 0 };
    std::uint32_t b{ 0 };
    float cost{ 1.0f };          // custo da aresta (>= 0)
};

struct HPathResult {
    bool found{ false };
    std::vector<std::uint32_t> nodes;   // sequência de nós (origem → destino)
    float totalCost{ 0.0f };
};

class IHierarchicalPath {
public:
    virtual ~IHierarchicalPath() = default;

    // Reconstrói o grafo (nós + arestas + portais). All-or-nothing: um nó
    // com região inexistente, aresta com nó desconhecido ou custo negativo
    // rejeita a configuração INTEIRA e mantém o grafo anterior.
    virtual bool configure(const std::vector<HPathNode>& nodes,
                           const std::vector<HPathEdge>& edges,
                           std::string& errorOut) = 0;

    virtual std::size_t node_count() const = 0;
    virtual std::size_t region_count() const = 0;

    // Caminho hierárquico de `start` a `goal` (ids de nós). Vazio (found
    // false) se não houver rota. Determinístico para o mesmo grafo.
    virtual HPathResult find_path(std::uint32_t start, std::uint32_t goal) const = 0;
};

std::unique_ptr<IHierarchicalPath> create_hierarchical_path();

}  // namespace engine::navigation

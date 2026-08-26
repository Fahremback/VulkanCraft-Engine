// HierarchicalPath.cpp — adapter do contrato IHierarchicalPath.
// A* de 2 níveis: (1) pré-computa, por par de regiões vizinhas, o menor
// custo portal a portal (A* concreto restrito às DUAS regiões — Dijkstra
// bidirecional não é necessário; a fronteira é pequena); (2) A* abstrato
// sobre regiões usando esses custos; (3) A* concreto na região de origem
// (nó → portal de saída) e na de destino (portal de entrada → nó) e
// concatena com o caminho abstrato. Desempate de fila por id (estável).

#include "engine/navigation/IHierarchicalPath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine::navigation {

namespace {

struct AStarNode {
    std::uint32_t id;
    float f;  // g + h
    float g;
    std::uint32_t parent;
};

struct GreaterF {
    bool operator()(const AStarNode& a, const AStarNode& b) const {
        if (a.f != b.f) return a.f > b.f;
        return a.id > b.id;  // desempate estável por id
    }
};

}  // namespace

class HierarchicalPathImpl final : public IHierarchicalPath {
public:
    bool configure(const std::vector<HPathNode>& nodes,
                   const std::vector<HPathEdge>& edges,
                   std::string& errorOut) override {
        // --- Validação all-or-nothing ---
        std::unordered_map<std::uint32_t, std::size_t> indexOf;
        std::unordered_map<std::uint32_t, std::uint32_t> regionOf;
        std::unordered_set<std::uint32_t> regions;
        for (const HPathNode& node : nodes) {
            if (node.cost < 0.0f) {
                errorOut = "hpath: custo de nó negativo";
                return false;
            }
            if (indexOf.count(node.id)) {
                errorOut = "hpath: nó duplicado";
                return false;
            }
            indexOf[node.id] = indexOf.size();
            regionOf[node.id] = node.region;
            regions.insert(node.region);
        }
        std::vector<HPathEdge> edgeList;
        edgeList.reserve(edges.size());
        for (const HPathEdge& edge : edges) {
            if (edge.cost < 0.0f) {
                errorOut = "hpath: custo de aresta negativo";
                return false;
            }
            if (!indexOf.count(edge.a) || !indexOf.count(edge.b)) {
                errorOut = "hpath: aresta com nó desconhecido";
                return false;
            }
            if (edge.a == edge.b) {
                errorOut = "hpath: aresta de auto-laço";
                return false;
            }
            edgeList.push_back(edge);
        }

        // --- Build adjacency (intra + inter) ---
        nodes_ = nodes;
        indexOf_ = std::move(indexOf);
        regionOf_ = std::move(regionOf);
        regions_ = std::vector<std::uint32_t>(regions.begin(), regions.end());
        std::sort(regions_.begin(), regions_.end());

        adjacency_.clear();
        adjacency_.resize(nodes_.size());
        edgeCosts_.clear();
        for (const HPathEdge& edge : edgeList) {
            const std::size_t ia = indexOf_[edge.a];
            const std::size_t ib = indexOf_[edge.b];
            adjacency_[ia].push_back(edge.b);
            adjacency_[ib].push_back(edge.a);
            edgeCosts_[key(edge.a, edge.b)] = edge.cost;
            edgeCosts_[key(edge.b, edge.a)] = edge.cost;
        }

        // --- Portal-pair costs entre regiões vizinhas (cache) ---
        regionPortalCosts_.clear();
        for (const HPathEdge& edge : edgeList) {
            const std::uint32_t ra = regionOf_[edge.a];
            const std::uint32_t rb = regionOf_[edge.b];
            if (ra == rb) continue;
            regionPortalCosts_[regionKey(ra, rb)][key(edge.a, edge.b)] = edge.cost;
        }
        return true;
    }

    std::size_t node_count() const override { return nodes_.size(); }
    std::size_t region_count() const override { return regions_.size(); }

    HPathResult find_path(std::uint32_t start, std::uint32_t goal) const override {
        HPathResult result;
        if (!indexOf_.count(start) || !indexOf_.count(goal)) return result;
        if (start == goal) {
            result.found = true;
            result.nodes.push_back(start);
            result.totalCost = 0.0f;
            return result;
        }
        const std::uint32_t startRegion = regionOf_.at(start);
        const std::uint32_t goalRegion = regionOf_.at(goal);

        // 1. A* abstrato sobre regiões.
        std::vector<std::uint32_t> abstractPath;
        if (!find_abstract(startRegion, goalRegion, abstractPath)) return result;

        // 2. Caminho concreto dentro da região de origem e de destino.
        std::vector<std::uint32_t> full;
        float total = 0.0f;
        if (abstractPath.size() == 1) {
            // Mesma região: A* concreto direto.
            return a_star(start, goal);
        }
        // Origem → portal de saída da primeira região.
        const std::uint32_t exitPortal = best_portal(startRegion, abstractPath[1], start);
        if (exitPortal == 0) return result;
        {
            HPathResult seg = a_star(start, exitPortal);
            if (!seg.found) return result;
            for (const std::uint32_t n : seg.nodes) full.push_back(n);
            total += seg.totalCost;
        }
        // Portais intermediários (região i → i+1): caminho portal a portal.
        for (std::size_t i = 1; i + 1 < abstractPath.size(); ++i) {
            const std::uint32_t inPortal = best_portal(abstractPath[i], abstractPath[i - 1], 0);
            const std::uint32_t outPortal = best_portal(abstractPath[i], abstractPath[i + 1], 0);
            if (inPortal == 0 || outPortal == 0) return result;
            HPathResult seg = a_star(inPortal, outPortal);
            if (!seg.found) return result;
            if (!full.empty() && full.back() == inPortal) {
                for (std::size_t n = 1; n < seg.nodes.size(); ++n) full.push_back(seg.nodes[n]);
            } else {
                for (const std::uint32_t n : seg.nodes) full.push_back(n);
            }
            total += seg.totalCost;
        }
        // Portal de entrada da última região → goal.
        const std::uint32_t lastIn = best_portal(goalRegion, abstractPath[abstractPath.size() - 2], 0);
        if (lastIn == 0) return result;
        {
            HPathResult seg = a_star(lastIn, goal);
            if (!seg.found) return result;
            if (!full.empty() && full.back() == lastIn) {
                for (std::size_t n = 1; n < seg.nodes.size(); ++n) full.push_back(seg.nodes[n]);
            } else {
                for (const std::uint32_t n : seg.nodes) full.push_back(n);
            }
            total += seg.totalCost;
        }

        result.found = !full.empty();
        result.nodes = std::move(full);
        result.totalCost = total;
        return result;
    }

private:
    static std::uint64_t key(std::uint32_t a, std::uint32_t b) {
        return (static_cast<std::uint64_t>(a) << 32) | b;
    }
    static std::uint64_t regionKey(std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return key(a, b);
    }

    static float heuristic(const HPathNode& a, const HPathNode& b) {
        const float dx = a.x - b.x;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    // A* concreto sobre os nós (qualquer par). Determinístico.
    HPathResult a_star(std::uint32_t start, std::uint32_t goal) const {
        HPathResult result;
        const std::size_t n = nodes_.size();
        std::vector<float> g(n, std::numeric_limits<float>::infinity());
        std::vector<std::uint32_t> parent(n, 0);
        std::vector<bool> closed(n, false);
        std::priority_queue<AStarNode, std::vector<AStarNode>, GreaterF> open;

        const std::size_t startIdx = indexOf_.at(start);
        const std::size_t goalIdx = indexOf_.at(goal);
        g[startIdx] = 0.0f;
        open.push(AStarNode{ start, heuristic(nodes_[startIdx], nodes_[goalIdx]), 0.0f, 0 });

        while (!open.empty()) {
            const AStarNode cur = open.top();
            open.pop();
            const std::size_t ci = indexOf_.at(cur.id);
            if (closed[ci]) continue;
            closed[ci] = true;
            if (cur.id == goal) {
                // Reconstrói o caminho.
                std::vector<std::uint32_t> rev;
                std::uint32_t id = goal;
                rev.push_back(id);
                while (id != start) {
                    id = parent[indexOf_.at(id)];
                    rev.push_back(id);
                }
                std::reverse(rev.begin(), rev.end());
                result.found = true;
                result.nodes = std::move(rev);
                result.totalCost = cur.g;
                return result;
            }
            for (const std::uint32_t nb : adjacency_[ci]) {
                const std::size_t ni = indexOf_.at(nb);
                if (closed[ni]) continue;
                const float cost = edgeCosts_.at(key(cur.id, nb));
                const float tentative = cur.g + cost + nodes_[ni].cost;
                if (tentative < g[ni]) {
                    g[ni] = tentative;
                    parent[ni] = cur.id;
                    open.push(AStarNode{ nb, tentative + heuristic(nodes_[ni], nodes_[goalIdx]),
                                         tentative, cur.id });
                }
            }
        }
        return result;
    }

    // A* abstrato: nós = regiões, arestas = regiões com portal(s). Custo da
    // aresta abstrata = menor custo portal a portal (pré-computado).
    bool find_abstract(std::uint32_t startRegion, std::uint32_t goalRegion,
                       std::vector<std::uint32_t>& outPath) const {
        if (startRegion == goalRegion) {
            outPath.push_back(startRegion);
            return true;
        }
        // Grafo abstrato: região → vizinhos (com custo mínimo portal-portal).
        std::unordered_map<std::uint32_t, std::vector<std::pair<std::uint32_t, float>>> graph;
        for (const auto& [rk, costs] : regionPortalCosts_) {
            const std::uint32_t ra = static_cast<std::uint32_t>(rk >> 32);
            const std::uint32_t rb = static_cast<std::uint32_t>(rk & 0xffffffffu);
            if (costs.empty()) continue;
            float best = std::numeric_limits<float>::infinity();
            for (const auto& [pk, cost] : costs) best = std::min(best, cost);
            graph[ra].push_back({ rb, best });
            graph[rb].push_back({ ra, best });
        }
        // A* sobre regiões (heurística 0 — custos arbitrários).
        std::unordered_map<std::uint32_t, float> g;
        std::unordered_map<std::uint32_t, std::uint32_t> parent;
        using QNode = std::pair<float, std::uint32_t>;  // (g, região)
        std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> open;
        g[startRegion] = 0.0f;
        open.push({ 0.0f, startRegion });
        while (!open.empty()) {
            const auto [cost, region] = open.top();
            open.pop();
            if (region == goalRegion) {
                std::vector<std::uint32_t> rev;
                std::uint32_t r = goalRegion;
                rev.push_back(r);
                while (r != startRegion) {
                    r = parent.at(r);
                    rev.push_back(r);
                }
                std::reverse(rev.begin(), rev.end());
                outPath = std::move(rev);
                return true;
            }
            auto found = graph.find(region);
            if (found == graph.end()) continue;
            for (const auto& [nb, edgeCost] : found->second) {
                const float tentative = cost + edgeCost;
                auto it = g.find(nb);
                if (it == g.end() || tentative < it->second) {
                    g[nb] = tentative;
                    parent[nb] = region;
                    open.push({ tentative, nb });
                }
            }
        }
        return false;
    }

    // Melhor portal entre duas regiões (menor custo); se `near` != 0,
    // prefere o portal mais perto do nó `near` (para a região de origem).
    std::uint32_t best_portal(std::uint32_t ra, std::uint32_t rb, std::uint32_t near) const {
        const auto found = regionPortalCosts_.find(regionKey(ra, rb));
        if (found == regionPortalCosts_.end()) return 0;
        std::uint32_t best = 0;
        float bestCost = std::numeric_limits<float>::infinity();
        for (const auto& [pk, cost] : found->second) {
            float score = cost;
            if (near != 0 && indexOf_.count(near)) {
                const std::uint32_t portalA = static_cast<std::uint32_t>(pk >> 32);
                const std::uint32_t portalB = static_cast<std::uint32_t>(pk & 0xffffffffu);
                const HPathNode& na = nodes_[indexOf_.at(portalA)];
                const HPathNode& nb = nodes_[indexOf_.at(portalB)];
                const HPathNode& nn = nodes_[indexOf_.at(near)];
                const float toA = std::sqrt((na.x - nn.x) * (na.x - nn.x) +
                                            (na.z - nn.z) * (na.z - nn.z));
                const float toB = std::sqrt((nb.x - nn.x) * (nb.x - nn.x) +
                                            (nb.z - nn.z) * (nb.z - nn.z));
                score += std::min(toA, toB);
            }
            if (score < bestCost) {
                bestCost = score;
                // retorna o nó do portal que está na região ra
                const std::uint32_t portalA = static_cast<std::uint32_t>(pk >> 32);
                const std::uint32_t portalB = static_cast<std::uint32_t>(pk & 0xffffffffu);
                best = regionOf_.at(portalA) == ra ? portalA : portalB;
            }
        }
        return best;
    }

    std::vector<HPathNode> nodes_;
    std::unordered_map<std::uint32_t, std::size_t> indexOf_;
    std::unordered_map<std::uint32_t, std::uint32_t> regionOf_;
    std::vector<std::uint32_t> regions_;
    std::vector<std::vector<std::uint32_t>> adjacency_;
    std::unordered_map<std::uint64_t, float> edgeCosts_;
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, float>> regionPortalCosts_;
};

std::unique_ptr<IHierarchicalPath> create_hierarchical_path() {
    return std::make_unique<HierarchicalPathImpl>();
}

}  // namespace engine::navigation

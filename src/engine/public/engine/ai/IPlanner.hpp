#pragma once
// IPlanner — contrato público de planejamento GOAP (agente 4 §3 item 3).
//
// Planejador de objetivos puro e determinístico: SEM RNG, SEM relógio de
// parede, SEM estado global — a mesma spec + estado + objetivo produzem o
// mesmo plano bit-exato entre instâncias. O núcleo NÃO conhece o jogo: mundo =
// átomos booleanos (nome → true/false); ações = {preconditions, effects,
// cost}; o plano é uma sequência de ids que o caller executa.
//
// Busca: uniform-cost (Dijkstra) sobre o espaço de estados; expansão em ordem
// de declaração das ações (determinística); empate de custo → ordem de
// declaração; uma ação não se repete no plano; `max_plan_length` limita o
// tamanho (terminação garantida). JSON versionado all-or-nothing bit-exact.

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine::ai {

struct PlannerAction {
    std::string id;
    double cost = 1.0;                        // finito e > 0
    std::map<std::string, bool> preconditions;  // átomos exigidos
    std::map<std::string, bool> effects;        // átomos alterados
};

struct PlannerSpec {
    std::vector<PlannerAction> actions;
    int max_plan_length = 16;  // >= 1

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct PlanResult {
    bool success = false;
    std::vector<std::string> actions;  // ids em ordem de execução
    double total_cost = 0.0;
};

// GOAP runtime (busca pura/determinística).
class IPlanner {
public:
    virtual ~IPlanner() = default;

    // Aplica a spec (all-or-nothing via PlannerSpec::validate).
    virtual bool configure(const PlannerSpec& spec, std::string& errorOut) = 0;

    // Define um átomo do estado atual (true/false).
    virtual void set_atom(const std::string& name, bool value) = 0;

    // Define um átomo do objetivo (true = quer presente, false = quer ausente).
    virtual void set_goal(const std::string& name, bool value) = 0;

    // Calcula o plano de menor custo do estado atual para o objetivo.
    // Determinístico; sucesso com plano vazio quando o objetivo já vale.
    virtual PlanResult plan(std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IPlanner).
std::unique_ptr<IPlanner> create_planner();

}  // namespace engine::ai

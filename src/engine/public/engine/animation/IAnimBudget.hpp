#pragma once
// IAnimBudget — orçamento de atualização de animação por frame (LOD de
// atualização). §4 item 49, componente "atualização por budget".
//
// Contrato público self-contained (std only), sem dependência de core.
// Política determinística de seleção dentro de um orçamento de tempo por
// frame, com FAIRNESS anti-starvation: entradas puladas acumulam boost de
// prioridade e sobem na fila até serem atendidas.
//
// Semântica determinística:
//   select(entries): ordena por (importance + owed) desc, empate por id
//     (estável); seleciona em ordem até que a PRÓXIMA estoure o orçamento
//     (nunca excede — sem item parcial). Selecionadas → owed = 0;
//     puladas → owed += boost. used_ms = soma dos custos selecionados.
//
// Escopo §4 item 49: LOD ✅ (IAnimationLod) + skinning CPU ✅ (#228)
// + budget (esta unidade); resta o lado GPU (renderer).

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct AnimUpdateEntry {
    std::string id;           // único entre os candidatos do frame
    double importance = 0.0;  // prioridade (maior = mais importante)
    double cost_ms = 1.0;     // custo estimado de atualizar (ms, >= 0)
};

struct BudgetFrame {
    std::vector<std::string> selected;  // ids selecionados (ordem de escolha)
    double used_ms = 0.0;
    std::vector<std::string> skipped;   // ids pulados (fora do orçamento)
};

// Agendador de budget determinístico (sem RNG/relógio real).
class IAnimBudget {
public:
    virtual ~IAnimBudget() = default;

    // Orçamento por frame (ms > 0) e boost de fairness (>= 0) somado à
    // prioridade de cada entrada pulada. All-or-nothing.
    virtual bool configure(double budget_ms, double boost,
                           std::string& errorOut) = 0;

    // Seleciona os candidatos do frame dentro do orçamento. Entradas com
    // custo 0 entram sempre. Erros: id vazio/duplicado, custo negativo.
    virtual BudgetFrame select(const std::vector<AnimUpdateEntry>& entries,
                               std::string& errorOut) = 0;

    virtual double budget_ms() const = 0;
    virtual double boost() const = 0;

    // Prioridade efetiva (importance + owed) de um id no próximo frame.
    virtual double effective_priority(const std::string& id) const = 0;

    // Zera owed de todos os ids (fairness reiniciada).
    virtual void reset() = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAnimBudget).
std::unique_ptr<IAnimBudget> create_anim_budget();

}  // namespace engine::animation

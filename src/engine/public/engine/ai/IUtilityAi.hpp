#pragma once
// IUtilityAi — contrato público de utility AI (agente 4 §3 item 3).
//
// Seleção de ações por utilidade, pura e determinística: SEM RNG, SEM relógio
// de parede, SEM estado global — as mesmas entradas produzem as mesmas
// utilidades bit-exatas entre instâncias. O núcleo NÃO conhece o jogo: o
// caller alimenta inputs normalizados via `set_input` e mapeia o id
// selecionado (`select()`) para a lógica real.
//
// Modelo:
//   - ação = id único + lista de considerações ponderadas.
//   - consideração = { input, curve, weight, min, max, threshold }:
//       normalized = clamp((value - min) / (max - min), 0, 1)   (max > min)
//       curve(linear)  = normalized
//       curve(inverse) = 1 - normalized
//       curve(step)    = normalized >= threshold ? 1 : 0
//   - utilidade da ação = Σ(weight·score) / Σ(weight)  (pesos 0 são ignorados;
//     sem considerações ou Σweight == 0 → utilidade 0).
//   - select(): a ação de maior utilidade; empate → a primeira na ordem de
//     declaração (determinístico). JSON versionado all-or-nothing bit-exact.

#include <memory>
#include <string>
#include <vector>

namespace engine::ai {

enum class UtilityCurve { Linear, Inverse, Step };

struct UtilityConsideration {
    std::string input;         // nome do input lido via set_input
    UtilityCurve curve = UtilityCurve::Linear;
    double weight = 1.0;       // >= 0 (0 = ignorado)
    double min = 0.0;          // fim inferior do remap (max > min)
    double max = 1.0;
    double threshold = 0.5;    // usado só por Step; em [0, 1]
};

struct UtilityAction {
    std::string id;
    std::vector<UtilityConsideration> considerations;
};

struct UtilitySpec {
    std::vector<UtilityAction> actions;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct UtilitySelection {
    std::string id;      // ação selecionada (vazia = nenhuma)
    double utility = 0.0;
};

struct UtilityScore {
    std::string id;
    double utility = 0.0;

    bool operator==(const UtilityScore& o) const {
        return id == o.id && utility == o.utility;
    }
};

// Utility AI runtime (seleção pura/determinística, sem estado persistente).
class IUtilityAi {
public:
    virtual ~IUtilityAi() = default;

    // Aplica a spec (all-or-nothing via UtilitySpec::validate).
    virtual bool configure(const UtilitySpec& spec, std::string& errorOut) = 0;

    // Define um input (valor normalmente em [0,1]; o remap min/max da
    // consideração normaliza). Inputs ausentes valem 0.
    virtual void set_input(const std::string& name, double value) = 0;

    // Utilidade de uma ação específica (0 para id desconhecido).
    virtual double score(const std::string& id) const = 0;

    // Ação de maior utilidade; empate → primeira na ordem de declaração.
    // Retorna id vazio quando não há ações.
    virtual UtilitySelection select() const = 0;

    // Utilidades de todas as ações, ordenadas por utilidade desc; empate →
    // ordem de declaração (determinístico).
    virtual std::vector<UtilityScore> utilities() const = 0;
};

// Fábrica do adapter (o único TU implementando IUtilityAi).
std::unique_ptr<IUtilityAi> create_utility_ai();

}  // namespace engine::ai

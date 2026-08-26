// ITimelinePolicy — política de ORÇAMENTO e COMPACTAÇÃO da timeline de
// estados temporais. Componente CORE do §6 item 68 ("garantir que snapshots
// de tempo tenham versionamento, budgets, compactação e compatibilidade com
// multiplayer"): o ITimeTravel (META §19) registra estados/branches; esta
// política decide, de forma DETERMINÍSTICA, quais estados REMOVER quando o
// orçamento estoura (mantém os `maxStates` lexicograficamente primeiros —
// regra estável sem timestamp) e quais COMPACTAR (estados do mesmo mundo
// com o mesmo caminho de snapshot = duplicatas exatas → manter o primeiro).
// O chamador (ITimeTravel/WorldManager) executa as remoções; a política é
// pura e testável headless.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace world {

// Mesma shape do TimelineStateInfo do ITimeTravel (name/world/path).
struct TimelinePolicyState {
    std::string name;
    std::string worldName;
    std::string path;
};

struct TimelinePolicyConfig {
    std::size_t maxStates{ 16 };      // >= 1
    bool compactionEnabled{ true };   // dedup de snapshots idênticos
};

class ITimelinePolicy {
public:
    virtual ~ITimelinePolicy() = default;

    // All-or-nothing: maxStates == 0 recusa.
    virtual bool configure(const TimelinePolicyConfig& config,
                           std::string& errorOut) = 0;

    // Estados a REMOVER quando count > maxStates: mantém os `maxStates`
    // primeiros em ordem lexicográfica de nome, remove o resto (em ordem).
    virtual std::vector<std::string> prune(
        const std::vector<TimelinePolicyState>& states) const = 0;

    // Estados a REMOVER por compactação: duplicatas exatas (mesmo mundo +
    // mesmo path), mantém o primeiro por ordem lexicográfica de nome.
    // Vazio quando compaction desabilitada.
    virtual std::vector<std::string> compact(
        const std::vector<TimelinePolicyState>& states) const = 0;

    virtual std::size_t max_states() const = 0;
};

std::unique_ptr<ITimelinePolicy> create_timeline_policy();

}  // namespace world
}  // namespace engine

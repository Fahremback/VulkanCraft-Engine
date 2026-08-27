#pragma once
// ICrowdSimulation — contrato público de MULTIDÕES determinísticas (agente 4,
// unidade de integração §10 l.176 — "Integrar multidões com LOD de simulação,
// sleeping, agregação distante e retomada determinística").
//
// Composição, sem duplicar os irmãos:
//   - IAiLod            → LOD por ENTIDADE (Full/Reduced/Aggregate/Dormant);
//   - ISimulationLod    → LOD por REGIÃO (tiers + sleeping + agregação);
//   - este contrato     → LOD por MULTIDÃO/AGENTE com sleeping por agente,
//     agregação distante por GRUPO (uma multidão distante vira um agregado de
//     contadores determinístico) e retomada bit-exact via serialização.
//
// Modelo:
//   - A multidão é uma coleção de AGENTES (id + posição). Cada agente é
//     classificado em um tier pela distância ao foco (radius por tier,
//     monotônico — Full < Reduced < Aggregate < Dormant).
//   - Sleeping: um agente Dormant para de tickar; `idle_ticks` acumula. O
//     contador de dormência é a "memória" de quanto tempo o agente ficou
//     parado — se o foco se aproximar, o agente ACORDA (volta a Full) e a
//     dormência é zerada.
//   - Agregação distante: quando um GRUPO de agentes fica inteiramente em
//     Aggregate, a multidão passa a evoluir por um modelo analítico
//     determinístico (contadores populacionais por tipo) em vez de tickar cada
//     agente. A transição Aggregate→Full carrega os contadores no evento de
//     retomada (handoff coerente).
//   - Budgets: `max_ticks_per_frame` limita quantos agentes tickam por frame
//     (excesso → os MAIS DISTANTES ficam para o próximo frame, determinístico
//     por ordenação). `max_agents` limita o tamanho da multidão (all-or-nothing
//     no configure).
//   - Sem RNG, sem relógio de parede, sem estado global: as mesmas entradas
//     produzem a mesma evolução bit-exata entre instâncias.
//   - Persistência: serialize_state/deserialize_state round-trip bit-exact
//     (tiers, dormência, contadores agregados, relógio de ticks) — save/load e
//     replicação de uma multidão distante.
//
// Self-contained (std apenas) + Vec3 de `engine/ai/ISteering.hpp`.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/ai/ISteering.hpp"

namespace engine::ai {

enum class CrowdTier {
    Full,       // ticka a cada frame
    Reduced,    // ticka a cada `reduced_interval` frames
    Aggregate,  // evoluído pelo modelo analítico (contadores)
    Dormant,    // não ticka (sleeping)
};

struct CrowdAgent {
    std::uint64_t id = 0;
    Vec3 position;
    std::string type;  // tag livre ("villager", "guard", ...)
};

// Configuração da multidão. `load_from_json`/`validate` são all-or-nothing.
struct CrowdSpec {
    double full_radius = 16.0;         // dist <= full → Full
    double reduced_radius = 64.0;      // dist <= reduced → Reduced (>= full)
    double aggregate_radius = 256.0;   // dist <= aggregate → Aggregate (>= reduced)
    double reduced_interval = 4.0;     // frames entre ticks Reduced (>= 1)
    std::uint32_t max_agents = 1000;   // teto de agentes (>= 1)
    std::uint32_t max_ticks_per_frame = 0;  // budget de ticks/frame (0 = ilimitado)

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct CrowdAgentState {
    std::uint64_t id = 0;
    CrowdTier tier = CrowdTier::Dormant;
    std::uint32_t idle_ticks = 0;  // dormência acumulada (Dormant incrementa)
    bool tick_this_frame = false;  // foi selecionado para tickar?
};

struct CrowdAggregate {
    std::string type;             // tipo dos agentes agregados
    std::uint64_t count = 0;      // população agregada
    double activity = 0.0;        // contador analítico determinístico
};

struct CrowdFrameResult {
    std::vector<CrowdAgentState> agent_states;   // ordenado por id
    std::vector<CrowdAggregate> aggregates;      // grupos Aggregate (por tipo)
    bool woke_any = false;   // algum Dormant acordou (foco se aproximou)?
};

class ICrowdSimulation {
public:
    virtual ~ICrowdSimulation() = default;

    // Aplica a spec (all-or-nothing via CrowdSpec::validate).
    virtual bool configure(const CrowdSpec& spec, std::string& errorOut) = 0;

    // Adiciona/remove agentes. `set_agents` substitui a população inteira
    // (all-or-nothing: id duplicado ou população acima de max_agents recusa
    // sem mutar). `remove_agent` é no-op se ausente.
    virtual bool set_agents(const std::vector<CrowdAgent>& agents,
                            std::string& errorOut) = 0;
    virtual void remove_agent(std::uint64_t id) = 0;
    virtual std::size_t agent_count() const = 0;

    // Avança `frames` frames de simulação a partir do foco `focus`. Cada frame
    // re-classifica os tiers por distância, dorme/acorda agentes, ticka os
    // selecionados (dentro do budget) e evolui os grupos Aggregate pelo modelo
    // analítico. Retorna o estado do ÚLTIMO frame. Determinístico.
    virtual CrowdFrameResult advance(const Vec3& focus, std::uint32_t frames,
                                     std::string& errorOut) = 0;

    // Estado observável do agente (tier/dormência atual).
    virtual bool agent_state(std::uint64_t id, CrowdAgentState& out) const = 0;

    // Estado serializado bit-exact / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando ICrowdSimulation).
std::unique_ptr<ICrowdSimulation> create_crowd_simulation();

}  // namespace engine::ai

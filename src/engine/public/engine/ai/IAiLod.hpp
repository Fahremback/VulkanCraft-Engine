#pragma once
// IAiLod — contrato público de LOD de IA por entidade (agente 4 §3 item 5).
//
// Motor de decisão de LOD para ENTIDADES de IA (complementar ao
// `engine/simulation/ISimulationLod.hpp` que é por região): puro e
// determinístico — SEM RNG, SEM relógio de parede, SEM estado global.
//
//   Full      → atualiza a cada tick
//   Reduced   → atualiza a cada `reduced_interval` ticks
//   Aggregate → atualiza a cada `aggregate_interval` ticks
//   Dormant   → não atualiza
//
// `tier_for(distance)` é a função pura de classificação; `should_update`
// aplica a lógica de intervalo por tick determinística; `allocate` classifica
// uma população e impõe budgets por tier (excesso → rebaixa o MAIS DISTANTE,
// empate → maior id rebaixado primeiro — determinístico). JSON versionado
// all-or-nothing bit-exact.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::ai {

enum class AiLodTier { Full, Reduced, Aggregate, Dormant };

struct AiLodSpec {
    double full_radius = 16.0;         // dist <= full → Full
    double reduced_radius = 64.0;      // dist <= reduced → Reduced (>= full)
    double aggregate_radius = 256.0;   // dist <= aggregate → Aggregate (>= reduced)
    double reduced_interval = 4.0;     // ticks entre updates Reduced (>= 1)
    double aggregate_interval = 16.0;  // ticks entre updates Aggregate (>= 1)
    int max_full = 0;                  // budget Full (0 = ilimitado)
    int max_reduced = 0;               // budget Reduced (0 = ilimitado)
    int max_aggregate = 0;             // budget Aggregate (0 = ilimitado)

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct AiLodEntry {
    std::uint64_t id = 0;
    double distance = 0.0;
};

struct AiLodAllocation {
    std::uint64_t id = 0;
    AiLodTier tier = AiLodTier::Dormant;
    bool update = false;  // deve atualizar neste tick?
};

// IA LOD runtime (classificação pura/determinística).
class IAiLod {
public:
    virtual ~IAiLod() = default;

    // Aplica a spec (all-or-nothing via AiLodSpec::validate).
    virtual bool configure(const AiLodSpec& spec, std::string& errorOut) = 0;

    // Tier puro por distância ao foco (sem estado).
    virtual AiLodTier tier_for(double distance) const = 0;

    // Intervalo determinístico por (tier, tick_index): Full sempre true,
    // Dormant sempre false, Reduced/Aggregate por módulo.
    virtual bool should_update(AiLodTier tier, std::uint64_t tick_index) const = 0;

    // Classifica a população (id, distância), impõe budgets por tier
    // (excesso → rebaixa o mais distante; empate → maior id primeiro) e
    // devolve alocações com a flag de update para `tick_index`.
    // Determinístico: mesma entrada → mesmas alocações bit-exatas.
    virtual std::vector<AiLodAllocation> allocate(
        std::uint64_t tick_index,
        const std::vector<AiLodEntry>& entries) const = 0;
};

// Fábrica do adapter (o único TU implementando IAiLod).
std::unique_ptr<IAiLod> create_ai_lod();

}  // namespace engine::ai

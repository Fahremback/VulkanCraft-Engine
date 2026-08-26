#pragma once
// IFixedTickSim — contrato público de fixed timestep determinístico
// (agente 4 §1 item 19).
//
// Acumulador de tempo fixo puro e determinístico: o jogo roda com um `fixed_dt`
// constante (independente do FPS) e o render interpola com `alpha`. SEM RNG,
// SEM relógio de parede, SEM estado global — a mesma sequência de `real_dt`
// produz o mesmo (ticks, alpha) bit-exato entre instâncias.
//
// Algoritmo (padrão fixed timestep com acumulador):
//   accumulator += real_dt
//   ticks = min(floor(accumulator / fixed_dt), max_ticks_per_frame)  // budget
//   accumulator -= ticks * fixed_dt
//   alpha = accumulator / fixed_dt   // [0, 1)
// `max_ticks_per_frame` evita o spiral of death (frames lentos não acumulam
// dívida infinita — o excesso fica retido no accumulator para o próximo frame).
// JSON versionado all-or-nothing bit-exact.

#include <memory>
#include <string>

namespace engine::simulation {

struct FixedTickSimSpec {
    double fixed_dt = 1.0 / 60.0;  // duração de UM tick de simulação (segundos)
    int max_ticks_per_frame = 8;   // budget de ticks por advance (>= 1)

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct FixedTickResult {
    int ticks = 0;      // quantos ticks fixed rodar neste frame
    double alpha = 0.0; // [0,1) — fator de interpolação para o render
};

// Fixed timestep runtime (acumulador puro/determinístico).
class IFixedTickSim {
public:
    virtual ~IFixedTickSim() = default;

    // Aplica a spec (all-or-nothing via FixedTickSimSpec::validate).
    virtual bool configure(const FixedTickSimSpec& spec, std::string& errorOut) = 0;

    // Avança um frame com `real_dt` (finito >= 0) e devolve quantos ticks
    // rodar + o alpha de interpolação. Determinístico.
    virtual FixedTickResult advance(double real_dt, std::string& errorOut) = 0;

    // Fator de interpolação atual (mesmo valor do último advance; [0,1)).
    virtual double alpha() const = 0;

    // Residual de tempo não convertido em tick (segundos, [0, fixed_dt)).
    virtual double accumulator() const = 0;

    // Zera o accumulator (novo mundo/restart).
    virtual void reset() = 0;

    // Estado (accumulator) serializado bit-exact / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IFixedTickSim).
std::unique_ptr<IFixedTickSim> create_fixed_tick_sim();

}  // namespace engine::simulation

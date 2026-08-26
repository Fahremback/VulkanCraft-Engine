#pragma once
// IAnimStateMachine — contrato público de animation state machine data-driven
// (agente 4 §4 item 1 — sobre a fundação IAnimCore).
//
// Máquina de estados de animação determinística e headless: cada estado
// referencia um CLIP (id resolvido pelo caller via IAnimCore — o contrato não
// acopla ao núcleo), avança `time_in_state` com `speed` e transiciona por
// evento/condição/timer. Puro e determinístico: SEM RNG, SEM relógio de
// parede, SEM estado global; a mesma spec + sequência de chamadas produzem o
// mesmo estado/tempo bit-exato entre instâncias. JSON versionado all-or-nothing
// bit-exact.
//
// Modelo:
//   - state: id + clip (id de clip) + speed (multiplicador do tempo, >= 0) +
//     loop (flag de loop — o tempo continua; o caller envolve pela duração).
//   - transition: de/para + UM gatilho (evento OU condição OU timer; exatamente
//     um). Timer = after_seconds (>= 0; transita quando time_in_state cruza).
//   - tick(dt): avança time_in_state += dt·speed; avalia transições por timer
//     e por condição na ORDEM DE DECLARAÇÃO (primeira que casa vence).
//   - send_event: avalia transições por evento (primeira que casa vence).
//   - estado terminal (sem transições de saída) → done().

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct AnimState {
    std::string id;
    std::string clip;       // id de clip (resolvido pelo caller via IAnimCore)
    double speed = 1.0;     // multiplicador do tempo (>= 0)
    bool loop = true;
};

struct AnimTransition {
    std::string from;
    std::string to;
    std::string on_event;      // gatilho por evento ("" = inativo)
    std::string on_condition;  // gatilho por condição ("" = inativo)
    double after_seconds = 0.0;  // gatilho por timer (0 = inativo)
    double blend_s = 0.0;        // crossfade entre estados (>= 0) — metadado
};

struct AnimStateMachineSpec {
    std::string id;
    std::string initial;
    std::vector<AnimState> states;
    std::vector<AnimTransition> transitions;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

// Animation state machine (transições determinísticas por evento/condição/timer).
class IAnimStateMachine {
public:
    virtual ~IAnimStateMachine() = default;

    // Aplica a spec (all-or-nothing via AnimStateMachineSpec::validate).
    virtual bool configure(const AnimStateMachineSpec& spec, std::string& errorOut) = 0;

    // Entra no estado inicial (time 0).
    virtual bool start(std::string& errorOut) = 0;

    virtual std::string state() const = 0;
    virtual std::string clip() const = 0;  // clip do estado atual
    virtual double time_in_state() const = 0;
    virtual double state_speed() const = 0;
    virtual bool is_looping() const = 0;

    // Evento (avalia transições por evento na ordem de declaração).
    virtual bool send_event(const std::string& event, std::string& errorOut) = 0;
    // Condição (avaliada no tick).
    virtual bool set_condition(const std::string& name, bool value,
                               std::string& errorOut) = 0;

    // Avança o tempo (dt >= 0) e avalia timer/condição na ordem de declaração.
    virtual bool tick(double dt, std::string& errorOut) = 0;

    // Estado terminal (sem transições de saída).
    virtual bool done() const = 0;

    // Estado (estado atual + tempo + condições) serializado bit-exact /
    // restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAnimStateMachine).
std::unique_ptr<IAnimStateMachine> create_anim_state_machine();

}  // namespace engine::animation

#pragma once
// IFsm — contrato público de máquina de estados finita (agente 4 §3 item 3).
//
// FSM data-driven e determinística: SEM RNG, SEM relógio de parede, SEM estado
// global — o tempo só entra pelo `dt` passado a `tick()`. O núcleo de decisão
// NÃO conhece o jogo: estados emitem `action` ids (enter/update/exit) que o
// caller mapeia para a lógica real, drenando-os via `drain_actions()`. JSON
// versionado all-or-nothing bit-exact (`to_json`/`load_from_json`).
//
// Modelo:
//   - estados: id único não-vazio; `enter`/`update`/`exit` (actions opcionais);
//     `terminal: true` marca estado final (`done()`).
//   - transições: cada uma tem EXATAMENTE UM gatilho —
//       on_event(name)      → dispara em send_event(name)
//       on_condition(name)  → dispara no tick se set_condition(name, true)
//       after_seconds(t)    → dispara no tick quando time_in_state >= t
//     Avaliadas em ordem de declaração; a primeira que casa vence (determinístico).
//   - start(): entra no estado inicial (emite o enter_action).
//   - tick(dt): avalia condição+time (ordem de declaração), dispara no máximo
//     UMA transição por tick; depois emite o update_action do estado atual.
//   - send_event(name): dispara imediatamente a primeira transição por evento
//     que casa (síncrono, determinístico).
//   - Em transição: exit do estado velho → enter do novo (nessa ordem), e o
//     time_in_state reseta. Estados terminais não transitam.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::ai {

struct FsmState {
    std::string id;
    std::string enter;    // action emitida ao entrar (vazio = nenhuma)
    std::string update;   // action emitida a cada tick no estado (vazio = nenhuma)
    std::string exit;     // action emitida ao sair (vazio = nenhuma)
    bool terminal = false;
};

struct FsmTransition {
    std::string from;
    std::string to;
    std::string on_event;        // gatilho por evento (vazio = inativo)
    std::string on_condition;    // gatilho por condição (vazio = inativo)
    double after_seconds = 0.0;  // gatilho por tempo (<= 0 = inativo)
};

struct FsmSpec {
    std::string initial;
    std::vector<FsmState> states;
    std::vector<FsmTransition> transitions;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

// FSM runtime (núcleo de decisão puro/determinístico).
class IFsm {
public:
    virtual ~IFsm() = default;

    // Aplica a spec (all-or-nothing via FsmSpec::validate).
    virtual bool configure(const FsmSpec& spec, std::string& errorOut) = 0;

    // Entra no estado inicial (emite o enter_action na fila de ações).
    virtual bool start(std::string& errorOut) = 0;

    // Avança um frame: avalia transições por condição/time em ordem de
    // declaração (no máximo uma por tick), depois emite o update_action.
    // dt finito >= 0.
    virtual bool tick(double dt, std::string& errorOut) = 0;

    // Dispara a primeira transição por evento que casa (síncrono).
    virtual bool send_event(const std::string& name, std::string& errorOut) = 0;

    // Define o valor de uma condição avaliada em tick (ordem de declaração).
    virtual void set_condition(const std::string& name, bool value) = 0;

    // Id do estado atual.
    virtual std::string state() const = 0;

    // true quando o estado atual é terminal.
    virtual bool done() const = 0;

    // Segundos no estado atual (determinístico, derivado do dt acumulado).
    virtual double time_in_state() const = 0;

    // Drena (e limpa) as actions emitidas desde o último drain, em ordem.
    virtual std::vector<std::string> drain_actions() = 0;

    // Estado (estado atual + timer) serializado bit-exact / restaurado
    // all-or-nothing (estado desconhecido recusa).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IFsm).
std::unique_ptr<IFsm> create_fsm();

}  // namespace engine::ai

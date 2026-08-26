#pragma once
// IAiEventBus — contrato público do bus de eventos de IA (agente 4 §3 item 1).
//
// Serviço de eventos determinístico para as decisões de IA: os contratos do
// domínio `engine/ai` (behavior tree, FSM, utility, planner, percepção) emitem
// eventos {tick, source, kind, payload} e o jogo os drena por frame. Puro e
// determinístico: SEM RNG, SEM relógio de parede, SEM estado global — a mesma
// sequência de emits produz a mesma serialização bit-exata. JSON versionado
// all-or-nothing bit-exact (round-trip preserva a ordem e os payloads).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::ai {

struct AiEvent {
    std::uint64_t tick = 0;
    std::string source;   // contrato emissor ("behavior_tree", "fsm", ...)
    std::string kind;     // tipo do evento ("state_changed", "action", ...)
    std::string payload;  // dados livres (JSON ou texto), opaco p/ o bus

    bool operator==(const AiEvent& o) const {
        return tick == o.tick && source == o.source && kind == o.kind &&
               payload == o.payload;
    }
};

struct AiEventBusSpec {
    int max_events = 0;  // 0 = ilimitado; > 0 = ring buffer (descarta o mais antigo)

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

// Bus de eventos de IA (log determinístico).
class IAiEventBus {
public:
    virtual ~IAiEventBus() = default;

    // Aplica a spec (all-or-nothing via AiEventBusSpec::validate).
    virtual bool configure(const AiEventBusSpec& spec, std::string& errorOut) = 0;

    // Registra um evento no fim do log. Com max_events > 0, o mais antigo é
    // descartado (FIFO determinístico) quando o log estoura.
    virtual void emit(std::uint64_t tick, const std::string& source,
                      const std::string& kind, const std::string& payload) = 0;

    // Log atual em ordem de emissão (sem limpar).
    virtual std::vector<AiEvent> peek() const = 0;

    // Log atual em ordem de emissão; depois limpa (drenar por frame).
    virtual std::vector<AiEvent> drain() = 0;

    // Limpa o log.
    virtual void clear() = 0;

    // Estado (log) serializado bit-exact / restaurado all-or-nothing.
    virtual std::string serialize() const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAiEventBus).
std::unique_ptr<IAiEventBus> create_ai_event_bus();

}  // namespace engine::ai

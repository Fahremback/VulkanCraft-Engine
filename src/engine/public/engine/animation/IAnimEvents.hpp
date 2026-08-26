#pragma once
// IAnimEvents — eventos nomeados em timestamps de clips de animação.
//
// Contrato público self-contained (std only) do §4 item 1 (unidade "events").
// Sem acoplamento a IAnimCore além da fábrica: o adapter recebe `IAnimCore&`
// apenas para validar o clip e sua duração no `add_event` (all-or-nothing).
//
// Semântica de polling determinística: `poll(clip, t0, t1)` devolve os eventos
// cujo tempo ∈ (t0, t1] — meio-aberto à direita — na ordem do clip
// (tempo, depois ordem de inserção). Um evento no exato t0 NÃO re-dispara;
// um evento no exato t1 dispara. Ideal para avanço por ticks (t0→t1).
//
// Escopo §4 item 1: CORE (#207) + state machine (#208) + root motion (#209)
// + events (#210); restam additive e masks (unidades registradas).

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct AnimEvent {
    std::string clip;
    double time = 0.0;
    std::string name;
};

// Eventos por clip — registro determinístico + polling meio-aberto.
class IAnimEvents {
public:
    virtual ~IAnimEvents() = default;

    // Valida all-or-nothing: clip existe no core; time ∈ [0, duration];
    // nome não vazio. Duplicata exata (clip+time+name) é rejeitada.
    virtual bool add_event(const AnimEvent& ev, std::string& errorOut) = 0;

    // Remove a primeira ocorrência exata; false se não existir.
    virtual bool remove_event(const std::string& clip, double time,
                              const std::string& name) = 0;

    // Eventos de um clip na ordem canônica (tempo, depois inserção).
    virtual std::vector<AnimEvent> events_for(const std::string& clip,
                                              std::string& errorOut) const = 0;

    // Eventos com tempo ∈ (t0, t1] — determinístico; clip desconhecido = erro.
    virtual std::vector<AnimEvent> poll(const std::string& clip, double t0,
                                        double t1,
                                        std::string& errorOut) const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAnimEvents).
std::unique_ptr<IAnimEvents> create_anim_events(IAnimCore& core);

}  // namespace engine::animation

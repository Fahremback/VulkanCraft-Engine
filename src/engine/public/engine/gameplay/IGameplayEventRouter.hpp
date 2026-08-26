// IGameplayEventRouter — wiring real entre a fila de eventos de gameplay
// (IGameplayEvents) e os sinks públicos de áudio (IAudioEventMapper) e
// métricas (IGameplayMetrics). Componente de WIRING dos §5 item 75
// (hooks visuais/sonoros) e §2 item 30 (métricas públicas): sistemas
// publicam eventos tipados; o router drena em FIFO e, por evento:
//  1) traduz o kind numérico → eventKind string via mapping configurável;
//  2) se existe trigger no audio mapper, emite um AudioTriggerRequest
//     (soundId/volume/pitch) na fila de saída (o backend de áudio real
//     consome — nunca tocamos no device);
//  3) registra um Counter no metrics por eventKind (contagem de eventos).
// Determinístico, headless, sem RNG. Os componentes são injetados (não
// owned). O mapping kind→string é all-or-nothing (duplicata recusa).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace audio {
class IAudioEventMapper;
}
namespace gameplay {

class IGameplayEvents;
class IGameplayMetrics;

// Requisição de áudio emitida pelo router (consumida pelo backend real).
struct AudioTriggerRequest {
    std::string eventKind;
    std::string soundId;
    float volume{ 1.0f };
    float pitch{ 1.0f };
};

class IGameplayEventRouter {
public:
    virtual ~IGameplayEventRouter() = default;

    // Configura o mapping kind numérico → eventKind string (all-or-nothing:
    // kind duplicado ou eventKind vazio recusa; o estado anterior é
    // preservado).
    virtual bool configure_mapping(
        const std::vector<std::pair<std::uint16_t, std::string>>& mapping,
        std::string& errorOut) = 0;

    // Drena os eventos pendentes (até maxCount; 0 = todos) e roteia.
    // Retorna as requisições de áudio emitidas na ordem dos eventos.
    virtual std::vector<AudioTriggerRequest> route(std::size_t maxCount = 0) = 0;

    // Contagem de eventos roteados (contador cumulativo; reset via reset()).
    virtual std::uint64_t routed_count() const = 0;

    virtual void reset() = 0;
};

// Cria o router com os componentes fornecidos (não owned; nullptr em
// qualquer um retorna nullptr).
std::unique_ptr<IGameplayEventRouter> create_gameplay_event_router(
    IGameplayEvents* events, engine::audio::IAudioEventMapper* audioMapper,
    IGameplayMetrics* metrics);

}  // namespace gameplay
}  // namespace engine

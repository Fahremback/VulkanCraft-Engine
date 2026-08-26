// IGameplayEvents — fila determinística de eventos de gameplay para hooks
// visuais/sonoros (renderer, editor, áudio) SEM acoplar os sistemas. Item
// §5 item 65 (hooks visuais/sonoros) — unidade CORE.
//
// Sistemas de gameplay (explosões, hit reactions, abilities, portais,
// destruição) PUBLICAM eventos tipados na fila; consumidores (renderer,
// áudio, editor) DRENAM na mesma ordem. O contrato não conhece nenhum
// consumidor: os sistemas só emitem, e quem quiser observa. Ordem FIFO
// estrita (ordem de publicação), capacity configurável (0 = ilimitado;
// com limite, publicar além do limite descarta o MAIS ANTIGO e conta o
// drop — determinístico).
//
// Self-contained (std), headless, determinístico. Sem RNG, sem estado
// global. Payload opaco (bytes) — o tipo do evento define o layout.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::gameplay {

// Um evento de gameplay. `kind` é um id estável por tipo (ex.: enum do
// chamador mapeado a uint16); `tick` é o tick lógico da publicação;
// `payload` são bytes opacos com o layout do tipo.
struct GameplayEvent {
    std::uint16_t kind{ 0 };
    std::uint64_t tick{ 0 };
    std::vector<std::uint8_t> payload;
};

class IGameplayEvents {
public:
    virtual ~IGameplayEvents() = default;

    // Publica um evento no fim da fila. Com capacity finita e fila cheia,
    // o evento MAIS ANTIGO é descartado (dropCount incremente) e o novo
    // entra. Determinístico.
    virtual void publish(std::uint16_t kind, std::uint64_t tick,
                         const std::vector<std::uint8_t>& payload) = 0;

    // Remove e retorna os eventos na ordem FIFO (até `maxCount`; 0 = todos
    // disponíveis). Consumir é destrutivo: eventos drenados saem da fila.
    virtual std::vector<GameplayEvent> drain(std::size_t maxCount = 0) = 0;

    virtual std::size_t pending_count() const = 0;
    virtual std::size_t dropped_count() const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IGameplayEvents> create_gameplay_events(std::size_t capacity = 0);

}  // namespace engine::gameplay

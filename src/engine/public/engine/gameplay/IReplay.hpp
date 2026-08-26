#pragma once
// IReplay — gravação e reprodução determinística de uma sessão de gameplay.
// Primeiro contrato do §5 item 60 (replay determinístico + profiling/debug).
//
// A simulação é determinística por frame quando recebe (tick, seed, inputs):
// o replay grava exatamente essa tripla por frame e a reproduz na MESMA
// ordem (bit-exact), permitindo re-executar a simulação e obter o mesmo
// resultado. O contrato NÃO conhece a simulação — inputs são opacos
// (bytes serializados pelo chamador) e o seed é a semente de RNG do frame.
//
// Self-contained (std only), headless, determinístico. Persistência JSON
// bit-exact e all-or-nothing (load só comita se TODOS os frames forem
// válidos — um frame fora de ordem/duplicado/tick decrescente rejeita o
// documento inteiro e deixa o estado anterior intacto).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::gameplay {

// Um frame gravado: o que a simulação precisa para reproduzir o tick.
struct ReplayFrame {
    std::uint64_t tick{ 0 };                 // tick lógico do frame
    std::uint32_t seed{ 0 };                 // semente de RNG usada no frame
    std::vector<std::uint8_t> inputs;        // inputs consolidados (opacos)
};

class IReplay {
public:
    virtual ~IReplay() = default;

    // --- Gravação ---
    // Grava um frame. `tick` deve ser estritamente maior que o último tick
    // gravado (ordem crescente, sem duplicatas) — caso contrário retorna
    // false e NADA é adicionado (all-or-nothing).
    virtual bool record_tick(std::uint64_t tick, std::uint32_t seed,
                             const std::vector<std::uint8_t>& inputs,
                             std::string& errorOut) = 0;

    virtual std::size_t frame_count() const = 0;
    virtual std::uint64_t first_tick() const = 0;
    virtual std::uint64_t last_tick() const = 0;

    // --- Reprodução ---
    // Posiciona o cursor no início. Depois, cada next_frame() entrega o
    // próximo frame gravado (bit-exact) e retorna false quando acaba.
    virtual bool begin_replay(std::string& errorOut) = 0;
    virtual bool next_frame(ReplayFrame& out) = 0;
    // Posiciona o cursor no primeiro frame com tick >= alvo (false se não
    // houver). O próximo next_frame() entrega esse frame.
    virtual bool seek_tick(std::uint64_t tick) = 0;

    // --- Edição ---
    // Descarta todos os frames com tick > alvo (trim do fim). Sempre ok
    // (mesmo vazio); frames anteriores ficam intactos.
    virtual bool truncate_after(std::uint64_t tick, std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

std::unique_ptr<IReplay> create_replay(std::size_t maxFrames = 0);

}  // namespace engine::gameplay

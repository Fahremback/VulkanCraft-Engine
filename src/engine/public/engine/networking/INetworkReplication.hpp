#pragma once
// INetworkReplication — replicação de estado por snapshots determinística.
// Primeiro contrato do domínio `engine/networking/` (§1 item 3 — interfaces
// estáveis; §6 item 4 — "snapshots" da rede pública).
//
// Núcleo headless da replicação: o host aplica frames de snapshot (tick +
// estados de entidade) em ordem estritamente crescente e cada lado consulta o
// último estado conhecido por entidade. O contrato NÃO conhece a rede nem a
// simulação — os dados de estado são opacos (bytes serializados pelo
// chamador), no mesmo espírito do IReplay (engine::gameplay). Determinístico:
// os mesmos frames na mesma ordem produzem o mesmo estado, bit-exact
// cross-instance.
//
// Self-contained (std only), headless, determinístico. Persistência JSON
// bit-exact e all-or-nothing (load só comita se TODOS os frames forem
// válidos — tick fora de ordem/decrescente, id duplicado ou kind vazio
// rejeita o documento inteiro e deixa o estado anterior intacto).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::networking {

// Estado replicado de uma entidade: id (opaco para o contrato), kind e os
// bytes do estado serializado pelo chamador.
struct NetworkEntityState {
    std::uint64_t entity_id{ 0 };
    std::string kind;                 // tipo da entidade (interesse/rota)
    std::vector<std::uint8_t> data;   // estado serializado (opaco)
};

// Um frame de snapshot aplicado ao estado replicado.
struct ReplicationFrame {
    std::uint64_t tick{ 0 };                    // tick lógico do frame
    std::vector<NetworkEntityState> states;     // estados do frame
};

class INetworkReplication {
public:
    virtual ~INetworkReplication() = default;

    // Identificador fixo da sessão replicada.
    virtual const std::string& session_id() const = 0;

    // Aplica um frame de snapshot. `frame.tick` deve ser estritamente maior
    // que o último tick aplicado, e os entity_id dentro do frame devem ser
    // únicos com kind não-vazio — caso contrário retorna false e NADA muda
    // (all-or-nothing). Um estado repetido no frame substitui o anterior.
    virtual bool apply_frame(const ReplicationFrame& frame, std::string& errorOut) = 0;

    // Último estado conhecido de uma entidade (nullptr se desconhecida).
    virtual const NetworkEntityState* state(std::uint64_t entity_id) const = 0;

    // Ids com estado conhecido, em ordem crescente.
    virtual std::vector<std::uint64_t> entity_ids() const = 0;

    virtual std::uint64_t tick_count() const = 0;   // frames aplicados
    virtual std::uint64_t last_tick() const = 0;    // último tick aplicado (0 = nenhum)

    // Descarta todo o estado replicado (nova sessão). Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria uma sessão de replicação. `sessionId` deve ser não-vazio (all-or-nothing).
std::unique_ptr<INetworkReplication> create_network_replication(const std::string& sessionId,
                                                                std::string& errorOut);

}  // namespace engine::networking

#pragma once
// INetworkRpc — RPC determinístico (chamadas remotas) para o domínio
// `engine/networking/` (§6 item 4 — "RPC" da rede pública).
//
// Núcleo headless do RPC: um registro de procedimentos remotos por nome, com
// payloads opacos (bytes serializados pelo chamador), ordem de execução
// determinística por sequência de chamada, e entrega confiável simulada por
// acknowledgment. O contrato NÃO conhece a rede — a fila de chamadas pode ser
// drenada por qualquer transporte (o mesmo espírito do INetworkReplication).
// Determinístico: as mesmas chamadas na mesma ordem produzem os mesmos
// acks, bit-exact cross-instance.
//
// Self-contained (std only), headless, determinístico. Persistência JSON
// bit-exact e all-or-nothing (load só comita se TODAS as chamadas forem
// válidas — procedure desconhecida, id duplicado ou payload vazio rejeita o
// documento inteiro e deixa o estado anterior intacto).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine::networking {

// Resultado da execução de um procedimento remoto. O handler é registrado
// pelo dono do procedimento; `data` é o payload de resposta (opaco).
struct RpcResult {
    bool ok{ false };
    std::vector<std::uint8_t> data;
    std::string error;               // preenchido quando !ok
};

// Handler de um procedimento remoto: recebe o payload e devolve o resultado.
// O contrato nunca inspeciona os bytes — a serialização é do chamador.
using RpcHandler = std::function<RpcResult(const std::vector<std::uint8_t>& payload)>;

// Uma chamada remota enfileirada para execução (ordem determinística por id).
struct RpcCall {
    std::uint64_t call_id{ 0 };      // sequência global (estritamente crescente)
    std::string procedure;           // nome do procedimento (não-vazio)
    std::vector<std::uint8_t> payload;  // argumentos (opacos)
    bool acked{ false };             // entregue e executado
};

class INetworkRpc {
public:
    virtual ~INetworkRpc() = default;

    // Identificador fixo da sessão RPC.
    virtual const std::string& session_id() const = 0;

    // Registra um procedimento remoto. Nome vazio ou duplicado → false e NADA
    // muda (all-or-nothing). `nullptr` handler → false.
    virtual bool register_procedure(const std::string& procedure, RpcHandler handler,
                                    std::string& errorOut) = 0;

    // Remove um procedimento. Sempre ok (ausente = no-op).
    virtual void unregister_procedure(const std::string& procedure) = 0;

    // Enfileira uma chamada para o próximo drain. Procedure desconhecida ou
    // payload inválido → false e NADA é enfileirado (all-or-nothing).
    virtual bool enqueue_call(const std::string& procedure,
                              const std::vector<std::uint8_t>& payload,
                              std::string& errorOut) = 0;

    // Executa as chamadas enfileiradas em ordem de enqueue e marca acked as
    // bem-sucedidas. Retorna os resultados na ordem das chamadas. Sem chamadas
    // → vetor vazio. O drain é destrutivo (a fila esvazia).
    virtual std::vector<RpcResult> drain(std::string& errorOut) = 0;

    // Chamadas enfileiradas ainda não executadas, em ordem de enqueue.
    virtual std::vector<RpcCall> pending_calls() const = 0;

    // Procedimentos registrados, em ordem alfabética.
    virtual std::vector<std::string> procedures() const = 0;

    // Próximo call_id a ser atribuído (sequência global).
    virtual std::uint64_t next_call_id() const = 0;

    // Descarta fila e registros (nova sessão). Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria uma sessão RPC. `sessionId` deve ser não-vazio (all-or-nothing).
std::unique_ptr<INetworkRpc> create_network_rpc(const std::string& sessionId,
                                                std::string& errorOut);

}  // namespace engine::networking

#pragma once
// IAuthoritativeRpc — comandos e eventos autoritativos com validação (seção F).
// Camada determinística por cima da entrega; o servidor é a única autoridade que
// EXECUTA comandos. O cliente apenas enfileira comandos com sequence/ack; o
// servidor valida (permissão, cooldown, reach, ownership), executa handlers e
// resume com códigos de erro SEGUROS (nunca vaza endereço/memória/caminho).
//
// Idempotência: cada (connection, command, sequence) executado é deduplicado —
// um retry/reconnect no mesmo sequence não re-executa o efeito. Eventos são
// server->client (broadcast ou alvo), separados de comandos (F.2).
//
// Self-contained (std), transport-free: os envelopes entram e saem por um
// transporte qualquer e são válidos por schema/limites ANTES de alocar efeito.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace networking {

// Regras de validação de um comando (F.6 permissoes/cooldown/reach/ownership).
struct CommandRules {
    std::string required_permission;      // "" = qualquer cliente autenticado
    std::uint64_t cooldown_millis{ 0 };   // 0 = sem cooldown
    float max_distance{ 0.0f };           // 0 = sem limite espacial
    bool require_ownership{ false };      // exige que o jogador seja dono da entidade-alvo
    std::size_t max_payload{ 4096 };      // limite antes de alocar efeito (H)
};

// Contexto do servidor no momento da execução (resolvido up-stack).
struct CommandContext {
    std::uint64_t connection_id{ 0 };
    std::uint64_t player_id{ 0 };
    std::uint64_t entity_net_id{ 0 };
    double origin_x{ 0.0 };
    double origin_y{ 0.0 };
    double origin_z{ 0.0 };
    std::uint64_t now_ms{ 0 };
    std::uint64_t tick{ 0 };
};

// Handler executado no SERVIDOR. Retorna ok + payload de resposta opcional.
using CommandHandler = std::function<struct CommandOutcome(
    const CommandContext&, const std::uint8_t* payload, std::size_t size)>;

struct CommandOutcome {
    bool ok{ false };
    std::vector<std::uint8_t> data;
};

// Comando digitado pelo cliente antes do transporte (com sequence/ack).
struct CommandEnvelope {
    std::uint64_t connection_id{ 0 };
    std::uint64_t sequence{ 0 };       // local do cliente (idempotência)
    std::uint64_t ack_sequence{ 0 };   // último processado pelo servidor
    std::string command;               // nome do procedimento autoritativo
    std::uint64_t entity_net_id{ 0 };
    // Reach validation: coordenadas do alvo (bloco/interação/vehicle).
    bool has_target{ false };
    double target_x{ 0.0 };
    double target_y{ 0.0 };
    double target_z{ 0.0 };
    std::vector<std::uint8_t> payload; // (schemas/limites validados no receive)
};

// Resultado do servidor para um comando (erros sempre códigos estáveis).
struct CommandResult {
    std::uint64_t sequence{ 0 };
    std::uint64_t connection_id{ 0 };
    bool ok{ false };
    std::string error;                 // código curto: "no_permission",
                                       // "cooldown", "out_of_reach", "no_ownership",
                                       // "invalid_schema", "unknown_command"
    std::vector<std::uint8_t> data;
};

class IAuthoritativeRpc {
public:
    virtual ~IAuthoritativeRpc() = default;

    // ---- server side ----
    virtual bool register_command(const std::string& name, CommandHandler handler,
                                  const CommandRules& rules, std::string& errorOut) = 0;
    virtual void unregister_command(const std::string& name) = 0;

    // Processa um lote de comandos recebidos: valida cada um e executa (em
    // ordem de chegada). Idempotência deduplica (connection,command,sequence).
    virtual std::vector<CommandResult> server_process(
        std::vector<CommandEnvelope> envelopes, const CommandContext& ctx,
        std::string& errorOut) = 0;

    // Evento server->client. target==0 => broadcast a todos; senão ao alvo.
    virtual bool server_enqueue_event(const std::string& name,
                                      const std::uint8_t* payload,
                                      std::size_t size,
                                      std::uint64_t target_connection,
                                      std::string& errorOut) = 0;
    virtual std::vector<CommandResult> server_drain_events(
        std::uint64_t connection, std::string& errorOut) = 0;

    // Torna um sequence processado conhecido do cliente (ack) para liberar
    // sua fila de retry.
    virtual bool client_mark_ack(std::uint64_t player_sequence,
                                 std::string& errorOut) = 0;

    // ---- client side ----
    virtual bool client_enqueue_command(const std::string& name,
                                        const std::uint8_t* payload,
                                        std::size_t size,
                                        std::uint64_t entity_net_id,
                                        std::string& errorOut) = 0;
    virtual std::vector<CommandEnvelope> client_drain_commands(
        std::size_t maximum, std::string& errorOut) = 0;
    virtual std::uint64_t client_fill_acked_sequence() const = 0;

    // Entidades de interesse para verificação de ownership (registradas pelo
    // servidor a partir do registre de ownership, seção D/E).
    virtual void server_set_owner(std::uint64_t entity_net_id,
                                  std::uint64_t player_id) = 0;
    virtual std::uint64_t server_owner_of(std::uint64_t entity_net_id) const = 0;

    virtual std::vector<std::string> commands() const = 0;
    virtual bool reset(std::string& errorOut) = 0;
};

std::unique_ptr<IAuthoritativeRpc> create_authoritative_rpc(std::string& errorOut);

}  // namespace networking
}  // namespace engine
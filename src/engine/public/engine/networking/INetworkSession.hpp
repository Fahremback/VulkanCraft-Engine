#pragma once
// INetworkSession — identidade e ciclo de vida de sessão (seção D do plano do
// Agente 3). Determinístico e transport-free: as identidades são ids opacos, o
// handshake valida versão/autenticação opcional/capabilities, e a sessão
// concede um token de reconexão reutilizável dentro da janela de retenção.
//
// Self-contained (std only), substitui a noção dispersa de "quem é a conexão"
// por um único dono das identidades: connection id, player id, entity network
// id e world id. Persistência não é papel deste contrato — a session é um
// estado vivo do servidor; snapshots autoritativos ficam no journal (H).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace networking {

// Versão do protocolo/do conteúdo. A compatibilidade é a soma dos vetores:
// qualquer divergência em campos não-neutros bloqueia a conexão (version
// negotiation A.6). Os campos são bits de feature-set, não versões de lib.
struct NetVersion {
    std::uint32_t protocol{ 1 };
    std::uint32_t registries{ 0 };   // registries/schemas de blocos, items, recipes
    std::uint32_t plugins{ 0 };      // plugins/serviços de gameplay
    std::uint32_t schemas{ 0 };      // schema de mensagens replicadas/RPC
    std::uint32_t content{ 0 };      // conteúdo: biomas, estruturas, prefabs

    [[nodiscard]] bool compatible_with(const NetVersion& other) const noexcept {
        return protocol == other.protocol &&
               registries == other.registries &&
               plugins == other.plugins &&
               schemas == other.schemas &&
               content == other.content;
    }
};

// Identidade persistente e não-ambígua de um cliente (D.1). `connection_id` é
// por conectar; `player_id` é o jogador (sobrevive reconnect); `entity_net_id`
// é a entidade-controlada (sobrevive portal/dimensão/reconnect); `world_id` é
// o mundo/dimensão corrente.
struct NetIdentity {
    std::uint64_t connection_id{ 0 };
    std::uint64_t player_id{ 0 };
    std::uint64_t entity_net_id{ 0 };
    std::uint64_t world_id{ 0 };
    std::string player_name;
    bool valid() const { return connection_id != 0 && player_id != 0; }
};

// Token de sessão para reconnect (D.4). Gerado no join, aceito no reconnect
// dentro de `ttl_millis` a partir de `issued_at`.
struct SessionToken {
    std::string value;                 // opaco (chave aleatória do servidor)
    std::uint64_t issued_at{ 0 };      // ms de relógio do servidor
    std::uint64_t ttl_millis{ 0 };     // janela de retenção
    bool used{ false };                // uma única retomada por token
    bool valid() const { return !value.empty(); }
};

// Capacidades declaradas no handshake (D.2). `requiredContent` são ids de
// conteúdo que o cliente exige; o servidor valida contra o seu catálogo.
struct SessionCapabilities {
    std::vector<std::string> required_content;
    std::vector<std::string> claimed_procedures;  // RPCs que o cliente oferece
};

// Resultado do handshake/join.
struct SessionResult {
    bool ok{ false };
    std::string error;
    NetIdentity identity;
    SessionToken token;
};

enum class SessionStatus : std::uint8_t {
    Unknown,        // conexão não registrada
    Handshaking,    // handshake iniciado, aguardando join
    Active,         // em jogo
    Reconnecting,   // token válido, retomando estado
    Left,           // graceful leave
    Kicked,
    Banned,
    Expired,        // timeout / token expirado
};

class INetworkSession {
public:
    virtual ~INetworkSession() = default;

    // Configura aquilo que o SERVIDOR oferece (versão corrente + catálogo).
    virtual void server_set_version(const NetVersion& version) = 0;
    virtual const NetVersion& server_version() const = 0;

    // D.2 handshake: valida versão (all-or-nothing) e, se authRequired,
    // exige `authPayload` não-vazio. Aprova as capabilities contra o catálogo.
    // `now_ms` é o relógio do servidor (ms).
    virtual SessionResult handshake(
        std::uint64_t connection_id, const NetVersion& clientVersion,
        const std::string& authPayload, const SessionCapabilities& caps,
        std::uint64_t now_ms) = 0;

    // D.3 join: associa player_id (e entity_net_id opcional) à conexão e emite
    // o token de reconexão. Duplicate login (player_id já ativo noutra conexão)
    // é rejeitado ou força o desligamento da anterior conforme `force`.
    virtual SessionResult join(std::uint64_t connection_id,
                               std::uint64_t player_id,
                               std::uint64_t world_id,
                               std::uint64_t entity_net_id,
                               bool force, std::uint64_t now_ms) = 0;

    // D.4 reconnect: troca o token por uma retomada. Valida o token (não
    // usado, dentro do TTL) e re-liga a mesma player_id/entity_net_id sob uma
    // nova connection_id, preservando ownership.
    virtual SessionResult reconnect(const std::string& tokenValue,
                                    std::uint64_t new_connection_id,
                                    std::uint64_t now_ms) = 0;

    // D.5 administração.
    virtual bool kick(std::uint64_t player_id, const std::string& reason) = 0;
    virtual bool ban(std::uint64_t player_id, const std::string& reason) = 0;
    virtual bool is_banned(std::uint64_t player_id) const = 0;
    // Marca o desconexão gracioso (libera a conexão, mantém token quando ativo).
    virtual void graceful_leave(std::uint64_t connection_id) = 0;
    // Timeout por falta de heartbeat / expiração de token.
    virtual void expire(std::uint64_t connection_id) = 0;

    // Consumo de heartbeat por conexão; retorna false se a sessão já não é
    // Active (o servidor deve encerrar a conexão).
    virtual bool touch(std::uint64_t connection_id, std::uint64_t now_ms) = 0;

    // D.6 ownership preservada em portal/dimensão/reconnect.
    virtual bool preserve_ownership(std::uint64_t connection_id,
                                    std::uint64_t entity_net_id) = 0;
    virtual bool transfer_world(std::uint64_t connection_id,
                                std::uint64_t new_world_id) = 0;

    // Consultas.
    virtual SessionStatus status(std::uint64_t player_id) const = 0;
    virtual std::vector<NetIdentity> connections() const = 0;
    virtual std::vector<SessionToken> live_tokens(std::uint32_t max) const = 0;
    virtual std::uint64_t active_player_count() const = 0;

    virtual bool reset(std::string& errorOut) = 0;
};

std::unique_ptr<INetworkSession> create_network_session(std::string& errorOut);

}  // namespace networking
}  // namespace engine
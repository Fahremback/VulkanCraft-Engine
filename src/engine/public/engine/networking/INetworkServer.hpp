#pragma once

#include "engine/networking/IAuthoritativeRpc.hpp"
#include "engine/networking/IClientPrediction.hpp"
#include "engine/networking/INetworkDiscovery.hpp"
#include "engine/networking/INetworkInterest.hpp"
#include "engine/networking/INetworkReplication.hpp"
#include "engine/networking/INetworkRpc.hpp"
#include "engine/networking/INetworkSession.hpp"
#include "engine/networking/IReplicationSecurity.hpp"
#include "engine/networking/ITransport.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace engine::networking {

// Configuração do servidor dedicado. `version` é a versão canônica para a
// negotiação com clientes (seção A.6/D); `security` são os limites anti-spam;
// `authRequired` força um payload de autenticação no handshake.
struct DedicatedServerConfig {
    std::string server_id;
    TransportConfig transport;
    std::uint32_t tick_rate{60};
    std::uint32_t max_clients{64};
    NetVersion version;
    SecurityLimits security;
    bool auth_required{false};
};
struct ServerMetrics {
    std::uint64_t tick{0};
    std::uint32_t clients{0};
    std::uint64_t messages_sent{0};
    std::uint64_t messages_received{0};
};
class INetworkServer {
public:
    virtual ~INetworkServer() = default;
    virtual bool start(const DedicatedServerConfig&, std::string&) = 0;
    virtual bool accept_client(std::uint64_t, std::string&) = 0;
    virtual bool remove_client(std::uint64_t, std::string&) = 0;
    virtual bool tick(std::string&) = 0;
    virtual bool stop(std::string&) = 0;
    virtual ServerMetrics metrics() const = 0;
    virtual TransportState state() const noexcept = 0;
    virtual INetworkReplication& replication() = 0;
    virtual INetworkRpc& rpc() = 0;
    virtual INetworkInterest& interest() = 0;
    virtual INetworkDiscovery& discovery() = 0;
    // Os sub-serviços canônicos da pilha (seções D/F/G/H): o runtime público
    // NÃO é mais só filas em memória — ele agrega transport real + sessão +
    // autoridade + predição + segurança. Sempre não-nulos após start().
    virtual INetworkSession& session() = 0;
    virtual IAuthoritativeRpc& authority() = 0;
    virtual IClientPrediction& prediction() = 0;
    virtual IReplicationSecurity& security() = 0;
    // Envia bytes a um cliente pelo transport real (channel-aware).
    virtual bool send_to_client(std::uint64_t connection, std::uint16_t channel,
                                bool reliable, bool unordered,
                                const std::vector<std::uint8_t>& payload,
                                std::string& error) = 0;
};
std::unique_ptr<INetworkServer> create_network_server();

// Helper de configuração canônica de servidor dedicado (Agente 3 §I): monta a
// DedicatedServerConfig a partir de valores simples (serverId, port, tickRate,
// maxClients, transport udp|loopback). Fonte única de verdade consumida pelo
// servidor dedicado real (main_server.cpp) e exposta ao editor/MCP — nunca uma
// trilha paralela de configuração.
DedicatedServerConfig make_dedicated_server_config(const std::string& serverId,
                                                   std::uint16_t port,
                                                   std::uint32_t tickRate,
                                                   std::uint32_t maxClients,
                                                   bool udp);
}

#pragma once
// INetworkGameClient — o lado CLIENTE do jogo usando a MESMA pilha pública do
// servidor (seção A/J: "jogo e servidor trocam dados pelo transport real e pelo
// mesmo protocolo"). O cliente não cria um runtime paralelo: ele reusa
// ITransport (loopback ou UDP real), INetworkSession (identidade + token de
// reconexão), IClientPrediction (predição/reconciliação locais) e insere as
// batches/snapshots recebidas na replicação de mundo local (IWorldReplication,
// client side). Transport-aware e determinístico.

#include "engine/networking/IClientPrediction.hpp"
#include "engine/networking/INetworkSession.hpp"
#include "engine/networking/ITransport.hpp"
#include "engine/world/IWorldReplication.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace networking {

struct GameClientConfig {
    TransportConfig transport;           // Loopback (editor/SP) ou Udp (rede)
    TransportEndpoint server;            // onde o servidor dedicado escuta
    NetVersion version;
    std::string player_name;
    std::uint64_t player_id{ 0 };
    std::string auth_payload;            // handshake auth opcional
    std::uint64_t start_world_id{ 1 };
};

class INetworkGameClient {
public:
    virtual ~INetworkGameClient() = default;

    // Conecta + handshake + join. Cria o transport real e a sessão local.
    virtual bool connect(const GameClientConfig&, std::string& errorOut) = 0;
    virtual void disconnect() = 0;
    virtual bool tick(double dt, std::string& errorOut) = 0;  // poll + heartbeat local

    // Predição local (G): mesmo contrato do servidor.
    virtual IClientPrediction& prediction() = 0;
    virtual INetworkSession& session() = 0;

    // Envio de comandos (bate na autoridade do servidor via authority).
    virtual bool send_command(const std::string& name, const std::uint8_t* payload,
                              std::size_t size, std::string& errorOut) = 0;
    // Envio genérico em um canal (snapshots/edits).
    virtual bool send_channel(std::uint16_t channel, bool reliable, bool unordered,
                              const std::vector<std::uint8_t>& payload,
                              std::string& errorOut) = 0;

    // Integra a replicação de mundo LOCAL: o caller fornece o mesmo
    // IWorldReplication que o servidor usa (client side).
    virtual void bind_world_replication(engine::world::IWorldReplication& worldReplication) = 0;
    virtual engine::world::IWorldReplication* world_replication() = 0;

    virtual bool connected() const = 0;
    virtual const TransportEndpoint& server_endpoint() const = 0;
};

std::unique_ptr<INetworkGameClient> create_network_game_client(std::string& errorOut);

}  // namespace networking
}  // namespace engine
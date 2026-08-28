#pragma once

#include "engine/networking/INetworkDiscovery.hpp"
#include "engine/networking/INetworkInterest.hpp"
#include "engine/networking/INetworkReplication.hpp"
#include "engine/networking/INetworkRpc.hpp"
#include "engine/networking/ITransport.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace engine::networking {

struct DedicatedServerConfig {
    std::string server_id;
    TransportConfig transport;
    std::uint32_t tick_rate{60};
    std::uint32_t max_clients{64};
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
};
std::unique_ptr<INetworkServer> create_network_server();
}

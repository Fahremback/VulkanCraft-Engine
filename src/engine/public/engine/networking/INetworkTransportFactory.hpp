#pragma once
#include "engine/networking/ITransport.hpp"
#include <memory>
#include <string>
#include <vector>
namespace engine::networking {
struct TransportBackendInfo { TransportKind kind{TransportKind::Loopback}; std::string name; bool available{false}; };
class INetworkTransportFactory {
public:
    virtual ~INetworkTransportFactory() = default;
    virtual std::vector<TransportBackendInfo> backends() const = 0;
    virtual std::unique_ptr<ITransport> create(const TransportConfig&, std::string&) const = 0;
};
std::unique_ptr<INetworkTransportFactory> create_network_transport_factory();
}

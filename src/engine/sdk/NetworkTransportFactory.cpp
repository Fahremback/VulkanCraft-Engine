#include "engine/networking/INetworkTransportFactory.hpp"
#include <utility>
namespace engine::networking { namespace {
class Factory final : public INetworkTransportFactory {
public:
 std::vector<TransportBackendInfo> backends() const override { return {{TransportKind::Loopback,"loopback",true},{TransportKind::Udp,"udp",false},{TransportKind::Tcp,"tcp",false}}; }
 std::unique_ptr<ITransport> create(const TransportConfig& c,std::string& e) const override { return create_transport(c,e); }
}; }
std::unique_ptr<INetworkTransportFactory> create_network_transport_factory(){return std::make_unique<Factory>();}
}

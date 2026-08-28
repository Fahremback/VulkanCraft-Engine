#include "engine/networking/INetworkServer.hpp"
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace engine::networking {
namespace {
class Server final : public INetworkServer {
public:
    bool start(const DedicatedServerConfig& config, std::string& error) override {
        if (config.server_id.empty() || config.tick_rate == 0 || config.max_clients == 0 || config.transport.endpoint.host.empty() || config.transport.endpoint.port == 0) { error="invalid_server_config"; return false; }
        transport_ = create_transport(config.transport, error);
        if (!transport_) return false;
        if (!transport_->start(config.transport, error)) return false;
        if (!transport_->connect(config.transport.endpoint, error)) return false;
        config_ = config; clients_.clear(); metrics_ = {}; return true;
    }
    bool accept_client(std::uint64_t id, std::string& error) override {
        if (!transport_ || transport_->state() != TransportState::Connected) { error="server_not_started"; return false; }
        if (id == 0 || clients_.size() >= config_.max_clients) { error="client_rejected"; return false; }
        return clients_.insert(id).second;
    }
    bool remove_client(std::uint64_t id, std::string&) override { return clients_.erase(id) != 0; }
    bool tick(std::string& error) override {
        if (!transport_ || transport_->state() != TransportState::Connected) { error="server_not_started"; return false; }
        auto messages = transport_->poll(error); (void)messages; metrics_.tick++; metrics_.clients = static_cast<std::uint32_t>(clients_.size()); metrics_.messages_received = transport_->received_count(); metrics_.messages_sent = transport_->sent_count(); return true;
    }
    bool stop(std::string&) override { if (transport_) transport_->stop(); clients_.clear(); return true; }
    ServerMetrics metrics() const override { return metrics_; }
    TransportState state() const noexcept override { return transport_ ? transport_->state() : TransportState::Stopped; }
    INetworkReplication& replication() override { return *replication_; }
    INetworkRpc& rpc() override { return *rpc_; }
    INetworkInterest& interest() override { return *interest_; }
    INetworkDiscovery& discovery() override { return *discovery_; }
private:
    DedicatedServerConfig config_;
    std::unique_ptr<ITransport> transport_;
    std::unordered_set<std::uint64_t> clients_;
    ServerMetrics metrics_;
    std::unique_ptr<INetworkReplication> replication_{create_network_replication("server", ignored_)};
    std::unique_ptr<INetworkRpc> rpc_{create_network_rpc("server", ignored_)};
    std::unique_ptr<INetworkInterest> interest_{create_network_interest("server", ignored_)};
    std::unique_ptr<INetworkDiscovery> discovery_{create_network_discovery("server", ignored_)};
    static inline std::string ignored_;
};
}
std::unique_ptr<INetworkServer> create_network_server() { return std::make_unique<Server>(); }
}

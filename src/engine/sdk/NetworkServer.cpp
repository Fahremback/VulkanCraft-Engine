// NetworkServer.cpp — the only translation unit implementing INetworkServer.
// The canonical runtime (section A): owns the real transport (per accepted
// client for UDP, a shared loopback for editor/single-player), and the
// deterministic sub-services (replication, rpc, interest, discovery, session,
// authority, prediction, security). Loopback and UDP share the exact same
// public protocol (TransportMessage with channel); the dedicated server routes
// inbound/outbound bytes through the transport, not through in-memory queues.

#include "engine/networking/INetworkServer.hpp"
#include "TransportInternal.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

class Server final : public INetworkServer {
public:
    ~Server() override { (void)stop_impl(); }

    bool start(const DedicatedServerConfig& config, std::string& error) override {
        if (config.server_id.empty() || config.tick_rate == 0 || config.max_clients == 0 ||
            config.transport.endpoint.host.empty() || config.transport.endpoint.port == 0) {
            error = "invalid_server_config";
            return false;
        }
        config_ = config;
        clients_.clear();
        perClient_.clear();
        metrics_ = {};

        // Sub-services of the canonical runtime.
        session_ = create_network_session(error);
        if (!session_) return false;
        authority_ = create_authoritative_rpc(error);
        if (!authority_) return false;
        prediction_ = create_client_prediction(error);
        if (!prediction_) return false;
        security_ = create_replication_security(config_.security, error);
        if (!security_) return false;
        replication_ = create_network_replication("server-replication", error);
        if (!replication_) return false;
        rpc_ = create_network_rpc("server-rpc", error);
        if (!rpc_) return false;
        interest_ = create_network_interest("server-interest", error);
        if (!interest_) return false;
        discovery_ = create_network_discovery("server-discovery", error);
        if (!discovery_) return false;

        session_->server_set_version(config_.version);

        baseTransport_ = create_transport(config_.transport, error);
        if (!baseTransport_) return false;
        if (!baseTransport_->start(config_.transport, error)) return false;
        if (!baseTransport_->connect(config_.transport.endpoint, error)) return false;

        startedAt_ = now_ms();
        return true;
    }

    bool accept_client(std::uint64_t id, std::string& error) override {
        if (!baseTransport_ || baseTransport_->state() != TransportState::Connected) {
            error = "server_not_started";
            return false;
        }
        if (id == 0 || clients_.size() >= config_.max_clients) { error = "client_rejected"; return false; }
        if (!clients_.insert(id).second) { error = "client_duplicate"; return false; }
        // Real per-client transport for the UDP backend.
        if (config_.transport.kind == TransportKind::Udp) {
            std::string tErr;
            auto t = create_udp_client_transport(config_.transport, tErr);
            if (!t || !t->connect(config_.transport.endpoint, tErr)) {
                clients_.erase(id);
                error = "client_transport_failed";
                return false;
            }
            t->set_peer_id(id, tErr);
            perClient_[id] = std::move(t);
        }
        return true;
    }

    bool remove_client(std::uint64_t id, std::string&) override {
        perClient_.erase(id);
        return clients_.erase(id) != 0;
    }

    bool tick(std::string& error) override {
        if (!baseTransport_) { error = "server_not_started"; return false; }
        ++metrics_.tick;
        metrics_.clients = static_cast<std::uint32_t>(clients_.size());

        const std::uint64_t now = now_ms();
        security_->advance_window(now);

        // Drain inbound from every client transport (loopback uses the shared
        // base transport; UDP uses one per accepted client).
        std::vector<std::pair<std::uint64_t, std::vector<TransportMessage>>> inbound;
        if (perClient_.empty()) {
            auto messages = baseTransport_->poll(error);
            inbound.emplace_back(0, std::move(messages));
        } else {
            for (const auto& [id, t] : perClient_) {
                auto messages = t->poll(error);
                inbound.emplace_back(id, std::move(messages));
            }
        }
        for (auto& [id, messages] : inbound) {
            (void)id;
            for (auto& m : messages) {
                if (!security_->observe_incoming(m.peer_id != 0 ? m.peer_id : 1,
                                                 m.payload.size())) {
                    metrics_.messages_received++;
                    continue;  // dropped by spam guard
                }
                metrics_.messages_received++;
            }
        }
        metrics_.messages_sent = baseTransport_->sent_count();
        return true;
    }

    bool stop(std::string& e) override { e.clear(); return stop_impl(); }

    ServerMetrics metrics() const override { return metrics_; }

    TransportState state() const noexcept override {
        return baseTransport_ ? baseTransport_->state() : TransportState::Stopped;
    }

    INetworkReplication& replication() override { return *replication_; }
    INetworkRpc& rpc() override { return *rpc_; }
    INetworkInterest& interest() override { return *interest_; }
    INetworkDiscovery& discovery() override { return *discovery_; }
    INetworkSession& session() override { return *session_; }
    IAuthoritativeRpc& authority() override { return *authority_; }
    IClientPrediction& prediction() override { return *prediction_; }
    IReplicationSecurity& security() override { return *security_; }

    bool send_to_client(std::uint64_t connection, std::uint16_t channel,
                        bool reliable, bool unordered,
                        const std::vector<std::uint8_t>& payload,
                        std::string& error) override {
        ITransport* t = nullptr;
        if (perClient_.empty()) {
            t = baseTransport_.get();
        } else {
            const auto it = perClient_.find(connection);
            if (it == perClient_.end()) { error = "unknown_client"; return false; }
            t = it->second.get();
        }
        if (t == nullptr || !t->send_channel(channel, reliable, unordered, payload, error)) {
            return false;
        }
        ++metrics_.messages_sent;
        return true;
    }

private:
    bool stop_impl() {
        if (baseTransport_) baseTransport_->stop();
        for (auto& [id, t] : perClient_) { (void)id; t->stop(); }
        perClient_.clear();
        clients_.clear();
        return true;
    }

    static std::uint64_t now_ms() {
        const auto n = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(n).count());
    }

    DedicatedServerConfig config_;
    std::unique_ptr<ITransport> baseTransport_;
    std::unordered_map<std::uint64_t, std::unique_ptr<ITransport>> perClient_;
    std::unordered_set<std::uint64_t> clients_;
    ServerMetrics metrics_;
    std::uint64_t startedAt_{ 0 };
    std::unique_ptr<INetworkSession> session_;
    std::unique_ptr<IAuthoritativeRpc> authority_;
    std::unique_ptr<IClientPrediction> prediction_;
    std::unique_ptr<IReplicationSecurity> security_;
    std::unique_ptr<INetworkReplication> replication_;
    std::unique_ptr<INetworkRpc> rpc_;
    std::unique_ptr<INetworkInterest> interest_;
    std::unique_ptr<INetworkDiscovery> discovery_;
};

}  // namespace

std::unique_ptr<INetworkServer> create_network_server() { return std::make_unique<Server>(); }

DedicatedServerConfig make_dedicated_server_config(
    const std::string& serverId, std::uint16_t port,
    std::uint32_t tickRate, std::uint32_t maxClients, bool udp) {
    DedicatedServerConfig cfg;
    cfg.server_id = serverId;
    cfg.transport.kind = udp ? TransportKind::Udp : TransportKind::Loopback;
    cfg.transport.endpoint = TransportEndpoint{ "127.0.0.1", port };
    cfg.tick_rate = tickRate;
    cfg.max_clients = maxClients;
    // Negociação de versão (A.6/D): servidor e cliente só trocam pacotes pela
    // MESMA versão canônica — todos os cinco eixos fixados no 1.
    cfg.version.protocol = 1;
    cfg.version.registries = 1;
    cfg.version.plugins = 1;
    cfg.version.schemas = 1;
    cfg.version.content = 1;
    return cfg;
}

}  // namespace networking
}  // namespace engine
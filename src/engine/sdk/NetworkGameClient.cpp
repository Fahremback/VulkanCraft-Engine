// NetworkGameClient.cpp — the only translation unit implementing
// INetworkGameClient. The game client reuses the same canonical public stack as
// the server (section A/J): real ITransport, INetworkSession identity + token,
// IClientPrediction, and the SAME IWorldReplication the server drives (client
// side). No parallel runtime. Deterministic core; the transport is the shared
// protocol.

#include "engine/networking/INetworkGameClient.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

class GameClientImpl final : public INetworkGameClient {
public:
    GameClientImpl() {
        session_ = create_network_session(ignored_);
        prediction_ = create_client_prediction(ignored_);
    }

    bool connect(const GameClientConfig& config, std::string& errorOut) override {
        config_ = config;
        transport_ = create_transport(config.transport, errorOut);
        if (!transport_) return false;
        if (!transport_->start(config.transport, errorOut)) return false;
        if (!transport_->connect(config.server, errorOut)) return false;
        transport_->set_peer_id(config_.player_id != 0 ? config_.player_id : 1, errorOut);

        // Session: handshake (mesma versão do servidor) + join (identidade).
        SessionCapabilities caps;
        const auto hs = session_->handshake(config_.player_id != 0 ? config_.player_id : 1,
                                            config.version, config_.auth_payload, caps, now_ms());
        if (!hs.ok) { errorOut = "handshake:" + hs.error; return false; }
        const auto jn = session_->join(config_.player_id != 0 ? config_.player_id : 1,
                                       config_.player_id, config_.start_world_id, 0, false, now_ms());
        if (!jn.ok || !jn.token.valid()) { errorOut = "join:" + jn.error; return false; }
        token_ = jn.token;
        connected_ = true;
        return true;
    }

    void disconnect() override {
        if (session_) session_->graceful_leave(config_.player_id != 0 ? config_.player_id : 1);
        if (transport_) transport_->stop();
        connected_ = false;
    }

    bool tick(double dt, std::string& errorOut) override {
        (void)dt;
        if (!connected_ || !transport_) { errorOut = "client_not_connected"; return false; }
        auto messages = transport_->poll(errorOut);
        incoming_count_ += messages.size();
        return true;
    }

    IClientPrediction& prediction() override { return *prediction_; }
    INetworkSession& session() override { return *session_; }

    bool send_command(const std::string& name, const std::uint8_t* payload,
                      std::size_t size, std::string& errorOut) override {
        if (!connected_) { errorOut = "client_not_connected"; return false; }
        return send_channel(0, true, false,
                            encode_command(name, payload, size), errorOut);
    }

    bool send_channel(std::uint16_t channel, bool reliable, bool unordered,
                      const std::vector<std::uint8_t>& payload,
                      std::string& errorOut) override {
        if (!connected_ || !transport_) { errorOut = "client_not_connected"; return false; }
        return transport_->send_channel(channel, reliable, unordered, payload, errorOut);
    }

    void bind_world_replication(engine::world::IWorldReplication& worldReplication) override {
        world_ = &worldReplication;
    }
    engine::world::IWorldReplication* world_replication() override { return world_; }

    bool connected() const override { return connected_; }
    const TransportEndpoint& server_endpoint() const override { return config_.server; }

private:
    std::vector<std::uint8_t> encode_command(const std::string& name,
                                             const std::uint8_t* payload, std::size_t size) const {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), payload, payload + size);
        return out;
    }

    static std::uint64_t now_ms() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count());
    }

    GameClientConfig config_;
    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<INetworkSession> session_;
    std::unique_ptr<IClientPrediction> prediction_;
    engine::world::IWorldReplication* world_{ nullptr };
    SessionToken token_;
    bool connected_{ false };
    std::size_t incoming_count_{ 0 };
    static inline std::string ignored_;
};

}  // namespace

std::unique_ptr<INetworkGameClient> create_network_game_client(std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<GameClientImpl>();
}

}  // namespace networking
}  // namespace engine
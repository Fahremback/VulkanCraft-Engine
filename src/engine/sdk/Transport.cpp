// Transport.cpp — deterministic Loopback transport + public create_transport
// dispatch. Loopback is the editor / single-player / CI backend: it shares the
// exact same public protocol (TransportMessage with channel/reliability) and
// never duplicates the wire format. The real UDP backend lives in
// UdpTransport.cpp and is selected by TransportKind::Udp.

#include "engine/networking/ITransport.hpp"
#include "TransportInternal.hpp"

#include <algorithm>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

class Loopback final : public ITransport {
    TransportConfig cfg_;
    TransportEndpoint local_;
    TransportEndpoint remote_;
    TransportState state_{ TransportState::Stopped };
    std::deque<TransportMessage> q_;
    std::uint64_t next_{ 0 };
    std::uint64_t sent_{ 0 };
    std::uint64_t received_{ 0 };
    std::uint64_t peer_{ 0 };
    std::uint64_t outboundBytes_{ 0 };

public:
    explicit Loopback(TransportConfig c) : cfg_(std::move(c)), local_(cfg_.endpoint) {}

    bool start(const TransportConfig& c, std::string& e) override {
        if (c.max_message_bytes == 0 || c.poll_budget == 0) { e = "invalid_transport_config"; return false; }
        cfg_ = c;
        local_ = c.endpoint;
        q_.clear();
        next_ = 0;
        sent_ = received_ = 0;
        outboundBytes_ = 0;
        state_ = TransportState::Listening;
        return true;
    }

    bool connect(const TransportEndpoint& e, std::string& err) override {
        if (state_ != TransportState::Listening && state_ != TransportState::Connected) {
            err = "transport_not_started";
            return false;
        }
        if (e.port == 0) { err = "invalid_endpoint"; return false; }
        remote_ = e;
        state_ = TransportState::Connected;
        return true;
    }

    bool send(const std::vector<std::uint8_t>& p, std::string& e) override {
        return send_channel(0, true, false, p, e);
    }

    bool send_unreliable(const std::vector<std::uint8_t>& p, std::string& e) override {
        return send_channel(2, false, true, p, e);
    }

    bool send_channel(std::uint16_t channel, bool reliable, bool /*unordered*/,
                      const std::vector<std::uint8_t>& p, std::string& e) override {
        if (state_ != TransportState::Connected) { e = "transport_not_connected"; return false; }
        if (p.empty() || p.size() > cfg_.max_message_bytes) { e = "invalid_payload"; return false; }
        if (outboundBytes_ > static_cast<std::uint64_t>(cfg_.max_message_bytes) * 8u) {
            e = "backpressure";
            return false;
        }
        TransportMessage msg;
        msg.sequence = next_++;
        msg.peer_id = peer_;
        msg.reliable = reliable;
        msg.channel = channel;
        msg.payload = p;
        q_.push_back(std::move(msg));
        ++sent_;
        outboundBytes_ += p.size();
        return true;
    }

    std::vector<TransportMessage> poll(std::string&) override {
        outboundBytes_ = 0;
        std::vector<TransportMessage> o;
        auto n = std::min<std::size_t>(cfg_.poll_budget, q_.size());
        while (n--) { o.push_back(std::move(q_.front())); q_.pop_front(); ++received_; }
        return o;
    }

    bool cancel(std::string&) override { outboundBytes_ = 0; q_.clear(); return true; }
    void stop() noexcept override { q_.clear(); state_ = TransportState::Stopped; }
    TransportState state() const noexcept override { return state_; }
    TransportEndpoint local_endpoint() const override { return local_; }
    TransportEndpoint remote_endpoint() const override { return remote_; }
    std::uint64_t sent_count() const noexcept override { return sent_; }
    std::uint64_t received_count() const noexcept override { return received_; }
    bool set_peer_id(std::uint64_t id, std::string& e) override {
        if (id == 0) { e = "invalid_peer_id"; return false; }
        peer_ = id;
        return true;
    }
    std::uint64_t peer_id() const noexcept override { return peer_; }
};

}  // namespace

// Loopback factory declared in TransportInternal.hpp; used by create_transport.
std::unique_ptr<ITransport> create_loopback_transport(const TransportConfig& c) {
    return std::make_unique<Loopback>(c);
}

std::unique_ptr<ITransport> create_transport(const TransportConfig& c, std::string& e) {
    if (c.endpoint.host.empty() || c.endpoint.port == 0) { e = "invalid_endpoint"; return {}; }
    if (c.max_message_bytes == 0 || c.poll_budget == 0) { e = "invalid_transport_config"; return {}; }
    switch (c.kind) {
        case TransportKind::Loopback: e.clear(); return create_loopback_transport(c);
        case TransportKind::Udp: e.clear(); return create_udp_transport(c, e);
        case TransportKind::Tcp: e = "transport_backend_unavailable"; return {};
    }
    e = "transport_backend_unavailable";
    return {};
}

}  // namespace networking
}  // namespace engine
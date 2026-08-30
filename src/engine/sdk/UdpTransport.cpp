// UdpTransport.cpp — REAL UDP transport backend for the public ITransport.
//
// Canonical stack section B: a genuine UDP provider over actual sockets. It
// wraps the proven reliable-ordered engine transport (Engine::Networking::
// ReliableTransport, built on SocketTransport real UDP sockets) and adds the
// canonical three-channel surface the networking domain requires:
//   channel 0  reliable ordered     — commands, authoritative edits, critical state
//   channel 1  reliable unordered   — latest-wins (transforms, overwrite state)
//   channel 2  unreliable sequenced — frequent snapshots, drop-on-loss
//
// The provider is a SINGLE-PEER transport (one ReliableTransport underneath);
// the dedicated server owns one such transport per accepted client (a common
// engine pattern), so multi-client authority lives in the INetworkServer while
// every individual connection rides real UDP. The editor / single-player path
// keeps the deterministic Loopback transport and shares the exact same public
// protocol (no duplicated protocol — TransportMessage carries channel/reliable).
//
// Real-socket truths applied here: packet framing, MTU fragmentation /
// reassembly and RLE compression come from ReliableTransport; heartbeat,
// timeout, retransmit-with-jitter and backpressure come from the same; channel
// tagging + latest-wins dedup and a hard outbound rate cap live here.

#include "engine/networking/ITransport.hpp"
// Same engine/src/engine depth as this file's dir (sdk/), so the relative
// include reaches the parallel-but-harnessed reliable socket transport.
#include "../networking/ReliableTransport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

using Reliable = ::Engine::Networking::ReliableTransport;

// First byte of every application payload tags which canonical channel a
// message belongs to, so the receiver can expose channel/reliability without
// duplicating protocol state.
enum EnvelopeChannel : std::uint8_t {
    kChannelReliableOrdered = 0,
    kChannelReliableUnordered = 1,
    kChannelUnreliableSequenced = 2,
};

void put_u32le(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

class UdpTransport final : public ITransport {
public:
    explicit UdpTransport(TransportConfig config) : config_(std::move(config)) {}

    bool start(const TransportConfig& c, std::string& error) override {
        if (c.endpoint.host.empty() || c.endpoint.port == 0 ||
            c.max_message_bytes == 0 || c.poll_budget == 0) {
            error = "invalid_transport_config";
            return false;
        }
        config_ = c;
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            ready_.clear();
        }
        sent_ = received_ = 0;
        deliveredSeq_ = 0;
        unorderedSeq_ = 0;
        local_ = config_.endpoint;
        reliable_ = std::make_unique<Reliable>(Reliable::Config{});
        reliable_->set_message_handler([this](const std::byte* data, std::size_t size) {
            on_message(reinterpret_cast<const std::uint8_t*>(data), size);
        });
        if (!reliable_->listen(config_.endpoint.port)) {
            error = "udp_bind_failed";
            return false;
        }
        listening_ = true;
        state_ = TransportState::Listening;
        return true;
    }

    bool connect(const TransportEndpoint& e, std::string& err) override {
        if (e.host.empty() || e.port == 0) { err = "invalid_endpoint"; return false; }
        remote_ = e;
        // Client path: connect without an explicit listen() — ReliableTransport
        // binds an ephemeral local socket itself and handshakes outward.
        if (!reliable_) {
            config_.endpoint = e;
            reliable_ = std::make_unique<Reliable>(Reliable::Config{});
            reliable_->set_message_handler([this](const std::byte* data, std::size_t size) {
                on_message(reinterpret_cast<const std::uint8_t*>(data), size);
            });
        }
        if (!reliable_->connect(e.host, e.port)) { err = "udp_connect_failed"; return false; }
        Reliable::Status status = reliable_->status();
        const auto begin = std::chrono::steady_clock::now();
        while (status == Reliable::Status::Handshaking) {
            reliable_->update(std::chrono::steady_clock::now());
            status = reliable_->status();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin).count() > 4000) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (status != Reliable::Status::Connected) { err = "udp_handshake_failed"; return false; }
        if (peer_ == 0) peer_ = 1;  // client single peer
        state_ = TransportState::Connected;
        return true;
    }

    bool send(const std::vector<std::uint8_t>& p, std::string& e) override {
        return send_channel(kChannelReliableOrdered, true, false, p, e);
    }

    bool send_unreliable(const std::vector<std::uint8_t>& p, std::string& e) override {
        return send_channel(kChannelUnreliableSequenced, false, true, p, e);
    }

    bool send_channel(std::uint16_t channel, bool reliable, bool /*unordered*/,
                      const std::vector<std::uint8_t>& p, std::string& e) override {
        (void)reliable;
        if (!reliable_ || reliable_->status() != Reliable::Status::Connected) {
            e = "transport_not_connected";
            return false;
        }
        if (p.empty() || p.size() > config_.max_message_bytes) { e = "invalid_payload"; return false; }
        // Hard outbound cap so a peer that outruns the network cannot grow an
        // unbounded reliable queue (rate limiting / backpressure). We track
        // our own outbound byte counter because ReliableTransport::sent_bytes()
        // counts frames (headers+overhead), not application messages.
        if (outboundBytes_ > static_cast<std::uint64_t>(config_.max_message_bytes) * 8u) {
            e = "backpressure";
            return false;
        }
        std::vector<std::uint8_t> framed;
        framed.reserve(p.size() + 5);
        std::uint8_t tag = kChannelReliableOrdered;
        if (channel == 2) {
            tag = kChannelUnreliableSequenced;
        } else if (channel == 1) {
            tag = kChannelReliableUnordered;
        }
        framed.push_back(tag);
        if (tag == kChannelReliableUnordered) {
            put_u32le(framed, ++unorderedSeq_);
        }
        framed.insert(framed.end(), p.begin(), p.end());
        const auto* raw = reinterpret_cast<const std::byte*>(framed.data());
        const bool ok = (tag == kChannelUnreliableSequenced)
                            ? reliable_->send_unreliable(raw, framed.size())
                            : reliable_->send(raw, framed.size());
        if (ok) { ++sent_; outboundBytes_ += framed.size(); }
        else e = "send_failed";
        return ok;
    }

    std::vector<TransportMessage> poll(std::string&) override {
        std::vector<TransportMessage> out;
        if (!reliable_) return out;
        // Rate-limit window: bytes sent since the previous poll gate the
        // next burst, so sustained abuse is capped per maintenance step.
        outboundBytes_ = 0;
        // Drive heartbeat / retransmission / timeout each poll.
        reliable_->update(std::chrono::steady_clock::now());
        std::deque<std::vector<std::uint8_t>> incoming;
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            incoming.swap(ready_);
        }
        for (auto& payload : incoming) {
            TransportMessage msg;
            msg.peer_id = peer_ != 0 ? peer_ : 1;
            msg.sequence = ++deliveredSeq_;
            decode_payload(payload, msg);
            out.push_back(std::move(msg));
            ++received_;
            if (out.size() >= config_.poll_budget) break;
        }
        return out;
    }

    bool cancel(std::string&) override {
        std::lock_guard<std::mutex> lock(readyMutex_);
        ready_.clear();
        return true;
    }

    void stop() noexcept override {
        if (reliable_) reliable_->close();
        state_ = TransportState::Stopped;
    }

    TransportState state() const noexcept override {
        if (!reliable_) return TransportState::Stopped;
        if (state_ == TransportState::Stopped) return TransportState::Stopped;
        switch (reliable_->status()) {
            case Reliable::Status::Connected: return TransportState::Connected;
            case Reliable::Status::TimedOut:  return TransportState::Failed;
            case Reliable::Status::Handshaking: return TransportState::Connected;
            default:
                // Disconnected but bound (server listening) => Listening.
                return (listening_) ? TransportState::Listening : TransportState::Stopped;
        }
    }

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

private:
    void on_message(const std::uint8_t* data, std::size_t size) {
        if (size == 0) return;
        std::lock_guard<std::mutex> lock(readyMutex_);
        ready_.push_back(std::vector<std::uint8_t>(data, data + size));
        // Sanity bound against an inbound flood before dereference/poll.
        const std::size_t cap = std::max<std::size_t>(
            static_cast<std::size_t>(config_.max_message_bytes) * 2u, 4096u);
        while (ready_.size() > cap) ready_.pop_back();
    }

    void decode_payload(const std::vector<std::uint8_t>& payload, TransportMessage& msg) const {
        if (payload.empty()) { msg.reliable = true; return; }
        const std::uint8_t tag = payload[0];
        msg.reliable = true;
        msg.unordered = false;
        if (tag == kChannelReliableUnordered) {
            msg.channel = 1;
            msg.unordered = true;
            if (payload.size() < 5) { msg.payload.assign(payload.begin() + 1, payload.end()); return; }
            std::uint32_t seq = 0;
            seq |= static_cast<std::uint32_t>(payload[1]);
            seq |= static_cast<std::uint32_t>(payload[2]) << 8;
            seq |= static_cast<std::uint32_t>(payload[3]) << 16;
            seq |= static_cast<std::uint32_t>(payload[4]) << 24;
            // ReliableTransport already delivers in order, so a repeated seq is
            // always stale re-delivery: keep latest and drop older.
            if (lastUnordered_ != 0 && seq <= lastUnordered_) return;
            lastUnordered_ = seq;
            msg.payload.assign(payload.begin() + 5, payload.end());
        } else if (tag == kChannelUnreliableSequenced) {
            msg.channel = 2;
            msg.reliable = false;
            msg.payload.assign(payload.begin() + 1, payload.end());
        } else {
            msg.channel = 0;
            msg.payload.assign(payload.begin() + 1, payload.end());
        }
    }

    TransportConfig config_;
    std::unique_ptr<Reliable> reliable_;
    TransportEndpoint local_;
    TransportEndpoint remote_;
    TransportState state_{ TransportState::Stopped };
    std::uint64_t peer_{ 0 };
    std::uint64_t sent_{ 0 };
    std::uint64_t received_{ 0 };
    std::uint64_t deliveredSeq_{ 0 };
    std::uint64_t outboundBytes_{ 0 };
    std::uint32_t unorderedSeq_{ 0 };
    mutable std::uint32_t lastUnordered_{ 0 };
    bool listening_{ false };
    mutable std::mutex readyMutex_;
    std::deque<std::vector<std::uint8_t>> ready_;

public:
    void inject_inbound(const std::uint8_t* data, std::size_t size) { on_message(data, size); }
};

}  // namespace

#include "TransportInternal.hpp"

std::unique_ptr<ITransport> create_udp_transport(const TransportConfig& c,
                                                 std::string& e) {
    if (c.endpoint.host.empty() || c.endpoint.port == 0) { e = "invalid_endpoint"; return {}; }
    return std::make_unique<UdpTransport>(c);
}

// Client-side UDP transport (dedicated server opens one per accepted client; a
// real listener may also be used for inbound). Shares the same UdpTransport.
std::unique_ptr<ITransport> create_udp_client_transport(const TransportConfig& c,
                                                        std::string& e) {
    if (c.endpoint.host.empty() || c.endpoint.port == 0) { e = "invalid_endpoint"; return {}; }
    return std::make_unique<UdpTransport>(c);
}

}  // namespace networking
}  // namespace engine
#pragma once

#include "SocketTransport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Networking {

// Reliable, ordered transport on top of an unreliable UDP socket
// (README §35 "networking with a real transport"):
//   • 32-bit sequence numbers + cumulative ACKs
//   • Selective retransmission with timeout and max-retry
//   • In-order delivery with gap handling (reorder buffer)
//   • Message fragmentation/defragmentation for payloads > MTU
//   • Lightweight compression (run-length on zero runs)
//   • Connection handshake (SYN/ACK), heartbeat keepalive, timeout
//
// The transport is endpoint-agnostic: the same class runs on both client and
// server sides, identified by a peer address (UDP is connectionless, so the
// "connection" is the peer address + our local socket).
class ReliableTransport final {
public:
    enum class Status { Disconnected, Handshaking, Connected, TimedOut };

    struct Config {
        std::chrono::milliseconds retransmitTimeout{100};
        std::chrono::milliseconds heartbeatInterval{250};
        std::chrono::milliseconds timeoutAfter{3000};
        std::uint32_t maxRetransmits{5};
        std::size_t maxPayload{1200};      // fragment size (MTU budget)
        std::size_t receiveBuffer{8192};   // reorder window
    };

    // Handles a fully reassembled, ordered message (after decompression).
    using MessageHandler = std::function<void(const std::byte* data, std::size_t size)>;
    using StatusHandler = std::function<void(Status)>;

    explicit ReliableTransport(Config config = {});
    ~ReliableTransport();

    // Binds a local UDP socket (0 = ephemeral) and marks us listening.
    // Returns false if the socket cannot be bound.
    [[nodiscard]] bool listen(std::uint16_t port = 0);

    // Connects to a peer (sends SYN). `port` is used when `listen()` was not
    // called with an explicit port (ephemeral).
    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port);

    void set_message_handler(MessageHandler handler) { messageHandler_ = std::move(handler); }
    void set_status_handler(StatusHandler handler) { statusHandler_ = std::move(handler); }

    // Sends a message reliably (fragmented if needed). Returns false when not
    // connected. Reliable semantics: queued and retransmitted until ACKed.
    [[nodiscard]] bool send(const std::byte* data, std::size_t size);

    // Unreliable side channel (e.g. snapshots) — sent as-is without sequencing.
    [[nodiscard]] bool send_unreliable(const std::byte* data, std::size_t size);

    // Must be called periodically; drives retransmission, heartbeats, timeouts
    // and pumps incoming datagrams. `now` in steady_clock time.
    void update(std::chrono::steady_clock::time_point now);

    [[nodiscard]] Status status() const;
    [[nodiscard]] bool connected() const noexcept { return status_ == Status::Connected; }
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] std::uint32_t sent_bytes() const noexcept { return sentBytes_; }
    [[nodiscard]] std::uint32_t received_bytes() const noexcept { return receivedBytes_; }
    [[nodiscard]] std::uint32_t retransmits() const noexcept { return retransmits_; }
    [[nodiscard]] std::uint32_t dropped_fragments() const noexcept { return droppedFragments_; }

    void disconnect();
    // disconnect() + tears down the local socket and receive thread — the
    // peer effectively goes silent (used to simulate a peer that stops
    // responding, e.g. the timeout test).
    void close();

    [[nodiscard]] static std::size_t compress(const std::byte* in, std::size_t size,
                                              std::byte* out, std::size_t capacity);
    [[nodiscard]] static std::size_t decompress(const std::byte* in, std::size_t size,
                                                std::byte* out, std::size_t capacity);

private:
    struct PacketHeader {
        std::uint32_t seq{};
        std::uint32_t ack{};
        std::uint16_t flags{};
        std::uint16_t fragmentIndex{};
        std::uint16_t fragmentCount{};
        std::uint16_t payloadSize{};   // bytes in this fragment (uncompressed)
    };
    static_assert(sizeof(PacketHeader) == 16); // 4+4+2+2+2+2, no padding

    enum Flags : std::uint16_t {
        FLAG_SYN = 1u << 0,
        FLAG_ACK = 1u << 1,
        FLAG_DATA = 1u << 2,
        FLAG_HEARTBEAT = 1u << 3,
        FLAG_COMPRESSED = 1u << 4,
    };

    struct OutgoingPacket {
        std::uint32_t seq{};
        std::vector<std::byte> payload;   // single fragment payload (pre-compression)
        std::uint16_t fragmentIndex{};    // kept so retransmits reassemble correctly
        std::uint16_t fragmentCount{};
        std::chrono::steady_clock::time_point lastSent{};
        std::uint32_t retries{};
        // Randomized extra delay before the next retry (RFC-6298-style
        // jitter). A deterministic retry schedule can phase-lock with a
        // deterministic loss pattern, letting the same packet be dropped
        // round after round while its neighbors sail through.
        std::uint32_t retryJitterMs{};
        bool acked{false};
    };

    struct Fragment {
        std::uint16_t index{};
        std::vector<std::byte> data;
    };

    struct PartialMessage {
        std::uint16_t count{};
        std::vector<Fragment> fragments;
        std::chrono::steady_clock::time_point received{};
    };

    void send_packet(const PacketHeader& header, const std::byte* payload, std::size_t payloadSize);
    void handle_datagram(const std::byte* data, std::size_t size,
                         std::chrono::steady_clock::time_point now);
    void process_data(const PacketHeader& header, const std::byte* payload, std::size_t size,
                      std::chrono::steady_clock::time_point now);
    // Delivers every complete, contiguous unit starting at nextInSeq_ (single-
    // fragment messages from reorderBuffer_, completed partials from
    // partials_) — the reliable-ordered guarantee under loss/reorder.
    void deliver_ordered();
    void deliver_message(std::vector<std::byte> message);
    void retransmit(std::chrono::steady_clock::time_point now);
    void send_heartbeat(std::chrono::steady_clock::time_point now);
    void check_timeout(std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::uint32_t next_sequence() noexcept { return nextOutSeq_++; }

    Config config_;
    SocketTransport socket_;
    Status status_{Status::Disconnected};

    std::uint32_t nextOutSeq_{1};
    std::uint32_t nextInSeq_{0};
    std::uint32_t lastAcked_{0};
    std::uint32_t sentBytes_{0};
    std::uint32_t receivedBytes_{0};
    std::uint32_t retransmits_{0};
    std::uint32_t droppedFragments_{0};

    mutable std::mutex mutex_;
    std::deque<OutgoingPacket> sendQueue_;
    std::map<std::uint32_t, std::vector<std::byte>> reorderBuffer_;  // seq -> payload
    std::map<std::uint32_t, PartialMessage> partials_;               // first seq -> parts
    std::unordered_map<std::uint32_t, std::vector<std::byte>> ackedSeen_;

    std::chrono::steady_clock::time_point lastSend_{};
    std::chrono::steady_clock::time_point lastReceive_{};
    std::chrono::steady_clock::time_point lastHeartbeat_{};
    bool handshakeSent_{false};
    std::uint32_t synRetries_{0};  // handshake SYN retries (loss-resilient connect)
    std::string peerTarget_;  // "ip:port" of the peer (server side, learned from SYN)

    MessageHandler messageHandler_;
    StatusHandler statusHandler_;

    // Thread-safe RNG for jitter calculation (replaces std::rand()).
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace Engine::Networking

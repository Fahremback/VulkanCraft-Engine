#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Networking {

// A datagram received from the network.
struct Datagram {
    std::vector<std::byte> payload;
    std::string peer;              // "ip:port"
    double receivedTime{};         // seconds since start (wall clock at receive)
};

enum class SocketKind { Udp, Tcp };

// Real socket transport: wraps Winsock on Windows and POSIX sockets elsewhere.
// Server: listen on a port, accept UDP peers (or TCP connections), deliver
// datagrams. Client: connect to an endpoint and send/receive.
class SocketTransport final {
public:
    using ReceiveCallback = std::function<void(Datagram)>;

    SocketTransport() = default;
    ~SocketTransport();

    SocketTransport(const SocketTransport&) = delete;
    SocketTransport& operator=(const SocketTransport&) = delete;

    // Server mode. Returns false on bind failure.
    bool listen(uint16_t port, SocketKind kind = SocketKind::Udp);
    // Client mode (UDP is connectionless: just targets an endpoint).
    bool connect(const std::string& host, uint16_t port, SocketKind kind = SocketKind::Udp);
    // Starts a background receive thread; callback invoked per datagram.
    bool start_receive(ReceiveCallback callback);
    void stop_receive();

    bool send_to(const std::string& peer, const std::byte* data, std::size_t size);
    bool send(const std::byte* data, std::size_t size); // client mode target

    // Poll one datagram (non-blocking). Returns nullopt if none available.
    // Use when not using the receive thread.
    std::optional<Datagram> poll();

    void close();
    [[nodiscard]] bool is_open() const noexcept { return socket_ != kInvalid; }
    [[nodiscard]] uint16_t local_port() const noexcept { return localPort_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return lastError_; }

    // Wall-clock seconds used for timeouts and RTT.
    static double now_seconds();

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t{0};
    uintptr_t socket_{kInvalid};
    uint16_t localPort_{0};
    bool isServer_{false};
    SocketKind kind_{SocketKind::Udp};
    std::string clientTarget_;
    ReceiveCallback callback_;
    // Read by the receive loop thread and written by whichever thread calls
    // start_receive()/stop_receive(). A plain bool read concurrently with a
    // write is undefined behavior (formal data race). Make it atomic so the
    // loop can safely observe a stop without instrumentation reporting a race.
    std::atomic_bool receiving_{false};
    void* receiveThread_{nullptr}; // std::thread* (kept opaque to avoid header deps)
    std::string lastError_;

    // TCP is length-framed (u32 network byte order) so the public Datagram
    // abstraction keeps message boundaries over a byte stream.  Server mode
    // owns one listening socket plus accepted peers; client mode uses socket_
    // directly.  Queued bytes are bounded to provide explicit backpressure.
    struct TcpPeerState {
        std::string endpoint;
        std::vector<std::byte> receive;
        std::deque<std::vector<std::byte>> pending;
        std::size_t pending_offset{0};
        std::size_t queued_bytes{0};
    };
    std::unordered_map<uintptr_t, TcpPeerState> tcpPeers_;
    mutable std::mutex tcpMutex_;
    static constexpr std::size_t kTcpMaxFrame = 4u * 1024u * 1024u;
    static constexpr std::size_t kTcpMaxQueuedBytes = 8u * 1024u * 1024u;

    void run_receive_loop();
    bool flush_tcp_locked(uintptr_t socket, TcpPeerState& peer);
    bool queue_tcp_locked(uintptr_t socket, TcpPeerState& peer,
                          const std::byte* data, std::size_t size);
    void accept_tcp_locked();
    void close_tcp_peer_locked(uintptr_t socket);
};

} // namespace Engine::Networking

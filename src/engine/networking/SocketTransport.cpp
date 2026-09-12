#include "SocketTransport.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SOCKET_T = SOCKET;
constexpr SOCKET_T kBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
using SOCKET_T = int;
constexpr SOCKET_T kBadSocket = -1;
#define SOCKET_ERROR (-1)
#endif

namespace Engine::Networking {
namespace {

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit() { WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa); }
    ~WinsockInit() { WSACleanup(); }
};
#endif

void close_native(SOCKET_T socket) {
    if (socket == kBadSocket) return;
#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

bool set_nonblocking(SOCKET_T socket) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int native_error() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool would_block(int error) {
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

std::string error_text(const char* operation, int error) {
    return std::string(operation) + " failed (socket_error=" + std::to_string(error) + ")";
}

std::string endpoint_of(const sockaddr_in& address) {
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip))) return {};
    return std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
}

bool resolve_ipv4(const std::string& host, uint16_t port, int socketType,
                  sockaddr_in& out, std::string& error) {
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = socketType;
    addrinfo* results = nullptr;
    char service[16]{};
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));
    const int rc = getaddrinfo(host.c_str(), service, &hints, &results);
    if (rc != 0 || results == nullptr) {
        error = "resolve failed for '" + host + "'";
        if (results) freeaddrinfo(results);
        return false;
    }
    std::memcpy(&out, results->ai_addr, sizeof(out));
    freeaddrinfo(results);
    return true;
}

int send_flags() {
    int flags = 0;
#if defined(__linux__)
    flags |= MSG_NOSIGNAL;
#endif
    return flags;
}

} // namespace

double SocketTransport::now_seconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

SocketTransport::~SocketTransport() {
    stop_receive();
    close();
}

bool SocketTransport::listen(uint16_t port, SocketKind kind) {
#if defined(_WIN32)
    static WinsockInit init;
#endif
    close();
    lastError_.clear();
    kind_ = kind;
    const int type = kind == SocketKind::Udp ? SOCK_DGRAM : SOCK_STREAM;
    const SOCKET_T native = ::socket(AF_INET, type, 0);
    if (native == kBadSocket) {
        lastError_ = error_text("socket", native_error());
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(native, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(native, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        lastError_ = error_text("bind", native_error());
        close_native(native);
        return false;
    }
    if (kind == SocketKind::Tcp && ::listen(native, 64) == SOCKET_ERROR) {
        lastError_ = error_text("listen", native_error());
        close_native(native);
        return false;
    }
    if (!set_nonblocking(native)) {
        lastError_ = error_text("nonblocking", native_error());
        close_native(native);
        return false;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int boundLength = sizeof(bound);
#else
    socklen_t boundLength = sizeof(bound);
#endif
    localPort_ = (::getsockname(native, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0)
        ? ntohs(bound.sin_port) : port;
    socket_ = static_cast<uintptr_t>(native);
    isServer_ = true;
    clientTarget_.clear();
    return true;
}

bool SocketTransport::connect(const std::string& host, uint16_t port, SocketKind kind) {
#if defined(_WIN32)
    static WinsockInit init;
#endif
    close();
    lastError_.clear();
    kind_ = kind;
    const int type = kind == SocketKind::Udp ? SOCK_DGRAM : SOCK_STREAM;
    const SOCKET_T native = ::socket(AF_INET, type, 0);
    if (native == kBadSocket) {
        lastError_ = error_text("socket", native_error());
        return false;
    }
    sockaddr_in remote{};
    if (!resolve_ipv4(host, port, type, remote, lastError_)) {
        close_native(native);
        return false;
    }
    if (kind == SocketKind::Tcp) {
        if (::connect(native, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR) {
            lastError_ = error_text("connect", native_error());
            close_native(native);
            return false;
        }
    } else {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = 0;
        if (::bind(native, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            lastError_ = error_text("bind", native_error());
            close_native(native);
            return false;
        }
    }
    if (!set_nonblocking(native)) {
        lastError_ = error_text("nonblocking", native_error());
        close_native(native);
        return false;
    }
    sockaddr_in local{};
#if defined(_WIN32)
    int localLength = sizeof(local);
#else
    socklen_t localLength = sizeof(local);
#endif
    localPort_ = (::getsockname(native, reinterpret_cast<sockaddr*>(&local), &localLength) == 0)
        ? ntohs(local.sin_port) : 0;
    socket_ = static_cast<uintptr_t>(native);
    isServer_ = false;
    clientTarget_ = endpoint_of(remote);
    if (kind == SocketKind::Tcp) {
        std::lock_guard<std::mutex> lock(tcpMutex_);
        tcpPeers_.emplace(socket_, TcpPeerState{clientTarget_});
    }
    return true;
}

bool SocketTransport::start_receive(ReceiveCallback callback) {
    if (socket_ == kInvalid || receiving_.load(std::memory_order_acquire) || !callback) {
        lastError_ = "receive start refused: socket closed, already receiving, or callback missing";
        return false;
    }
    callback_ = std::move(callback);
    receiving_.store(true, std::memory_order_release);
    receiveThread_ = new std::thread([this] { run_receive_loop(); });
    return true;
}

void SocketTransport::run_receive_loop() {
    while (receiving_.load(std::memory_order_acquire)) {
        auto datagram = poll();
        if (datagram) {
            if (callback_) callback_(std::move(*datagram));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void SocketTransport::stop_receive() {
    receiving_.store(false, std::memory_order_release);
    if (!receiveThread_) return;
    auto* thread = static_cast<std::thread*>(receiveThread_);
    if (thread->joinable()) thread->join();
    delete thread;
    receiveThread_ = nullptr;
}

bool SocketTransport::queue_tcp_locked(uintptr_t socket, TcpPeerState& peer,
                                       const std::byte* data, std::size_t size) {
    (void)socket;
    if ((data == nullptr && size != 0) || size > kTcpMaxFrame) {
        lastError_ = "tcp frame invalid or exceeds maximum";
        return false;
    }
    if (peer.queued_bytes + size + sizeof(std::uint32_t) > kTcpMaxQueuedBytes) {
        lastError_ = "tcp backpressure: pending queue limit exceeded";
        return false;
    }
    std::vector<std::byte> frame(sizeof(std::uint32_t) + size);
    const std::uint32_t encoded = htonl(static_cast<std::uint32_t>(size));
    std::memcpy(frame.data(), &encoded, sizeof(encoded));
    if (size != 0) std::memcpy(frame.data() + sizeof(encoded), data, size);
    peer.queued_bytes += frame.size();
    peer.pending.push_back(std::move(frame));
    return true;
}

bool SocketTransport::flush_tcp_locked(uintptr_t socket, TcpPeerState& peer) {
    while (!peer.pending.empty()) {
        auto& frame = peer.pending.front();
        const std::size_t remaining = frame.size() - peer.pending_offset;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        const int sent = static_cast<int>(::send(
            static_cast<SOCKET_T>(socket),
            reinterpret_cast<const char*>(frame.data() + peer.pending_offset),
            static_cast<int>(chunk), send_flags()));
        if (sent > 0) {
            peer.pending_offset += static_cast<std::size_t>(sent);
            if (peer.pending_offset == frame.size()) {
                peer.queued_bytes -= frame.size();
                peer.pending.pop_front();
                peer.pending_offset = 0;
            }
            continue;
        }
        if (sent == 0) {
            lastError_ = "tcp send returned closed connection";
            return false;
        }
        const int error = native_error();
        if (would_block(error)) return true;
        lastError_ = error_text("send", error);
        return false;
    }
    return true;
}

void SocketTransport::accept_tcp_locked() {
    if (!isServer_ || kind_ != SocketKind::Tcp || socket_ == kInvalid) return;
    for (;;) {
        sockaddr_in remote{};
#if defined(_WIN32)
        int remoteLength = sizeof(remote);
#else
        socklen_t remoteLength = sizeof(remote);
#endif
        const SOCKET_T accepted = ::accept(static_cast<SOCKET_T>(socket_),
                                           reinterpret_cast<sockaddr*>(&remote), &remoteLength);
        if (accepted == kBadSocket) {
            const int error = native_error();
            if (!would_block(error)) lastError_ = error_text("accept", error);
            return;
        }
        if (!set_nonblocking(accepted)) {
            lastError_ = error_text("accept nonblocking", native_error());
            close_native(accepted);
            continue;
        }
        const uintptr_t handle = static_cast<uintptr_t>(accepted);
        tcpPeers_[handle] = TcpPeerState{endpoint_of(remote)};
    }
}

void SocketTransport::close_tcp_peer_locked(uintptr_t socket) {
    tcpPeers_.erase(socket);
    close_native(static_cast<SOCKET_T>(socket));
    if (!isServer_ && socket_ == socket) {
        socket_ = kInvalid;
        clientTarget_.clear();
        localPort_ = 0;
    }
}

bool SocketTransport::send_to(const std::string& peer, const std::byte* data, std::size_t size) {
    if (socket_ == kInvalid) {
        lastError_ = "send_to on closed socket";
        return false;
    }
    if (kind_ == SocketKind::Tcp) {
        std::lock_guard<std::mutex> lock(tcpMutex_);
        if (!isServer_) {
            auto found = tcpPeers_.find(socket_);
            if (found == tcpPeers_.end() || (!peer.empty() && peer != found->second.endpoint)) {
                lastError_ = "tcp peer not connected";
                return false;
            }
            return queue_tcp_locked(socket_, found->second, data, size) &&
                   flush_tcp_locked(socket_, found->second);
        }
        for (auto& [handle, state] : tcpPeers_) {
            if (state.endpoint != peer) continue;
            return queue_tcp_locked(handle, state, data, size) && flush_tcp_locked(handle, state);
        }
        lastError_ = "tcp peer not found: " + peer;
        return false;
    }

    const std::size_t colon = peer.rfind(':');
    if (colon == std::string::npos) {
        lastError_ = "invalid peer endpoint";
        return false;
    }
    unsigned long parsedPort = 0;
    try { parsedPort = std::stoul(peer.substr(colon + 1)); }
    catch (...) { lastError_ = "invalid peer port"; return false; }
    if (parsedPort > 65535u) { lastError_ = "invalid peer port"; return false; }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(static_cast<uint16_t>(parsedPort));
    if (inet_pton(AF_INET, peer.substr(0, colon).c_str(), &remote.sin_addr) != 1) {
        lastError_ = "invalid peer address";
        return false;
    }
    if (size > 65507 || (data == nullptr && size != 0)) {
        lastError_ = "udp datagram invalid or too large";
        return false;
    }
    const int sent = static_cast<int>(::sendto(
        static_cast<SOCKET_T>(socket_), reinterpret_cast<const char*>(data),
        static_cast<int>(size), send_flags(), reinterpret_cast<sockaddr*>(&remote), sizeof(remote)));
    if (sent < 0) {
        const int error = native_error();
        lastError_ = would_block(error) ? "udp backpressure" : error_text("sendto", error);
        return false;
    }
    return static_cast<std::size_t>(sent) == size;
}

bool SocketTransport::send(const std::byte* data, std::size_t size) {
    if (socket_ == kInvalid || isServer_) {
        lastError_ = "send requires a connected client socket";
        return false;
    }
    if (kind_ == SocketKind::Tcp) {
        std::lock_guard<std::mutex> lock(tcpMutex_);
        auto found = tcpPeers_.find(socket_);
        if (found == tcpPeers_.end()) { lastError_ = "tcp client state missing"; return false; }
        return queue_tcp_locked(socket_, found->second, data, size) &&
               flush_tcp_locked(socket_, found->second);
    }
    return send_to(clientTarget_, data, size);
}

std::optional<Datagram> SocketTransport::poll() {
    if (socket_ == kInvalid) return std::nullopt;
    if (kind_ == SocketKind::Udp) {
        std::byte buffer[65536];
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = sizeof(from);
#else
        socklen_t fromLength = sizeof(from);
#endif
        const int received = static_cast<int>(::recvfrom(
            static_cast<SOCKET_T>(socket_), reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLength));
        if (received < 0) {
            const int error = native_error();
            if (!would_block(error)) lastError_ = error_text("recvfrom", error);
            return std::nullopt;
        }
        Datagram datagram;
        datagram.payload.assign(buffer, buffer + received);
        datagram.peer = endpoint_of(from);
        datagram.receivedTime = now_seconds();
        return datagram;
    }

    std::lock_guard<std::mutex> lock(tcpMutex_);
    accept_tcp_locked();
    std::vector<uintptr_t> handles;
    handles.reserve(tcpPeers_.size());
    for (const auto& [handle, state] : tcpPeers_) { (void)state; handles.push_back(handle); }
    for (const uintptr_t handle : handles) {
        auto found = tcpPeers_.find(handle);
        if (found == tcpPeers_.end()) continue;
        TcpPeerState& state = found->second;
        if (!flush_tcp_locked(handle, state)) {
            close_tcp_peer_locked(handle);
            continue;
        }
        std::byte buffer[64 * 1024];
        const int received = static_cast<int>(::recv(
            static_cast<SOCKET_T>(handle), reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
        if (received > 0) {
            state.receive.insert(state.receive.end(), buffer, buffer + received);
        } else if (received == 0) {
            close_tcp_peer_locked(handle);
            continue;
        } else {
            const int error = native_error();
            if (!would_block(error)) {
                lastError_ = error_text("recv", error);
                close_tcp_peer_locked(handle);
                continue;
            }
        }
        if (state.receive.size() < sizeof(std::uint32_t)) continue;
        std::uint32_t encoded = 0;
        std::memcpy(&encoded, state.receive.data(), sizeof(encoded));
        const std::size_t frameSize = ntohl(encoded);
        if (frameSize > kTcpMaxFrame) {
            lastError_ = "tcp frame exceeds maximum";
            close_tcp_peer_locked(handle);
            continue;
        }
        const std::size_t total = sizeof(std::uint32_t) + frameSize;
        if (state.receive.size() < total) continue;
        Datagram datagram;
        datagram.payload.assign(state.receive.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)),
                                state.receive.begin() + static_cast<std::ptrdiff_t>(total));
        datagram.peer = state.endpoint;
        datagram.receivedTime = now_seconds();
        state.receive.erase(state.receive.begin(),
                            state.receive.begin() + static_cast<std::ptrdiff_t>(total));
        return datagram;
    }
    return std::nullopt;
}

void SocketTransport::close() {
    std::lock_guard<std::mutex> lock(tcpMutex_);
    for (const auto& [handle, state] : tcpPeers_) {
        (void)state;
        if (handle != socket_ || isServer_) close_native(static_cast<SOCKET_T>(handle));
    }
    tcpPeers_.clear();
    if (socket_ != kInvalid) {
        close_native(static_cast<SOCKET_T>(socket_));
        socket_ = kInvalid;
    }
    clientTarget_.clear();
    isServer_ = false;
    localPort_ = 0;
    kind_ = SocketKind::Udp;
}

} // namespace Engine::Networking

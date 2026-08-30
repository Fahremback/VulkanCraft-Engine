#include "SocketTransport.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
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
#include <signal.h>
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
    // TCP is NOT implemented end-to-end here (no accept()/recv()/send() path;
    // the receive loop and datagram framing are UDP-shaped). Rather than expose
    // a half-working flag that promises a mode it cannot deliver (A1-TCP-MEIO),
    // reject it explicitly: selectors get a clear failure instead of silence.
    if (kind == SocketKind::Tcp) {
        lastError_ = "SocketKind::Tcp is not supported by SocketTransport";
        return false;
    }
    close();
    const int type = (kind == SocketKind::Udp) ? SOCK_DGRAM : SOCK_STREAM;
    const SOCKET_T s = ::socket(AF_INET, type, 0);
    if (s == kBadSocket) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
#if defined(_WIN32)
        closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }
    // Query the actual bound port (relevant when port == 0 / ephemeral).
    {
        sockaddr_in bound{};
#if defined(_WIN32)
        int boundLen = sizeof(bound);
#else
        socklen_t boundLen = sizeof(bound);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
            localPort_ = ntohs(bound.sin_port);
        } else {
            localPort_ = port;
        }
    }
    if (kind == SocketKind::Tcp) {
        if (::listen(s, 8) == SOCKET_ERROR) {
#if defined(_WIN32)
            closesocket(s);
#else
            ::close(s);
#endif
            return false;
        }
    }
    // Non-blocking mode.
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    socket_ = static_cast<uintptr_t>(s);
    // localPort_ was already set from getsockname above (ephemeral-aware).
    isServer_ = true;
    return true;
}

bool SocketTransport::connect(const std::string& host, uint16_t port, SocketKind kind) {
#if defined(_WIN32)
    static WinsockInit init;
#endif
    // See listen(): TCP has no functional path here; reject it explicitly so a
    // caller cannot silently fall into a half-configured UDP-shaped socket.
    if (kind == SocketKind::Tcp) {
        lastError_ = "SocketKind::Tcp is not supported by SocketTransport";
        return false;
    }
    close();
    const int type = (kind == SocketKind::Udp) ? SOCK_DGRAM : SOCK_STREAM;
    const SOCKET_T s = ::socket(AF_INET, type, 0);
    if (s == kBadSocket) return false;

    // Resolve host.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Try DNS resolution.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = type;
        addrinfo* results = nullptr;
        char serviceBuf[16];
        std::snprintf(serviceBuf, sizeof(serviceBuf), "%u", port);
        if (getaddrinfo(host.c_str(), serviceBuf, &hints, &results) == 0 && results) {
            std::memcpy(&addr, results->ai_addr, sizeof(addr));
            freeaddrinfo(results);
        } else {
#if defined(_WIN32)
            closesocket(s);
#else
            ::close(s);
#endif
            return false;
        }
    }

    if (kind == SocketKind::Udp) {
        // Connectionless: remember the target, no connect() needed.
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        clientTarget_ = std::string(ip) + ":" + std::to_string(port);
        localPort_ = 0;
        isServer_ = false;
        // Bind an ephemeral local port so replies come back, then report the
        // actual bound port (0/"ephemeral" is not the real source port).
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0) {
#if defined(_WIN32)
            int boundLen = sizeof(local);
#else
            socklen_t boundLen = sizeof(local);
#endif
            if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &boundLen) == 0) {
                localPort_ = ntohs(local.sin_port);
            }
        }
    } else {
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
#if defined(_WIN32)
            closesocket(s);
#else
            ::close(s);
#endif
            return false;
        }
    }
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    socket_ = static_cast<uintptr_t>(s);
    isServer_ = false;
    return true;
}

bool SocketTransport::start_receive(ReceiveCallback callback) {
    if (socket_ == kInvalid || receiving_) return false;
    callback_ = std::move(callback);
    receiving_ = true;
    receiveThread_ = new std::thread([this] { run_receive_loop(); });
    return true;
}

void SocketTransport::run_receive_loop() {
    while (receiving_.load(std::memory_order_acquire)) {
        auto dg = poll();
        if (dg) {
            if (callback_) callback_(std::move(*dg));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void SocketTransport::stop_receive() {
    receiving_.store(false, std::memory_order_release);
    if (receiveThread_) {
        auto* t = static_cast<std::thread*>(receiveThread_);
        if (t->joinable()) t->join();
        delete t;
        receiveThread_ = nullptr;
    }
}

bool SocketTransport::send_to(const std::string& peer, const std::byte* data, std::size_t size) {
    if (socket_ == kInvalid) return false;
    // Parse "ip:port".
    const size_t colon = peer.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string ip = peer.substr(0, colon);
    const uint16_t port = static_cast<uint16_t>(std::stoi(peer.substr(colon + 1)));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
    const int flags = 0
#if defined(__linux__)
        | MSG_NOSIGNAL
#endif
        ;
    const int n = static_cast<int>(::sendto(static_cast<SOCKET_T>(socket_), reinterpret_cast<const char*>(data),
                                            size, flags, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    return n >= 0;
}

bool SocketTransport::send(const std::byte* data, std::size_t size) {
    if (socket_ == kInvalid) return false;
    if (isServer_) return false;
    if (clientTarget_.empty()) return false;
    if (size > 65507) return false;
    return send_to(clientTarget_, data, size);
}

std::optional<Datagram> SocketTransport::poll() {
    if (socket_ == kInvalid) return std::nullopt;
    std::byte buffer[65536];
    sockaddr_in from{};
#if defined(_WIN32)
    int fromLen = sizeof(from);
#else
    socklen_t fromLen = sizeof(from);
#endif
    const int n = static_cast<int>(::recvfrom(static_cast<SOCKET_T>(socket_), reinterpret_cast<char*>(buffer),
                                              sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
    if (n < 0) return std::nullopt;
    Datagram dg;
    dg.payload.assign(buffer, buffer + n);
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    dg.peer = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
    dg.receivedTime = now_seconds();
    return dg;
}

void SocketTransport::close() {
    if (socket_ != kInvalid) {
#if defined(_WIN32)
        closesocket(static_cast<SOCKET_T>(socket_));
#else
        ::close(static_cast<SOCKET_T>(socket_));
#endif
        socket_ = kInvalid;
    }
    clientTarget_.clear();
    isServer_ = false;
    localPort_ = 0;
}

} // namespace Engine::Networking

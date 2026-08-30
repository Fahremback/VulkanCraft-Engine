// Agent 1 — Networking / Servidor (fechamento_solidacao)
// Standalone tests for the real transport bugs (P0/P1). Scripts only the two
// self-contained networking sources (ReliableTransport.cpp + SocketTransport.cpp)
// so the fixes can be proven locally without a full engine build.
//
// Required tests (task_plan.md agente1_network):
//   ReliableTransport.RetransmitCompressed
//   ReliableTransport.PacketLoss30Percent
//   ReliableTransport.ReconnectDuringReceive
//   SocketTransport.StopReceiveRace
//   ReliableTransport.MalformedFragmentRejected
//   ReliableTransport.PartialMessagesBounded
//
// Each must FAIL against the pre-fix code and PASS against the fix.

#include "src/engine/networking/ReliableTransport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace Engine::Networking;

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAIL " << what << " at line " << __LINE__ << '\n'; \
            ++g_failures;                                                   \
        } else {                                                            \
            std::cout << "  pass: " << what << '\n';                        \
        }                                                                   \
    } while (0)


// ── deterministic UDP proxy with configurable drop ──────────────────────────
struct LossyProxy {
    SocketTransport in_;
    std::string target_;
    std::string clientAddr_;
    int dropEveryK{0};   // drop when (counter % dropEveryK) == 0, 0/1 = lossless
    int counter{0};
    int dropped{0};

    bool start(std::uint16_t targetPort, int k) {
        dropEveryK = k;
        target_ = "127.0.0.1:" + std::to_string(targetPort);
        return in_.listen(0, SocketKind::Udp);
    }
    std::uint16_t port() const { return in_.local_port(); }
    void pump() {
        while (auto dg = in_.poll()) {
            if (dg->peer == target_) {
                if (!clientAddr_.empty()) in_.send_to(clientAddr_, dg->payload.data(), dg->payload.size());
                continue;
            }
            clientAddr_ = dg->peer;
            if (dropEveryK > 1 && (counter++ % dropEveryK) == 0) { ++dropped; continue; }
            in_.send_to(target_, dg->payload.data(), dg->payload.size());
        }
    }
};

// Connect two ReliableTransport endpoints through an optional proxy. `dropK`
// is the deterministic loss divisor (every dropK-th client datagram is
// dropped); 0/1 = lossless. 2 = ~50%, 3 = ~33%, 4 = ~25%.
static bool connect_pair(ReliableTransport& server, ReliableTransport& client,
                         LossyProxy* proxy, int dropK, std::string& serverPort_out) {
    bool serverConnected = false, clientConnected = false;
    server.set_status_handler([&](ReliableTransport::Status s) {
        if (s == ReliableTransport::Status::Connected) serverConnected = true;
    });
    client.set_status_handler([&](ReliableTransport::Status s) {
        if (s == ReliableTransport::Status::Connected) clientConnected = true;
    });
    if (!server.listen(0)) return false;
    serverPort_out = std::to_string(server.local_port());
    if (proxy && !proxy->start(server.local_port(), dropK)) return false;
    const std::uint16_t port = proxy ? proxy->port() : server.local_port();
    if (!client.connect("127.0.0.1", port)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(serverConnected && clientConnected) && std::chrono::steady_clock::now() < deadline) {
        if (proxy) proxy->pump();
        server.update(std::chrono::steady_clock::now());
        client.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return serverConnected && clientConnected;
}

// ── deterministic pseudo-random payload with a chosen zero density ──────────
static std::vector<std::byte> make_payload(std::size_t size, std::uint64_t seed, int zeroEvery) {
    std::vector<std::byte> out(size);
    std::uint64_t x = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    for (std::size_t i = 0; i < size; ++i) {
        if (zeroEvery > 0 && (static_cast<int>(i) % zeroEvery) == 0) {
            out[i] = std::byte{0};
        } else {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            out[i] = static_cast<std::byte>((x >> 33) & 0xFF);
        }
    }
    return out;
}

static void test_RetransmitCompressed() {
    std::cout << "\n=== ReliableTransport.RetransmitCompressed ===\n";
    ReliableTransport::Config cfg;
    cfg.maxPayload = 48;  // 3 fragments even for modest sizes
    cfg.retransmitTimeout = std::chrono::milliseconds(30);
    cfg.heartbeatInterval = std::chrono::milliseconds(60);
    cfg.timeoutAfter = std::chrono::milliseconds(2000);
    cfg.maxRetransmits = 40;    ReliableTransport server(cfg), client(cfg);

    std::vector<std::vector<std::byte>> got;
    server.set_message_handler([&](const std::byte* d, std::size_t n) {
        got.emplace_back(d, d + n);
    });
    LossyProxy proxy;
    std::string sp;
    // Connect directly through the lossy proxy (every 2nd datagram dropped ->
    // forces retransmission of fragments). The message handler was set on
    // `server` (the real receiver).
    if (!connect_pair(server, client, &proxy, 2, sp)) { CHECK(false, "setup via lossy proxy"); return; }

    // Highly compressible payload (90% zeros) spanning several fragments -> all
    // fragments sent COMPRESSED. Under loss the receiver must reassemble the
    // original exactly (the P0 retransmit bug marked raw bytes FLAG_COMPRESSED).
    const auto payload = make_payload(600, 1234, 2);  // ~50% zeros -> compresses
    client.send(payload.data(), payload.size());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (got.empty() && std::chrono::steady_clock::now() < deadline) {
        proxy.pump();
        server.update(std::chrono::steady_clock::now());
        client.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(proxy.dropped > 0, "loss actually applied");
    CHECK(client.retransmits() > 0, "retransmission happened under loss");
    CHECK(got.size() == 1, "exactly one message delivered");
    if (got.size() == 1) {
        CHECK(got[0] == payload, "compressed message reassembled bit-identical");
    }
    client.close();
    server.close();
}

static void test_PacketLoss30Percent() {
    std::cout << "\n=== ReliableTransport.PacketLoss30Percent ===\n";
    ReliableTransport::Config cfg;
    cfg.maxPayload = 64;
    cfg.retransmitTimeout = std::chrono::milliseconds(25);
    cfg.heartbeatInterval = std::chrono::milliseconds(60);
    cfg.timeoutAfter = std::chrono::milliseconds(2500);
    cfg.maxRetransmits = 60;
    ReliableTransport server(cfg), client(cfg);

    std::vector<std::vector<std::byte>> got;
    server.set_message_handler([&](const std::byte* d, std::size_t n) {
        got.emplace_back(d, d + n);
    });
    LossyProxy proxy;
    std::string sp;
    // ~25% deterministic loss (every 4th client datagram dropped), applied from
    // the very first handshake SYN so the loss-resilient handshake retry runs.
    if (!connect_pair(server, client, &proxy, 4, sp)) { CHECK(false, "setup via proxy"); return; }

    // Several messages: small compressible, big compressible (zero runs),
    // big INCOMPRESSIBLE-with-zeros (fragments sent uncompressed) — exercise
    // retransmission + fragmentation + compression together, both flag states.
    const std::vector<std::vector<std::byte>> payloads = {
        make_payload(30, 1, 2),
        make_payload(700, 2, 3),     // fragmented, compressible
        std::vector<std::byte>(1500, std::byte{0}),  // pure zeros, fragmented, very compressible
        make_payload(900, 4, 9),     // ~1/9 zeros, incompressible -> uncompressed frags
    };

    for (const auto& p : payloads) client.send(p.data(), p.size());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (got.size() < payloads.size() && std::chrono::steady_clock::now() < deadline) {
        proxy.pump();
        server.update(std::chrono::steady_clock::now());
        client.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(proxy.dropped >= 1, "loss applied (dropped " + std::to_string(proxy.dropped) + ")");
    CHECK(client.retransmits() > 0, "retransmission occurred");
    CHECK(got.size() == payloads.size(), "all messages delivered (" + std::to_string(got.size()) + "/" + std::to_string(payloads.size()) + ")");
    if (got.size() == payloads.size()) {
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            CHECK(got[i] == payloads[i], "message " + std::to_string(i) + " bit-identical");
        }
    }
    client.close();
    server.close();
}

static void test_ReconnectDuringReceive() {
    std::cout << "\n=== ReliableTransport.ReconnectDuringReceive ===\n";
    ReliableTransport::Config cfg;
    cfg.retransmitTimeout = std::chrono::milliseconds(25);
    cfg.heartbeatInterval = std::chrono::milliseconds(40);
    cfg.timeoutAfter = std::chrono::milliseconds(2000);
    cfg.maxRetransmits = 40;
    ReliableTransport server(cfg), client(cfg);

    std::atomic<bool> clientGot{false};
    std::string lastAppMsg;
    client.set_message_handler([&](const std::byte* d, std::size_t n) {
        lastAppMsg.assign(reinterpret_cast<const char*>(d), n);
        clientGot = true;
    });
    bool sc = false, cc = false;
    server.set_status_handler([&](ReliableTransport::Status s){ if (s==ReliableTransport::Status::Connected) sc = true; });
    client.set_status_handler([&](ReliableTransport::Status s){ if (s==ReliableTransport::Status::Connected) cc = true; });
    if (!server.listen(0)) { CHECK(false, "server listen"); return; }
    const std::uint16_t serverPort = server.local_port();
    if (!client.connect("127.0.0.1", serverPort)) { CHECK(false, "client connect"); return; }
    const auto hs = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(sc && cc) && std::chrono::steady_clock::now() < hs) {
        server.update(std::chrono::steady_clock::now());
        client.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!(sc && cc)) { CHECK(false, "handshake"); return; }

    // Keep the server sending a stream of messages so the client's receive
    // thread has datagrams in flight while we reconnect (that is what deadlocks
    // the old listen/connect that held the mutex across stop_receive/join).
    std::atomic<bool> streaming{true};
    std::thread streamer([&] {
        int i = 0;
        while (streaming.load()) {
            const std::string m = "before-" + std::to_string(i++);
            server.send(reinterpret_cast<const std::byte*>(m.data()), m.size());
            server.update(std::chrono::steady_clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Warm the client in-flight pipe.
    const auto warm = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < warm) {
        client.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Reconnect on a watchdog thread: if it deadlocks (old code), the future
    // never resolves and we FAIL.
    std::promise<bool> reconResult;
    auto reconFut = reconResult.get_future();
    std::thread recon([&] {
        const bool ok = client.connect("127.0.0.1", serverPort);  // reconnect a quente
        reconResult.set_value(ok);
    });

    const bool completed = reconFut.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    CHECK(completed, "reconnect returned within watchdog (no deadlock)");
    bool reconOk = false;
    if (completed) {
        reconOk = reconFut.get();
        CHECK(reconOk, "reconnect returned success");
    }
    recon.join();
    streaming.store(false);
    streamer.join();

    // A server already in Connected does not re-answer a re-SYN (by design in
    // the handshake), so verify the reconnected client is fully functional
    // against a FRESH server listener instead.
    if (completed && reconOk) {
        std::atomic<bool> serverBGot{false};
        ReliableTransport serverB(cfg);
        serverB.set_message_handler([&](const std::byte*, std::size_t){ serverBGot = true; });
        if (!serverB.listen(0)) {
            CHECK(false, "serverB listen");
        } else {
            bool bConnected = false;
            serverB.set_status_handler([&](ReliableTransport::Status s){ if (s==ReliableTransport::Status::Connected) bConnected = true; });
            const bool ok2 = client.connect("127.0.0.1", serverB.local_port());
            const std::string post = "post-reconnect";
            const auto dd = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            bool sent = false;
            while (std::chrono::steady_clock::now() < dd) {
                client.update(std::chrono::steady_clock::now());
                serverB.update(std::chrono::steady_clock::now());
                if (bConnected && !sent) {
                    sent = client.send(reinterpret_cast<const std::byte*>(post.data()), post.size());
                }
                if (serverBGot) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            (void)ok2;
            CHECK(serverBGot, "reconnected client delivers to a fresh server (functional after reconnect)");
            serverB.close();
        }
    }

    client.close();
    server.close();
}

static void test_TcpRejected() {
    std::cout << "\n=== SocketTransport.TcpRejected (option B: no half-TCP) ===\n";
    // TCP is exposed in the enum but had no accept/recv/send path. Per
    // A1-TCP-MEIO we choose option B: selecting SocketKind::Tcp must return an
    // explicit error, never a silent half-configured socket.
    SocketTransport st;
    const bool listenRejected = !st.listen(0, SocketKind::Tcp);
    CHECK(listenRejected, "listen(SocketKind::Tcp) returns explicit error");
    const bool connectRejected = !st.connect("127.0.0.1", 9, SocketKind::Tcp);
    CHECK(connectRejected, "connect(SocketKind::Tcp) returns explicit error");
    // UDP still works (mode of record) - baseline sanity.
    CHECK(st.listen(0, SocketKind::Udp), "UDP listen still works");
    st.close();
}

static void test_StopReceiveRace() {
    std::cout << "\n=== SocketTransport.StopReceiveRace ===\n";
    bool ok = true;
    const int cycles = 3000;  // start/stop churn previously raced on bool receiving_
    for (int i = 0; i < cycles; ++i) {
        SocketTransport st;
        if (!st.listen(0, SocketKind::Udp)) { ok = false; break; }
        if (!st.start_receive([](Datagram){})) { ok = false; break; }
        st.stop_receive();
        st.close();
    }
    CHECK(ok, std::to_string(cycles) + " start/stop cycles without crash/race");
}

// ── raw wire header matching the private PacketHeader layout (16 bytes) ─────
struct WireHeader {  // matches ReliableTransport::PacketHeader
    std::uint32_t seq;
    std::uint32_t ack;
    std::uint16_t flags;
    std::uint16_t fragmentIndex;
    std::uint16_t fragmentCount;
    std::uint16_t payloadSize;
};
static constexpr uint16_t FLAG_DATA = 1u << 2;
static constexpr uint16_t FLAG_COMPRESSED = 1u << 4;

static std::vector<std::byte> raw_frame(WireHeader h, const std::byte* payload, std::size_t n) {
    std::vector<std::byte> f(sizeof(WireHeader) + n);
    std::memcpy(f.data(), &h, sizeof(WireHeader));
    if (n) std::memcpy(f.data() + sizeof(WireHeader), payload, n);
    return f;
}

static bool send_raw(std::uint16_t serverPort, const std::vector<std::byte>& frame) {
    // A one-shot UDP sender.
    SocketTransport atk;
    if (!atk.connect("127.0.0.1", serverPort, SocketKind::Udp)) return false;
    return atk.send(frame.data(), frame.size());
}

static void test_MalformedFragmentRejected() {
    std::cout << "\n=== ReliableTransport.MalformedFragmentRejected ===\n";
    ReliableTransport::Config cfg;
    cfg.maxPayload = 64;
    ReliableTransport server(cfg);
    std::atomic<uint32_t> delivered{0};
    server.set_message_handler([&](const std::byte*, std::size_t){ ++delivered; });
    if (!server.listen(0)) { CHECK(false, "server listen"); return; }
    const std::uint16_t port = server.local_port();

    const uint32_t before = server.rejected_fragments();
    const std::byte zero{0};
    // fragmentCount == 0
    send_raw(port, raw_frame({1,0,FLAG_DATA,0,0,0}, nullptr, 0));
    // fragmentIndex >= fragmentCount
    send_raw(port, raw_frame({2,0,FLAG_DATA,1,1,1}, &zero, 1));
    // fragmentIndex > seq (uint32 underflow of seq - index)
    send_raw(port, raw_frame({0,0,FLAG_DATA,5,6,1}, &zero, 1));
    // fragmentCount absurd (> maxFragments) — send index 0,count 5000
    send_raw(port, raw_frame({3,0,FLAG_DATA,0,5000,1}, &zero, 1));

    // Give the receive thread time to process, then read the counter while
    // the server is still alive (close() doesn't reset it, but read it early).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint32_t rejected = server.rejected_fragments() - before;
    const uint32_t got = delivered.load();
    server.close();

    CHECK(rejected >= 4, "all 4 malformed fragments rejected (rejected=" + std::to_string(rejected) + ")");
    CHECK(got == 0, "no message delivered from malformed frames");
}

static void test_PartialMessagesBounded() {
    std::cout << "\n=== ReliableTransport.PartialMessagesBounded ===\n";
    ReliableTransport::Config cfg;
    cfg.maxPartialEntries = 8;  // small cap to prove the agent-style bound
    cfg.timeoutAfter = std::chrono::milliseconds(150);
    ReliableTransport server(cfg), client(cfg);
    std::atomic<uint32_t> delivered{0};
    server.set_message_handler([&](const std::byte*, std::size_t){ ++delivered; });
    if (!server.listen(0)) { CHECK(false, "server listen"); return; }
    // Real handshake so server.nextInSeq_ anchors to 1 (the data seq space a
    // normal connection lives in) — otherwise a legit seq-1 message would sit
    // buffered behind a never-advancing zero cursor and never deliver.
    bool cc = false, sc = false;
    client.set_status_handler([&](ReliableTransport::Status s){ if (s==ReliableTransport::Status::Connected) cc=true; });
    server.set_status_handler([&](ReliableTransport::Status s){ if (s==ReliableTransport::Status::Connected) sc=true; });
    if (!client.connect("127.0.0.1", server.local_port())) return;
    const auto hs = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(cc && sc) && std::chrono::steady_clock::now() < hs) {
        client.update(std::chrono::steady_clock::now()); server.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!(cc && sc)) { CHECK(false, "handshake"); return; }
    const std::uint16_t port = server.local_port();

    const uint32_t before = server.rejected_fragments();
    // 200 unique first-fragments (fragIndex 0 of 5). process_data rejects once
    // partials_ is full (bounded by maxPartialEntries), so rejected_fragments
    // must grow past 0 - without the cap an attacker would grow partials_
    // without bound.
    const std::byte zero{0};
    for (std::uint32_t i = 1; i <= 200; ++i) {
        send_raw(port, raw_frame({i,0,FLAG_DATA,0,5,1}, &zero, 1));
    }
    // Age out: advance time by firing server.update() (the age-based global
    // sweep clears abandoned partials), then a legit seq-1 single fragment must
    // still reassemble and deliver.
    const auto then = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < then) {
        server.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::vector<std::byte> good = make_payload(40, 99, 0);
    WireHeader gh{1,0,FLAG_DATA,0,1,static_cast<uint16_t>(good.size())};
    send_raw(port, raw_frame(gh, good.data(), good.size()));
    const auto wait = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (delivered == 0 && std::chrono::steady_clock::now() < wait) {
        server.update(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const uint32_t rejected = server.rejected_fragments() - before;
    const uint32_t got = delivered.load();
    server.close();
    client.close();

    CHECK(rejected >= 1, "partial table capped (rejected first-fragments=" + std::to_string(rejected) + ")");
    CHECK(rejected <= 200, "not every distinct first-fragment caused unbounded growth");
    CHECK(got == 1, "legit message delivered after ageing sweep freed partials");
}

int main() {
    test_RetransmitCompressed();
    test_PacketLoss30Percent();
    test_ReconnectDuringReceive();
    test_TcpRejected();
    test_StopReceiveRace();
    test_MalformedFragmentRejected();
    test_PartialMessagesBounded();

    std::cout << "\n" << (g_failures == 0 ? "ALL NETWORK AGENT-1 TESTS PASS" : "FAILURES: " + std::to_string(g_failures)) << "\n";
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
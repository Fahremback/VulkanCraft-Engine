#include "ReliableTransport.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace Engine::Networking {

ReliableTransport::ReliableTransport(Config config) : config_(config) {}

ReliableTransport::~ReliableTransport() {
    disconnect();
    socket_.stop_receive();
}

bool ReliableTransport::listen(std::uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    socket_.stop_receive();
    if (!socket_.listen(port, SocketKind::Udp)) return false;
    socket_.start_receive([this](Datagram datagram) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!datagram.peer.empty() && peerTarget_.empty()) {
            peerTarget_ = datagram.peer;  // learn server-side peer endpoint
        }
        handle_datagram(datagram.payload.data(), datagram.payload.size(), now);
    });
    status_ = Status::Disconnected;
    lastReceive_ = std::chrono::steady_clock::now();
    lastHeartbeat_ = lastReceive_;
    return true;
}

bool ReliableTransport::connect(const std::string& host, std::uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    socket_.stop_receive();
    if (!socket_.listen(0, SocketKind::Udp)) return false;
    if (!socket_.connect(host, port, SocketKind::Udp)) return false;
    socket_.start_receive([this](Datagram datagram) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!datagram.peer.empty() && peerTarget_.empty()) {
            peerTarget_ = datagram.peer;  // learn server-side peer endpoint
        }
        handle_datagram(datagram.payload.data(), datagram.payload.size(), now);
    });
    status_ = Status::Handshaking;
    handshakeSent_ = false;
    lastSend_ = std::chrono::steady_clock::time_point{};
    lastReceive_ = std::chrono::steady_clock::now();
    lastHeartbeat_ = lastReceive_;
    return true;
}

bool ReliableTransport::send(const std::byte* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != Status::Connected || size == 0) return false;

    // Fragment the payload into MTU-sized chunks; each fragment is one packet.
    const std::size_t maxFrag = config_.maxPayload;
    std::size_t offset = 0;
    std::uint16_t index = 0;
    const std::uint16_t count = static_cast<std::uint16_t>((size + maxFrag - 1) / maxFrag);
    while (offset < size) {
        const std::size_t chunk = std::min(maxFrag, size - offset);
        const std::uint32_t seq = next_sequence();
        OutgoingPacket packet;
        packet.seq = seq;
        packet.payload.assign(data + offset, data + offset + chunk);
        packet.lastSent = std::chrono::steady_clock::time_point{};

        PacketHeader header{};
        header.seq = seq;
        header.ack = nextInSeq_;
        header.flags = FLAG_DATA | FLAG_COMPRESSED;
        header.fragmentIndex = index++;
        header.fragmentCount = count;
        header.payloadSize = static_cast<std::uint16_t>(chunk);
        send_packet(header, packet.payload.data(), packet.payload.size());

        sendQueue_.push_back(std::move(packet));
        offset += chunk;
    }
    return true;
}

bool ReliableTransport::send_unreliable(const std::byte* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != Status::Connected || size == 0) return false;
    PacketHeader header{};
    header.seq = next_sequence();
    header.ack = nextInSeq_;
    header.flags = FLAG_DATA;  // no reliability flag: not queued, not ACKed
    header.fragmentIndex = 0;
    header.fragmentCount = 1;
    header.payloadSize = static_cast<std::uint16_t>(size);
    send_packet(header, data, size);
    return true;
}

void ReliableTransport::send_packet(const PacketHeader& header, const std::byte* payload,
                                    std::size_t payloadSize) {
    std::vector<std::byte> frame(sizeof(PacketHeader) + payloadSize);
    std::memcpy(frame.data(), &header, sizeof(PacketHeader));
    if (payloadSize > 0) std::memcpy(frame.data() + sizeof(PacketHeader), payload, payloadSize);
    if (!peerTarget_.empty()) {
        socket_.send_to(peerTarget_, frame.data(), frame.size());
    } else {
        socket_.send(frame.data(), frame.size());
    }
    sentBytes_ += static_cast<std::uint32_t>(frame.size());
    lastSend_ = std::chrono::steady_clock::now();
}

void ReliableTransport::update(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The receive thread delivers datagrams via the callback; nothing to poll
    // here. Drive handshake, retransmission, heartbeats and timeouts.
    if (status_ == Status::Handshaking && !handshakeSent_) {
        PacketHeader syn{};
        syn.seq = next_sequence();
        syn.ack = 0;
        syn.flags = FLAG_SYN;
        send_packet(syn, nullptr, 0);
        handshakeSent_ = true;
        lastSend_ = now;
    }
    if (status_ == Status::Connected) {
        retransmit(now);
        send_heartbeat(now);
    }
    check_timeout(now);
}

void ReliableTransport::handle_datagram(const std::byte* data, std::size_t size,
                                        std::chrono::steady_clock::time_point now) {
    if (size < sizeof(PacketHeader)) return;
    PacketHeader header;
    std::memcpy(&header, data, sizeof(PacketHeader));
    const std::byte* payload = data + sizeof(PacketHeader);
    const std::size_t payloadSize = size - sizeof(PacketHeader);
    receivedBytes_ += static_cast<std::uint32_t>(size);
    lastReceive_ = now;

    const bool syn = (header.flags & FLAG_SYN) != 0;
    const bool ack = (header.flags & FLAG_ACK) != 0;
    const bool dataFlag = (header.flags & FLAG_DATA) != 0;
    const bool heartbeat = (header.flags & FLAG_HEARTBEAT) != 0;
    const bool compressed = (header.flags & FLAG_COMPRESSED) != 0;

    if (syn) {
        // Peer wants to connect; answer with SYN|ACK and go connected.
        if (status_ == Status::Disconnected || status_ == Status::Handshaking) {
            PacketHeader synAck{};
            synAck.seq = next_sequence();
            synAck.ack = header.seq;
            synAck.flags = FLAG_SYN | FLAG_ACK;
            send_packet(synAck, nullptr, 0);
            if (status_ != Status::Connected) {
                status_ = Status::Connected;
                if (statusHandler_) statusHandler_(Status::Connected);
            }
        }
        return;
    }
    if (ack && (header.flags & FLAG_SYN) == 0 && !dataFlag && !heartbeat) {
        // Pure ACK: mark everything <= ack as acked.
        lastAcked_ = std::max(lastAcked_, header.ack);
        while (!sendQueue_.empty() && sendQueue_.front().seq <= header.ack) {
            sendQueue_.pop_front();
        }
        return;
    }
    if (heartbeat) {
        // Respond to heartbeat with a pure ACK (piggybacked below).
        PacketHeader hbAck{};
        hbAck.seq = next_sequence();
        hbAck.ack = header.seq;
        hbAck.flags = FLAG_ACK;
        send_packet(hbAck, nullptr, 0);
        return;
    }
    if (dataFlag) {
        process_data(header, payload, payloadSize, now);
    }
}

void ReliableTransport::process_data(const PacketHeader& header, const std::byte* payload,
                                     std::size_t size, std::chrono::steady_clock::time_point now) {
    if (header.fragmentCount == 1 && header.fragmentIndex == 0) {
        // Single-fragment message: deliver immediately.
        if (header.seq >= nextInSeq_) nextInSeq_ = header.seq + 1;
        std::vector<std::byte> message(payload, payload + size);
        if (header.flags & FLAG_COMPRESSED) {
            std::vector<std::byte> decompressed(config_.receiveBuffer);
            const std::size_t n = decompress(payload, size, decompressed.data(), decompressed.size());
            decompressed.resize(n);
            message = std::move(decompressed);
        }
        deliver_message(std::move(message));
        return;
    }

    // Multi-fragment message: accumulate until complete.
    auto& partial = partials_[header.seq - header.fragmentIndex];
    partial.count = header.fragmentCount;
    partial.received = now;
    bool exists = false;
    for (auto& f : partial.fragments) {
        if (f.index == header.fragmentIndex) { exists = true; break; }
    }
    if (!exists) partial.fragments.push_back({header.fragmentIndex, {payload, payload + size}});

    if (partial.fragments.size() == partial.count) {
        // Sort by index and concatenate.
        std::sort(partial.fragments.begin(), partial.fragments.end(),
                  [](const Fragment& a, const Fragment& b) { return a.index < b.index; });
        std::size_t total = 0;
        for (const auto& f : partial.fragments) total += f.data.size();
        std::vector<std::byte> message;
        message.reserve(total);
        for (const auto& f : partial.fragments) {
            message.insert(message.end(), f.data.begin(), f.data.end());
        }
        if (header.flags & FLAG_COMPRESSED) {
            std::vector<std::byte> decompressed(config_.receiveBuffer);
            const std::size_t n = decompress(message.data(), message.size(),
                                             decompressed.data(), decompressed.size());
            decompressed.resize(n);
            message = std::move(decompressed);
        }
        partials_.erase(header.seq - header.fragmentIndex);
        deliver_message(std::move(message));
    } else {
        // Drop if it has been incomplete too long.
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const auto recvMs = std::chrono::duration_cast<std::chrono::milliseconds>(partial.received.time_since_epoch()).count();
        if (nowMs - recvMs > static_cast<long long>(config_.timeoutAfter.count())) {
            partials_.erase(header.seq - header.fragmentIndex);
            ++droppedFragments_;
        }
    }
}

void ReliableTransport::deliver_message(std::vector<std::byte> message) {
    if (messageHandler_) {
        // Call the handler outside the lock to avoid re-entrancy deadlocks.
        MessageHandler handler = messageHandler_;
        mutex_.unlock();
        handler(message.data(), message.size());
        mutex_.lock();
    }
}

void ReliableTransport::retransmit(std::chrono::steady_clock::time_point now) {
    for (auto& packet : sendQueue_) {
        if (packet.acked) continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - packet.lastSent);
        if (elapsed >= config_.retransmitTimeout && packet.lastSent != std::chrono::steady_clock::time_point{}) {
            if (packet.retries >= config_.maxRetransmits) {
                status_ = Status::TimedOut;
                if (statusHandler_) statusHandler_(Status::TimedOut);
                sendQueue_.clear();
                return;
            }
            ++packet.retries;
            ++retransmits_;
            packet.lastSent = now;
            PacketHeader header{};
            header.seq = packet.seq;
            header.ack = nextInSeq_;
            header.flags = FLAG_DATA | FLAG_COMPRESSED;
            header.fragmentIndex = 0;
            header.fragmentCount = 1;
            header.payloadSize = static_cast<std::uint16_t>(packet.payload.size());
            send_packet(header, packet.payload.data(), packet.payload.size());
        }
    }
}

void ReliableTransport::send_heartbeat(std::chrono::steady_clock::time_point now) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat_);
    if (elapsed >= config_.heartbeatInterval) {
        PacketHeader hb{};
        hb.seq = next_sequence();
        hb.ack = nextInSeq_;
        hb.flags = FLAG_HEARTBEAT;
        send_packet(hb, nullptr, 0);
        lastHeartbeat_ = now;
    }
}

void ReliableTransport::check_timeout(std::chrono::steady_clock::time_point now) {
    if (status_ != Status::Connected && status_ != Status::Handshaking) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReceive_);
    if (elapsed >= config_.timeoutAfter) {
        status_ = Status::TimedOut;
        if (statusHandler_) statusHandler_(Status::TimedOut);
        sendQueue_.clear();
    }
}

void ReliableTransport::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    sendQueue_.clear();
    reorderBuffer_.clear();
    partials_.clear();
    status_ = Status::Disconnected;
    handshakeSent_ = false;
}

ReliableTransport::Status ReliableTransport::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::uint16_t ReliableTransport::local_port() const {
    return socket_.local_port();
}

// ─── Compression: RLE on zero runs (very effective for transform data) ───
std::size_t ReliableTransport::compress(const std::byte* in, std::size_t size,
                                        std::byte* out, std::size_t capacity) {
    std::size_t o = 0;
    std::size_t i = 0;
    while (i < size) {
        if (o + 2 > capacity) return 0;
        if (in[i] == std::byte{0}) {
            std::size_t run = 0;
            while (i + run < size && in[i + run] == std::byte{0} && run < 255) ++run;
            out[o++] = std::byte{0};
            out[o++] = static_cast<std::byte>(run);
            i += run;
        } else {
            out[o++] = in[i++];
        }
    }
    return o;
}

std::size_t ReliableTransport::decompress(const std::byte* in, std::size_t size,
                                          std::byte* out, std::size_t capacity) {
    std::size_t o = 0;
    std::size_t i = 0;
    while (i < size) {
        if (in[i] == std::byte{0}) {
            if (i + 1 >= size) return 0;
            const std::size_t run = static_cast<std::size_t>(in[i + 1]);
            if (o + run > capacity) return 0;
            std::memset(out + o, 0, run);
            o += run;
            i += 2;
        } else {
            if (o >= capacity) return 0;
            out[o++] = in[i++];
        }
    }
    return o;
}

} // namespace Engine::Networking

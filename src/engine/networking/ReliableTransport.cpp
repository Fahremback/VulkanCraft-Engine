#include "ReliableTransport.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace Engine::Networking {

ReliableTransport::ReliableTransport(Config config) : config_(config) {}

ReliableTransport::~ReliableTransport() {
    close();
}

void ReliableTransport::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sendQueue_.clear();
        reorderBuffer_.clear();
        partials_.clear();
        status_ = Status::Disconnected;
        handshakeSent_ = false;
        synRetries_ = 0;
    }
    // Release the mutex BEFORE tearing down the socket: stop_receive() joins
    // the receive thread, and that thread's callback takes the same mutex —
    // joining while holding it deadlocks whenever a datagram is in flight
    // (the callback blocks on the lock while the join waits for the thread).
    socket_.stop_receive();
    socket_.close();
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
    nextOutSeq_ = 1;  // fresh DATA seq space for this connection
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
    nextOutSeq_ = 1;  // fresh DATA seq space for this connection
    handshakeSent_ = false;
    synRetries_ = 0;
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
        packet.fragmentIndex = index;   // retransmits must repeat these, not 0/1
        packet.fragmentCount = count;
        // Stamp the actual send time: retransmit() skips packets whose
        // lastSent is empty, so an unstamped packet could never be
        // retransmitted — under real loss the reliable layer silently lost
        // the fragment instead of resending it.
        packet.lastSent = std::chrono::steady_clock::now();
        // First retry deadline gets the same jitter treatment as the rest.
        std::uniform_int_distribution<std::uint32_t> jitterDist(0, static_cast<std::uint32_t>(config_.retransmitTimeout.count() / 2));
        packet.retryJitterMs = jitterDist(rng_);

        // Compress the fragment. If compression expands the data (or fails),
        // send uncompressed and clear the FLAG_COMPRESSED bit so the receiver
        // does not attempt decompression on raw data.
        std::vector<std::byte> compressedBuf(config_.maxPayload * 2);
        const std::size_t compressedSize = compress(packet.payload.data(), packet.payload.size(),
                                                    compressedBuf.data(), compressedBuf.size());
        const bool useCompression = compressedSize > 0 && compressedSize < packet.payload.size();
        const std::byte* sendPayload = useCompression ? compressedBuf.data() : packet.payload.data();
        const std::size_t sendSize = useCompression ? compressedSize : packet.payload.size();

        PacketHeader header{};
        header.seq = seq;
        header.ack = nextInSeq_;
        header.flags = useCompression ? (FLAG_DATA | FLAG_COMPRESSED) : FLAG_DATA;
        header.fragmentIndex = index++;
        header.fragmentCount = count;
        header.payloadSize = static_cast<std::uint16_t>(sendSize);
        send_packet(header, sendPayload, sendSize);

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
    if (status_ == Status::Handshaking) {
        // Loss-resilient handshake: retry the SYN on the retransmit cadence
        // until the peer answers (or maxRetransmits, then TimedOut). A single
        // SYN can be dropped on a lossy link — the old single-shot handshake
        // stranded the client in Handshaking until the timeout.
        const bool first = !handshakeSent_;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastSend_).count();
        if (first || elapsed >= static_cast<long long>(config_.retransmitTimeout.count())) {
            if (!first && ++synRetries_ > config_.maxRetransmits) {
                status_ = Status::TimedOut;
                if (statusHandler_) statusHandler_(Status::TimedOut);
                return;
            }
            // Control packets (SYN, SYN-ACK, heartbeats, pure ACKs) live OUTSIDE
            // the DATA seq space: data seqs restart at 1 per connection and stay
            // contiguous, so the receiver's cursor never hits a phantom gap from
            // a control packet that will never arrive as data. Seq 0 is the
            // control sentinel (data seqs start at 1).
            PacketHeader syn{};
            syn.seq = 0;
            syn.ack = 0;
            syn.flags = FLAG_SYN;
            send_packet(syn, nullptr, 0);
            handshakeSent_ = true;
            lastSend_ = now;
        }
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

    // Piggybacked ACKs: every packet carries the peer's receive cursor. The
    // heartbeats and data carry it too — without processing it here, a sender
    // would never learn its data was received, would retransmit everything
    // until maxRetransmits and kill the connection on the first lost packet.
    // Strictly-less-than: the cursor is the NEXT expected seq, so seq == ack
    // is NOT yet received (it must stay queued for retransmission).
    lastAcked_ = std::max(lastAcked_, header.ack);
    while (!sendQueue_.empty() && sendQueue_.front().seq < header.ack) {
        sendQueue_.pop_front();
    }

    if (syn) {
        const bool isReply = (header.flags & FLAG_ACK) != 0;
        // SYN|ACK while Handshaking: our SYN was accepted (client side).
        // Accept without replying — answering would consume a data seq and
        // shift our data stream past the peer's receive cursor (a phantom gap
        // that stalls reorder delivery forever).
        if (isReply && status_ == Status::Handshaking) {
            // SYN|ACK while Handshaking: our SYN was accepted (client side).
            // Accept without replying — answering would consume a data seq and
            // shift our data stream past the peer's receive cursor (a phantom
            // gap that stalls reorder delivery forever).
            status_ = Status::Connected;
            synRetries_ = 0;
            // Both peers restart their DATA seq space at 1 per connection, so
            // the receive cursor anchors unconditionally on seq 1 — no
            // prediction from handshake seqs, which is what makes the
            // handshake robust under loss AND latency (a SYN retry or a
            // delayed duplicate never shifts the cursor).
            nextInSeq_ = 1u;
            if (statusHandler_) statusHandler_(Status::Connected);
            return;
        }
        // Peer wants to connect (pure SYN); answer with SYN|ACK and go
        // connected. Also covers simultaneous connect (both peers send SYN
        // while Handshaking) by responding to the other side's plain SYN.
        if (status_ == Status::Disconnected || status_ == Status::Handshaking) {
            PacketHeader synAck{};
            synAck.seq = 0;  // control sentinel, outside the data seq space
            synAck.ack = header.seq;
            synAck.flags = FLAG_SYN | FLAG_ACK;
            send_packet(synAck, nullptr, 0);
            if (status_ != Status::Connected) {
                status_ = Status::Connected;
                synRetries_ = 0;
                nextInSeq_ = 1u;
                if (statusHandler_) statusHandler_(Status::Connected);
            }
        }
        return;
    }
    if (ack && (header.flags & FLAG_SYN) == 0 && !dataFlag && !heartbeat) {
        // Pure ACK: handled by the piggybacked path above; nothing left to do.
        return;
    }
    if (heartbeat) {
        // Respond to heartbeat with a pure ACK (piggybacked below). Control
        // packets must not consume the DATA seq space (a consumed seq between
        // data packets would create a phantom gap that stalls the reorder
        // cursor); peek the next data seq without advancing it.
        PacketHeader hbAck{};
        hbAck.seq = 0;  // control sentinel, outside the data seq space
        hbAck.ack = header.seq;
        hbAck.flags = FLAG_ACK;
        send_packet(hbAck, nullptr, 0);
        return;
    }
    if (std::getenv("VC_TRACE_SEQ")) {
        std::fprintf(stderr, "[trace] port=%u %s%s%s%s seq=%u ack=%u frag=%u/%u nextIn=%u\n",
                     local_port(), syn ? "S" : "", ack ? "A" : "", dataFlag ? "D" : "",
                     heartbeat ? "H" : "", header.seq, header.ack,
                     header.fragmentIndex, header.fragmentCount, nextInSeq_);
    }
    if (dataFlag) {
        process_data(header, payload, payloadSize, now);
    }
}

void ReliableTransport::process_data(const PacketHeader& header, const std::byte* payload,
                                     std::size_t size, std::chrono::steady_clock::time_point now) {
    if (header.fragmentCount == 1 && header.fragmentIndex == 0) {
        // Single-fragment message: buffer by sequence, deliver only in order.
        if (header.seq < nextInSeq_) return;  // stale duplicate of a delivered seq
        if (reorderBuffer_.count(header.seq) == 0) {
            std::vector<std::byte> message(payload, payload + size);
            if (header.flags & FLAG_COMPRESSED) {
                std::vector<std::byte> decompressed(config_.receiveBuffer);
                const std::size_t n = decompress(payload, size, decompressed.data(), decompressed.size());
                decompressed.resize(n);
                message = std::move(decompressed);
            }
            reorderBuffer_[header.seq] = std::move(message);
        }
        deliver_ordered();
        return;
    }

    // Multi-fragment message: accumulate until complete.
    const std::uint32_t firstSeq = header.seq - header.fragmentIndex;
    auto& partial = partials_[firstSeq];
    partial.count = header.fragmentCount;
    partial.received = now;
    bool exists = false;
    for (auto& f : partial.fragments) {
        if (f.index == header.fragmentIndex) { exists = true; break; }
    }
    if (!exists) partial.fragments.push_back({header.fragmentIndex, {payload, payload + size}});

    if (partial.fragments.size() == partial.count) {
        deliver_ordered();
    } else {
        // Drop if it has been incomplete too long.
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const auto recvMs = std::chrono::duration_cast<std::chrono::milliseconds>(partial.received.time_since_epoch()).count();
        if (nowMs - recvMs > static_cast<long long>(config_.timeoutAfter.count())) {
            partials_.erase(firstSeq);
            ++droppedFragments_;
        }
    }
}

void ReliableTransport::deliver_ordered() {
    // Flush every complete contiguous unit starting at nextInSeq_: single-
    // fragment messages and completed partials, in sequence order. Anything
    // with a gap stays buffered until the missing fragment arrives (loss +
    // retransmission or reorder), preserving the reliable-ordered contract.
    for (;;) {
        const auto single = reorderBuffer_.find(nextInSeq_);
        if (single != reorderBuffer_.end()) {
            std::vector<std::byte> message = std::move(single->second);
            reorderBuffer_.erase(single);
            ++nextInSeq_;
            deliver_message(std::move(message));
            continue;
        }
        const auto partial = partials_.find(nextInSeq_);
        if (partial != partials_.end() &&
            partial->second.fragments.size() == partial->second.count) {
            // Sort by index and concatenate.
            std::sort(partial->second.fragments.begin(), partial->second.fragments.end(),
                      [](const Fragment& a, const Fragment& b) { return a.index < b.index; });
            std::size_t total = 0;
            for (const auto& f : partial->second.fragments) total += f.data.size();
            std::vector<std::byte> message;
            message.reserve(total);
            for (const auto& f : partial->second.fragments) {
                message.insert(message.end(), f.data.begin(), f.data.end());
            }
            const std::uint32_t count = partial->second.count;
            partials_.erase(partial);
            nextInSeq_ += count;
            // Fragments were compressed individually by send(); decompress the
            // assembled message (non-compressed when too small to shrink).
            std::vector<std::byte> decompressed(config_.receiveBuffer);
            const std::size_t n = decompress(message.data(), message.size(),
                                             decompressed.data(), decompressed.size());
            decompressed.resize(n);
            deliver_message(std::move(decompressed));
            continue;
        }
        break;
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
    // Each packet gets its own randomized retry deadline (RFC-6298-style
    // jitter, randomized again on every retry). Without jitter, retransmits
    // land on a deterministic schedule that can phase-lock with a
    // deterministic loss pattern (e.g. "drop every 2nd datagram"): the same
    // packet gets dropped round after round while its neighbors sail through,
    // and the connection dies on maxRetransmits for a packet that never got a
    // fair try.
    for (auto& packet : sendQueue_) {
        if (packet.acked) continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - packet.lastSent).count();
        const auto due = static_cast<long long>(config_.retransmitTimeout.count()) + packet.retryJitterMs;
        if (elapsed >= due && packet.lastSent != std::chrono::steady_clock::time_point{}) {
            if (packet.retries >= config_.maxRetransmits) {
                status_ = Status::TimedOut;
                if (statusHandler_) statusHandler_(Status::TimedOut);
                sendQueue_.clear();
                return;
            }
            ++packet.retries;
            ++retransmits_;
            packet.lastSent = now;
            std::uniform_int_distribution<std::uint32_t> jitterDist(0, static_cast<std::uint32_t>(config_.retransmitTimeout.count() / 2));
            packet.retryJitterMs = jitterDist(rng_);
            PacketHeader header{};
            header.seq = packet.seq;
            header.ack = nextInSeq_;
            header.flags = FLAG_DATA | FLAG_COMPRESSED;
            // Repeat the ORIGINAL fragment metadata: retransmitting a fragment
            // of a multi-fragment message as "index 0 of 1" made the receiver
            // deliver a truncated message and the real one never completed.
            header.fragmentIndex = packet.fragmentIndex;
            header.fragmentCount = packet.fragmentCount;
            header.payloadSize = static_cast<std::uint16_t>(packet.payload.size());
            send_packet(header, packet.payload.data(), packet.payload.size());
        }
    }
}

void ReliableTransport::send_heartbeat(std::chrono::steady_clock::time_point now) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat_);
    if (elapsed >= config_.heartbeatInterval) {
        PacketHeader hb{};
        hb.seq = 0;  // control sentinel, outside the data seq space
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
    synRetries_ = 0;
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

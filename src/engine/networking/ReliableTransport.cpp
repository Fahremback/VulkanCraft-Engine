#include "ReliableTransport.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace Engine::Networking {

ReliableTransport::ReliableTransport() : config_(Config{}) {}

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
    // The receive thread's callback locks mutex_. Calling socket_.stop_receive()
    // (which joins that thread) WHILE holding mutex_ is a textbook A↔B
    // deadlock whenever a datagram is in flight: the join waits for the thread
    // to finish, but the thread's callback is blocked on the lock we hold.
    // Adopt the close() lifecycle: tear down the socket/receive thread first,
    // then re-bind and restart under the lock.
    socket_.stop_receive();
    socket_.close();
    if (!socket_.listen(port, SocketKind::Udp)) return false;
    socket_.start_receive([this](Datagram datagram) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!datagram.peer.empty() && peerTarget_.empty()) {
            peerTarget_ = datagram.peer;  // learn server-side peer endpoint
        }
        handle_datagram(datagram.payload.data(), datagram.payload.size(), now);
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = Status::Disconnected;
        nextOutSeq_ = 1;  // fresh DATA seq space for this connection
        // Drop any learned peer from a prior connection. A re-listen is a new
        // endpoint whose peer is re-learned from the first inbound datagram;
        // keeping the stale address would misroute our first sends.
        peerTarget_.clear();
        lastReceive_ = std::chrono::steady_clock::now();
        lastHeartbeat_ = lastReceive_;
    }
    return true;
}

bool ReliableTransport::connect(const std::string& host, std::uint16_t port) {
    // See listen(): never call socket_.stop_receive() under mutex_. Tear the
    // receive thread down first, then bind/connect and restart it, then update
    // state under the lock.
    socket_.stop_receive();
    socket_.close();
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = Status::Handshaking;
        nextOutSeq_ = 1;  // fresh DATA seq space for this connection
        handshakeSent_ = false;
        synRetries_ = 0;
        // Reconnect to a (possibly different) peer: must forget the old peer
        // address, else the first SYN here goes to the previous connection's
        // endpoint (send_packet routes on peerTarget_ when non-empty).
        peerTarget_.clear();
        lastSend_ = std::chrono::steady_clock::time_point{};
        lastReceive_ = std::chrono::steady_clock::now();
        lastHeartbeat_ = lastReceive_;
    }
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

        // Remember exactly what went on the wire (compressed OR raw bytes with
        // the matching FLAG_COMPRESSED bit). retransmit() replays this verbatim
        // instead of re-deriving the compression decision, so a peer never
        // receives a COMPRESSED-flagged packet whose body is plain bytes.
        packet.wirePayload.assign(sendPayload, sendPayload + sendSize);
        packet.wireFlags = header.flags;

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
    sweep_stale_buffers(now);
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
    // ── Malformed-fragment validation (DoS hardening) ──────────────────────
    // Every field is attacker-controlled from the wire. A hostile peer can
    // otherwise underflow frag/seq math, allocate unbounded partials, or force
    // absurd reassembly sizes. Reject the fragment outright and drop it.
    const auto reject = [&]() { ++rejectedFragments_; return; };

    if (header.fragmentCount == 0) return reject();          // no such message shape
    if (header.fragmentIndex >= header.fragmentCount) return reject();  // index out of range
    // firstSeq = seq - index would underflow when index > seq (uint32 wrap to
    // a huge number that can alias a live partial / cause unbounded growth).
    if (header.fragmentIndex > header.seq) return reject();
    if (header.seq < nextInSeq_) return;  // stale duplicate of a delivered seq
    if (size > config_.maxMessageSize) return reject();      // absurd single fragment
    if (header.fragmentCount > config_.maxFragments) return reject();  // absurd message

    if (header.fragmentCount == 1 && header.fragmentIndex == 0) {
        // Single-fragment message: buffer by sequence, deliver only in order.
        if (reorderBuffer_.size() >= config_.maxReorderEntries) {
            // Reorder buffer is saturated with out-of-order/unacked single
            // messages. Drop the newest to bound memory under a flood of
            // unique unseen seqs; a bogus stream should not grow unbounded.
            return reject();
        }
        if (reorderBuffer_.count(header.seq) == 0) {
            std::vector<std::byte> message(payload, payload + size);
            if (header.flags & FLAG_COMPRESSED) {
                std::vector<std::byte> decompressed(config_.receiveBuffer);
                const std::size_t n = decompress(payload, size, decompressed.data(), decompressed.size());
                decompressed.resize(n);
                if (decompressed.size() > config_.maxMessageSize) return reject();
                message = std::move(decompressed);
            }
            reorderBuffer_[header.seq] = std::move(message);
        }
        deliver_ordered();
        return;
    }

    // Multi-fragment message: accumulate until complete (with limits).
    const std::uint32_t firstSeq = header.seq - header.fragmentIndex;  // safe: index<=seq checked
    // Bound the partial table: never let an attacker grow unbounded partials
    // with first-fragment-only floods. Also bound the fragment count per
    // message and the assembled size.
    if (partials_.size() >= config_.maxPartialEntries &&
        partials_.find(firstSeq) == partials_.end()) {
        return reject();
    }
    auto& partial = partials_[firstSeq];
    partial.count = header.fragmentCount;
    partial.received = now;
    if (partial.fragments.size() >= partial.count) {
        // All fragments already present; this is a duplicate/invalid send.
        ++rejectedFragments_;
        return;
    }
    // Decompress this fragment NOW from its own wire flag (send() compresses
    // each fragment independently and may mix compressed/uncompressed when the
    // data barely shrinks). Storing the decoded bytes here means deliver_ordered()
    // just concatenates — it must NOT bulk-decompress the assembled stream,
    // which would corrupt incompressible fragments that went out uncompressed.
    std::vector<std::byte> fragData;
    if (header.flags & FLAG_COMPRESSED) {
        std::vector<std::byte> dec(config_.receiveBuffer);
        const std::size_t n = decompress(payload, size, dec.data(), dec.size());
        dec.resize(n);
        if (n == 0 || dec.size() > config_.maxMessageSize) {
            partials_.erase(firstSeq);
            return reject();
        }
        fragData = std::move(dec);
    } else {
        fragData.assign(payload, payload + size);
    }
    // Bound assembled message size (on the decoded size).
    std::size_t assembledSoFar = 0;
    for (const auto& f : partial.fragments) assembledSoFar += f.data.size();
    if (assembledSoFar + fragData.size() > config_.maxMessageSize) {
        partials_.erase(firstSeq);
        return reject();
    }
    bool exists = false;
    for (auto& f : partial.fragments) {
        if (f.index == header.fragmentIndex) { exists = true; break; }
    }
    if (!exists) partial.fragments.push_back({header.fragmentIndex, std::move(fragData)});

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
            // Fragments were already decompressed individually by process_data()
            // from their own wire flags; just hand the assembled bytes through.
            deliver_message(std::move(message));
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

void ReliableTransport::sweep_stale_buffers(std::chrono::steady_clock::time_point now) {
    // Global, age-based sweep that runs on the update() cadence regardless of
    // which datagram just arrived. Without it, abandoned partials (a message
    // ended at fragment 1 of N and no more ever came) and stale out-of-order
    // singletons stayed in their tables until the exact same firstSeq was
    // re-touched — a hostile flood of unique first fragments grew partials_
    // without bound. Run it periodically, never under the packet-hot path.
    const auto interval = std::chrono::milliseconds(std::max<long long>(16ll,
        static_cast<long long>(config_.timeoutAfter.count()) / 4));
    if (lastSweep_ != std::chrono::steady_clock::time_point{} &&
        (now - lastSweep_) < interval) {
        return;
    }
    lastSweep_ = now;

    // Evict abandoned partials by age (the core fix). Without this sweep, a
    // message that ended at fragment 1 of N and never resumed would linger in
    // partials_ until the exact same firstSeq was re-touched; a hostile flood
    // of unique first-fragments grew the table without bound.
    for (auto it = partials_.begin(); it != partials_.end();) {
        if (now - it->second.received > config_.timeoutAfter) {
            ++droppedFragments_;
            it = partials_.erase(it);
        } else {
            ++it;
        }
    }
    // Out-of-order singletons have no per-entry timestamp; keep the reorder
    // table bounded as a hard safety net (process_data already caps insertion,
    // this is belt-and-braces). check_timeout clears everything on timeout.
    while (reorderBuffer_.size() > config_.maxReorderEntries) {
        auto oldest = reorderBuffer_.begin();
        ++droppedFragments_;
        reorderBuffer_.erase(oldest);
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
            // Repeat the EXACT wire flags that were sent originally. If the
            // fragment went on the wire uncompressed (compression did not
            // shrink it), wireFlags has no FLAG_COMPRESSED bit, so the
            // retransmission replays raw bytes as raw — the receiver never
            // tries to decompress plain data (the P0 corruption bug).
            header.flags = packet.wireFlags;
            // Repeat the ORIGINAL fragment metadata: retransmitting a fragment
            // of a multi-fragment message as "index 0 of 1" made the receiver
            // deliver a truncated message and the real one never completed.
            header.fragmentIndex = packet.fragmentIndex;
            header.fragmentCount = packet.fragmentCount;
            // Replay the byte-exact wire payload (compressed or raw).
            header.payloadSize = static_cast<std::uint16_t>(packet.wirePayload.size());
            send_packet(header, packet.wirePayload.data(), packet.wirePayload.size());
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

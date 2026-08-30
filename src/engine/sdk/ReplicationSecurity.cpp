// ReplicationSecurity.cpp — the only translation unit implementing
// IReplicationSecurity. Deterministic guards (section H): pre-allocation schema
// validation, per-connection anti-spam windows, amplification refusal, and an
// authoritative transaction journal that replays deterministically by sequence.

#include "engine/networking/IReplicationSecurity.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

struct ConnectionWindow {
    std::size_t count{ 0 };
    std::vector<std::size_t> request_sizes;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc) & 1));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

class ReplicationSecurityImpl final : public IReplicationSecurity {
public:
    explicit ReplicationSecurityImpl(const SecurityLimits& limits) : limits_(limits) {}

    bool register_schema(const PayloadSchema& schema, std::string& errorOut) override {
        if (schema.name.empty() || schema.max_size == 0) {
            errorOut = "security: invalid_schema_meta";
            return false;
        }
        for (const auto& f : schema.fields) {
            if (f.name.empty() || f.size == 0) { errorOut = "security: invalid_schema_field"; return false; }
            if (f.kind != FieldKind::FixedBytes && f.range_ok != 0 &&
                (f.max_value < f.min_value || f.max_value <= 0)) {
                errorOut = "security: invalid_schema_range";
                return false;
            }
        }
        if (schemas_.count(schema.name) != 0) { errorOut = "security: schema_duplicate"; return false; }
        schemas_[schema.name] = schema;
        return true;
    }

    bool validate(const std::string& schemaName, const std::uint8_t* data,
                  std::size_t size, std::string& errorOut) const override {
        const auto it = schemas_.find(schemaName);
        if (it == schemas_.end()) { errorOut = "security: unknown_schema"; return false; }
        const PayloadSchema& schema = it->second;
        if (size > schema.max_size) { errorOut = "security: size_exceeds_schema"; return false; }
        for (const auto& f : schema.fields) {
            if (f.kind == FieldKind::FixedBytes) {
                if (size < f.size) { errorOut = "security: field_out_of_bounds"; return false; }
                continue;
            }
            const std::size_t width = field_width(f.kind);
            if (size < width) { errorOut = "security: field_out_of_bounds"; return false; }
            if (f.range_ok == 0) continue;  // campo sem verificação de faixa
            const std::int64_t value = decode(data, f.kind);
            if (value < f.min_value || value > f.max_value) {
                errorOut = "security: field_range_violation";
                return false;
            }
        }
        return true;
    }

    static std::size_t field_width(FieldKind kind) noexcept {
        switch (kind) {
            case FieldKind::U8: return 1;
            case FieldKind::U16: return 2;
            case FieldKind::U32:
            case FieldKind::I32:
            case FieldKind::F32: return 4;
            default: return 1;
        }
    }

    static std::int64_t decode(const std::uint8_t* data, FieldKind kind) noexcept {
        switch (kind) {
            case FieldKind::U8: return static_cast<std::int64_t>(data[0]);
            case FieldKind::U16: return static_cast<std::int64_t>(data[0] | (static_cast<std::uint32_t>(data[1]) << 8));
            case FieldKind::U32: {
                std::uint32_t raw = 0;
                for (int i = 0; i < 4; ++i) raw |= static_cast<std::uint32_t>(data[i]) << (8 * i);
                return static_cast<std::int64_t>(raw & 0xFFFFFFFFu);
            }
            case FieldKind::I32: {
                std::uint32_t raw = 0;
                for (int i = 0; i < 4; ++i) raw |= static_cast<std::uint32_t>(data[i]) << (8 * i);
                return static_cast<std::int32_t>(raw);
            }
            default: return 0;
        }
    }

    bool advance_window(std::uint64_t now_ms) override {
        const auto win = now_ms / limits_.window_millis;
        const auto it = activeWindows_.find(currentWindowId_);
        if (it != activeWindows_.end() && currentWindowId_ != win) {
            activeWindows_.erase(it);  // purge the previous window's counters
        }
        currentWindowId_ = win;
        if (activeWindows_.count(currentWindowId_) == 0) activeWindows_[currentWindowId_] = {};
        return true;
    }

    bool observe_incoming(std::uint64_t connection_id, std::size_t bytes) override {
        if (bytes > limits_.max_payload) { ++dropped_; return false; }
        auto& win = activeWindows_[currentWindowId_];
        auto& cw = win[connection_id];
        if (cw.count >= limits_.max_messages_per_window) { ++dropped_; return false; }
        ++cw.count;
        cw.request_sizes.push_back(bytes);
        if (cw.request_sizes.size() > 16) cw.request_sizes.erase(cw.request_sizes.begin());
        return true;
    }

    bool amplification_ok(std::uint64_t connection_id, std::size_t req_bytes,
                          std::size_t resp_bytes) const override {
        if (!limits_.amplification_guard) return true;
        const auto winIt = activeWindows_.find(currentWindowId_);
        if (winIt == activeWindows_.end()) return resp_bytes <= req_bytes * limits_.max_response_ratio;
        const auto connIt = winIt->second.find(connection_id);
        if (connIt == winIt->second.end()) return resp_bytes <= req_bytes * limits_.max_response_ratio;
        std::size_t req = req_bytes;
        if (!connIt->second.request_sizes.empty()) req = connIt->second.request_sizes.back();
        if (req == 0) req = 1;
        return resp_bytes <= req * limits_.max_response_ratio;
    }

    bool journal_record(const std::string& kind, const std::uint8_t* data,
                        std::size_t size, std::uint64_t tick,
                        std::uint64_t& out_sequence, std::string& errorOut) override {
        if (kind.empty()) { errorOut = "security: journal_empty_kind"; return false; }
        JournalEntry entry;
        entry.tick = tick;
        entry.sequence = ++lastSequence_;
        entry.kind = kind;
        entry.data.assign(data, data + size);
        entry.crc = crc32(data, size);
        journal_.push_back(std::move(entry));
        // Bounded recovery window: drop the oldest entry when the cap is hit,
        // so a long-running authoritative server never grows the journal
        // without bound. Sequences stay strictly increasing (no reuse);
        // journal_since/replay simply iterate the retained window. Deque
        // pop_front keeps the trim O(1) even at the cap.
        if (journal_.size() > limits_.journal_max_entries) {
            journal_.pop_front();
        }
        out_sequence = lastSequence_;
        return true;
    }

    std::vector<JournalEntry> journal_since(std::uint64_t after_sequence) const override {
        std::vector<JournalEntry> out;
        for (const auto& e : journal_) {
            if (e.sequence > after_sequence) out.push_back(e);
        }
        return out;
    }

    std::size_t replay(std::uint64_t after_sequence, ReplayConsumer consumer) const override {
        std::size_t emitted = 0;
        for (const auto& e : journal_) {
            if (e.sequence <= after_sequence || !consumer) continue;
            if (crc32(e.data.data(), e.data.size()) != e.crc) continue;  // corruption skip
            consumer(e);
            ++emitted;
        }
        return emitted;
    }

    std::size_t journal_size() const override { return journal_.size(); }
    std::uint64_t last_journal_sequence() const override { return lastSequence_; }
    std::size_t dropped_spam() const noexcept override { return dropped_; }
    std::size_t dropped_amplification() const noexcept override { return droppedAmp_; }

    bool reset(std::string&) override {
        schemas_.clear();
        activeWindows_.clear();
        journal_.clear();
        lastSequence_ = 0;
        dropped_ = 0;
        droppedAmp_ = 0;
        return true;
    }

private:
    SecurityLimits limits_;
    std::map<std::string, PayloadSchema> schemas_;   // deterministic order
    using WindowMap = std::unordered_map<std::uint64_t, ConnectionWindow>;
    std::unordered_map<std::uint64_t, WindowMap> activeWindows_;
    std::uint64_t currentWindowId_{ 0 };
    // Deque (not vector) so the bounded-window trim (pop_front) is O(1);
    // journal_since/replay iterate it in sequence order.
    std::deque<JournalEntry> journal_;
    std::uint64_t lastSequence_{ 0 };
    std::size_t dropped_{ 0 };
    mutable std::size_t droppedAmp_{ 0 };
};

}  // namespace

std::unique_ptr<IReplicationSecurity> create_replication_security(
    const SecurityLimits& limits, std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<ReplicationSecurityImpl>(limits);
}

}  // namespace networking
}  // namespace engine
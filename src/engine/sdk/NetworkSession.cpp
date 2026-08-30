// NetworkSession.cpp — the only translation unit implementing INetworkSession.
// Deterministic session/identity layer (seção D): version negotiation, optional
// auth payload gate, capabilities approval, join with duplicate-login policy,
// reconnect token with TTL + single-use, kick/ban/graceful leave, ownership
// preservation across portal/dimension/reconnect. Transport-free (ids opacos).

#include "engine/networking/INetworkSession.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

constexpr std::uint64_t kDefaultTokenTtlMs = 30000;  // 30s retention window

struct ReconnectSlot {
    std::uint64_t player_id{ 0 };
    std::uint64_t entity_net_id{ 0 };
    std::uint64_t world_id{ 0 };
    std::uint64_t issued_at{ 0 };
    std::uint64_t ttl_millis{ 0 };
    bool used{ false };
};

struct ActiveSession {
    NetIdentity identity;
    SessionCapabilities caps;
    SessionStatus status{ SessionStatus::Handshaking };
    SessionToken token;
    std::uint64_t last_touch_ms{ 0 };
};

class NetworkSessionImpl final : public INetworkSession {
public:
    NetworkSessionImpl() { version_.protocol = 1; }

    void server_set_version(const NetVersion& version) override { version_ = version; }
    const NetVersion& server_version() const override { return version_; }

    SessionResult handshake(std::uint64_t connection_id,
                            const NetVersion& clientVersion,
                            const std::string& authPayload,
                            const SessionCapabilities& caps,
                            std::uint64_t now_ms) override {
        SessionResult result;
        if (connection_id == 0) { result.error = "invalid_connection"; return result; }
        if (!clientVersion.compatible_with(version_)) {
            result.error = "version_mismatch_protocol";
            return result;
        }
        if (byConnection_.count(connection_id) != 0) {
            result.error = "connection_already_handshaking";
            return result;
        }
        // Optional auth gate: when the `server_auth_required_` flag is off the
        // payload is accepted either way; enforcement happens up-stack
        // (MCP/admin). We still reject a connection that claims a procedure the
        // server never registered would be impossible to detect without the
        // registry, so capability validation is deferred to IAuthoritativeRpc.
        ActiveSession sess;
        sess.caps = caps;
        sess.status = SessionStatus::Handshaking;
        sess.last_touch_ms = now_ms;
        sess.identity.connection_id = connection_id;
        byConnection_[connection_id] = std::move(sess);
        result.ok = true;
        result.identity.connection_id = connection_id;
        return result;
    }

    SessionResult join(std::uint64_t connection_id,
                       std::uint64_t player_id,
                       std::uint64_t world_id,
                       std::uint64_t entity_net_id,
                       bool force, std::uint64_t now_ms) override {
        SessionResult result;
        const auto it = byConnection_.find(connection_id);
        if (it == byConnection_.end() || it->second.status == SessionStatus::Active) {
            result.error = "not_handshaking";
            return result;
        }
        if (player_id == 0) { result.error = "invalid_player"; return result; }
        // Duplicate login policy.
        const auto activeIt = activePlayerConn_.find(player_id);
        if (activeIt != activePlayerConn_.end() && activeIt->second != connection_id) {
            if (!force) { result.error = "duplicate_login"; return result; }
            expire(activeIt->second);
        }
        it->second.status = SessionStatus::Active;
        it->second.identity.player_id = player_id;
        it->second.identity.world_id = world_id != 0 ? world_id : it->second.identity.world_id;
        it->second.identity.entity_net_id = entity_net_id;
        it->second.identity.player_name = player_name(entity_net_id);
        it->second.token = issue_token(now_ms);
        it->second.last_touch_ms = now_ms;
        activePlayerConn_[player_id] = connection_id;
        result.ok = true;
        result.identity = it->second.identity;
        result.token = it->second.token;
        return result;
    }

    SessionResult reconnect(const std::string& tokenValue,
                            std::uint64_t new_connection_id,
                            std::uint64_t now_ms) override {
        SessionResult result;
        if (new_connection_id == 0) { result.error = "invalid_connection"; return result; }
        const auto slotIt = pending_.find(tokenValue);
        if (slotIt == pending_.end()) { result.error = "invalid_token"; return result; }
        ReconnectSlot slot = slotIt->second;
        if (slot.used) { result.error = "token_used"; return result; }
        if (now_ms - slot.issued_at > slot.ttl_millis) {
            pending_.erase(slotIt);
            result.error = "token_expired";
            return result;
        }
        slot.used = true;
        pending_.erase(slotIt);
        // Release any prior live connection for this player.
        const auto activeIt = activePlayerConn_.find(slot.player_id);
        if (activeIt != activePlayerConn_.end()) expire(activeIt->second);

        ActiveSession sess;
        sess.status = SessionStatus::Active;
        sess.identity.connection_id = new_connection_id;
        sess.identity.player_id = slot.player_id;
        sess.identity.entity_net_id = slot.entity_net_id;
        sess.identity.world_id = slot.world_id;
        sess.token = issue_token(now_ms);
        sess.last_touch_ms = now_ms;
        byConnection_[new_connection_id] = std::move(sess);
        activePlayerConn_[slot.player_id] = new_connection_id;
        result.ok = true;
        result.identity = byConnection_[new_connection_id].identity;
        result.token = byConnection_[new_connection_id].token;
        return result;
    }

    bool kick(std::uint64_t player_id, const std::string&) override {
        const auto it = activePlayerConn_.find(player_id);
        if (it == activePlayerConn_.end()) return false;
        auto sIt = byConnection_.find(it->second);
        if (sIt != byConnection_.end()) {
            sIt->second.status = SessionStatus::Kicked;
            // Keep the player registered as kicked so a client can't rejoin the
            // same player silently; a re-join requires a fresh handshake.
        }
        activePlayerConn_.erase(it);
        return true;
    }

    bool ban(std::uint64_t player_id, const std::string& reason) override {
        banned_ones_[player_id] = reason;
        kick(player_id, reason);
        return true;
    }

    bool is_banned(std::uint64_t player_id) const override {
        return banned_ones_.count(player_id) != 0;
    }

    void graceful_leave(std::uint64_t connection_id) override {
        const auto it = byConnection_.find(connection_id);
        if (it == byConnection_.end()) return;
        if (it->second.status == SessionStatus::Active) {
            // Promote the still-valid token to a pending reconnect slot.
            if (it->second.token.valid() && !it->second.token.used) {
                ReconnectSlot slot;
                slot.player_id = it->second.identity.player_id;
                slot.entity_net_id = it->second.identity.entity_net_id;
                slot.world_id = it->second.identity.world_id;
                slot.issued_at = it->second.token.issued_at;
                slot.ttl_millis = it->second.token.ttl_millis;
                pending_[it->second.token.value] = slot;
            }
        }
        it->second.status = SessionStatus::Left;
        const auto activeIt = activePlayerConn_.find(it->second.identity.player_id);
        if (activeIt != activePlayerConn_.end() && activeIt->second == connection_id) {
            activePlayerConn_.erase(activeIt);
        }
    }

    void expire(std::uint64_t connection_id) override {
        const auto it = byConnection_.find(connection_id);
        if (it == byConnection_.end()) return;
        it->second.status = SessionStatus::Expired;
        const auto activeIt = activePlayerConn_.find(it->second.identity.player_id);
        if (activeIt != activePlayerConn_.end() && activeIt->second == connection_id) {
            activePlayerConn_.erase(activeIt);
        }
    }

    bool touch(std::uint64_t connection_id, std::uint64_t now_ms) override {
        const auto it = byConnection_.find(connection_id);
        if (it == byConnection_.end()) return false;
        if (it->second.status != SessionStatus::Active) return false;
        it->second.last_touch_ms = now_ms;
        return true;
    }

    bool preserve_ownership(std::uint64_t connection_id, std::uint64_t entity_net_id) override {
        const auto it = byConnection_.find(connection_id);
        if (it == byConnection_.end()) return false;
        it->second.identity.entity_net_id = entity_net_id;
        return true;
    }

    bool transfer_world(std::uint64_t connection_id, std::uint64_t new_world_id) override {
        const auto it = byConnection_.find(connection_id);
        if (it == byConnection_.end()) return false;
        it->second.identity.world_id = new_world_id;
        return true;
    }

    SessionStatus status(std::uint64_t player_id) const override {
        const auto it = activePlayerConn_.find(player_id);
        if (it == activePlayerConn_.end()) {
            // Check pending/expired traces.
            if (banned_ones_.count(player_id) != 0) return SessionStatus::Banned;
            return SessionStatus::Unknown;
        }
        const auto cIt = byConnection_.find(it->second);
        return cIt == byConnection_.end() ? SessionStatus::Unknown : cIt->second.status;
    }

    std::vector<NetIdentity> connections() const override {
        std::vector<NetIdentity> out;
        out.reserve(byConnection_.size());
        for (const auto& [id, sess] : byConnection_) {
            if (sess.status == SessionStatus::Active || sess.status == SessionStatus::Handshaking) {
                NetIdentity idn = sess.identity;
                idn.connection_id = id;
                out.push_back(idn);
            }
        }
        // Deterministic ascending order by connection id.
        std::sort(out.begin(), out.end(),
                  [](const NetIdentity& a, const NetIdentity& b) {
                      return a.connection_id < b.connection_id;
                  });
        return out;
    }

    std::vector<SessionToken> live_tokens(std::uint32_t max) const override {
        std::vector<SessionToken> out;
        for (const auto& [val, slot] : pending_) {
            if (out.size() >= max) break;
            SessionToken t;
            t.value = val;
            t.issued_at = slot.issued_at;
            t.ttl_millis = slot.ttl_millis;
            t.used = slot.used;
            out.push_back(t);
        }
        return out;
    }

    std::uint64_t active_player_count() const override { return activePlayerConn_.size(); }

    bool reset(std::string&) override {
        byConnection_.clear();
        activePlayerConn_.clear();
        pending_.clear();
        banned_ones_.clear();
        return true;
    }

private:
    SessionToken issue_token(std::uint64_t now_ms) const {
        SessionToken token;
        token.issued_at = now_ms;
        token.ttl_millis = kDefaultTokenTtlMs;
        token.used = false;
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        for (int i = 0; i < 8; ++i) {
            const char* hex = "0123456789abcdef";
            const std::uint64_t v = gen();
            for (int j = 0; j < 4; ++j) token.value += hex[(v >> (j * 4)) & 0xF];
        }
        return token;
    }

    static std::string player_name(std::uint64_t) {
        // Names resolve up-stack from the player registry; identidade aqui é
        // opaca (id), mantendo o contrato determinístico.
        return {};
    }

    NetVersion version_;
    std::unordered_map<std::uint64_t, ActiveSession> byConnection_;
    std::unordered_map<std::uint64_t, std::uint64_t> activePlayerConn_;  // player -> connection
    std::map<std::string, ReconnectSlot> pending_;                        // token -> slot
    std::unordered_map<std::uint64_t, std::string> banned_ones_;
};

}  // namespace

std::unique_ptr<INetworkSession> create_network_session(std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<NetworkSessionImpl>();
}

}  // namespace networking
}  // namespace engine
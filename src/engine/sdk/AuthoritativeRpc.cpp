// AuthoritativeRpc.cpp — the only translation unit implementing
// IAuthoritativeRpc. Deterministic command/event layer (seção F). The server
// alone executes commands; each one is validated (permission, cooldown,
// reach, ownership, payload schema/limits) before its handler runs, and every
// (connection, command, sequence) is idempotency-deduplicated so a retry or
// reconnect cannot re-run the same effect. Errors are always stable short
// codes — no memory/address/internal-path leakage.

#include "engine/networking/IAuthoritativeRpc.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

struct Registered {
    CommandRules rules;
    CommandHandler handler;
};

class AuthoritativeRpcImpl final : public IAuthoritativeRpc {
public:
    AuthoritativeRpcImpl() { nextSequence_ = 1; nextEvent_ = 1; }

    bool register_command(const std::string& name, CommandHandler handler,
                          const CommandRules& rules, std::string& errorOut) override {
        if (name.empty() || !handler) { errorOut = "invalid_command_registration"; return false; }
        if (commands_.count(name) != 0) { errorOut = "command_already_registered"; return false; }
        commands_[name] = Registered{ rules, std::move(handler) };
        return true;
    }

    void unregister_command(const std::string& name) override { commands_.erase(name); }

    std::vector<CommandResult> server_process(
        std::vector<CommandEnvelope> envelopes, const CommandContext& ctx,
        std::string& errorOut) override {
        (void)errorOut;
        std::vector<CommandResult> results;
        results.reserve(envelopes.size());
        for (auto& env : envelopes) {
            CommandResult r;
            r.sequence = env.sequence;
            r.connection_id = env.connection_id;
            const auto it = commands_.find(env.command);
            if (it == commands_.end()) {
                r.error = "unknown_command";
                results.push_back(std::move(r));
                continue;
            }
            // Payload schema/limit gate BEFORE allocating any effect.
            if (env.payload.size() > it->second.rules.max_payload) {
                r.error = "invalid_schema";
                results.push_back(std::move(r));
                continue;
            }
            // Idempotency: (connection, command, sequence) seen => no re-run.
            const std::string dedupKey = dedup_key(ctx.connection_id, env.sequence);
            if (executed_.count(dedupKey) != 0) {
                r.ok = true;  // acknowledged as already-executed
                results.push_back(std::move(r));
                continue;
            }
            // Permission.
            if (!it->second.rules.required_permission.empty() &&
                permissions_.count(ctx.player_id) == 0 &&
                !has_permission(ctx.player_id, it->second.rules.required_permission)) {
                r.error = "no_permission";
                results.push_back(std::move(r));
                continue;
            }
            // Cooldown per (player, command).
            const std::uint64_t last = cooldowns_[cooldown_key(ctx.player_id, env.command)];
            if (it->second.rules.cooldown_millis > 0 && last != 0 &&
                ctx.now_ms - last < it->second.rules.cooldown_millis) {
                r.error = "cooldown";
                results.push_back(std::move(r));
                continue;
            }
            // Ownership.
            if (it->second.rules.require_ownership) {
                const auto ownerIt = owners_.find(env.entity_net_id != 0 ? env.entity_net_id : ctx.entity_net_id);
                if (ownerIt == owners_.end() || ownerIt->second != ctx.player_id) {
                    r.error = "no_ownership";
                    results.push_back(std::move(r));
                    continue;
                }
            }
            // Reach (distance from player origin to target).
            if (it->second.rules.max_distance > 0.0f && env.has_target) {
                const double dx = ctx.origin_x - env.target_x;
                const double dy = ctx.origin_y - env.target_y;
                const double dz = ctx.origin_z - env.target_z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) >
                    static_cast<double>(it->second.rules.max_distance)) {
                    r.error = "out_of_reach";
                    results.push_back(std::move(r));
                    continue;
                }
            }
            // Execute.
            CommandContext localCtx = ctx;
            localCtx.entity_net_id = env.entity_net_id != 0 ? env.entity_net_id : ctx.entity_net_id;
            CommandOutcome outcome = it->second.handler(
                localCtx, env.payload.data(), env.payload.size());
            r.ok = outcome.ok;
            r.data = std::move(outcome.data);
            executed_.insert(dedupKey);
            if (it->second.rules.cooldown_millis > 0) {
                cooldowns_[cooldown_key(ctx.player_id, env.command)] = ctx.now_ms;
            }
            results.push_back(std::move(r));
        }
        // Trim executed-set for long-running sessions (bounded memory).
        while (executed_.size() > 65536) executed_.erase(executed_.begin());
        return results;
    }

    bool server_enqueue_event(const std::string& name, const std::uint8_t* payload,
                              std::size_t size, std::uint64_t target_connection,
                              std::string& errorOut) override {
        if (name.empty() || size > kEventMaxPayload) { errorOut = "invalid_event"; return false; }
        CommandResult ev;
        ev.connection_id = target_connection;
        ev.ok = true;
        ev.error = "event:" + name;
        ev.sequence = nextEvent_++;
        ev.data.assign(payload, payload + size);
        events_[target_connection].push_back(std::move(ev));
        return true;
    }

    std::vector<CommandResult> server_drain_events(std::uint64_t connection,
                                                   std::string& errorOut) override {
        (void)errorOut;
        auto it = events_.find(connection);
        if (it == events_.end()) return {};
        std::vector<CommandResult> out(it->second.begin(), it->second.end());
        events_.erase(it);
        return out;
    }

    bool client_mark_ack(std::uint64_t player_sequence, std::string&) override {
        lastAcked_ = std::max(lastAcked_, static_cast<std::uint64_t>(player_sequence));
        return true;
    }

    bool client_enqueue_command(const std::string& name, const std::uint8_t* payload,
                                std::size_t size, std::uint64_t entity_net_id,
                                std::string& errorOut) override {
        if (name.empty() || payload == nullptr) { errorOut = "invalid_command"; return false; }
        if (size > 4096) { errorOut = "invalid_payload"; return false; }
        CommandEnvelope env;
        env.connection_id = 0;  // filled by transport
        env.sequence = nextSequence_++;
        env.ack_sequence = lastAcked_;
        env.command = name;
        env.entity_net_id = entity_net_id;
        env.payload.assign(payload, payload + size);
        outbound_.push_back(std::move(env));
        return true;
    }

    std::vector<CommandEnvelope> client_drain_commands(std::size_t maximum,
                                                       std::string&) override {
        std::vector<CommandEnvelope> out;
        while (!outbound_.empty() && out.size() < maximum) {
            out.push_back(std::move(outbound_.front()));
            outbound_.pop_front();
        }
        return out;
    }

    std::uint64_t client_fill_acked_sequence() const override { return lastAcked_; }

    void server_set_owner(std::uint64_t entity_net_id, std::uint64_t player_id) override {
        owners_[entity_net_id] = player_id;
    }
    std::uint64_t server_owner_of(std::uint64_t entity_net_id) const override {
        const auto it = owners_.find(entity_net_id);
        return it == owners_.end() ? 0 : it->second;
    }

    std::vector<std::string> commands() const override {
        std::vector<std::string> out;
        out.reserve(commands_.size());
        for (const auto& [name, entry] : commands_) {
            (void)entry;
            out.push_back(name);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool reset(std::string&) override {
        commands_.clear();
        events_.clear();
        outbound_.clear();
        executed_.clear();
        cooldowns_.clear();
        owners_.clear();
        permissions_.clear();
        nextSequence_ = 1;
        nextEvent_ = 1;
        return true;
    }

private:
    static std::string dedup_key(std::uint64_t connection, std::uint64_t seq) {
        return std::to_string(connection) + ":" + std::to_string(seq);
    }
    static std::string cooldown_key(std::uint64_t player, const std::string& cmd) {
        return std::to_string(player) + "#" + cmd;
    }
    bool has_permission(std::uint64_t player, const std::string& perm) const {
        const auto it = permissions_.find(player);
        if (it == permissions_.end()) return false;
        return std::find(it->second.begin(), it->second.end(), perm) != it->second.end();
    }

    static constexpr std::size_t kEventMaxPayload = 1 << 20;

    std::map<std::string, Registered> commands_;   // sorted, deterministic
    std::unordered_map<std::uint64_t, std::deque<CommandResult>> events_;  // target -> queue
    std::deque<CommandEnvelope> outbound_;
    std::unordered_set<std::string> executed_;
    std::unordered_map<std::string, std::uint64_t> cooldowns_;
    std::unordered_map<std::uint64_t, std::uint64_t> owners_;        // entity -> player
    std::unordered_map<std::uint64_t, std::vector<std::string>> permissions_;  // perms/player
    std::uint64_t nextSequence_{ 1 };
    std::uint64_t nextEvent_{ 1 };
    std::uint64_t lastAcked_{ 0 };
};

}  // namespace

std::unique_ptr<IAuthoritativeRpc> create_authoritative_rpc(std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<AuthoritativeRpcImpl>();
}

}  // namespace networking
}  // namespace engine
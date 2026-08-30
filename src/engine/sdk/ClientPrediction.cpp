// ClientPrediction.cpp — the only translation unit implementing
// IClientPrediction. Deterministic client-side prediction, server
// reconciliation with input replay, remote-snapshot interpolation, and
// transactional block-edit rollback (section G). Transport-free.

#include "engine/networking/IClientPrediction.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace networking {
namespace {

constexpr double kDefaultExtrapolateSeconds = 0.10;

struct PendingInput {
    PredictionInput input;
};

struct RemoteHistory {
    // tick-sorted snapshots for this entity (server time, not local dt).
    std::vector<RemoteSnapshot> snapshots;
};

PredictedPose default_step(const PredictedPose& from, const PredictionInput& in) {
    PredictedPose next = from;
    const float speed = 6.0f;
    const float dt = std::max(in.dt, 0.0f);
    next.x += static_cast<double>(in.move_x * speed * dt);
    next.z += static_cast<double>(in.move_z * speed * dt);
    if (in.jump) next.y += 0.001;  // marker only; the real controller refines
    return next;
}

class ClientPredictionImpl final : public IClientPrediction {
public:
    ClientPredictionImpl() { step_ = default_step; }

    void set_step(PredictionStep step) override {
        step_ = step ? std::move(step) : PredictionStep(default_step);
    }

    PredictionInput predict(float dt, float move_x, float move_z, bool jump) override {
        PredictionInput in;
        in.sequence = nextSequence_++;
        in.dt = dt;
        in.move_x = move_x;
        in.move_z = move_z;
        in.jump = jump;
        pose_ = step_(pose_, in);
        pending_.push_back(PendingInput{ in });
        return in;
    }

    void server_ack(std::uint32_t acked_sequence) override {
        while (!pending_.empty() && pending_.front().input.sequence <= acked_sequence) {
            pending_.pop_front();
        }
    }

    ReconcileResult reconcile(const PredictedPose& authoritative,
                              std::uint32_t acked_sequence) override {
        ReconcileResult r;
        server_ack(acked_sequence);
        r.error = static_cast<float>(
            std::sqrt((pose_.x - authoritative.x) * (pose_.x - authoritative.x) +
                      (pose_.y - authoritative.y) * (pose_.y - authoritative.y) +
                      (pose_.z - authoritative.z) * (pose_.z - authoritative.z)));
        if (r.error > kThreshold) {
            r.corrected = true;
            pose_ = authoritative;
            // Replay unacked inputs forward from the authoritative state.
            std::vector<PendingInput> replay(pending_.begin(), pending_.end());
            for (const auto& p : replay) {
                pose_ = step_(pose_, p.input);
                ++r.replayed_inputs;
            }
        }
        return r;
    }

    bool push_remote_snapshot(const RemoteSnapshot& snapshot) override {
        auto& hist = remote_[snapshot.entity_net_id];
        // Insert sorted by tick, replacing duplicates.
        auto it = std::lower_bound(hist.snapshots.begin(), hist.snapshots.end(), snapshot,
                                   [](const RemoteSnapshot& a, const RemoteSnapshot& b) {
                                       return a.tick < b.tick;
                                   });
        if (it != hist.snapshots.end() && it->tick == snapshot.tick) {
            *it = snapshot;
        } else {
            hist.snapshots.insert(it, snapshot);
        }
        if (hist.snapshots.size() > 64) hist.snapshots.erase(hist.snapshots.begin());
        return true;
    }

    bool sample_remote(std::uint64_t entity_net_id, double render_time,
                       PredictedPose& out) override {
        const auto hit = remote_.find(entity_net_id);
        if (hit == remote_.end() || hit->second.snapshots.empty()) return false;
        const auto& snaps = hit->second.snapshots;
        if (snaps.size() == 1) { out = snaps.front().pose; return true; }
        const RemoteSnapshot* older = nullptr;
        const RemoteSnapshot* newer = nullptr;
        for (std::size_t i = 0; i < snaps.size(); ++i) {
            if (snaps[i].server_time <= render_time) older = &snaps[i];
            else { newer = &snaps[i]; break; }
        }
        if (older == nullptr) { older = &snaps.front(); }
        if (newer == nullptr) {
            // Extrapolate briefly from the newest known velocity-less record.
            newer = older;
        }
        double denom = newer->server_time - older->server_time;
        double alpha = denom > 1e-9 ? (render_time - older->server_time) / denom : 0.0;
        if (alpha < 0.0) alpha = 0.0;
        if (alpha > 1.0) {
            if (render_time - older->server_time > kDefaultExtrapolateSeconds) alpha = 1.0;
            // allow short extrapolation beyond: clamp linear
            if (alpha > 2.0) alpha = 2.0;
        }
        out.x = older->pose.x + (newer->pose.x - older->pose.x) * alpha;
        out.y = older->pose.y + (newer->pose.y - older->pose.y) * alpha;
        out.z = older->pose.z + (newer->pose.z - older->pose.z) * alpha;
        out.yaw = newer->pose.yaw;
        return true;
    }

    std::uint32_t predict_block(BlockEditKind kind, int x, int y, int z,
                                std::uint32_t block_before,
                                std::uint32_t block_after) override {
        const std::uint32_t seq = nextSequence_++;
        BlockPrediction bp;
        bp.sequence = seq;
        bp.kind = kind;
        bp.x = x;
        bp.y = y;
        bp.z = z;
        bp.block_before = block_before;
        bp.block_after = block_after;
        bp.server_accepted = false;
        pendingBlocks_.emplace(seq, bp);
        return seq;
    }

    bool confirm_block(std::uint32_t sequence, bool accepted) override {
        auto it = pendingBlocks_.find(sequence);
        if (it == pendingBlocks_.end()) return false;
        BlockPrediction& bp = it->second;
        bp.server_accepted = accepted;
        if (!accepted) {
            // Transactional rollback: signal the caller to restore before-state.
            RollbackSignal sig;
            sig.sequence = sequence;
            sig.kind = bp.kind;
            sig.x = bp.x;
            sig.y = bp.y;
            sig.z = bp.z;
            sig.restore_block = bp.block_before;
            rollbacks_.push_back(sig);
        }
        pendingBlocks_.erase(it);
        return true;
    }

    std::vector<RollbackSignal> drain_rollbacks() override {
        std::vector<RollbackSignal> out = std::move(rollbacks_);
        rollbacks_.clear();
        return out;
    }

    const PredictedPose& pose() const override { return pose_; }
    std::uint32_t next_sequence() const override { return nextSequence_; }
    std::size_t pending_input_count() const override { return pending_.size(); }
    std::size_t pending_block_edits() const override { return pendingBlocks_.size(); }

    bool reset(std::string&) override {
        pose_ = {};
        pending_.clear();
        remote_.clear();
        pendingBlocks_.clear();
        rollbacks_.clear();
        nextSequence_ = 1;
        return true;
    }

private:
    static constexpr float kThreshold = 0.05f;

    PredictionStep step_;
    PredictedPose pose_;
    std::uint32_t nextSequence_{ 1 };
    std::deque<PendingInput> pending_;
    std::unordered_map<std::uint64_t, RemoteHistory> remote_;
    std::map<std::uint32_t, BlockPrediction> pendingBlocks_;
    std::vector<RollbackSignal> rollbacks_;
};

}  // namespace

std::unique_ptr<IClientPrediction> create_client_prediction(std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<ClientPredictionImpl>();
}

}  // namespace networking
}  // namespace engine
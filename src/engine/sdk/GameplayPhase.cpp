#include "engine/gameplay/IGameplayPhase.hpp"
#include <algorithm>

namespace engine::gameplay {
namespace {
class Phase final : public IGameplayPhase {
public:
    bool configure(const std::vector<GameplayDomain>& domains, std::string& errorOut) override {
        std::vector<GameplayDomainStatus> next;
        for (const auto domain : domains) {
            if (std::find_if(next.begin(), next.end(), [domain](const auto& item) { return item.domain == domain; }) != next.end()) {
                errorOut = "duplicate gameplay domain";
                return false;
            }
            next.push_back({domain, false, false, false, false});
        }
        if (next.empty()) { errorOut = "at least one gameplay domain is required"; return false; }
        status_ = std::move(next);
        errorOut.clear();
        return true;
    }

    bool mark_producer_bound(GameplayDomain domain) override { return mark(domain, &GameplayDomainStatus::producerBound); }
    bool mark_consumer_bound(GameplayDomain domain) override { return mark(domain, &GameplayDomainStatus::consumerBound); }
    bool mark_persistence_bound(GameplayDomain domain) override { return mark(domain, &GameplayDomainStatus::persistenceBound); }
    bool mark_replication_bound(GameplayDomain domain) override { return mark(domain, &GameplayDomainStatus::replicationBound); }

    bool complete(std::string& errorOut) const override {
        if (status_.empty()) { errorOut = "phase is not configured"; return false; }
        for (const auto& item : status_) {
            if (!item.producerBound || !item.consumerBound) { errorOut = "producer and consumer bindings are required"; return false; }
            if (requires_persistence(item.domain) && !item.persistenceBound) { errorOut = "persistence binding is required for a stateful domain"; return false; }
            if (requires_replication(item.domain) && !item.replicationBound) { errorOut = "replication binding is required for a replicated domain"; return false; }
        }
        errorOut.clear();
        return true;
    }

    std::vector<GameplayDomainStatus> status() const override { return status_; }
    void reset() override { status_.clear(); }

private:
    using Flag = bool GameplayDomainStatus::*;
    bool mark(GameplayDomain domain, Flag flag) {
        const auto it = std::find_if(status_.begin(), status_.end(), [domain](const auto& item) { return item.domain == domain; });
        if (it == status_.end()) return false;
        (*it).*flag = true;
        return true;
    }
    static bool requires_persistence(GameplayDomain domain) {
        return domain == GameplayDomain::Ecs || domain == GameplayDomain::Animation || domain == GameplayDomain::Worlds || domain == GameplayDomain::Multiplayer;
    }
    static bool requires_replication(GameplayDomain domain) {
        return domain == GameplayDomain::Ecs || domain == GameplayDomain::Physics || domain == GameplayDomain::Voxel || domain == GameplayDomain::Multiplayer || domain == GameplayDomain::Worlds;
    }
    std::vector<GameplayDomainStatus> status_;
};
}
std::unique_ptr<IGameplayPhase> create_gameplay_phase() { return std::make_unique<Phase>(); }
} // namespace engine::gameplay

// NavigationSchedulerBridge.cpp — the only TU implementing the public
// navigation scheduler bridge (Agente 4 §2 item 27 WIRING): enqueue path
// queries with priority/timeout/budget into the scheduler, dispatch the
// frame batch to the provider's async API, collect finished results.
// Pure std; the provider is injected (never owned).

#include "engine/navigation/INavigationSchedulerBridge.hpp"

#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/navigation/INavigationProvider.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace engine {
namespace navigation {
namespace {

struct PendingQuery {
    float startX{ 0.0f };
    float startY{ 0.0f };
    float startZ{ 0.0f };
    float goalX{ 0.0f };
    float goalY{ 0.0f };
    float goalZ{ 0.0f };
    std::uint64_t providerId{ 0 };   // id devolvido pelo begin_async_path
    bool dispatched = false;
    bool inFlight = false;
};

bool finite4(float v) { return std::isfinite(v); }

std::string serialize_path(const PathResult& path) {
    // JSON opaco determinístico: waypoints como array plano de números.
    std::string out = "{\"found\":";
    out += path.found ? "true" : "false";
    out += ",\"waypoints\":[";
    for (std::size_t i = 0; i < path.waypoints.size(); ++i) {
        if (i != 0) out += ",";
        out += std::to_string(path.waypoints[i]);
    }
    out += "]}";
    return out;
}

class NavigationSchedulerBridge final : public INavigationSchedulerBridge {
public:
    NavigationSchedulerBridge(IAsyncQueryScheduler* scheduler,
                              INavigationProvider* provider)
        : scheduler_(scheduler), provider_(provider) {}

    bool configure(std::size_t maxQueued, float maxBudgetSeconds,
                   std::string& errorOut) override {
        if (maxQueued == 0 || !finite4(maxBudgetSeconds) ||
            maxBudgetSeconds <= 0.0f) {
            errorOut = "navigation scheduler bridge: maxQueued and "
                       "maxBudgetSeconds must be > 0";
            return false;
        }
        if (!scheduler_->configure(maxQueued, errorOut)) return false;
        maxBudgetSeconds_ = maxBudgetSeconds;
        return true;
    }

    bool enqueue_path(std::uint64_t queryId, float priority,
                      float timeoutSeconds, float estimatedCost,
                      float startX, float startY, float startZ,
                      float goalX, float goalY, float goalZ,
                      std::string& errorOut) override {
        if (!finite4(startX) || !finite4(startY) || !finite4(startZ) ||
            !finite4(goalX) || !finite4(goalY) || !finite4(goalZ)) {
            errorOut = "navigation scheduler bridge: non-finite points";
            return false;
        }
        AsyncQuerySpec spec;
        spec.queryId = queryId;
        spec.priority = priority;
        spec.timeoutSeconds = timeoutSeconds;
        spec.estimatedCost = estimatedCost;
        if (!scheduler_->enqueue(spec, errorOut)) return false;
        PendingQuery pending;
        pending.startX = startX; pending.startY = startY; pending.startZ = startZ;
        pending.goalX = goalX; pending.goalY = goalY; pending.goalZ = goalZ;
        pending_.emplace(queryId, pending);
        return true;
    }

    bool cancel(std::uint64_t queryId) override {
        if (scheduler_->is_queued(queryId)) {
            // Ainda enfileirada: remove do scheduler e do mapa.
            scheduler_->cancel(queryId);
            pending_.erase(queryId);
            return true;
        }
        const auto found = pending_.find(queryId);
        if (found == pending_.end()) return false;
        if (found->second.inFlight) {
            // Em voo: propaga para o provider (join).
            provider_->cancel_async_path(found->second.providerId);
        }
        pending_.erase(found);
        return true;
    }

    std::vector<std::uint64_t> tick(float dt) override {
        return scheduler_->tick(dt);
    }

    std::vector<NavQueryResult> frame() override {
        std::vector<NavQueryResult> results;

        // 1) Despacha o lote do orçamento para o provider.
        const std::vector<std::uint64_t> batch =
            scheduler_->dispatch(maxBudgetSeconds_);
        for (const std::uint64_t queryId : batch) {
            const auto found = pending_.find(queryId);
            if (found == pending_.end()) continue;
            std::string error;
            const std::uint64_t providerId =
                provider_->begin_async_path(found->second.startX,
                                            found->second.startY,
                                            found->second.startZ,
                                            found->second.goalX,
                                            found->second.goalY,
                                            found->second.goalZ, error);
            if (providerId == 0) {
                // Provider recusou: reporta como Failed honesto.
                NavQueryResult result;
                result.queryId = queryId;
                result.succeeded = false;
                result.error = "provider refused: " + error;
                results.push_back(result);
                pending_.erase(found);
            } else {
                found->second.providerId = providerId;
                found->second.dispatched = true;
                found->second.inFlight = true;
            }
        }

        // 2) Coleta os resultados concluídos (Succeeded/Failed/Cancelled).
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (!it->second.inFlight) {
                ++it;
                continue;
            }
            PathResult pathResult;
            std::string error;
            const PathRequestStatus status =
                provider_->poll_async_path(it->second.providerId, pathResult,
                                           error);
            if (status == PathRequestStatus::Succeeded) {
                NavQueryResult result;
                result.queryId = it->first;
                result.succeeded = true;
                result.pathJson = serialize_path(pathResult);
                results.push_back(result);
                it = pending_.erase(it);
            } else if (status == PathRequestStatus::Failed) {
                NavQueryResult result;
                result.queryId = it->first;
                result.succeeded = false;
                result.error = error;
                results.push_back(result);
                it = pending_.erase(it);
            } else if (status == PathRequestStatus::Cancelled) {
                NavQueryResult result;
                result.queryId = it->first;
                result.cancelled = true;
                results.push_back(result);
                it = pending_.erase(it);
            } else {
                // Queued/Running/Invalid: ainda em voo ou desconhecido.
                ++it;
            }
        }

        // Ordena por id crescente (determinístico).
        std::sort(results.begin(), results.end(),
                  [](const NavQueryResult& a, const NavQueryResult& b) {
                      return a.queryId < b.queryId;
                  });
        return results;
    }

    std::size_t queued_count() const override {
        return scheduler_->queued_count();
    }

    std::size_t in_flight_count() const override {
        std::size_t count = 0;
        for (const auto& entry : pending_) {
            if (entry.second.inFlight) ++count;
        }
        return count;
    }

    void reset() override {
        scheduler_->reset();
        pending_.clear();
    }

private:
    IAsyncQueryScheduler* scheduler_{ nullptr };
    INavigationProvider* provider_{ nullptr };
    float maxBudgetSeconds_{ 1.0f };
    std::map<std::uint64_t, PendingQuery> pending_;
};

}  // namespace

std::unique_ptr<INavigationSchedulerBridge> create_navigation_scheduler_bridge(
    IAsyncQueryScheduler* scheduler, INavigationProvider* provider) {
    if (scheduler == nullptr || provider == nullptr) return nullptr;
    return std::make_unique<NavigationSchedulerBridge>(scheduler, provider);
}

}  // namespace navigation
}  // namespace engine

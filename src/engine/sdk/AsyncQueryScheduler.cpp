// AsyncQueryScheduler.cpp — the only TU implementing the public async query
// scheduler contract (Agente 4 §2 item 27 CORE): a deterministic priority
// queue with timeout (scheduler clock) and per-frame budget dispatch.
// Pure std, no threads, no navmesh — the caller decides where dispatched
// queries run. All-or-nothing enqueue; deterministic order everywhere.

#include "engine/navigation/IAsyncQueryScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace engine {
namespace navigation {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    // IEEE-754: exponent all-ones => NaN or infinity.
    return (bits & 0x7f800000u) != 0x7f800000u;
}

struct Entry {
    AsyncQuerySpec spec;
    float waitSeconds{ 0.0f };  // relógio do scheduler desde o enqueue
    std::uint64_t seq{ 0 };     // ordem de enqueue (FIFO entre prioridades iguais)
};

class AsyncQueryScheduler final : public IAsyncQueryScheduler {
public:
    AsyncQueryScheduler() = default;

    bool configure(std::size_t maxQueued, std::string& errorOut) override {
        if (maxQueued == 0) {
            errorOut = "async query scheduler: maxQueued must be >= 1";
            return false;
        }
        maxQueued_ = maxQueued;
        return true;
    }

    bool enqueue(const AsyncQuerySpec& spec, std::string& errorOut) override {
        if (spec.queryId == 0) {
            errorOut = "async query scheduler: queryId must be non-zero";
            return false;
        }
        if (byId_.count(spec.queryId) != 0) {
            errorOut = "async query scheduler: duplicate queryId " +
                       std::to_string(spec.queryId);
            return false;
        }
        if (!finite_float(spec.priority)) {
            errorOut = "async query scheduler: priority must be finite";
            return false;
        }
        if (!finite_float(spec.timeoutSeconds) || spec.timeoutSeconds < 0.0f) {
            errorOut = "async query scheduler: timeoutSeconds must be finite and >= 0";
            return false;
        }
        if (!finite_float(spec.estimatedCost) || spec.estimatedCost <= 0.0f) {
            errorOut = "async query scheduler: estimatedCost must be finite and > 0";
            return false;
        }
        if (byId_.size() >= maxQueued_) {
            errorOut = "async query scheduler: queue full (" +
                       std::to_string(maxQueued_) + ")";
            return false;
        }
        Entry entry;
        entry.spec = spec;
        entry.waitSeconds = 0.0f;
        entry.seq = nextSeq_++;
        byId_[spec.queryId] = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back(std::move(entry));
        return true;
    }

    bool cancel(std::uint64_t queryId) override {
        const auto found = byId_.find(queryId);
        if (found == byId_.end()) return false;
        erase(found->second);
        return true;
    }

    std::vector<std::uint64_t> tick(float dt) override {
        if (!(dt > 0.0f) || entries_.empty()) return {};  // NaN/<=0 = no-op
        std::vector<std::uint64_t> expired;
        for (std::size_t i = 0; i < entries_.size();) {
            Entry& entry = entries_[i];
            entry.waitSeconds += dt;
            if (entry.spec.timeoutSeconds > 0.0f &&
                entry.waitSeconds >= entry.spec.timeoutSeconds) {
                expired.push_back(entry.spec.queryId);
                erase(i);  // não incrementa i (o elemento i foi substituído)
            } else {
                ++i;
            }
        }
        std::sort(expired.begin(), expired.end());
        return expired;
    }

    std::vector<std::uint64_t> dispatch(float budgetSeconds) override {
        if (!(budgetSeconds > 0.0f) || entries_.empty()) return {};  // NaN/<=0 = vazio
        // Ordem: prioridade desc, FIFO (seq asc) entre iguais.
        std::vector<std::size_t> order(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            const Entry& ea = entries_[a];
            const Entry& eb = entries_[b];
            if (ea.spec.priority != eb.spec.priority) {
                return ea.spec.priority > eb.spec.priority;
            }
            return ea.seq < eb.seq;
        });

        std::vector<std::uint64_t> selected;
        float remaining = budgetSeconds;
        std::vector<std::size_t> toErase;
        for (const std::size_t index : order) {
            const Entry& entry = entries_[index];
            if (entry.spec.estimatedCost <= remaining) {
                remaining -= entry.spec.estimatedCost;
                selected.push_back(entry.spec.queryId);
                toErase.push_back(index);
            }
            // custo > restante: pula (greedy), não bloqueia as seguintes.
        }
        // Remove do fim para o começo (índices válidos após cada erase).
        std::sort(toErase.begin(), toErase.end(), std::greater<std::size_t>());
        for (const std::size_t index : toErase) erase(index);
        return selected;
    }

    std::size_t queued_count() const override { return entries_.size(); }

    bool is_queued(std::uint64_t queryId) const override {
        return byId_.count(queryId) != 0;
    }

    void reset() override {
        entries_.clear();
        byId_.clear();
    }

private:
    // Remove a entrada no índice `index` e re-mapeia os ids afetados.
    void erase(std::size_t index) {
        const std::uint64_t removedId = entries_[index].spec.queryId;
        entries_[index] = std::move(entries_.back());
        entries_.pop_back();
        byId_.erase(removedId);
        if (index < entries_.size()) {
            byId_[entries_[index].spec.queryId] = static_cast<std::uint32_t>(index);
        }
    }

    std::size_t maxQueued_{ 64 };
    std::uint64_t nextSeq_{ 1 };
    std::vector<Entry> entries_;
    std::map<std::uint64_t, std::uint32_t> byId_;
};

}  // namespace

std::unique_ptr<IAsyncQueryScheduler> create_async_query_scheduler() {
    return std::make_unique<AsyncQueryScheduler>();
}

}  // namespace navigation
}  // namespace engine

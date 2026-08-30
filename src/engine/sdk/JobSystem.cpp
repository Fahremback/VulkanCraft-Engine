#include "engine/jobs/IJobSystem.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#if __has_include(<concurrentqueue.h>)
#include <concurrentqueue.h>
#define VC_HAS_CONCURRENTQUEUE 1
#else
#define VC_HAS_CONCURRENTQUEUE 0
#endif

namespace engine::jobs {
namespace {

class JobSystem final : public IJobSystem {
    using Item = std::pair<JobHandle, std::function<void()>>;

public:
    JobHandle submit(std::function<void()> job) override {
        if (!job) return {};
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return {};  // Job rejected: system is shut down.
        }
        const JobHandle handle{nextId_.fetch_add(1, std::memory_order_relaxed)};
        states_[handle.id] = JobState::Queued;
        push_job({handle, std::move(job)});
        return handle;
    }

    // Shuts down the job system: all subsequent submit() calls return an
    // invalid handle. Already-queued jobs remain in the queue for drain().
    void shutdown() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }

    bool cancel(JobHandle handle) override {
        std::lock_guard lock(mutex_);
        const auto stateIt = states_.find(handle.id);
        if (stateIt == states_.end() || stateIt->second != JobState::Queued) {
            return false;
        }
        std::deque<Item> retained;
        bool removed = false;
        while (auto item = pop_item()) {
            if (item->first == handle) {
                removed = true;
            } else {
                retained.push_back(std::move(*item));
            }
        }
        for (auto& item : retained) push_job(std::move(item));
        if (removed) stateIt->second = JobState::Cancelled;
        return removed;
    }

    JobState state(JobHandle handle) const override {
        std::lock_guard lock(mutex_);
        const auto it = states_.find(handle.id);
        return it == states_.end() ? JobState::Cancelled : it->second;
    }

    std::size_t pending() const override {
        std::lock_guard lock(mutex_);
#if VC_HAS_CONCURRENTQUEUE
        return jobs_.size_approx();
#else
        return jobs_.size();
#endif
    }

    void drain() override {
        for (;;) {
            Item item;
            {
                std::lock_guard lock(mutex_);
                auto queued = pop_item();
                if (!queued) return;
                item = std::move(*queued);
                auto it = states_.find(item.first.id);
                if (it == states_.end() || it->second == JobState::Cancelled) continue;
                it->second = JobState::Running;
            }
            if (item.second) item.second();
            std::lock_guard lock(mutex_);
            states_[item.first.id] = JobState::Completed;
        }
    }

private:
    void push_job(Item item) {
#if VC_HAS_CONCURRENTQUEUE
        jobs_.enqueue(std::move(item));
#else
        jobs_.push_back(std::move(item));
#endif
    }

    std::unique_ptr<Item> pop_item() {
#if VC_HAS_CONCURRENTQUEUE
        Item item;
        if (!jobs_.try_dequeue(item)) return {};
        return std::make_unique<Item>(std::move(item));
#else
        if (jobs_.empty()) return {};
        auto item = std::make_unique<Item>(std::move(jobs_.front()));
        jobs_.pop_front();
        return item;
#endif
    }

    mutable std::mutex mutex_;
#if VC_HAS_CONCURRENTQUEUE
    moodycamel::ConcurrentQueue<Item> jobs_;
#else
    std::deque<Item> jobs_;
#endif
    std::unordered_map<std::uint64_t, JobState> states_;
    std::atomic<std::uint64_t> nextId_{1};
    bool stopped_{false};
};

} // namespace

std::shared_ptr<IJobSystem> create_job_system() {
    return std::make_shared<JobSystem>();
}

} // namespace engine::jobs

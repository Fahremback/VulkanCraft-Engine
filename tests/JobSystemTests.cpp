#include "engine/jobs/IJobSystem.hpp"

#include <atomic>
#include <cassert>
#include <vector>

int main() {
    auto jobs = engine::jobs::create_job_system();
    assert(jobs->submit({}).id == 0);

    std::atomic<int> completed{0};
    std::vector<int> order;
    const auto first = jobs->submit([&] { order.push_back(1); completed.fetch_add(1); });
    const auto cancelled = jobs->submit([&] { completed.fetch_add(100); });
    const auto third = jobs->submit([&] { order.push_back(3); completed.fetch_add(1); });

    assert(first.id != 0 && cancelled.id != 0 && third.id != 0);
    assert(jobs->state(first) == engine::jobs::JobState::Queued);
    assert(jobs->pending() == 3);
    assert(jobs->cancel(cancelled));
    assert(jobs->state(cancelled) == engine::jobs::JobState::Cancelled);
    assert(!jobs->cancel(cancelled));
    assert(jobs->pending() == 2);

    jobs->drain();
    assert((order == std::vector<int>{1, 3}));
    assert(completed.load() == 2);
    assert(jobs->state(first) == engine::jobs::JobState::Completed);
    assert(jobs->state(third) == engine::jobs::JobState::Completed);
    assert(jobs->pending() == 0);
    return 0;
}

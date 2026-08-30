#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace engine::jobs {

enum class JobState : std::uint8_t {
    Queued,
    Running,
    Completed,
    Cancelled,
};

struct JobHandle {
    std::uint64_t id{0};
    friend bool operator==(const JobHandle&, const JobHandle&) = default;
};

class IJobSystem {
public:
    virtual ~IJobSystem() = default;

    // Queues work and returns a unique handle. A null job is rejected with id 0.
    virtual JobHandle submit(std::function<void()> job) = 0;
    // Cancels only queued work. Running/completed jobs cannot be cancelled.
    virtual bool cancel(JobHandle handle) = 0;
    // Returns the current state, or Cancelled for an unknown/cancelled handle.
    virtual JobState state(JobHandle handle) const = 0;
    virtual std::size_t pending() const = 0;
    // Executes queued jobs in submission order until the queue is empty.
    virtual void drain() = 0;
    // Shuts the system down: every subsequent submit() is rejected with an
    // invalid handle (id 0) instead of being silently accepted and dropped
    // (A4-JOBS-SHUTDOWN-SILENCIOSO). Already-queued jobs remain for drain().
    virtual void shutdown() = 0;
};

std::shared_ptr<IJobSystem> create_job_system();

} // namespace engine::jobs

#include "ThreadPool.hpp"

#include <atomic>
#include <future>
#include <iostream>

namespace {

bool expect_equal(const char* phase, int actual, int expected) {
    if (actual == expected) {
        return true;
    }

    std::cerr << phase << ": expected " << expected << " completed tasks, got "
              << actual << '\n';
    return false;
}

} // namespace

int main() {
    ThreadPool pool(4);
    std::atomic<int> completed{0};

    constexpr int firstBatchSize = 64;
    std::promise<void> releaseFirstBatch;
    const std::shared_future<void> firstBatchGate = releaseFirstBatch.get_future().share();

    for (int i = 0; i < firstBatchSize; ++i) {
        pool.enqueue([&completed, firstBatchGate] {
            firstBatchGate.wait();
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    releaseFirstBatch.set_value();
    pool.wait_idle();
    if (!expect_equal("first wait", completed.load(std::memory_order_relaxed), firstBatchSize)) {
        return 1;
    }

    // Waiting on an already-idle pool must be safe and return immediately.
    pool.wait_idle();
    if (!expect_equal("repeated idle wait", completed.load(std::memory_order_relaxed), firstBatchSize)) {
        return 1;
    }

    constexpr int secondBatchSize = 37;
    for (int i = 0; i < secondBatchSize; ++i) {
        pool.enqueue([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.wait_idle();
    if (!expect_equal(
            "second wait",
            completed.load(std::memory_order_relaxed),
            firstBatchSize + secondBatchSize)) {
        return 1;
    }

    return 0;
}

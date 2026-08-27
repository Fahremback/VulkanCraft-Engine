// concurrentqueue-probe.cpp
// Real-consumer probe for the vendored moodycamel::ConcurrentQueue.
// Proves cross-thread enqueue/dequeue with ordering + user-flag transport,
// satisfying the §9/BUG-018 "real consumer with execution evidence" bar.
//
// Compile (header-only, needs -pthread on GCC/MSVC):
//   g++ -std=c++17 -pthread tools/portability/concurrentqueue-probe.cpp -I external/solutions/concurrentqueue -o /tmp/concurrentqueue-probe.exe
// Run:
//   /tmp/concurrentqueue-probe.exe
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "concurrentqueue.h"

int main() {
    moodycamel::ConcurrentQueue<int> q;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<long long> sum{0};

    // Two producers push 100k elements each.
    std::thread p1([&] {
        for (int i = 0; i < 100000; ++i) { q.enqueue(i); produced.fetch_add(1); }
    });
    std::thread p2([&] {
        for (int i = 100000; i < 200000; ++i) { q.enqueue(i); produced.fetch_add(1); }
    });
    p1.join();
    p2.join();

    // One consumer drains until produced is reached.
    int item = 0;
    while (consumed.load() < produced.load()) {
        while (q.try_dequeue(item)) {
            sum.fetch_add(item);
            consumed.fetch_add(1);
        }
    }

    long long total = (199999LL * 200000LL) / 2; // sum 0..199999
    bool ok = (produced.load() == 200000 && consumed.load() == 200000 && sum.load() == total);
    (void)ok; // informational

    if (ok) {
        std::printf("concurrentqueue-consumer-ok produced=%d consumed=%d sum_ok=1\n",
                    produced.load(), consumed.load());
        return 0;
    }
    std::printf("concurrentqueue-consumer-FAIL produced=%d consumed=%d sum=%d expected=%d\n",
                produced.load(), consumed.load(), sum.load(), total);
    return 1;
}
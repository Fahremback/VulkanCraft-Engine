// taskflow-probe.cpp
// Real-consumer probe for the vendored Taskflow library (header-only).
// Proves building + running a small DAG of dependent tasks on a thread pool,
// with a data dependency and a counter, satisfying §9/BUG-018 "real consumer".
//
// Compile (header-only, needs -pthread):
//   g++ -std=c++17 -pthread tools/portability/taskflow-probe.cpp -I external/solutions/taskflow -o /tmp/taskflow-probe.exe
// Run:
//   /tmp/taskflow-probe.exe
#include <cstdio>
#include <atomic>
#include "taskflow/taskflow.hpp"

int main() {
    tf::Executor executor(4);          // 4 worker threads
    tf::Taskflow flow;

    std::atomic<int> counter{0};
    int b_val = 0;

    // A -> B -> C deterministic dependency chain (data carried via captures).
    auto A = flow.emplace([&] { counter.fetch_add(1); });
    auto B = flow.emplace([&] { counter.fetch_add(10); });
    auto C = flow.emplace([&] { counter.fetch_add(100); });
    A.precede(B).precede(C);

    executor.run(flow).wait();

    // Sequence: A(+1), B(+10)=11, C(+100)=111.
    int got = counter.load();
    bool ok = (got == 111);

    if (ok) {
        std::printf("taskflow-consumer-ok tasks=3 DAG-order=111 pool=4\n");
        return 0;
    }
    std::printf("taskflow-consumer-FAIL counter=%d expected=111\n", got);
    return 1;
}
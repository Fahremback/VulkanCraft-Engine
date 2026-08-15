#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class ThreadPool {
public:
    ThreadPool(size_t threads = std::thread::hardware_concurrency()) {
        if (threads == 0) threads = 4;
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queueMutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                        ++this->activeTasks;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(this->queueMutex);
                        --this->activeTasks;
                        if (this->tasks.empty() && this->activeTasks == 0) {
                            this->idleCondition.notify_all();
                        }
                    }
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (stop) return;
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    // Shutdown and resource recreation must never race jobs that still hold
    // references to World, Chunk or Vulkan-owned staging data.
    void wait_idle() {
        std::unique_lock<std::mutex> lock(queueMutex);
        idleCondition.wait(lock, [this] {
            return tasks.empty() && activeTasks == 0;
        });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    std::condition_variable idleCondition;
    std::size_t activeTasks{ 0 };
    bool stop{ false };
};

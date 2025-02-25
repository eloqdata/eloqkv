#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace EloqDS
{
class ThreadWorkerPool
{
public:
    ThreadWorkerPool(size_t max_workers_num = 1);
    ~ThreadWorkerPool() = default;

    void SubmitWork(std::function<void()> work);
    size_t WorkQueueSize();
    void Shutdown();
    size_t WorkerPoolSize()
    {
        return max_workers_num_;
    }

private:
    size_t max_workers_num_;
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> work_queue_;
    std::mutex work_queue_mutex_;
    std::condition_variable work_queue_cv_;
    std::atomic<bool> shutdown_indicator_{false};
};
}  // namespace EloqDS

#include "thread_worker_pool.h"

#include <cassert>

namespace EloqDS
{
ThreadWorkerPool::ThreadWorkerPool(size_t max_workers_num)
    : max_workers_num_(max_workers_num)
{
    for (size_t i = 0; i < max_workers_num_; i++)
    {
        std::thread worker = std::thread(
            [this]
            {
                while (true)
                {
                    // Acquire work queue mutex
                    std::unique_lock<std::mutex> lk(work_queue_mutex_);
                    // Wait for new work come in or shutdown happen
                    work_queue_cv_.wait(
                        lk,
                        [this]
                        {
                            return !work_queue_.empty() ||
                                   shutdown_indicator_.load(
                                       std::memory_order_acquire);
                        });
                    // Take work if work queue is not empty
                    if (!work_queue_.empty())
                    {
                        std::function<void()> work =
                            std::move(work_queue_.front());
                        work_queue_.pop_front();
                        lk.unlock();
                        // Do work
                        work();
                    }
                    else
                    {
                        // Quit loop if shutdown
                        assert(shutdown_indicator_.load(
                            std::memory_order_acquire));
                        lk.unlock();
                        break;
                    }
                }
            });
        workers_.push_back(std::move(worker));
    }
}

size_t ThreadWorkerPool::WorkQueueSize()
{
    std::unique_lock<std::mutex> lk(work_queue_mutex_);
    return work_queue_.size();
}

void ThreadWorkerPool::SubmitWork(std::function<void()> work)
{
    std::unique_lock<std::mutex> lk(work_queue_mutex_);
    if (shutdown_indicator_.load(std::memory_order_acquire))
    {
        return;
    }
    work_queue_.push_back(std::move(work));
    work_queue_cv_.notify_one();
}

void ThreadWorkerPool::Shutdown()
{
    {
        std::unique_lock<std::mutex> lk(work_queue_mutex_);
        shutdown_indicator_.store(true, std::memory_order_release);
        work_queue_cv_.notify_all();
    }

    for (std::thread &worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}
}  // namespace EloqDS

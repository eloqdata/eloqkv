#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace EloqKV
{

/**
 * @brief A generic thread-safe Read-Copy-Update (RCU) container.
 *
 * Provides lock-free read access to a read-mostly data structure,
 * and serializes writes by creating a copy of the state, modifying it
 * inside a callback, and atomically publishing it.
 */
template <typename T>
class Rcu
{
public:
    Rcu() : state_(std::make_shared<T>())
    {
    }
    explicit Rcu(std::shared_ptr<const T> state) : state_(std::move(state))
    {
    }

    std::shared_ptr<const T> Read() const
    {
        return state_.load(std::memory_order_acquire);
    }

    template <typename Func>
    auto Update(Func &&func) -> decltype(func(std::declval<T &>()))
    {
        std::lock_guard<std::mutex> lock(write_mu_);
        auto latest = state_.load(std::memory_order_acquire);
        auto copy = std::make_shared<T>(*latest);
        if constexpr (std::is_void_v<decltype(func(std::declval<T &>()))>)
        {
            func(*copy);
            state_.store(std::shared_ptr<const T>(copy),
                         std::memory_order_release);
        }
        else
        {
            decltype(auto) res = func(*copy);
            state_.store(std::shared_ptr<const T>(copy),
                         std::memory_order_release);
            return res;
        }
    }

private:
    std::atomic<std::shared_ptr<const T>> state_;
    mutable std::mutex write_mu_;
};

}  // namespace EloqKV

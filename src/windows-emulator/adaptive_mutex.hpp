#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <utils/concurrency.hpp>

namespace sogen
{
    // The kernel lock's mutex: a waiter spins for the lock before it parks in the OS, and whichever waiter is running
    // when the lock comes free takes it.
    //
    // std::mutex parks a waiter within microseconds, so under the 120K acquisitions a second of a game a released lock
    // sat free until the OS had woken a parked waiter, about a third of the main thread's waiting. Handing the lock over
    // in arrival order (a ticket lock) removed that but stalled every waiter behind one that was not running, whenever
    // other work took the host's cores. A woken waiter spins again before it parks again, so the waiters that never
    // parked cannot keep it from the lock.
    class adaptive_mutex
    {
      public:
        adaptive_mutex() = default;
        adaptive_mutex(const adaptive_mutex&) = delete;
        adaptive_mutex& operator=(const adaptive_mutex&) = delete;
        adaptive_mutex(adaptive_mutex&&) = delete;
        adaptive_mutex& operator=(adaptive_mutex&&) = delete;
        ~adaptive_mutex() = default;

        void lock()
        {
            if (this->try_lock() || this->spin_for(held))
            {
                return;
            }

            while (this->state_.exchange(held_with_waiters, std::memory_order_acquire) != free)
            {
                this->state_.wait(held_with_waiters, std::memory_order_relaxed);
                if (this->spin_for(held_with_waiters))
                {
                    return;
                }
            }
        }

        bool try_lock()
        {
            uint32_t expected = free;
            return this->state_.compare_exchange_strong(expected, held, std::memory_order_acquire, std::memory_order_relaxed);
        }

        void unlock()
        {
            if (this->state_.exchange(free, std::memory_order_release) == held_with_waiters)
            {
                this->state_.notify_one();
            }
        }

      private:
        static constexpr uint32_t free = 0;
        static constexpr uint32_t held = 1;
        static constexpr uint32_t held_with_waiters = 2; // an unlock wakes one parked waiter
        static constexpr auto spin_time = std::chrono::microseconds(30);

        // A waiter that has parked takes the lock as held_with_waiters, as others may still be parked.
        bool spin_for(const uint32_t taken_as)
        {
            const auto start = std::chrono::steady_clock::now();
            for (uint32_t i = 1;; ++i)
            {
                utils::concurrency::cpu_relax();
                uint32_t expected = free;
                if (this->state_.load(std::memory_order_relaxed) == free &&
                    this->state_.compare_exchange_strong(expected, taken_as, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    return true;
                }

                if ((i & 63) == 0 && std::chrono::steady_clock::now() - start >= spin_time)
                {
                    return false;
                }
            }
        }

        std::atomic<uint32_t> state_{free};
    };
}

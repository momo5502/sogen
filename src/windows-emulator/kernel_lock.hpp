#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace sogen
{

    // The emulator kernel lock (docs/multi-vcpu-design.md, sections 2 and 7).
    // Serializes all emulator code that touches shared kernel state: acquired at
    // every entry point (VM-exit hook callbacks, UI event delivery, scheduler
    // iterations) and released while guest code executes. Interior code asserts
    // ownership instead of re-acquiring; the lock is deliberately not recursive,
    // so nested acquisition is caught immediately in debug builds.
    class kernel_lock
    {
      public:
        struct profile_stats
        {
            uint64_t acquisitions; // total successful lock() calls
            uint64_t contended;    // acquisitions that had to block because another thread held it
            uint64_t wait_nanos;   // cumulative time spent blocked on contended acquisitions
            uint64_t held_nanos;   // cumulative time the lock was held (BEL "busy" time)
        };

        // Env-gated so a normal run pays nothing: the instrumented path (a try_lock plus two
        // steady_clock reads per acquisition) only runs when SOGEN_LOCK_PROFILE is set.
        static bool profiling_enabled()
        {
            static const bool enabled = std::getenv("SOGEN_LOCK_PROFILE") != nullptr;
            return enabled;
        }

        void lock()
        {
            assert(!this->is_held_by_current_thread() && "The kernel lock is not recursive");

            if (profiling_enabled())
            {
                this->lock_profiled();
            }
            else
            {
                this->mutex_.lock();
            }

            this->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        }

        // Acquire only if the lock is free. Lets a host-owned thread touch kernel state without ever stalling
        // behind a long-running emulator operation; the caller is expected to retry later or skip the work.
        bool try_lock()
        {
            assert(!this->is_held_by_current_thread() && "The kernel lock is not recursive");

            if (!this->mutex_.try_lock())
            {
                return false;
            }

            if (profiling_enabled())
            {
                this->acquisitions_.fetch_add(1, std::memory_order_relaxed);
                this->held_since_ = std::chrono::steady_clock::now();
            }

            this->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
            return true;
        }

        void unlock()
        {
            this->owner_.store({}, std::memory_order_relaxed);

            if (profiling_enabled())
            {
                this->held_nanos_.fetch_add(nanos_since(this->held_since_), std::memory_order_relaxed);
            }

            this->mutex_.unlock();
        }

        bool is_held_by_current_thread() const
        {
            return this->owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
        }

        void assert_held() const
        {
            // Evaluated outside assert() so the member access survives in NDEBUG builds (where assert
            // expands to nothing); the relaxed load is otherwise dead and elided by the optimizer.
            [[maybe_unused]] const bool held = this->is_held_by_current_thread();
            assert(held && "The kernel lock must be held");
        }

        profile_stats profile() const
        {
            return {
                .acquisitions = this->acquisitions_.load(std::memory_order_relaxed),
                .contended = this->contended_.load(std::memory_order_relaxed),
                .wait_nanos = this->wait_nanos_.load(std::memory_order_relaxed),
                .held_nanos = this->held_nanos_.load(std::memory_order_relaxed),
            };
        }

      private:
        using clock = std::chrono::steady_clock;

        static uint64_t nanos_since(const clock::time_point start)
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count());
        }

        void lock_profiled()
        {
            if (!this->mutex_.try_lock())
            {
                const auto start = clock::now();
                this->mutex_.lock();
                this->wait_nanos_.fetch_add(nanos_since(start), std::memory_order_relaxed);
                this->contended_.fetch_add(1, std::memory_order_relaxed);
            }

            this->acquisitions_.fetch_add(1, std::memory_order_relaxed);
            this->held_since_ = clock::now();
        }

        std::mutex mutex_{};
        std::atomic<std::thread::id> owner_{};

        // Guarded by mutex_ (written/read only by the current holder).
        clock::time_point held_since_{};

        std::atomic<uint64_t> acquisitions_{};
        std::atomic<uint64_t> contended_{};
        std::atomic<uint64_t> wait_nanos_{};
        std::atomic<uint64_t> held_nanos_{};
    };

} // namespace sogen

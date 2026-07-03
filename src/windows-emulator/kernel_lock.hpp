#pragma once

#include <atomic>
#include <cassert>
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
        void lock()
        {
            assert(!this->is_held_by_current_thread() && "The kernel lock is not recursive");
            this->mutex_.lock();
            this->owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        }

        void unlock()
        {
            this->owner_.store({}, std::memory_order_relaxed);
            this->mutex_.unlock();
        }

        bool is_held_by_current_thread() const
        {
            return this->owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
        }

        void assert_held() const
        {
            assert(this->is_held_by_current_thread() && "The kernel lock must be held");
        }

      private:
        std::mutex mutex_{};
        std::atomic<std::thread::id> owner_{};
    };

} // namespace sogen

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace sogen
{
    // Counts the frames the guest presents through Vulkan and can hold them to a frame rate. The limit applies to all
    // swapchains of the process together. It is set from host threads while the guest runs, so everything is
    // thread safe.
    class frame_pacer
    {
      public:
        using clock = std::chrono::steady_clock;

        struct statistics
        {
            uint64_t frames{};
            // The longest time between two presents since the previous read; it is not the GPU's render time.
            uint64_t worst_present_interval_ns{};
        };

        // 0 turns the limit off.
        void set_limit(const uint32_t frames_per_second)
        {
            this->limit_ = frames_per_second;
        }

        bool limited() const
        {
            return this->limit_ != 0;
        }

        // Blocks until the next frame may be presented. A frame that comes late moves the schedule instead of letting
        // the following frames catch up in a burst.
        void wait()
        {
            const auto limit = this->limit_.load();
            if (limit == 0)
            {
                return;
            }

            const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::seconds{1}) / limit;
            clock::time_point due{};
            {
                const std::lock_guard lock{this->mutex_};
                const auto now = clock::now();
                due = this->next_present_ > now ? this->next_present_ : now;
                this->next_present_ = due + period;
            }

            std::this_thread::sleep_until(due);
        }

        void presented()
        {
            const auto now = clock::now();
            const std::lock_guard lock{this->mutex_};
            if (this->last_present_ != clock::time_point{})
            {
                const auto interval =
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - this->last_present_).count());
                this->worst_interval_ns_ = std::max(this->worst_interval_ns_, interval);
            }
            this->last_present_ = now;
            ++this->frames_;
        }

        statistics take_statistics()
        {
            const std::lock_guard lock{this->mutex_};
            return {.frames = this->frames_, .worst_present_interval_ns = std::exchange(this->worst_interval_ns_, 0)};
        }

      private:
        std::atomic<uint32_t> limit_{};
        std::mutex mutex_{};
        clock::time_point next_present_{};
        clock::time_point last_present_{};
        uint64_t frames_{};
        uint64_t worst_interval_ns_{};
    };
}

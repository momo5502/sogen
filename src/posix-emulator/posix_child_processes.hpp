#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace sogen
{
    // The status word wait4/waitpid report; Linux and XNU share this encoding.
    constexpr int posix_exited_wait_status(const int exit_code)
    {
        return (exit_code & 0xFF) << 8;
    }

    constexpr int posix_signaled_wait_status(const int signal)
    {
        return signal & 0x7F;
    }

    struct posix_terminated_child
    {
        uint32_t process_id{};
        uint32_t process_group_id{};
        int wait_status{};
    };

    // Children that have terminated and not been waited for yet.
    class posix_child_processes
    {
      public:
        void add_terminated(const posix_terminated_child& child)
        {
            this->terminated_.push_back(child);
        }

        // Selects as waitpid's pid argument does: a child (> 0), any child (-1), the caller's group (0) or a group (< -1).
        std::optional<posix_terminated_child> find(const int32_t selector, const uint32_t caller_group_id) const
        {
            const auto it = std::ranges::find_if(this->terminated_, [&](const posix_terminated_child& child) {
                if (selector > 0)
                {
                    return child.process_id == static_cast<uint32_t>(selector);
                }

                if (selector == -1)
                {
                    return true;
                }

                const auto group_id = selector == 0 ? caller_group_id : static_cast<uint32_t>(-static_cast<int64_t>(selector));
                return child.process_group_id == group_id;
            });

            if (it == this->terminated_.end())
            {
                return std::nullopt;
            }

            return *it;
        }

        void reap(const uint32_t process_id)
        {
            std::erase_if(this->terminated_, [&](const posix_terminated_child& child) { return child.process_id == process_id; });
        }

      private:
        std::vector<posix_terminated_child> terminated_{};
    };
}

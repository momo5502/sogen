#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace sogen
{
    // Per thread: another thread may look objects up while a command records, such as a fence or semaphore poll.
    inline thread_local std::unordered_set<uint64_t>* vulkan_recording_references{};

    template <typename T>
    class vulkan_object_table : public std::unordered_map<uint64_t, T>
    {
      public:
        auto find(const uint64_t id)
        {
            note(id);
            return std::unordered_map<uint64_t, T>::find(id);
        }

        auto find(const uint64_t id) const
        {
            note(id);
            return std::unordered_map<uint64_t, T>::find(id);
        }

      private:
        static void note(const uint64_t id)
        {
            if (vulkan_recording_references)
            {
                vulkan_recording_references->insert(id);
            }
        }
    };
}

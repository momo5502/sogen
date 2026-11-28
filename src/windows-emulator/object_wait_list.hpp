#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <utils/serialization.hpp>

namespace sogen
{
    class emulator_thread;
    struct process_context;
    struct event;
    struct semaphore;
    struct mutant;

    // KeWaitForSingleObject and KeWaitForMultipleObjects link each wait block at the tail of the object's wait list, and
    // KeSetEvent, KeReleaseSemaphoreEx and KeReleaseMutant end the waits from the head.
    class object_wait_list
    {
      public:
        struct entry
        {
            emulator_thread* thread{};
            uint32_t index{};
            uint32_t thread_id{};
        };

        object_wait_list() = default;
        ~object_wait_list();

        object_wait_list(object_wait_list&& obj) noexcept;
        object_wait_list& operator=(object_wait_list&& obj) noexcept;

        object_wait_list(const object_wait_list&) = delete;
        object_wait_list& operator=(const object_wait_list&) = delete;

        bool empty() const
        {
            return this->entries_.empty();
        }

        size_t size() const
        {
            return this->entries_.size();
        }

        const std::vector<entry>& entries() const
        {
            return this->entries_;
        }

        void link(emulator_thread& thread, uint32_t index);
        void unlink(emulator_thread& thread, uint32_t index);

        entry take_front();

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

        void restore_links(process_context& process);

        void clear();

      private:
        std::vector<entry> entries_{};
    };

    struct event_signal
    {
        bool was_signaled{};
    };

    event_signal signal_event(event& e);

    void release_semaphore_units(semaphore& s, uint32_t previous_count);

    void hand_over_mutant(mutant& m, uint32_t process_id);
}

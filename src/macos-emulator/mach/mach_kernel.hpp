#pragma once

#include "mach_exception.hpp"
#include "mach_port_namespace.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <serialization.hpp>

namespace sogen
{
    constexpr uint64_t MACH_MAIN_THREAD_ID = 1;

    struct mach_unserviced_send
    {
        mach::port_name_t port{};
        int32_t routine{};
    };

    struct mach_port_notification
    {
        mach::port_name_t watched{};
        int32_t msgid{};
        uint32_t sync{};
        mach::port_name_t notify{};
    };

    // A handle over a range of guest memory that a later mach_vm_map turns back into an address. sogen's
    // address space has no second mapping of one range, so an entry names the range it was made over and
    // mapping it hands that same range back.
    struct mach_memory_entry
    {
        uint64_t address{};
        uint64_t size{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    struct mach_semaphore
    {
        int32_t value{};
        int32_t policy{};
        uint64_t id{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    class mach_kernel
    {
      public:
        mach::mach_port_namespace ports{};

        // The last message queued to a port no server reads. A blocked receive is always the tail of one
        // of these, and naming the routine turns "the guest is stuck" into a request to implement.
        mach_unserviced_send last_unserviced_send{};

        // Starts at one because zero is os_activity's "no activity" sentinel, and a first id of zero
        // would read as the absence of one.
        uint64_t next_activity_id{1};

        mach::port_name_t task_self{};
        mach::port_name_t host_self{};
        mach::port_name_t bootstrap{};

        mach::exception_state exceptions{};
        std::optional<mach::raised_exception> last_exception{};

        // Registrations from mach_port_request_notification. Nothing is ever queued from these: the
        // kernel-object ports guests watch (XPC service ports above all) never die while the task lives.
        std::vector<mach_port_notification> notifications{};

        // Nanoseconds per tick of the counter the guest actually reads. Derived from CNTFRQ_EL0 rather
        // than fixed at Apple hardware's 125/3, because the emulated counter runs at 62.5 MHz (QEMU's
        // GTIMER_SCALE of 16) -- reporting the hardware ratio would scale every duration a guest
        // computes from mach_absolute_time() by 2.6.
        // Stage 5's dyld publishes the real values here; answering task_dyld_info with zeroes is a
        // better Stage 4 default than KERN_INVALID_ARGUMENT, which would be a different failure to debug.
        uint64_t all_image_info_address{};
        uint64_t all_image_info_size{};

        uint32_t timebase_numer{16};
        uint32_t timebase_denom{1};

        void setup(uint64_t main_thread_id);
        void adopt_counter_frequency(uint64_t frequency_hz);

        mach::port_name_t create_semaphore(int32_t policy, int32_t value);
        mach_semaphore* find_semaphore(mach::port_name_t name);
        mach::kern_return_t semaphore_signal(mach::port_name_t name);
        mach::kern_return_t semaphore_wait(mach::port_name_t name);
        mach::port_name_t create_voucher();

        mach::port_name_t create_memory_entry(uint64_t address, uint64_t size);
        const mach_memory_entry* find_memory_entry(mach::port_name_t name) const;

        // mk_timer. A timer is a receive right the kernel owns the send side of; arming schedules one
        // header-only message onto it. Which deadline is armed lives here rather than in the port entry
        // because the port namespace models rights, not scheduling.
        struct armed_timer
        {
            mach::port_name_t name{};
            uint64_t deadline{};
        };

        // The IOKit master port. IOKit's whole client surface is MIG to this one port, and libraries
        // that cannot get it send to MACH_PORT_NULL and then wait for a reply that cannot come.
        mach::port_name_t io_master_port();

        mach::port_name_t create_timer();
        mach::kern_return_t destroy_timer(mach::port_name_t name);
        bool arm_timer(mach::port_name_t name, uint64_t deadline);
        uint64_t cancel_timer(mach::port_name_t name);
        void disarm_timer(mach::port_name_t name);
        std::optional<armed_timer> earliest_armed_timer() const;
        mach::port_name_t clock_service(uint32_t clock_id);

        mach::port_name_t thread_self_for(uint64_t thread_id);
        mach::port_name_t make_special_reply_port(uint64_t thread_id);

        mach::port_name_t get_task_special_port(int32_t which) const;
        mach::kern_return_t set_task_special_port(int32_t which, mach::port_name_t name);
        mach::port_name_t get_host_special_port(int32_t which) const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::map<uint64_t, mach::port_name_t> thread_ports_{};
        std::map<uint64_t, mach::port_name_t> special_reply_ports_{};
        std::map<int32_t, mach::port_name_t> task_special_ports_{};
        std::map<int32_t, mach::port_name_t> host_special_ports_{};
        std::map<mach::port_name_t, mach_semaphore> semaphores_{};
        std::map<uint32_t, mach::port_name_t> clock_ports_{};
        std::map<mach::port_name_t, uint64_t> armed_timers_{};
        std::map<mach::port_name_t, mach_memory_entry> memory_entries_{};
        mach::port_name_t io_master_{};
        uint64_t next_object_id_{1};
    };
}

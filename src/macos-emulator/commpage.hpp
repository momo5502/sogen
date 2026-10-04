#pragma once

#include "std_include.hpp"

#include "macos_memory_manager.hpp"
#include "macos_system_info.hpp"

#include <arch_emulator.hpp>
#include <serialization.hpp>

namespace sogen
{
    // Offsets from _COMM_PAGE_START_ADDRESS, per osfmk/arm/cpu_capabilities.h. The RO_ prefixed ones are
    // relative to _COMM_PAGE64_RO_ADDRESS instead; that second page does not exist on x86-64.
    namespace commpage_offset
    {
        constexpr uint64_t SIGNATURE = 0x000;
        constexpr uint64_t CPU_CAPABILITIES64 = 0x010;
        constexpr uint64_t VERSION = 0x01E;
        constexpr uint64_t CPU_CAPABILITIES = 0x020;
        constexpr uint64_t NCPUS = 0x022;
        constexpr uint64_t CACHE_LINESIZE = 0x026;
        constexpr uint64_t CPU_CLUSTERS = 0x02F;
        constexpr uint64_t MEMORY_PRESSURE = 0x030;
        constexpr uint64_t ACTIVE_CPUS = 0x034;
        constexpr uint64_t PHYSICAL_CPUS = 0x035;
        constexpr uint64_t LOGICAL_CPUS = 0x036;
        constexpr uint64_t MEMORY_SIZE = 0x038;
        constexpr uint64_t TIMEOFDAY_DATA = 0x040;
        constexpr uint64_t CPUFAMILY = 0x080;
        constexpr uint64_t TIMEBASE_OFFSET = 0x088;
        constexpr uint64_t USER_TIMEBASE = 0x090;
        constexpr uint64_t CONT_HWCLOCK = 0x091;
        constexpr uint64_t DTRACE_DOF_ENABLED = 0x092;
        constexpr uint64_t CONT_TIMEBASE = 0x098;
        constexpr uint64_t BOOTTIME_USEC = 0x0A0;
        constexpr uint64_t CONT_HW_TIMEBASE = 0x0A8;
        constexpr uint64_t APPROX_TIME = 0x0C0;
        constexpr uint64_t APPROX_TIME_SUPPORTED = 0x0C8;
        constexpr uint64_t KDEBUG_ENABLE = 0x100;
        constexpr uint64_t ATM_DIAGNOSTIC_CONFIG = 0x104;
        constexpr uint64_t MULTIUSER_CONFIG = 0x108;
        constexpr uint64_t NEWTIMEOFDAY_DATA = 0x120;
        constexpr uint64_t REMOTETIME_PARAMS = 0x148;
        constexpr uint64_t DYLD_FLAGS = 0x160;
        constexpr uint64_t CPU_QUIESCENT_COUNTER = 0x180;
        constexpr uint64_t CPU_TO_CLUSTER = 0x200;
        constexpr uint64_t CPU_TO_CLUSTER_LENGTH = 0x100;

        constexpr uint64_t RO_USER_PAGE_SHIFT_32 = 0x024;
        constexpr uint64_t RO_USER_PAGE_SHIFT_64 = 0x025;
        constexpr uint64_t RO_KERNEL_PAGE_SHIFT = 0x037;
        constexpr uint64_t RO_DEV_FIRM = 0x084;

        static_assert(CPU_TO_CLUSTER + CPU_TO_CLUSTER_LENGTH <= MACOS_COMMPAGE_FIELD_LENGTH,
                      "every field has to fit in the region libSystem treats as the commpage");
    }

    // user_timebase_type_t (osfmk/arm/cpu_capabilities.h). libSystem branches on this value to pick the
    // counter register it reads inline: NOSPEC reads CNTVCTSS_EL0 and NOSPEC_APPLE reads the Apple
    // IMPDEF S3_4_C15_C10_6, neither of which the emulated CPU implements, so both fault. SPEC reads
    // CNTVCT_EL0, which it does implement.
    enum class macos_user_timebase : uint8_t
    {
        none = 0,
        spec = 1,
        nospec = 2,
        nospec_apple = 3,
    };

    class macos_commpage
    {
      public:
        void setup(macos_memory_manager& memory, arm64_64_cpu& cpu, const macos_system_info& info);
        void update_time(macos_memory_manager& memory, arm64_64_cpu& cpu) const;

        uint64_t get_base() const
        {
            return this->base_address_;
        }

        uint64_t get_readonly_base() const
        {
            return this->readonly_base_address_;
        }

        bool is_mapped() const
        {
            return this->mapped_;
        }

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        uint64_t base_address_{};
        uint64_t readonly_base_address_{};
        bool mapped_{};
    };
}

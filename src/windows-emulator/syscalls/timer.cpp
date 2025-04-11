#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtQueryTimerResolution(const syscall_context&, const emulator_object<ULONG> maximum_time,
                                           const emulator_object<ULONG> minimum_time,
                                           const emulator_object<ULONG> current_time)
    {
        maximum_time.write_if_valid(0x0002625a);
        minimum_time.write_if_valid(0x00001388);
        current_time.write_if_valid(0x00002710);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetTimerResolution(const syscall_context&, const ULONG /*desired_resolution*/,
                                         const BOOLEAN set_resolution, const emulator_object<ULONG> current_resolution)
    {
        if (current_resolution)
        {
            current_resolution.write(0x0002625a);
        }

        if (set_resolution)
        {
            return STATUS_TIMER_RESOLUTION_NOT_SET;
        }

        return STATUS_SUCCESS;
    }
}
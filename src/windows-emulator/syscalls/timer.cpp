#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtQueryTimerResolution(const syscall_context&, const emulator_object<ULONG> maximum_time,
                                           const emulator_object<ULONG> minimum_time, const emulator_object<ULONG> current_time)
    {
        maximum_time.write_if_valid(0x0002625a);
        minimum_time.write_if_valid(0x00001388);
        current_time.write_if_valid(0x00002710);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetTimerResolution(const syscall_context&, const ULONG /*desired_resolution*/, const BOOLEAN set_resolution,
                                         const emulator_object<ULONG> current_resolution)
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

    NTSTATUS handle_NtCreateTimer2(const syscall_context& c, const emulator_object<handle> timer_handle, uint64_t /*reserved*/,
                                   const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG /*attributes*/,
                                   ACCESS_MASK /*desired_access*/)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                name = read_unicode_string(c.emu, attributes.ObjectName);
                c.win_emu.callbacks.on_generic_access("Opening timer", name);
            }
        }

        if (!name.empty())
        {
            for (auto& entry : c.proc.timers)
            {
                if (entry.second.name == name)
                {
                    ++entry.second.ref_count;
                    timer_handle.write(c.proc.timers.make_handle(entry.first));
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        timer t{};
        t.name = std::move(name);

        const auto h = c.proc.timers.store(std::move(t));
        timer_handle.write(h);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateTimer(const syscall_context& c, const emulator_object<handle> timer_handle, ACCESS_MASK desired_access,
                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG timer_type)
    {
        return handle_NtCreateTimer2(c, timer_handle, 0, object_attributes, timer_type, desired_access);
    }

    NTSTATUS handle_NtSetTimer()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetTimer2()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetTimerEx(const syscall_context& /*c*/, handle /*timer_handle*/, uint32_t /*timer_set_info_class*/,
                                 uint64_t /*timer_set_information*/, ULONG /*timer_set_information_length*/)
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtCancelTimer()
    {
        return STATUS_SUCCESS;
    }
}

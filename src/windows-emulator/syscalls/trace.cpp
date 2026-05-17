#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    namespace
    {
        constexpr uint32_t k_trace_control_wow64_bit = 0x80000000u;
        constexpr uint32_t k_trace_control_register_guids = 0x0Fu;
        constexpr uint32_t k_trace_control_add_notification_event = 0x1Bu;
        constexpr uint32_t k_trace_control_set_provider_traits = 0x1Eu;

        NTSTATUS handle_trace_control_add_notification_event(const syscall_context& c, const uint64_t input_buffer,
                                                             const ULONG input_buffer_length, const emulator_object<ULONG> return_length)
        {
            if (return_length)
            {
                return_length.write(0);
            }

            if (input_buffer == 0 || input_buffer_length != sizeof(uint32_t))
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto raw_event_handle = c.emu.read_memory<uint32_t>(input_buffer);

            handle event_handle{};
            event_handle.bits = raw_event_handle;

            if (!c.proc.events.get(event_handle))
            {
                return STATUS_INVALID_HANDLE;
            }

            if (c.proc.etw_notification_event)
            {
                return STATUS_UNSUCCESSFUL;
            }

            const auto held_handle = c.proc.events.duplicate(event_handle);
            if (!held_handle)
            {
                return STATUS_INVALID_HANDLE;
            }

            c.proc.etw_notification_event = *held_handle;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_trace_control_passthrough(const uint64_t output_buffer, const ULONG output_buffer_length,
                                                  const emulator_object<ULONG> return_length)
        {
            if (output_buffer_length != 0 && output_buffer == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (return_length)
            {
                return_length.write(output_buffer_length);
            }

            return STATUS_SUCCESS;
        }
    }

    NTSTATUS handle_NtTraceControl(const syscall_context& c, const ULONG function_code, const uint64_t input_buffer,
                                   const ULONG input_buffer_length, const uint64_t output_buffer, const ULONG output_buffer_length,
                                   const emulator_object<ULONG> return_length)
    {
        const auto base_function_code = function_code & ~k_trace_control_wow64_bit;

        switch (base_function_code)
        {
        case k_trace_control_add_notification_event:
            return handle_trace_control_add_notification_event(c, input_buffer, input_buffer_length, return_length);
        case k_trace_control_register_guids:
        case k_trace_control_set_provider_traits:
            return handle_trace_control_passthrough(output_buffer, output_buffer_length, return_length);
        default:
            return STATUS_NOT_SUPPORTED;
        }
    }
}

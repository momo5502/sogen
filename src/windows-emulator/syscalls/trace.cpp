#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{
    namespace syscalls
    {
        namespace
        {
            constexpr uint32_t k_trace_control_wow64_bit = 0x80000000u;
            constexpr uint32_t k_trace_control_register_guids = 0x0Fu;
            constexpr uint32_t k_trace_control_add_notification_event = 0x1Bu;
            constexpr uint32_t k_trace_control_set_provider_traits = 0x1Eu;

            NTSTATUS handle_trace_control_add_notification_event(const syscall_context& c, const uint64_t input_buffer,
                                                                 const ULONG input_buffer_length,
                                                                 const emulator_object<ULONG> return_length)
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

                const auto held_handle = c.proc.events.duplicate(event_handle);
                if (!held_handle)
                {
                    return STATUS_INVALID_HANDLE;
                }

                c.proc.etw_notification_events.push_back(*held_handle);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_trace_control_passthrough(const syscall_context& c, const uint64_t output_buffer,
                                                      const ULONG output_buffer_length, const emulator_object<ULONG> return_length)
            {
                if (output_buffer_length != 0 && output_buffer == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // Real Windows populates output_buffer with a genuine registration structure (e.g.
                // register_guids' TRACE_GUID_REGISTRATION array or set_provider_traits' output) that
                // callers then read as authoritative - trusting the STATUS_SUCCESS below. Leaving it
                // untouched hands back whatever garbage was already there, which real wow64/ntdll code
                // can misinterpret as real counts/offsets and crash on (confirmed: a subsequent binary-
                // search-style lookup computed a wild address from exactly this kind of leftover value).
                // Zero-filling is the safe default - every real registration structure here treats an
                // all-zero result as "nothing registered" rather than a valid entry.
                if (output_buffer_length != 0)
                {
                    c.emu.set_memory(output_buffer, 0, output_buffer_length);
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
                return handle_trace_control_passthrough(c, output_buffer, output_buffer_length, return_length);
            default:
                return STATUS_NOT_SUPPORTED;
            }
        }
    }

} // namespace sogen

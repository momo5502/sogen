#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../cpu_context.hpp"
#include "../exception_dispatch.hpp"

namespace sogen
{

    namespace syscalls
    {
        NTSTATUS handle_NtRaiseHardError(const syscall_context& c, const NTSTATUS error_status, const ULONG number_of_parameters,
                                         const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*unicode_string_parameter_mask*/,
                                         const uint64_t parameters, const HARDERROR_RESPONSE_OPTION /*valid_response_option*/,
                                         const emulator_object<HARDERROR_RESPONSE> response)
        {
            if (number_of_parameters > MAXIMUM_HARDERROR_PARAMETERS)
            {
                return STATUS_INVALID_PARAMETER_2;
            }

            if (response)
            {
                response.try_write(ResponseAbort);
            }

            if (error_status & STATUS_SERVICE_NOTIFICATION && number_of_parameters >= 3)
            {
                std::array<uint64_t, 3> params = {0, 0, 0};

                try
                {
                    if (c.emu.try_read_memory(parameters, &params, sizeof(params)))
                    {
                        const auto message =
                            read_unicode_string(c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, params[0]});
                        c.win_emu.log.error("Error Message: %s\n", u16_to_u8(message).c_str());
                    }
                }
                catch (...)
                {
                    // ignore
                }
            }

            c.proc.exit_status = error_status;
            c.win_emu.callbacks.on_exception();
            c.emu.stop();

            return STATUS_SUCCESS;
        }

        // A debugger sees the exception first; when it handles it, the thread continues from the raising context, as
        // after NtContinue. A first chance it does not handle goes to the guest's handlers.
        NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                         const emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> exception_record,
                                         const emulator_object<CONTEXT64> thread_context, const BOOLEAN first_chance)
        {
            const auto record = exception_record.read();
            const auto context = thread_context.read();

            // The raising context becomes the thread's state before the debugger is told, as it already is on the
            // hardware exception path. Otherwise the debugger's GetThreadContext shows this syscall's stub registers,
            // and a SetThreadContext it makes before DBG_CONTINUE is overwritten here afterwards.
            cpu_context::restore(c.x64_cpu(), context);

            if (report_exception_to_debugger(c.win_emu, c.vcpu, record, first_chance != FALSE))
            {
                c.write_status = false;
                return STATUS_SUCCESS;
            }

            if (first_chance)
            {
                c.write_status = false;
                dispatch_exception_record(c.win_emu, c.vcpu, record, context);
                return STATUS_SUCCESS;
            }

            c.proc.exit_status = record.ExceptionCode;
            c.win_emu.callbacks.on_exception();
            c.emu.stop();

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen

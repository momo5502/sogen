#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtRaiseHardError(const syscall_context& c, const NTSTATUS error_status, const ULONG /*number_of_parameters*/,
                                     const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>
                                     /*unicode_string_parameter_mask*/,
                                     const emulator_object<DWORD> /*parameters*/, const HARDERROR_RESPONSE_OPTION /*valid_response_option*/,
                                     const emulator_object<HARDERROR_RESPONSE> response)
    {
        if (response)
        {
            response.write(ResponseAbort);
        }

        c.proc.exit_status = error_status;
        c.win_emu.callbacks.on_exception();
        c.emu.stop();

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                     const emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> /*exception_record*/,
                                     const emulator_object<CONTEXT64> /*thread_context*/, const BOOLEAN handle_exception)
    {
        if (handle_exception)
        {
            c.win_emu.log.error("Unhandled exceptions not supported yet!\n");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        c.win_emu.callbacks.on_exception();
        c.emu.stop();

        return STATUS_SUCCESS;
    }
}

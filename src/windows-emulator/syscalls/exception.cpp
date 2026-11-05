#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../cpu_context.hpp"
#include "../exception_dispatch.hpp"

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            std::optional<std::u16string> try_read_hard_error_string(const syscall_context& c, const uint64_t address)
            {
                UNICODE_STRING<EmulatorTraits<Emu64>> string{};
                if (!c.emu.try_read_memory(address, &string, sizeof(string)) || string.Length % sizeof(char16_t) != 0 ||
                    string.Length > string.MaximumLength)
                {
                    return std::nullopt;
                }

                std::u16string text(string.Length / sizeof(char16_t), u'\0');
                if (!text.empty() && !c.emu.try_read_memory(string.Buffer, text.data(), string.Length))
                {
                    return std::nullopt;
                }

                return text;
            }

            void log_hard_error(const syscall_context& c, const NTSTATUS error_status, const ULONG number_of_parameters,
                                const ULONG unicode_string_parameter_mask, const uint64_t parameters)
            {
                c.win_emu.log.error("Hard error 0x%08X\n", static_cast<uint32_t>(error_status));

                std::array<uint64_t, MAXIMUM_HARDERROR_PARAMETERS> values{};
                if (!c.emu.try_read_memory(parameters, values.data(), number_of_parameters * sizeof(uint64_t)))
                {
                    return;
                }

                for (ULONG i = 0; i < number_of_parameters; ++i)
                {
                    const auto is_string = (unicode_string_parameter_mask & (1u << i)) != 0;
                    const auto text = is_string ? try_read_hard_error_string(c, values.at(i)) : std::nullopt;
                    if (text)
                    {
                        c.win_emu.log.error("  Parameter %u: %s\n", static_cast<unsigned int>(i), u16_to_u8(*text).c_str());
                    }
                    else
                    {
                        c.win_emu.log.error("  Parameter %u: 0x%llX\n", static_cast<unsigned int>(i),
                                            static_cast<unsigned long long>(values.at(i)));
                    }
                }
            }
        }

        NTSTATUS handle_NtRaiseHardError(const syscall_context& c, const NTSTATUS error_status, const ULONG number_of_parameters,
                                         const ULONG unicode_string_parameter_mask, const uint64_t parameters,
                                         const HARDERROR_RESPONSE_OPTION /*valid_response_option*/,
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

            log_hard_error(c, error_status, number_of_parameters, unicode_string_parameter_mask, parameters);

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

            // The guest passes the context of its own architecture, so the read has to match it: an ARM64 context is
            // smaller than a CONTEXT64 and reading the larger one can run off the end of its mapping.
            auto* arm64 = dynamic_cast<arm64_cpu*>(&c.emu);
            const auto arm64_raising_context = arm64 ? c.emu.read_memory<arm64_context>(thread_context.value()) : arm64_context{};
            const auto context = arm64 ? CONTEXT64{} : thread_context.read();

            // The raising context becomes the thread's state before the debugger is told, as it already is on the
            // hardware exception path. Otherwise the debugger's GetThreadContext shows this syscall's stub registers,
            // and a SetThreadContext it makes before DBG_CONTINUE is overwritten afterwards.
            if (c.win_emu.debugger().is_attached())
            {
                if (arm64)
                {
                    cpu_context::restore(*arm64, arm64_raising_context);
                }
                else
                {
                    cpu_context::restore(c.x64_cpu(), context);
                }
            }

            if (report_exception_to_debugger(c.win_emu, c.vcpu, record, first_chance != FALSE))
            {
                c.write_status = false;
                return STATUS_SUCCESS;
            }

            if (first_chance)
            {
                c.write_status = false;
                if (arm64)
                {
                    dispatch_exception_record(c.win_emu, c.vcpu, record, arm64_raising_context);
                }
                else
                {
                    dispatch_exception_record(c.win_emu, c.vcpu, record, context);
                }
                return STATUS_SUCCESS;
            }

            c.proc.exit_status = record.ExceptionCode;
            c.win_emu.callbacks.on_exception();
            c.emu.stop();

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen

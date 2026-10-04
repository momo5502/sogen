#include "std_include.hpp"

#include "macos_trace_emitter.hpp"

#include "macos_flag_decoders.hpp"
#include "macos_syscall_trace.hpp"

#include "../macos_emulator.hpp"
#include "../macos_syscall_utils.hpp"

#include <array>

namespace sogen
{
    namespace
    {
        std::array<uint64_t, 8> collect_arguments(const macos_syscall_context& context)
        {
            std::array<uint64_t, 8> arguments{};

            for (size_t i = 0; i < arguments.size(); ++i)
            {
                arguments[i] = get_macos_syscall_argument(context, i);
            }

            return arguments;
        }

        macos_trace_options make_options(const macos_emulator& emu)
        {
            return macos_trace_options{.string_limit = emu.trace.string_limit, .buffer_preview_limit = emu.trace.buffer_preview_limit};
        }

        void publish(macos_emulator& emu, const std::vector<macos_trace_detail>& details)
        {
            for (const auto& detail : details)
            {
                emu.callbacks.on_trace_detail(detail);
            }
        }

        bool tracing_arguments(const macos_emulator& emu)
        {
            return emu.trace.decode_arguments && static_cast<bool>(emu.callbacks.on_trace_detail);
        }
    }

    // The unused name parameters stay: stage 4 wants them for hand-placed mach decoding, and changing a
    // signature the dispatcher already calls is worse churn than an unnamed parameter.
    void emit_bsd_syscall_trace(macos_emulator& emu, const macos_syscall_context& context, const uint32_t number, std::string_view)
    {
        if (!tracing_arguments(emu))
        {
            return;
        }

        // A tracing failure must never terminate emulation, so the whole decode is fenced. Nothing below
        // this line is allowed to observe the exception; the trace degrades to one marker row.
        try
        {
            publish(emu, describe_bsd_syscall(emu.memory, number, collect_arguments(context), make_options(emu)));
        }
        catch (...)
        {
            emu.callbacks.on_trace_detail(macos_trace_detail{.label = {}, .value = "<argument decoding failed>"});
        }
    }

    void emit_mach_trap_trace(macos_emulator& emu, const macos_syscall_context& context, const uint32_t index, std::string_view)
    {
        if (!tracing_arguments(emu))
        {
            return;
        }

        try
        {
            publish(emu, describe_mach_trap(index, collect_arguments(context)));
        }
        catch (...)
        {
            emu.callbacks.on_trace_detail(macos_trace_detail{.label = {}, .value = "<argument decoding failed>"});
        }
    }

    void emit_syscall_error_trace(macos_emulator& emu, const std::string_view name)
    {
        if (!emu.callbacks.on_syscall_error)
        {
            return;
        }

        // Darwin signals failure out of band: the carry flag says a call failed and x0 holds a positive
        // errno rather than a negative return value, so nothing about x0 alone distinguishes -1 from an
        // error code.
        const auto nzcv = emu.emu().reg(arm64_register::nzcv);
        if ((nzcv & MACOS_NZCV_CARRY) == 0)
        {
            return;
        }

        const auto error = static_cast<int64_t>(emu.emu().reg(arm64_register::x0));
        emu.callbacks.on_syscall_error(name, error, macos_errno_name(error));
    }
}

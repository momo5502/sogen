#pragma once

#include <cstdint>
#include <string_view>

namespace sogen
{
    enum class stop_reason : uint8_t
    {
        none,
        unknown_syscall,
        unimplemented_syscall,
        syscall_exception,
        instruction_limit,
        normal_exit,
        signal_termination,
        unhandled_memory_violation,
        explicit_stop,
        backend_error,
        breakpoint,
        watchpoint,
        unhandled_cpu_exception,
        mach_receive_deadlock,
        ulock_wait_deadlock,
        workqueue_park_deadlock,
        image_load_failure,
        semwait_signal_deadlock,
        psynch_wait_deadlock,
    };

    constexpr std::string_view stop_reason_name(const stop_reason reason)
    {
        switch (reason)
        {
        case stop_reason::none:
            return "none";
        case stop_reason::unknown_syscall:
            return "unknown_syscall";
        case stop_reason::unimplemented_syscall:
            return "unimplemented_syscall";
        case stop_reason::syscall_exception:
            return "syscall_exception";
        case stop_reason::instruction_limit:
            return "instruction_limit";
        case stop_reason::normal_exit:
            return "normal_exit";
        case stop_reason::signal_termination:
            return "signal_termination";
        case stop_reason::unhandled_memory_violation:
            return "unhandled_memory_violation";
        case stop_reason::explicit_stop:
            return "explicit_stop";
        case stop_reason::backend_error:
            return "backend_error";
        case stop_reason::breakpoint:
            return "breakpoint";
        case stop_reason::watchpoint:
            return "watchpoint";
        case stop_reason::unhandled_cpu_exception:
            return "unhandled_cpu_exception";
        case stop_reason::mach_receive_deadlock:
            return "mach_receive_deadlock";
        case stop_reason::ulock_wait_deadlock:
            return "ulock_wait_deadlock";
        case stop_reason::workqueue_park_deadlock:
            return "workqueue_park_deadlock";
        case stop_reason::image_load_failure:
            return "image_load_failure";
        case stop_reason::semwait_signal_deadlock:
            return "semwait_signal_deadlock";
        case stop_reason::psynch_wait_deadlock:
            return "psynch_wait_deadlock";
        }

        return "unknown";
    }
}

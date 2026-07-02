#pragma once

#include <cstdint>

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
    };
}

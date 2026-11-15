#pragma once

#include "seccomp_state.hpp"

#include <csignal>

namespace sogen::seccomp
{
    struct single_step_result
    {
        int signal{};
        siginfo_t info{};
    };

    single_step_result single_step_guest(register_state& registers, int entry_signal);
}

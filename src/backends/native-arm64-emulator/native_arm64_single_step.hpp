#pragma once

#include "native_arm64_state.hpp"

#include <csignal>

namespace sogen::native_arm64
{
    struct single_step_result
    {
        int signal{};
        siginfo_t info{};
    };

    single_step_result single_step_native(register_state& registers, int entry_signal);
}

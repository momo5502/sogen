#pragma once

#include <posix_signals.hpp>

#include <array>
#include <cstdint>

namespace sogen
{
    constexpr int MACOS_NSIG = 32;
    constexpr int MACOS_SIGKILL = 9;
    constexpr int MACOS_SIGSTOP = 17;
    constexpr posix_signal_numbers MACOS_SIGNAL_NUMBERS{.highest = MACOS_NSIG - 1, .kill = MACOS_SIGKILL, .stop = MACOS_SIGSTOP};
    constexpr uint64_t MACOS_SIG_DFL = 0;
    constexpr uint64_t MACOS_SIG_IGN = 1;
    constexpr int32_t MACOS_SS_DISABLE = 4;
    constexpr posix_signal_stack MACOS_DISABLED_SIGNAL_STACK{.flags = MACOS_SS_DISABLE};

    struct macos_signal_action
    {
        uint64_t handler{};
        uint32_t mask{};
        int32_t flags{};
    };

    // Signals are never delivered in the emulator, so the dispositions only need to read back what was set.
    struct macos_signal_state
    {
        uint32_t mask{};
        std::array<macos_signal_action, MACOS_NSIG> actions{};
        posix_signal_stack stack{MACOS_DISABLED_SIGNAL_STACK};

        // XNU's execsigs: caught signals revert to their default action, ignored ones stay ignored, the mask is kept and
        // the alternate stack is dropped.
        void reset_on_exec()
        {
            for (auto& action : this->actions)
            {
                if (action.handler != MACOS_SIG_DFL && action.handler != MACOS_SIG_IGN)
                {
                    action = {};
                }
            }

            this->stack = MACOS_DISABLED_SIGNAL_STACK;
        }
    };
}

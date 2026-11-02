#pragma once

#include <array>
#include <cstdint>

namespace sogen
{
    constexpr int MACOS_NSIG = 32;
    constexpr int MACOS_SIGKILL = 9;
    constexpr int MACOS_SIGSTOP = 17;
    constexpr uint64_t MACOS_SIG_DFL = 0;
    constexpr uint64_t MACOS_SIG_IGN = 1;
    constexpr int32_t MACOS_SS_ONSTACK = 1;
    constexpr int32_t MACOS_SS_DISABLE = 4;

    struct macos_signal_action
    {
        uint64_t handler{};
        uint32_t mask{};
        int32_t flags{};
    };

    struct macos_signal_stack
    {
        uint64_t base{};
        uint64_t size{};
        int32_t flags{MACOS_SS_DISABLE};
    };

    // Signals are never delivered in the emulator, so the dispositions only need to read back what was set.
    struct macos_signal_state
    {
        uint32_t mask{};
        std::array<macos_signal_action, MACOS_NSIG> actions{};
        macos_signal_stack stack{};

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

            this->stack = {};
        }
    };
}

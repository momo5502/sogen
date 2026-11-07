#pragma once

#include "posix_result.hpp"

#include <cstdint>

namespace sogen
{
    // Signals are numbered from 1; Linux goes up to 64 and XNU to 31, and the two place SIGKILL and SIGSTOP, which
    // cannot be caught, blocked or ignored, differently.
    struct posix_signal_numbers
    {
        int highest{};
        int kill{};
        int stop{};
    };

    constexpr uint64_t posix_signal_bit(const int signal)
    {
        return uint64_t{1} << (signal - 1);
    }

    constexpr uint64_t posix_unblockable_signals(const posix_signal_numbers& numbers)
    {
        return posix_signal_bit(numbers.kill) | posix_signal_bit(numbers.stop);
    }

    constexpr bool posix_is_valid_signal(const posix_signal_numbers& numbers, const int signal)
    {
        return signal > 0 && signal <= numbers.highest;
    }

    constexpr bool posix_is_catchable_signal(const posix_signal_numbers& numbers, const int signal)
    {
        return posix_is_valid_signal(numbers, signal) && signal != numbers.kill && signal != numbers.stop;
    }

    enum class posix_signal_mask_operation
    {
        block,
        unblock,
        set,
    };

    // Like the kernels, the signals that cannot be blocked are silently left out of the mask.
    uint64_t posix_updated_signal_mask(uint64_t mask, posix_signal_mask_operation operation, uint64_t set,
                                       const posix_signal_numbers& numbers);

    struct posix_signal_stack
    {
        uint64_t base{};
        uint64_t size{};
        int32_t flags{};
    };

    // SS_ONSTACK is 1 on both kernels, while SS_DISABLE and MINSIGSTKSZ differ. A request asks for one mode, enabled
    // or disabled, alongside flags the stack keeps and reports back (Linux's SS_AUTODISARM); Linux also takes
    // SS_ONSTACK as a request to enable the stack.
    struct posix_signal_stack_rules
    {
        int32_t disable_flag{};
        int32_t kept_flags{};
        bool on_stack_enables{};
        uint64_t minimum_size{};
    };

    constexpr int32_t POSIX_SS_ONSTACK = 1;

    posix_result posix_set_signal_stack(posix_signal_stack& stack, const posix_signal_stack& requested,
                                        const posix_signal_stack_rules& rules);
}

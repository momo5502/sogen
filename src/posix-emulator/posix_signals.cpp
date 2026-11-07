#include "posix_signals.hpp"

namespace sogen
{
    uint64_t posix_updated_signal_mask(const uint64_t mask, const posix_signal_mask_operation operation, const uint64_t set,
                                       const posix_signal_numbers& numbers)
    {
        uint64_t updated = mask;
        switch (operation)
        {
        case posix_signal_mask_operation::block:
            updated |= set;
            break;
        case posix_signal_mask_operation::unblock:
            updated &= ~set;
            break;
        case posix_signal_mask_operation::set:
            updated = set;
            break;
        }

        return updated & ~posix_unblockable_signals(numbers);
    }

    posix_result posix_set_signal_stack(posix_signal_stack& stack, const posix_signal_stack& requested,
                                        const posix_signal_stack_rules& rules)
    {
        if ((requested.flags & ~rules.accepted_flags) != 0)
        {
            return posix_errno::einval;
        }

        if ((stack.flags & POSIX_SS_ONSTACK) != 0)
        {
            return posix_errno::eperm;
        }

        if ((requested.flags & rules.disable_flag) != 0)
        {
            stack.flags = rules.disable_flag;
            return uint64_t{0};
        }

        if (requested.size < rules.minimum_size)
        {
            return posix_errno::enomem;
        }

        stack = {.base = requested.base, .size = requested.size, .flags = 0};
        return uint64_t{0};
    }
}

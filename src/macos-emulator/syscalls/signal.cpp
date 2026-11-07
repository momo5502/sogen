#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <posix_signals.hpp>

#include <optional>

namespace sogen
{
    namespace
    {
        // The kernel side of sigaction takes struct __sigaction, which carries libc's signal trampoline, and
        // returns the plain struct sigaction.
        struct user_sigaction_with_trampoline
        {
            uint64_t handler;
            uint64_t trampoline;
            uint32_t mask;
            int32_t flags;
        };

        struct user_sigaction
        {
            uint64_t handler;
            uint32_t mask;
            int32_t flags;
        };

        struct user_stack
        {
            uint64_t base;
            uint64_t size;
            int32_t flags;
        };

        static_assert(sizeof(user_sigaction_with_trampoline) == 24);
        static_assert(sizeof(user_sigaction) == 16);
        static_assert(sizeof(user_stack) == 24);

        constexpr auto UNBLOCKABLE_SIGNALS = static_cast<uint32_t>(posix_unblockable_signals(MACOS_SIGNAL_NUMBERS));
        constexpr posix_signal_stack_rules SIGNAL_STACK_RULES{
            .disable_flag = MACOS_SS_DISABLE,
            .kept_flags = 0,
            .on_stack_enables = false,
            .minimum_size = 32768,
        };

        std::optional<posix_signal_mask_operation> mask_operation(const uint64_t how)
        {
            constexpr uint64_t sig_block = 1;
            constexpr uint64_t sig_unblock = 2;
            constexpr uint64_t sig_setmask = 3;

            switch (how)
            {
            case sig_block:
                return posix_signal_mask_operation::block;
            case sig_unblock:
                return posix_signal_mask_operation::unblock;
            case sig_setmask:
                return posix_signal_mask_operation::set;
            default:
                return std::nullopt;
            }
        }
    }

    void sys_sigprocmask(const macos_syscall_context& c)
    {
        const auto how = get_macos_syscall_argument(c.emu, 0);
        const auto set_pointer = get_macos_syscall_argument(c.emu, 1);
        const auto old_set_pointer = get_macos_syscall_argument(c.emu, 2);

        auto& mask = c.emu_ref.signals.mask;
        if (old_set_pointer != 0 && !c.emu.try_write_memory(old_set_pointer, &mask, sizeof(mask)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (set_pointer != 0)
        {
            uint32_t set{};
            if (!c.emu.try_read_memory(set_pointer, &set, sizeof(set)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            const auto operation = mask_operation(how);
            if (!operation)
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                return;
            }

            mask = static_cast<uint32_t>(posix_updated_signal_mask(mask, *operation, set, MACOS_SIGNAL_NUMBERS));
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_sigaction(const macos_syscall_context& c)
    {
        const auto signal = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto new_action_pointer = get_macos_syscall_argument(c.emu, 1);
        const auto old_action_pointer = get_macos_syscall_argument(c.emu, 2);

        if (!posix_is_catchable_signal(MACOS_SIGNAL_NUMBERS, signal))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        user_sigaction_with_trampoline new_action{};
        if (new_action_pointer != 0 && !c.emu.try_read_memory(new_action_pointer, &new_action, sizeof(new_action)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        auto& action = c.emu_ref.signals.actions.at(static_cast<size_t>(signal));
        if (old_action_pointer != 0)
        {
            const user_sigaction old_action{.handler = action.handler, .mask = action.mask, .flags = action.flags};
            if (!c.emu.try_write_memory(old_action_pointer, &old_action, sizeof(old_action)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }
        }

        if (new_action_pointer != 0)
        {
            action = {.handler = new_action.handler, .mask = new_action.mask & ~UNBLOCKABLE_SIGNALS, .flags = new_action.flags};
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_sigaltstack(const macos_syscall_context& c)
    {
        const auto new_stack_pointer = get_macos_syscall_argument(c.emu, 0);
        const auto old_stack_pointer = get_macos_syscall_argument(c.emu, 1);

        auto& stack = c.emu_ref.signals.stack;
        if (old_stack_pointer != 0)
        {
            const user_stack old_stack{.base = stack.base, .size = stack.size, .flags = stack.flags};
            if (!c.emu.try_write_memory(old_stack_pointer, &old_stack, sizeof(old_stack)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }
        }

        if (new_stack_pointer == 0)
        {
            write_macos_syscall_success(c, 0);
            return;
        }

        user_stack new_stack{};
        if (!c.emu.try_read_memory(new_stack_pointer, &new_stack, sizeof(new_stack)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(
            c,
            posix_set_signal_stack(stack, {.base = new_stack.base, .size = new_stack.size, .flags = new_stack.flags}, SIGNAL_STACK_RULES));
    }

    // libc's longjmp tells the kernel it left the alternate signal stack through sigreturn with no context. Real
    // returns from a handler cannot happen, since signals are never delivered.
    void sys_sigreturn(const macos_syscall_context& c)
    {
        constexpr uint64_t set_alternate_stack = 0x40000000;
        constexpr uint64_t reset_alternate_stack = 0x80000000;

        const auto style = get_macos_syscall_argument(c.emu, 1) & 0xFFFFFFFF;
        auto& stack = c.emu_ref.signals.stack;
        if (style == set_alternate_stack)
        {
            stack.flags |= POSIX_SS_ONSTACK;
        }
        else if (style == reset_alternate_stack)
        {
            stack.flags &= ~POSIX_SS_ONSTACK;
        }
        else
        {
            c.emu_ref.log.error("Unhandled sigreturn(style 0x%llx)\n", static_cast<unsigned long long>(style));
            c.emu_ref.stop();
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}

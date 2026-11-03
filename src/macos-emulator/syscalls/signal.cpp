#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

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

        constexpr uint32_t signal_bit(const int signal)
        {
            return 1U << (signal - 1);
        }

        constexpr uint32_t UNBLOCKABLE_SIGNALS = signal_bit(MACOS_SIGKILL) | signal_bit(MACOS_SIGSTOP);
        constexpr uint64_t MINIMUM_SIGNAL_STACK_SIZE = 32768;
    }

    void sys_sigprocmask(const macos_syscall_context& c)
    {
        constexpr uint64_t sig_block = 1;
        constexpr uint64_t sig_unblock = 2;
        constexpr uint64_t sig_setmask = 3;

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

            switch (how)
            {
            case sig_block:
                mask |= set;
                break;
            case sig_unblock:
                mask &= ~set;
                break;
            case sig_setmask:
                mask = set;
                break;
            default:
                write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                return;
            }
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_sigaction(const macos_syscall_context& c)
    {
        const auto signal = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto new_action_pointer = get_macos_syscall_argument(c.emu, 1);
        const auto old_action_pointer = get_macos_syscall_argument(c.emu, 2);

        if (signal <= 0 || signal >= MACOS_NSIG || signal == MACOS_SIGKILL || signal == MACOS_SIGSTOP)
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

        if ((new_stack.flags & ~MACOS_SS_DISABLE) != 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        if ((stack.flags & MACOS_SS_ONSTACK) != 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EPERM);
            return;
        }

        if ((new_stack.flags & MACOS_SS_DISABLE) != 0)
        {
            stack.flags = MACOS_SS_DISABLE;
            write_macos_syscall_success(c, 0);
            return;
        }

        if (new_stack.size < MINIMUM_SIGNAL_STACK_SIZE)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
            return;
        }

        stack = {.base = new_stack.base, .size = new_stack.size, .flags = 0};
        write_macos_syscall_success(c, 0);
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
            stack.flags |= MACOS_SS_ONSTACK;
        }
        else if (style == reset_alternate_stack)
        {
            stack.flags &= ~MACOS_SS_ONSTACK;
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

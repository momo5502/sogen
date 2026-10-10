#include "macos_syscall_dispatcher.hpp"
#include "macos_emulator.hpp"
#include "syscalls/macos_syscalls.hpp"

#include <cinttypes>

namespace sogen
{
    namespace
    {
        // XNU encodes the trap class in the top byte of rax: 1 = Mach, 2 = Unix (BSD), 3 = machine-dependent.
        constexpr uint64_t SYSCALL_CLASS_SHIFT = 24;
        constexpr uint64_t SYSCALL_NUMBER_MASK = 0xFFFFFF;
        constexpr uint64_t SYSCALL_CLASS_UNIX = 2;

        constexpr uint64_t SYS_EXIT = 1;
        constexpr uint64_t SYS_WRITE = 4;
        constexpr uint64_t SYS_MPROTECT = 74;
    }

    macos_syscall_dispatcher::macos_syscall_dispatcher()
    {
        this->register_unix_handler(SYS_EXIT, sys_exit, "exit");
        this->register_unix_handler(SYS_WRITE, sys_write, "write");
        this->register_unix_handler(SYS_MPROTECT, sys_mprotect, "mprotect");
    }

    void macos_syscall_dispatcher::register_unix_handler(const uint64_t number, const macos_syscall_handler handler,
                                                         const std::string_view name)
    {
        this->unix_handlers_.at(number) = {.handler = handler, .name = std::string{name}};
    }

    instruction_hook_continuation macos_syscall_dispatcher::dispatch(macos_emulator& emu) const
    {
        const auto rax = emu.emu().reg(x86_register::rax);
        const auto syscall_class = rax >> SYSCALL_CLASS_SHIFT;
        const auto number = rax & SYSCALL_NUMBER_MASK;

        const auto* entry =
            syscall_class == SYSCALL_CLASS_UNIX && number < this->unix_handlers_.size() ? &this->unix_handlers_.at(number) : nullptr;
        if (entry == nullptr || entry->handler == nullptr)
        {
            emu.log.error("Unimplemented syscall: class %" PRIu64 ", number %" PRIu64 " (rax=0x%" PRIx64 ")\n", syscall_class, number, rax);
            emu.stop();
            return instruction_hook_continuation::skip_instruction;
        }

        entry->handler({.emu_ref = emu, .emu = emu.emu()});
        return instruction_hook_continuation::skip_instruction;
    }
}

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
        constexpr uint64_t SYSCALL_CLASS_MACH = 1;
        constexpr uint64_t SYSCALL_CLASS_UNIX = 2;
        constexpr uint64_t SYSCALL_CLASS_MACHDEP = 3;

        constexpr uint64_t MACHDEP_THREAD_FAST_SET_CTHREAD_SELF = 3;

        constexpr uint64_t MACH_VM_ALLOCATE_TRAP = 10;
        constexpr uint64_t MACH_VM_DEALLOCATE_TRAP = 12;
        constexpr uint64_t MACH_VM_PROTECT_TRAP = 14;
        constexpr uint64_t MACH_VM_MAP_TRAP = 15;
        constexpr uint64_t MACH_PORT_DEALLOCATE_TRAP = 18;
        constexpr uint64_t MACH_PORT_CONSTRUCT_TRAP = 24;
        constexpr uint64_t MACH_REPLY_PORT = 26;
        constexpr uint64_t MACH_TASK_SELF_TRAP = 28;
        constexpr uint64_t MACH_MSG2_TRAP = 47;
        constexpr uint64_t MACH_HOST_SELF_TRAP = 29;

        constexpr uint64_t SYS_EXIT = 1;
        constexpr uint64_t SYS_WRITE = 4;
        constexpr uint64_t SYS_OPEN = 5;
        constexpr uint64_t SYS_CLOSE = 6;
        constexpr uint64_t SYS_GETPID = 20;
        constexpr uint64_t SYS_SIGPROCMASK = 48;
        constexpr uint64_t SYS_MPROTECT = 74;
        constexpr uint64_t SYS_FCNTL = 92;
        constexpr uint64_t SYS_SYSCTL = 202;
        constexpr uint64_t SYS_SHARED_REGION_CHECK_NP = 294;
        constexpr uint64_t SYS_THREAD_SELFID = 372;
        constexpr uint64_t SYS_MAC_SYSCALL = 381;
        constexpr uint64_t SYS_CSRCTL = 483;
        constexpr uint64_t SYS_ABORT_WITH_PAYLOAD = 521;
    }

    macos_syscall_dispatcher::macos_syscall_dispatcher()
    {
        this->register_unix_handler(SYS_EXIT, sys_exit, "exit");
        this->register_unix_handler(SYS_WRITE, sys_write, "write");
        this->register_unix_handler(SYS_OPEN, sys_open, "open");
        this->register_unix_handler(SYS_CLOSE, sys_close, "close");
        this->register_unix_handler(SYS_GETPID, sys_getpid, "getpid");
        this->register_unix_handler(SYS_SIGPROCMASK, sys_sigprocmask, "sigprocmask");
        this->register_unix_handler(SYS_MPROTECT, sys_mprotect, "mprotect");
        this->register_unix_handler(SYS_FCNTL, sys_fcntl, "fcntl");
        this->register_unix_handler(SYS_SYSCTL, sys_sysctl, "sysctl");
        this->register_unix_handler(SYS_SHARED_REGION_CHECK_NP, sys_shared_region_check_np, "shared_region_check_np");
        this->register_unix_handler(SYS_THREAD_SELFID, sys_thread_selfid, "thread_selfid");
        this->register_unix_handler(SYS_MAC_SYSCALL, sys_mac_syscall, "__mac_syscall");
        this->register_unix_handler(SYS_CSRCTL, sys_csrctl, "csrctl");
        this->register_unix_handler(SYS_ABORT_WITH_PAYLOAD, sys_abort_with_payload, "abort_with_payload");

        this->register_machdep_handler(MACHDEP_THREAD_FAST_SET_CTHREAD_SELF, machdep_thread_fast_set_cthread_self,
                                       "thread_fast_set_cthread_self");

        this->register_mach_handler(MACH_VM_ALLOCATE_TRAP, mach_vm_allocate_trap, "_kernelrpc_mach_vm_allocate_trap");
        this->register_mach_handler(MACH_VM_DEALLOCATE_TRAP, mach_vm_deallocate_trap, "_kernelrpc_mach_vm_deallocate_trap");
        this->register_mach_handler(MACH_VM_PROTECT_TRAP, mach_vm_protect_trap, "_kernelrpc_mach_vm_protect_trap");
        this->register_mach_handler(MACH_VM_MAP_TRAP, mach_vm_map_trap, "_kernelrpc_mach_vm_map_trap");
        this->register_mach_handler(MACH_PORT_DEALLOCATE_TRAP, mach_port_deallocate_trap, "_kernelrpc_mach_port_deallocate_trap");
        this->register_mach_handler(MACH_PORT_CONSTRUCT_TRAP, mach_port_construct_trap, "_kernelrpc_mach_port_construct_trap");
        this->register_mach_handler(MACH_REPLY_PORT, mach_reply_port, "mach_reply_port");
        this->register_mach_handler(MACH_TASK_SELF_TRAP, mach_task_self_trap, "task_self_trap");
        this->register_mach_handler(MACH_HOST_SELF_TRAP, mach_host_self_trap, "host_self_trap");
        this->register_mach_handler(MACH_MSG2_TRAP, mach_msg2_trap, "mach_msg2_trap");
    }

    void macos_syscall_dispatcher::register_unix_handler(const uint64_t number, const macos_syscall_handler handler,
                                                         const std::string_view name)
    {
        this->unix_handlers_.at(number) = {.handler = handler, .name = std::string{name}};
    }

    void macos_syscall_dispatcher::register_mach_handler(const uint64_t number, const macos_syscall_handler handler,
                                                         const std::string_view name)
    {
        this->mach_handlers_.at(number) = {.handler = handler, .name = std::string{name}};
    }

    void macos_syscall_dispatcher::register_machdep_handler(const uint64_t number, const macos_syscall_handler handler,
                                                            const std::string_view name)
    {
        this->machdep_handlers_.at(number) = {.handler = handler, .name = std::string{name}};
    }

    const macos_syscall_entry* macos_syscall_dispatcher::find_entry(const uint64_t syscall_class, const uint64_t number) const
    {
        if (syscall_class == SYSCALL_CLASS_UNIX && number < this->unix_handlers_.size())
        {
            return &this->unix_handlers_.at(number);
        }

        if (syscall_class == SYSCALL_CLASS_MACH && number < this->mach_handlers_.size())
        {
            return &this->mach_handlers_.at(number);
        }

        if (syscall_class == SYSCALL_CLASS_MACHDEP && number < this->machdep_handlers_.size())
        {
            return &this->machdep_handlers_.at(number);
        }

        return nullptr;
    }

    instruction_hook_continuation macos_syscall_dispatcher::dispatch(macos_emulator& emu) const
    {
        const auto rax = emu.emu().reg(x86_register::rax);
        const auto syscall_class = rax >> SYSCALL_CLASS_SHIFT;
        const auto number = rax & SYSCALL_NUMBER_MASK;

        const auto* entry = this->find_entry(syscall_class, number);
        if (entry == nullptr || entry->handler == nullptr)
        {
            emu.log.error("Unimplemented syscall: class %" PRIu64 ", number %" PRIu64 " (rax=0x%" PRIx64 ")\n", syscall_class, number, rax);
            emu.stop();
            return instruction_hook_continuation::skip_instruction;
        }

        emu.log.log("%s (rip 0x%" PRIx64 ")\n", entry->name.c_str(), emu.emu().read_instruction_pointer());
        entry->handler({.emu_ref = emu, .emu = emu.emu()});
        return instruction_hook_continuation::skip_instruction;
    }
}

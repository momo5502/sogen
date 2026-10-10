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
        constexpr uint64_t MACH_PORT_MOD_REFS_TRAP = 19;
        constexpr uint64_t MACH_PORT_CONSTRUCT_TRAP = 24;
        constexpr uint64_t MACH_REPLY_PORT = 26;
        constexpr uint64_t MACH_THREAD_SELF_TRAP = 27;
        constexpr uint64_t MACH_TASK_SELF_TRAP = 28;
        constexpr uint64_t MACH_MSG2_TRAP = 47;
        constexpr uint64_t MACH_HOST_SELF_TRAP = 29;

        constexpr uint64_t SYS_EXIT = 1;
        constexpr uint64_t SYS_WRITE = 4;
        constexpr uint64_t SYS_OPEN = 5;
        constexpr uint64_t SYS_CLOSE = 6;
        constexpr uint64_t SYS_GETPID = 20;
        constexpr uint64_t SYS_ACCESS = 33;
        constexpr uint64_t SYS_SIGPROCMASK = 48;
        constexpr uint64_t SYS_IOCTL = 54;
        constexpr uint64_t SYS_MUNMAP = 73;
        constexpr uint64_t SYS_MPROTECT = 74;
        constexpr uint64_t SYS_FCNTL = 92;
        constexpr uint64_t SYS_SOCKET = 97;
        constexpr uint64_t SYS_CONNECT = 98;
        constexpr uint64_t SYS_GETTIMEOFDAY = 116;
        constexpr uint64_t SYS_SENDTO = 133;
        constexpr uint64_t SYS_CSOPS = 169;
        constexpr uint64_t SYS_MMAP = 197;
        constexpr uint64_t SYS_SYSCTL = 202;
        constexpr uint64_t SYS_GETATTRLIST = 220;
        constexpr uint64_t SYS_SHM_OPEN = 266;
        constexpr uint64_t SYS_SHARED_REGION_CHECK_NP = 294;
        constexpr uint64_t SYS_ISSETUGID = 327;
        constexpr uint64_t SYS_PROC_INFO = 336;
        constexpr uint64_t SYS_STAT64 = 338;
        constexpr uint64_t SYS_FSTAT64 = 339;
        constexpr uint64_t SYS_LSTAT64 = 340;
        constexpr uint64_t SYS_GETFSSTAT64 = 347;
        constexpr uint64_t SYS_BSDTHREAD_REGISTER = 366;
        constexpr uint64_t SYS_THREAD_SELFID = 372;
        constexpr uint64_t SYS_FSGETPATH = 427;
        constexpr uint64_t SYS_MAC_SYSCALL = 381;
        constexpr uint64_t SYS_CSRCTL = 483;
        constexpr uint64_t SYS_GETENTROPY = 500;
        constexpr uint64_t SYS_ABORT_WITH_PAYLOAD = 521;
        constexpr uint64_t SYS_MAP_WITH_LINKING_NP = 550;
    }

    macos_syscall_dispatcher::macos_syscall_dispatcher()
    {
        this->register_unix_handler(SYS_EXIT, sys_exit, "exit");
        this->register_unix_handler(SYS_WRITE, sys_write, "write");
        this->register_unix_handler(SYS_OPEN, sys_open, "open");
        this->register_unix_handler(SYS_CLOSE, sys_close, "close");
        this->register_unix_handler(SYS_GETPID, sys_getpid, "getpid");
        this->register_unix_handler(SYS_ACCESS, sys_access, "access");
        this->register_unix_handler(SYS_SIGPROCMASK, sys_sigprocmask, "sigprocmask");
        this->register_unix_handler(SYS_IOCTL, sys_ioctl, "ioctl");
        this->register_unix_handler(SYS_MUNMAP, sys_munmap, "munmap");
        this->register_unix_handler(SYS_MPROTECT, sys_mprotect, "mprotect");
        this->register_unix_handler(SYS_FCNTL, sys_fcntl, "fcntl");
        this->register_unix_handler(SYS_SOCKET, sys_socket, "socket");
        this->register_unix_handler(SYS_CONNECT, sys_connect, "connect");
        this->register_unix_handler(SYS_SENDTO, sys_sendto, "sendto");
        this->register_unix_handler(SYS_GETTIMEOFDAY, sys_gettimeofday, "gettimeofday");
        this->register_unix_handler(SYS_CSOPS, sys_csops, "csops");
        this->register_unix_handler(SYS_MMAP, sys_mmap, "mmap");
        this->register_unix_handler(SYS_SYSCTL, sys_sysctl, "sysctl");
        this->register_unix_handler(SYS_GETATTRLIST, sys_getattrlist, "getattrlist");
        this->register_unix_handler(SYS_SHM_OPEN, sys_shm_open, "shm_open");
        this->register_unix_handler(SYS_SHARED_REGION_CHECK_NP, sys_shared_region_check_np, "shared_region_check_np");
        this->register_unix_handler(SYS_ISSETUGID, sys_issetugid, "issetugid");
        this->register_unix_handler(SYS_PROC_INFO, sys_proc_info, "proc_info");
        this->register_unix_handler(SYS_STAT64, sys_stat64, "stat64");
        this->register_unix_handler(SYS_FSTAT64, sys_fstat64, "fstat64");
        this->register_unix_handler(SYS_LSTAT64, sys_lstat64, "lstat64");
        this->register_unix_handler(SYS_GETFSSTAT64, sys_getfsstat64, "getfsstat64");
        this->register_unix_handler(SYS_BSDTHREAD_REGISTER, sys_bsdthread_register, "bsdthread_register");
        this->register_unix_handler(SYS_THREAD_SELFID, sys_thread_selfid, "thread_selfid");
        this->register_unix_handler(SYS_MAC_SYSCALL, sys_mac_syscall, "__mac_syscall");
        this->register_unix_handler(SYS_FSGETPATH, sys_fsgetpath, "fsgetpath");
        this->register_unix_handler(SYS_CSRCTL, sys_csrctl, "csrctl");
        this->register_unix_handler(SYS_GETENTROPY, sys_getentropy, "getentropy");
        this->register_unix_handler(SYS_ABORT_WITH_PAYLOAD, sys_abort_with_payload, "abort_with_payload");
        this->register_unix_handler(SYS_MAP_WITH_LINKING_NP, sys_map_with_linking_np, "map_with_linking_np");

        this->register_machdep_handler(MACHDEP_THREAD_FAST_SET_CTHREAD_SELF, machdep_thread_fast_set_cthread_self,
                                       "thread_fast_set_cthread_self");

        this->register_mach_handler(MACH_VM_ALLOCATE_TRAP, mach_vm_allocate_trap, "_kernelrpc_mach_vm_allocate_trap");
        this->register_mach_handler(MACH_VM_DEALLOCATE_TRAP, mach_vm_deallocate_trap, "_kernelrpc_mach_vm_deallocate_trap");
        this->register_mach_handler(MACH_VM_PROTECT_TRAP, mach_vm_protect_trap, "_kernelrpc_mach_vm_protect_trap");
        this->register_mach_handler(MACH_VM_MAP_TRAP, mach_vm_map_trap, "_kernelrpc_mach_vm_map_trap");
        this->register_mach_handler(MACH_PORT_DEALLOCATE_TRAP, mach_port_deallocate_trap, "_kernelrpc_mach_port_deallocate_trap");
        this->register_mach_handler(MACH_PORT_MOD_REFS_TRAP, mach_port_mod_refs_trap, "_kernelrpc_mach_port_mod_refs_trap");
        this->register_mach_handler(MACH_PORT_CONSTRUCT_TRAP, mach_port_construct_trap, "_kernelrpc_mach_port_construct_trap");
        this->register_mach_handler(MACH_REPLY_PORT, mach_reply_port, "mach_reply_port");
        this->register_mach_handler(MACH_THREAD_SELF_TRAP, mach_thread_self_trap, "thread_self_trap");
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

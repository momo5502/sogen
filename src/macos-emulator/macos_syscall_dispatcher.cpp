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
        constexpr uint64_t MACH_THREAD_GET_SPECIAL_REPLY_PORT_TRAP = 50;
        constexpr uint64_t MACH_HOST_CREATE_MACH_VOUCHER_TRAP = 70;
        constexpr uint64_t MACH_HOST_SELF_TRAP = 29;

        constexpr uint64_t MACOS_SYS_exit = 1;
        constexpr uint64_t MACOS_SYS_read = 3;
        constexpr uint64_t MACOS_SYS_write = 4;
        constexpr uint64_t MACOS_SYS_open = 5;
        constexpr uint64_t MACOS_SYS_close = 6;
        constexpr uint64_t MACOS_SYS_chdir = 12;
        constexpr uint64_t MACOS_SYS_fchdir = 13;
        constexpr uint64_t MACOS_SYS_getpid = 20;
        constexpr uint64_t MACOS_SYS_getuid = 24;
        constexpr uint64_t MACOS_SYS_geteuid = 25;
        constexpr uint64_t MACOS_SYS_getpeername = 31;
        constexpr uint64_t MACOS_SYS_access = 33;
        constexpr uint64_t MACOS_SYS_getppid = 39;
        constexpr uint64_t MACOS_SYS_dup = 41;
        constexpr uint64_t MACOS_SYS_getegid = 43;
        constexpr uint64_t MACOS_SYS_sigaction = 46;
        constexpr uint64_t MACOS_SYS_getgid = 47;
        constexpr uint64_t MACOS_SYS_sigprocmask = 48;
        constexpr uint64_t MACOS_SYS_sigaltstack = 53;
        constexpr uint64_t MACOS_SYS_ioctl = 54;
        constexpr uint64_t MACOS_SYS_munmap = 73;
        constexpr uint64_t MACOS_SYS_mprotect = 74;
        constexpr uint64_t MACOS_SYS_getgroups = 79;
        constexpr uint64_t MACOS_SYS_getpgrp = 81;
        constexpr uint64_t MACOS_SYS_setpgid = 82;
        constexpr uint64_t MACOS_SYS_getdtablesize = 89;
        constexpr uint64_t MACOS_SYS_dup2 = 90;
        constexpr uint64_t MACOS_SYS_fcntl = 92;
        constexpr uint64_t MACOS_SYS_socket = 97;
        constexpr uint64_t MACOS_SYS_connect = 98;
        constexpr uint64_t MACOS_SYS_gettimeofday = 116;
        constexpr uint64_t MACOS_SYS_sendto = 133;
        constexpr uint64_t MACOS_SYS_csops = 169;
        constexpr uint64_t MACOS_SYS_csops_audittoken = 170;
        constexpr uint64_t MACOS_SYS_sigreturn = 184;
        constexpr uint64_t MACOS_SYS_pathconf = 191;
        constexpr uint64_t MACOS_SYS_getrlimit = 194;
        constexpr uint64_t MACOS_SYS_mmap = 197;
        constexpr uint64_t MACOS_SYS_lseek = 199;
        constexpr uint64_t MACOS_SYS_sysctl = 202;
        constexpr uint64_t MACOS_SYS_getattrlist = 220;
        constexpr uint64_t MACOS_SYS_fgetattrlist = 228;
        constexpr uint64_t MACOS_SYS_shm_open = 266;
        constexpr uint64_t MACOS_SYS_shared_region_check_np = 294;
        constexpr uint64_t MACOS_SYS_issetugid = 327;
        constexpr uint64_t MACOS_SYS_proc_info = 336;
        constexpr uint64_t MACOS_SYS_stat64 = 338;
        constexpr uint64_t MACOS_SYS_fstat64 = 339;
        constexpr uint64_t MACOS_SYS_lstat64 = 340;
        constexpr uint64_t MACOS_SYS_getdirentries64 = 344;
        constexpr uint64_t MACOS_SYS_fstatfs64 = 346;
        constexpr uint64_t MACOS_SYS_getfsstat64 = 347;
        constexpr uint64_t MACOS_SYS_bsdthread_register = 366;
        constexpr uint64_t MACOS_SYS_read_nocancel = 396;
        constexpr uint64_t MACOS_SYS_write_nocancel = 397;
        constexpr uint64_t MACOS_SYS_open_nocancel = 398;
        constexpr uint64_t MACOS_SYS_close_nocancel = 399;
        constexpr uint64_t MACOS_SYS_fcntl_nocancel = 406;
        constexpr uint64_t MACOS_SYS_thread_selfid = 372;
        constexpr uint64_t MACOS_SYS_fsgetpath = 427;
        constexpr uint64_t MACOS_SYS_mac_syscall = 381;
        constexpr uint64_t MACOS_SYS_getattrlistbulk = 461;
        constexpr uint64_t MACOS_SYS_fstatat64 = 470;
        constexpr uint64_t MACOS_SYS_csrctl = 483;
        constexpr uint64_t MACOS_SYS_getentropy = 500;
        constexpr uint64_t MACOS_SYS_abort_with_payload = 521;
        constexpr uint64_t MACOS_SYS_map_with_linking_np = 550;
    }

    macos_syscall_dispatcher::macos_syscall_dispatcher()
    {
        this->register_unix_handler(MACOS_SYS_exit, sys_exit, "exit");
        this->register_unix_handler(MACOS_SYS_read, sys_read, "read");
        this->register_unix_handler(MACOS_SYS_write, sys_write, "write");
        this->register_unix_handler(MACOS_SYS_open, sys_open, "open");
        this->register_unix_handler(MACOS_SYS_close, sys_close, "close");
        this->register_unix_handler(MACOS_SYS_chdir, sys_chdir, "chdir");
        this->register_unix_handler(MACOS_SYS_fchdir, sys_fchdir, "fchdir");
        this->register_unix_handler(MACOS_SYS_getpid, sys_getpid, "getpid");
        this->register_unix_handler(MACOS_SYS_getuid, sys_getuid, "getuid");
        this->register_unix_handler(MACOS_SYS_geteuid, sys_geteuid, "geteuid");
        this->register_unix_handler(MACOS_SYS_getpeername, sys_getpeername, "getpeername");
        this->register_unix_handler(MACOS_SYS_access, sys_access, "access");
        this->register_unix_handler(MACOS_SYS_getppid, sys_getppid, "getppid");
        this->register_unix_handler(MACOS_SYS_dup, sys_dup, "dup");
        this->register_unix_handler(MACOS_SYS_getegid, sys_getegid, "getegid");
        this->register_unix_handler(MACOS_SYS_sigaction, sys_sigaction, "sigaction");
        this->register_unix_handler(MACOS_SYS_getgid, sys_getgid, "getgid");
        this->register_unix_handler(MACOS_SYS_sigprocmask, sys_sigprocmask, "sigprocmask");
        this->register_unix_handler(MACOS_SYS_sigaltstack, sys_sigaltstack, "sigaltstack");
        this->register_unix_handler(MACOS_SYS_ioctl, sys_ioctl, "ioctl");
        this->register_unix_handler(MACOS_SYS_munmap, sys_munmap, "munmap");
        this->register_unix_handler(MACOS_SYS_mprotect, sys_mprotect, "mprotect");
        this->register_unix_handler(MACOS_SYS_getgroups, sys_getgroups, "getgroups");
        this->register_unix_handler(MACOS_SYS_getpgrp, sys_getpgrp, "getpgrp");
        this->register_unix_handler(MACOS_SYS_setpgid, sys_setpgid, "setpgid");
        this->register_unix_handler(MACOS_SYS_getdtablesize, sys_getdtablesize, "getdtablesize");
        this->register_unix_handler(MACOS_SYS_dup2, sys_dup2, "dup2");
        this->register_unix_handler(MACOS_SYS_fcntl, sys_fcntl, "fcntl");
        this->register_unix_handler(MACOS_SYS_socket, sys_socket, "socket");
        this->register_unix_handler(MACOS_SYS_connect, sys_connect, "connect");
        this->register_unix_handler(MACOS_SYS_sendto, sys_sendto, "sendto");
        this->register_unix_handler(MACOS_SYS_gettimeofday, sys_gettimeofday, "gettimeofday");
        this->register_unix_handler(MACOS_SYS_csops, sys_csops, "csops");
        this->register_unix_handler(MACOS_SYS_csops_audittoken, sys_csops_audittoken, "csops_audittoken");
        this->register_unix_handler(MACOS_SYS_sigreturn, sys_sigreturn, "sigreturn");
        this->register_unix_handler(MACOS_SYS_pathconf, sys_pathconf, "pathconf");
        this->register_unix_handler(MACOS_SYS_getrlimit, sys_getrlimit, "getrlimit");
        this->register_unix_handler(MACOS_SYS_mmap, sys_mmap, "mmap");
        this->register_unix_handler(MACOS_SYS_lseek, sys_lseek, "lseek");
        this->register_unix_handler(MACOS_SYS_sysctl, sys_sysctl, "sysctl");
        this->register_unix_handler(MACOS_SYS_getattrlist, sys_getattrlist, "getattrlist");
        this->register_unix_handler(MACOS_SYS_fgetattrlist, sys_fgetattrlist, "fgetattrlist");
        this->register_unix_handler(MACOS_SYS_shm_open, sys_shm_open, "shm_open");
        this->register_unix_handler(MACOS_SYS_shared_region_check_np, sys_shared_region_check_np, "shared_region_check_np");
        this->register_unix_handler(MACOS_SYS_issetugid, sys_issetugid, "issetugid");
        this->register_unix_handler(MACOS_SYS_proc_info, sys_proc_info, "proc_info");
        this->register_unix_handler(MACOS_SYS_stat64, sys_stat64, "stat64");
        this->register_unix_handler(MACOS_SYS_fstat64, sys_fstat64, "fstat64");
        this->register_unix_handler(MACOS_SYS_lstat64, sys_lstat64, "lstat64");
        this->register_unix_handler(MACOS_SYS_getdirentries64, sys_getdirentries64, "getdirentries64");
        this->register_unix_handler(MACOS_SYS_fstatfs64, sys_fstatfs64, "fstatfs64");
        this->register_unix_handler(MACOS_SYS_getfsstat64, sys_getfsstat64, "getfsstat64");
        this->register_unix_handler(MACOS_SYS_bsdthread_register, sys_bsdthread_register, "bsdthread_register");
        this->register_unix_handler(MACOS_SYS_read_nocancel, sys_read, "read_nocancel");
        this->register_unix_handler(MACOS_SYS_write_nocancel, sys_write, "write_nocancel");
        this->register_unix_handler(MACOS_SYS_open_nocancel, sys_open, "open_nocancel");
        this->register_unix_handler(MACOS_SYS_close_nocancel, sys_close, "close_nocancel");
        this->register_unix_handler(MACOS_SYS_fcntl_nocancel, sys_fcntl, "fcntl_nocancel");
        this->register_unix_handler(MACOS_SYS_thread_selfid, sys_thread_selfid, "thread_selfid");
        this->register_unix_handler(MACOS_SYS_mac_syscall, sys_mac_syscall, "__mac_syscall");
        this->register_unix_handler(MACOS_SYS_fsgetpath, sys_fsgetpath, "fsgetpath");
        this->register_unix_handler(MACOS_SYS_getattrlistbulk, sys_getattrlistbulk, "getattrlistbulk");
        this->register_unix_handler(MACOS_SYS_fstatat64, sys_fstatat64, "fstatat64");
        this->register_unix_handler(MACOS_SYS_csrctl, sys_csrctl, "csrctl");
        this->register_unix_handler(MACOS_SYS_getentropy, sys_getentropy, "getentropy");
        this->register_unix_handler(MACOS_SYS_abort_with_payload, sys_abort_with_payload, "abort_with_payload");
        this->register_unix_handler(MACOS_SYS_map_with_linking_np, sys_map_with_linking_np, "map_with_linking_np");

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
        this->register_mach_handler(MACH_THREAD_GET_SPECIAL_REPLY_PORT_TRAP, mach_thread_get_special_reply_port,
                                    "thread_get_special_reply_port");
        this->register_mach_handler(MACH_HOST_CREATE_MACH_VOUCHER_TRAP, mach_host_create_mach_voucher_trap,
                                    "host_create_mach_voucher_trap");
    }

    void macos_syscall_dispatcher::register_unix_handler(const uint64_t number, const macos_syscall_handler handler,
                                                         const std::string_view name)
    {
        this->unix_handlers_.at(static_cast<size_t>(number)) = {.handler = handler, .name = std::string{name}};
    }

    void macos_syscall_dispatcher::register_mach_handler(const uint64_t number, const macos_syscall_handler handler,
                                                         const std::string_view name)
    {
        this->mach_handlers_.at(static_cast<size_t>(number)) = {.handler = handler, .name = std::string{name}};
    }

    void macos_syscall_dispatcher::register_machdep_handler(const uint64_t number, const macos_syscall_handler handler,
                                                            const std::string_view name)
    {
        this->machdep_handlers_.at(static_cast<size_t>(number)) = {.handler = handler, .name = std::string{name}};
    }

    const macos_syscall_entry* macos_syscall_dispatcher::find_entry(const uint64_t syscall_class, const uint64_t number) const
    {
        if (syscall_class == SYSCALL_CLASS_UNIX && number < this->unix_handlers_.size())
        {
            return &this->unix_handlers_.at(static_cast<size_t>(number));
        }

        if (syscall_class == SYSCALL_CLASS_MACH && number < this->mach_handlers_.size())
        {
            return &this->mach_handlers_.at(static_cast<size_t>(number));
        }

        if (syscall_class == SYSCALL_CLASS_MACHDEP && number < this->machdep_handlers_.size())
        {
            return &this->machdep_handlers_.at(static_cast<size_t>(number));
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

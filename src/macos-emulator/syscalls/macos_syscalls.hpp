#pragma once

#include "../macos_syscall_context.hpp"

namespace sogen
{
    void sys_exit(const macos_syscall_context& c);
    void sys_getpid(const macos_syscall_context& c);
    void sys_thread_selfid(const macos_syscall_context& c);
    void sys_mac_syscall(const macos_syscall_context& c);
    void sys_csrctl(const macos_syscall_context& c);
    void sys_abort_with_payload(const macos_syscall_context& c);
    void sys_write(const macos_syscall_context& c);
    void sys_open(const macos_syscall_context& c);
    void sys_close(const macos_syscall_context& c);
    void sys_fcntl(const macos_syscall_context& c);
    void sys_sigprocmask(const macos_syscall_context& c);
    void sys_mprotect(const macos_syscall_context& c);
    void sys_sysctl(const macos_syscall_context& c);
    void sys_shared_region_check_np(const macos_syscall_context& c);

    void mach_vm_allocate_trap(const macos_syscall_context& c);
    void mach_vm_deallocate_trap(const macos_syscall_context& c);
    void mach_vm_protect_trap(const macos_syscall_context& c);
    void mach_vm_map_trap(const macos_syscall_context& c);
    void mach_port_deallocate_trap(const macos_syscall_context& c);
    void mach_port_construct_trap(const macos_syscall_context& c);
    void mach_reply_port(const macos_syscall_context& c);
    void mach_task_self_trap(const macos_syscall_context& c);
    void mach_host_self_trap(const macos_syscall_context& c);
    void mach_msg2_trap(const macos_syscall_context& c);

    void machdep_thread_fast_set_cthread_self(const macos_syscall_context& c);
}

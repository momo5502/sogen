#pragma once

#include "../macos_syscall_context.hpp"

namespace sogen
{
    void sys_exit(const macos_syscall_context& c);
    void sys_getpid(const macos_syscall_context& c);
    void sys_thread_selfid(const macos_syscall_context& c);
    void sys_write(const macos_syscall_context& c);
    void sys_mprotect(const macos_syscall_context& c);
    void sys_shared_region_check_np(const macos_syscall_context& c);

    void mach_vm_deallocate_trap(const macos_syscall_context& c);
    void mach_vm_protect_trap(const macos_syscall_context& c);
    void mach_vm_map_trap(const macos_syscall_context& c);
    void mach_reply_port(const macos_syscall_context& c);
    void mach_task_self_trap(const macos_syscall_context& c);

    void machdep_thread_fast_set_cthread_self(const macos_syscall_context& c);
}

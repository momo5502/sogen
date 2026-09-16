#pragma once

#include "../macos_syscall_utils.hpp"
#include "mach_types.hpp"

#include <memory_permission.hpp>

namespace sogen::mach_traps
{
    // Mach traps report failure as a kern_return_t in x0 and never touch the carry flag, so these are
    // deliberately not write_macos_syscall_error: libsyscall's trap stubs have no cerror branch.
    void write_mach_result(const macos_syscall_context& c, mach::kern_return_t result);
    void write_mach_port_result(const macos_syscall_context& c, mach::port_name_t name);

    uint64_t current_thread_id(const macos_syscall_context& c);

    memory_permission vm_prot_to_permission(uint32_t protection);

    void trap_vm_allocate(const macos_syscall_context& c);
    void trap_vm_deallocate(const macos_syscall_context& c);
    void trap_vm_protect(const macos_syscall_context& c);
    void trap_vm_map(const macos_syscall_context& c);

    void trap_task_self(const macos_syscall_context& c);
    void trap_host_self(const macos_syscall_context& c);
    void trap_thread_self(const macos_syscall_context& c);

    void trap_port_allocate(const macos_syscall_context& c);
    void trap_port_deallocate(const macos_syscall_context& c);
    void trap_port_mod_refs(const macos_syscall_context& c);
    void trap_port_insert_right(const macos_syscall_context& c);
    void trap_port_move_member(const macos_syscall_context& c);
    void trap_port_insert_member(const macos_syscall_context& c);
    void trap_port_extract_member(const macos_syscall_context& c);
    void trap_port_construct(const macos_syscall_context& c);
    void trap_port_destruct(const macos_syscall_context& c);
    void trap_reply_port(const macos_syscall_context& c);
    void trap_thread_get_special_reply_port(const macos_syscall_context& c);
    void trap_semaphore_signal(const macos_syscall_context& c);
    void trap_semaphore_wait(const macos_syscall_context& c);
    void trap_semaphore_wait_signal(const macos_syscall_context& c);
    void trap_semaphore_timedwait(const macos_syscall_context& c);
    void trap_semaphore_timedwait_signal(const macos_syscall_context& c);
    void trap_semaphore_signal_all(const macos_syscall_context& c);
    void trap_host_create_mach_voucher(const macos_syscall_context& c);
    void trap_mach_timebase_info(const macos_syscall_context& c);
    void trap_mk_timer_create(const macos_syscall_context& c);
    void trap_mk_timer_destroy(const macos_syscall_context& c);
    void trap_mk_timer_arm(const macos_syscall_context& c);
    void trap_mk_timer_arm_leeway(const macos_syscall_context& c);
    void trap_mk_timer_cancel(const macos_syscall_context& c);
    void trap_mach_msg2(const macos_syscall_context& c);
    void trap_port_get_attributes(const macos_syscall_context& c);
    void trap_port_guard(const macos_syscall_context& c);
    void trap_port_unguard(const macos_syscall_context& c);
    void trap_port_type(const macos_syscall_context& c);
    void trap_request_notification(const macos_syscall_context& c);
    void trap_generate_activity_id(const macos_syscall_context& c);
    void trap_task_name_for_pid(const macos_syscall_context& c);
}

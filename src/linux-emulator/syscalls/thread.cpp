#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <cstring>

using namespace linux_errno;

// --- Phase 4b: Threading syscalls ---

// clone flags
namespace
{
    constexpr uint64_t CLONE_VM = 0x00000100;
    constexpr uint64_t CLONE_FS = 0x00000200;
    constexpr uint64_t CLONE_FILES = 0x00000400;
    constexpr uint64_t CLONE_SIGHAND = 0x00000800;
    constexpr uint64_t CLONE_THREAD = 0x00010000;
    constexpr uint64_t CLONE_SETTLS = 0x00080000;
    constexpr uint64_t CLONE_PARENT_SETTID = 0x00100000;
    constexpr uint64_t CLONE_CHILD_CLEARTID = 0x00200000;
    constexpr uint64_t CLONE_CHILD_SETTID = 0x01000000;

    // Futex operations
    constexpr int FUTEX_WAIT = 0;
    constexpr int FUTEX_WAKE = 1;
    constexpr int FUTEX_WAIT_BITSET = 9;
    constexpr int FUTEX_WAKE_BITSET = 10;
    constexpr int FUTEX_PRIVATE_FLAG = 128;
    constexpr int FUTEX_CMD_MASK = ~FUTEX_PRIVATE_FLAG;

    constexpr uint32_t FUTEX_BITSET_MATCH_ANY = 0xFFFFFFFF;
}

void sys_clone(const linux_syscall_context& c)
{
    // clone(unsigned long flags, void *child_stack, int *parent_tid, int *child_tid, unsigned long tls)
    const auto flags = get_linux_syscall_argument(c.emu, 0);
    const auto child_stack = get_linux_syscall_argument(c.emu, 1);
    const auto parent_tid_addr = get_linux_syscall_argument(c.emu, 2);
    const auto child_tid_addr = get_linux_syscall_argument(c.emu, 3);
    const auto tls = get_linux_syscall_argument(c.emu, 4);

    // We only support CLONE_THREAD (pthread_create style) for now
    if (!(flags & CLONE_THREAD))
    {
        // fork()-style clone is not yet supported
        c.emu_ref.log.warn("clone() without CLONE_THREAD not supported (flags=0x%llx)\n", static_cast<unsigned long long>(flags));
        write_linux_syscall_result(c, -LINUX_ENOSYS);
        return;
    }

    if (child_stack == 0)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    // Create the new thread
    const auto new_tid = c.proc.next_tid++;

    linux_thread new_thread{};
    new_thread.tid = new_tid;
    new_thread.stack_base = child_stack; // child_stack points to top of stack
    new_thread.stack_size = 0;           // unknown

    // Save the parent's current register state into the child's saved_regs
    // The child thread will start by returning 0 from clone()
    new_thread.save(c.emu);

    // The child returns 0 from the syscall
    new_thread.saved_regs.rax = 0;

    // The child uses the provided stack
    new_thread.saved_regs.rsp = child_stack;

    // Advance the child's RIP past the syscall instruction (already done by the hook)
    // The saved RIP should already be the return address

    // Set TLS if requested
    if (flags & CLONE_SETTLS)
    {
        new_thread.fs_base = tls;
        new_thread.saved_regs.fs_base = tls;
    }

    // Handle tid addresses
    if (flags & CLONE_CHILD_CLEARTID)
    {
        new_thread.clear_child_tid = child_tid_addr;
    }

    if (flags & CLONE_CHILD_SETTID)
    {
        // Write the child's tid to the address in the child's address space
        const auto tid32 = static_cast<int32_t>(new_tid);
        c.emu.write_memory(child_tid_addr, &tid32, sizeof(tid32));
    }

    if (flags & CLONE_PARENT_SETTID)
    {
        // Write the child's tid to parent_tid in the parent's address space
        const auto tid32 = static_cast<int32_t>(new_tid);
        c.emu.write_memory(parent_tid_addr, &tid32, sizeof(tid32));
    }

    // Insert the thread
    c.proc.threads.emplace(new_tid, std::move(new_thread));

    // Parent returns the child's tid
    write_linux_syscall_result(c, static_cast<int64_t>(new_tid));
}

void sys_futex(const linux_syscall_context& c)
{
    const auto uaddr = get_linux_syscall_argument(c.emu, 0);
    const auto futex_op = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto val = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2));
    // timeout/val2 is arg 3
    // uaddr2 is arg 4
    const auto val3 = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 5));

    const auto cmd = futex_op & FUTEX_CMD_MASK;

    switch (cmd)
    {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        // Read the current value at uaddr
        uint32_t current_val{};
        c.emu.read_memory(uaddr, &current_val, sizeof(current_val));

        if (current_val != val)
        {
            // Value changed — return EAGAIN (spurious wakeup)
            write_linux_syscall_result(c, -LINUX_EAGAIN);
            return;
        }

        // Check if there are other threads that could wake us
        bool has_other_threads = false;
        for (const auto& [tid, t] : c.proc.threads)
        {
            if (tid != c.proc.active_thread->tid && !t.terminated)
            {
                has_other_threads = true;
                break;
            }
        }

        if (!has_other_threads)
        {
            // Single-threaded: no one can wake us, return EAGAIN to avoid deadlock
            write_linux_syscall_result(c, -LINUX_EAGAIN);
            return;
        }

        // Multi-threaded: mark this thread as waiting on the futex
        const auto bitset = (cmd == FUTEX_WAIT_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;

        c.proc.active_thread->wait_state = thread_wait_state::futex_wait;
        c.proc.active_thread->futex_wait_address = uaddr;
        c.proc.active_thread->futex_wait_val = val;
        c.proc.active_thread->futex_wait_bitset = bitset;

        // The result will be set when the thread is woken (0) or on timeout (-ETIMEDOUT)
        // For now, pre-set the result to 0 (success) — it will be woken by FUTEX_WAKE
        write_linux_syscall_result(c, 0);
        break;
    }

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        // Wake up to val threads waiting on this futex address.
        const auto bitset = (cmd == FUTEX_WAKE_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        int woken = 0;
        const auto max_wake = static_cast<int>(val);

        for (auto& [tid, t] : c.proc.threads)
        {
            if (woken >= max_wake)
            {
                break;
            }

            if (t.wait_state == thread_wait_state::futex_wait && t.futex_wait_address == uaddr && (t.futex_wait_bitset & bitset) != 0)
            {
                t.wake_from_futex();
                ++woken;
            }
        }

        write_linux_syscall_result(c, static_cast<int64_t>(woken));
        break;
    }

    default:
        c.emu_ref.log.warn("futex: unsupported operation %d\n", cmd);
        write_linux_syscall_result(c, -LINUX_ENOSYS);
        break;
    }
}

#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <posix_signals.hpp>

#include <cstring>

namespace sogen
{

    using namespace linux_errno; // NOLINT(google-build-using-namespace)

    namespace
    {
        constexpr int ARCH_SET_GS = 0x1001;
        constexpr int ARCH_SET_FS = 0x1002;
        constexpr int ARCH_GET_FS = 0x1003;
        constexpr int ARCH_GET_GS = 0x1004;
        constexpr uint64_t SYSCALL_INSTRUCTION_SIZE = 2;

        constexpr posix_signal_numbers LINUX_SIGNAL_NUMBERS{
            .highest = linux_signals::LINUX_NSIG,
            .kill = linux_signals::LINUX_SIGKILL,
            .stop = linux_signals::LINUX_SIGSTOP,
        };

        constexpr int32_t LINUX_SS_AUTODISARM = static_cast<int32_t>(1U << 31);
        constexpr posix_signal_stack_rules SIGNAL_STACK_RULES{
            .disable_flag = LINUX_SS_DISABLE,
            .kept_flags = LINUX_SS_AUTODISARM,
            .on_stack_enables = true,
            .minimum_size = 2048,
        };

        struct linux_stack
        {
            uint64_t sp;
            int32_t flags;
            int32_t padding;
            uint64_t size;
        };

        static_assert(sizeof(linux_stack) == 24);

        // A disabled stack reads back without a range, as the kernel clears it.
        linux_stack to_linux_stack(const posix_signal_stack& stack)
        {
            if (stack.flags & LINUX_SS_DISABLE)
            {
                return {.sp = 0, .flags = stack.flags, .padding = 0, .size = 0};
            }

            return {.sp = stack.base, .flags = stack.flags, .padding = 0, .size = stack.size};
        }

        std::optional<posix_signal_mask_operation> mask_operation(const uint64_t how)
        {
            constexpr uint64_t LINUX_SIG_BLOCK = 0;
            constexpr uint64_t LINUX_SIG_UNBLOCK = 1;
            constexpr uint64_t LINUX_SIG_SETMASK = 2;

            switch (how)
            {
            case LINUX_SIG_BLOCK:
                return posix_signal_mask_operation::block;
            case LINUX_SIG_UNBLOCK:
                return posix_signal_mask_operation::unblock;
            case LINUX_SIG_SETMASK:
                return posix_signal_mask_operation::set;
            default:
                return std::nullopt;
            }
        }

        // Linux utsname structure
        // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        struct linux_utsname
        {
            char sysname[65];
            char nodename[65];
            char release[65];
            char version[65];
            char machine[65];
            char domainname[65];
        };

        // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

        static_assert(sizeof(linux_utsname) == 390);

        void copy_utsname_field(char* dest, const char* src, size_t max_len)
        {
            memset(dest, 0, max_len);
            const auto len = strlen(src);
            memcpy(dest, src, std::min(len, max_len - 1));
        }

        bool is_valid_signal(const int sig)
        {
            return sig >= 0 && sig <= linux_signals::LINUX_NSIG;
        }

        void deliver_signal_after_success(const linux_syscall_context& c, const int sig)
        {
            write_linux_syscall_result(c, 0);

            // Delivering a handled signal from inside the syscall hook replaces RIP
            // with the user handler. Finalize the syscall first so the backend does
            // not apply its normal syscall-length adjustment to the handler address,
            // and so sigreturn resumes at the instruction after syscall.
            const auto resume_rip = c.emu.read_instruction_pointer() + SYSCALL_INSTRUCTION_SIZE;
            c.emu.reg(x86_register::rip, resume_rip);
            c.mark_instruction_pointer_finalized();

            c.emu_ref.signals.deliver_signal(c.emu_ref, sig, 0, linux_signals::LINUX_SI_USER);
        }
    }

    void sys_exit(const linux_syscall_context& c)
    {
        const auto status = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        if (c.proc.active_thread)
        {
            auto& thread = *c.proc.active_thread;
            thread.terminated = true;
            thread.exit_code = status;
            c.emu_ref.on_thread_terminated(thread);

            if (c.emu_ref.perform_thread_switch())
            {
                return;
            }
        }

        c.proc.exit_status = status;
        c.emu_ref.record_stop(stop_reason::normal_exit, std::to_string(status));
        c.emu_ref.stop();
    }

    void sys_exit_group(const linux_syscall_context& c)
    {
        const auto status = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        for (auto& [_, thread] : c.proc.threads)
        {
            if (!thread.terminated)
            {
                thread.terminated = true;
                thread.exit_code = status;
                c.emu_ref.on_thread_terminated(thread);
            }
        }

        c.proc.exit_status = status;
        c.emu_ref.record_stop(stop_reason::normal_exit, std::to_string(status));
        c.emu_ref.stop();
    }

    void sys_uname(const linux_syscall_context& c)
    {
        const auto buf_addr = get_linux_syscall_argument(c.emu, 0);

        linux_utsname uts{};
        copy_utsname_field(uts.sysname, "Linux", sizeof(uts.sysname));
        copy_utsname_field(uts.nodename, "sogen", sizeof(uts.nodename));
        copy_utsname_field(uts.release, "5.15.0-sogen", sizeof(uts.release));
        copy_utsname_field(uts.version, "#1 SMP", sizeof(uts.version));
        copy_utsname_field(uts.machine, "x86_64", sizeof(uts.machine));
        copy_utsname_field(uts.domainname, "(none)", sizeof(uts.domainname));

        c.emu.write_memory(buf_addr, &uts, sizeof(uts));
        write_linux_syscall_result(c, 0);
    }

    void sys_arch_prctl(const linux_syscall_context& c)
    {
        const auto code = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto addr = get_linux_syscall_argument(c.emu, 1);

        switch (code)
        {
        case ARCH_SET_FS:
            c.emu.set_segment_base(x86_register::fs, addr);
            if (c.proc.active_thread)
            {
                c.proc.active_thread->fs_base = addr;
            }
            write_linux_syscall_result(c, 0);
            break;

        case ARCH_GET_FS: {
            const auto fs_base = c.emu.get_segment_base(x86_register::fs);
            c.emu.write_memory(addr, &fs_base, sizeof(fs_base));
            write_linux_syscall_result(c, 0);
            break;
        }

        case ARCH_SET_GS:
            c.emu.set_segment_base(x86_register::gs, addr);
            write_linux_syscall_result(c, 0);
            break;

        case ARCH_GET_GS: {
            const auto gs_base = c.emu.get_segment_base(x86_register::gs);
            c.emu.write_memory(addr, &gs_base, sizeof(gs_base));
            write_linux_syscall_result(c, 0);
            break;
        }

        default:
            write_linux_syscall_result(c, -LINUX_EINVAL);
            break;
        }
    }

    void sys_prlimit64(const linux_syscall_context& c)
    {
        // Stub: return 0 (success) without actually changing limits
        (void)c;
        write_linux_syscall_result(c, 0);
    }

    void sys_rseq(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, -LINUX_ENOSYS);
    }

    void sys_getpid(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.pid);
    }

    void sys_getppid(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.ppid);
    }

    void sys_getuid(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.uid);
    }

    void sys_getgid(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.gid);
    }

    void sys_geteuid(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.euid);
    }

    void sys_getegid(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.egid);
    }

    void sys_gettid(const linux_syscall_context& c)
    {
        if (c.proc.active_thread)
        {
            write_linux_syscall_result(c, c.proc.active_thread->tid);
        }
        else
        {
            write_linux_syscall_result(c, c.proc.pid);
        }
    }

    void sys_set_tid_address(const linux_syscall_context& c)
    {
        const auto tidptr = get_linux_syscall_argument(c.emu, 0);

        if (c.proc.active_thread)
        {
            c.proc.active_thread->clear_child_tid = tidptr;
            write_linux_syscall_result(c, c.proc.active_thread->tid);
        }
        else
        {
            write_linux_syscall_result(c, c.proc.pid);
        }
    }

    void sys_set_robust_list(const linux_syscall_context& c)
    {
        const auto head = get_linux_syscall_argument(c.emu, 0);
        // len is arg 1, we don't validate it

        if (c.proc.active_thread)
        {
            c.proc.active_thread->robust_list_head = head;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_getrandom(const linux_syscall_context& c)
    {
        const auto buf_addr = get_linux_syscall_argument(c.emu, 0);
        const auto buflen = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));
        // flags is arg 2

        // Fill with deterministic pseudo-random data
        std::vector<uint8_t> buffer(buflen);
        for (size_t i = 0; i < buflen; ++i)
        {
            buffer.at(i) = static_cast<uint8_t>((i * 131 + 0x5A + buflen) & 0xFF);
        }

        c.emu.write_memory(buf_addr, buffer.data(), buflen);
        write_linux_syscall_result(c, static_cast<int64_t>(buflen));
    }

    void sys_rt_sigaction(const linux_syscall_context& c)
    {
        const auto signum = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto act_addr = get_linux_syscall_argument(c.emu, 1);
        const auto oldact_addr = get_linux_syscall_argument(c.emu, 2);
        const auto sigsetsize = get_linux_syscall_argument(c.emu, 3);

        if (sigsetsize != sizeof(uint64_t) || !posix_is_valid_signal(LINUX_SIGNAL_NUMBERS, signum) ||
            (act_addr != 0 && !posix_is_catchable_signal(LINUX_SIGNAL_NUMBERS, signum)))
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        linux_kernel_sigaction new_action{};
        if (act_addr != 0 && !c.emu.try_read_memory(act_addr, &new_action, sizeof(new_action)))
        {
            write_linux_syscall_result(c, -LINUX_EFAULT);
            return;
        }

        auto& action = c.emu_ref.signals.actions.at(static_cast<size_t>(signum));
        if (oldact_addr != 0 && !c.emu.try_write_memory(oldact_addr, &action, sizeof(action)))
        {
            write_linux_syscall_result(c, -LINUX_EFAULT);
            return;
        }

        if (act_addr != 0)
        {
            new_action.linux_sa_mask &= ~posix_unblockable_signals(LINUX_SIGNAL_NUMBERS);
            action = new_action;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_rt_sigprocmask(const linux_syscall_context& c)
    {
        const auto how = get_linux_syscall_argument(c.emu, 0);
        const auto set_addr = get_linux_syscall_argument(c.emu, 1);
        const auto oldset_addr = get_linux_syscall_argument(c.emu, 2);
        const auto sigsetsize = get_linux_syscall_argument(c.emu, 3);

        auto* thread = c.proc.active_thread;
        if (sigsetsize != sizeof(uint64_t) || !thread)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        const auto old_mask = thread->signal_mask;
        if (set_addr != 0)
        {
            uint64_t set{};
            if (!c.emu.try_read_memory(set_addr, &set, sizeof(set)))
            {
                write_linux_syscall_result(c, -LINUX_EFAULT);
                return;
            }

            const auto operation = mask_operation(how);
            if (!operation)
            {
                write_linux_syscall_result(c, -LINUX_EINVAL);
                return;
            }

            thread->signal_mask = posix_updated_signal_mask(old_mask, *operation, set, LINUX_SIGNAL_NUMBERS);
        }

        if (oldset_addr != 0 && !c.emu.try_write_memory(oldset_addr, &old_mask, sizeof(old_mask)))
        {
            write_linux_syscall_result(c, -LINUX_EFAULT);
            return;
        }

        write_linux_syscall_result(c, 0);
    }

    // The previous stack is reported after the new one is in place, as the kernel copies it out last.
    void sys_sigaltstack(const linux_syscall_context& c)
    {
        const auto new_stack_addr = get_linux_syscall_argument(c.emu, 0);
        const auto old_stack_addr = get_linux_syscall_argument(c.emu, 1);

        auto* thread = c.proc.active_thread;
        if (!thread)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        auto& stack = thread->signal_stack;
        const auto old_stack = to_linux_stack(stack);
        if (new_stack_addr != 0)
        {
            linux_stack requested{};
            if (!c.emu.try_read_memory(new_stack_addr, &requested, sizeof(requested)))
            {
                write_linux_syscall_result(c, -LINUX_EFAULT);
                return;
            }

            const auto result =
                posix_set_signal_stack(stack, {.base = requested.sp, .size = requested.size, .flags = requested.flags}, SIGNAL_STACK_RULES);
            if (std::holds_alternative<posix_errno>(result))
            {
                write_linux_syscall_result(c, result);
                return;
            }
        }

        if (old_stack_addr != 0 && !c.emu.try_write_memory(old_stack_addr, &old_stack, sizeof(old_stack)))
        {
            write_linux_syscall_result(c, -LINUX_EFAULT);
            return;
        }

        write_linux_syscall_result(c, 0);
    }

    // --- Phase 4b: Additional process syscalls ---

    void sys_sched_yield(const linux_syscall_context& c)
    {
        // No-op in single-threaded emulation
        write_linux_syscall_result(c, 0);
    }

    void sys_sched_getaffinity(const linux_syscall_context& c)
    {
        // pid is arg 0 (ignored — always return for self)
        const auto cpusetsize = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));
        const auto mask_addr = get_linux_syscall_argument(c.emu, 2);

        if (cpusetsize == 0 || mask_addr == 0)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        // Emulate a single CPU (bit 0 set)
        const auto write_size = std::min(cpusetsize, static_cast<size_t>(128));
        std::vector<uint8_t> mask(write_size, 0);
        mask.front() = 1; // CPU 0

        c.emu.write_memory(mask_addr, mask.data(), write_size);
        write_linux_syscall_result(c, static_cast<int64_t>(write_size));
    }

    void sys_sched_getscheduler(const linux_syscall_context& c)
    {
        const auto pid = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        if (pid != 0 && static_cast<uint32_t>(pid) != c.proc.pid)
        {
            write_linux_syscall_result(c, -LINUX_ESRCH);
            return;
        }

        // SCHED_OTHER
        write_linux_syscall_result(c, 0);
    }

    void sys_sched_getparam(const linux_syscall_context& c)
    {
        const auto pid = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto param_addr = get_linux_syscall_argument(c.emu, 1);

        if (param_addr == 0)
        {
            write_linux_syscall_result(c, -LINUX_EFAULT);
            return;
        }

        if (pid != 0 && static_cast<uint32_t>(pid) != c.proc.pid)
        {
            write_linux_syscall_result(c, -LINUX_ESRCH);
            return;
        }

        struct linux_sched_param
        {
            int32_t sched_priority;
        };

        linux_sched_param param{};
        param.sched_priority = 0;
        c.emu.write_memory(param_addr, &param, sizeof(param));

        write_linux_syscall_result(c, 0);
    }

    void sys_getpriority(const linux_syscall_context& c)
    {
        const auto which = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto who = static_cast<int>(get_linux_syscall_argument(c.emu, 1));

        constexpr int LINUX_PRIO_PROCESS = 0;
        constexpr int LINUX_PRIO_PGRP = 1;
        constexpr int LINUX_PRIO_USER = 2;

        if (which != LINUX_PRIO_PROCESS && which != LINUX_PRIO_PGRP && which != LINUX_PRIO_USER)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        if (which == LINUX_PRIO_PROCESS && who != 0 && static_cast<uint32_t>(who) != c.proc.pid)
        {
            write_linux_syscall_result(c, -LINUX_ESRCH);
            return;
        }

        // Linux nice range is [-20, 19]. Return default priority 0.
        write_linux_syscall_result(c, 0);
    }

    void sys_clock_getres(const linux_syscall_context& c)
    {
        // clock_id is arg 0 (ignored — all clocks report 1ns resolution)
        const auto res_addr = get_linux_syscall_argument(c.emu, 1);

        if (res_addr != 0)
        {
            struct linux_timespec_res
            {
                int64_t tv_sec;
                int64_t tv_nsec;
            };

            linux_timespec_res ts{};
            ts.tv_sec = 0;
            ts.tv_nsec = 1; // 1 nanosecond resolution

            c.emu.write_memory(res_addr, &ts, sizeof(ts));
        }

        write_linux_syscall_result(c, 0);
    }

    // --- Phase 4c: Signal and process management syscalls ---

    void sys_rt_sigreturn(const linux_syscall_context& c)
    {
        // Restore context from the rt_sigframe on the stack.
        // This is called when the signal handler returns via the sigreturn trampoline.
        signal_dispatcher::sigreturn(c.emu_ref);
        c.mark_instruction_pointer_finalized();
        // Don't write a syscall result — the restored context provides all register values.
    }

    void sys_kill(const linux_syscall_context& c)
    {
        const auto pid = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto sig = static_cast<int>(get_linux_syscall_argument(c.emu, 1));

        const auto is_self = (pid >= 0 && static_cast<uint32_t>(pid) == c.proc.pid) || pid == 0 || pid == -1;

        if (!is_valid_signal(sig))
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        // If sending signal 0, it's just a process existence check
        if (sig == 0)
        {
            if (is_self)
            {
                write_linux_syscall_result(c, 0);
            }
            else
            {
                write_linux_syscall_result(c, -LINUX_ESRCH);
            }
            return;
        }

        // Sending to ourselves
        if (is_self)
        {
            deliver_signal_after_success(c, sig);
            return;
        }

        // Sending to another process — we only emulate one process
        write_linux_syscall_result(c, -LINUX_ESRCH);
    }

    void sys_tgkill(const linux_syscall_context& c)
    {
        // tgkill(int tgid, int tid, int sig)
        const auto tgid = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto tid = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto sig = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        (void)tgid;

        const auto tid_exists =
            (tid >= 0 && static_cast<uint32_t>(tid) == c.proc.pid) || (tid >= 0 && c.proc.threads.contains(static_cast<uint32_t>(tid)));

        if (!is_valid_signal(sig))
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        // Signal 0 is existence check
        if (sig == 0)
        {
            if (tid_exists)
            {
                write_linux_syscall_result(c, 0);
            }
            else
            {
                write_linux_syscall_result(c, -LINUX_ESRCH);
            }
            return;
        }

        if (!tid_exists)
        {
            write_linux_syscall_result(c, -LINUX_ESRCH);
            return;
        }

        deliver_signal_after_success(c, sig);
    }

    void sys_wait4(const linux_syscall_context& c)
    {
        // wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage)
        // We don't support fork, so there are no child processes to wait for
        (void)c;
        write_linux_syscall_result(c, -LINUX_ECHILD);
    }

    void sys_execve(const linux_syscall_context& c)
    {
        // execve replaces the current process image. This is complex to support
        // in an emulator — it would require re-loading a new ELF binary.
        // For now, return ENOSYS.
        const auto filename_addr = get_linux_syscall_argument(c.emu, 0);
        const auto filename = read_string<char>(c.emu, filename_addr);

        c.emu_ref.log.warn("execve(\"%s\"): not yet implemented\n", filename.c_str());
        write_linux_syscall_result(c, -LINUX_ENOSYS);
    }

    void sys_prctl(const linux_syscall_context& c)
    {
        const auto option = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        constexpr int PR_SET_NAME = 15;
        constexpr int PR_GET_NAME = 16;
        constexpr int PR_SET_NO_NEW_PRIVS = 38;
        constexpr int PR_SET_PDEATHSIG = 1;
        constexpr int PR_SET_VMA = 0x53564d41;

        switch (option)
        {
        case PR_SET_NAME:
            // Set thread name — stub (we could store it)
            write_linux_syscall_result(c, 0);
            break;

        case PR_GET_NAME: {
            // Return "emulated" as the thread name
            const auto addr = get_linux_syscall_argument(c.emu, 1);
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
            const char name[] = "emulated";
            c.emu.write_memory(addr, name, sizeof(name));
            write_linux_syscall_result(c, 0);
            break;
        }

        case PR_SET_NO_NEW_PRIVS:
            // Accept and ignore
            write_linux_syscall_result(c, 0);
            break;

        case PR_SET_PDEATHSIG:
            // Accept and ignore
            write_linux_syscall_result(c, 0);
            break;

        case PR_SET_VMA:
            // Used by Android/musl to name VMA regions — accept and ignore
            write_linux_syscall_result(c, 0);
            break;

        default:
            c.emu_ref.log.warn("prctl: unsupported option %d\n", option);
            write_linux_syscall_result(c, -LINUX_EINVAL);
            break;
        }
    }

    void sys_getrlimit(const linux_syscall_context& c)
    {
        // getrlimit(int resource, struct rlimit *rlim)
        // const auto resource = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto rlim_addr = get_linux_syscall_argument(c.emu, 1);

        // Return generous default limits
        struct linux_rlimit
        {
            uint64_t rlim_cur;
            uint64_t rlim_max;
        };

        linux_rlimit rl{};
        rl.rlim_cur = 0x7FFFFFFFFFFFFFFF; // RLIM_INFINITY
        rl.rlim_max = 0x7FFFFFFFFFFFFFFF;

        c.emu.write_memory(rlim_addr, &rl, sizeof(rl));
        write_linux_syscall_result(c, 0);
    }

    void sys_getrusage(const linux_syscall_context& c)
    {
        // getrusage(int who, struct rusage *usage)
        // const auto who = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto usage_addr = get_linux_syscall_argument(c.emu, 1);

        // Return zeroed rusage (no resource usage to report)
        constexpr size_t RUSAGE_SIZE = 144; // sizeof(struct rusage) on x86-64
        std::vector<uint8_t> zeros(RUSAGE_SIZE, 0);
        c.emu.write_memory(usage_addr, zeros.data(), RUSAGE_SIZE);
        write_linux_syscall_result(c, 0);
    }

    void sys_sysinfo(const linux_syscall_context& c)
    {
        const auto info_addr = get_linux_syscall_argument(c.emu, 0);

        // Linux struct sysinfo
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
#pragma pack(push, 1)

        struct linux_sysinfo
        {
            int64_t uptime;
            uint64_t loads[3];
            uint64_t totalram;
            uint64_t freeram;
            uint64_t sharedram;
            uint64_t bufferram;
            uint64_t totalswap;
            uint64_t freeswap;
            uint16_t procs;
            uint16_t pad;
            uint32_t pad2;
            uint64_t totalhigh;
            uint64_t freehigh;
            uint32_t mem_unit;
            char padding[4]; // padding to 112 bytes
        };

#pragma pack(pop)
        // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

        linux_sysinfo si{};
        si.uptime = 3600;
        si.totalram = 4ULL * 1024 * 1024 * 1024; // 4 GB
        si.freeram = 2ULL * 1024 * 1024 * 1024;  // 2 GB
        si.procs = 1;
        si.mem_unit = 1;

        c.emu.write_memory(info_addr, &si, sizeof(si));
        write_linux_syscall_result(c, 0);
    }

    void sys_umask(const linux_syscall_context& c)
    {
        // umask(mode_t mask) — return old mask, store new one
        // We don't track this; just return 0022 (default)
        (void)get_linux_syscall_argument(c.emu, 0);
        write_linux_syscall_result(c, 0022);
    }

    void sys_getpgrp(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, c.proc.pid);
    }

    void sys_getpgid(const linux_syscall_context& c)
    {
        // getpgid(pid_t pid) — return process group id
        (void)get_linux_syscall_argument(c.emu, 0);
        write_linux_syscall_result(c, c.proc.pid);
    }

    void sys_setpgid(const linux_syscall_context& c)
    {
        // setpgid(pid_t pid, pid_t pgid) — stub
        (void)c;
        write_linux_syscall_result(c, 0);
    }

    void sys_getsid(const linux_syscall_context& c)
    {
        (void)get_linux_syscall_argument(c.emu, 0);
        write_linux_syscall_result(c, c.proc.pid);
    }

    void sys_setsid(const linux_syscall_context& c)
    {
        (void)c;
        write_linux_syscall_result(c, c.proc.pid);
    }

} // namespace sogen

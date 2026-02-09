#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <cstring>

using namespace linux_errno;

namespace
{
    constexpr int ARCH_SET_GS = 0x1001;
    constexpr int ARCH_SET_FS = 0x1002;
    constexpr int ARCH_GET_FS = 0x1003;
    constexpr int ARCH_GET_GS = 0x1004;

    // Linux utsname structure
    struct linux_utsname
    {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    };

    static_assert(sizeof(linux_utsname) == 390);

    void copy_utsname_field(char* dest, const char* src, size_t max_len)
    {
        memset(dest, 0, max_len);
        const auto len = strlen(src);
        memcpy(dest, src, std::min(len, max_len - 1));
    }
}

void sys_exit(const linux_syscall_context& c)
{
    const auto status = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    if (c.proc.active_thread)
    {
        c.proc.active_thread->terminated = true;
        c.proc.active_thread->exit_code = status;
    }

    c.proc.exit_status = status;
    c.emu_ref.stop();
}

void sys_exit_group(const linux_syscall_context& c)
{
    const auto status = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    c.proc.exit_status = status;
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
        buffer[i] = static_cast<uint8_t>((i * 131 + 0x5A + buflen) & 0xFF);
    }

    c.emu.write_memory(buf_addr, buffer.data(), buflen);
    write_linux_syscall_result(c, static_cast<int64_t>(buflen));
}

void sys_rt_sigaction(const linux_syscall_context& c)
{
    const auto signum = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto act_addr = get_linux_syscall_argument(c.emu, 1);
    const auto oldact_addr = get_linux_syscall_argument(c.emu, 2);
    // sigsetsize is arg 3 (should be 8)

    if (signum < 1 || signum >= 64)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    // Return old action if requested
    if (oldact_addr != 0)
    {
        const auto& old_action = c.emu_ref.signals.actions[signum];
        c.emu.write_memory(oldact_addr, &old_action, sizeof(old_action));
    }

    // Store new action if provided
    if (act_addr != 0)
    {
        linux_kernel_sigaction new_action{};
        c.emu.read_memory(act_addr, &new_action, sizeof(new_action));
        c.emu_ref.signals.actions[signum] = new_action;
    }

    write_linux_syscall_result(c, 0);
}

void sys_rt_sigprocmask(const linux_syscall_context& c)
{
    const auto how = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto set_addr = get_linux_syscall_argument(c.emu, 1);
    const auto oldset_addr = get_linux_syscall_argument(c.emu, 2);
    // sigsetsize is arg 3

    (void)how;
    (void)set_addr;

    // If oldset is requested, write the current mask
    if (oldset_addr != 0 && c.proc.active_thread)
    {
        const auto mask = c.proc.active_thread->signal_mask;
        c.emu.write_memory(oldset_addr, &mask, sizeof(mask));
    }

    write_linux_syscall_result(c, 0);
}

void sys_sigaltstack(const linux_syscall_context& c)
{
    // Stub: pretend we set the alternate signal stack
    (void)c;
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
    mask[0] = 1; // CPU 0

    c.emu.write_memory(mask_addr, mask.data(), write_size);
    write_linux_syscall_result(c, static_cast<int64_t>(write_size));
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
    c.emu_ref.signals.sigreturn(c.emu_ref);
    // Don't write a syscall result — the restored context provides all register values.
    // Note: sigreturn() adjusts RIP by -2 to compensate for the backend advancing
    // past the syscall instruction after this hook returns.
}

void sys_kill(const linux_syscall_context& c)
{
    const auto pid = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto sig = static_cast<int>(get_linux_syscall_argument(c.emu, 1));

    const auto is_self = (pid >= 0 && static_cast<uint32_t>(pid) == c.proc.pid) || pid == 0 || pid == -1;

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
        constexpr int LINUX_SIGKILL = 9;
        constexpr int LINUX_SIGTERM = 15;
        constexpr int LINUX_SIGABRT = 6;

        if (sig == LINUX_SIGKILL || sig == LINUX_SIGTERM || sig == LINUX_SIGABRT)
        {
            c.proc.exit_status = 128 + sig;
            c.emu_ref.stop();
            write_linux_syscall_result(c, 0);
            return;
        }

        // For other signals, just succeed (handler dispatch not yet implemented)
        write_linux_syscall_result(c, 0);
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
        (tid >= 0 && static_cast<uint32_t>(tid) == c.proc.pid) || (tid >= 0 && c.proc.threads.count(static_cast<uint32_t>(tid)) > 0);

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

    // Sending fatal signal to a thread
    constexpr int LINUX_SIGKILL = 9;
    constexpr int LINUX_SIGTERM = 15;
    constexpr int LINUX_SIGABRT = 6;

    if (sig == LINUX_SIGKILL || sig == LINUX_SIGTERM || sig == LINUX_SIGABRT)
    {
        c.proc.exit_status = 128 + sig;
        c.emu_ref.stop();
        write_linux_syscall_result(c, 0);
        return;
    }

    // Other signals: succeed silently
    write_linux_syscall_result(c, 0);
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

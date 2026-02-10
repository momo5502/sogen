#include "std_include.hpp"
#include "linux_syscall_dispatcher.hpp"
#include "linux_emulator.hpp"

using namespace linux_syscalls;
using namespace linux_errno;

// Forward declarations of all syscall handlers (defined in syscalls/*.cpp)
// Tier 1 — file
void sys_read(const linux_syscall_context& c);
void sys_write(const linux_syscall_context& c);
void sys_open(const linux_syscall_context& c);
void sys_close(const linux_syscall_context& c);
void sys_fstat(const linux_syscall_context& c);
void sys_lseek(const linux_syscall_context& c);
void sys_access(const linux_syscall_context& c);
void sys_openat(const linux_syscall_context& c);
void sys_fsync(const linux_syscall_context& c);
void sys_fdatasync(const linux_syscall_context& c);

// Tier 2 — file
void sys_stat(const linux_syscall_context& c);
void sys_lstat(const linux_syscall_context& c);
void sys_pread64(const linux_syscall_context& c);
void sys_writev(const linux_syscall_context& c);
void sys_dup(const linux_syscall_context& c);
void sys_dup2(const linux_syscall_context& c);
void sys_dup3(const linux_syscall_context& c);
void sys_fcntl(const linux_syscall_context& c);
void sys_ioctl(const linux_syscall_context& c);
void sys_getcwd(const linux_syscall_context& c);
void sys_readlink(const linux_syscall_context& c);
void sys_readlinkat(const linux_syscall_context& c);
void sys_getdents64(const linux_syscall_context& c);
void sys_newfstatat(const linux_syscall_context& c);

// Tier 1 — memory
void sys_mmap(const linux_syscall_context& c);
void sys_mprotect(const linux_syscall_context& c);
void sys_munmap(const linux_syscall_context& c);
void sys_brk(const linux_syscall_context& c);

// Tier 1 — process
void sys_exit(const linux_syscall_context& c);
void sys_exit_group(const linux_syscall_context& c);
void sys_uname(const linux_syscall_context& c);
void sys_arch_prctl(const linux_syscall_context& c);
void sys_prlimit64(const linux_syscall_context& c);
void sys_rseq(const linux_syscall_context& c);
void sys_getpid(const linux_syscall_context& c);
void sys_getppid(const linux_syscall_context& c);
void sys_getuid(const linux_syscall_context& c);
void sys_getgid(const linux_syscall_context& c);
void sys_geteuid(const linux_syscall_context& c);
void sys_getegid(const linux_syscall_context& c);
void sys_gettid(const linux_syscall_context& c);
void sys_set_tid_address(const linux_syscall_context& c);
void sys_set_robust_list(const linux_syscall_context& c);
void sys_getrandom(const linux_syscall_context& c);
void sys_rt_sigaction(const linux_syscall_context& c);
void sys_rt_sigprocmask(const linux_syscall_context& c);
void sys_sigaltstack(const linux_syscall_context& c);

// Tier 2 — memory
void sys_mremap(const linux_syscall_context& c);
void sys_madvise(const linux_syscall_context& c);

// Tier 2 — process
void sys_sched_yield(const linux_syscall_context& c);
void sys_sched_getaffinity(const linux_syscall_context& c);
void sys_sched_getscheduler(const linux_syscall_context& c);
void sys_sched_getparam(const linux_syscall_context& c);
void sys_getpriority(const linux_syscall_context& c);
void sys_clock_getres(const linux_syscall_context& c);

// Tier 1 — time
void sys_clock_gettime(const linux_syscall_context& c);
void sys_gettimeofday(const linux_syscall_context& c);
void sys_time(const linux_syscall_context& c);

// Tier 2 — time
void sys_nanosleep(const linux_syscall_context& c);
void sys_clock_nanosleep(const linux_syscall_context& c);

// Tier 2 — I/O
void sys_pipe(const linux_syscall_context& c);
void sys_pipe2(const linux_syscall_context& c);
void sys_eventfd(const linux_syscall_context& c);
void sys_eventfd2(const linux_syscall_context& c);

// Tier 2 — threading
void sys_clone(const linux_syscall_context& c);
void sys_futex(const linux_syscall_context& c);

// Tier 3 — file operations
void sys_rename(const linux_syscall_context& c);
void sys_renameat2(const linux_syscall_context& c);
void sys_unlink(const linux_syscall_context& c);
void sys_unlinkat(const linux_syscall_context& c);
void sys_mkdir(const linux_syscall_context& c);
void sys_mkdirat(const linux_syscall_context& c);
void sys_rmdir(const linux_syscall_context& c);
void sys_symlink(const linux_syscall_context& c);
void sys_symlinkat(const linux_syscall_context& c);
void sys_chmod(const linux_syscall_context& c);
void sys_fchmod(const linux_syscall_context& c);
void sys_fchmodat(const linux_syscall_context& c);
void sys_chown(const linux_syscall_context& c);
void sys_fchown(const linux_syscall_context& c);
void sys_fchownat(const linux_syscall_context& c);
void sys_truncate(const linux_syscall_context& c);
void sys_ftruncate(const linux_syscall_context& c);
void sys_pwrite64(const linux_syscall_context& c);
void sys_readv(const linux_syscall_context& c);
void sys_faccessat(const linux_syscall_context& c);
void sys_statfs(const linux_syscall_context& c);
void sys_fstatfs(const linux_syscall_context& c);

// Tier 3 — signal/process
void sys_rt_sigreturn(const linux_syscall_context& c);
void sys_kill(const linux_syscall_context& c);
void sys_tgkill(const linux_syscall_context& c);
void sys_wait4(const linux_syscall_context& c);
void sys_execve(const linux_syscall_context& c);
void sys_prctl(const linux_syscall_context& c);
void sys_getrlimit(const linux_syscall_context& c);
void sys_getrusage(const linux_syscall_context& c);
void sys_sysinfo(const linux_syscall_context& c);
void sys_umask(const linux_syscall_context& c);
void sys_getpgrp(const linux_syscall_context& c);
void sys_getpgid(const linux_syscall_context& c);
void sys_setpgid(const linux_syscall_context& c);
void sys_getsid(const linux_syscall_context& c);
void sys_setsid(const linux_syscall_context& c);

// Tier 3 — socket
void sys_socket(const linux_syscall_context& c);
void sys_connect(const linux_syscall_context& c);
void sys_accept(const linux_syscall_context& c);
void sys_accept4(const linux_syscall_context& c);
void sys_sendto(const linux_syscall_context& c);
void sys_recvfrom(const linux_syscall_context& c);
void sys_sendmsg(const linux_syscall_context& c);
void sys_recvmsg(const linux_syscall_context& c);
void sys_shutdown(const linux_syscall_context& c);
void sys_bind(const linux_syscall_context& c);
void sys_listen(const linux_syscall_context& c);
void sys_setsockopt(const linux_syscall_context& c);
void sys_getsockopt(const linux_syscall_context& c);
void sys_socketpair(const linux_syscall_context& c);
void sys_getsockname(const linux_syscall_context& c);
void sys_getpeername(const linux_syscall_context& c);

// Tier 3 — I/O multiplexing
void sys_epoll_create1(const linux_syscall_context& c);
void sys_epoll_ctl(const linux_syscall_context& c);
void sys_epoll_wait(const linux_syscall_context& c);
void sys_epoll_pwait(const linux_syscall_context& c);
void sys_poll(const linux_syscall_context& c);
void sys_ppoll(const linux_syscall_context& c);
void sys_select(const linux_syscall_context& c);
void sys_pselect6(const linux_syscall_context& c);

void linux_syscall_dispatcher::dispatch(linux_emulator& emu_ref)
{
    auto& e = emu_ref.emu();
    const auto syscall_id = e.reg(x86_register::rax);

    if (syscall_id >= this->handlers_.size())
    {
        emu_ref.log.warn("Unimplemented syscall: %llu\n", static_cast<unsigned long long>(syscall_id));
        e.reg(x86_register::rax, static_cast<uint64_t>(-LINUX_ENOSYS));
        return;
    }

    const auto& entry = this->handlers_[syscall_id];
    if (!entry.handler)
    {
        emu_ref.log.warn("Unimplemented syscall: %llu (%s)\n", static_cast<unsigned long long>(syscall_id),
                         entry.name.empty() ? "unknown" : entry.name.c_str());
        e.reg(x86_register::rax, static_cast<uint64_t>(-LINUX_ENOSYS));
        return;
    }

    linux_syscall_context ctx{emu_ref, e, emu_ref.process};
    entry.handler(ctx);
}

void linux_syscall_dispatcher::add_handlers()
{
    // File syscalls (Tier 1)
    this->handlers_[LINUX_SYS_read] = {sys_read, "read"};
    this->handlers_[LINUX_SYS_write] = {sys_write, "write"};
    this->handlers_[LINUX_SYS_open] = {sys_open, "open"};
    this->handlers_[LINUX_SYS_close] = {sys_close, "close"};
    this->handlers_[LINUX_SYS_fstat] = {sys_fstat, "fstat"};
    this->handlers_[LINUX_SYS_lseek] = {sys_lseek, "lseek"};
    this->handlers_[LINUX_SYS_access] = {sys_access, "access"};
    this->handlers_[LINUX_SYS_openat] = {sys_openat, "openat"};
    this->handlers_[LINUX_SYS_fsync] = {sys_fsync, "fsync"};
    this->handlers_[LINUX_SYS_fdatasync] = {sys_fdatasync, "fdatasync"};

    // File syscalls (Tier 2)
    this->handlers_[LINUX_SYS_stat] = {sys_stat, "stat"};
    this->handlers_[LINUX_SYS_lstat] = {sys_lstat, "lstat"};
    this->handlers_[LINUX_SYS_pread64] = {sys_pread64, "pread64"};
    this->handlers_[LINUX_SYS_writev] = {sys_writev, "writev"};
    this->handlers_[LINUX_SYS_dup] = {sys_dup, "dup"};
    this->handlers_[LINUX_SYS_dup2] = {sys_dup2, "dup2"};
    this->handlers_[LINUX_SYS_dup3] = {sys_dup3, "dup3"};
    this->handlers_[LINUX_SYS_fcntl] = {sys_fcntl, "fcntl"};
    this->handlers_[LINUX_SYS_ioctl] = {sys_ioctl, "ioctl"};
    this->handlers_[LINUX_SYS_getcwd] = {sys_getcwd, "getcwd"};
    this->handlers_[LINUX_SYS_readlink] = {sys_readlink, "readlink"};
    this->handlers_[LINUX_SYS_readlinkat] = {sys_readlinkat, "readlinkat"};
    this->handlers_[LINUX_SYS_getdents64] = {sys_getdents64, "getdents64"};
    this->handlers_[LINUX_SYS_newfstatat] = {sys_newfstatat, "newfstatat"};

    // Memory syscalls
    this->handlers_[LINUX_SYS_mmap] = {sys_mmap, "mmap"};
    this->handlers_[LINUX_SYS_mprotect] = {sys_mprotect, "mprotect"};
    this->handlers_[LINUX_SYS_munmap] = {sys_munmap, "munmap"};
    this->handlers_[LINUX_SYS_brk] = {sys_brk, "brk"};

    // Process syscalls
    this->handlers_[LINUX_SYS_exit] = {sys_exit, "exit"};
    this->handlers_[LINUX_SYS_exit_group] = {sys_exit_group, "exit_group"};
    this->handlers_[LINUX_SYS_uname] = {sys_uname, "uname"};
    this->handlers_[LINUX_SYS_arch_prctl] = {sys_arch_prctl, "arch_prctl"};
    this->handlers_[LINUX_SYS_prlimit64] = {sys_prlimit64, "prlimit64"};
    this->handlers_[LINUX_SYS_rseq] = {sys_rseq, "rseq"};
    this->handlers_[LINUX_SYS_getpid] = {sys_getpid, "getpid"};
    this->handlers_[LINUX_SYS_getppid] = {sys_getppid, "getppid"};
    this->handlers_[LINUX_SYS_getuid] = {sys_getuid, "getuid"};
    this->handlers_[LINUX_SYS_getgid] = {sys_getgid, "getgid"};
    this->handlers_[LINUX_SYS_geteuid] = {sys_geteuid, "geteuid"};
    this->handlers_[LINUX_SYS_getegid] = {sys_getegid, "getegid"};
    this->handlers_[LINUX_SYS_gettid] = {sys_gettid, "gettid"};
    this->handlers_[LINUX_SYS_set_tid_address] = {sys_set_tid_address, "set_tid_address"};
    this->handlers_[LINUX_SYS_set_robust_list] = {sys_set_robust_list, "set_robust_list"};
    this->handlers_[LINUX_SYS_getrandom] = {sys_getrandom, "getrandom"};
    this->handlers_[LINUX_SYS_rt_sigaction] = {sys_rt_sigaction, "rt_sigaction"};
    this->handlers_[LINUX_SYS_rt_sigprocmask] = {sys_rt_sigprocmask, "rt_sigprocmask"};
    this->handlers_[LINUX_SYS_sigaltstack] = {sys_sigaltstack, "sigaltstack"};

    // Memory syscalls (Tier 2)
    this->handlers_[LINUX_SYS_mremap] = {sys_mremap, "mremap"};
    this->handlers_[LINUX_SYS_madvise] = {sys_madvise, "madvise"};

    // Process syscalls (Tier 2)
    this->handlers_[LINUX_SYS_sched_yield] = {sys_sched_yield, "sched_yield"};
    this->handlers_[LINUX_SYS_sched_getaffinity] = {sys_sched_getaffinity, "sched_getaffinity"};
    this->handlers_[LINUX_SYS_sched_getscheduler] = {sys_sched_getscheduler, "sched_getscheduler"};
    this->handlers_[LINUX_SYS_sched_getparam] = {sys_sched_getparam, "sched_getparam"};
    this->handlers_[LINUX_SYS_getpriority] = {sys_getpriority, "getpriority"};
    this->handlers_[LINUX_SYS_clock_getres] = {sys_clock_getres, "clock_getres"};

    // Time syscalls
    this->handlers_[LINUX_SYS_clock_gettime] = {sys_clock_gettime, "clock_gettime"};
    this->handlers_[LINUX_SYS_gettimeofday] = {sys_gettimeofday, "gettimeofday"};
    this->handlers_[LINUX_SYS_time] = {sys_time, "time"};
    this->handlers_[LINUX_SYS_nanosleep] = {sys_nanosleep, "nanosleep"};
    this->handlers_[LINUX_SYS_clock_nanosleep] = {sys_clock_nanosleep, "clock_nanosleep"};

    // I/O syscalls (Tier 2)
    this->handlers_[LINUX_SYS_pipe] = {sys_pipe, "pipe"};
    this->handlers_[LINUX_SYS_pipe2] = {sys_pipe2, "pipe2"};
    this->handlers_[LINUX_SYS_eventfd] = {sys_eventfd, "eventfd"};
    this->handlers_[LINUX_SYS_eventfd2] = {sys_eventfd2, "eventfd2"};

    // Threading syscalls (Tier 2)
    this->handlers_[LINUX_SYS_clone] = {sys_clone, "clone"};
    this->handlers_[LINUX_SYS_futex] = {sys_futex, "futex"};

    // File syscalls (Tier 3)
    this->handlers_[LINUX_SYS_rename] = {sys_rename, "rename"};
    this->handlers_[LINUX_SYS_renameat2] = {sys_renameat2, "renameat2"};
    this->handlers_[LINUX_SYS_unlink] = {sys_unlink, "unlink"};
    this->handlers_[LINUX_SYS_unlinkat] = {sys_unlinkat, "unlinkat"};
    this->handlers_[LINUX_SYS_mkdir] = {sys_mkdir, "mkdir"};
    this->handlers_[LINUX_SYS_mkdirat] = {sys_mkdirat, "mkdirat"};
    this->handlers_[LINUX_SYS_rmdir] = {sys_rmdir, "rmdir"};
    this->handlers_[LINUX_SYS_symlink] = {sys_symlink, "symlink"};
    this->handlers_[LINUX_SYS_symlinkat] = {sys_symlinkat, "symlinkat"};
    this->handlers_[LINUX_SYS_chmod] = {sys_chmod, "chmod"};
    this->handlers_[LINUX_SYS_fchmod] = {sys_fchmod, "fchmod"};
    this->handlers_[LINUX_SYS_fchmodat] = {sys_fchmodat, "fchmodat"};
    this->handlers_[LINUX_SYS_chown] = {sys_chown, "chown"};
    this->handlers_[LINUX_SYS_fchown] = {sys_fchown, "fchown"};
    this->handlers_[LINUX_SYS_fchownat] = {sys_fchownat, "fchownat"};
    this->handlers_[LINUX_SYS_truncate] = {sys_truncate, "truncate"};
    this->handlers_[LINUX_SYS_ftruncate] = {sys_ftruncate, "ftruncate"};
    this->handlers_[LINUX_SYS_pwrite64] = {sys_pwrite64, "pwrite64"};
    this->handlers_[LINUX_SYS_readv] = {sys_readv, "readv"};
    this->handlers_[LINUX_SYS_faccessat] = {sys_faccessat, "faccessat"};
    this->handlers_[LINUX_SYS_statfs] = {sys_statfs, "statfs"};
    this->handlers_[LINUX_SYS_fstatfs] = {sys_fstatfs, "fstatfs"};

    // Signal/process syscalls (Tier 3)
    this->handlers_[LINUX_SYS_rt_sigreturn] = {sys_rt_sigreturn, "rt_sigreturn"};
    this->handlers_[LINUX_SYS_kill] = {sys_kill, "kill"};
    this->handlers_[LINUX_SYS_tgkill] = {sys_tgkill, "tgkill"};
    this->handlers_[LINUX_SYS_wait4] = {sys_wait4, "wait4"};
    this->handlers_[LINUX_SYS_execve] = {sys_execve, "execve"};
    this->handlers_[LINUX_SYS_prctl] = {sys_prctl, "prctl"};
    this->handlers_[LINUX_SYS_getrlimit] = {sys_getrlimit, "getrlimit"};
    this->handlers_[LINUX_SYS_getrusage] = {sys_getrusage, "getrusage"};
    this->handlers_[LINUX_SYS_sysinfo] = {sys_sysinfo, "sysinfo"};
    this->handlers_[LINUX_SYS_umask] = {sys_umask, "umask"};
    this->handlers_[LINUX_SYS_getpgrp] = {sys_getpgrp, "getpgrp"};
    this->handlers_[LINUX_SYS_getpgid] = {sys_getpgid, "getpgid"};
    this->handlers_[LINUX_SYS_setpgid] = {sys_setpgid, "setpgid"};
    this->handlers_[LINUX_SYS_getsid] = {sys_getsid, "getsid"};
    this->handlers_[LINUX_SYS_setsid] = {sys_setsid, "setsid"};

    // Socket syscalls (Tier 3)
    this->handlers_[LINUX_SYS_socket] = {sys_socket, "socket"};
    this->handlers_[LINUX_SYS_connect] = {sys_connect, "connect"};
    this->handlers_[LINUX_SYS_accept] = {sys_accept, "accept"};
    this->handlers_[LINUX_SYS_accept4] = {sys_accept4, "accept4"};
    this->handlers_[LINUX_SYS_sendto] = {sys_sendto, "sendto"};
    this->handlers_[LINUX_SYS_recvfrom] = {sys_recvfrom, "recvfrom"};
    this->handlers_[LINUX_SYS_sendmsg] = {sys_sendmsg, "sendmsg"};
    this->handlers_[LINUX_SYS_recvmsg] = {sys_recvmsg, "recvmsg"};
    this->handlers_[LINUX_SYS_shutdown] = {sys_shutdown, "shutdown"};
    this->handlers_[LINUX_SYS_bind] = {sys_bind, "bind"};
    this->handlers_[LINUX_SYS_listen] = {sys_listen, "listen"};
    this->handlers_[LINUX_SYS_setsockopt] = {sys_setsockopt, "setsockopt"};
    this->handlers_[LINUX_SYS_getsockopt] = {sys_getsockopt, "getsockopt"};
    this->handlers_[LINUX_SYS_socketpair] = {sys_socketpair, "socketpair"};
    this->handlers_[LINUX_SYS_getsockname] = {sys_getsockname, "getsockname"};
    this->handlers_[LINUX_SYS_getpeername] = {sys_getpeername, "getpeername"};

    // I/O multiplexing syscalls (Tier 3)
    this->handlers_[LINUX_SYS_epoll_create1] = {sys_epoll_create1, "epoll_create1"};
    this->handlers_[LINUX_SYS_epoll_ctl] = {sys_epoll_ctl, "epoll_ctl"};
    this->handlers_[LINUX_SYS_epoll_wait] = {sys_epoll_wait, "epoll_wait"};
    this->handlers_[LINUX_SYS_epoll_pwait] = {sys_epoll_pwait, "epoll_pwait"};
    this->handlers_[LINUX_SYS_poll] = {sys_poll, "poll"};
    this->handlers_[LINUX_SYS_ppoll] = {sys_ppoll, "ppoll"};
    this->handlers_[LINUX_SYS_select] = {sys_select, "select"};
    this->handlers_[LINUX_SYS_pselect6] = {sys_pselect6, "pselect6"};

    // Named but unimplemented syscalls (for better logging)
    this->handlers_[LINUX_SYS_fork].name = "fork";
    this->handlers_[LINUX_SYS_vfork].name = "vfork";
}

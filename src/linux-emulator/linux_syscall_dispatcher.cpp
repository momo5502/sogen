#include "std_include.hpp"
#include "linux_syscall_dispatcher.hpp"
#include "linux_emulator.hpp"

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

using namespace linux_syscalls;
using namespace linux_errno;
// NOLINTEND(google-build-using-namespace)

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
        emu_ref.log.warn("Unimplemented syscall: %" PRIu64 "\n", syscall_id);
        e.reg(x86_register::rax, static_cast<uint64_t>(-LINUX_ENOSYS));
        return;
    }

    const auto& entry = this->handlers_[static_cast<size_t>(syscall_id)];
    if (!entry.handler)
    {
        emu_ref.log.warn("Unimplemented syscall: %" PRIu64 " (%s)\n", syscall_id, entry.name.empty() ? "unknown" : entry.name.c_str());
        e.reg(x86_register::rax, static_cast<uint64_t>(-LINUX_ENOSYS));
        return;
    }

    linux_syscall_context ctx{.emu_ref = emu_ref, .emu = e, .proc = emu_ref.process};
    entry.handler(ctx);
}

void linux_syscall_dispatcher::add_handlers()
{
    // File syscalls (Tier 1)
    this->handlers_[LINUX_SYS_read] = {.handler = sys_read, .name = "read"};
    this->handlers_[LINUX_SYS_write] = {.handler = sys_write, .name = "write"};
    this->handlers_[LINUX_SYS_open] = {.handler = sys_open, .name = "open"};
    this->handlers_[LINUX_SYS_close] = {.handler = sys_close, .name = "close"};
    this->handlers_[LINUX_SYS_fstat] = {.handler = sys_fstat, .name = "fstat"};
    this->handlers_[LINUX_SYS_lseek] = {.handler = sys_lseek, .name = "lseek"};
    this->handlers_[LINUX_SYS_access] = {.handler = sys_access, .name = "access"};
    this->handlers_[LINUX_SYS_openat] = {.handler = sys_openat, .name = "openat"};
    this->handlers_[LINUX_SYS_fsync] = {.handler = sys_fsync, .name = "fsync"};
    this->handlers_[LINUX_SYS_fdatasync] = {.handler = sys_fdatasync, .name = "fdatasync"};

    // File syscalls (Tier 2)
    this->handlers_[LINUX_SYS_stat] = {.handler = sys_stat, .name = "stat"};
    this->handlers_[LINUX_SYS_lstat] = {.handler = sys_lstat, .name = "lstat"};
    this->handlers_[LINUX_SYS_pread64] = {.handler = sys_pread64, .name = "pread64"};
    this->handlers_[LINUX_SYS_writev] = {.handler = sys_writev, .name = "writev"};
    this->handlers_[LINUX_SYS_dup] = {.handler = sys_dup, .name = "dup"};
    this->handlers_[LINUX_SYS_dup2] = {.handler = sys_dup2, .name = "dup2"};
    this->handlers_[LINUX_SYS_dup3] = {.handler = sys_dup3, .name = "dup3"};
    this->handlers_[LINUX_SYS_fcntl] = {.handler = sys_fcntl, .name = "fcntl"};
    this->handlers_[LINUX_SYS_ioctl] = {.handler = sys_ioctl, .name = "ioctl"};
    this->handlers_[LINUX_SYS_getcwd] = {.handler = sys_getcwd, .name = "getcwd"};
    this->handlers_[LINUX_SYS_readlink] = {.handler = sys_readlink, .name = "readlink"};
    this->handlers_[LINUX_SYS_readlinkat] = {.handler = sys_readlinkat, .name = "readlinkat"};
    this->handlers_[LINUX_SYS_getdents64] = {.handler = sys_getdents64, .name = "getdents64"};
    this->handlers_[LINUX_SYS_newfstatat] = {.handler = sys_newfstatat, .name = "newfstatat"};

    // Memory syscalls
    this->handlers_[LINUX_SYS_mmap] = {.handler = sys_mmap, .name = "mmap"};
    this->handlers_[LINUX_SYS_mprotect] = {.handler = sys_mprotect, .name = "mprotect"};
    this->handlers_[LINUX_SYS_munmap] = {.handler = sys_munmap, .name = "munmap"};
    this->handlers_[LINUX_SYS_brk] = {.handler = sys_brk, .name = "brk"};

    // Process syscalls
    this->handlers_[LINUX_SYS_exit] = {.handler = sys_exit, .name = "exit"};
    this->handlers_[LINUX_SYS_exit_group] = {.handler = sys_exit_group, .name = "exit_group"};
    this->handlers_[LINUX_SYS_uname] = {.handler = sys_uname, .name = "uname"};
    this->handlers_[LINUX_SYS_arch_prctl] = {.handler = sys_arch_prctl, .name = "arch_prctl"};
    this->handlers_[LINUX_SYS_prlimit64] = {.handler = sys_prlimit64, .name = "prlimit64"};
    this->handlers_[LINUX_SYS_rseq] = {.handler = sys_rseq, .name = "rseq"};
    this->handlers_[LINUX_SYS_getpid] = {.handler = sys_getpid, .name = "getpid"};
    this->handlers_[LINUX_SYS_getppid] = {.handler = sys_getppid, .name = "getppid"};
    this->handlers_[LINUX_SYS_getuid] = {.handler = sys_getuid, .name = "getuid"};
    this->handlers_[LINUX_SYS_getgid] = {.handler = sys_getgid, .name = "getgid"};
    this->handlers_[LINUX_SYS_geteuid] = {.handler = sys_geteuid, .name = "geteuid"};
    this->handlers_[LINUX_SYS_getegid] = {.handler = sys_getegid, .name = "getegid"};
    this->handlers_[LINUX_SYS_gettid] = {.handler = sys_gettid, .name = "gettid"};
    this->handlers_[LINUX_SYS_set_tid_address] = {.handler = sys_set_tid_address, .name = "set_tid_address"};
    this->handlers_[LINUX_SYS_set_robust_list] = {.handler = sys_set_robust_list, .name = "set_robust_list"};
    this->handlers_[LINUX_SYS_getrandom] = {.handler = sys_getrandom, .name = "getrandom"};
    this->handlers_[LINUX_SYS_rt_sigaction] = {.handler = sys_rt_sigaction, .name = "rt_sigaction"};
    this->handlers_[LINUX_SYS_rt_sigprocmask] = {.handler = sys_rt_sigprocmask, .name = "rt_sigprocmask"};
    this->handlers_[LINUX_SYS_sigaltstack] = {.handler = sys_sigaltstack, .name = "sigaltstack"};

    // Memory syscalls (Tier 2)
    this->handlers_[LINUX_SYS_mremap] = {.handler = sys_mremap, .name = "mremap"};
    this->handlers_[LINUX_SYS_madvise] = {.handler = sys_madvise, .name = "madvise"};

    // Process syscalls (Tier 2)
    this->handlers_[LINUX_SYS_sched_yield] = {.handler = sys_sched_yield, .name = "sched_yield"};
    this->handlers_[LINUX_SYS_sched_getaffinity] = {.handler = sys_sched_getaffinity, .name = "sched_getaffinity"};
    this->handlers_[LINUX_SYS_sched_getscheduler] = {.handler = sys_sched_getscheduler, .name = "sched_getscheduler"};
    this->handlers_[LINUX_SYS_sched_getparam] = {.handler = sys_sched_getparam, .name = "sched_getparam"};
    this->handlers_[LINUX_SYS_getpriority] = {.handler = sys_getpriority, .name = "getpriority"};
    this->handlers_[LINUX_SYS_clock_getres] = {.handler = sys_clock_getres, .name = "clock_getres"};

    // Time syscalls
    this->handlers_[LINUX_SYS_clock_gettime] = {.handler = sys_clock_gettime, .name = "clock_gettime"};
    this->handlers_[LINUX_SYS_gettimeofday] = {.handler = sys_gettimeofday, .name = "gettimeofday"};
    this->handlers_[LINUX_SYS_time] = {.handler = sys_time, .name = "time"};
    this->handlers_[LINUX_SYS_nanosleep] = {.handler = sys_nanosleep, .name = "nanosleep"};
    this->handlers_[LINUX_SYS_clock_nanosleep] = {.handler = sys_clock_nanosleep, .name = "clock_nanosleep"};

    // I/O syscalls (Tier 2)
    this->handlers_[LINUX_SYS_pipe] = {.handler = sys_pipe, .name = "pipe"};
    this->handlers_[LINUX_SYS_pipe2] = {.handler = sys_pipe2, .name = "pipe2"};
    this->handlers_[LINUX_SYS_eventfd] = {.handler = sys_eventfd, .name = "eventfd"};
    this->handlers_[LINUX_SYS_eventfd2] = {.handler = sys_eventfd2, .name = "eventfd2"};

    // Threading syscalls (Tier 2)
    this->handlers_[LINUX_SYS_clone] = {.handler = sys_clone, .name = "clone"};
    this->handlers_[LINUX_SYS_futex] = {.handler = sys_futex, .name = "futex"};

    // File syscalls (Tier 3)
    this->handlers_[LINUX_SYS_rename] = {.handler = sys_rename, .name = "rename"};
    this->handlers_[LINUX_SYS_renameat2] = {.handler = sys_renameat2, .name = "renameat2"};
    this->handlers_[LINUX_SYS_unlink] = {.handler = sys_unlink, .name = "unlink"};
    this->handlers_[LINUX_SYS_unlinkat] = {.handler = sys_unlinkat, .name = "unlinkat"};
    this->handlers_[LINUX_SYS_mkdir] = {.handler = sys_mkdir, .name = "mkdir"};
    this->handlers_[LINUX_SYS_mkdirat] = {.handler = sys_mkdirat, .name = "mkdirat"};
    this->handlers_[LINUX_SYS_rmdir] = {.handler = sys_rmdir, .name = "rmdir"};
    this->handlers_[LINUX_SYS_symlink] = {.handler = sys_symlink, .name = "symlink"};
    this->handlers_[LINUX_SYS_symlinkat] = {.handler = sys_symlinkat, .name = "symlinkat"};
    this->handlers_[LINUX_SYS_chmod] = {.handler = sys_chmod, .name = "chmod"};
    this->handlers_[LINUX_SYS_fchmod] = {.handler = sys_fchmod, .name = "fchmod"};
    this->handlers_[LINUX_SYS_fchmodat] = {.handler = sys_fchmodat, .name = "fchmodat"};
    this->handlers_[LINUX_SYS_chown] = {.handler = sys_chown, .name = "chown"};
    this->handlers_[LINUX_SYS_fchown] = {.handler = sys_fchown, .name = "fchown"};
    this->handlers_[LINUX_SYS_fchownat] = {.handler = sys_fchownat, .name = "fchownat"};
    this->handlers_[LINUX_SYS_truncate] = {.handler = sys_truncate, .name = "truncate"};
    this->handlers_[LINUX_SYS_ftruncate] = {.handler = sys_ftruncate, .name = "ftruncate"};
    this->handlers_[LINUX_SYS_pwrite64] = {.handler = sys_pwrite64, .name = "pwrite64"};
    this->handlers_[LINUX_SYS_readv] = {.handler = sys_readv, .name = "readv"};
    this->handlers_[LINUX_SYS_faccessat] = {.handler = sys_faccessat, .name = "faccessat"};
    this->handlers_[LINUX_SYS_statfs] = {.handler = sys_statfs, .name = "statfs"};
    this->handlers_[LINUX_SYS_fstatfs] = {.handler = sys_fstatfs, .name = "fstatfs"};

    // Signal/process syscalls (Tier 3)
    this->handlers_[LINUX_SYS_rt_sigreturn] = {.handler = sys_rt_sigreturn, .name = "rt_sigreturn"};
    this->handlers_[LINUX_SYS_kill] = {.handler = sys_kill, .name = "kill"};
    this->handlers_[LINUX_SYS_tgkill] = {.handler = sys_tgkill, .name = "tgkill"};
    this->handlers_[LINUX_SYS_wait4] = {.handler = sys_wait4, .name = "wait4"};
    this->handlers_[LINUX_SYS_execve] = {.handler = sys_execve, .name = "execve"};
    this->handlers_[LINUX_SYS_prctl] = {.handler = sys_prctl, .name = "prctl"};
    this->handlers_[LINUX_SYS_getrlimit] = {.handler = sys_getrlimit, .name = "getrlimit"};
    this->handlers_[LINUX_SYS_getrusage] = {.handler = sys_getrusage, .name = "getrusage"};
    this->handlers_[LINUX_SYS_sysinfo] = {.handler = sys_sysinfo, .name = "sysinfo"};
    this->handlers_[LINUX_SYS_umask] = {.handler = sys_umask, .name = "umask"};
    this->handlers_[LINUX_SYS_getpgrp] = {.handler = sys_getpgrp, .name = "getpgrp"};
    this->handlers_[LINUX_SYS_getpgid] = {.handler = sys_getpgid, .name = "getpgid"};
    this->handlers_[LINUX_SYS_setpgid] = {.handler = sys_setpgid, .name = "setpgid"};
    this->handlers_[LINUX_SYS_getsid] = {.handler = sys_getsid, .name = "getsid"};
    this->handlers_[LINUX_SYS_setsid] = {.handler = sys_setsid, .name = "setsid"};

    // Socket syscalls (Tier 3)
    this->handlers_[LINUX_SYS_socket] = {.handler = sys_socket, .name = "socket"};
    this->handlers_[LINUX_SYS_connect] = {.handler = sys_connect, .name = "connect"};
    this->handlers_[LINUX_SYS_accept] = {.handler = sys_accept, .name = "accept"};
    this->handlers_[LINUX_SYS_accept4] = {.handler = sys_accept4, .name = "accept4"};
    this->handlers_[LINUX_SYS_sendto] = {.handler = sys_sendto, .name = "sendto"};
    this->handlers_[LINUX_SYS_recvfrom] = {.handler = sys_recvfrom, .name = "recvfrom"};
    this->handlers_[LINUX_SYS_sendmsg] = {.handler = sys_sendmsg, .name = "sendmsg"};
    this->handlers_[LINUX_SYS_recvmsg] = {.handler = sys_recvmsg, .name = "recvmsg"};
    this->handlers_[LINUX_SYS_shutdown] = {.handler = sys_shutdown, .name = "shutdown"};
    this->handlers_[LINUX_SYS_bind] = {.handler = sys_bind, .name = "bind"};
    this->handlers_[LINUX_SYS_listen] = {.handler = sys_listen, .name = "listen"};
    this->handlers_[LINUX_SYS_setsockopt] = {.handler = sys_setsockopt, .name = "setsockopt"};
    this->handlers_[LINUX_SYS_getsockopt] = {.handler = sys_getsockopt, .name = "getsockopt"};
    this->handlers_[LINUX_SYS_socketpair] = {.handler = sys_socketpair, .name = "socketpair"};
    this->handlers_[LINUX_SYS_getsockname] = {.handler = sys_getsockname, .name = "getsockname"};
    this->handlers_[LINUX_SYS_getpeername] = {.handler = sys_getpeername, .name = "getpeername"};

    // I/O multiplexing syscalls (Tier 3)
    this->handlers_[LINUX_SYS_epoll_create1] = {.handler = sys_epoll_create1, .name = "epoll_create1"};
    this->handlers_[LINUX_SYS_epoll_ctl] = {.handler = sys_epoll_ctl, .name = "epoll_ctl"};
    this->handlers_[LINUX_SYS_epoll_wait] = {.handler = sys_epoll_wait, .name = "epoll_wait"};
    this->handlers_[LINUX_SYS_epoll_pwait] = {.handler = sys_epoll_pwait, .name = "epoll_pwait"};
    this->handlers_[LINUX_SYS_poll] = {.handler = sys_poll, .name = "poll"};
    this->handlers_[LINUX_SYS_ppoll] = {.handler = sys_ppoll, .name = "ppoll"};
    this->handlers_[LINUX_SYS_select] = {.handler = sys_select, .name = "select"};
    this->handlers_[LINUX_SYS_pselect6] = {.handler = sys_pselect6, .name = "pselect6"};

    // Named but unimplemented syscalls (for better logging)
    this->handlers_[LINUX_SYS_fork].name = "fork";
    this->handlers_[LINUX_SYS_vfork].name = "vfork";
}

} // namespace sogen

#include "std_include.hpp"
#include "bsd_syscall_dispatcher.hpp"

#include "trace/bsd_syscall_table.hpp"

#include "trace/macos_trace_emitter.hpp"
#include "macos_emulator.hpp"
#include "gui/macos_native_dispatch.hpp"
#include "gui/macos_guest_call.hpp"

#include <array>
#include <cstdio>

#include <cerrno>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_syscalls;
    using namespace macos_errno;
    // NOLINTEND(google-build-using-namespace)

    void sys_exit(const macos_syscall_context& c);
    void sys_abort_with_payload(const macos_syscall_context& c);
    void sys_sigprocmask(const macos_syscall_context& c);
    void sys_pread(const macos_syscall_context& c);
    void sys_pwrite(const macos_syscall_context& c);
    void sys_fsync(const macos_syscall_context& c);
    void sys_fchown(const macos_syscall_context& c);
    void sys_fchmod(const macos_syscall_context& c);
    void sys_ftruncate(const macos_syscall_context& c);
    void sys_futimes(const macos_syscall_context& c);
    void sys_bsdthread_ctl(const macos_syscall_context& c);
    void sys_pthread_kill(const macos_syscall_context& c);
    void sys_terminate_with_payload(const macos_syscall_context& c);
    void sys_getpid(const macos_syscall_context& c);
    void sys_getppid(const macos_syscall_context& c);
    void sys_getuid(const macos_syscall_context& c);
    void sys_geteuid(const macos_syscall_context& c);
    void sys_getgid(const macos_syscall_context& c);
    void sys_getegid(const macos_syscall_context& c);
    void sys_gethostuuid(const macos_syscall_context& c);
    void sys_iopolicysys(const macos_syscall_context& c);
    void sys_issetugid(const macos_syscall_context& c);
    void sys_getentropy(const macos_syscall_context& c);
    void sys_thread_selfid(const macos_syscall_context& c);
    void sys_csops(const macos_syscall_context& c);
    void sys_csops_audittoken(const macos_syscall_context& c);
    void sys_proc_info(const macos_syscall_context& c);
    void sys_mac_syscall(const macos_syscall_context& c);
    void sys_bsdthread_register(const macos_syscall_context& c);
    void sys_workq_open(const macos_syscall_context& c);
    void sys_workq_kernreturn(const macos_syscall_context& c);
    void sys_kqueue(const macos_syscall_context& c);
    void sys_kevent_qos(const macos_syscall_context& c);
    void sys_kevent_id(const macos_syscall_context& c);
    void sys_bsdthread_create(const macos_syscall_context& c);
    void sys_bsdthread_terminate(const macos_syscall_context& c);
    void sys_disable_threadsignal(const macos_syscall_context& c);
    void sys_persona(const macos_syscall_context& c);
    void sys_ulock_wait(const macos_syscall_context& c);
    void sys_ulock_wake(const macos_syscall_context& c);
    void sys_semwait_signal(const macos_syscall_context& c);
    void sys_psynch_mutexwait(const macos_syscall_context& c);
    void sys_psynch_mutexdrop(const macos_syscall_context& c);
    void sys_psynch_cvbroad(const macos_syscall_context& c);
    void sys_psynch_cvsignal(const macos_syscall_context& c);
    void sys_psynch_cvwait(const macos_syscall_context& c);
    void sys_psynch_cvclrprepost(const macos_syscall_context& c);

    void sys_read(const macos_syscall_context& c);
    void sys_write(const macos_syscall_context& c);
    void sys_readv(const macos_syscall_context& c);
    void sys_writev(const macos_syscall_context& c);
    void sys_close(const macos_syscall_context& c);
    void sys_lseek(const macos_syscall_context& c);
    void sys_fcntl(const macos_syscall_context& c);
    void sys_ioctl(const macos_syscall_context& c);
    void sys_dup(const macos_syscall_context& c);
    void sys_dup2(const macos_syscall_context& c);

    void sys_open(const macos_syscall_context& c);
    void sys_shm_open(const macos_syscall_context& c);
    void sys_socket(const macos_syscall_context& c);
    void sys_connect(const macos_syscall_context& c);
    void sys_openat(const macos_syscall_context& c);
    void sys_access(const macos_syscall_context& c);
    void sys_faccessat(const macos_syscall_context& c);
    void sys_stat64(const macos_syscall_context& c);
    void sys_lstat64(const macos_syscall_context& c);
    void sys_fstat64(const macos_syscall_context& c);
    void sys_fstatat64(const macos_syscall_context& c);
    void sys_readlink(const macos_syscall_context& c);
    void sys_unlink(const macos_syscall_context& c);
    void sys_rename(const macos_syscall_context& c);
    void sys_rmdir(const macos_syscall_context& c);
    void sys_truncate(const macos_syscall_context& c);
    void sys_mkdir(const macos_syscall_context& c);
    void sys_mkdirat(const macos_syscall_context& c);
    void sys_guarded_open_np(const macos_syscall_context& c);
    void sys_guarded_open_dprotected_np(const macos_syscall_context& c);
    void sys_guarded_close_np(const macos_syscall_context& c);
    void sys_guarded_write_np(const macos_syscall_context& c);
    void sys_guarded_pwrite_np(const macos_syscall_context& c);
    void sys_getdirentries64(const macos_syscall_context& c);
    void sys_statfs64(const macos_syscall_context& c);
    void sys_fstatfs64(const macos_syscall_context& c);

    void sys_mmap(const macos_syscall_context& c);
    void sys_munmap(const macos_syscall_context& c);
    void sys_mprotect(const macos_syscall_context& c);
    void sys_madvise(const macos_syscall_context& c);
    void sys_shared_region_check_np(const macos_syscall_context& c);

    void sys_sysctl(const macos_syscall_context& c);
    void sys_sysctlbyname(const macos_syscall_context& c);

    void sys_gettimeofday(const macos_syscall_context& c);
    void mach_trap_absolute_time(const macos_syscall_context& c);
    void mach_trap_continuous_time(const macos_syscall_context& c);

    uint64_t get_macos_syscall_argument(const macos_syscall_context& c, const size_t index)
    {
        if (index >= MACOS_MAX_SYSCALL_ARGUMENTS || c.argument_offset > MACOS_MAX_SYSCALL_ARGUMENTS - index)
        {
            c.emu_ref.log.warn("Darwin arm64 syscalls have at most %zu register arguments and never spill to the "
                               "stack; argument %zu was requested at offset %zu\n",
                               MACOS_MAX_SYSCALL_ARGUMENTS, index, c.argument_offset);
            c.emu_ref.record_stop(stop_reason::syscall_exception, "syscall argument index out of range");
            c.emu_ref.stop();
            return 0;
        }

        const auto slot = index + c.argument_offset;
        return c.emu.reg(static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + slot));
    }

    void write_macos_syscall_error(const macos_syscall_context& c, const int64_t error)
    {
        if (error < 0)
        {
            c.emu_ref.log.warn("Darwin reports errno %" PRId64 " as a positive value in x0 with the carry flag set; "
                               "the negated form is a Linux convention and reaches the guest unchanged\n",
                               error);
        }

        c.emu.reg(arm64_register::x0, static_cast<uint64_t>(error));
        c.emu.reg(arm64_register::x1, uint64_t{0});
        c.emu.reg(arm64_register::nzcv, c.emu.reg(arm64_register::nzcv) | MACOS_NZCV_CARRY);
    }

    int64_t map_host_errno_to_macos(const int host_errno)
    {
        switch (host_errno)
        {
        case EPERM:
            return MACOS_EPERM;
        case ENOENT:
            return MACOS_ENOENT;
        case ESRCH:
            return MACOS_ESRCH;
        case EINTR:
            return MACOS_EINTR;
        case EIO:
            return MACOS_EIO;
        case ENXIO:
            return MACOS_ENXIO;
        case E2BIG:
            return MACOS_E2BIG;
        case ENOEXEC:
            return MACOS_ENOEXEC;
        case EBADF:
            return MACOS_EBADF;
        case ECHILD:
            return MACOS_ECHILD;
        case EDEADLK:
            return MACOS_EDEADLK;
        case ENOMEM:
            return MACOS_ENOMEM;
        case EACCES:
            return MACOS_EACCES;
        case EFAULT:
            return MACOS_EFAULT;
        case EBUSY:
            return MACOS_EBUSY;
        case EEXIST:
            return MACOS_EEXIST;
        case EXDEV:
            return MACOS_EXDEV;
        case ENODEV:
            return MACOS_ENODEV;
        case ENOTDIR:
            return MACOS_ENOTDIR;
        case EISDIR:
            return MACOS_EISDIR;
        case EINVAL:
            return MACOS_EINVAL;
        case ENFILE:
            return MACOS_ENFILE;
        case EMFILE:
            return MACOS_EMFILE;
        case ENOTTY:
            return MACOS_ENOTTY;
        case EFBIG:
            return MACOS_EFBIG;
        case ENOSPC:
            return MACOS_ENOSPC;
        case ESPIPE:
            return MACOS_ESPIPE;
        case EROFS:
            return MACOS_EROFS;
        case EMLINK:
            return MACOS_EMLINK;
        case EPIPE:
            return MACOS_EPIPE;
        case EDOM:
            return MACOS_EDOM;
        case ERANGE:
            return MACOS_ERANGE;
        case EAGAIN:
            return MACOS_EAGAIN;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
            return MACOS_EWOULDBLOCK;
#endif
        case EINPROGRESS:
            return MACOS_EINPROGRESS;
        case ENOTSOCK:
            return MACOS_ENOTSOCK;
        case ENOTSUP:
            return MACOS_ENOTSUP;
        case EADDRINUSE:
            return MACOS_EADDRINUSE;
        case ETIMEDOUT:
            return MACOS_ETIMEDOUT;
        case ECONNREFUSED:
            return MACOS_ECONNREFUSED;
        case ELOOP:
            return MACOS_ELOOP;
        case ENAMETOOLONG:
            return MACOS_ENAMETOOLONG;
        case ENOTEMPTY:
            return MACOS_ENOTEMPTY;
        case ENOSYS:
            return MACOS_ENOSYS;
        case EOVERFLOW:
            return MACOS_EOVERFLOW;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
            return MACOS_EOPNOTSUPP;
#endif
        default:
            return MACOS_EINVAL;
        }
    }

    bool fd_guard_permits_plain_call(const macos_syscall_context& c, const int fd, const uint32_t operation, const std::string_view call)
    {
        const auto* guard = c.proc.guard_of(fd);
        if (guard == nullptr || (guard->flags & operation) == 0)
        {
            return true;
        }

        std::array<char, 256> detail{};
        std::snprintf(detail.data(), detail.size(), "%.*s on fd %d, which guarded_open_np protected with guardflags 0x%x, at %s",
                      static_cast<int>(call.size()), call.data(), fd, guard->flags,
                      c.emu_ref.symbolizer.format(c.emu.read_instruction_pointer()).c_str());

        write_macos_syscall_error(c, MACOS_EPERM);
        c.emu_ref.record_stop(stop_reason::syscall_exception, detail.data());
        c.emu_ref.stop();
        return false;
    }

    bool fd_guard_matches_argument(const macos_syscall_context& c, const int fd, const uint64_t guard_address, const std::string_view call)
    {
        const auto* guard = c.proc.guard_of(fd);
        if (guard == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return false;
        }

        uint64_t named = 0;
        if (!c.emu_ref.memory.try_read_memory(guard_address, &named, sizeof(named)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return false;
        }

        if (named != guard->id)
        {
            c.emu_ref.log.warn("%.*s named guard 0x%016" PRIx64 " for fd %d, which is guarded by 0x%016" PRIx64 "\n",
                               static_cast<int>(call.size()), call.data(), named, fd, guard->id);
            write_macos_syscall_error(c, MACOS_EPERM);
            return false;
        }

        return true;
    }

    void bsd_syscall_dispatcher::add_handlers()
    {
        this->register_handler(MACOS_SYS_exit, sys_exit, "exit");
        this->register_handler(MACOS_SYS_sigprocmask, sys_sigprocmask, "sigprocmask");

        // __pthread_sigmask sets the calling *thread's* mask rather than the process's. With one thread
        // those are the same mask, and sogen has one thread; the day it does not, this needs to move
        // onto macos_thread rather than gain a separate handler.
        this->register_handler(MACOS_SYS_pthread_sigmask, sys_sigprocmask, "__pthread_sigmask");
        this->register_handler(MACOS_SYS_pthread_kill, sys_pthread_kill, "__pthread_kill");
        this->register_handler(MACOS_SYS_pread, sys_pread, "pread");
        this->register_handler(MACOS_SYS_pwrite, sys_pwrite, "pwrite");
        this->register_handler(MACOS_SYS_bsdthread_ctl, sys_bsdthread_ctl, "bsdthread_ctl");
        this->register_handler(MACOS_SYS_abort_with_payload, sys_abort_with_payload, "abort_with_payload");
        this->register_handler(MACOS_SYS_terminate_with_payload, sys_terminate_with_payload, "terminate_with_payload");
        this->register_handler(MACOS_SYS_getpid, sys_getpid, "getpid");
        this->register_handler(MACOS_SYS_kdebug_typefilter, sys_kdebug_typefilter, "kdebug_typefilter");
        this->register_handler(MACOS_SYS_kdebug_trace_string, sys_kdebug_trace_string, "kdebug_trace_string");
        this->register_handler(MACOS_SYS_kdebug_trace64, sys_kdebug_trace, "kdebug_trace64");
        this->register_handler(MACOS_SYS_kdebug_trace, sys_kdebug_trace, "kdebug_trace");
        this->register_handler(MACOS_SYS_getrlimit, sys_getrlimit, "getrlimit");
        this->register_handler(MACOS_SYS_setrlimit, sys_setrlimit, "setrlimit");
        this->register_handler(MACOS_SYS_sigaction, sys_sigaction, "sigaction");
        this->register_handler(MACOS_SYS_fgetattrlist, sys_fgetattrlist, "fgetattrlist");
        this->register_handler(MACOS_SYS_pathconf, sys_pathconf, "pathconf");
        this->register_handler(MACOS_SYS_fpathconf, sys_fpathconf, "fpathconf");
        this->register_handler(MACOS_SYS_gettid, sys_gettid, "gettid");
        this->register_handler(MACOS_SYS_getaudit_addr, sys_getaudit_addr, "getaudit_addr");
        this->register_handler(MACOS_SYS_gethostuuid, sys_gethostuuid, "gethostuuid");
        this->register_handler(MACOS_SYS_getppid, sys_getppid, "getppid");
        this->register_handler(MACOS_SYS_getuid, sys_getuid, "getuid");
        this->register_handler(MACOS_SYS_geteuid, sys_geteuid, "geteuid");
        this->register_handler(MACOS_SYS_getgid, sys_getgid, "getgid");
        this->register_handler(MACOS_SYS_getegid, sys_getegid, "getegid");
        this->register_handler(MACOS_SYS_iopolicysys, sys_iopolicysys, "iopolicysys");
        this->register_handler(MACOS_SYS_issetugid, sys_issetugid, "issetugid");
        this->register_handler(MACOS_SYS_getentropy, sys_getentropy, "getentropy");
        this->register_handler(MACOS_SYS_thread_selfid, sys_thread_selfid, "thread_selfid");
        this->register_handler(MACOS_SYS_csops, sys_csops, "csops");
        this->register_handler(MACOS_SYS_csops_audittoken, sys_csops_audittoken, "csops_audittoken");
        this->register_handler(MACOS_SYS_proc_info, sys_proc_info, "proc_info");
        this->register_handler(MACOS_SYS_mac_syscall, sys_mac_syscall, "mac_syscall");
        this->register_handler(MACOS_SYS_bsdthread_register, sys_bsdthread_register, "bsdthread_register");
        this->register_handler(MACOS_SYS_workq_open, sys_workq_open, "workq_open");
        this->register_handler(MACOS_SYS_workq_kernreturn, sys_workq_kernreturn, "workq_kernreturn");
        this->register_handler(MACOS_SYS_kqueue, sys_kqueue, "kqueue");
        this->register_handler(MACOS_SYS_kevent_qos, sys_kevent_qos, "kevent_qos");
        this->register_handler(MACOS_SYS_kevent_id, sys_kevent_id, "kevent_id");
        this->register_handler(MACOS_SYS_bsdthread_create, sys_bsdthread_create, "bsdthread_create");
        this->register_handler(MACOS_SYS_bsdthread_terminate, sys_bsdthread_terminate, "bsdthread_terminate");
        this->register_handler(MACOS_SYS_disable_threadsignal, sys_disable_threadsignal, "__disable_threadsignal");
        this->register_handler(MACOS_SYS_persona, sys_persona, "persona");
        this->register_handler(MACOS_SYS_ulock_wait, sys_ulock_wait, "ulock_wait");
        this->register_handler(MACOS_SYS_ulock_wake, sys_ulock_wake, "ulock_wake");
        // The _nocancel form only skips a pthread cancellation check, which an emulator without
        // cancellable threads does not have either -- same alias rule as read_nocancel & co.
        this->register_handler(MACOS_SYS_semwait_signal, sys_semwait_signal, "__semwait_signal");
        this->register_handler(MACOS_SYS_semwait_signal_nocancel, sys_semwait_signal, "__semwait_signal_nocancel");
        this->register_handler(MACOS_SYS_psynch_mutexwait, sys_psynch_mutexwait, "psynch_mutexwait");
        this->register_handler(MACOS_SYS_psynch_mutexdrop, sys_psynch_mutexdrop, "psynch_mutexdrop");
        this->register_handler(MACOS_SYS_psynch_cvbroad, sys_psynch_cvbroad, "psynch_cvbroad");
        this->register_handler(MACOS_SYS_psynch_cvsignal, sys_psynch_cvsignal, "psynch_cvsignal");
        this->register_handler(MACOS_SYS_psynch_cvwait, sys_psynch_cvwait, "psynch_cvwait");
        this->register_handler(MACOS_SYS_psynch_cvclrprepost, sys_psynch_cvclrprepost, "psynch_cvclrprepost");

        this->register_handler(MACOS_SYS_read, sys_read, "read");
        this->register_handler(MACOS_SYS_write, sys_write, "write");
        this->register_handler(MACOS_SYS_readv, sys_readv, "readv");
        this->register_handler(MACOS_SYS_writev, sys_writev, "writev");
        this->register_handler(MACOS_SYS_close, sys_close, "close");
        this->register_handler(MACOS_SYS_lseek, sys_lseek, "lseek");
        this->register_handler(MACOS_SYS_fcntl, sys_fcntl, "fcntl");
        this->register_handler(MACOS_SYS_ioctl, sys_ioctl, "ioctl");
        this->register_handler(MACOS_SYS_dup, sys_dup, "dup");
        this->register_handler(MACOS_SYS_dup2, sys_dup2, "dup2");

        this->register_handler(MACOS_SYS_open, sys_open, "open");
        this->register_handler(MACOS_SYS_shm_open, sys_shm_open, "shm_open");
        this->register_handler(MACOS_SYS_socket, sys_socket, "socket");
        this->register_handler(MACOS_SYS_connect, sys_connect, "connect");
        this->register_handler(MACOS_SYS_openat, sys_openat, "openat");
        this->register_handler(MACOS_SYS_read_nocancel, sys_read, "read_nocancel");
        this->register_handler(MACOS_SYS_write_nocancel, sys_write, "write_nocancel");
        this->register_handler(MACOS_SYS_open_nocancel, sys_open, "open_nocancel");
        this->register_handler(MACOS_SYS_close_nocancel, sys_close, "close_nocancel");
        this->register_handler(MACOS_SYS_fcntl_nocancel, sys_fcntl, "fcntl_nocancel");
        this->register_handler(MACOS_SYS_connect_nocancel, sys_connect, "connect_nocancel");
        this->register_handler(MACOS_SYS_readv_nocancel, sys_readv, "readv_nocancel");
        this->register_handler(MACOS_SYS_writev_nocancel, sys_writev, "writev_nocancel");
        this->register_handler(MACOS_SYS_pread_nocancel, sys_pread, "pread_nocancel");
        this->register_handler(MACOS_SYS_fsync_nocancel, sys_fsync, "fsync_nocancel");
        this->register_handler(MACOS_SYS_pwrite_nocancel, sys_pwrite, "pwrite_nocancel");
        this->register_handler(MACOS_SYS_openat_nocancel, sys_openat, "openat_nocancel");
        this->register_handler(MACOS_SYS_access, sys_access, "access");
        this->register_handler(MACOS_SYS_faccessat, sys_faccessat, "faccessat");
        this->register_handler(MACOS_SYS_stat64, sys_stat64, "stat64");
        this->register_handler(MACOS_SYS_lstat64, sys_lstat64, "lstat64");
        this->register_handler(MACOS_SYS_fstat64, sys_fstat64, "fstat64");
        this->register_handler(MACOS_SYS_fstatat64, sys_fstatat64, "fstatat64");
        this->register_handler(MACOS_SYS_readlink, sys_readlink, "readlink");
        this->register_handler(MACOS_SYS_unlink, sys_unlink, "unlink");
        this->register_handler(MACOS_SYS_rename, sys_rename, "rename");
        this->register_handler(MACOS_SYS_rmdir, sys_rmdir, "rmdir");
        this->register_handler(MACOS_SYS_truncate, sys_truncate, "truncate");
        this->register_handler(MACOS_SYS_ftruncate, sys_ftruncate, "ftruncate");
        this->register_handler(MACOS_SYS_futimes, sys_futimes, "futimes");
        this->register_handler(MACOS_SYS_fchown, sys_fchown, "fchown");
        this->register_handler(MACOS_SYS_fchmod, sys_fchmod, "fchmod");
        this->register_handler(MACOS_SYS_fsync, sys_fsync, "fsync");
        this->register_handler(MACOS_SYS_mkdir, sys_mkdir, "mkdir");
        this->register_handler(MACOS_SYS_mkdirat, sys_mkdirat, "mkdirat");

        // A guarded descriptor is an ordinary one carrying a caller-chosen id and the set of operations
        // that may only be performed by naming the id back. Everything past the id is the plain syscall,
        // which is why these share their implementations with open, close, write and pwrite.
        this->register_handler(MACOS_SYS_guarded_open_np, sys_guarded_open_np, "guarded_open_np");
        this->register_handler(MACOS_SYS_guarded_open_dprotected_np, sys_guarded_open_dprotected_np, "guarded_open_dprotected_np");
        this->register_handler(MACOS_SYS_guarded_close_np, sys_guarded_close_np, "guarded_close_np");
        this->register_handler(MACOS_SYS_guarded_write_np, sys_guarded_write_np, "guarded_write_np");
        this->register_handler(MACOS_SYS_guarded_pwrite_np, sys_guarded_pwrite_np, "guarded_pwrite_np");
        this->register_handler(MACOS_SYS_getdirentries64, sys_getdirentries64, "getdirentries64");
        this->register_handler(MACOS_SYS_statfs64, sys_statfs64, "statfs64");
        this->register_handler(MACOS_SYS_fstatfs64, sys_fstatfs64, "fstatfs64");

        this->register_handler(MACOS_SYS_mmap, sys_mmap, "mmap");
        this->register_handler(MACOS_SYS_munmap, sys_munmap, "munmap");
        this->register_handler(MACOS_SYS_mprotect, sys_mprotect, "mprotect");
        this->register_handler(MACOS_SYS_madvise, sys_madvise, "madvise");
        this->register_handler(MACOS_SYS_shared_region_check_np, sys_shared_region_check_np, "shared_region_check_np");

        this->register_handler(MACOS_SYS_sysctl, sys_sysctl, "sysctl");
        this->register_handler(MACOS_SYS_sysctlbyname, sys_sysctlbyname, "sysctlbyname");

        this->register_handler(MACOS_SYS_gettimeofday, sys_gettimeofday, "gettimeofday");

        // The table is indexed by the negated x16, so these are the positive counterparts of the -3 and
        // -4 xnu special-cases in handle_svc.
        this->register_mach_trap(macos_mach_traps::MACOS_MACH_abstime, mach_trap_absolute_time, "mach_absolute_time");
        this->register_mach_trap(macos_mach_traps::MACOS_MACH_conttime, mach_trap_continuous_time, "mach_continuous_time");

        this->add_mach_traps();
        this->add_dyld_handlers();
    }

    std::vector<uint64_t> registered_bsd_syscall_numbers(const bsd_syscall_dispatcher& dispatcher)
    {
        std::vector<uint64_t> numbers{};

        for (size_t i = 0; i < dispatcher.handlers_.size(); ++i)
        {
            if (dispatcher.handlers_[i].handler != nullptr)
            {
                numbers.push_back(static_cast<uint64_t>(i));
            }
        }

        return numbers;
    }

    instruction_hook_continuation bsd_syscall_dispatcher::dispatch(macos_emulator& emu_ref) const
    {
        auto& e = emu_ref.emu();

        // A trap written over a shared cache export reaches the same hook as a real syscall, so the
        // patched entries are checked before x16 is read. The carry flag is deliberately left alone on
        // this path: these are ordinary C function returns, and a caller's flags survive a real bl.
        if (const auto trap_pc = e.read_instruction_pointer(); trap_pc >= 4)
        {
            const auto entry = trap_pc - 4;

            auto* calls = emu_ref.guest_call_stack();
            if (calls != nullptr && calls->handle_trap(emu_ref, entry))
            {
                return instruction_hook_continuation::finalized_instruction_pointer;
            }

            auto* native = emu_ref.native_dispatch();
            if (native != nullptr && native->invoke(emu_ref, entry))
            {
                return instruction_hook_continuation::finalized_instruction_pointer;
            }
        }

        // xnu clears the carry flag on the trap path before the handler runs, so a syscall that writes no
        // result still reports success to a `cerror` stub branching on `b.lo`.
        clear_macos_syscall_carry(e);

        const auto trap_no = static_cast<int32_t>(static_cast<uint32_t>(e.reg(arm64_register::x16)));

        macos_syscall_context ctx{.emu_ref = emu_ref, .emu = e, .proc = emu_ref.process, .argument_offset = 0};

        const bsd_syscall_handler_entry* entry = nullptr;
        uint64_t reported_id = 0;
        std::string_view reported_name{"<unknown>"};

        // The sentinel comparison has to precede the `< 0` test, exactly as xnu orders it:
        // MACOS_PLATFORM_SYSCALL_TRAP_NO is INT32_MIN, so the Mach-trap branch would negate it into itself
        // and look up a nonsensical table index.
        if (trap_no == MACOS_PLATFORM_SYSCALL_TRAP_NO)
        {
            static const bsd_syscall_handler_entry platform_entry{.handler = sys_platform_syscall, .name = "platform_syscall"};
            entry = &platform_entry;
            reported_name = platform_entry.name;
        }
        else if (trap_no < 0)
        {
            const auto index = static_cast<uint32_t>(-trap_no);
            reported_id = index;
            entry = this->get_mach_trap_entry(index);
            reported_name = (entry != nullptr && !entry->name.empty()) ? std::string_view{entry->name} : std::string_view{"mach_trap"};
        }
        else
        {
            auto number = static_cast<uint64_t>(trap_no);
            if (trap_no == 0)
            {
                number = e.reg(arm64_register::x0);
                ctx.argument_offset = 1;
            }

            reported_id = number;
            entry = this->get_entry(number);
            if (entry != nullptr && !entry->name.empty())
            {
                reported_name = std::string_view{entry->name};
            }
        }

        if (emu_ref.callbacks.on_syscall)
        {
            const auto res = emu_ref.callbacks.on_syscall(reported_id, reported_name);
            if (res == instruction_hook_continuation::skip_instruction ||
                res == instruction_hook_continuation::finalized_instruction_pointer)
            {
                return res;
            }
        }

        if (entry == nullptr || !entry->handler)
        {
            // Halting rather than returning ENOSYS is the point: the libSystem syscalls beyond dyld's
            // measured set are not enumerated anywhere, and a silent ENOSYS turns each one into an
            // unexplained crash hundreds of thousands of instructions later. Naming the syscall and the
            // module that issued it is the bring-up loop's only instrument.
            // The dispatcher's own table only knows the syscalls sogen implements, so an unimplemented one
            // has no name there -- and "bsd 305 (<unknown>)" sends the reader to a syscall list instead of
            // to the code. The generated prototype table knows every number Darwin defines, so it is
            // asked before the name is printed.
            auto named = reported_name;
            if (trap_no >= 0 && (named.empty() || named == "<unknown>"))
            {
                if (const auto* prototype = find_bsd_syscall_prototype(static_cast<uint32_t>(reported_id)); prototype != nullptr)
                {
                    named = prototype->name;
                }
            }

            std::array<char, 256> detail{};
            std::snprintf(detail.data(), detail.size(), "%s %" PRIu64 " (%.*s) at %s", trap_no < 0 ? "mach trap" : "bsd", reported_id,
                          static_cast<int>(named.size()), named.data(), emu_ref.symbolizer.format(e.read_instruction_pointer()).c_str());

            write_macos_syscall_error(ctx, MACOS_ENOSYS);
            emu_ref.record_stop(stop_reason::unimplemented_syscall, detail.data());
            emu_ref.stop();
            return instruction_hook_continuation::skip_instruction;
        }

        if (trap_no < 0)
        {
            const auto continuation = emu_ref.callbacks.on_mach_trap(static_cast<uint32_t>(reported_id), reported_name);
            if (continuation != instruction_hook_continuation::run_instruction)
            {
                return continuation;
            }

            emit_mach_trap_trace(emu_ref, ctx, static_cast<uint32_t>(reported_id), reported_name);
        }
        else
        {
            emit_bsd_syscall_trace(emu_ref, ctx, static_cast<uint32_t>(reported_id), reported_name);
        }

        // Arguments are read before the handler runs and the errno after: by the time a handler returns,
        // x0 holds the result and a write buffer may already have been consumed.
        entry->handler(ctx);
        emit_syscall_error_trace(emu_ref, reported_name);
        return instruction_hook_continuation::skip_instruction;
    }

}

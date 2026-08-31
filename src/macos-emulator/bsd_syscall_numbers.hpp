#pragma once

#include <cstdint>

// Darwin BSD syscall numbers, from xnu's bsd/kern/syscalls.master. They are architecture-independent:
// arm64 passes the number in x16 rather than adding a class byte the way the x86_64 ABI does.

namespace sogen
{

    namespace macos_syscalls
    {
        constexpr uint64_t MACOS_SYS_syscall = 0;
        constexpr uint64_t MACOS_SYS_exit = 1;
        constexpr uint64_t MACOS_SYS_read = 3;
        constexpr uint64_t MACOS_SYS_write = 4;
        constexpr uint64_t MACOS_SYS_open = 5;
        constexpr uint64_t MACOS_SYS_close = 6;
        constexpr uint64_t MACOS_SYS_unlink = 10;
        constexpr uint64_t MACOS_SYS_mkdir = 136;
        constexpr uint64_t MACOS_SYS_getpid = 20;
        constexpr uint64_t MACOS_SYS_kdebug_typefilter = 177;
        constexpr uint64_t MACOS_SYS_kdebug_trace_string = 178;
        constexpr uint64_t MACOS_SYS_kdebug_trace64 = 179;
        constexpr uint64_t MACOS_SYS_kdebug_trace = 180;
        constexpr uint64_t MACOS_SYS_sigprocmask = 48;
        constexpr uint64_t MACOS_SYS_getuid = 24;
        constexpr uint64_t MACOS_SYS_geteuid = 25;
        constexpr uint64_t MACOS_SYS_access = 33;
        constexpr uint64_t MACOS_SYS_getppid = 39;
        constexpr uint64_t MACOS_SYS_dup = 41;
        constexpr uint64_t MACOS_SYS_getegid = 43;
        constexpr uint64_t MACOS_SYS_getgid = 47;
        constexpr uint64_t MACOS_SYS_ioctl = 54;
        constexpr uint64_t MACOS_SYS_getrlimit = 194;
        constexpr uint64_t MACOS_SYS_setrlimit = 195;
        constexpr uint64_t MACOS_SYS_sigaction = 46;
        constexpr uint64_t MACOS_SYS_fgetattrlist = 228;
        // 286 is gettid, which XNU also exposes as pthread_getugid_np; getaudit_addr is 357. Getting
        // these the wrong way round writes a 48-byte struct where the caller reserved 4 bytes.
        constexpr uint64_t MACOS_SYS_gettid = 286;
        constexpr uint64_t MACOS_SYS_getaudit_addr = 357;
        constexpr uint64_t MACOS_SYS_readlink = 58;
        constexpr uint64_t MACOS_SYS_munmap = 73;
        constexpr uint64_t MACOS_SYS_mprotect = 74;
        constexpr uint64_t MACOS_SYS_madvise = 75;
        constexpr uint64_t MACOS_SYS_dup2 = 90;
        constexpr uint64_t MACOS_SYS_fcntl = 92;
        constexpr uint64_t MACOS_SYS_socket = 97;
        constexpr uint64_t MACOS_SYS_connect = 98;
        constexpr uint64_t MACOS_SYS_gettimeofday = 116;
        constexpr uint64_t MACOS_SYS_readv = 120;
        constexpr uint64_t MACOS_SYS_writev = 121;
        constexpr uint64_t MACOS_SYS_csops = 169;
        constexpr uint64_t MACOS_SYS_csops_audittoken = 170;
        // What libsqlite3 does to a database file once it is open. The set is that library's own import
        // table rather than a guess; flock is the one file call it imports that sogen leaves out, because
        // it only reaches it through the unix-flock VFS, which nothing here selects.
        constexpr uint64_t MACOS_SYS_fsync = 95;
        constexpr uint64_t MACOS_SYS_fchown = 123;
        constexpr uint64_t MACOS_SYS_fchmod = 124;
        constexpr uint64_t MACOS_SYS_rename = 128;
        constexpr uint64_t MACOS_SYS_rmdir = 137;
        constexpr uint64_t MACOS_SYS_futimes = 139;
        constexpr uint64_t MACOS_SYS_truncate = 200;
        constexpr uint64_t MACOS_SYS_ftruncate = 201;
        constexpr uint64_t MACOS_SYS_gethostuuid = 142;
        constexpr uint64_t MACOS_SYS_pread = 153;
        constexpr uint64_t MACOS_SYS_pwrite = 154;
        constexpr uint64_t MACOS_SYS_mmap = 197;
        constexpr uint64_t MACOS_SYS_crossarch_trap = 38;
        constexpr uint64_t MACOS_SYS_lseek = 199;
        constexpr uint64_t MACOS_SYS_sysctl = 202;
        constexpr uint64_t MACOS_SYS_getattrlist = 220;
        constexpr uint64_t MACOS_SYS_pathconf = 191;
        constexpr uint64_t MACOS_SYS_fpathconf = 192;
        constexpr uint64_t MACOS_SYS_fsctl = 242;
        constexpr uint64_t MACOS_SYS_shm_open = 266;
        constexpr uint64_t MACOS_SYS_sysctlbyname = 274;
        constexpr uint64_t MACOS_SYS_shared_region_check_np = 294;
        constexpr uint64_t MACOS_SYS_psynch_mutexwait = 301;
        constexpr uint64_t MACOS_SYS_psynch_mutexdrop = 302;
        constexpr uint64_t MACOS_SYS_psynch_cvbroad = 303;
        constexpr uint64_t MACOS_SYS_psynch_cvsignal = 304;
        constexpr uint64_t MACOS_SYS_psynch_cvwait = 305;
        constexpr uint64_t MACOS_SYS_psynch_cvclrprepost = 312;
        constexpr uint64_t MACOS_SYS_iopolicysys = 322;
        constexpr uint64_t MACOS_SYS_issetugid = 327;
        constexpr uint64_t MACOS_SYS_pthread_kill = 328;
        constexpr uint64_t MACOS_SYS_pthread_sigmask = 329;
        constexpr uint64_t MACOS_SYS_semwait_signal = 334;
        constexpr uint64_t MACOS_SYS_proc_info = 336;
        constexpr uint64_t MACOS_SYS_stat64 = 338;
        constexpr uint64_t MACOS_SYS_fstat64 = 339;
        constexpr uint64_t MACOS_SYS_lstat64 = 340;
        constexpr uint64_t MACOS_SYS_getdirentries64 = 344;
        constexpr uint64_t MACOS_SYS_statfs64 = 345;
        constexpr uint64_t MACOS_SYS_fstatfs64 = 346;
        constexpr uint64_t MACOS_SYS_getfsstat64 = 347;
        constexpr uint64_t MACOS_SYS_bsdthread_create = 360;
        constexpr uint64_t MACOS_SYS_bsdthread_terminate = 361;
        constexpr uint64_t MACOS_SYS_kqueue = 362;
        constexpr uint64_t MACOS_SYS_bsdthread_register = 366;
        constexpr uint64_t MACOS_SYS_workq_open = 367;
        constexpr uint64_t MACOS_SYS_workq_kernreturn = 368;
        constexpr uint64_t MACOS_SYS_kevent_qos = 374;
        constexpr uint64_t MACOS_SYS_kevent_id = 375;
        constexpr uint64_t MACOS_SYS_thread_selfid = 372;
        constexpr uint64_t MACOS_SYS_mac_syscall = 381;
        // libSystem routes a call through the _nocancel entry whenever the thread is not a cancellation
        // point, which is most of the time -- libsystem_trace reaches open_nocancel before main(). The
        // kernel runs the identical implementation and only skips the pthread cancellation check, which
        // an emulator without cancellable threads does not have either, so these are plain aliases. The
        // numbers are not contiguous with their cancellable forms and were read from this host's own
        // libsystem_kernel stubs rather than guessed.
        constexpr uint64_t MACOS_SYS_read_nocancel = 396;
        constexpr uint64_t MACOS_SYS_write_nocancel = 397;
        constexpr uint64_t MACOS_SYS_open_nocancel = 398;
        constexpr uint64_t MACOS_SYS_close_nocancel = 399;
        constexpr uint64_t MACOS_SYS_fcntl_nocancel = 406;
        constexpr uint64_t MACOS_SYS_connect_nocancel = 409;
        constexpr uint64_t MACOS_SYS_readv_nocancel = 411;
        constexpr uint64_t MACOS_SYS_writev_nocancel = 412;
        constexpr uint64_t MACOS_SYS_pread_nocancel = 414;
        constexpr uint64_t MACOS_SYS_fsync_nocancel = 408;
        constexpr uint64_t MACOS_SYS_pwrite_nocancel = 415;
        constexpr uint64_t MACOS_SYS_semwait_signal_nocancel = 423;
        constexpr uint64_t MACOS_SYS_openat_nocancel = 464;

        // The guarded descriptor family. libsqlite3 opens every database file this way and then writes and
        // closes it only through the guarded calls, so CoreData reaches all five before it has read a row;
        // libdispatch and libxpc guard descriptors of their own. xnu also names guarded_kqueue_np (443)
        // and guarded_writev_np (487), and a scan of this host's whole shared cache for callers found
        // none, so they are left out rather than written against a caller that does not exist.
        constexpr uint64_t MACOS_SYS_guarded_open_np = 441;
        constexpr uint64_t MACOS_SYS_guarded_close_np = 442;
        constexpr uint64_t MACOS_SYS_guarded_open_dprotected_np = 484;
        constexpr uint64_t MACOS_SYS_guarded_write_np = 485;
        constexpr uint64_t MACOS_SYS_guarded_pwrite_np = 486;

        constexpr uint64_t MACOS_SYS_fsgetpath = 427;
        constexpr uint64_t MACOS_SYS_getattrlistbulk = 461;
        constexpr uint64_t MACOS_SYS_openat = 463;
        constexpr uint64_t MACOS_SYS_faccessat = 466;
        constexpr uint64_t MACOS_SYS_fstatat64 = 470;
        constexpr uint64_t MACOS_SYS_mkdirat = 475;
        constexpr uint64_t MACOS_SYS_csrctl = 483;
        constexpr uint64_t MACOS_SYS_mremap_encrypted = 489;
        constexpr uint64_t MACOS_SYS_bsdthread_ctl = 478;
        constexpr uint64_t MACOS_SYS_getentropy = 500;
        constexpr uint64_t MACOS_SYS_ulock_wait = 515;
        constexpr uint64_t MACOS_SYS_disable_threadsignal = 331;
        constexpr uint64_t MACOS_SYS_persona = 494;
        constexpr uint64_t MACOS_SYS_ulock_wake = 516;
        constexpr uint64_t MACOS_SYS_terminate_with_payload = 520;
        constexpr uint64_t MACOS_SYS_abort_with_payload = 521;
        constexpr uint64_t MACOS_SYS_objc_bp_assist_cfg_np = 535;
        constexpr uint64_t MACOS_SYS_shared_region_map_and_slide_2_np = 536;
        constexpr uint64_t MACOS_SYS_map_with_linking_np = 550;
    }

    namespace macos_mach_traps
    {
        constexpr uint32_t MACOS_MACH_abstime = 3;
        constexpr uint32_t MACOS_MACH_conttime = 4;
    }

}

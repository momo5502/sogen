#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <memory_permission.hpp>

namespace sogen
{

    constexpr uint64_t MACOS_PAGE_SIZE = 0x4000ULL;
    constexpr uint64_t MACOS_ALLOCATION_GRANULARITY = MACOS_PAGE_SIZE;

    // Darwin's default soft RLIMIT_NOFILE, as reported by `launchctl limit maxfiles`. Descriptors are
    // backed by real host handles, so without this a guest loop exhausts the emulator process's own
    // descriptor table rather than only its own.
    constexpr int MACOS_MAX_OPEN_DESCRIPTORS = 256;

    // svc #0x80, the Darwin syscall trap. Written over a shared cache export's first instruction to
    // hand the call to a native implementation.
    constexpr uint32_t MACOS_ARM64_SVC_80 = 0xD4001001u;

    // One page of svc #0x80, used as the return address of a call a native handler makes into the guest.
    // Placed above the shared region so it collides with nothing the guest maps for itself.
    // What bsdthread_register hands back, which libpthread keeps as __pthread_supported_features and
    // treats as zero-means-uninitialised. The kernel on build 25G76 returns 0x400011DF, read out of a
    // live process rather than assembled from header constants.
    //
    // Reported whole, including KEVENT (0x40) and WORKLOOP (0x80). Clearing those looks like the honest
    // thing -- sogen serves no kevent-delivered work -- but libdispatch on this build has no other path:
    // with them clear it aborts inside _libdispatch_init with the mask in x0. There is no subset of this
    // word that is both truthful and usable, so it matches the kernel and the kevent entry points answer
    // for themselves.
    constexpr uint64_t MACOS_PTHREAD_SUPPORTED_FEATURES = 0x400011DFULL;

    // WQOPS_SETUP_DISPATCH, the one workq_kernreturn operation that asks for nothing to be scheduled.
    constexpr uint32_t MACOS_WQOPS_SETUP_DISPATCH = 0x400;

    // WQOPS_THREAD_RETURN, xnu bsd/pthread/workqueue_syscalls.h: a worker parking itself back into the
    // kernel pool. On a real kernel the call does not return to userspace -- __pthread_wqthread
    // returning at all traps _start_wqthread's brk #1 -- and the thread is next entered at
    // _start_wqthread with the REUSE bit set.
    constexpr uint32_t MACOS_WQOPS_THREAD_RETURN = 0x4;

    // The park variants __pthread_wqthread uses when it handled kernel-delivered events: x1 points at
    // the kevent buffer the kernel refills on the next wake. Measured 2026-08-27 on the host (cgsdemo
    // under lldb): 0x100 follows a workloop (flags bit 22) entry, 0x40 a plain kevent (bit 19) one.
    // WQOPS_QUEUE_REQTHREADS (xnu bsd/pthread/pthread_workqueue.c) is libdispatch asking the kernel for
    // worker threads outright rather than through a kevent registration: arg2 is how many, arg3 the
    // pthread priority. A queue with no kevent source -- a plain dispatch_async, or libxpc synthesizing
    // a reply for a connection that never reached a daemon -- runs no other way.
    constexpr uint32_t MACOS_WQOPS_QUEUE_REQTHREADS = 0x20;
    constexpr uint32_t MACOS_WQOPS_QUEUE_REQTHREADS2 = 0x30;

    constexpr uint32_t MACOS_WQOPS_THREAD_WORKLOOP_RETURN = 0x100;
    constexpr uint32_t MACOS_WQOPS_THREAD_KEVENT_RETURN = 0x40;

    // Call flags of kevent_qos/kevent_id (arg 7), measured 2026-08-27 from this build's libdispatch;
 // see ("Measured
    // 2026-08-27"). WORKQ says the registration targets the process workqueue and the kq argument is
    // ignored (observed 0xffffffff); WORKLOOP is the kevent_id form, where arg 0 is the workloop's
    // dynamic kq id rather than a file descriptor.
    constexpr uint32_t MACOS_KEVENT_FLAG_WORKQ = 0x20;
    constexpr uint32_t MACOS_KEVENT_FLAG_WORKLOOP = 0x400;

    // sys/event.h changelist operation bits.
    constexpr uint16_t MACOS_EV_ADD = 0x1;
    constexpr uint16_t MACOS_EV_DELETE = 0x2;
    constexpr uint16_t MACOS_EV_ENABLE = 0x4;
    constexpr uint16_t MACOS_EV_DISABLE = 0x8;
    constexpr uint16_t MACOS_EV_ONESHOT = 0x10;
    constexpr uint16_t MACOS_EV_CLEAR = 0x20;
    constexpr uint16_t MACOS_EV_DISPATCH = 0x80;
    constexpr uint16_t MACOS_EV_VANISHED = 0x200;

    // sys/event.h EVFILT_TIMER and its fflags. The unit bits are mutually exclusive and pick what the
    // changelist's `data` counts (xnu bsd/kern/kern_event.c filt_timervalidate); with none of them set
    // it is milliseconds. NOTE_ABSOLUTE turns `data` from an interval into a deadline -- measured
    // 2026-08-27 on this host: against mach_absolute_time when NOTE_MACHTIME is set, and against the
    // calendar clock when it is not. NOTE_LEEWAY only names ext[1] as a coalescing window, and
    // NOTE_CRITICAL/NOTE_BACKGROUND only widen or narrow it, so a clock that fires a deadline exactly
    // once reads all three and uses none.
    constexpr int16_t MACOS_EVFILT_TIMER = -7;
    constexpr uint32_t MACOS_NOTE_SECONDS = 0x00000001;
    constexpr uint32_t MACOS_NOTE_USECONDS = 0x00000002;
    constexpr uint32_t MACOS_NOTE_NSECONDS = 0x00000004;
    constexpr uint32_t MACOS_NOTE_ABSOLUTE = 0x00000008;
    constexpr uint32_t MACOS_NOTE_LEEWAY = 0x00000010;
    constexpr uint32_t MACOS_NOTE_CRITICAL = 0x00000020;
    constexpr uint32_t MACOS_NOTE_BACKGROUND = 0x00000040;
    constexpr uint32_t MACOS_NOTE_MACH_CONTINUOUS_TIME = 0x00000080;
    constexpr uint32_t MACOS_NOTE_MACHTIME = 0x00000100;

    // sys/event.h filter id. EVFILT_MACHPORT monitors a mach port name; with MACH_RCV_MSG fflags the
    // kernel receives kernel-side when a message arrives and wakes a workqueue worker with the event.
    constexpr int16_t MACOS_EVFILT_MACHPORT = -8;

    // sys/event.h EVFILT_USER: a knote with no kernel event source at all -- the guest fires it itself
    // with NOTE_TRIGGER, which is how libdispatch wakes a queue that is waiting on nothing else. The
    // control bits pick how the changelist's 24 user flag bits combine into the knote's stored set.
    constexpr int16_t MACOS_EVFILT_USER = -10;
    constexpr uint32_t MACOS_NOTE_FFNOP = 0x00000000;
    constexpr uint32_t MACOS_NOTE_FFAND = 0x40000000;
    constexpr uint32_t MACOS_NOTE_FFOR = 0x80000000;
    constexpr uint32_t MACOS_NOTE_FFCOPY = 0xC0000000;
    constexpr uint32_t MACOS_NOTE_FFCTRLMASK = 0xC0000000;
    constexpr uint32_t MACOS_NOTE_FFLAGSMASK = 0x00FFFFFF;
    constexpr uint32_t MACOS_NOTE_TRIGGER = 0x01000000;

    // xnu bsd/sys/event_private.h. NOTE_WL_THREAD_REQUEST is the only changelist bit the headers
    // literally name a thread request (same measurement as the call flags above).
    constexpr int16_t MACOS_EVFILT_WORKLOOP = -17;
    constexpr uint32_t MACOS_NOTE_WL_THREAD_REQUEST = 0x1;

    constexpr uint64_t MACOS_GUI_TRAP_BASE = 0x2F0000000ULL;

    // Window backing stores live in one arena so a present can be bounds-checked with a range test
    // rather than by trusting whatever address the guest left in the window record.
    constexpr uint64_t MACOS_GUI_ARENA_BASE = 0x300000000ULL;
    constexpr uint64_t MACOS_GUI_ARENA_SIZE = 0x40000000ULL;
    constexpr int32_t MACOS_GUI_MAX_WINDOW_DIMENSION = 16384;

 // Stage A worker threads:
    // every spawned worker gets a 512 KiB stack and a raw page for the pthread struct libpthread lays
    // out itself, one slot per worker out of an arena disjoint from the GUI arena above and the trap
    // page. The arena bound is the worker cap.
    constexpr uint64_t MACOS_WORKQUEUE_ARENA_BASE = 0x340000000ULL;
    constexpr uint64_t MACOS_WORKQUEUE_ARENA_SIZE = 0x4000000ULL;
    constexpr uint64_t MACOS_WORKQUEUE_STACK_SIZE = 0x80000ULL;
    constexpr uint64_t MACOS_WORKQUEUE_SLOT_SIZE = MACOS_WORKQUEUE_STACK_SIZE + MACOS_PAGE_SIZE;
    constexpr size_t MACOS_WORKQUEUE_MAX_WORKERS = MACOS_WORKQUEUE_ARENA_SIZE / MACOS_WORKQUEUE_SLOT_SIZE;
    // Confirmed a regular export of this build's libsystem_pthread in the measurement the spec cites, so
    // runtime resolution should never need it; when the cache cannot be read it stands in, with a warn.
    constexpr uint64_t MACOS_START_WQTHREAD_FALLBACK = 0x1804FAC08ULL;

    // The flags a newly spawned (non-REUSE) worker is entered with, measured 2026-08-27 by breaking at
    // __pthread_wqthread's entry in a live host process (a user-initiated global-queue worker). x4 = 0
    // traps in _pthread_wqthread_setup with "BUG IN LIBPTHREAD: thread_set_tsd_base() wasn't called by
    // the kernel" -- bit 21 says the thread pointer is already live -- and a flags word without the
    // priority bit traps with "BUG IN LIBPTHREAD: Missing priority". The same measurement showed the
    // kernel passing keventlist = NULL and nkevents = 0, not the request entry.
    constexpr uint32_t MACOS_WQTHREAD_SPAWN_FLAGS = 0x244005;

    // Measured 2026-08-27 at start_wqthread on the host (cgsdemo under lldb): a workloop spawn carries
    // the workloop bit (22) plus the setup bits, and the kevent buffer lives 0x480 below the pthread
    // page. A wake re-enters with REUSE (bit 17) set and the QoS bits below bit 14 varying with the
    // queue; 0x4f4005 followed workloop-kqueue events, 0x1e4008 a process-workqueue one.
    constexpr uint32_t MACOS_WQTHREAD_WORKLOOP_SPAWN_FLAGS = 0x6C4004;
    constexpr uint32_t MACOS_WQTHREAD_WORKLOOP_WAKE_FLAGS = 0x4F4005;
    constexpr uint32_t MACOS_WQTHREAD_WORKQ_WAKE_FLAGS = 0x1E4008;

    // WQ_FLAG_THREAD_REUSE. A pool-parked worker handed back to a thread request re-enters with the
    // spawn contract plus this bit, because libpthread has already run its one-time setup on it.
    constexpr uint32_t MACOS_WQTHREAD_REUSE_FLAG = 0x20000;

    // WQ_FLAG_THREAD_PRIO_MASK / WQ_FLAG_THREAD_OVERCOMMIT, and the pthread_priority_t fields the
    // request carries them in.
    constexpr uint32_t MACOS_WQTHREAD_PRIO_MASK = 0x000000FFu;
    constexpr uint32_t MACOS_WQTHREAD_OVERCOMMIT_FLAG = 0x00010000u;
    constexpr uint32_t MACOS_PTHREAD_PRIORITY_QOS_MASK = 0x00FFFF00u;
    constexpr uint32_t MACOS_PTHREAD_PRIORITY_QOS_SHIFT = 8;
    constexpr uint32_t MACOS_PTHREAD_PRIORITY_OVERCOMMIT_FLAG = 0x80000000u;

    // A thread request names the root queue its worker must drain, and libpthread reads that off the
    // worker's own flags word -- so handing every worker one constant makes a request for any other QoS
    // arrive on the wrong queue, find nothing, and park. Measured 2026-08-28 on the host by breaking on
    // __workq_kernreturn and start_wqthread together:
    //
    //   background 0x2ff, utility 0x4ff, default/user-initiated 0x10ff, user-interactive 0x20ff
    //
    // The QoS field is 1 << (thread_qos - 1), and the worker's flags carry thread_qos itself in the low
    // byte: 0x10ff (1 << 4, so thread_qos 5) produced flags 0x244005.
    constexpr uint32_t macos_wqthread_flags_for_priority(const uint32_t pthread_priority)
    {
        const auto qos_bits = (pthread_priority & MACOS_PTHREAD_PRIORITY_QOS_MASK) >> MACOS_PTHREAD_PRIORITY_QOS_SHIFT;

        uint32_t thread_qos = 0;
        for (uint32_t bit = 0; bit < 16; ++bit)
        {
            if ((qos_bits >> bit) != 0)
            {
                thread_qos = bit + 1;
            }
        }

        auto flags = (MACOS_WQTHREAD_SPAWN_FLAGS & ~MACOS_WQTHREAD_PRIO_MASK) | (thread_qos & MACOS_WQTHREAD_PRIO_MASK);

        if ((pthread_priority & MACOS_PTHREAD_PRIORITY_OVERCOMMIT_FLAG) != 0)
        {
            flags |= MACOS_WQTHREAD_OVERCOMMIT_FLAG;
        }

        return flags;
    }

    constexpr uint64_t MACOS_WORKQUEUE_EVENT_BUFFER_OFFSET = 0x480;

    constexpr std::string_view MACOS_PTHREAD_IMAGE_PATH = "/usr/lib/system/libsystem_pthread.dylib";

    // xnu bsd/sys/ulock.h: the opcode is the low byte and everything above it is flags. A wider mask
    // folds ULF_WAKE_ALL into the opcode and makes an ordinary libdispatch broadcast unrecognisable.
    constexpr uint32_t MACOS_UL_OPCODE_MASK = 0x000000FFu;
    constexpr uint32_t MACOS_ULF_WAKE_ALL = 0x00000100u;
    constexpr uint32_t MACOS_ULF_WAKE_THREAD = 0x00000200u;

    // With this set the syscall reports failure by succeeding and handing back -errno in x0, rather
    // than a positive errno with the carry flag raised (xnu bsd/kern/sys_ulock.c). libplatform's
    // os_unfair_lock sets it on every call and reads the carry form as errno 1, which it aborts on.
    constexpr uint32_t MACOS_ULF_NO_ERRNO = 0x01000000u;

    // xnu bsd/sys/persona.h. A persona is a launchd-assigned identity; an ordinary process has none,
    // and each operation refuses that differently -- see sys_persona for the measured answers.
    constexpr uint32_t MACOS_PERSONA_OP_ALLOC = 1;
    constexpr uint32_t MACOS_PERSONA_OP_PALLOC = 2;
    constexpr uint32_t MACOS_PERSONA_OP_DEALLOC = 3;
    constexpr uint32_t MACOS_PERSONA_OP_GET = 4;
    constexpr uint32_t MACOS_PERSONA_OP_INFO = 5;
    constexpr uint32_t MACOS_PERSONA_OP_PIDINFO = 6;
    constexpr uint32_t MACOS_PERSONA_OP_FIND = 7;
    constexpr uint32_t MACOS_PERSONA_OP_SUPPORT = 8;
    constexpr uint32_t MACOS_PERSONA_OP_FIND_BY_TYPE = 9;

    constexpr uint32_t MACOS_UL_COMPARE_AND_WAIT = 1;
    constexpr uint32_t MACOS_UL_UNFAIR_LOCK = 2;
    constexpr uint32_t MACOS_UL_COMPARE_AND_WAIT_SHARED = 3;

    // libpthread's psynch words (xnu bsd/sys/pthread_shims.h, libpthread kern/kern_internal.h). Every
    // mutex and condition variable sequence word keeps a 24-bit counter in its top bits and state in the
    // low byte, and the kernel answers in the same encoding: libpthread compares the answer against the
    // word it holds and spins or aborts on anything else. Measured against this host's libpthread on
    // 2026-08-28 -- a contended pthread_mutex_lock is handed (mgen & COUNT_MASK) | EBIT | KBIT, and a
    // pthread_cond_signal is answered (waiters * PTHRW_INC) | CBIT.
    constexpr uint32_t MACOS_PTHRW_INC = 0x00000100u;
    constexpr uint32_t MACOS_PTHRW_COUNT_MASK = 0xFFFFFF00u;

    constexpr uint32_t MACOS_PTH_RWL_KBIT = 0x00000001u;
    constexpr uint32_t MACOS_PTH_RWL_EBIT = 0x00000002u;

    constexpr uint32_t MACOS_PTH_RWS_CV_CBIT = 0x00000001u;
    constexpr uint32_t MACOS_PTH_RWS_CV_PBIT = 0x00000002u;

    constexpr std::string_view MACOS_SKYLIGHT_IMAGE_PATH = "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight";

    constexpr std::string_view MACOS_CORE_GRAPHICS_IMAGE_PATH = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";

    constexpr std::string_view MACOS_QUARTZ_CORE_IMAGE_PATH = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";

    constexpr std::string_view MACOS_APP_KIT_IMAGE_PATH = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
    constexpr std::string_view MACOS_CORE_FOUNDATION_IMAGE_PATH =
        "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

    // The Process Manager surface AppKit walks before it opens a window. It lives in HIServices and
    // talks to coreservicesd over MIG rather than to the window server.
    constexpr std::string_view MACOS_HI_SERVICES_IMAGE_PATH =
        "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/HIServices.framework/Versions/A/HIServices";

    constexpr std::string_view MACOS_NULL_DEVICE_PATH = "/dev/null";
    constexpr std::string_view MACOS_DYLD_GUEST_PATH = "/usr/lib/dyld";
    constexpr std::string_view MACOS_DYLD_CACHE_GUEST_DIR = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld";
    constexpr std::string_view MACOS_DYLD_CACHE_GUEST_PATH =
        "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e";
    constexpr std::string_view MACOS_DYLD_CACHE_CLASSIC_GUEST_PATH = "/System/Library/dyld/dyld_shared_cache_arm64e";
    constexpr std::string_view MACOS_DYLD_ALL_IMAGE_INFO_SECTION = "__all_image_info";

    constexpr uint64_t MACOS_PATH_MAX = 1024;

    constexpr uint64_t MACOS_PAGEZERO_END = 0x100000000ULL;
    constexpr uint64_t MACOS_EXECUTABLE_BASE = 0x100000000ULL;

    constexpr uint64_t MACOS_MAIN_STACK_TOP = 0x16FC00000ULL;
    constexpr uint64_t MACOS_MAIN_STACK_SIZE = 0x800000ULL;

    // libpthread reads TPIDRRO_EL0 and takes its own struct to start 0xE0 below it:
    //   mrs x8, TPIDRRO_EL0 ; subs x21, x8, #224 ; str x8, [x21]
    // so the thread pointer has to name real memory with room beneath it, not a token. The thread's
    // TSD slots live above the pointer, which is why the page extends both ways.
    constexpr uint64_t MACOS_MAIN_THREAD_STATE_BASE = MACOS_MAIN_STACK_TOP - MACOS_MAIN_STACK_SIZE - MACOS_PAGE_SIZE;
    constexpr uint64_t MACOS_MAIN_THREAD_STATE_SIZE = MACOS_PAGE_SIZE;
    constexpr uint64_t MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET = 0xE0;

    // _PTHREAD_TSD_SLOT_MACH_THREAD_SELF. The kernel writes the new thread's port here before it ever
    // runs -- _pthread_start reads it back out of the struct, not out of the register it was also handed
    // it in, and calls abort_with_reason("Unable to allocate thread port, possible port leak") when it
    // is null. Measured 2026-08-28: the load is `ldr w8, [x19, #0xf8]`, which is this slot.
    constexpr uint64_t MACOS_PTHREAD_TSD_SLOT_MACH_THREAD_SELF = 3;

    // bsdthread_create flags. CUSTOM says the caller allocated the stack and the pthread struct itself;
    // TSD_BASE_SET tells _pthread_start the thread pointer is already live, which it is once the thread's
    // saved tpidrro_el0 is set, so it does not go back to the kernel for it.
    constexpr uint32_t MACOS_PTHREAD_START_CUSTOM = 0x01000000;
    constexpr uint32_t MACOS_PTHREAD_START_TSD_BASE_SET = 0x10000000;

    constexpr uint64_t MACOS_SHARED_CACHE_BASE = 0x180000000ULL;
    constexpr uint64_t MACOS_SHARED_CACHE_END = 0x2E6440000ULL;

    constexpr uint64_t MACOS_DEFAULT_MMAP_BASE = 0x300000000ULL;

    constexpr uint64_t MACOS_COMMPAGE_NESTING_START = 0xFC0000000ULL;
    constexpr uint64_t MACOS_COMMPAGE_NESTING_SIZE = 0x40000000ULL;

    constexpr uint64_t MACOS_MAX_MMAP_END_EXCL = MACOS_COMMPAGE_NESTING_START;

    constexpr uint64_t MACOS_COMMPAGE_BASE = 0xFFFFFC000ULL;
    constexpr uint64_t MACOS_COMMPAGE_RO_BASE = 0xFFFFF4000ULL;
    constexpr uint64_t MACOS_COMMPAGE_MAP_SIZE = MACOS_PAGE_SIZE;
    constexpr uint64_t MACOS_COMMPAGE_FIELD_LENGTH = 0x1000ULL;

    namespace macos_errno
    {
        constexpr int64_t MACOS_EPERM = 1;
        constexpr int64_t MACOS_ENOENT = 2;
        constexpr int64_t MACOS_ESRCH = 3;
        constexpr int64_t MACOS_EINTR = 4;
        constexpr int64_t MACOS_EIO = 5;
        constexpr int64_t MACOS_ENXIO = 6;
        constexpr int64_t MACOS_E2BIG = 7;
        constexpr int64_t MACOS_ENOEXEC = 8;
        constexpr int64_t MACOS_EBADF = 9;
        constexpr int64_t MACOS_ECHILD = 10;
        constexpr int64_t MACOS_EDEADLK = 11;
        constexpr int64_t MACOS_ENOMEM = 12;
        constexpr int64_t MACOS_EACCES = 13;
        constexpr int64_t MACOS_EFAULT = 14;
        constexpr int64_t MACOS_EBUSY = 16;
        constexpr int64_t MACOS_EEXIST = 17;
        constexpr int64_t MACOS_EXDEV = 18;
        constexpr int64_t MACOS_ENODEV = 19;
        constexpr int64_t MACOS_ENOTDIR = 20;
        constexpr int64_t MACOS_EISDIR = 21;
        constexpr int64_t MACOS_EINVAL = 22;
        constexpr int64_t MACOS_ENFILE = 23;
        constexpr int64_t MACOS_EMFILE = 24;
        constexpr int64_t MACOS_ENOTTY = 25;
        constexpr int64_t MACOS_EFBIG = 27;
        constexpr int64_t MACOS_ENOSPC = 28;
        constexpr int64_t MACOS_ESPIPE = 29;
        constexpr int64_t MACOS_EROFS = 30;
        constexpr int64_t MACOS_EMLINK = 31;
        constexpr int64_t MACOS_EPIPE = 32;
        constexpr int64_t MACOS_EDOM = 33;
        constexpr int64_t MACOS_ERANGE = 34;
        constexpr int64_t MACOS_EAGAIN = 35;
        constexpr int64_t MACOS_EWOULDBLOCK = MACOS_EAGAIN;
        constexpr int64_t MACOS_EINPROGRESS = 36;
        constexpr int64_t MACOS_ENOTSOCK = 38;
        constexpr int64_t MACOS_ENOTSUP = 45;
        constexpr int64_t MACOS_EAFNOSUPPORT = 47;
        constexpr int64_t MACOS_EADDRINUSE = 48;
        constexpr int64_t MACOS_ETIMEDOUT = 60;
        constexpr int64_t MACOS_ECONNREFUSED = 61;
        constexpr int64_t MACOS_ELOOP = 62;
        constexpr int64_t MACOS_ENAMETOOLONG = 63;
        constexpr int64_t MACOS_ENOTEMPTY = 66;
        constexpr int64_t MACOS_ENOSYS = 78;
        constexpr int64_t MACOS_EOVERFLOW = 84;
        constexpr int64_t MACOS_EOPNOTSUPP = 102;
    }

    namespace macos_open
    {
        constexpr int32_t MACOS_O_RDONLY = 0x0;
        constexpr int32_t MACOS_O_WRONLY = 0x1;
        constexpr int32_t MACOS_O_RDWR = 0x2;
        constexpr int32_t MACOS_O_ACCMODE = 0x3;
        constexpr int32_t MACOS_O_NONBLOCK = 0x4;
        constexpr int32_t MACOS_O_APPEND = 0x8;
        constexpr int32_t MACOS_O_NOFOLLOW = 0x100;
        constexpr int32_t MACOS_O_CREAT = 0x200;
        constexpr int32_t MACOS_O_TRUNC = 0x400;
        constexpr int32_t MACOS_O_EXCL = 0x800;
        constexpr int32_t MACOS_O_DIRECTORY = 0x100000;
        constexpr int32_t MACOS_O_SYMLINK = 0x200000;
        constexpr int32_t MACOS_O_CLOEXEC = 0x1000000;
        constexpr int32_t MACOS_AT_FDCWD = -2;
        constexpr int32_t MACOS_AT_SYMLINK_NOFOLLOW = 0x0020;

        constexpr int32_t MACOS_F_OK = 0;
        constexpr int32_t MACOS_X_OK = 0x1;
        constexpr int32_t MACOS_W_OK = 0x2;
        constexpr int32_t MACOS_R_OK = 0x4;
    }

    // xnu bsd/sys/guarded.h. guarded_open_np pins a caller-chosen id to a descriptor together with the
    // operations that may then only be performed by naming the id back; reaching for one of them through
    // the ordinary syscall is what xnu turns into a fatal EXC_GUARD_FD.
    //
    // Measured against this host's kernel on 2026-08-31 rather than assumed, because the header is
    // private and the bit order is not the one the flag names suggest: an open with guardflags 0x02 and
    // no other bit succeeds, 0x01 alone is EINVAL, and 0x20 is EINVAL. kern_guarded.c spends no bit on
    // GUARD_REQUIRED -- it defines it as GUARD_DUP, so every guarded descriptor is dup-guarded whether
    // the caller meant it or not -- and rejects any guardflags outside the five below.
    namespace macos_guard
    {
        constexpr uint32_t MACOS_GUARD_CLOSE = 1u << 0;
        constexpr uint32_t MACOS_GUARD_DUP = 1u << 1;
        constexpr uint32_t MACOS_GUARD_SOCKET_IPC = 1u << 2;
        constexpr uint32_t MACOS_GUARD_FILEPORT = 1u << 3;
        constexpr uint32_t MACOS_GUARD_WRITE = 1u << 4;

        constexpr uint32_t MACOS_GUARD_REQUIRED = MACOS_GUARD_DUP;
        constexpr uint32_t MACOS_GUARD_ALL =
            MACOS_GUARD_CLOSE | MACOS_GUARD_DUP | MACOS_GUARD_SOCKET_IPC | MACOS_GUARD_FILEPORT | MACOS_GUARD_WRITE;
    }

    namespace macos_mmap
    {
        constexpr int32_t MACOS_PROT_NONE = 0x0;
        constexpr int32_t MACOS_PROT_READ = 0x1;
        constexpr int32_t MACOS_PROT_WRITE = 0x2;
        constexpr int32_t MACOS_PROT_EXEC = 0x4;

        constexpr int32_t MACOS_MAP_SHARED = 0x1;
        constexpr int32_t MACOS_MAP_PRIVATE = 0x2;
        constexpr int32_t MACOS_MAP_FIXED = 0x10;
        constexpr int32_t MACOS_MAP_NORESERVE = 0x40;
        constexpr int32_t MACOS_MAP_JIT = 0x800;
        constexpr int32_t MACOS_MAP_ANON = 0x1000;

        constexpr int32_t MACOS_MADV_NORMAL = 0;
        constexpr int32_t MACOS_MADV_DONTNEED = 4;
        constexpr int32_t MACOS_MADV_FREE = 5;
        constexpr int32_t MACOS_MADV_FREE_REUSABLE = 7;
        constexpr int32_t MACOS_MADV_FREE_REUSE = 8;

        // The one advice with observable semantics. libmalloc uses it in place of a memset on the
        // calloc path for page-sized blocks, so an emulator that reports success without zeroing hands
        // every large calloc the previous owner's bytes -- measured, and it is what made CoreFoundation
        // release a pointer it read out of a hash bucket that was never cleared.
        constexpr int32_t MACOS_MADV_ZERO = 11;
    }

    namespace macos_fcntl
    {
        constexpr int32_t MACOS_F_DUPFD = 0;
        constexpr int32_t MACOS_F_GETFD = 1;
        constexpr int32_t MACOS_F_SETFD = 2;
        constexpr int32_t MACOS_F_GETFL = 3;
        constexpr int32_t MACOS_F_SETFL = 4;
        constexpr int32_t MACOS_F_GETLK = 7;
        constexpr int32_t MACOS_F_SETLK = 8;
        constexpr int32_t MACOS_F_SETLKW = 9;
        constexpr int32_t MACOS_F_GETPATH = 50;
        constexpr int32_t MACOS_F_DUPFD_CLOEXEC = 67;

        // The open-file-description locks. They differ from the three above only in what owns the lock:
        // the description rather than the process, so closing any other descriptor for the same file
        // does not drop them. libsqlite3 takes these on a database and on its .store-conch.
        constexpr int32_t MACOS_F_OFD_SETLK = 90;
        constexpr int32_t MACOS_F_OFD_SETLKW = 91;
        constexpr int32_t MACOS_F_OFD_GETLK = 92;

        constexpr int16_t MACOS_F_RDLCK = 1;
        constexpr int16_t MACOS_F_UNLCK = 2;
        constexpr int16_t MACOS_F_WRLCK = 3;
        constexpr int32_t MACOS_F_ADDFILESIGS_RETURN = 97;
        constexpr int32_t MACOS_F_CHECK_LV = 98;
        constexpr int32_t MACOS_F_GETPATH_NOFIRMLINK = 102;
        constexpr int32_t MACOS_FD_CLOEXEC = 1;

        constexpr int32_t MACOS_SEEK_SET = 0;
        constexpr int32_t MACOS_SEEK_CUR = 1;
        constexpr int32_t MACOS_SEEK_END = 2;
    }

    // Darwin puts the two shorts after the pid rather than ahead of the offsets the way Linux orders
    // them, so a Linux struct flock decoded here reads the length as the type. Measured on this host.
    struct macos_flock
    {
        int64_t l_start;
        int64_t l_len;
        int32_t l_pid;
        int16_t l_type;
        int16_t l_whence;
    };

    static_assert(sizeof(macos_flock) == 24, "Darwin's struct flock is three guest words");

    struct macos_iovec
    {
        uint64_t iov_base;
        uint64_t iov_len;
    };

    static_assert(sizeof(macos_iovec) == 16, "the 64-bit Darwin struct iovec is two guest words");

    // Darwin's suseconds_t stays 32-bit on arm64 while time_t widens to 64, so the trailing padding is
    // part of the ABI rather than an artefact of this declaration.
    struct macos_timeval
    {
        int64_t tv_sec;
        int32_t tv_usec;
        int32_t tv_pad;
    };

    static_assert(sizeof(macos_timeval) == 16, "the 64-bit Darwin struct timeval is two guest words");

    namespace macos_stat_mode
    {
        constexpr uint16_t MACOS_S_IFMT = 0xF000;
        constexpr uint16_t MACOS_S_IFIFO = 0x1000;
        constexpr uint16_t MACOS_S_IFCHR = 0x2000;
        constexpr uint16_t MACOS_S_IFDIR = 0x4000;
        constexpr uint16_t MACOS_S_IFBLK = 0x6000;
        constexpr uint16_t MACOS_S_IFREG = 0x8000;
        constexpr uint16_t MACOS_S_IFLNK = 0xA000;
        constexpr uint16_t MACOS_S_IFSOCK = 0xC000;
        constexpr uint16_t MACOS_S_IPERM = 0x0FFF;
    }

    namespace macos_dirent_type
    {
        constexpr uint8_t MACOS_DT_UNKNOWN = 0;
        constexpr uint8_t MACOS_DT_FIFO = 1;
        constexpr uint8_t MACOS_DT_CHR = 2;
        constexpr uint8_t MACOS_DT_DIR = 4;
        constexpr uint8_t MACOS_DT_BLK = 6;
        constexpr uint8_t MACOS_DT_REG = 8;
        constexpr uint8_t MACOS_DT_LNK = 10;
        constexpr uint8_t MACOS_DT_SOCK = 12;
    }

    namespace macos_sysctl_mib
    {
        constexpr int32_t MACOS_CTL_KERN = 1;
        constexpr int32_t MACOS_CTL_VM = 2;
        constexpr int32_t MACOS_CTL_VFS = 3;
        constexpr int32_t MACOS_CTL_NET = 4;
        constexpr int32_t MACOS_CTL_HW = 6;
        constexpr int32_t MACOS_CTL_MACHDEP = 7;
        constexpr int32_t MACOS_CTL_USER = 8;

        constexpr int32_t MACOS_KERN_OSTYPE = 1;
        constexpr int32_t MACOS_KERN_OSRELEASE = 2;
        constexpr int32_t MACOS_KERN_OSREV = 3;
        constexpr int32_t MACOS_KERN_VERSION = 4;
        constexpr int32_t MACOS_KERN_ARGMAX = 8;
        constexpr int32_t MACOS_KERN_HOSTNAME = 10;
        constexpr int32_t MACOS_KERN_PROC = 14;
        constexpr int32_t MACOS_KERN_PROCARGS2 = 49;
        constexpr int32_t MACOS_KERN_USRSTACK64 = 59;
        constexpr int32_t MACOS_KERN_OSVERSION = 65;

        constexpr int32_t MACOS_HW_MACHINE = 1;
        constexpr int32_t MACOS_HW_MODEL = 2;
        constexpr int32_t MACOS_HW_NCPU = 3;
        constexpr int32_t MACOS_HW_BYTEORDER = 4;
        constexpr int32_t MACOS_HW_PAGESIZE = 7;
        constexpr int32_t MACOS_HW_CACHELINE = 16;
        constexpr int32_t MACOS_HW_MEMSIZE = 24;
        constexpr int32_t MACOS_HW_AVAILCPU = 25;
    }

    // One conversion for every Darwin PROT_* to permission mapping: the BSD mmap/mprotect syscalls and
    // the mach_vm traps and MIG routines all grant memory, so they must not be able to disagree.
    constexpr memory_permission macos_prot_to_permission(const int32_t protection)
    {
        auto permissions = memory_permission::none;

        if ((protection & macos_mmap::MACOS_PROT_READ) != 0)
        {
            permissions |= memory_permission::read;
        }

        if ((protection & macos_mmap::MACOS_PROT_WRITE) != 0)
        {
            permissions |= memory_permission::write;
        }

        if ((protection & macos_mmap::MACOS_PROT_EXEC) != 0)
        {
            permissions |= memory_permission::exec;
        }

        return permissions;
    }
} // namespace sogen

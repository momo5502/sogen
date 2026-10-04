#include "../std_include.hpp"
#include "../macos_emulator.hpp"
#include "../macos_syscall_utils.hpp"
#include "../mach/mach_exception.hpp"

#include <guest/guest_memory_object.hpp>

#include <cstring>

#include <cinttypes>
#include <cstdio>

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <set>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_errno;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        constexpr uint64_t MACOS_GETENTROPY_MAX_LENGTH = 256;
        constexpr uint64_t MACOS_ENTROPY_SEED = 0x9E3779B97F4A7C15ULL;

        // What a thread is waiting for, for the deadlock reports. Empty for a thread that is not parked:
        // only the caller knows whether that means runnable or "the thread reporting this deadlock".
        std::string describe_thread_park(const macos_thread& thread)
        {
            std::array<char, 128> state{};

            if (thread.workqueue_kport != 0 && thread.blocked_on_port == thread.workqueue_kport)
            {
                std::snprintf(state.data(), state.size(), " parked in the workqueue pool (kport 0x%x)", thread.workqueue_kport);
            }
            else if (thread.blocked_on_port != 0)
            {
                std::snprintf(state.data(), state.size(), " parked in a mach receive on port 0x%x", thread.blocked_on_port);
            }
            else if (thread.blocked_on_ulock != 0)
            {
                std::snprintf(state.data(), state.size(), " parked in ulock_wait on 0x%" PRIx64, thread.blocked_on_ulock);
            }
            else if (thread.blocked_on_sem != 0)
            {
                std::snprintf(state.data(), state.size(), " parked in __semwait_signal on semaphore 0x%x", thread.blocked_on_sem);
            }
            else if (thread.blocked_on_psynch_mutex != 0)
            {
                std::snprintf(state.data(), state.size(), " parked in psynch_mutexwait on mutex 0x%" PRIx64,
                              thread.blocked_on_psynch_mutex);
            }
            else if (thread.blocked_on_psynch_cv != 0)
            {
                std::snprintf(state.data(), state.size(), " parked in psynch_cvwait on condition variable 0x%" PRIx64,
                              thread.blocked_on_psynch_cv);
            }
            else
            {
                return {};
            }

            return state.data();
        }

        std::string describe_every_thread_park(const macos_syscall_context& c, const uint64_t reporting_thread)
        {
            std::string detail{};

            for (const auto& [id, thread] : c.proc.threads)
            {
                if (thread.terminated)
                {
                    continue;
                }

                auto state = describe_thread_park(thread);
                if (state.empty())
                {
                    state = id == reporting_thread ? " the thread reporting this deadlock" : " runnable";
                }

                detail += "\n    thread " + std::to_string(id) + state;
            }

            return detail;
        }
    }

    void sys_exit(const macos_syscall_context& c)
    {
        const auto status = static_cast<int>(static_cast<int32_t>(get_macos_syscall_argument(c, 0)));

        if (c.proc.active_thread != nullptr)
        {
            c.proc.active_thread->terminated = true;
            c.proc.active_thread->exit_code = status;
        }

        c.proc.exit_status = status;
        c.emu_ref.callbacks.on_process_exit(status);
        c.emu_ref.record_stop(stop_reason::normal_exit, std::to_string(status));
        c.emu_ref.stop();
    }

    namespace
    {
        std::string read_payload_reason(const macos_syscall_context& c, const uint64_t address)
        {
            if (address == 0)
            {
                return {};
            }

            std::string reason{};
            for (uint64_t i = 0; i < MACOS_PATH_MAX; ++i)
            {
                char character{};
                if (!c.emu_ref.memory.try_read_memory(address + i, &character, sizeof(character)) || character == '\0')
                {
                    break;
                }

                reason.push_back(character);
            }

            return reason;
        }

        // libxpc, libmalloc and libobjc all report a fatal initialisation error this way rather than
        // through a signal, so the reason string is usually the only diagnosis of a missing mach entry
        // point that the guest ever produces.
        void terminate_with_payload(const macos_syscall_context& c, const bool abort)
        {
            const auto reason_namespace = static_cast<uint32_t>(get_macos_syscall_argument(c, abort ? 0 : 1));
            const auto reason_code = get_macos_syscall_argument(c, abort ? 1 : 2);
            const auto reason_string = get_macos_syscall_argument(c, abort ? 4 : 5);

            auto detail = std::string{abort ? "abort_with_payload" : "terminate_with_payload"} +
                          " namespace=" + std::to_string(reason_namespace) + " code=" + std::to_string(reason_code);

            if (const auto reason = read_payload_reason(c, reason_string); !reason.empty())
            {
                detail += " reason=\"" + reason + "\"";
            }

            // EXC_CRASH carries the os_reason pair, namespace first, in the two exception code slots.
            const auto exception_code = static_cast<uint64_t>(reason_namespace);
            const auto exception_subcode = reason_code;
            const auto raised = mach::raise_guest_exception(c.emu_ref, mach::exception_type::crash, exception_code, exception_subcode);
            detail += " (" + std::string{mach::signal_name(raised.signal)} + ")";

            if (c.proc.active_thread != nullptr)
            {
                c.proc.active_thread->terminated = true;
                c.proc.active_thread->exit_code = raised.signal;
            }

            c.proc.exit_status = raised.signal;
            c.emu_ref.record_stop(stop_reason::signal_termination, std::move(detail));
            c.emu_ref.stop();
        }
    }

    void sys_abort_with_payload(const macos_syscall_context& c)
    {
        terminate_with_payload(c, true);
    }

    void sys_terminate_with_payload(const macos_syscall_context& c)
    {
        terminate_with_payload(c, false);
    }

    // dyld blocks and restores the signal mask around its own initialisation. Nothing in sogen delivers
    // a signal, so the mask is state the guest owns and reads back rather than something the kernel
    // acts on; oldset has to be filled or the restore writes back garbage.
    void sys_sigprocmask(const macos_syscall_context& c)
    {
        const auto how = static_cast<int32_t>(get_macos_syscall_argument(c, 0));
        const auto set = get_macos_syscall_argument(c, 1);
        const auto oldset = get_macos_syscall_argument(c, 2);

        constexpr int32_t MACOS_SIG_BLOCK = 1;
        constexpr int32_t MACOS_SIG_UNBLOCK = 2;
        constexpr int32_t MACOS_SIG_SETMASK = 3;

        if (oldset != 0 && !c.emu_ref.memory.try_write_memory(oldset, &c.proc.signal_mask, sizeof(c.proc.signal_mask)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        if (set == 0)
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        uint32_t requested{};
        if (!c.emu_ref.memory.try_read_memory(set, &requested, sizeof(requested)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        switch (how)
        {
        case MACOS_SIG_BLOCK:
            c.proc.signal_mask |= requested;
            break;
        case MACOS_SIG_UNBLOCK:
            c.proc.signal_mask &= ~requested;
            break;
        case MACOS_SIG_SETMASK:
            c.proc.signal_mask = requested;
            break;
        default:
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // libpthread's initialisation calls this once. The command is named by value rather than by a
    // constant from a header, because the enumeration lives in xnu's private pthread headers and is not
    // in the SDK: 0x1000 arrives with a boolean and asks whether the workqueue may kill idle threads.
    // Nothing here has a workqueue, so there is nothing to permit or refuse.
    //
    // An unimplemented command inside an implemented syscall would be silent, which is exactly how a
    // missing MIG routine stayed hidden until it burned the guest's stack. Unknown commands are named.
    void sys_bsdthread_ctl(const macos_syscall_context& c)
    {
        constexpr uint64_t MACOS_BSDTHREAD_CTL_QOS_OVERRIDE_START = 0x20;
        constexpr uint64_t MACOS_BSDTHREAD_CTL_QOS_OVERRIDE_END = 0x40;
        constexpr uint64_t MACOS_BSDTHREAD_CTL_QOS_OVERRIDE_RESET = 0x800;
        constexpr uint64_t MACOS_BSDTHREAD_CTL_SET_SELF = 0x100;
        constexpr uint64_t MACOS_BSDTHREAD_CTL_WORKQ_ALLOW_KILL = 0x1000;

        const auto command = get_macos_syscall_argument(c, 0);

        if (command == MACOS_BSDTHREAD_CTL_WORKQ_ALLOW_KILL)
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        // A QoS override raises another thread's priority for as long as this one waits on it -- priority
        // inheritance across a lock. sogen's scheduler is not priority-ordered, so an override changes
        // nothing it can express; the calls still have to succeed, because libdispatch pairs a start
        // with an end and treats a failed start as a reason to abandon the work item.
        if (command == MACOS_BSDTHREAD_CTL_QOS_OVERRIDE_START || command == MACOS_BSDTHREAD_CTL_QOS_OVERRIDE_END ||
            command == MACOS_BSDTHREAD_CTL_QOS_OVERRIDE_RESET)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                c.emu_ref.log.info("bsdthread_ctl QoS overrides are accepted but not applied: sogen's scheduler is not "
                                   "priority-ordered\n");
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        // BSDTHREAD_CTL_SET_SELF (xnu bsd/kern/pthread_shims.c): the calling thread's own QoS class,
        // voucher and fixed-priority bit, which libdispatch sets before and after every work item.
        // Nothing here schedules by priority and vouchers are not per-thread state, so the values are
        // recorded rather than applied -- but the call has to succeed: _dispatch_set_priority_and_voucher
        // treats a failure as fatal to the item it was about to run.
        if (command == MACOS_BSDTHREAD_CTL_SET_SELF)
        {
            if (auto* thread = c.proc.active_thread; thread != nullptr)
            {
                thread->pthread_priority = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));
            }

            static bool reported = false;
            if (!reported)
            {
                reported = true;
                c.emu_ref.log.info("bsdthread_ctl SET_SELF is recorded but not applied: sogen's scheduler is not priority-ordered\n");
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        c.emu_ref.log.warn("unimplemented bsdthread_ctl command 0x%" PRIx64 "\n", command);
        write_macos_syscall_error(c, MACOS_ENOTSUP);
    }

    // A guest that kills itself with SIGABRT has hit an assertion or a failed initialiser, and it writes
    // nothing anywhere before doing it. The backtrace is the only account of what happened.
    void report_guest_abort(const macos_syscall_context& c, const uint32_t signal_number)
    {
        constexpr uint32_t abort_signal = 6;
        if (signal_number != abort_signal)
        {
            return;
        }

        c.emu_ref.log.warn("guest raised SIGABRT; backtrace:\n");
        for (const auto& frame : c.emu_ref.backtrace())
        {
            c.emu_ref.log.warn("    %s\n", frame.c_str());
        }
    }

    // Nothing here delivers signals, so a thread that signals itself can only be terminated. That is
    // what actually happens for the fatal ones anyway: abort() reaches the kernel this way, and a guest
    // that gets a success return instead would carry on past its own abort.
    void sys_disable_threadsignal(const macos_syscall_context& c)
    {
        if (c.proc.active_thread != nullptr)
        {
            c.proc.active_thread->signals_disabled = true;
        }

        // Measured on 25G76: every argument value answers 0, on a worker thread and on the main thread
        // alike. The argument selects nothing a caller can observe from the return.
        write_macos_syscall_result(c, 0);
    }

    void sys_pthread_kill(const macos_syscall_context& c)
    {
        const auto port = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto signal = static_cast<int32_t>(get_macos_syscall_argument(c, 1));

        // A thread that disabled its signals stops being a target: the existence probe fails the same
        // way a delivery does, so a caller cannot tell the thread is still running.
        const auto object = c.emu_ref.mach.ports.object_of(port);
        if (object.kind == mach::kernel_object_kind::thread)
        {
            const auto target = c.proc.threads.find(object.id);
            if (target != c.proc.threads.end() && target->second.signals_disabled)
            {
                write_macos_syscall_error(c, MACOS_ESRCH);
                return;
            }
        }

        // Signal 0 asks whether the thread exists rather than delivering anything.
        if (signal == 0)
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        if (signal < 0 || signal > 31)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        report_guest_abort(c, static_cast<uint32_t>(signal));

        const auto raised = mach::raise_guest_exception(c.emu_ref, mach::exception_type::software, static_cast<uint64_t>(signal), port);

        if (c.proc.active_thread != nullptr)
        {
            c.proc.active_thread->terminated = true;
            c.proc.active_thread->exit_code = signal;
        }

        c.proc.exit_status = signal;
        c.emu_ref.record_stop(stop_reason::signal_termination, "the guest signalled its own thread: signal " + std::to_string(signal) +
                                                                   " (" + std::string{mach::signal_name(signal)} +
                                                                   "), port=" + std::to_string(port) + ", raised as " +
                                                                   std::string{mach::exception_type_name(raised.type)});
        c.emu_ref.stop();
    }

    namespace
    {
        // The values a process actually sees on macOS 26, read back from getrlimit on this host rather
        // than invented. RLIM_INFINITY is what most of them report; the two that matter to a start-up are
        // the stack, which libsystem_c uses to place its guard page, and the descriptor count, which
        // libmalloc and libdispatch size tables from.
        constexpr uint64_t MACOS_RLIM_INFINITY = 0x7FFFFFFFFFFFFFFFULL;

        struct macos_rlimit
        {
            uint64_t current{};
            uint64_t maximum{};
        };

        std::optional<macos_rlimit> limit_for(const uint64_t resource)
        {
            switch (resource)
            {
            case 0: // RLIMIT_CPU
            case 1: // RLIMIT_FSIZE
            case 2: // RLIMIT_DATA
            case 4: // RLIMIT_CORE
            case 5: // RLIMIT_AS
            case 6: // RLIMIT_MEMLOCK
                return macos_rlimit{.current = MACOS_RLIM_INFINITY, .maximum = MACOS_RLIM_INFINITY};

            case 3: // RLIMIT_STACK
                return macos_rlimit{.current = MACOS_MAIN_STACK_SIZE, .maximum = MACOS_MAIN_STACK_SIZE};

            case 7: // RLIMIT_NPROC
                return macos_rlimit{.current = 2666, .maximum = 4000};

            case 8: // RLIMIT_NOFILE
                return macos_rlimit{.current = 256, .maximum = MACOS_MAX_OPEN_DESCRIPTORS};

            default:
                return std::nullopt;
            }
        }
    }

    void sys_getrlimit(const macos_syscall_context& c)
    {
        const auto resource = get_macos_syscall_argument(c, 0);
        const auto destination = get_macos_syscall_argument(c, 1);

        const auto limit = limit_for(resource);
        if (!limit)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const guest_object<macos_rlimit> out{c.emu_ref.memory, destination};
        if (!out.try_write(*limit))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // Accepted and discarded. A process lowering its own limits is describing itself to the kernel, not
    // asking the emulator for anything, and reporting failure would make a guest that checks the result
    // abort a start-up it could otherwise complete.
    void sys_setrlimit(const macos_syscall_context& c)
    {
        const auto resource = get_macos_syscall_argument(c, 0);

        if (!limit_for(resource))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    namespace
    {
#pragma pack(push, 1)

        // auditinfo_addr_t (bsm/audit.h). The terminal address is left zero: this process has no
        // controlling terminal and none of its callers reads it, but the session id is not optional --
        // libsystem_c uses it to decide which session the process belongs to.
        struct macos_auditinfo_addr
        {
            uint32_t auid{};
            uint32_t mask_success{};
            uint32_t mask_failure{};
            uint32_t termid_port{};
            uint32_t termid_type{};
            uint32_t termid_addr[4]{};
            uint32_t asid{};
            uint64_t flags{};
        };

#pragma pack(pop)

        static_assert(sizeof(macos_auditinfo_addr) == 48);
    }

    // gettid(uid_t *uidp, gid_t *gidp), which libsystem also exposes as pthread_getugid_np. Two 4-byte
    // values and nothing more: CoreFoundation calls it with two adjacent stack slots, so anything larger
    // written here lands on the caller's frame and takes the stack canary with it.
    void sys_gettid(const macos_syscall_context& c)
    {
        const auto uid_pointer = get_macos_syscall_argument(c, 0);
        const auto gid_pointer = get_macos_syscall_argument(c, 1);

        if (uid_pointer != 0)
        {
            const guest_object<uint32_t> out{c.emu_ref.memory, uid_pointer};
            if (!out.try_write(c.proc.uid))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }
        }

        if (gid_pointer != 0)
        {
            const guest_object<uint32_t> out{c.emu_ref.memory, gid_pointer};
            if (!out.try_write(c.proc.gid))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_getaudit_addr(const macos_syscall_context& c)
    {
        const auto destination = get_macos_syscall_argument(c, 0);
        const auto length = get_macos_syscall_argument(c, 1);

        if (length < sizeof(macos_auditinfo_addr))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        // The same session task_audit_token reports. Two answers that disagreed would let a guest catch
        // the emulator contradicting itself about who it is.
        const macos_auditinfo_addr info{
            .auid = c.proc.uid,
            .asid = c.proc.audit_session_id,
        };

        const guest_object<macos_auditinfo_addr> out{c.emu_ref.memory, destination};
        if (!out.try_write(info))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    namespace
    {
#pragma pack(push, 1)

        // What the syscall takes, which is not what <signal.h> declares: the user-space struct sigaction
        // has no trampoline, and libsystem_c inserts one before making the call.
        struct macos_user_sigaction
        {
            uint64_t handler{};
            uint64_t trampoline{};
            uint32_t mask{};
            uint32_t flags{};
        };

        // What comes back, which is the user-space shape.
        struct macos_old_sigaction
        {
            uint64_t handler{};
            uint32_t mask{};
            uint32_t flags{};
        };

#pragma pack(pop)

        static_assert(sizeof(macos_user_sigaction) == 24);
        static_assert(sizeof(macos_old_sigaction) == 16);

        constexpr uint32_t MACOS_NSIG = 32;
    }

    void sys_sigaction(const macos_syscall_context& c)
    {
        const auto signal_number = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto incoming = get_macos_syscall_argument(c, 1);
        const auto outgoing = get_macos_syscall_argument(c, 2);

        // SIGKILL and SIGSTOP are 9 and 17 and cannot be caught, which a guest may be checking for.
        if (signal_number == 0 || signal_number >= MACOS_NSIG || signal_number == 9 || signal_number == 17)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto existing = c.proc.signal_actions.find(signal_number);

        if (outgoing != 0)
        {
            const macos_old_sigaction previous{
                .handler = existing == c.proc.signal_actions.end() ? 0 : existing->second.handler,
                .mask = existing == c.proc.signal_actions.end() ? 0 : existing->second.mask,
                .flags = existing == c.proc.signal_actions.end() ? 0 : existing->second.flags,
            };

            const guest_object<macos_old_sigaction> out{c.emu_ref.memory, outgoing};
            if (!out.try_write(previous))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }
        }

        if (incoming != 0)
        {
            const guest_object<macos_user_sigaction> in{c.emu_ref.memory, incoming};
            const auto action = in.try_read();
            if (!action)
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            c.proc.signal_actions[signal_number] = macos_process_context::macos_signal_action{
                .handler = action->handler,
                .trampoline = action->trampoline,
                .mask = action->mask,
                .flags = action->flags,
            };
        }

        write_macos_syscall_result(c, 0);
    }

    // The kdebug family. A trace point is written into the kernel's ring buffer only while a tracing
    // session is armed, and there is never one here: no ktrace tool can attach to an emulated process,
    // and a buffer nothing reads is not worth the cost on every instrumented call in libdispatch,
    // libsystem_malloc and CoreFoundation. Accepting the trace point and dropping it is what an
    // unarmed kdebug does on a real kernel too.
    void sys_kdebug_trace(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, 0);
    }

    // The string-interning form answers with the id the caller may reuse for the same string. Zero is
    // the "no id" answer, and callers treat it as one.
    void sys_kdebug_trace_string(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, 0);
    }

    // The typefilter is a shared page the kernel maps so a client can decide, without a syscall, whether
    // a class of trace points is enabled. There is no buffer behind it to filter for.
    void sys_kdebug_typefilter(const macos_syscall_context& c)
    {
        write_macos_syscall_error(c, MACOS_ENOTSUP);
    }

    void sys_getpid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, c.proc.pid);
    }

    void sys_getppid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, c.proc.ppid);
    }

    // SQLite's proxy locking reaches this on the way to CoreData's store: it stamps the host's identity
    // into the .store-conch file so a second machine sharing the database over a network volume can tell
    // whose lock it is reading. A synthetic id is the right answer twice over -- the emulated machine is
    // not this host, and handing a sample the operator's real hardware UUID would leak an identifier that
    // follows the analyst around. The bytes are fixed rather than random so a conch written by one run is
    // recognised by the next, and they are a well-formed version 4 UUID whose first bytes spell SogenEMU.
    void sys_gethostuuid(const macos_syscall_context& c)
    {
        static constexpr std::array<uint8_t, 16> host_uuid{0x53, 0x6F, 0x67, 0x65, 0x6E, 0x45, 0x4D, 0x55,
                                                           0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

        if (!c.emu_ref.memory.try_write_memory(get_macos_syscall_argument(c, 0), host_uuid.data(), host_uuid.size()))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_getuid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, c.proc.uid);
    }

    void sys_geteuid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, c.proc.euid);
    }

    void sys_getgid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, c.proc.gid);
    }

    void sys_getegid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, c.proc.egid);
    }

    // iopolicysys(cmd, struct _iopol_param_t*) -- bsd/kern/kern_resource.c. libSystem's
    // getiopolicy_np/setiopolicy_np are the only callers; libdispatch reaches them when it puts a
    // worker thread at a throttled QoS. Every I/O policy the emulator can offer is the default one:
    // sogen has no throttled I/O band to move a thread into, so a get answers IOPOL_DEFAULT and a set
    // is accepted and reported by scope and type once, rather than being silently forgotten.
    void sys_iopolicysys(const macos_syscall_context& c)
    {
        constexpr uint32_t iopol_cmd_set = 0;
        constexpr uint32_t iopol_cmd_get = 1;
        constexpr int32_t iopol_default = 0;

        const auto command = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto parameter = get_macos_syscall_argument(c, 1);

        std::array<int32_t, 3> policy{};
        if (parameter == 0 || !c.emu_ref.memory.try_read_memory(parameter, policy.data(), sizeof(policy)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        if (command == iopol_cmd_get)
        {
            policy[2] = iopol_default;
            if (!c.emu_ref.memory.try_write_memory(parameter, policy.data(), sizeof(policy)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        if (command != iopol_cmd_set)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        static std::set<std::pair<int32_t, int32_t>> reported{};
        if (reported.emplace(policy[0], policy[1]).second)
        {
            c.emu_ref.log.info("iopolicysys: I/O policy %d for scope %d type %d is accepted and not applied; sogen has one "
                               "unthrottled I/O band\n",
                               policy[2], policy[0], policy[1]);
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_issetugid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, 0);
    }

    void sys_thread_selfid(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, static_cast<int64_t>(c.proc.active_thread != nullptr ? c.proc.active_thread->thread_id : 0));
    }

    void sys_getentropy(const macos_syscall_context& c)
    {
        const auto buffer_address = get_macos_syscall_argument(c, 0);
        const auto length = get_macos_syscall_argument(c, 1);

        if (length > MACOS_GETENTROPY_MAX_LENGTH)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        // Seeded from the retired instruction count instead of a host entropy source: an analysis has to
        // replay identically, and the count still separates one call from the next within a run. This is
        // deliberately not a CSPRNG - the seed is low-entropy and a guest can count its own instructions -
        // so nothing derived from it may ever leave the sandbox as security material.
        std::mt19937_64 generator{MACOS_ENTROPY_SEED ^ c.emu_ref.get_executed_instructions()};

        std::array<uint8_t, MACOS_GETENTROPY_MAX_LENGTH> bytes{};
        for (size_t i = 0; i < length; ++i)
        {
            bytes[i] = static_cast<uint8_t>(generator());
        }

        if (length > 0 && !c.emu_ref.memory.try_write_memory(buffer_address, bytes.data(), static_cast<size_t>(length)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // Code-signing enforcement is a declared non-goal, and a permissive stub is more dangerous than an
    // honest failure: dyld branches on the specific operations it cares about, so a fabricated success
    // sends it down a path nothing here implements.
    void sys_csops(const macos_syscall_context& c)
    {
        write_macos_syscall_error(c, MACOS_EINVAL);
    }

    void sys_csops_audittoken(const macos_syscall_context& c)
    {
        write_macos_syscall_error(c, MACOS_EINVAL);
    }

    namespace
    {
        constexpr uint32_t PROC_INFO_CALL_PIDINFO = 2;
        constexpr uint32_t PROC_PIDTBSDINFO = 3;
        constexpr uint32_t PROC_PIDT_SHORTBSDINFO = 13;

        // Not in the SDK header; XNU declares it privately. Anything asking who it is reaches it, and the
        // 56-byte size is what identifies it on the wire.
        constexpr uint32_t PROC_PIDUNIQIDENTIFIERINFO = 17;

        constexpr size_t MACOS_MAXCOMLEN = 16;

#pragma pack(push, 1)

        struct macos_proc_bsdinfo
        {
            uint32_t flags{};
            uint32_t status{};
            uint32_t xstatus{};
            uint32_t pid{};
            uint32_t ppid{};
            uint32_t uid{};
            uint32_t gid{};
            uint32_t ruid{};
            uint32_t rgid{};
            uint32_t svuid{};
            uint32_t svgid{};
            uint32_t rfu_1{};
            char comm[MACOS_MAXCOMLEN]{};
            char name[2 * MACOS_MAXCOMLEN]{};
            uint32_t nfiles{};
            uint32_t pgid{};
            uint32_t pjobc{};
            uint32_t e_tdev{};
            uint32_t e_tpgid{};
            int32_t nice{};
            uint64_t start_tvsec{};
            uint64_t start_tvusec{};
        };

        struct macos_proc_bsdshortinfo
        {
            uint32_t pid{};
            uint32_t ppid{};
            uint32_t pgid{};
            uint32_t status{};
            char comm[MACOS_MAXCOMLEN]{};
            uint32_t flags{};
            uint32_t uid{};
            uint32_t gid{};
            uint32_t ruid{};
            uint32_t rgid{};
            uint32_t svuid{};
            uint32_t svgid{};
            uint32_t rfu{};
        };

        struct macos_proc_uniqidentifierinfo
        {
            uint8_t uuid[16]{};
            uint64_t uniqueid{};
            uint64_t puniqueid{};
            uint64_t reserve2{};
            uint64_t reserve3{};
            uint64_t reserve4{};
        };

#pragma pack(pop)

        static_assert(sizeof(macos_proc_bsdinfo) == 136);
        static_assert(sizeof(macos_proc_bsdshortinfo) == 64);
        static_assert(sizeof(macos_proc_uniqidentifierinfo) == 56);

        constexpr uint32_t MACOS_PROC_STATUS_RUNNING = 2; // SRUN
        constexpr uint32_t MACOS_PROC_FLAG_LP64 = 0x20;

        void copy_process_name(char* destination, const size_t capacity, const std::string& path)
        {
            const auto name = std::filesystem::path{path}.filename().string();
            const auto count = std::min(name.size(), capacity - 1);
            std::memcpy(destination, name.data(), count);
        }
    }

    // Only the identity flavours, and only for this process. A guest asking about another pid is asking
    // about a machine the emulator is not modelling, and answering would be inventing one.
    void sys_proc_info(const macos_syscall_context& c)
    {
        const auto call = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto pid = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));
        const auto flavor = static_cast<uint32_t>(get_macos_syscall_argument(c, 2));
        const auto buffer = get_macos_syscall_argument(c, 4);
        const auto buffer_size = get_macos_syscall_argument(c, 5);

        if (call != PROC_INFO_CALL_PIDINFO || pid != c.proc.pid)
        {
            write_macos_syscall_error(c, MACOS_ESRCH);
            return;
        }

        const auto answer = [&](const auto& value) {
            if (buffer_size < sizeof(value))
            {
                write_macos_syscall_error(c, MACOS_ENOSPC);
                return;
            }

            const guest_object<std::decay_t<decltype(value)>> out{c.emu_ref.memory, buffer};
            if (!out.try_write(value))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            // proc_info reports how many bytes it wrote, not zero.
            write_macos_syscall_result(c, static_cast<int64_t>(sizeof(value)));
        };

        switch (flavor)
        {
        case PROC_PIDTBSDINFO: {
            macos_proc_bsdinfo info{};
            info.flags = MACOS_PROC_FLAG_LP64;
            info.status = MACOS_PROC_STATUS_RUNNING;
            info.pid = c.proc.pid;
            info.ppid = c.proc.ppid;
            info.uid = c.proc.euid;
            info.gid = c.proc.egid;
            info.ruid = c.proc.uid;
            info.rgid = c.proc.gid;
            info.svuid = c.proc.uid;
            info.svgid = c.proc.gid;
            info.pgid = c.proc.pid;
            // The count is not tracked; zero is what a process with no open descriptors would report, and no
            // caller of this flavour has been seen to read it.
            info.nfiles = 0;
            copy_process_name(info.comm, sizeof(info.comm), c.proc.executable_path);
            copy_process_name(info.name, sizeof(info.name), c.proc.executable_path);
            answer(info);
            return;
        }

        case PROC_PIDT_SHORTBSDINFO: {
            macos_proc_bsdshortinfo info{};
            info.pid = c.proc.pid;
            info.ppid = c.proc.ppid;
            info.pgid = c.proc.pid;
            info.status = MACOS_PROC_STATUS_RUNNING;
            info.flags = MACOS_PROC_FLAG_LP64;
            info.uid = c.proc.euid;
            info.gid = c.proc.egid;
            info.ruid = c.proc.uid;
            info.rgid = c.proc.gid;
            info.svuid = c.proc.uid;
            info.svgid = c.proc.gid;
            copy_process_name(info.comm, sizeof(info.comm), c.proc.executable_path);
            answer(info);
            return;
        }

        case PROC_PIDUNIQIDENTIFIERINFO: {
            macos_proc_uniqidentifierinfo info{};

            // Derived from the pid rather than random: two calls in one run must agree, and a guest that
            // caches the value and compares it later would otherwise catch the difference.
            info.uniqueid = 0x5060'0000'0000'0000ULL | c.proc.pid;
            info.puniqueid = 0x5060'0000'0000'0000ULL | c.proc.ppid;
            for (size_t i = 0; i < sizeof(info.uuid); ++i)
            {
                info.uuid[i] = static_cast<uint8_t>((c.proc.pid * 31 + i * 7 + 1) & 0xFF);
            }

            answer(info);
            return;
        }

        default:
            write_macos_syscall_error(c, MACOS_ENOSYS);
            return;
        }
    }

    void sys_mac_syscall(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, 0);
    }

    void sys_bsdthread_register(const macos_syscall_context& c)
    {
        // arm64e libpthread hands both entry points over signed, and the kernel ptrauth_strip()s them
        // before it ever stores them -- it enters a new thread by writing pc, which is not an
        // authenticating branch. Keeping the signature makes the first real pthread_create() start its
        // thread at a pc with the signature still in its top bits and fault before its first instruction.
        constexpr uint64_t POINTER_BITS = 0x0000FFFFFFFFFFFFULL;

        c.proc.pthread_thread_start = get_macos_syscall_argument(c, 0) & POINTER_BITS;
        c.proc.pthread_wqthread = get_macos_syscall_argument(c, 1) & POINTER_BITS;

        // The return value is not a status: libpthread stores it as __pthread_supported_features and
        // treats zero as "libpthread has not been initialized", which it reports through os_crash with
        // exactly that string. Returning success without it left every guest that reached a workqueue
        // path dead at a brk #0xb001 in libsystem_pthread.
        //
        // The value is the one this machine's kernel returns, read out of a live process at
        // __pthread_supported_features rather than assembled from header constants: the guest's
        // libpthread comes from this build's cache and its paths are written against what this kernel
        // says. A narrower mask would send it down branches this build never takes.
        write_macos_syscall_result(c, MACOS_PTHREAD_SUPPORTED_FEATURES);
    }

    void sys_workq_open(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, 0);
    }

    void sys_workq_kernreturn(const macos_syscall_context& c)
    {
        const auto options = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));

        // libpthread hands the kernel its dispatch entry points once, before any work exists. There is
        // nothing to schedule at that point, so recording the request and reporting success is the whole
        // of it.
        if (options == MACOS_WQOPS_SETUP_DISPATCH)
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        // WQOPS_THREAD_RETURN (xnu bsd/pthread/workqueue_syscalls.h): a worker parks itself back into the
        // kernel pool. The call does not return to userspace on a real kernel -- _start_wqthread traps
        // brk #1 if __pthread_wqthread ever returns -- so the worker is parked on its own kport and the
        // cpu goes to whoever can still run. The WORKLOOP/KEVENT variants (measured 2026-08-27: 0x100
        // and 0x40, with x1 naming the worker's kevent buffer) park the same way.
        const auto pool_park = options == MACOS_WQOPS_THREAD_RETURN || options == MACOS_WQOPS_THREAD_WORKLOOP_RETURN ||
                               options == MACOS_WQOPS_THREAD_KEVENT_RETURN;

        // The two kevent variants also carry a changelist: the events the worker handled are answered
        // and the next round of knotes registered in the same call that parks it.
        if (options == MACOS_WQOPS_THREAD_KEVENT_RETURN || options == MACOS_WQOPS_THREAD_WORKLOOP_RETURN)
        {
            apply_worker_return_changelist(c, get_macos_syscall_argument(c, 1), static_cast<int32_t>(get_macos_syscall_argument(c, 2)),
                                           options == MACOS_WQOPS_THREAD_WORKLOOP_RETURN);
        }

        if (pool_park)
        {
            auto* waiter = c.proc.active_thread;
            if (waiter == nullptr || waiter->workqueue_kport == 0)
            {
                c.emu_ref.log.warn("workq_kernreturn WQOPS_THREAD_RETURN from a thread sogen did not spawn as a worker\n");
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            // A worker parking while events wait would sleep through its own wake: the kernel hands
            // the events straight over, continuing the thread at _start_wqthread instead.
            if (c.emu_ref.workqueue.continue_worker_with_pending_events(c.emu_ref, *waiter))
            {
                return;
            }

            // The other place a settled application runs out of work, and the same borrow: a repaint
            // that only moved CoreAnimation has nothing left to rasterise it, and the pool going quiet
            // is the moment to. The bare return is right for the same reason the reschedule branch
            // below has one.
            if (c.emu_ref.borrow_a_waiting_thread_for_a_frame())
            {
                return;
            }

            waiter->blocked_on_port = waiter->workqueue_kport;
            if (c.emu_ref.reschedule_away_from_a_blocked_thread())
            {
                // No result is written: if the thread is ever scheduled again it re-runs the svc and
                // parks again, exactly as it would after a spurious schedule on a real kernel.
                return;
            }

            waiter->blocked_on_port = 0;

            // A pool that has run dry is where a launched application actually comes to rest -- measured
            // on Calculator, which reports its idle here about half the time and on a mach receive the
            // rest. With a host that can still deliver input this is an app waiting for a click, not a
            // deadlock, so it parks rather than halting. The bare return is right for the same reason
            // the reschedule branch above returns without writing a result.
            if (c.emu_ref.can_wake_from_host_input())
            {
                c.emu_ref.park_for_host_input();
                return;
            }

            // Everything is parked -- workers in the pool, whatever they were serving in a wait of its
            // own -- and nothing is left that could wake any of it. Enumerate the parks: the name of
            // what each thread waits on is the report.
            std::string detail = "workqueue thread " + std::to_string(waiter->thread_id) +
                                 " returned itself to the kernel pool and no runnable thread is left:";
            for (const auto& [id, thread] : c.proc.threads)
            {
                if (thread.terminated)
                {
                    continue;
                }

                auto state = describe_thread_park(thread);
                if (state.empty())
                {
                    state = id == waiter->thread_id ? " the thread reporting this deadlock" : " runnable";
                }

                detail += "\n    thread " + std::to_string(id) + state;

                if (thread.last_send_port != 0)
                {
                    std::array<char, 96> sent{};
                    std::snprintf(sent.data(), sent.size(), " (last sent routine 0x%x to port 0x%x)", thread.last_send_routine,
                                  thread.last_send_port);
                    detail += sent.data();
                }

                // A parked thread's wait site is invisible without walking its saved stack -- the port
                // number alone never says which library decided to wait. The frame chain can end inside
                // a library (leaf-optimized frames), so raw-scan candidates follow the walked frames.
                auto frames = c.emu_ref.backtrace_from(thread.saved_regs.pc, thread.saved_regs.fp, 6);
                for (const auto& scanned : c.emu_ref.stack_scan(thread.saved_regs.sp, 4))
                {
                    if (std::ranges::find(frames, scanned) == frames.end())
                    {
                        frames.push_back(scanned);
                    }
                }
                for (const auto& frame : frames)
                {
                    detail += "\n        " + frame;
                }
            }

            if (c.emu_ref.mach.last_unserviced_send.routine != 0)
            {
                detail += "\n    no server answered routine " + std::to_string(c.emu_ref.mach.last_unserviced_send.routine) +
                          " sent to port " + std::to_string(c.emu_ref.mach.last_unserviced_send.port);
            }

            // A reply sitting undrained on a port is the difference between "nobody answered" and
            // "answered, but nobody picked it up" -- the two walls need opposite fixes.
            for (const auto& queued : c.emu_ref.mach.ports.non_empty_queues())
            {
                std::array<char, 128> line{};
                std::snprintf(line.data(), line.size(), "port 0x%x holds %zu undelivered message(s), first id 0x%x", queued.name,
                              queued.depth, queued.first_message_id);
                detail += "\n    ";
                detail += line.data();
            }

            c.emu_ref.record_stop(stop_reason::workqueue_park_deadlock, detail);
            c.emu_ref.stop();
            return;
        }

        if (options == MACOS_WQOPS_QUEUE_REQTHREADS || options == MACOS_WQOPS_QUEUE_REQTHREADS2)
        {
            const auto requested = static_cast<int32_t>(get_macos_syscall_argument(c, 2));
            if (requested <= 0)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            const auto priority = static_cast<uint32_t>(get_macos_syscall_argument(c, 3));

            for (int32_t i = 0; i < requested; ++i)
            {
                if (!c.emu_ref.workqueue.request_worker(c.emu_ref, priority))
                {
                    break;
                }
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        // Everything else asks the kernel to run work on a thread it owns, and sogen owns no such
        // thread. Refused rather than answered with success: a caller told its item was accepted waits
        // for a completion that will never come, and a hang is far harder to read than an errno.
        c.emu_ref.log.warn("workq_kernreturn option 0x%x needs kernel-owned worker threads, which are not emulated\n", options);
        write_macos_syscall_error(c, MACOS_ENOTSUP);
    }

    void sys_bsdthread_create(const macos_syscall_context& c)
    {
        const auto start_routine = get_macos_syscall_argument(c, 0);
        const auto start_argument = get_macos_syscall_argument(c, 1);
        const auto stack = get_macos_syscall_argument(c, 2);
        const auto pthread = get_macos_syscall_argument(c, 3);
        const auto flags = static_cast<uint32_t>(get_macos_syscall_argument(c, 4));

        if (c.proc.pthread_thread_start == 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        // Only the custom form is reachable from a current libpthread: it allocates the stack and the
        // pthread struct itself and passes both in. Serving the other form would mean the emulator
        // reproducing the struct layout libpthread owns, so it declines rather than guessing at it.
        if ((flags & MACOS_PTHREAD_START_CUSTOM) == 0 || pthread == 0 || stack == 0)
        {
            write_macos_syscall_error(c, MACOS_ENOTSUP);
            return;
        }

        // Only the top is known here; the mapping below it belongs to libpthread, which made it.
        const auto thread_id = c.proc.create_thread(stack, 0, c.proc.pthread_thread_start);
        auto& thread = c.proc.threads.at(thread_id);

        thread.thread_self = pthread + MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET;
        thread.saved_regs.tpidrro_el0 = thread.thread_self;

        const auto thread_port = c.emu_ref.mach.thread_self_for(thread_id);

        // _pthread_start reads the port out of the TSD rather than out of x1, so writing only the
        // register leaves it reading zero and calling abort_with_reason.
        const auto tsd_slot = thread.thread_self + MACOS_PTHREAD_TSD_SLOT_MACH_THREAD_SELF * sizeof(uint64_t);
        const uint64_t tsd_value = thread_port;
        if (!c.emu_ref.memory.try_write_memory(tsd_slot, &tsd_value, sizeof(tsd_value)))
        {
            c.emu_ref.log.warn("bsdthread_create: the pthread struct at 0x%" PRIx64 " has no writable TSD\n", pthread);
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        // The register contract _pthread_start reads on entry. It authenticates x0 against the pthread
        // struct's own signed self pointer, so passing anything but the caller's pointer traps there
        // rather than failing here.
        thread.saved_regs.x[0] = pthread;
        thread.saved_regs.x[1] = thread_port;
        thread.saved_regs.x[2] = start_routine;
        thread.saved_regs.x[3] = start_argument;
        thread.saved_regs.x[4] = stack;
        thread.saved_regs.x[5] = flags | MACOS_PTHREAD_START_TSD_BASE_SET;

        c.emu_ref.callbacks.on_thread_create(thread_id, start_routine, start_argument);
        write_macos_syscall_result(c, static_cast<int64_t>(pthread));
    }

    void sys_bsdthread_terminate(const macos_syscall_context& c)
    {
        auto* thread = c.proc.active_thread;
        if (thread == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        thread->terminated = true;
        c.emu_ref.callbacks.on_thread_terminated(thread->thread_id);

        // The caller does not come back from this, so there is no result to write. Whether another
        // thread is left to run is the emulator's problem, not this thread's.
        c.emu_ref.resume_some_thread();
    }

    namespace
    {
        void write_ulock_error(const macos_syscall_context& c, const uint32_t operation, const int64_t error)
        {
            if ((operation & MACOS_ULF_NO_ERRNO) != 0)
            {
                write_macos_syscall_result(c, -error);
                return;
            }

            write_macos_syscall_error(c, error);
        }
    }

    void sys_ulock_wait(const macos_syscall_context& c)
    {
        const auto operation = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto address = get_macos_syscall_argument(c, 1);
        const auto value = get_macos_syscall_argument(c, 2);
        const auto timeout = static_cast<uint32_t>(get_macos_syscall_argument(c, 3));

        auto* waiter = c.proc.active_thread;

        // Reaching the wait again means the thread was rescheduled onto its own svc, so any earlier park
        // is over whatever happens next -- the same re-entry rule the mach receive path follows.
        if (waiter != nullptr)
        {
            waiter->blocked_on_ulock = 0;
        }

        // UL_UNFAIR_LOCK parks on the same compare as the other two. What xnu does extra for it is
        // resolve the owner out of the lock word's port name to donate priority, and answer EOWNERDEAD
        // when that name is dead; sogen has no priority to donate, and a lock word naming a thread that
        // is gone reaches the deadlock report below, which says more than libplatform's abort would.
        const auto opcode = operation & MACOS_UL_OPCODE_MASK;
        if (opcode != MACOS_UL_COMPARE_AND_WAIT && opcode != MACOS_UL_COMPARE_AND_WAIT_SHARED && opcode != MACOS_UL_UNFAIR_LOCK)
        {
            static std::set<uint32_t> reported{};
            if (reported.insert(opcode).second)
            {
                c.emu_ref.log.warn("ulock_wait operation 0x%x is not modelled\n", opcode);
            }

            write_ulock_error(c, operation, MACOS_EINVAL);
            return;
        }

        uint32_t current{};
        if (!c.emu_ref.memory.try_read_memory(address, &current, sizeof(current)))
        {
            write_ulock_error(c, operation, MACOS_EFAULT);
            return;
        }

        // UL_COMPARE_AND_WAIT (xnu bsd/sys/ulock.h): the word changing before the wait means the wake
        // already happened, so a mismatch returns immediately instead of parking through it.
        if (current != static_cast<uint32_t>(value))
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        // Timeouts are not modelled, matching the mach timeout stance: returning immediately would be a
        // lie, so the thread parks and the report names it.
        if (timeout != 0)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                c.emu_ref.log.warn("ulock_wait with a timeout (0x%x us) parks without one; ulock timeouts are not modelled\n", timeout);
            }
        }

        if (waiter == nullptr)
        {
            write_ulock_error(c, operation, MACOS_EINVAL);
            return;
        }

        waiter->blocked_on_ulock = address;
        if (c.emu_ref.reschedule_away_from_a_blocked_thread())
        {
            // No result is written: the rewound pc re-runs the whole wait when this thread is scheduled
            // again, and the answer then is the one that counts.
            return;
        }

        waiter->blocked_on_ulock = 0;

        // Nothing else can run, so nobody can ever store to the word or wake the address. Same shape as
        // the mach receive deadlock: halt and name the condition.
        size_t runnable = 0;
        for (const auto& [id, thread] : c.proc.threads)
        {
            runnable += thread.terminated ? 0u : 1u;
        }

        std::array<char, 256> head{};
        std::snprintf(head.data(), head.size(),
                      "ulock_wait on address 0x%" PRIx64 " with no runnable thread left to wake it, %zu live thread%s", address, runnable,
                      runnable == 1 ? "" : "s");

        auto detail = std::string{head.data()};
        for (const auto& frame : c.emu_ref.backtrace(8))
        {
            detail += "\n    " + frame;
        }

        c.emu_ref.record_stop(stop_reason::ulock_wait_deadlock, detail);
        c.emu_ref.stop();
    }

    void sys_ulock_wake(const macos_syscall_context& c)
    {
        const auto operation = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto address = get_macos_syscall_argument(c, 1);

        // UL_UNFAIR_LOCK wakes ride the same address queue as the compare-and-wait forms, so all three
        // clear one waiter; xnu bsd/sys/ulock.h names no other wake opcode the userspace libraries emit.
        const auto opcode = operation & MACOS_UL_OPCODE_MASK;
        if (opcode != MACOS_UL_COMPARE_AND_WAIT && opcode != MACOS_UL_COMPARE_AND_WAIT_SHARED && opcode != MACOS_UL_UNFAIR_LOCK)
        {
            static std::set<uint32_t> reported{};
            if (reported.insert(opcode).second)
            {
                c.emu_ref.log.warn("ulock_wake operation 0x%x is not modelled\n", opcode);
            }

            write_ulock_error(c, operation, MACOS_EINVAL);
            return;
        }

        if ((operation & MACOS_ULF_WAKE_THREAD) != 0)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                c.emu_ref.log.warn("ulock_wake with ULF_WAKE_THREAD wakes whichever thread is parked on the address rather than the "
                                   "one named in the request\n");
            }
        }

        const auto wake_all = (operation & MACOS_ULF_WAKE_ALL) != 0;
        while (const auto woken = c.proc.wake_ulock_waiter_of(address))
        {
            c.emu_ref.log.info("waking thread %" PRIu64 " parked on ulock 0x%" PRIx64 "\n", woken, address);
            if (!wake_all)
            {
                break;
            }
        }

        write_macos_syscall_result(c, 0);
    }

    namespace
    {
        constexpr uint64_t NSEC_PER_SECOND = 1000000000ULL;

        // The timeout arrives as a timespec that is either relative or an absolute wall-clock deadline
        // (xnu bsd/kern/kern_sig.c __semwait_signal_nocancel). Both reduce to a relative wait; an
        // absolute deadline in the past collapses to {0,0}, which the mach layer treats as non-blocking.
        uint64_t semwait_timeout_ns(const bool relative, const uint64_t sec, const uint64_t nsec)
        {
            if (relative)
            {
                return sec * NSEC_PER_SECOND + nsec;
            }

            const auto epoch = std::chrono::system_clock::now().time_since_epoch();
            const auto now_ns =
                static_cast<uint64_t>(std::max<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count(), 0));
            const auto target_ns = sec * NSEC_PER_SECOND + nsec;

            return target_ns > now_ns ? target_ns - now_ns : 0;
        }

        uint64_t ns_to_absolute_ticks(const macos_syscall_context& c, const uint64_t ns)
        {
            return ns / c.emu_ref.mach.timebase_numer * c.emu_ref.mach.timebase_denom;
        }
    }

    // __semwait_signal(cond_sem, mutex_sem, timeout, relative, tv_sec, tv_nsec): wait on a condition
    // semaphore, signalling the mutex semaphore around the wait. A nonzero mutex is signalled on every
    // path -- fast consume, immediate timeout and park alike -- because that is xnu's wait_signal
    // pairing (osfmk/kern/sync_sema.c semaphore_wait_internal); the caller re-acquires the mutex itself.
    void sys_semwait_signal(const macos_syscall_context& c)
    {
        const auto cond_sem = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto mutex_sem = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));
        const auto timeout = static_cast<uint32_t>(get_macos_syscall_argument(c, 2));
        const auto relative = static_cast<uint32_t>(get_macos_syscall_argument(c, 3));
        const auto tv_sec = static_cast<int64_t>(get_macos_syscall_argument(c, 4));
        const auto tv_nsec = static_cast<int32_t>(get_macos_syscall_argument(c, 5));

        auto* waiter = c.proc.active_thread;

        // Reaching the wait again means the thread was rescheduled onto its own svc, so any earlier park
        // is over whatever happens next -- the same re-entry rule the mach receive path follows.
        if (waiter != nullptr)
        {
            waiter->blocked_on_sem = 0;
            waiter->semwait_deadline = 0;
        }

        // The only wake with no signal behind it: the scheduler fired this thread's deadline because
        // nothing else could run.
        if (waiter != nullptr && waiter->semwait_timed_out)
        {
            waiter->semwait_timed_out = false;
            write_macos_syscall_error(c, MACOS_ETIMEDOUT);
            return;
        }

        // xnu inspects the timespec only when the timeout flag says there is one; an indefinite wait
        // carries no clamping and no EINTR. The clamp itself exists because mach cannot express a
        // timeout that long, so an out-of-range tv_sec is truncated and a *completed* wait then
        // answers EINTR instead of 0.
        const auto truncated = timeout != 0 && (static_cast<uint64_t>(tv_sec) & 0xFFFFFFFF00000000ULL) != 0;
        const auto clamped_sec = truncated ? 0xFFFFFFFFULL : static_cast<uint64_t>(tv_sec);

        if (timeout != 0 && (tv_nsec < 0 || static_cast<uint64_t>(tv_nsec) >= NSEC_PER_SECOND))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        auto* cond = c.emu_ref.mach.find_semaphore(cond_sem);
        auto* mutex = mutex_sem != 0 ? c.emu_ref.mach.find_semaphore(mutex_sem) : nullptr;

        if (cond == nullptr || (mutex_sem != 0 && mutex == nullptr))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto timeout_ns = timeout != 0 ? semwait_timeout_ns(relative != 0, clamped_sec, static_cast<uint64_t>(tv_nsec)) : 0;

        // A parked-then-woken wait re-runs here with the mutex semaphore already signalled once, back
        // when the wait registered -- in xnu the whole park is a single semaphore_wait_signal, so the
        // wake adds nothing. Consume the signal's count and answer; if another wait consumed the count
        // first, fall through and park again, still without a second mutex signal.
        const auto woken_by_signal = waiter != nullptr && waiter->semwait_woken;
        if (woken_by_signal)
        {
            waiter->semwait_woken = false;
            if (cond->value > 0)
            {
                --cond->value;
                if (truncated)
                {
                    write_macos_syscall_error(c, MACOS_EINTR);
                    return;
                }

                write_macos_syscall_result(c, 0);
                return;
            }
        }

        const auto signal_mutex = [&] {
            if (mutex != nullptr && !woken_by_signal)
            {
                c.emu_ref.mach.semaphore_signal(mutex_sem);
                if (const auto woken = c.proc.wake_semwait_waiter_of(mutex_sem); woken != 0)
                {
                    c.emu_ref.log.info("waking thread %" PRIu64 " parked in __semwait_signal on semaphore 0x%x\n", woken, mutex_sem);
                }
            }
        };

        if (cond->value > 0)
        {
            --cond->value;
            signal_mutex();
            if (truncated)
            {
                write_macos_syscall_error(c, MACOS_EINTR);
                return;
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        // A {0,0} timeout is NOBLOCK in the mach layer: ETIMEDOUT without ever parking.
        if (timeout != 0 && timeout_ns == 0)
        {
            signal_mutex();
            write_macos_syscall_error(c, MACOS_ETIMEDOUT);
            return;
        }

        if (waiter == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        waiter->blocked_on_sem = cond_sem;
        if (timeout != 0)
        {
            const auto now_ticks = c.emu.read_system_register(3, 3, 14, 0, 2);
            waiter->semwait_deadline = now_ticks + ns_to_absolute_ticks(c, timeout_ns);
        }

        signal_mutex();

        // cond_sem == mutex_sem self-pairing: the mutex signal was aimed at the semaphore this thread
        // just parked on and woke the wait straight back up. Consume the count and report success, the
        // same end state as xnu's handoff.
        if (waiter->blocked_on_sem == 0)
        {
            --cond->value;
            if (truncated)
            {
                write_macos_syscall_error(c, MACOS_EINTR);
                return;
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        if (c.emu_ref.reschedule_away_from_a_blocked_thread())
        {
            // No result is written: the rewound pc re-runs the whole wait when this thread is scheduled
            // again, and the answer then is the one that counts.
            return;
        }

        waiter->blocked_on_sem = 0;
        waiter->semwait_deadline = 0;

        // Nothing else can run and no timed waiter is left for the scheduler to wake, so nobody can
        // ever signal this semaphore. Same shape as the ulock deadlock: halt and name the condition.
        std::array<char, 256> head{};
        std::snprintf(head.data(), head.size(), "__semwait_signal on semaphore 0x%x with no runnable thread left to signal it", cond_sem);

        auto detail = std::string{head.data()} + describe_every_thread_park(c, waiter->thread_id);

        for (const auto& frame : c.emu_ref.backtrace(8))
        {
            detail += "\n    " + frame;
        }

        c.emu_ref.record_stop(stop_reason::semwait_signal_deadlock, detail);
        c.emu_ref.stop();
    }

    // The psynch family (BSD 301-305 and 312) is libpthread's kernel side for a contended
    // pthread_mutex and for every pthread_cond. Its whole difficulty is the sequence words: libpthread
    // keeps its own copy in the pthread object and compares the kernel's answer against it, so a wrong
    // value is not an error the guest reports but a spin or an abort somewhere else entirely. Every
    // number below was read out of this host's libpthread under lldb on 2026-08-28 rather than derived
    // from the header constants.
    namespace
    {
        uint32_t psynch_mutex_handoff_bits(const uint32_t mgen)
        {
            return (mgen & MACOS_PTHRW_COUNT_MASK) | MACOS_PTH_RWL_EBIT | MACOS_PTH_RWL_KBIT;
        }

        // Measured: three threads queued on one mutex with lock sequences 0x102, 0x202 and 0x302 were
        // each handed 0x303, which is the *dropper's* sequence word, not their own.
        void psynch_hand_over_mutex(const macos_syscall_context& c, const uint64_t mutex, const uint32_t mgen)
        {
            const auto updatebits = psynch_mutex_handoff_bits(mgen);

            if (c.proc.wake_psynch_mutex_waiter_of(mutex, updatebits) != 0)
            {
                return;
            }

            // xnu keeps the hand-off as a prepost (kw_pre_rwwc) rather than dropping it: the contender
            // that made libpthread take the kernel path may not have reached its own syscall yet, and a
            // discarded hand-off leaves it parked on a lock nobody holds.
            c.proc.psynch_mutex_preposts[mutex] = updatebits;
        }

        void report_psynch_deadlock(const macos_syscall_context& c, const macos_thread& waiter, const std::string& head)
        {
            auto detail = head + describe_every_thread_park(c, waiter.thread_id);

            for (const auto& frame : c.emu_ref.backtrace(8))
            {
                detail += "\n    " + frame;
            }

            c.emu_ref.record_stop(stop_reason::psynch_wait_deadlock, detail);
            c.emu_ref.stop();
        }
    }

    void sys_psynch_mutexwait(const macos_syscall_context& c)
    {
        const auto mutex = get_macos_syscall_argument(c, 0);
        const auto mgen = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));

        auto* waiter = c.proc.active_thread;
        if (waiter == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        // Reaching the wait again means the thread was rescheduled onto its own svc, so any earlier park
        // is over whatever happens next -- the same re-entry rule the mach receive path follows. A thread
        // rescheduled without a hand-off keeps the ticket it queued with, or it would lose its place.
        const auto requeued = std::exchange(waiter->blocked_on_psynch_mutex, uint64_t{0}) == mutex;

        if (waiter->psynch_mutex_updatebits != 0)
        {
            write_macos_syscall_result(c, std::exchange(waiter->psynch_mutex_updatebits, 0u));
            return;
        }

        if (const auto prepost = c.proc.psynch_mutex_preposts.find(mutex); prepost != c.proc.psynch_mutex_preposts.end())
        {
            const auto updatebits = prepost->second;
            c.proc.psynch_mutex_preposts.erase(prepost);
            write_macos_syscall_result(c, updatebits);
            return;
        }

        waiter->blocked_on_psynch_mutex = mutex;
        if (!requeued)
        {
            waiter->psynch_wait_ticket = c.proc.next_psynch_ticket++;
        }

        if (c.emu_ref.reschedule_away_from_a_blocked_thread())
        {
            // No result is written: the rewound pc re-runs the whole wait when this thread is scheduled
            // again, and the answer then is the one that counts.
            return;
        }

        waiter->blocked_on_psynch_mutex = 0;

        std::array<char, 192> head{};
        std::snprintf(head.data(), head.size(),
                      "psynch_mutexwait on mutex 0x%" PRIx64 " (sequence 0x%x) with no runnable thread left to drop it", mutex, mgen);
        report_psynch_deadlock(c, *waiter, head.data());
    }

    void sys_psynch_mutexdrop(const macos_syscall_context& c)
    {
        const auto mutex = get_macos_syscall_argument(c, 0);
        const auto mgen = static_cast<uint32_t>(get_macos_syscall_argument(c, 1));

        psynch_hand_over_mutex(c, mutex, mgen);

        // Measured: libpthread only checks the drop for -1, and this host's kernel answers 0.
        write_macos_syscall_result(c, 0);
    }

    void sys_psynch_cvwait(const macos_syscall_context& c)
    {
        const auto cv = get_macos_syscall_argument(c, 0);
        const auto mutex = get_macos_syscall_argument(c, 3);
        const auto mugen = get_macos_syscall_argument(c, 4);
        const auto sec = get_macos_syscall_argument(c, 6);
        const auto nsec = static_cast<uint32_t>(get_macos_syscall_argument(c, 7));

        auto* waiter = c.proc.active_thread;
        if (waiter == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto requeued = std::exchange(waiter->blocked_on_psynch_cv, uint64_t{0}) == cv;
        waiter->psynch_deadline = 0;

        if (std::exchange(waiter->psynch_timed_out, false))
        {
            write_macos_syscall_error(c, MACOS_ETIMEDOUT);
            return;
        }

        // A signalled wait re-runs the svc with the mutex already dropped, back when the wait
        // registered, so the drop must not happen a second time.
        if (std::exchange(waiter->psynch_cv_woken, false))
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        // The mutex the caller held is dropped by the kernel as part of the wait, with the same sequence
        // word psynch_mutexdrop would carry. libpthread passes 0 whenever it could drop the mutex in
        // userspace, which is every uncontended case. Measured: mugen's low half is the lock sequence
        // (0x200 there produced the 0x203 the successor was handed) and its high half is not read.
        if (mutex != 0)
        {
            psynch_hand_over_mutex(c, mutex, static_cast<uint32_t>(mugen));
        }

        if (const auto prepost = c.proc.psynch_cv_preposts.find(cv); prepost != c.proc.psynch_cv_preposts.end())
        {
            if (--prepost->second == 0)
            {
                c.proc.psynch_cv_preposts.erase(prepost);
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        // {sec, nsec} is always relative: libpthread converts an absolute pthread_cond_timedwait deadline
        // with its own gettimeofday before the call, and answers ETIMEDOUT itself for one already past.
        // {0, 0} is an indefinite wait, not an expired one.
        constexpr uint64_t MAX_TIMEOUT_SECONDS = (UINT64_MAX - UINT32_MAX) / NSEC_PER_SECOND;
        const auto timeout_ns = std::min(sec, MAX_TIMEOUT_SECONDS) * NSEC_PER_SECOND + nsec;

        waiter->blocked_on_psynch_cv = cv;
        if (!requeued)
        {
            waiter->psynch_wait_ticket = c.proc.next_psynch_ticket++;
        }

        if (timeout_ns != 0)
        {
            const auto now_ticks = c.emu.read_system_register(3, 3, 14, 0, 2);
            waiter->psynch_deadline = now_ticks + ns_to_absolute_ticks(c, timeout_ns);
        }

        if (c.emu_ref.reschedule_away_from_a_blocked_thread())
        {
            return;
        }

        waiter->blocked_on_psynch_cv = 0;
        waiter->psynch_deadline = 0;

        std::array<char, 192> head{};
        std::snprintf(head.data(), head.size(),
                      "psynch_cvwait on condition variable 0x%" PRIx64 " with no runnable thread left to signal it", cv);
        report_psynch_deadlock(c, *waiter, head.data());
    }

    namespace
    {
        // Both wake calls answer with the number of waiters they released, scaled by PTHRW_INC, plus the
        // C bit. libpthread adds that count to its own S word, so a wake that releases fewer threads than
        // it claims leaves the condition variable permanently out of step. Measured: one waiter answers
        // 0x101 and a broadcast over two answers 0x201.
        void write_psynch_wake_result(const macos_syscall_context& c, const uint64_t cv, const uint32_t claimed, const size_t woken)
        {
            auto updatebits = claimed | MACOS_PTH_RWS_CV_CBIT;

            const auto expected = claimed / MACOS_PTHRW_INC;
            if (woken < expected)
            {
                // The signal outran its waiter. xnu records the difference as a prepost so the wait that
                // registers next returns without parking, and reports it with the P bit, which is what
                // sends libpthread to psynch_cvclrprepost once its own count catches up.
                c.proc.psynch_cv_preposts[cv] += static_cast<uint32_t>(expected - woken);
                updatebits |= MACOS_PTH_RWS_CV_PBIT;

                static std::set<uint64_t> reported{};
                if (reported.insert(cv).second)
                {
                    c.emu_ref.log.warn("psynch wake of condition variable 0x%" PRIx64 " claimed %u waiter(s) and found %zu; the "
                                       "rest are preposted\n",
                                       cv, expected, woken);
                }
            }

            write_macos_syscall_result(c, updatebits);
        }
    }

    void sys_psynch_cvsignal(const macos_syscall_context& c)
    {
        const auto cv = get_macos_syscall_argument(c, 0);
        const auto thread_port = static_cast<uint32_t>(get_macos_syscall_argument(c, 3));
        const auto mutex = get_macos_syscall_argument(c, 4);
        const auto mugen = get_macos_syscall_argument(c, 5);

        if (thread_port != 0)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                c.emu_ref.log.warn("psynch_cvsignal names thread port 0x%x; sogen wakes the longest-queued waiter instead\n", thread_port);
            }
        }

        if (mutex != 0)
        {
            psynch_hand_over_mutex(c, mutex, static_cast<uint32_t>(mugen));
        }

        write_psynch_wake_result(c, cv, MACOS_PTHRW_INC, c.proc.wake_psynch_cv_waiters_of(cv, 1));
    }

    void sys_psynch_cvbroad(const macos_syscall_context& c)
    {
        const auto cv = get_macos_syscall_argument(c, 0);
        const auto cvudgen = get_macos_syscall_argument(c, 2);
        const auto mutex = get_macos_syscall_argument(c, 4);
        const auto mugen = get_macos_syscall_argument(c, 5);

        // cvudgen carries the caller's U word above the number of waiters it is claiming, in the same
        // PTHRW_INC units as every other sequence word.
        const auto claimed = static_cast<uint32_t>(cvudgen) & MACOS_PTHRW_COUNT_MASK;

        if (mutex != 0)
        {
            psynch_hand_over_mutex(c, mutex, static_cast<uint32_t>(mugen));
        }

        write_psynch_wake_result(c, cv, claimed, c.proc.wake_psynch_cv_waiters_of(cv, claimed / MACOS_PTHRW_INC));
    }

    void sys_psynch_cvclrprepost(const macos_syscall_context& c)
    {
        const auto cv = get_macos_syscall_argument(c, 0);

        c.proc.psynch_cv_preposts.erase(cv);
        write_macos_syscall_result(c, 0);
    }

    // persona(operation, flags, info, id, idlen). A persona is a launchd-assigned identity a normal
    // process never has, and every operation refuses one that does not -- so the whole table is a
    // refusal table. Measured on 25G76 by calling each operation from an ordinary process:
    // PERSONA_OP_GET's ESRCH is the one callers read,
    // and answering it EPERM instead makes a caller think it was denied rather than that it has none.
    void sys_persona(const macos_syscall_context& c)
    {
        const auto operation = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto idlen = get_macos_syscall_argument(c, 4);

        switch (operation)
        {
        case MACOS_PERSONA_OP_ALLOC:
        case MACOS_PERSONA_OP_PALLOC:
        case MACOS_PERSONA_OP_DEALLOC:
        case MACOS_PERSONA_OP_INFO:
        case MACOS_PERSONA_OP_PIDINFO:
            write_macos_syscall_error(c, MACOS_EPERM);
            return;

        case MACOS_PERSONA_OP_GET:
            write_macos_syscall_error(c, MACOS_ESRCH);
            return;

        case MACOS_PERSONA_OP_FIND:
        case MACOS_PERSONA_OP_FIND_BY_TYPE:
            // The only operations that touch the caller's memory before refusing: the match count is
            // stored before the argument that would have failed the call is looked at.
            if (idlen != 0)
            {
                const size_t none = 0;
                c.emu_ref.memory.try_write_memory(idlen, &none, sizeof(none));
            }

            write_macos_syscall_error(c, MACOS_EINVAL);
            return;

        case MACOS_PERSONA_OP_SUPPORT:
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;

        default:
            write_macos_syscall_error(c, MACOS_ENOSYS);
            return;
        }
    }
}

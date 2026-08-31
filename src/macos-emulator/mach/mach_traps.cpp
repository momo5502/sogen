#include "../std_include.hpp"
#include "io_surface_user_client.hpp"
#include "mach_traps.hpp"

#include <set>
#include <tuple>

#include "../bsd_syscall_dispatcher.hpp"
#include "../macos_emulator.hpp"
#include "mach_msg.hpp"

#include <array>

#include <address_utils.hpp>
#include <guest/guest_memory_object.hpp>

namespace sogen
{
    namespace mach_traps
    {
        namespace
        {
            // mach_port_options_t: flags at +0, mpl.mpl_qlimit at +4, the rest reserved for options this
            // emulator does not model.
            constexpr size_t PORT_OPTIONS_SIZE = 24;
            constexpr size_t PORT_OPTIONS_FLAGS_OFFSET = 0;
            constexpr size_t PORT_OPTIONS_QLIMIT_OFFSET = 4;

            constexpr uint32_t PORT_LIMITS_INFO = 1;
            constexpr uint32_t PORT_RECEIVE_STATUS = 2;
            constexpr uint32_t PORT_LIMITS_INFO_COUNT = 1;
            constexpr uint32_t PORT_RECEIVE_STATUS_COUNT = 10;

            // mach_port_status_t, ten natural_t fields; only the ones this emulator can answer are filled.
            constexpr size_t STATUS_QLIMIT_WORD = 3;
            constexpr size_t STATUS_MSGCOUNT_WORD = 4;
            constexpr size_t STATUS_SRIGHTS_WORD = 6;

            mach::port_name_t argument_name(const macos_syscall_context& c, const size_t index)
            {
                return static_cast<mach::port_name_t>(get_macos_syscall_argument(c, index));
            }

            uint32_t argument_u32(const macos_syscall_context& c, const size_t index)
            {
                return static_cast<uint32_t>(get_macos_syscall_argument(c, index));
            }

            int32_t argument_delta(const macos_syscall_context& c, const size_t index)
            {
                return static_cast<int32_t>(argument_u32(c, index));
            }

            bool write_out_word(const macos_syscall_context& c, const uint64_t address, const uint32_t value)
            {
                const guest_object<uint32_t> out{c.emu_ref.memory, address};
                return out.try_write(value);
            }

            void write_allocated_name(const macos_syscall_context& c, const uint64_t address, const mach::port_name_t name)
            {
                write_mach_result(c, write_out_word(c, address, name) ? mach::kr::success : mach::kr::invalid_address);
            }

            constexpr uint32_t VM_FLAGS_ANYWHERE = 0x1;
            constexpr uint32_t VM_FLAGS_OVERWRITE = 0x4000;

            // Measured against libsystem_kernel on macOS 26.6.1: all four VM traps answer a target that is
            // not the caller's own task with MACH_SEND_INVALID_DEST rather than a kern_return_t, because
            // xnu's kern_kernelrpc.c seeds rv with it before looking the task up.
            constexpr mach::kern_return_t VM_TRAP_INVALID_TARGET = static_cast<mach::kern_return_t>(mach::msgr::send_invalid_dest);

            // Bounds the aligned-base search: find_free_allocation_base only ever moves forward, so a mask
            // that no hole in the map can satisfy would otherwise walk the whole address space.
            constexpr size_t MASKED_BASE_ATTEMPTS = 64;

            bool targets_current_task(const macos_syscall_context& c)
            {
                return argument_name(c, 0) == c.emu_ref.mach.task_self;
            }

            uint64_t find_masked_base(macos_memory_manager& memory, const size_t size, const uint64_t mask)
            {
                uint64_t hint = 0;

                for (size_t attempt = 0; attempt < MASKED_BASE_ATTEMPTS; ++attempt)
                {
                    const auto base = memory.find_free_allocation_base(size, hint, true);
                    if (base == 0 || (base & mask) == 0)
                    {
                        return base;
                    }

                    if (mask > ~uint64_t{0} - base)
                    {
                        return 0;
                    }

                    hint = (base + mask) & ~mask;
                    if (hint <= base)
                    {
                        return 0;
                    }
                }

                return 0;
            }

            void place_allocation(const macos_syscall_context& c, const guest_object<uint64_t>& address_object, const uint64_t requested,
                                  const uint64_t size, const uint32_t flags, const uint64_t mask, const memory_permission permissions)
            {
                const auto rounded = page_align_up(size, MACOS_PAGE_SIZE);
                if (rounded < size)
                {
                    write_mach_result(c, mach::kr::invalid_argument);
                    return;
                }

                // Redundant with find_free_allocation_base and is_reserved_range, which refuse every size
                // this rejects, and deliberately kept for the same reason sys_mmap keeps its own copy: it
                // states the bound while the size is still the raw guest value, before the size_t cast
                // below. No test can tell the two layers apart; a mutation that deletes it stays green.
                if (rounded > MACOS_MAX_MMAP_END_EXCL)
                {
                    write_mach_result(c, mach::kr::no_space);
                    return;
                }

                auto& memory = c.emu_ref.memory;
                const auto length = static_cast<size_t>(rounded);
                const auto anywhere = (flags & VM_FLAGS_ANYWHERE) != 0;

                const auto base = anywhere ? find_masked_base(memory, length, mask) : page_align_down(requested, MACOS_PAGE_SIZE);

                if (!anywhere && (flags & VM_FLAGS_OVERWRITE) != 0)
                {
                    memory.release_memory(base, length);
                }

                if (base == 0 || !memory.allocate_memory(base, length, permissions))
                {
                    write_mach_result(c, mach::kr::no_space);
                    return;
                }

                // xnu leaks the region when the copyout fails; releasing it instead keeps a guest that
                // never learns the address from growing the map without a way to free it again.
                if (!address_object.try_write(base))
                {
                    memory.release_memory(base, length);
                    write_mach_result(c, mach::kr::memory_error);
                    return;
                }

                write_mach_result(c, mach::kr::success);
            }
        }

        memory_permission vm_prot_to_permission(const uint32_t protection)
        {
            return macos_prot_to_permission(static_cast<int32_t>(protection));
        }

        void trap_vm_allocate(const macos_syscall_context& c)
        {
            if (!targets_current_task(c))
            {
                write_mach_result(c, VM_TRAP_INVALID_TARGET);
                return;
            }

            const auto address_pointer = get_macos_syscall_argument(c, 1);
            const auto size = get_macos_syscall_argument(c, 2);
            const auto flags = argument_u32(c, 3);

            const guest_object<uint64_t> address_object{c.emu_ref.memory, address_pointer};
            const auto requested = address_object.try_read();
            if (!requested)
            {
                write_mach_result(c, mach::kr::memory_error);
                return;
            }

            // Measured on macOS 26.6.1: a zero size succeeds here and stores a zero address, where the
            // same size through the map trap below is KERN_INVALID_ARGUMENT.
            if (size == 0)
            {
                write_mach_result(c, address_object.try_write(0) ? mach::kr::success : mach::kr::memory_error);
                return;
            }

            place_allocation(c, address_object, *requested, size, flags, 0, memory_permission::read_write);
        }

        void trap_vm_map(const macos_syscall_context& c)
        {
            if (!targets_current_task(c))
            {
                write_mach_result(c, VM_TRAP_INVALID_TARGET);
                return;
            }

            const auto address_pointer = get_macos_syscall_argument(c, 1);
            const auto size = get_macos_syscall_argument(c, 2);
            const auto mask = get_macos_syscall_argument(c, 3);
            const auto flags = argument_u32(c, 4);
            const auto protection = argument_u32(c, 5);

            const guest_object<uint64_t> address_object{c.emu_ref.memory, address_pointer};
            const auto requested = address_object.try_read();
            if (!requested)
            {
                write_mach_result(c, mach::kr::memory_error);
                return;
            }

            if (size == 0)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            place_allocation(c, address_object, *requested, size, flags, mask, vm_prot_to_permission(protection));
        }

        // Darwin's vm_map_remove reports success for a range that holds no entries at all, so an
        // unmapped or out-of-range address is not an error here; only a size that cannot be rounded to
        // pages is. Measured against _kernelrpc_mach_vm_deallocate_trap on macOS 26.6.1.
        void trap_vm_deallocate(const macos_syscall_context& c)
        {
            if (!targets_current_task(c))
            {
                write_mach_result(c, VM_TRAP_INVALID_TARGET);
                return;
            }

            const auto address = get_macos_syscall_argument(c, 1);
            const auto size = get_macos_syscall_argument(c, 2);

            if (size == 0)
            {
                write_mach_result(c, mach::kr::success);
                return;
            }

            if (page_align_up(size, MACOS_PAGE_SIZE) < size)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            c.emu_ref.memory.release_memory(address, static_cast<size_t>(size));
            write_mach_result(c, mach::kr::success);
        }

        // set_maximum is read and dropped: sogen models one protection per region, so lowering a maximum
        // has nothing to lower. The consequence is that a later widening succeeds here where Darwin
        // answers KERN_PROTECTION_FAILURE.
        void trap_vm_protect(const macos_syscall_context& c)
        {
            if (!targets_current_task(c))
            {
                write_mach_result(c, VM_TRAP_INVALID_TARGET);
                return;
            }

            const auto address = get_macos_syscall_argument(c, 1);
            const auto size = get_macos_syscall_argument(c, 2);
            const auto protection = argument_u32(c, 4);

            if (size == 0)
            {
                write_mach_result(c, mach::kr::success);
                return;
            }

            if (page_align_up(size, MACOS_PAGE_SIZE) < size)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            const auto changed = c.emu_ref.memory.protect_memory(address, static_cast<size_t>(size), vm_prot_to_permission(protection));
            write_mach_result(c, changed ? mach::kr::success : mach::kr::invalid_address);
        }

        // _kernelrpc_mach_vm_purgable_control_trap(task, address, control, int *state). CoreAnimation and
        // CoreGraphics mark their backing stores volatile between frames and read the old state back to
        // find out whether the kernel purged the pages while they were volatile. sogen never purges, so
        // the old state is always VM_PURGABLE_NONVOLATILE and the data is always still there -- which is
        // the answer a machine under no memory pressure gives.
        void trap_vm_purgable_control(const macos_syscall_context& c)
        {
            constexpr uint32_t purgable_set_state = 0;
            constexpr uint32_t purgable_get_state = 1;
            constexpr uint32_t purgable_purge_all = 2;
            constexpr uint32_t purgable_nonvolatile = 0;
            constexpr uint32_t purgable_state_mask = 3;

            if (!targets_current_task(c))
            {
                write_mach_result(c, VM_TRAP_INVALID_TARGET);
                return;
            }

            const auto control = argument_u32(c, 2);
            if (control == purgable_purge_all)
            {
                write_mach_result(c, mach::kr::success);
                return;
            }

            if (control != purgable_set_state && control != purgable_get_state)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            const auto state_pointer = get_macos_syscall_argument(c, 3);
            uint32_t state = 0;
            if (state_pointer == 0 || !c.emu_ref.memory.try_read_memory(state_pointer, &state, sizeof(state)))
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            if (control == purgable_set_state && (state & purgable_state_mask) != purgable_nonvolatile)
            {
                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    c.emu_ref.log.info("mach_vm_purgable_control: a volatile purgeable range is recorded and never purged; every "
                                       "later query answers VM_PURGABLE_NONVOLATILE\n");
                }
            }

            state = purgable_nonvolatile;
            if (!c.emu_ref.memory.try_write_memory(state_pointer, &state, sizeof(state)))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void write_mach_result(const macos_syscall_context& c, const mach::kern_return_t result)
        {
            c.emu.reg(arm64_register::x0, static_cast<uint64_t>(static_cast<uint32_t>(result)));
            c.emu.reg(arm64_register::x1, uint64_t{0});
        }

        void write_mach_port_result(const macos_syscall_context& c, const mach::port_name_t name)
        {
            c.emu.reg(arm64_register::x0, static_cast<uint64_t>(name));
            c.emu.reg(arm64_register::x1, uint64_t{0});
        }

        uint64_t current_thread_id(const macos_syscall_context& c)
        {
            return c.proc.active_thread ? c.proc.active_thread->thread_id : MACH_MAIN_THREAD_ID;
        }

        void trap_task_self(const macos_syscall_context& c)
        {
            write_mach_port_result(c, c.emu_ref.mach.task_self);
        }

        // task_name_for_pid(target_tport, pid, mach_port_name_t* tn). QuartzCore's CA::Context::
        // connect_remote asks for its own name port before it registers with the render server, so an
        // unimplemented trap here stops any process that builds a CAContext.
        void trap_task_name_for_pid(const macos_syscall_context& c)
        {
            const auto pid = static_cast<int32_t>(argument_u32(c, 1));
            const auto out = get_macos_syscall_argument(c, 2);

            if (out == 0)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            // sogen emulates one process. The kernel answers KERN_FAILURE for a pid it cannot find, and
            // that is the honest answer for every pid but the guest's own.
            if (pid != 0 && pid != static_cast<int32_t>(c.emu_ref.process.pid))
            {
                write_mach_result(c, mach::kr::failure);
                return;
            }

            write_allocated_name(c, out, c.emu_ref.mach.ports.insert_send_right(c.emu_ref.mach.task_self));
        }

        void trap_host_self(const macos_syscall_context& c)
        {
            write_mach_port_result(c, c.emu_ref.mach.host_self);
        }

        void trap_thread_self(const macos_syscall_context& c)
        {
            write_mach_port_result(c, c.emu_ref.mach.thread_self_for(current_thread_id(c)));
        }

        // xnu also allocates a dead name here; this namespace has no way to create one without a right
        // that already died, so that flavour is rejected rather than faked.
        void trap_port_allocate(const macos_syscall_context& c)
        {
            const auto right = argument_u32(c, 1);
            const auto out_address = get_macos_syscall_argument(c, 2);

            auto& ports = c.emu_ref.mach.ports;

            mach::port_name_t name = mach::PORT_NULL;
            if (right == static_cast<uint32_t>(mach::right_kind::receive))
            {
                name = ports.allocate_receive_right();
            }
            else if (right == static_cast<uint32_t>(mach::right_kind::port_set))
            {
                name = ports.allocate_port_set();
            }
            else
            {
                write_mach_result(c, mach::kr::invalid_value);
                return;
            }

            if (name == mach::PORT_NULL)
            {
                write_mach_result(c, mach::kr::no_space);
                return;
            }

            write_allocated_name(c, out_address, name);
        }

        void trap_port_deallocate(const macos_syscall_context& c)
        {
            write_mach_result(c, c.emu_ref.mach.ports.deallocate(argument_name(c, 1)));
        }

        void trap_port_mod_refs(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto right = argument_u32(c, 2);
            const auto delta = argument_delta(c, 3);

            write_mach_result(c, c.emu_ref.mach.ports.mod_refs(name, static_cast<mach::right_kind>(right), delta));
        }

        // A name in this namespace is derived from the entry it belongs to, so a right cannot be installed
        // under a caller-chosen name. Only libSystem's own idiom is representable: naming the port whose
        // receive right the caller already holds.
        void trap_port_insert_right(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto poly = argument_name(c, 2);
            const auto poly_disposition = argument_u32(c, 3);

            auto& ports = c.emu_ref.mach.ports;

            const auto* entry = ports.find(poly);
            if (!entry)
            {
                write_mach_result(c, mach::kr::invalid_name);
                return;
            }

            if (name != poly)
            {
                write_mach_result(c, mach::kr::name_exists);
                return;
            }

            if (poly_disposition != mach::disposition::make_send && poly_disposition != mach::disposition::copy_send)
            {
                write_mach_result(c, mach::kr::invalid_value);
                return;
            }

            if (!entry->has_receive || entry->dead)
            {
                write_mach_result(c, mach::kr::invalid_right);
                return;
            }

            if (entry->send_urefs >= mach::mach_port_namespace::MAX_UREFS)
            {
                write_mach_result(c, mach::kr::urefs_overflow);
                return;
            }

            ports.insert_send_right(poly);
            write_mach_result(c, mach::kr::success);
        }

        void trap_port_construct(const macos_syscall_context& c)
        {
            const auto options_address = get_macos_syscall_argument(c, 1);
            const auto context = get_macos_syscall_argument(c, 2);
            const auto out_address = get_macos_syscall_argument(c, 3);

            std::array<uint8_t, PORT_OPTIONS_SIZE> options{};
            if (!c.emu_ref.memory.try_read_memory(options_address, options.data(), options.size()))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            const auto flags = mach::read_u32(options, PORT_OPTIONS_FLAGS_OFFSET);
            const auto queue_limit = mach::read_u32(options, PORT_OPTIONS_QLIMIT_OFFSET);
            const auto limits_requested = (flags & mach::MPO_QLIMIT) != 0;

            if (limits_requested && queue_limit > mach::PORT_QLIMIT_MAX)
            {
                write_mach_result(c, mach::kr::invalid_value);
                return;
            }

            auto& ports = c.emu_ref.mach.ports;

            const auto name = ports.allocate_receive_right();
            auto* entry = ports.find(name);
            if (!entry)
            {
                write_mach_result(c, mach::kr::no_space);
                return;
            }

            if (limits_requested)
            {
                entry->queue_limit = queue_limit;
            }

            if ((flags & mach::MPO_INSERT_SEND_RIGHT) != 0)
            {
                ports.insert_send_right(name);
            }

            if ((flags & mach::MPO_CONTEXT_AS_GUARD) != 0)
            {
                ports.guard(name, context, (flags & mach::MPO_STRICT) != 0);
            }

            write_allocated_name(c, out_address, name);
        }

        void trap_port_destruct(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto send_right_delta = argument_delta(c, 2);
            const auto guard = get_macos_syscall_argument(c, 3);

            write_mach_result(c, c.emu_ref.mach.ports.destruct(name, send_right_delta, guard));
        }

        void trap_reply_port(const macos_syscall_context& c)
        {
            write_mach_port_result(c, c.emu_ref.mach.ports.allocate_receive_right());
        }

        void trap_thread_get_special_reply_port(const macos_syscall_context& c)
        {
            write_mach_port_result(c, c.emu_ref.mach.make_special_reply_port(current_thread_id(c)));
        }

        void trap_semaphore_signal(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 0);
            const auto result = c.emu_ref.mach.semaphore_signal(name);
            write_mach_result(c, result);

            // A __semwait_signal waiter parked on this semaphore is woken by the signal; its rewound svc
            // re-runs on schedule and consumes the count the signal just added.
            if (result == mach::kr::success)
            {
                if (const auto woken = c.proc.wake_semwait_waiter_of(name); woken != 0)
                {
                    c.emu_ref.log.info("waking thread %" PRIu64 " parked in __semwait_signal on semaphore 0x%x\n", woken, name);
                }
            }
        }

        // semaphore_wait has no timeout in its signature, and libdispatch's _dispatch_sema4_wait asserts
        // on anything but KERN_SUCCESS or KERN_ABORTED -- it crashed the guest with x0=49 while this
        // returned KERN_OPERATION_TIMED_OUT for an unsignalled semaphore. So the wait parks, on the same
        // thread state __semwait_signal uses, and semaphore_signal is what ends it.
        //
        // An untimed park carries deadline 0 deliberately: the scheduler's virtual clock fires only
        // deadlines, so a semaphore nobody can signal reports itself as a deadlock rather than being
        // quietly turned into a timeout the caller is not allowed to see.
        void semaphore_park(const macos_syscall_context& c, const mach::port_name_t name, const bool timed, const uint64_t deadline)
        {
            auto* waiter = c.proc.active_thread;

            // Reaching the wait again means the thread was rescheduled onto its own svc, so any earlier
            // park is over whatever happens next.
            if (waiter != nullptr)
            {
                waiter->blocked_on_sem = 0;
                waiter->semwait_deadline = 0;
                waiter->semwait_woken = false;

                if (waiter->semwait_timed_out)
                {
                    waiter->semwait_timed_out = false;
                    write_mach_result(c, mach::kr::operation_timed_out);
                    return;
                }
            }

            auto* semaphore = c.emu_ref.mach.find_semaphore(name);
            if (semaphore == nullptr)
            {
                write_mach_result(c, mach::kr::invalid_name);
                return;
            }

            if (semaphore->value > 0)
            {
                --semaphore->value;
                write_mach_result(c, mach::kr::success);
                return;
            }

            if (timed && deadline == 0)
            {
                write_mach_result(c, mach::kr::operation_timed_out);
                return;
            }

            if (waiter == nullptr)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            waiter->blocked_on_sem = name;
            waiter->semwait_deadline = timed ? deadline : 0;

            if (c.emu_ref.reschedule_away_from_a_blocked_thread())
            {
                // No result is written: the rewound pc re-runs the wait when this thread is scheduled
                // again, and the answer then is the one that counts.
                return;
            }

            waiter->blocked_on_sem = 0;
            waiter->semwait_deadline = 0;

            std::array<char, 192> head{};
            std::snprintf(head.data(), head.size(), "semaphore_wait on semaphore 0x%x with no runnable thread left to signal it", name);

            auto detail = std::string{head.data()};
            for (const auto& frame : c.emu_ref.backtrace(8))
            {
                detail += "\n    " + frame;
            }

            c.emu_ref.record_stop(stop_reason::semwait_signal_deadlock, detail);
            c.emu_ref.stop();
        }

        uint64_t semaphore_deadline(const macos_syscall_context& c, const size_t seconds_index)
        {
            constexpr uint64_t NSEC_PER_SECOND = 1000000000ULL;

            const auto seconds = get_macos_syscall_argument(c, seconds_index);
            const auto nanoseconds = static_cast<uint32_t>(get_macos_syscall_argument(c, seconds_index + 1));
            const auto total = seconds * NSEC_PER_SECOND + nanoseconds;
            if (total == 0)
            {
                return 0;
            }

            const auto frequency = c.emu.read_system_register(3, 3, 14, 0, 0);
            const auto now = c.emu.read_system_register(3, 3, 14, 0, 2);
            return now + (frequency == 0 ? total : total * frequency / NSEC_PER_SECOND);
        }

        void trap_semaphore_wait(const macos_syscall_context& c)
        {
            semaphore_park(c, argument_name(c, 0), false, 0);
        }

        void trap_semaphore_timedwait(const macos_syscall_context& c)
        {
            semaphore_park(c, argument_name(c, 0), true, semaphore_deadline(c, 1));
        }

        // The paired forms signal one semaphore and wait on another as one step. The signal has to
        // happen before the park, or a handoff between two threads deadlocks on itself.
        void signal_paired_semaphore(const macos_syscall_context& c, const mach::port_name_t signal_name)
        {
            if (signal_name == mach::PORT_NULL || c.emu_ref.mach.semaphore_signal(signal_name) != mach::kr::success)
            {
                return;
            }

            if (const auto woken = c.proc.wake_semwait_waiter_of(signal_name); woken != 0)
            {
                c.emu_ref.log.info("waking thread %" PRIu64 " parked on semaphore 0x%x\n", woken, signal_name);
            }
        }

        void trap_semaphore_wait_signal(const macos_syscall_context& c)
        {
            const auto wait_name = argument_name(c, 0);
            if (c.proc.active_thread == nullptr || c.proc.active_thread->blocked_on_sem == 0)
            {
                signal_paired_semaphore(c, argument_name(c, 1));
            }

            semaphore_park(c, wait_name, false, 0);
        }

        void trap_semaphore_timedwait_signal(const macos_syscall_context& c)
        {
            const auto wait_name = argument_name(c, 0);
            if (c.proc.active_thread == nullptr || c.proc.active_thread->blocked_on_sem == 0)
            {
                signal_paired_semaphore(c, argument_name(c, 1));
            }

            semaphore_park(c, wait_name, true, semaphore_deadline(c, 2));
        }

        // semaphore_signal_all releases every waiter at once and leaves the count at zero, rather than
        // adding one per waiter: a semaphore used as a broadcast gate is reset by the broadcast.
        void trap_semaphore_signal_all(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 0);

            auto* semaphore = c.emu_ref.mach.find_semaphore(name);
            if (semaphore == nullptr)
            {
                write_mach_result(c, mach::kr::invalid_name);
                return;
            }

            const auto woken = c.proc.wake_all_semwait_waiters_of(name);
            semaphore->value = static_cast<int32_t>(woken);

            write_mach_result(c, mach::kr::success);
        }

        void trap_host_create_mach_voucher(const macos_syscall_context& c)
        {
            const auto out_address = get_macos_syscall_argument(c, 3);
            const auto name = c.emu_ref.mach.create_voucher();
            if (name == mach::PORT_NULL)
            {
                write_mach_result(c, mach::kr::resource_shortage);
                return;
            }

            const guest_object<uint32_t> out{c.emu_ref.memory, out_address};
            if (!out.try_write(name))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void trap_mach_timebase_info(const macos_syscall_context& c)
        {
            const auto address = get_macos_syscall_argument(c, 0);

            const guest_object<uint32_t> numer{c.emu_ref.memory, address};
            const guest_object<uint32_t> denom{c.emu_ref.memory, address + sizeof(uint32_t)};

            if (!numer.try_write(c.emu_ref.mach.timebase_numer) || !denom.try_write(c.emu_ref.mach.timebase_denom))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void trap_mach_msg2(const macos_syscall_context& c)
        {
            const auto call = mach::decode_msg2_call(c);

            // The syscall trace shows a mach_msg2 as eight opaque words and says nothing about which
            // thread made it, which is the one thing a two-sided IPC wall needs. Verbose-only.
            c.emu_ref.log.info("thread %" PRIu64 " mach_msg2 options 0x%" PRIx64 " routine 0x%x remote 0x%x local 0x%x send %u "
                               "rcv %u on 0x%x\n",
                               current_thread_id(c), call.options, call.header.id, call.header.remote_port, call.header.local_port,
                               call.send_size, call.rcv_size, call.rcv_name);

            const auto result = mach::perform_msg(c.emu_ref, call);

            // The receive parked this thread and the register file now belongs to another one. Writing a
            // result here would write it into that thread's x0, and the parked thread would come back to
            // a receive it never made.
            if (result == mach::msgr::rcv_interrupted)
            {
                return;
            }

            // A caller is free to ignore a mach_msg failure, so an unmodelled one surfaces much later as
            // an unexplained wait somewhere else. Naming it once per (result, port, routine) keeps the
            // first cause findable without turning a retry loop into a wall of text.
            if (result != mach::msgr::success)
            {
                static std::set<std::tuple<uint32_t, mach::port_name_t, int32_t>> reported{};
                if (reported.emplace(result, call.header.remote_port, call.header.id).second)
                {
                    c.emu_ref.log.warn(
                        "mach_msg2 returned 0x%x for routine 0x%x on port 0x%x (options 0x%" PRIx64 ", send %u, rcv %u on port 0x%x)\n",
                        result, call.header.id, call.header.remote_port, call.options, call.send_size, call.rcv_size, call.rcv_name);
                }
            }

            write_mach_result(c, static_cast<mach::kern_return_t>(result));
        }

        void trap_port_get_attributes(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto flavor = argument_u32(c, 2);
            const auto info_address = get_macos_syscall_argument(c, 3);
            const auto count_address = get_macos_syscall_argument(c, 4);

            const auto* entry = c.emu_ref.mach.ports.find(name);
            if (!entry)
            {
                write_mach_result(c, mach::kr::invalid_name);
                return;
            }

            const guest_object<uint32_t> count_object{c.emu_ref.memory, count_address};
            const auto requested = count_object.try_read();
            if (!requested)
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            std::array<uint32_t, PORT_RECEIVE_STATUS_COUNT> info{};
            uint32_t produced = 0;

            switch (flavor)
            {
            case PORT_LIMITS_INFO:
                info[0] = entry->queue_limit;
                produced = PORT_LIMITS_INFO_COUNT;
                break;

            case PORT_RECEIVE_STATUS:
                info[STATUS_QLIMIT_WORD] = entry->queue_limit;
                info[STATUS_MSGCOUNT_WORD] = static_cast<uint32_t>(entry->queue.size());
                info[STATUS_SRIGHTS_WORD] = entry->send_urefs > 0 ? 1 : 0;
                produced = PORT_RECEIVE_STATUS_COUNT;
                break;

            default:
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            if (*requested < produced)
            {
                write_mach_result(c, mach::kr::failure);
                return;
            }

            if (!c.emu_ref.memory.try_write_memory(info_address, info.data(), produced * sizeof(uint32_t)) ||
                !count_object.try_write(produced))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void trap_port_guard(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto guard = get_macos_syscall_argument(c, 2);
            const auto strict = get_macos_syscall_argument(c, 3) != 0;

            write_mach_result(c, c.emu_ref.mach.ports.guard(name, guard, strict));
        }

        void trap_port_unguard(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto guard = get_macos_syscall_argument(c, 2);

            write_mach_result(c, c.emu_ref.mach.ports.unguard(name, guard));
        }

        // os_activity's identifier source. The kernel hands out a run of `count` consecutive ids and
        // returns the first; the guest then uses that run without asking again, which is why the counter
        // has to advance by the whole count rather than by one. Uniqueness within the process is the only
        // property anything depends on -- these end up in log records, not in decisions.
        void trap_request_notification(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto msgid = static_cast<int32_t>(argument_u32(c, 2));
            const auto sync = argument_u32(c, 3);
            const auto notify = argument_name(c, 4);
            const auto previous_address = get_macos_syscall_argument(c, 5);

            auto& ports = c.emu_ref.mach.ports;

            if (!ports.exists(name))
            {
                write_mach_result(c, mach::kr::invalid_name);
                return;
            }

            const auto* notify_entry = ports.find(notify);
            if (notify_entry == nullptr || !notify_entry->has_send_once)
            {
                write_mach_result(c, mach::kr::invalid_right);
                return;
            }

            // The notification fires when the watched name dies. The kernel-object ports this exists for
            // (XPC service ports) never die while the task lives, so remembering the registration is the
            // whole implementation; nothing is ever queued.
            c.emu_ref.mach.notifications.push_back({.watched = name, .msgid = msgid, .sync = sync, .notify = notify});

            if (previous_address != 0 && !write_out_word(c, previous_address, mach::PORT_NULL))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void trap_generate_activity_id(const macos_syscall_context& c)
        {
            const auto count = static_cast<int32_t>(argument_u32(c, 1));
            const auto out_address = get_macos_syscall_argument(c, 2);

            if (count <= 0 || out_address == 0)
            {
                write_mach_result(c, mach::kr::invalid_argument);
                return;
            }

            const auto first = c.emu_ref.mach.next_activity_id;
            c.emu_ref.mach.next_activity_id += static_cast<uint64_t>(count);

            if (!c.emu_ref.memory.try_write_memory(out_address, &first, sizeof(first)))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void trap_port_move_member(const macos_syscall_context& c)
        {
            write_mach_result(c, c.emu_ref.mach.ports.move_member(argument_name(c, 1), argument_name(c, 2)));
        }

        void trap_port_insert_member(const macos_syscall_context& c)
        {
            write_mach_result(c, c.emu_ref.mach.ports.insert_member(argument_name(c, 1), argument_name(c, 2)));
        }

        void trap_port_extract_member(const macos_syscall_context& c)
        {
            write_mach_result(c, c.emu_ref.mach.ports.extract_member(argument_name(c, 1), argument_name(c, 2)));
        }

        void trap_mk_timer_create(const macos_syscall_context& c)
        {
            // The trap returns the timer's port name in x0 rather than a kern_return_t.
            write_mach_port_result(c, c.emu_ref.mach.create_timer());
        }

        void trap_mk_timer_destroy(const macos_syscall_context& c)
        {
            write_mach_result(c, c.emu_ref.mach.destroy_timer(argument_name(c, 0)));
        }

        void trap_mk_timer_arm(const macos_syscall_context& c)
        {
            // (name, expire_time). The return value is whether a deadline was already armed.
            const auto name = argument_name(c, 0);
            const auto deadline = get_macos_syscall_argument(c, 1);
            write_mach_result(c, c.emu_ref.mach.arm_timer(name, deadline) ? 1 : 0);
        }

        void trap_mk_timer_arm_leeway(const macos_syscall_context& c)
        {
            // (name, flags, expire_time, leeway). Leeway only widens the window a real kernel may
            // coalesce inside, and sogen's virtual clock fires a deadline exactly once, so it is read
            // and not used.
            const auto name = argument_name(c, 0);
            const auto deadline = get_macos_syscall_argument(c, 2);
            write_mach_result(c, c.emu_ref.mach.arm_timer(name, deadline) ? 1 : 0);
        }

        void trap_mk_timer_cancel(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 0);
            const auto out_address = get_macos_syscall_argument(c, 1);
            const auto deadline = c.emu_ref.mach.cancel_timer(name);

            if (out_address != 0 && !c.emu_ref.memory.try_write_memory(out_address, &deadline, sizeof(deadline)))
            {
                write_mach_result(c, mach::kr::invalid_address);
                return;
            }

            write_mach_result(c, mach::kr::success);
        }

        void trap_port_type(const macos_syscall_context& c)
        {
            const auto name = argument_name(c, 1);
            const auto out_address = get_macos_syscall_argument(c, 2);

            auto& ports = c.emu_ref.mach.ports;
            if (!ports.exists(name))
            {
                write_mach_result(c, mach::kr::invalid_name);
                return;
            }

            write_mach_result(c, write_out_word(c, out_address, ports.type_of(name)) ? mach::kr::success : mach::kr::invalid_address);
        }
    }

    void bsd_syscall_dispatcher::add_mach_traps()
    {
        this->register_mach_trap(10, mach_traps::trap_vm_allocate, "_kernelrpc_mach_vm_allocate_trap");
        this->register_mach_trap(11, mach_traps::trap_vm_purgable_control, "_kernelrpc_mach_vm_purgable_control_trap");
        this->register_mach_trap(12, mach_traps::trap_vm_deallocate, "_kernelrpc_mach_vm_deallocate_trap");
        this->register_mach_trap(14, mach_traps::trap_vm_protect, "_kernelrpc_mach_vm_protect_trap");
        this->register_mach_trap(15, mach_traps::trap_vm_map, "_kernelrpc_mach_vm_map_trap");
        this->register_mach_trap(16, mach_traps::trap_port_allocate, "_kernelrpc_mach_port_allocate_trap");
        this->register_mach_trap(18, mach_traps::trap_port_deallocate, "_kernelrpc_mach_port_deallocate_trap");
        this->register_mach_trap(19, mach_traps::trap_port_mod_refs, "_kernelrpc_mach_port_mod_refs_trap");
        this->register_mach_trap(20, mach_traps::trap_port_move_member, "_kernelrpc_mach_port_move_member_trap");
        this->register_mach_trap(21, mach_traps::trap_port_insert_right, "_kernelrpc_mach_port_insert_right_trap");
        this->register_mach_trap(22, mach_traps::trap_port_insert_member, "_kernelrpc_mach_port_insert_member_trap");
        this->register_mach_trap(23, mach_traps::trap_port_extract_member, "_kernelrpc_mach_port_extract_member_trap");
        this->register_mach_trap(24, mach_traps::trap_port_construct, "_kernelrpc_mach_port_construct_trap");
        this->register_mach_trap(25, mach_traps::trap_port_destruct, "_kernelrpc_mach_port_destruct_trap");
        this->register_mach_trap(26, mach_traps::trap_reply_port, "mach_reply_port");
        this->register_mach_trap(27, mach_traps::trap_thread_self, "thread_self_trap");
        this->register_mach_trap(28, mach_traps::trap_task_self, "task_self_trap");
        this->register_mach_trap(29, mach_traps::trap_host_self, "host_self_trap");
        this->register_mach_trap(40, mach_traps::trap_port_get_attributes, "_kernelrpc_mach_port_get_attributes_trap");
        this->register_mach_trap(41, mach_traps::trap_port_guard, "_kernelrpc_mach_port_guard_trap");
        this->register_mach_trap(42, mach_traps::trap_port_unguard, "_kernelrpc_mach_port_unguard_trap");
        this->register_mach_trap(33, mach_traps::trap_semaphore_signal, "semaphore_signal_trap");
        this->register_mach_trap(34, mach_traps::trap_semaphore_signal_all, "semaphore_signal_all_trap");
        this->register_mach_trap(36, mach_traps::trap_semaphore_wait, "semaphore_wait_trap");
        this->register_mach_trap(37, mach_traps::trap_semaphore_wait_signal, "semaphore_wait_signal_trap");
        this->register_mach_trap(38, mach_traps::trap_semaphore_timedwait, "semaphore_timedwait_trap");
        this->register_mach_trap(39, mach_traps::trap_semaphore_timedwait_signal, "semaphore_timedwait_signal_trap");
        this->register_mach_trap(50, mach_traps::trap_thread_get_special_reply_port, "thread_get_special_reply_port");
        this->register_mach_trap(47, mach_traps::trap_mach_msg2, "mach_msg2_trap");
        this->register_mach_trap(70, mach_traps::trap_host_create_mach_voucher, "host_create_mach_voucher_trap");
        this->register_mach_trap(89, mach_traps::trap_mach_timebase_info, "mach_timebase_info_trap");
        this->register_mach_trap(91, mach_traps::trap_mk_timer_create, "mk_timer_create");
        this->register_mach_trap(92, mach_traps::trap_mk_timer_destroy, "mk_timer_destroy");
        this->register_mach_trap(93, mach_traps::trap_mk_timer_arm, "mk_timer_arm");
        this->register_mach_trap(94, mach_traps::trap_mk_timer_cancel, "mk_timer_cancel");
        this->register_mach_trap(95, mach_traps::trap_mk_timer_arm_leeway, "mk_timer_arm_leeway");
        this->register_mach_trap(76, mach_traps::trap_port_type, "_kernelrpc_mach_port_type_trap");
        this->register_mach_trap(77, mach_traps::trap_request_notification, "_kernelrpc_mach_port_request_notification_trap");
        this->register_mach_trap(43, mach_traps::trap_generate_activity_id, "mach_generate_activity_id");
        this->register_mach_trap(44, mach_traps::trap_task_name_for_pid, "task_name_for_pid");
        this->register_mach_trap(100, mach_traps::trap_iokit_user_client, "iokit_user_client_trap");
    }
}

#include "std_include.hpp"
#include "macos_emulator.hpp"

#include "mach/mach_exception.hpp"
#include "mach/mach_msg.hpp"
#include "gui/macos_layer_tree.hpp"
#include "module/dyld_cache_pager.hpp"
#include "module/macho_mapping.hpp"

#include <utils/io.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <ranges>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace sogen
{
    namespace
    {
        constexpr std::string_view MACOS_EMULATOR_STATE_VERSION = "macos-emulator-state-v5";

        // Long enough that an idle guest costs no measurable CPU, short enough that a click is not
        // visibly late: a pointer press and its release are 60-100 ms apart when a person clicks.
        constexpr uint32_t HOST_INPUT_POLL_MILLISECONDS = 8;

        void sleep_for_host_input_poll()
        {
#ifdef __EMSCRIPTEN__
            // Not a host sleep: the browser build runs the emulator on the worker thread that also
            // carries frames and input, so this has to unwind through ASYNCIFY and let the worker's
            // message loop run. Blocking it would stop the very thing being waited for.
            emscripten_sleep(HOST_INPUT_POLL_MILLISECONDS);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(HOST_INPUT_POLL_MILLISECONDS));
#endif
        }

        constexpr int arm64_excp_udef = 1;
        constexpr int arm64_excp_swi = 2;
        constexpr int arm64_excp_prefetch_abort = 3;
        constexpr int arm64_excp_data_abort = 4;
        constexpr int arm64_excp_bkpt = 7;

        const char* describe_arm64_exception(const int exception_index)
        {
            switch (exception_index)
            {
            case arm64_excp_udef:
                return "undefined instruction";
            case arm64_excp_prefetch_abort:
                return "prefetch abort";
            case arm64_excp_data_abort:
                return "data abort";
            case arm64_excp_bkpt:
                return "breakpoint";
            default:
                return "cpu exception";
            }
        }

        std::string format_exception_stop_detail(const int exception_index, const uint64_t pc)
        {
            std::array<char, 96> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s index=%d pc=0x%" PRIx64, describe_arm64_exception(exception_index),
                          exception_index, pc);
            return buffer.data();
        }

        const char* describe_memory_operation(const memory_operation operation)
        {
            if (operation == memory_permission::read)
            {
                return "read";
            }

            if (operation == memory_permission::write)
            {
                return "write";
            }

            if (operation == memory_permission::exec)
            {
                return "exec";
            }

            return "access";
        }

        std::string format_memory_violation_stop_detail(const uint64_t address, const size_t size, const memory_operation operation,
                                                        const memory_violation_type type, const uint64_t pc)
        {
            std::array<char, 128> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s %s address=0x%" PRIx64 " size=%zu pc=0x%" PRIx64,
                          type == memory_violation_type::unmapped ? "unmapped" : "protection", describe_memory_operation(operation),
                          address, size, pc);
            return buffer.data();
        }

        struct resolved_application_path
        {
            std::filesystem::path host_path{};
            std::string guest_path{};
        };

        std::optional<std::string> guest_path_from_root(const guest_file_system& file_sys, const std::filesystem::path& host_path)
        {
            const auto& root = file_sys.root();
            if (root.empty())
            {
                return std::nullopt;
            }

            const auto relative = host_path.lexically_normal().lexically_relative(root.lexically_normal());
            if (relative.empty() || relative == "." || *relative.begin() == "..")
            {
                return std::nullopt;
            }

            return guest_file_system::normalize_guest_path_string("/" + relative.generic_string());
        }

        // Resolving through the root first makes a guest path such as /bin/foo prefer <root>/bin/foo over
        // the host /bin/foo when both exist.
        void ensure_environment_entry(std::vector<std::string>& envp, const std::string_view key, const std::string_view value)
        {
            const std::string prefix = std::string{key} + "=";

            for (auto& entry : envp)
            {
                if (entry.starts_with(prefix))
                {
                    entry = prefix + std::string{value};
                    return;
                }
            }

            envp.push_back(prefix + std::string{value});
        }

        resolved_application_path resolve_application_path(const guest_file_system& file_sys, const std::filesystem::path& executable)
        {
            std::error_code error{};
            auto host_path = file_sys.translate(executable.string());

            if (!std::filesystem::exists(host_path, error) && executable.is_absolute() && std::filesystem::exists(executable, error))
            {
                host_path = executable.lexically_normal();

                auto guest_path = guest_path_from_root(file_sys, host_path);
                if (!guest_path)
                {
                    guest_path = guest_file_system::normalize_guest_path_string("/" + host_path.filename().generic_string());
                }

                return {.host_path = std::move(host_path), .guest_path = std::move(*guest_path)};
            }

            auto guest_path = guest_path_from_root(file_sys, host_path);
            if (!guest_path)
            {
                guest_path = guest_file_system::normalize_guest_path_string(executable.generic_string());
            }

            return {.host_path = std::move(host_path), .guest_path = std::move(*guest_path)};
        }
    }

    macos_emulator::macos_emulator(std::unique_ptr<arm64_64_emulator> emu, const std::filesystem::path& emulation_root)
        : emu_(std::move(emu)),
          emulation_root(emulation_root),
          file_sys(emulation_root),
          memory(*this->emu_),
          mod_manager(this->memory)
    {
        this->commpage.setup(this->memory, *this->emu_, this->system_info);
        this->mach.setup(MACH_MAIN_THREAD_ID);
        this->mach.adopt_counter_frequency(this->emu().read_system_register(3, 3, 14, 0, 0));
        this->dispatcher.add_handlers();
        this->setup_hooks();
    }

    // map_macho_from_data does not unwind the segments it already mapped when a later one fails, so a
    // throw here leaves the guest address space partially occupied: the emulator instance is dead and may
    // not be reused or retried at another base.
    void macos_emulator::load_application(const std::filesystem::path& executable, std::vector<std::string> argv,
                                          const std::vector<std::string>& envp)
    {
        const auto application = resolve_application_path(this->file_sys, executable);

        this->file_sys.add_passthrough_prefix(application.host_path.parent_path());
        this->mod_manager.adopt_file_system(this->file_sys);

        this->mod_manager.map_main_modules(application.host_path);

        for (const auto& [base, mod] : this->mod_manager.get_modules())
        {
            this->callbacks.on_module_load(mod);
        }

        if (!this->mod_manager.executable)
        {
            throw std::runtime_error("Failed to map executable: " + executable.string());
        }

        if (argv.empty())
        {
            argv.push_back(application.guest_path);
        }

        const auto& module = *this->mod_manager.executable;
        this->process.setup(*this->emu_, this->memory, module.entry_point, argv, envp, application.guest_path);

        this->log.info("macOS emulator initialized\n");
        this->log.info("  Executable: %s\n", application.host_path.string().c_str());
        this->log.info("  Entry point: 0x%" PRIx64 "\n", module.entry_point);
        this->log.info("  Image: 0x%" PRIx64 " - 0x%" PRIx64 "\n", module.image_start, module.image_start + module.size_of_image);

        if (this->mod_manager.dylinker)
        {
            this->log.info("  Dylinker: %s (loaded at 0x%" PRIx64 ", entry 0x%" PRIx64 ")\n", module.dylinker_path.c_str(),
                           this->mod_manager.dylinker->image_base, this->mod_manager.dylinker->entry_point);
        }
    }

    uint64_t macos_emulator::main_entry_point() const
    {
        const auto* image = this->mod_manager.executable;
        if (image == nullptr || !image->main_entry_offset)
        {
            return 0;
        }

        return image->image_base + *image->main_entry_offset;
    }

    bool macos_emulator::load_executable(const std::filesystem::path& executable, std::vector<std::string> argv,
                                         std::vector<std::string> envp)
    {
        const auto application = resolve_application_path(this->file_sys, executable);

        std::error_code error{};
        const auto status = std::filesystem::status(application.host_path, error);
        if (!std::filesystem::is_regular_file(status))
        {
            this->record_stop(stop_reason::image_load_failure, "Executable not found: " + application.host_path.string());
            return false;
        }

        std::vector<std::byte> data{};
        if (!utils::io::read_file(application.host_path, &data))
        {
            this->record_stop(stop_reason::image_load_failure, "Failed to read Mach-O image: " + application.host_path.string());
            return false;
        }

        bool declares_dylinker = false;

        try
        {
            const auto slice = select_macho_slice(data, application.host_path);
            const auto metadata = read_macho_module_metadata(data, application.host_path, slice, 0);
            declares_dylinker = !metadata.dylinker_path.empty();

            this->pointer_authentication = metadata.is_arm64e();
            this->emu().set_pointer_authentication(this->pointer_authentication);
        }
        catch (const std::exception& e)
        {
            this->record_stop(stop_reason::image_load_failure, e.what());
            return false;
        }

        if (declares_dylinker)
        {
            return this->load_dyld_application(executable, std::move(argv), std::move(envp));
        }

        try
        {
            this->load_application(executable, std::move(argv), envp);
        }
        catch (const std::exception& e)
        {
            this->record_stop(stop_reason::image_load_failure, e.what());
            return false;
        }

        return true;
    }

    bool macos_emulator::load_dyld_application(const std::filesystem::path& executable, std::vector<std::string> argv,
                                               std::vector<std::string> envp)
    {
        const auto application = resolve_application_path(this->file_sys, executable);

        this->file_sys.add_passthrough_prefix(application.host_path.parent_path());
        this->mod_manager.adopt_file_system(this->file_sys);

        try
        {
            this->mod_manager.map_main_modules(application.host_path);
        }
        catch (const std::exception& e)
        {
            this->record_stop(stop_reason::image_load_failure, e.what());
            return false;
        }

        for (const auto& [base, mod] : this->mod_manager.get_modules())
        {
            this->callbacks.on_module_load(mod);
        }

        if (this->mod_manager.executable == nullptr || this->mod_manager.dylinker == nullptr)
        {
            this->record_stop(stop_reason::image_load_failure, "the executable declares no usable dynamic linker");
            return false;
        }

        for (const auto& section : this->mod_manager.dylinker->sections)
        {
            if (section.name != MACOS_DYLD_ALL_IMAGE_INFO_SECTION)
            {
                continue;
            }

            // XNU learns dyld_all_image_infos from the dylinker's own __all_image_info section at exec
            // time and publishes it through task_info(TASK_DYLD_INFO); a debugger and dyld's own
            // notification path both read it from there. The section moved from __DATA to __DATA_DIRTY
            // on macOS 26, so it is located by name and never by segment.
            this->mach.all_image_info_address = section.start;
            this->mach.all_image_info_size = section.length;
            break;
        }

        if (argv.empty())
        {
            argv.push_back(application.guest_path);
        }

        // Both are verified on build 25G76 and neither alone is load-bearing: private mode also removes
        // the handoff to the copy of dyld inside the cache, and PAGEIN_LINKING=0 keeps fixups in
        // userspace where sogen can watch them.
        ensure_environment_entry(envp, "DYLD_SHARED_REGION", "private");
        ensure_environment_entry(envp, "DYLD_PAGEIN_LINKING", "0");

        const auto guest_dylinker = this->mod_manager.executable->dylinker_path;

        if (!this->process.setup_for_dyld(this->emu(), this->memory, this->mod_manager.dylinker->entry_point,
                                          this->mod_manager.executable->image_base, argv, envp, application.guest_path))
        {
            this->record_stop(stop_reason::image_load_failure, "could not lay out the initial process stack");
            return false;
        }

        this->process.apple_strings.executable_file = this->identities.acquire(application.guest_path);
        this->process.apple_strings.dyld_file = this->identities.acquire(guest_dylinker);
        this->process.apple_strings.th_port = this->mach.thread_self_for(this->process.active_thread->thread_id);
        this->process.apple = this->process.apple_strings.render();

        // The stack is laid out twice on purpose. setup_for_dyld needs the thread to exist before a Mach
        // port can be named for it, and that port name has to appear inside apple[]. The second pass is
        // a few hundred bytes of guest writes and is what keeps setup_for_dyld free of any Mach
        // dependency.
        const auto layout =
            build_dyld_kernel_args(this->memory, MACOS_MAIN_STACK_TOP, this->process.stack_base, this->mod_manager.executable->image_base,
                                   this->process.argv, this->process.envp, this->process.apple);
        if (!layout.valid)
        {
            this->record_stop(stop_reason::image_load_failure, "could not lay out the initial process stack");
            return false;
        }

        this->process.kernel_args_pointer = layout.stack_pointer;
        this->emu().reg(arm64_register::sp, layout.stack_pointer);
        this->process.active_thread->save(this->emu());

        this->symbolizer.attach_modules(this->mod_manager);

        // Two guest paths, because the cache moved under the Cryptexes overlay but dyld still looks in
        // the classic location first and a root may only provide one of them. Without a match every
        // address inside the cache reports as a bare number, which is most of the addresses that matter
        // once libSystem is running.
        const auto attached =
            std::ranges::any_of(std::array<std::string_view, 2>{MACOS_DYLD_CACHE_GUEST_PATH, MACOS_DYLD_CACHE_CLASSIC_GUEST_PATH},
                                [this](const std::string_view guest_path) {
                                    return this->symbolizer.attach_shared_cache(this->file_sys.translate(std::string{guest_path}));
                                });

        if (!attached)
        {
            this->log.print(color::yellow, "no shared cache found; addresses inside it will not be attributed\n");
        }

        this->log.info("macOS emulator initialized for dyld\n");
        this->log.info("  Executable: %s\n", application.host_path.string().c_str());
        this->log.info("  Dylinker: %s (entry 0x%" PRIx64 ")\n", guest_dylinker.c_str(), this->mod_manager.dylinker->entry_point);

        return true;
    }

    void macos_emulator::setup_hooks()
    {
        this->emu().hook_instruction(arm64_hookable_instructions::svc,
                                     [this](cpu_interface&, uint64_t) { return this->dispatch_syscall(); });

        // Installing the svc hook above disarms unicorn's own fault reporting, so the catch-all has to go
        // in with it rather than later: cpu_handle_exception sets its `catched` flag once per UC_HOOK_INTR
        // callback *invoked*, not per exception handled, and AArch64 leaves uc->stop_interrupt null, so
        // EXCP_UDEF, EXCP_PREFETCH_ABORT and EXCP_DATA_ABORT stop raising UC_ERR_EXCEPTION as soon as any
        // interrupt hook exists. The guest then re-enters the faulting pc forever: an unbounded run never
        // returns at all, and a counted one burns its whole budget on one instruction that never retires.
        this->emu().hook_interrupt([this](cpu_interface&, const int exception_index) {
            if (exception_index == arm64_excp_swi)
            {
                return;
            }

            const auto pc = this->emu().read_instruction_pointer();
            const auto raised = mach::report_cpu_exception(*this, static_cast<uint32_t>(exception_index), pc);

            this->record_stop(stop_reason::unhandled_cpu_exception, mach::format_exception_detail(*this, raised) + " [" +
                                                                        format_exception_stop_detail(exception_index, pc) + "]");
            this->callbacks.on_cpu_exception(pc, exception_index, describe_arm64_exception(exception_index));
            this->stop();
        });

        // Unicorn reports a fault twice: once here and once as UC_ERR_*_UNMAPPED out of uc_emu_start,
        // which start() catches. Recording the reason here is what keeps the second report from
        // overwriting it with backend_error.
        this->emu().hook_memory_violation([this](cpu_interface&, const uint64_t address, const size_t size,
                                                 const memory_operation operation, const memory_violation_type type) {
            const auto pc = this->emu().read_instruction_pointer();
            const auto raised = mach::report_memory_violation(*this, address, type == memory_violation_type::protection, pc);

            this->record_stop(stop_reason::unhandled_memory_violation,
                              mach::format_exception_detail(*this, raised) + " [" +
                                  format_memory_violation_stop_detail(address, size, operation, type, pc) + "]");
            return memory_violation_continuation::stop;
        });

        // A per-instruction UC_HOOK_CODE callback costs more than the interpreter itself once dyld is
        // running -- reaching main() is 10^7 to 10^8 instructions -- and the block hook carries an exact
        // instruction_count, so the counter keeps its meaning.
        this->emu().hook_basic_block([this](cpu_interface&, const basic_block& block) {
            this->executed_instructions_ += block.instruction_count;
            ++this->executed_basic_blocks_;
        });

        this->counts_instructions_ = true;
    }

    // Unicorn does not re-add the instruction size after an AArch64 interrupt callback, so pc already
    // points past the svc and must be left alone.
    instruction_hook_continuation macos_emulator::dispatch_syscall()
    {
        return this->dispatcher.dispatch(*this);
    }

    bool macos_emulator::activate_thread(const uint64_t thread_id)
    {
        const auto entry = this->process.threads.find(thread_id);
        if (entry == this->process.threads.end() || entry->second.terminated)
        {
            return false;
        }

        auto* previous = this->process.active_thread;
        if (previous != nullptr && !previous->terminated)
        {
            previous->save(this->emu());
        }

        this->process.active_thread = &entry->second;
        entry->second.restore(this->emu());
        return true;
    }

    // A deadline that has not arrived yet is not due. Firing it early is what turns an idle run loop
    // into a spin at full speed: CFRunLoop re-arms a timeout timer on every pass, so a guest that asks
    // to sleep is woken at once, finds nothing to do, and asks again -- 30 billion instructions of it in
    // one measured run. The guest's counter is derived from the host's, so waiting on the host is what
    // makes the guest's own clock reach the deadline.
    void macos_emulator::wait_until(const uint64_t deadline)
    {
        // Bounded, because this runs with the whole emulator stopped: a guest that asks to sleep for a
        // minute must not make the emulator unresponsive for a minute. Past the cap the deadline fires
        // early, which is the old behaviour and still correct, only less restful.
        constexpr uint64_t MAX_WAIT_NANOSECONDS = 20'000'000;
        constexpr uint64_t NSEC_PER_SECOND = 1'000'000'000;

        const auto frequency = this->emu().read_system_register(3, 3, 14, 0, 0);
        const auto now = this->emu().read_system_register(3, 3, 14, 0, 2);
        if (frequency == 0 || deadline <= now)
        {
            return;
        }

        const auto ticks = deadline - now;
        const auto nanoseconds =
            std::min(MAX_WAIT_NANOSECONDS, (ticks / frequency) * NSEC_PER_SECOND + ((ticks % frequency) * NSEC_PER_SECOND) / frequency);

        // The only place a run is idle long enough to read the host. Without this an SDL window collects
        // clicks that reach the guest only when something else happens to wake the emulator, which for a
        // guest parked on its run loop is never.
        this->ui.pump_input(*this);

#ifdef __EMSCRIPTEN__
        // Not a host sleep. The browser build runs the emulator on the worker thread that also carries
        // frames and input, and blocking it would stop both -- but emscripten_sleep does not block: it
        // unwinds through ASYNCIFY, lets the worker's message loop run, and resumes. Waiting for nothing
        // is what the alternative cost: a browser guest whose deadlines all fired early spun its run
        // loop for 1700 s and advanced 116 syscalls.
        emscripten_sleep(static_cast<uint32_t>(nanoseconds / 1'000'000));
#else
        std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
#endif
    }

    bool macos_emulator::reschedule_away_from_a_blocked_thread()
    {
        auto* waiter = this->process.active_thread;
        if (waiter == nullptr)
        {
            return false;
        }

        // The svc has already retired, so pc points past it. Rewinding is what makes the receive run
        // again when this thread is next scheduled instead of falling through as though it had succeeded.
        this->emu().reg(arm64_register::pc, this->emu().read_instruction_pointer() - 4);

        const auto running = waiter->thread_id;

        for (auto& [thread_id, thread] : this->process.threads)
        {
            if (thread_id == running || thread.terminated || thread.blocked())
            {
                continue;
            }

            if (this->activate_thread(thread_id))
            {
                return true;
            }
        }

        // A parked waiter whose semaphore already carries a count has its answer without any timer:
        // wake it as a successful wait instead of letting its deadline fire below. Every in-guest
        // signal wakes a waiter together with the count, so the scheduler never actually meets this
        // state -- the re-check guards counts raised out of band (state restore, direct mach calls).
        for (auto& [thread_id, thread] : this->process.threads)
        {
            if (thread.terminated || thread.blocked_on_sem == 0)
            {
                continue;
            }

            const auto* semaphore = this->mach.find_semaphore(thread.blocked_on_sem);
            if (semaphore == nullptr || semaphore->value == 0)
            {
                continue;
            }

            thread.blocked_on_sem = 0;
            thread.semwait_deadline = 0;
            thread.semwait_woken = true;

            if (this->activate_thread(thread_id))
            {
                return true;
            }
        }

        // Nothing can run, so no signal can arrive from the outside either: the next event a real
        // kernel would deliver is the earliest pending deadline. Firing it is the honest virtual-time
        // step -- the alternative is declaring a deadlock in a process that is merely sleeping. The
        // woken thread re-runs its svc, finds the marker, and returns ETIMEDOUT. Indefinite waiters
        // (deadline 0) get nothing: with nobody left to signal them, they are the deadlock.
        macos_thread* earliest = nullptr;
        for (auto& [thread_id, thread] : this->process.threads)
        {
            if (thread.terminated || thread.timed_wait_deadline() == 0)
            {
                continue;
            }

            if (earliest == nullptr || thread.timed_wait_deadline() < earliest->timed_wait_deadline())
            {
                earliest = &thread;
            }
        }

        // An armed mk_timer is the other kind of pending deadline, and the two share one clock. A run
        // loop with no work left is parked on its wait port with a timer armed against it, so without
        // this the process reads as deadlocked at every idle moment.
        const auto timer = this->mach.earliest_armed_timer();

        // A kqueue EVFILT_TIMER deadline is the third, on the same clock again: no guest action makes it
        // arrive, so with everything parked it is as much "the next event" as a timed park or an armed
        // mk_timer. Firing it queues the knote's event, which a workqueue worker is what collects.
        const auto kqueue_deadline = this->process.kqueues.earliest_timer_deadline();
        if (kqueue_deadline.has_value() && (earliest == nullptr || *kqueue_deadline <= earliest->timed_wait_deadline()) &&
            (!timer.has_value() || *kqueue_deadline <= timer->deadline))
        {
            this->wait_until(*kqueue_deadline);

            const auto now = this->emu().read_system_register(3, 3, 14, 0, 2);
            if (now < *kqueue_deadline)
            {
                // wait_until is capped, so a deadline beyond the cap has not arrived yet. The rewound pc
                // makes re-running the caller's own wait the way to come back and check again.
                return true;
            }

            if (this->process.kqueues.fire_due_timers(now) != 0)
            {
                this->workqueue.wake_parked_worker(*this);
            }

            if (!waiter->blocked())
            {
                return true;
            }

            for (auto& [thread_id, thread] : this->process.threads)
            {
                if (thread_id == running || thread.terminated || thread.blocked())
                {
                    continue;
                }

                if (this->activate_thread(thread_id))
                {
                    return true;
                }
            }
        }

        if (timer.has_value() && (earliest == nullptr || timer->deadline <= earliest->timed_wait_deadline()))
        {
            this->wait_until(timer->deadline);
            this->mach.disarm_timer(timer->name);
            mach::deliver_timer_expiration(*this, timer->name);

            // The expiration may have landed on the port the caller itself parked on, in which case the
            // rewound pc is all it takes: re-running the receive finds the message.
            if (!waiter->blocked())
            {
                return true;
            }

            for (auto& [thread_id, thread] : this->process.threads)
            {
                if (thread_id == running || thread.terminated || thread.blocked())
                {
                    continue;
                }

                if (this->activate_thread(thread_id))
                {
                    return true;
                }
            }
        }

        if (earliest != nullptr)
        {
            earliest->fire_timed_wait();

            if (this->activate_thread(earliest->thread_id))
            {
                return true;
            }
        }

        // Nothing to switch to. The rewind has to be undone, or the caller's own halt would report a pc
        // pointing at the trap rather than after it.
        this->emu().reg(arm64_register::pc, this->emu().read_instruction_pointer() + 4);
        return false;
    }

    bool macos_emulator::can_wake_from_host_input() const
    {
        return this->ui_backend_ && this->ui_backend_->can_deliver_input();
    }

    void macos_emulator::park_for_host_input()
    {
        // The svc has already retired, so pc points past it. Rewinding is what makes the wait run again
        // once something arrives, the way a kernel restarts an interrupted syscall.
        this->emu().reg(arm64_register::pc, this->emu().read_instruction_pointer() - 4);

        const auto before = this->ui.delivered_input_count();

        while (!this->should_stop_ && this->can_wake_from_host_input())
        {
            if (this->on_host_idle)
            {
                this->on_host_idle();
            }

            this->ui.pump_input(*this);

            if (this->ui.delivered_input_count() != before)
            {
                return;
            }

            sleep_for_host_input_poll();
        }
    }

    bool macos_emulator::borrow_a_waiting_thread_for_a_frame()
    {
        // The frame cadence a run with no front-end behind it has is the one the guest asked for, and
        // the screenshots measured against it are what says so. This exists to serve a live front-end:
        // it is the one that shows a repaint and the one that can ask for another.
        if (!this->can_wake_from_host_input() || !this->ui.enabled || this->ui.calls.active())
        {
            return false;
        }

        // The svc has already retired, so pc points past it. The chain unwinds by restoring the register
        // file rather than by returning to lr, so rewinding first is what makes the borrowed thread run
        // its own wait again afterwards -- the way a kernel restarts an interrupted syscall.
        this->emu().reg(arm64_register::pc, this->emu().read_instruction_pointer() - 4);

        if (this->ui.calls.arm_resume(*this))
        {
            // Resolving is what presents: the resolver settles by compositing once the last raster is
            // attached. Presenting here instead would put last frame's picture on screen with the layer
            // the guest just redrew missing from it.
            if (macos_layer_tree_resolve_contents(*this) && this->ui.calls.active())
            {
                return true;
            }

            this->ui.calls.disarm_resume();
        }

        this->emu().reg(arm64_register::pc, this->emu().read_instruction_pointer() + 4);
        return false;
    }

    bool macos_emulator::resume_some_thread()
    {
        const auto running = this->process.active_thread != nullptr ? this->process.active_thread->thread_id : 0;

        auto candidate = this->process.threads.upper_bound(running);
        for (size_t checked = 0; checked < this->process.threads.size(); ++checked)
        {
            if (candidate == this->process.threads.end())
            {
                candidate = this->process.threads.begin();
            }

            const auto thread_id = candidate->first;
            ++candidate;

            if (thread_id != running && this->activate_thread(thread_id))
            {
                return true;
            }
        }

        return false;
    }

    void macos_emulator::record_stop(const stop_reason reason, std::string detail)
    {
        this->last_stop_reason_ = reason;
        this->last_stop_detail_ = std::move(detail);
    }

    macos_emulator::~macos_emulator()
    {
        // The layer tree is keyed by emulator address rather than held as a member, because a
        // macos_native_handler takes no context. Dropping it here is what keeps a recycled address from
        // inheriting a dead run's layers.
        macos_layer_tree_release(*this);
    }

    std::vector<std::string> macos_emulator::backtrace(const size_t limit) const
    {
        auto& cpu = const_cast<macos_emulator*>(this)->emu();
        return this->backtrace_from(cpu.read_instruction_pointer(), cpu.reg(arm64_register::x29), limit);
    }

    std::vector<std::string> macos_emulator::backtrace_from(const uint64_t pc, const uint64_t fp, const size_t limit) const
    {
        std::vector<std::string> frames{};

        frames.push_back(this->symbolizer.format(pc));

        auto frame = fp;

        // AAPCS64: [x29] is the caller's frame pointer and [x29 + 8] its return address. A frame that is
        // not 16-byte aligned, or that does not move upward, is the end of the chain rather than another
        // entry -- following it would print noise that looks like a stack.
        uint64_t previous = 0;
        while (frames.size() < limit && frame != 0 && (frame & 0xF) == 0 && frame > previous)
        {
            uint64_t next_frame = 0;
            uint64_t return_address = 0;

            if (!this->memory.try_read_memory(frame, &next_frame, sizeof(next_frame)) ||
                !this->memory.try_read_memory(frame + 8, &return_address, sizeof(return_address)) || return_address == 0)
            {
                break;
            }

            // Return addresses are signed with PAC on arm64e; the top bits are a signature, not an
            // address, and the symbolizer would not find anything without stripping them.
            frames.push_back(this->symbolizer.format(return_address & 0x0000FFFFFFFFFFFFULL));

            previous = frame;
            frame = next_frame;
        }

        return frames;
    }

    std::vector<std::string> macos_emulator::stack_scan(const uint64_t sp, const size_t limit) const
    {
        std::vector<std::string> frames{};
        if (sp == 0)
        {
            return frames;
        }

        for (uint64_t addr = sp; frames.size() < limit && addr + sizeof(uint64_t) <= sp + 0x4000; addr += sizeof(uint64_t))
        {
            uint64_t word = 0;
            if (!this->memory.try_read_memory(addr, &word, sizeof(word)))
            {
                break;
            }

            const auto origin = this->symbolizer.describe(word & 0x0000FFFFFFFFFFFFULL);
            if (!origin.has_value() || origin->symbol.empty())
            {
                continue;
            }

            // A return address points just past a call; formatting the word itself would misattribute
            // it to whatever follows the call site.
            frames.push_back(this->symbolizer.format(origin->base + origin->offset - 4));
        }

        return frames;
    }

    std::string macos_emulator::dyld_error_message() const
    {
        // dyld_all_image_infos, arm64: version, infoArrayCount, infoArray, notification,
        // processDetachedFromSharedRegion, libSystemInitialized, dyldImageLoadAddress, jitInfo,
        // dyldVersion, then errorMessage at 0x38.
        constexpr uint64_t ERROR_MESSAGE_OFFSET = 0x38;
        constexpr size_t MAX_MESSAGE = 1024;

        if (this->mach.all_image_info_address == 0 || this->mach.all_image_info_size <= ERROR_MESSAGE_OFFSET + sizeof(uint64_t))
        {
            return {};
        }

        uint64_t message_pointer = 0;
        if (!this->memory.try_read_memory(this->mach.all_image_info_address + ERROR_MESSAGE_OFFSET, &message_pointer,
                                          sizeof(message_pointer)) ||
            message_pointer == 0)
        {
            return {};
        }

        std::string message{};
        for (size_t i = 0; i < MAX_MESSAGE; ++i)
        {
            char character = 0;
            if (!this->memory.try_read_memory(message_pointer + i, &character, sizeof(character)) || character == 0)
            {
                break;
            }

            message.push_back(character);
        }

        return message;
    }

    void macos_emulator::start(const size_t count)
    {
        this->should_stop_ = false;
        this->last_stop_reason_ = stop_reason::none;
        this->last_stop_detail_.clear();

        const auto use_count = count > 0;
        const auto target_instructions = this->executed_instructions_ + count;

        while (!this->should_stop_)
        {
            size_t budget = 0;

            if (use_count)
            {
                const auto current_instructions = this->executed_instructions_;
                if (current_instructions >= target_instructions)
                {
                    break;
                }

                budget = static_cast<size_t>(target_instructions - current_instructions);
            }

            try
            {
                this->emu().start(budget);
            }
            catch (const std::exception& e)
            {
                if (this->last_stop_reason_ == stop_reason::none)
                {
                    this->record_stop(stop_reason::backend_error, e.what());
                }

                this->should_stop_ = true;
                break;
            }
            catch (...)
            {
                if (this->last_stop_reason_ == stop_reason::none)
                {
                    this->record_stop(stop_reason::backend_error, "unknown backend error");
                }

                this->should_stop_ = true;
                break;
            }

            if (!this->emu().has_violation())
            {
                break;
            }
        }

        // Without the tally the target is unknowable, but the conclusion is not: every other way out of
        // the loop above records a reason first, so a bounded run that reaches here unexplained is one
        // the backend ended by exhausting the budget it was given.
        const auto reached_bound = this->counts_instructions_ ? this->executed_instructions_ >= target_instructions : true;

        if (use_count && reached_bound && this->last_stop_reason_ == stop_reason::none)
        {
            this->record_stop(stop_reason::instruction_limit, "count=" + std::to_string(count));
        }
    }

    void macos_emulator::stop()
    {
        if (this->last_stop_reason_ == stop_reason::none)
        {
            this->record_stop(stop_reason::explicit_stop);
        }

        this->should_stop_ = true;
        this->emu().stop();
    }

    void macos_emulator::serialize(utils::buffer_serializer& buffer, const bool is_snapshot) const
    {
        buffer.write(std::string{MACOS_EMULATOR_STATE_VERSION});
        this->emu().serialize_state(buffer, is_snapshot);
        buffer.write(this->executed_instructions_);
        buffer.write(this->executed_basic_blocks_);
        buffer.write(this->last_stop_reason_);
        buffer.write(this->last_stop_detail_);
        this->memory.serialize_memory_state(buffer);
        this->identities.serialize(buffer);
        this->process.serialize(buffer);
        this->mach.serialize(buffer);
    }

    void macos_emulator::deserialize(utils::buffer_deserializer& buffer, const bool is_snapshot)
    {
        const auto version = buffer.read<std::string>();
        if (version != MACOS_EMULATOR_STATE_VERSION)
        {
            throw std::runtime_error("Unsupported macOS emulator state version: " + version);
        }

        this->emu().deserialize_state(buffer, is_snapshot);
        buffer.read(this->executed_instructions_);
        buffer.read(this->executed_basic_blocks_);
        buffer.read(this->last_stop_reason_);
        buffer.read(this->last_stop_detail_);
        this->memory.deserialize_memory_state(buffer);
        this->identities.deserialize(buffer);
        this->process.deserialize(buffer);
        this->mach.deserialize(buffer);

        // The commpage is emulator-owned, not guest state: macos_memory_manager refuses to map a backed
        // region into the reserved range a restore comes back through, so it never travels in the
        // snapshot and has to be synthesized again against the restored cpu.
        this->commpage = macos_commpage{};
        this->commpage.setup(this->memory, *this->emu_, this->system_info);

        this->should_stop_ = false;
    }
}

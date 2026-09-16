#pragma once

#include "std_include.hpp"

#include "bsd_syscall_dispatcher.hpp"
#include "gui/macos_ui_state.hpp"
#include "commpage.hpp"
#include "macos_memory_manager.hpp"
#include "macos_emulator_callbacks.hpp"
#include "macos_process_context.hpp"
#include "macos_system_info.hpp"
#include "macos_workqueue.hpp"
#include "mach/mach_kernel.hpp"
#include "module/macos_module_manager.hpp"

#include <atomic>
#include <functional>

#include <arch_emulator.hpp>
#include "macos_file_identity.hpp"
#include "macos_symbolizer.hpp"

#include <guest/guest_file_system.hpp>
#include <hook_interface.hpp>
#include <logger.hpp>
#include <serialization.hpp>
#include <stop_reason.hpp>

namespace sogen
{
    class dyld_cache_pager;
    class macos_native_dispatch;
    class macos_guest_call_stack;

    class macos_emulator
    {
        std::unique_ptr<arm64_64_emulator> emu_{};

      public:
        macos_emulator(std::unique_ptr<arm64_64_emulator> emu, const std::filesystem::path& emulation_root);

        macos_emulator(const macos_emulator&) = delete;
        macos_emulator& operator=(const macos_emulator&) = delete;
        macos_emulator(macos_emulator&&) = delete;
        macos_emulator& operator=(macos_emulator&&) = delete;
        // Out of line: cache_pager holds an incomplete type, and unique_ptr needs the destructor
        // defined where that type is complete.
        ~macos_emulator();

        std::filesystem::path emulation_root{};
        logger log{};
        guest_file_system file_sys{};
        macos_file_identity_table identities{};
        macos_memory_manager memory;
        macos_module_manager mod_manager;
        macos_symbolizer symbolizer{};
        macos_system_info system_info{};
        macos_commpage commpage{};
        std::unique_ptr<dyld_cache_pager> cache_pager{};

        // Normally the pager only takes over where the host cannot map a file, which is the browser. Set
        // this to take that path deliberately: it is the only way to exercise lazy cache paging against
        // the real 5.4 GB cache, since a platform with mmap never reaches it otherwise.
        bool force_lazy_cache_paging{false};

        // Set from the main executable's cpusubtype before anything runs. A plain-arm64 process runs
        // with the pointer-authentication keys off, so the cache's auth pointers have to be left
        // unsigned as well: with the keys off autda is a no-op, and a signature nothing strips would be
        // dereferenced along with the address.
        bool pointer_authentication{true};
        mach_kernel mach{};
        macos_process_context process{};
        bsd_syscall_dispatcher dispatcher{};
        macos_ui_state ui{};
        macos_workqueue workqueue{};

        // The host path of the mapped shared cache, stashed when dyld maps it so subsystems that resolve
        // cache exports at run time (worker spawn, GUI bind) do not each need the guest path translated
        // again. Empty until shared_region_map_and_slide runs.
        std::filesystem::path shared_cache_host_path{};

        void set_ui_backend(std::unique_ptr<ui_backend> backend)
        {
            this->ui_backend_ = backend ? std::move(backend) : std::make_unique<null_ui_backend>();
        }

        ui_backend& ui_host()
        {
            if (!this->ui_backend_)
            {
                this->ui_backend_ = std::make_unique<null_ui_backend>();
            }

            return *this->ui_backend_;
        }

        void set_native_dispatch(macos_native_dispatch* dispatch)
        {
            this->native_dispatch_ = dispatch;
        }

        macos_native_dispatch* native_dispatch() const
        {
            return this->native_dispatch_;
        }

        void set_guest_call_stack(macos_guest_call_stack* calls)
        {
            this->guest_calls_ = calls;
        }

        macos_guest_call_stack* guest_call_stack() const
        {
            return this->guest_calls_;
        }

        void load_application(const std::filesystem::path& executable, std::vector<std::string> argv, const std::vector<std::string>& envp);

        bool load_dyld_application(const std::filesystem::path& executable, std::vector<std::string> argv, std::vector<std::string> envp);

        // Picks the launch path from the image itself: an executable naming a dynamic linker has to begin
        // inside that linker, not at its own entry point, or nothing is bound and its first call into
        // libSystem faults. The choice is made before anything is mapped, because load_dyld_application
        // only discovers the absence of a dylinker after it has already mapped the modules and there is
        // no way back from there.
        bool load_executable(const std::filesystem::path& executable, std::vector<std::string> argv, std::vector<std::string> envp);

        uint64_t main_entry_point() const;

        arm64_64_emulator& emu()
        {
            return *this->emu_;
        }

        const arm64_64_emulator& emu() const
        {
            return *this->emu_;
        }

        macos_emulator_callbacks callbacks{};
        macos_trace_settings trace{};

        void start(size_t count = 0);
        void stop();

        bool activate_thread(uint64_t thread_id);

        // Round-robin over whatever is still runnable. The emulator holds one CPU, so a thread runs
        // until it asks to give the CPU up; nothing preempts it.
        bool resume_some_thread();

        // Parks the running thread on its own svc and switches to a thread that is not itself parked in a
        // wait (mach receive, ulock, __semwait_signal or psynch). False only when no runnable thread and no timed
        // waiter exist: a timed waiter's deadline is fired instead, because with nothing runnable the
        // earliest timer is the next event that can happen. False is then a real deadlock rather than a
        // scheduling decision the emulator has not made yet.
        bool reschedule_away_from_a_blocked_thread();

        // Whether a guest that has run out of work is waiting rather than hung. Nothing inside the guest
        // can wake a thread once every other one is parked, so the only honest discriminator is whether
        // an input source outside it is still attached -- an SDL window or a browser page. Headless
        // front-ends have none, and there an idle guest is a finished run.
        bool can_wake_from_host_input() const;

        // Rewinds the parked thread onto its own svc and polls the host until input reaches the guest or
        // the run is stopped. The caller must return without writing a syscall result: the thread comes
        // back to a receive it never finished making.
        void park_for_host_input();

        // Rasterises whatever the guest's layer tree still owes a frame and presents it, by borrowing a
        // thread whose own wait has just found nothing. True when a guest-call chain was started, and
        // the caller must then return without writing a syscall result: the chain runs as the guest's
        // own code and unwinds by putting the register file back and re-running the wait.
        bool borrow_a_waiting_thread_for_a_frame();

        // Run while park_for_host_input polls. A front-end that drives its input and frame paths from the
        // syscall hook has no other way in: a parked guest makes no syscalls.
        std::function<void()> on_host_idle{};

        // Sleeps until the guest's own counter reaches `deadline`, bounded. Public so a test can pin
        // that an already-passed deadline costs nothing.
        void wait_until(uint64_t deadline);

        void record_stop(stop_reason reason, std::string detail = {});

        // dyld reports why it gave up by leaving a string in dyld_all_image_infos->errorMessage, where
        // the crash reporter would read it. Without this the guest simply aborts: dyld's halt path writes
        // nothing to any descriptor, so a failed launch looks identical to a program calling abort().
        std::string dyld_error_message() const;

        // Walks the frame-pointer chain from the current state, symbolizing each return address. The
        // guest aborts with no message of its own when an initialiser gives up, and this is the only way
        // to find out which one without single-stepping millions of instructions.
        std::vector<std::string> backtrace(size_t limit = 24) const;

        // Same walk from an explicit pc/fp pair -- a parked thread's saved state, whose wait site is
        // otherwise invisible to the deadlock reports.
        std::vector<std::string> backtrace_from(uint64_t pc, uint64_t fp, size_t limit = 24) const;

        // Fallback for a frame chain that breaks early: scans the raw stack for words that resolve into
        // a known module. Noisy by nature -- every candidate is a guess -- so only for reports.
        std::vector<std::string> stack_scan(uint64_t sp, size_t limit = 8) const;

        stop_reason last_stop_reason() const
        {
            return this->last_stop_reason_;
        }

        const std::string& last_stop_detail() const
        {
            return this->last_stop_detail_;
        }

        uint64_t get_executed_instructions() const
        {
            return this->executed_instructions_;
        }

        uint64_t get_executed_basic_blocks() const
        {
            return this->executed_basic_blocks_;
        }

        // False in the browser build, where the basic-block hook cannot be registered. Callers that
        // report a count have to ask, because the tally then stays at zero and reads as "nothing ran"
        // rather than "not measured here".
        bool counts_executed_instructions() const
        {
            return this->counts_instructions_;
        }

        void serialize(utils::buffer_serializer& buffer, bool is_snapshot) const;
        void deserialize(utils::buffer_deserializer& buffer, bool is_snapshot);

      private:
        std::unique_ptr<ui_backend> ui_backend_{};
        macos_native_dispatch* native_dispatch_{nullptr};
        macos_guest_call_stack* guest_calls_{nullptr};

        void setup_hooks();
        instruction_hook_continuation dispatch_syscall();

        std::atomic_bool should_stop_{false};
        uint64_t executed_instructions_{0};
        uint64_t executed_basic_blocks_{0};
        bool counts_instructions_{false};
        stop_reason last_stop_reason_{stop_reason::none};
        std::string last_stop_detail_{};
    };
}

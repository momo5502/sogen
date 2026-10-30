#pragma once

#include "process_manager.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>

namespace sogen
{
    class windows_emulator;

    // The debuggee's end of a debugger attachment. An event stops the thread that reports it until the debugger,
    // talking through the process manager connection, continues it.
    //
    // The debugger's requests run on the emulation thread: some backends only let a vCPU's own thread touch it, and
    // others deadlock when guest memory changes under a running hook. A stopped thread runs them while it waits;
    // otherwise the emulation loop runs them between time slices.
    class debug_port
    {
      public:
        bool is_attached() const;
        void attach();
        // Drops a pending event and lets a thread stopped at one go on as if it had been continued.
        void detach();

        // Runs on the emulation thread with the kernel lock held and returns the debugger's continue status.
        uint32_t report(windows_emulator& win_emu, process_debug_event event);

        // Bracket the emulation loop; requests outside it run on the caller's thread.
        void begin_emulation();
        void end_emulation();

        // The emulation loop calls this without the kernel lock.
        void run_pending_tasks();

        // These run on the process manager connection thread.
        std::optional<process_debug_event> take_event();
        bool continue_event(uint32_t thread_id, uint32_t status);

        // Runs task under the kernel lock on the emulation thread, or right away when emulation is not running.
        void run_on_emulation_thread(windows_emulator& win_emu, const std::function<void()>& task);

      private:
        struct pending_task
        {
            std::function<void()> run{};
            std::promise<void>* done{};
        };

        mutable std::mutex mutex_{};
        std::condition_variable condition_{};
        bool attached_{};
        bool emulating_{};
        std::optional<process_debug_event> undelivered_{};
        std::optional<uint32_t> stopped_thread_{};
        std::optional<uint32_t> continue_status_{};
        std::deque<pending_task> tasks_{};

        void run_tasks(std::unique_lock<std::mutex>& lock);
    };
}

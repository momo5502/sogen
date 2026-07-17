#pragma once

#include <arch_emulator.hpp>

#include "syscall_dispatcher.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)

namespace sogen
{

    struct syscall_context;
    class ui_backend;
    struct process_context;
    class emulator_thread;

    struct window_destroy_step
    {
        hwnd handle{};
        qmsg message{};
    };

    class window_destroy_orchestrator
    {
      public:
        window_destroy_orchestrator(window_destroy_state& state, const syscall_context& c);

        void start(window& root) const;
        std::optional<window_destroy_step> advance() const;

      private:
        hwnd find_window_by_guest_pointer(uint64_t window_ptr) const;
        void unlink_window_from_parent_and_siblings(const window& win) const;
        window_destroy_frame make_frame(const window& win) const;
        void push_frame(const window& win) const;
        void pop_frame_allocation(window_destroy_frame& frame) const;
        std::vector<hwnd> collect_dependents(const window& win) const;
        void finalize_frame(window_destroy_frame& frame, const window& win) const;

        window_destroy_state& state_;
        x86_64_cpu& emu_;
        process_context& proc_;
        const emulator_thread& thread_;
        ui_backend& ui_;
    };

} // namespace sogen

// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

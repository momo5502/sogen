#pragma once

#include "syscall_dispatcher.hpp"

namespace sogen
{

    struct syscall_context;

    struct window_destroy_step
    {
        hwnd handle{};
        qmsg message{};
    };

    class window_destroy_orchestrator
    {
      public:
        explicit window_destroy_orchestrator(window_destroy_state& state);

        void start(const syscall_context& c, window& root);
        std::optional<window_destroy_step> advance(const syscall_context& c);

      private:
        hwnd find_window_by_guest_pointer(const process_context& proc, uint64_t window_ptr) const;
        void unlink_window_from_parent_and_siblings(const syscall_context& c, window& win) const;
        window_destroy_frame make_frame(const syscall_context& c, const window& win) const;
        void push_frame(const syscall_context& c, window& win);
        void pop_frame_allocation(const syscall_context& c, window_destroy_frame& frame) const;
        std::vector<hwnd> collect_dependents(const syscall_context& c, const window& win) const;
        void finalize_frame(const syscall_context& c, window_destroy_frame& frame, window& win) const;

        window_destroy_state& state_;
    };

} // namespace sogen

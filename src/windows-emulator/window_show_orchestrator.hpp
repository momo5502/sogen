#pragma once

#include "syscall_dispatcher.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)

namespace sogen
{

    struct syscall_context;

    struct window_show_step
    {
        hwnd handle{};
        qmsg message{};
    };

    class window_show_orchestrator
    {
      public:
        window_show_orchestrator(window_show_data& data, const syscall_context& c);

        void start_show(const window& win, uint32_t visibility_flags, bool activate_window, bool emit_size_move,
                        bool emit_activate_app) const;
        void start_hide(const window& win, uint32_t visibility_flags) const;
        void set_parent_erase_window(hwnd parent) const;
        void window_position_completed(bool activation_position) const;
        void erase_background_completed(uint64_t result) const;
        std::optional<window_show_step> advance() const;
        void release() const;

      private:
        void allocate_window_positions(const window& win, uint32_t visibility_flags) const;
        void collect_visible_descendants(const window& parent, std::vector<hwnd>& descendants) const;
        void build_descendant_paint(hwnd descendant) const;
        void release_descendant_dc() const;
        window_show_step prepare_step(hwnd handle, qmsg message) const;

        window_show_data& data_;
        const syscall_context& context_;
    };

} // namespace sogen

// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

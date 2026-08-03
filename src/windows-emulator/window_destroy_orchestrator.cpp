#include "window_destroy_orchestrator.hpp"

#include "syscall_utils.hpp"

namespace sogen
{

    window_destroy_orchestrator::window_destroy_orchestrator(window_destroy_state& state, const syscall_context& c)
        : state_(state),
          emu_(c.emu),
          proc_(c.proc),
          thread_(c.thread()),
          ui_(c.win_emu.ui())
    {
    }

    void window_destroy_orchestrator::start(window& root) const
    {
        this->push_frame(root, true);
    }

    std::optional<window_destroy_step> window_destroy_orchestrator::advance() const
    {
        while (!this->state_.frames.empty())
        {
            auto& frame = this->state_.frames.back();
            auto* win = this->proc_.windows.get(frame.handle);
            if (!win || win->thread_id != this->thread_.id)
            {
                this->pop_frame_allocation(frame);
                this->state_.frames.pop_back();
                continue;
            }

            if (frame.unlink_pending && (frame.message_queue.empty() || frame.message_queue.back().message != WM_PARENTNOTIFY))
            {
                this->unlink_window_from_parent_and_siblings(*win);
                frame.unlink_pending = false;
            }

            if (!frame.message_queue.empty())
            {
                const auto m = frame.message_queue.back();
                frame.message_queue.pop_back();
                if (m.message == WM_WINDOWPOSCHANGING)
                {
                    frame.pending_window_pos_address = m.lParam;
                }
                const auto target = m.message == WM_PARENTNOTIFY ? frame.parent_notify_handle : frame.handle;
                return window_destroy_step{.handle = target, .message = m};
            }

            switch (frame.phase)
            {
            case window_destroy_phase::messages:
                frame.phase = window_destroy_phase::children;
                continue;

            case window_destroy_phase::children: {
                const auto dependents = this->collect_dependents(*win);
                frame.phase = window_destroy_phase::nc_destroy;

                for (const auto dependent : std::ranges::reverse_view(dependents))
                {
                    if (auto* dependent_win = this->proc_.windows.get(dependent))
                    {
                        this->push_frame(*dependent_win, false);
                    }
                }
                continue;
            }

            case window_destroy_phase::nc_destroy:
                this->state_.nc_destroy_frames.push_back(std::move(frame));
                this->state_.frames.pop_back();
                continue;

            case window_destroy_phase::finalize:
                throw std::runtime_error("Invalid window destruction phase");
            }
        }

        while (this->state_.nc_destroy_index < this->state_.nc_destroy_frames.size())
        {
            auto& frame = this->state_.nc_destroy_frames[this->state_.nc_destroy_index];
            auto* win = this->proc_.windows.get(frame.handle);
            if (!win || win->thread_id != this->thread_.id)
            {
                this->pop_frame_allocation(frame);
                ++this->state_.nc_destroy_index;
                continue;
            }

            if (frame.phase == window_destroy_phase::nc_destroy)
            {
                frame.phase = window_destroy_phase::finalize;
                return window_destroy_step{
                    .handle = frame.handle,
                    .message = {.message = WM_NCDESTROY, .wParam = 0, .lParam = 0},
                };
            }

            this->finalize_frame(frame, *win);
            ++this->state_.nc_destroy_index;
        }

        this->state_.nc_destroy_frames.clear();
        this->state_.nc_destroy_index = 0;
        return std::nullopt;
    }

    hwnd window_destroy_orchestrator::find_window_by_guest_pointer(const uint64_t window_ptr) const
    {
        if (window_ptr == 0)
        {
            return 0;
        }

        for (const auto& [_, win] : this->proc_.windows)
        {
            if (win.guest.value() == window_ptr)
            {
                return win.handle;
            }
        }

        return 0;
    }

    void window_destroy_orchestrator::unlink_window_from_parent_and_siblings(const window& win) const
    {
        uint64_t parent = 0;
        uint64_t prev = 0;
        uint64_t next = 0;
        win.guest.access([&](USER_WINDOW& guest_win) {
            parent = guest_win.spwndParent;
            prev = guest_win.spwndPrev;
            next = guest_win.spwndNext;
        });

        const auto win_ptr = win.guest.value();

        if (parent != 0)
        {
            emulator_object<USER_WINDOW> parent_obj{this->emu_, parent};
            parent_obj.access([&](USER_WINDOW& parent_guest) {
                if (parent_guest.spwndChild == win_ptr)
                {
                    parent_guest.spwndChild = next;
                }
            });
        }

        if (prev != 0)
        {
            emulator_object<USER_WINDOW> prev_obj{this->emu_, prev};
            prev_obj.access([&](USER_WINDOW& prev_guest) {
                if (prev_guest.spwndNext == win_ptr)
                {
                    prev_guest.spwndNext = next;
                }
            });
        }

        if (next != 0)
        {
            emulator_object<USER_WINDOW> next_obj{this->emu_, next};
            next_obj.access([&](USER_WINDOW& next_guest) {
                if (next_guest.spwndPrev == win_ptr)
                {
                    next_guest.spwndPrev = prev;
                }
            });
        }
    }

    window_destroy_frame window_destroy_orchestrator::make_frame(const window& win, const bool is_direct_target) const
    {
        window_destroy_frame frame{};
        frame.handle = win.handle;

        if (is_direct_target && (win.style & WS_VISIBLE) != 0)
        {
            const auto flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW;
            const EMU_WINDOWPOS changing_position{
                .hwnd = win.handle,
                .hwndInsertAfter = 0,
                .x = 0,
                .y = 0,
                .cx = 0,
                .cy = 0,
                .flags = flags,
            };
            const EMU_WINDOWPOS changed_position{
                .hwnd = win.handle,
                .hwndInsertAfter = 0,
                .x = win.x,
                .y = win.y,
                .cx = win.width,
                .cy = win.height,
                .flags = flags | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE,
            };
            frame.window_pos_alloc = this->emu_.push_stack(changing_position);
            frame.changed_window_pos_alloc = this->emu_.push_stack(changed_position);

            frame.message_queue = {
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_KILLFOCUS, .wParam = 0, .lParam = 0},
            };
            const std::initializer_list<qmsg> hide_messages = {
                {.message = WM_ACTIVATE, .wParam = 0, .lParam = 0},
                {.message = WM_NCACTIVATE, .wParam = FALSE, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = frame.window_pos_alloc.address()},
                {.message = WM_UAHDESTROYWINDOW, .wParam = 0, .lParam = 0},
            };
            frame.message_queue.insert(frame.message_queue.end(), hide_messages);
        }
        else if (is_direct_target)
        {
            frame.message_queue = {
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_UAHDESTROYWINDOW, .wParam = 0, .lParam = 0},
            };
        }
        else
        {
            frame.message_queue = {
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
            };
        }

        if (is_direct_target && (win.style & WS_CHILD) != 0 && (win.ex_style & WS_EX_NOPARENTNOTIFY) == 0 && win.parent_handle != 0)
        {
            uint64_t child_id{};
            win.guest.access([&](const USER_WINDOW& guest_win) { child_id = guest_win.wID; });
            frame.parent_notify_handle = win.parent_handle;
            frame.message_queue.push_back({
                .message = WM_PARENTNOTIFY,
                .wParam = static_cast<uint64_t>(WM_DESTROY) | (static_cast<uint64_t>(child_id & 0xFFFF) << 16),
                .lParam = win.handle,
            });
        }

        return frame;
    }

    void window_destroy_orchestrator::push_frame(const window& win, const bool is_direct_target) const
    {
        this->state_.frames.push_back(this->make_frame(win, is_direct_target));
    }

    void window_destroy_orchestrator::pop_frame_allocation(window_destroy_frame& frame) const
    {
        if (frame.changed_window_pos_alloc)
        {
            this->emu_.pop_stack(frame.changed_window_pos_alloc);
        }
        if (frame.window_pos_alloc)
        {
            this->emu_.pop_stack(frame.window_pos_alloc);
        }
    }

    std::vector<hwnd> window_destroy_orchestrator::collect_dependents(const window& win) const
    {
        std::vector<hwnd> dependents{};

        auto add_dependent = [&](const hwnd dependent) {
            if (dependent == 0 || dependent == win.handle || std::ranges::find(dependents, dependent) != dependents.end())
            {
                return;
            }

            const auto* dependent_win = this->proc_.windows.get(dependent);
            if (!dependent_win || dependent_win->thread_id != win.thread_id)
            {
                return;
            }

            dependents.push_back(dependent);
        };

        uint64_t child = 0;
        win.guest.access([&](const USER_WINDOW& guest_win) { child = guest_win.spwndChild; });

        for (size_t guard = 0; child != 0 && guard < this->proc_.windows.size(); ++guard)
        {
            add_dependent(this->find_window_by_guest_pointer(child));

            emulator_object<USER_WINDOW> child_obj{this->emu_, child};
            child_obj.access([&](const USER_WINDOW& child_guest) { child = child_guest.spwndNext; });
        }

        for (const auto& [_, candidate] : this->proc_.windows)
        {
            if (candidate.owner_handle == win.handle)
            {
                add_dependent(candidate.handle);
            }
        }

        return dependents;
    }

    void window_destroy_orchestrator::finalize_frame(window_destroy_frame& frame, const window& win) const
    {
        this->pop_frame_allocation(frame);
        this->thread_.remove_window_messages(frame.handle);

        win.guest.access([&](USER_WINDOW& guest_win) {
            guest_win.spwndParent = 0;
            guest_win.spwndChild = 0;
            guest_win.spwndOwner = 0;
            guest_win.spwndNext = 0;
            guest_win.spwndPrev = 0;
        });

        this->ui_.destroy_window(frame.handle);
        this->proc_.gdi_window_surfaces.erase(static_cast<uint32_t>(frame.handle));
        (void)this->proc_.windows.erase(frame.handle);
    }

} // namespace sogen

#include "std_include.hpp"
#include "window_destroy_orchestrator.hpp"

#include "syscall_utils.hpp"

namespace sogen
{

    window_destroy_orchestrator::window_destroy_orchestrator(window_destroy_state& state)
        : state_(state)
    {
    }

    void window_destroy_orchestrator::start(const syscall_context& c, window& root)
    {
        this->push_frame(c, root);
    }

    std::optional<window_destroy_step> window_destroy_orchestrator::advance(const syscall_context& c)
    {
        while (!this->state_.frames.empty())
        {
            auto& frame = this->state_.frames.back();
            auto* win = c.proc.windows.get(frame.handle);
            if (!win || win->thread_id != c.proc.active_thread->id)
            {
                this->pop_frame_allocation(c, frame);
                this->state_.frames.pop_back();
                continue;
            }

            if (!frame.message_queue.empty())
            {
                const auto m = frame.message_queue.back();
                frame.message_queue.pop_back();
                return window_destroy_step{.handle = frame.handle, .message = m};
            }

            switch (frame.phase)
            {
            case window_destroy_phase::messages:
                frame.phase = window_destroy_phase::children;
                continue;

            case window_destroy_phase::children: {
                const auto dependents = this->collect_dependents(c, *win);
                frame.phase = window_destroy_phase::nc_destroy;

                for (const auto dependent : std::ranges::reverse_view(dependents))
                {
                    if (auto* dependent_win = c.proc.windows.get(dependent))
                    {
                        this->push_frame(c, *dependent_win);
                    }
                }
                continue;
            }

            case window_destroy_phase::nc_destroy:
                frame.phase = window_destroy_phase::finalize;
                frame.message_queue = {{.message = WM_NCDESTROY, .wParam = 0, .lParam = 0}};
                continue;

            case window_destroy_phase::finalize:
                this->finalize_frame(c, frame, *win);
                this->state_.frames.pop_back();
                continue;
            }
        }

        return std::nullopt;
    }

    hwnd window_destroy_orchestrator::find_window_by_guest_pointer(const process_context& proc, const uint64_t window_ptr) const
    {
        if (window_ptr == 0)
        {
            return 0;
        }

        for (const auto& [_, win] : proc.windows)
        {
            if (win.guest.value() == window_ptr)
            {
                return win.handle;
            }
        }

        return 0;
    }

    void window_destroy_orchestrator::unlink_window_from_parent_and_siblings(const syscall_context& c, window& win) const
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
            emulator_object<USER_WINDOW> parent_obj{c.emu, parent};
            parent_obj.access([&](USER_WINDOW& parent_guest) {
                if (parent_guest.spwndChild == win_ptr)
                {
                    parent_guest.spwndChild = next;
                }
            });
        }

        if (prev != 0)
        {
            emulator_object<USER_WINDOW> prev_obj{c.emu, prev};
            prev_obj.access([&](USER_WINDOW& prev_guest) {
                if (prev_guest.spwndNext == win_ptr)
                {
                    prev_guest.spwndNext = next;
                }
            });
        }

        if (next != 0)
        {
            emulator_object<USER_WINDOW> next_obj{c.emu, next};
            next_obj.access([&](USER_WINDOW& next_guest) {
                if (next_guest.spwndPrev == win_ptr)
                {
                    next_guest.spwndPrev = prev;
                }
            });
        }
    }

    window_destroy_frame window_destroy_orchestrator::make_frame(const syscall_context& c, const window& win) const
    {
        window_destroy_frame frame{};
        frame.handle = win.handle;

        if ((win.style & WS_VISIBLE) != 0)
        {
            EMU_WINDOWPOS wp{};
            wp.hwnd = win.handle;
            wp.hwndInsertAfter = 0;
            wp.x = win.x;
            wp.y = win.y;
            wp.cx = win.width;
            wp.cy = win.height;
            wp.flags = SWP_HIDEWINDOW;
            frame.window_pos_alloc = c.emu.push_stack(wp);

            frame.message_queue = {
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_KILLFOCUS, .wParam = 0, .lParam = 0},
                {.message = WM_ACTIVATE, .wParam = 0, .lParam = 0},
                {.message = WM_NCACTIVATE, .wParam = FALSE, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = frame.window_pos_alloc.address},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = frame.window_pos_alloc.address},
                {.message = WM_UAHDESTROYWINDOW, .wParam = 0, .lParam = 0},
            };
        }
        else
        {
            frame.message_queue = {
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_UAHDESTROYWINDOW, .wParam = 0, .lParam = 0},
            };
        }

        return frame;
    }

    void window_destroy_orchestrator::push_frame(const syscall_context& c, window& win)
    {
        this->unlink_window_from_parent_and_siblings(c, win);
        this->state_.frames.push_back(this->make_frame(c, win));
    }

    void window_destroy_orchestrator::pop_frame_allocation(const syscall_context& c, window_destroy_frame& frame) const
    {
        if (frame.window_pos_alloc.address != 0)
        {
            c.emu.pop_stack(std::move(frame.window_pos_alloc));
        }
    }

    std::vector<hwnd> window_destroy_orchestrator::collect_dependents(const syscall_context& c, const window& win) const
    {
        std::vector<hwnd> dependents{};

        auto add_dependent = [&](const hwnd dependent) {
            if (dependent == 0 || dependent == win.handle || std::ranges::find(dependents, dependent) != dependents.end())
            {
                return;
            }

            const auto* dependent_win = c.proc.windows.get(dependent);
            if (!dependent_win || dependent_win->thread_id != win.thread_id)
            {
                return;
            }

            dependents.push_back(dependent);
        };

        uint64_t child = 0;
        win.guest.access([&](const USER_WINDOW& guest_win) { child = guest_win.spwndChild; });

        for (size_t guard = 0; child != 0 && guard < c.proc.windows.size(); ++guard)
        {
            add_dependent(this->find_window_by_guest_pointer(c.proc, child));

            emulator_object<USER_WINDOW> child_obj{c.emu, child};
            child_obj.access([&](const USER_WINDOW& child_guest) { child = child_guest.spwndNext; });
        }

        for (const auto& [_, candidate] : c.proc.windows)
        {
            if (candidate.owner_handle == win.handle)
            {
                add_dependent(candidate.handle);
            }
        }

        return dependents;
    }

    void window_destroy_orchestrator::finalize_frame(const syscall_context& c, window_destroy_frame& frame, window& win) const
    {
        this->pop_frame_allocation(c, frame);

        win.guest.access([&](USER_WINDOW& guest_win) {
            guest_win.spwndParent = 0;
            guest_win.spwndChild = 0;
            guest_win.spwndOwner = 0;
            guest_win.spwndNext = 0;
            guest_win.spwndPrev = 0;
        });

        c.win_emu.ui().destroy_window(frame.handle);
        (void)c.proc.windows.erase(frame.handle);
    }

} // namespace sogen

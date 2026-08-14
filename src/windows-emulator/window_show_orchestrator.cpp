#include "window_show_orchestrator.hpp"

#include "syscall_utils.hpp"

namespace sogen
{
    namespace
    {
        constexpr uint64_t k_hrgn_window = 1;

        uint64_t pack_message_lparam(const int low, const int high)
        {
            return static_cast<uint64_t>(static_cast<uint16_t>(low)) | (static_cast<uint64_t>(static_cast<uint16_t>(high)) << 16);
        }
    }

    namespace syscalls
    {
        hdc create_gdi_window_dc(const syscall_context& c, hwnd window);
        uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
    }

    window_show_orchestrator::window_show_orchestrator(window_show_data& data, const syscall_context& c)
        : data_(data),
          context_(c)
    {
    }

    void window_show_orchestrator::start_show(const window& win, const uint32_t visibility_flags, const bool activate_window,
                                              const bool emit_size_move, const bool emit_activate_app) const
    {
        this->data_.handle = win.handle;
        this->allocate_window_positions(win, visibility_flags);
        this->data_.erase_background_dc = syscalls::create_gdi_window_dc(this->context_, win.handle);

        if (activate_window)
        {
            const EMU_WINDOWPOS activation_position{
                .hwnd = win.handle,
                .hwndInsertAfter = 0,
                .x = 0,
                .y = 0,
                .cx = 0,
                .cy = 0,
                .flags = SWP_NOMOVE | SWP_NOSIZE,
            };
            this->data_.activation_window_pos_alloc = this->context_.emu.push_stack(activation_position);
        }

        if (emit_size_move)
        {
            this->data_.message_queue.push_back(
                {.message = WM_MOVE, .wParam = 0, .lParam = pack_message_lparam(win.client_x(), win.client_y())});
            this->data_.message_queue.push_back(
                {.message = WM_SIZE, .wParam = 0, .lParam = pack_message_lparam(win.client_width(), win.client_height())});
        }

        this->data_.message_queue.push_back({.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = 0});
        if (this->data_.erase_background_dc != 0)
        {
            this->data_.message_queue.push_back({.message = WM_ERASEBKGND, .wParam = this->data_.erase_background_dc, .lParam = 0});
        }
        this->data_.message_queue.push_back({.message = WM_NCPAINT, .wParam = k_hrgn_window, .lParam = 0});

        if (activate_window)
        {
            this->data_.message_queue.push_back({.message = WM_SETFOCUS, .wParam = 0, .lParam = 0});
            this->data_.message_queue.push_back({.message = WM_ACTIVATE, .wParam = 1, .lParam = 0});
            this->data_.message_queue.push_back({.message = WM_NCACTIVATE, .wParam = TRUE, .lParam = 0});
            if (emit_activate_app)
            {
                this->data_.message_queue.push_back({.message = WM_ACTIVATEAPP, .wParam = TRUE, .lParam = 0});
            }
            this->data_.message_queue.push_back({
                .message = WM_WINDOWPOSCHANGING,
                .wParam = 0,
                .lParam = this->data_.activation_window_pos_alloc.address(),
            });
        }

        this->data_.message_queue.push_back({
            .message = WM_WINDOWPOSCHANGING,
            .wParam = 0,
            .lParam = this->data_.window_pos_alloc.address(),
        });
        this->data_.message_queue.push_back({.message = WM_SHOWWINDOW, .wParam = TRUE, .lParam = 0});
    }

    void window_show_orchestrator::start_hide(const window& win, const uint32_t visibility_flags) const
    {
        this->data_.handle = win.handle;
        this->allocate_window_positions(win, visibility_flags);
        this->data_.message_queue = {
            {.message = WM_KILLFOCUS, .wParam = 0, .lParam = 0},
            {.message = WM_ACTIVATE, .wParam = 0, .lParam = 0},
            {.message = WM_NCACTIVATE, .wParam = FALSE, .lParam = 0},
            {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = 0},
            {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = this->data_.window_pos_alloc.address()},
            {.message = WM_SHOWWINDOW, .wParam = FALSE, .lParam = 0},
        };
    }

    void window_show_orchestrator::set_parent_erase_window(const hwnd parent) const
    {
        this->data_.parent_erase_window = parent;
        this->data_.parent_erase_background_dc = syscalls::create_gdi_window_dc(this->context_, parent);
    }

    void window_show_orchestrator::window_position_completed(const bool activation_position) const
    {
        if (!activation_position && this->data_.parent_erase_window != 0 && this->data_.parent_erase_background_dc != 0)
        {
            this->data_.parent_erase_pending = true;
        }
    }

    void window_show_orchestrator::erase_background_completed(const uint64_t result) const
    {
        if (this->data_.pending_erase_window == 0)
        {
            return;
        }

        if (auto* win = this->context_.proc.windows.get(this->data_.pending_erase_window))
        {
            win->erase_pending = result == 0;
        }
        this->data_.pending_erase_window = 0;
    }

    std::optional<window_show_step> window_show_orchestrator::advance() const
    {
        if (this->data_.parent_erase_pending)
        {
            this->data_.parent_erase_pending = false;
            if (this->context_.proc.windows.get(this->data_.parent_erase_window))
            {
                return this->prepare_step(this->data_.parent_erase_window,
                                          {.message = WM_ERASEBKGND, .wParam = this->data_.parent_erase_background_dc, .lParam = 0});
            }
        }

        while (!this->data_.visible_descendants.empty() || !this->data_.descendant_message_queue.empty() ||
               this->data_.descendant_erase_background_dc != 0)
        {
            if (this->data_.descendant_message_queue.empty())
            {
                this->release_descendant_dc();
                if (this->data_.visible_descendants.empty())
                {
                    break;
                }

                const auto descendant_handle = this->data_.visible_descendants.back();
                if (!this->context_.proc.windows.get(descendant_handle) ||
                    !this->context_.proc.is_window_effectively_visible(descendant_handle))
                {
                    this->data_.visible_descendants.pop_back();
                    continue;
                }

                this->build_descendant_paint(descendant_handle);
            }

            const auto descendant_handle = this->data_.visible_descendants.back();
            if (!this->context_.proc.windows.get(descendant_handle) ||
                !this->context_.proc.is_window_effectively_visible(descendant_handle))
            {
                this->data_.descendant_message_queue.clear();
                this->release_descendant_dc();
                this->data_.visible_descendants.pop_back();
                continue;
            }

            const auto message = this->data_.descendant_message_queue.back();
            this->data_.descendant_message_queue.pop_back();
            if (this->data_.descendant_message_queue.empty())
            {
                this->data_.visible_descendants.pop_back();
            }
            return this->prepare_step(descendant_handle, message);
        }

        if (auto* win = this->context_.proc.windows.get(this->data_.handle); win && !this->data_.message_queue.empty())
        {
            const auto message = this->data_.message_queue.back();
            this->data_.message_queue.pop_back();

            if (message.message == WM_WINDOWPOSCHANGING)
            {
                this->data_.pending_window_pos_address = message.lParam;
            }
            if (message.message == WM_ERASEBKGND)
            {
                this->collect_visible_descendants(*win, this->data_.visible_descendants);
                std::ranges::reverse(this->data_.visible_descendants);
            }

            return this->prepare_step(this->data_.handle, message);
        }

        this->release();
        return std::nullopt;
    }

    window_show_step window_show_orchestrator::prepare_step(const hwnd handle, const qmsg message) const
    {
        if (message.message == WM_ERASEBKGND)
        {
            this->data_.pending_erase_window = handle;
        }
        return {.handle = handle, .message = message};
    }

    void window_show_orchestrator::build_descendant_paint(const hwnd descendant) const
    {
        this->data_.descendant_erase_background_dc = syscalls::create_gdi_window_dc(this->context_, descendant);
        if (this->data_.descendant_erase_background_dc != 0)
        {
            this->data_.descendant_message_queue.push_back(
                {.message = WM_ERASEBKGND, .wParam = this->data_.descendant_erase_background_dc, .lParam = 0});
        }
        this->data_.descendant_message_queue.push_back({.message = WM_NCPAINT, .wParam = k_hrgn_window, .lParam = 0});
    }

    void window_show_orchestrator::release_descendant_dc() const
    {
        if (this->data_.descendant_erase_background_dc != 0)
        {
            (void)syscalls::handle_NtGdiDeleteObjectApp(this->context_, static_cast<uint32_t>(this->data_.descendant_erase_background_dc));
            this->data_.descendant_erase_background_dc = 0;
        }
    }

    void window_show_orchestrator::release() const
    {
        this->release_descendant_dc();

        if (this->data_.activation_window_pos_alloc)
        {
            this->context_.emu.pop_stack(this->data_.activation_window_pos_alloc);
        }
        if (this->data_.changed_window_pos_alloc)
        {
            this->context_.emu.pop_stack(this->data_.changed_window_pos_alloc);
        }
        if (this->data_.window_pos_alloc)
        {
            this->context_.emu.pop_stack(this->data_.window_pos_alloc);
        }

        if (this->data_.erase_background_dc != 0)
        {
            (void)syscalls::handle_NtGdiDeleteObjectApp(this->context_, static_cast<uint32_t>(this->data_.erase_background_dc));
            this->data_.erase_background_dc = 0;
        }
        if (this->data_.parent_erase_background_dc != 0)
        {
            (void)syscalls::handle_NtGdiDeleteObjectApp(this->context_, static_cast<uint32_t>(this->data_.parent_erase_background_dc));
            this->data_.parent_erase_background_dc = 0;
        }
    }

    void window_show_orchestrator::allocate_window_positions(const window& win, const uint32_t visibility_flags) const
    {
        const EMU_WINDOWPOS changing_position{
            .hwnd = win.handle,
            .hwndInsertAfter = 0,
            .x = 0,
            .y = 0,
            .cx = 0,
            .cy = 0,
            .flags = SWP_NOMOVE | SWP_NOSIZE | visibility_flags,
        };
        const EMU_WINDOWPOS changed_position{
            .hwnd = win.handle,
            .hwndInsertAfter = 0,
            .x = win.x,
            .y = win.y,
            .cx = win.width,
            .cy = win.height,
            .flags = SWP_NOMOVE | SWP_NOSIZE | visibility_flags | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE,
        };
        this->data_.window_pos_alloc = this->context_.emu.push_stack(changing_position);
        this->data_.changed_window_pos_alloc = this->context_.emu.push_stack(changed_position);
    }

    void window_show_orchestrator::collect_visible_descendants(const window& parent, std::vector<hwnd>& descendants) const
    {
        for (auto& [index, child] : this->context_.proc.windows)
        {
            (void)index;
            if (child.parent_handle != parent.handle || (child.style & WS_VISIBLE) == 0 ||
                !this->context_.proc.is_window_effectively_visible(child.handle))
            {
                continue;
            }

            descendants.push_back(child.handle);
            this->collect_visible_descendants(child, descendants);
        }
    }

} // namespace sogen

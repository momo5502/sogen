#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    hwnd handle_NtUserSetParent(const syscall_context& c, hwnd hwnd_child, hwnd hwnd_new_parent);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_framed = WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_THICKFRAME;
        constexpr int k_frame = 3;
        constexpr int k_border = 1;

        class WindowScreenRectTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            window& create_window(const hwnd parent, const uint32_t style, const RECT& rect)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.parent_handle = parent != 0 ? parent : this->emu.process.default_desktop_window_handle.bits;
                win.style = style;
                win.x = rect.left;
                win.y = rect.top;
                win.width = rect.right - rect.left;
                win.height = rect.bottom - rect.top;
                win.guest.access([&](USER_WINDOW& guest) { guest.hWnd = handle.bits; });
                this->emu.process.sync_guest_window_rects(win);
                return win;
            }

            static RECT guest_window_rect(const window& win)
            {
                return win.guest.read().rcWindow;
            }

            static RECT guest_client_rect(const window& win)
            {
                return win.guest.read().rcClient;
            }

            windows_emulator emu = create_sample_emulator();
        };

        bool operator==(const RECT& a, const RECT& b)
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }
    }

    TEST_F(WindowScreenRectTest, GuestRectsOfChildWindowsAreInScreenCoordinates)
    {
        auto& parent = this->create_window(0, k_framed, {.left = 100, .top = 50, .right = 400, .bottom = 250});
        auto& child =
            this->create_window(parent.handle, WS_CHILD | WS_VISIBLE | WS_BORDER, {.left = 10, .top = 20, .right = 60, .bottom = 60});

        EXPECT_TRUE((RECT{.left = 100, .top = 50, .right = 400, .bottom = 250}) == guest_window_rect(parent));
        EXPECT_TRUE((RECT{.left = 100 + k_frame, .top = 50 + k_frame, .right = 400 - k_frame, .bottom = 250 - k_frame}) ==
                    guest_client_rect(parent));

        constexpr int child_left = 100 + k_frame + 10;
        constexpr int child_top = 50 + k_frame + 20;
        EXPECT_TRUE((RECT{.left = child_left, .top = child_top, .right = child_left + 50, .bottom = child_top + 40}) ==
                    guest_window_rect(child));
        EXPECT_TRUE((RECT{.left = child_left + k_border,
                          .top = child_top + k_border,
                          .right = child_left + 50 - k_border,
                          .bottom = child_top + 40 - k_border}) == guest_client_rect(child));
    }

    TEST_F(WindowScreenRectTest, DescendantGuestRectsFollowTheirAncestors)
    {
        auto& root = this->create_window(0, k_framed, {.left = 100, .top = 50, .right = 400, .bottom = 250});
        auto& child =
            this->create_window(root.handle, WS_CHILD | WS_VISIBLE | WS_BORDER, {.left = 10, .top = 20, .right = 60, .bottom = 60});
        auto& grandchild = this->create_window(child.handle, WS_CHILD | WS_VISIBLE, {.left = 5, .top = 6, .right = 15, .bottom = 16});

        const auto expect_positions = [&](const int root_client_left, const int root_client_top) {
            EXPECT_EQ(10, child.x);
            EXPECT_EQ(20, child.y);
            EXPECT_EQ(5, grandchild.x);
            EXPECT_EQ(6, grandchild.y);
            EXPECT_EQ(root_client_left + 10, guest_window_rect(child).left);
            EXPECT_EQ(root_client_top + 20, guest_window_rect(child).top);
            EXPECT_EQ(root_client_left + 10 + k_border + 5, guest_window_rect(grandchild).left);
            EXPECT_EQ(root_client_top + 20 + k_border + 6, guest_window_rect(grandchild).top);
        };

        expect_positions(100 + k_frame, 50 + k_frame);

        root.x = 300;
        root.y = 150;
        this->emu.process.sync_guest_window_rects(root);
        expect_positions(300 + k_frame, 150 + k_frame);

        auto& other = this->create_window(0, k_framed, {.left = 500, .top = 400, .right = 700, .bottom = 600});
        syscalls::handle_NtUserSetParent(this->context(), child.handle, other.handle);
        expect_positions(500 + k_frame, 400 + k_frame);
    }
}

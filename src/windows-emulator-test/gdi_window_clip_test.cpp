#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <initializer_list>

namespace sogen::syscalls
{
    hdc handle_NtUserGetDC(const syscall_context& c, hwnd window);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
}

namespace sogen::test
{
    namespace
    {
        constexpr COLORREF k_red = 0x0000FF;
        constexpr COLORREF k_clipped = 0xFFFFFFFF;

        class GdiWindowClipTest : public testing::Test
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

            hwnd create_window(const hwnd parent, const uint32_t style, const RECT& rect)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.parent_handle = parent;
                win.style = style;
                win.x = rect.left;
                win.y = rect.top;
                win.width = rect.right - rect.left;
                win.height = rect.bottom - rect.top;
                win.host_surface_window = parent == 0;
                win.guest.access([&](USER_WINDOW& guest) { guest.hWnd = handle.bits; });
                return win.handle;
            }

            // win32k keeps a parent's children as a list that starts at the front of the Z order.
            void set_z_order(const hwnd parent, const std::initializer_list<hwnd> front_to_back)
            {
                uint64_t next = 0;
                for (auto it = std::rbegin(front_to_back); it != std::rend(front_to_back); ++it)
                {
                    const auto* child = this->emu.process.windows.get(*it);
                    child->guest.access([&](USER_WINDOW& guest) { guest.spwndNext = next; });
                    next = child->guest.value();
                }

                this->emu.process.windows.get(parent)->guest.access([&](USER_WINDOW& guest) { guest.spwndChild = next; });
            }

            COLORREF set_pixel(const hwnd window, const int x, const int y)
            {
                const auto dc = syscalls::handle_NtUserGetDC(this->context(), window);
                return syscalls::handle_NtGdiSetPixel(this->context(), dc, x, y, k_red);
            }

            windows_emulator emu = create_sample_emulator();
        };
    }

    TEST_F(GdiWindowClipTest, ClipSiblingsKeepsAWindowOffTheSiblingsInFrontOfIt)
    {
        const auto top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto back =
            this->create_window(top_level, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, {.left = 0, .top = 0, .right = 20, .bottom = 20});
        const auto front =
            this->create_window(top_level, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, {.left = 10, .top = 10, .right = 30, .bottom = 30});
        this->set_z_order(top_level, {front, back});

        EXPECT_EQ(k_clipped, this->set_pixel(back, 15, 15));
        EXPECT_EQ(k_red, this->set_pixel(back, 5, 5));
        EXPECT_EQ(k_red, this->set_pixel(front, 5, 5));
    }

    TEST_F(GdiWindowClipTest, ClipChildrenKeepsAWindowOffItsChildren)
    {
        const auto top_level =
            this->create_window(0, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto child = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 10, .top = 10, .right = 20, .bottom = 20});
        this->set_z_order(top_level, {child});

        EXPECT_EQ(k_clipped, this->set_pixel(top_level, 15, 15));
        EXPECT_EQ(k_red, this->set_pixel(top_level, 5, 5));
        EXPECT_EQ(k_red, this->set_pixel(child, 5, 5));
    }
}

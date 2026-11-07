#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <initializer_list>
#include <optional>
#include <tuple>

namespace sogen::syscalls
{
    hdc handle_NtUserGetDC(const syscall_context& c, hwnd window);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    BOOL handle_NtUserInvalidateRect(const syscall_context& c, hwnd hwnd, emulator_object<RECT> rect, BOOL erase);
    BOOL handle_NtUserRedrawWindow(const syscall_context& c, hwnd hwnd, emulator_object<RECT> update_rect, uint64_t update_rgn, UINT flags);
    BOOL handle_NtUserGetUpdateRect(const syscall_context& c, hwnd hwnd, emulator_object<RECT> rect, BOOL erase);
}

namespace sogen::test
{
    namespace
    {
        constexpr COLORREF k_red = 0x0000FF;
        constexpr COLORREF k_clipped = 0xFFFFFFFF;

        class WindowPaintingTest : public testing::Test
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

            bool needs_repaint(const hwnd window)
            {
                return syscalls::handle_NtUserGetUpdateRect(this->context(), window, {this->emu.memory}, FALSE) != FALSE;
            }

            std::optional<std::tuple<LONG, LONG, LONG, LONG>> update_rect(const hwnd window)
            {
                const auto rect = this->guest_rect({});
                if (syscalls::handle_NtUserGetUpdateRect(this->context(), window, rect, FALSE) == FALSE)
                {
                    return std::nullopt;
                }
                const auto value = rect.read();
                return std::tuple{value.left, value.top, value.right, value.bottom};
            }

            void invalidate_all(const std::initializer_list<hwnd> windows)
            {
                for (const auto window : windows)
                {
                    syscalls::handle_NtUserInvalidateRect(this->context(), window, {this->emu.memory}, FALSE);
                }
            }

            void redraw(const hwnd window, const UINT flags)
            {
                syscalls::handle_NtUserRedrawWindow(this->context(), window, {this->emu.memory}, 0, flags);
            }

            emulator_object<RECT> guest_rect(const RECT& rect)
            {
                if (this->rect_storage == 0)
                {
                    this->rect_storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                }

                const emulator_object<RECT> object{this->emu.memory, this->rect_storage};
                object.write(rect);
                return object;
            }

            windows_emulator emu = create_sample_emulator();
            emulator_pointer rect_storage{};
        };
    }

    TEST_F(WindowPaintingTest, ClipSiblingsKeepsAWindowOffTheSiblingsInFrontOfIt)
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

    TEST_F(WindowPaintingTest, ClipChildrenKeepsAWindowOffItsChildren)
    {
        const auto top_level =
            this->create_window(0, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto child = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 10, .top = 10, .right = 20, .bottom = 20});
        this->set_z_order(top_level, {child});

        EXPECT_EQ(k_clipped, this->set_pixel(top_level, 15, 15));
        EXPECT_EQ(k_red, this->set_pixel(top_level, 5, 5));
        EXPECT_EQ(k_red, this->set_pixel(child, 5, 5));
    }

    TEST_F(WindowPaintingTest, ValidatingPartOfAWindowKeepsTheChildrenOutsideThatPart)
    {
        const auto top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto left = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 20, .bottom = 20});
        const auto right = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 20, .top = 0, .right = 40, .bottom = 20});
        this->set_z_order(top_level, {left, right});

        syscalls::handle_NtUserInvalidateRect(this->context(), left, {this->emu.memory}, FALSE);
        syscalls::handle_NtUserInvalidateRect(this->context(), right, {this->emu.memory}, FALSE);
        ASSERT_TRUE(this->needs_repaint(left));
        ASSERT_TRUE(this->needs_repaint(right));

        syscalls::handle_NtUserRedrawWindow(this->context(), top_level, this->guest_rect({.left = 0, .top = 0, .right = 20, .bottom = 20}),
                                            0, RDW_VALIDATE);

        EXPECT_FALSE(this->needs_repaint(left));
        EXPECT_TRUE(this->needs_repaint(right));
    }

    // RedrawWindow and InvalidateRect reach the children of a window without WS_CLIPCHILDREN, recursively until a window with
    // WS_CLIPCHILDREN; RDW_ALLCHILDREN and RDW_NOCHILDREN override that, and RDW_VALIDATE follows the same rule.
    TEST_F(WindowPaintingTest, InvalidatingAParentInvalidatesThePartOfEachChildItCovers)
    {
        const auto top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto covered = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 10, .top = 10, .right = 30, .bottom = 30});
        const auto grandchild = this->create_window(covered, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 5, .bottom = 5});
        const auto outside = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 30, .top = 0, .right = 40, .bottom = 10});

        syscalls::handle_NtUserInvalidateRect(this->context(), top_level,
                                              this->guest_rect({.left = 0, .top = 0, .right = 20, .bottom = 20}), FALSE);

        EXPECT_EQ(std::tuple(0, 0, 10, 10), this->update_rect(covered));
        EXPECT_EQ(std::tuple(0, 0, 5, 5), this->update_rect(grandchild));
        EXPECT_FALSE(this->update_rect(outside));
    }

    TEST_F(WindowPaintingTest, InvalidationStopsAtAWindowWithClipChildren)
    {
        const auto top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto clipping =
            this->create_window(top_level, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, {.left = 0, .top = 0, .right = 20, .bottom = 20});
        const auto grandchild = this->create_window(clipping, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 5, .bottom = 5});

        this->invalidate_all({top_level});

        EXPECT_TRUE(this->needs_repaint(clipping));
        EXPECT_FALSE(this->needs_repaint(grandchild));
    }

    TEST_F(WindowPaintingTest, AllChildrenReachesPastClipChildrenAndNoChildrenStopsAtTheWindow)
    {
        const auto top_level =
            this->create_window(0, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto child = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 20, .bottom = 20});
        const auto other_top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto other_child =
            this->create_window(other_top_level, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 20, .bottom = 20});

        this->redraw(top_level, RDW_INVALIDATE | RDW_ALLCHILDREN);
        this->redraw(other_top_level, RDW_INVALIDATE | RDW_NOCHILDREN);

        EXPECT_TRUE(this->needs_repaint(child));
        EXPECT_TRUE(this->needs_repaint(other_top_level));
        EXPECT_FALSE(this->needs_repaint(other_child));
    }

    TEST_F(WindowPaintingTest, ValidatingFollowsTheSameChildRule)
    {
        const auto top_level =
            this->create_window(0, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto child = this->create_window(top_level, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 20, .bottom = 20});
        const auto other_top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
        const auto other_child =
            this->create_window(other_top_level, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 20, .bottom = 20});

        this->invalidate_all({top_level, child, other_top_level, other_child});
        this->redraw(top_level, RDW_VALIDATE);
        this->redraw(other_top_level, RDW_VALIDATE | RDW_NOCHILDREN);

        EXPECT_FALSE(this->needs_repaint(top_level));
        EXPECT_TRUE(this->needs_repaint(child));
        EXPECT_FALSE(this->needs_repaint(other_top_level));
        EXPECT_TRUE(this->needs_repaint(other_child));

        this->redraw(top_level, RDW_VALIDATE | RDW_ALLCHILDREN);
        EXPECT_FALSE(this->needs_repaint(child));
    }
}

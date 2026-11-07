#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <tuple>

namespace sogen::syscalls
{
    hwnd handle_NtUserSetParent(const syscall_context& c, hwnd hwnd_child, hwnd hwnd_new_parent);
    hdc handle_NtUserGetDC(const syscall_context& c, hwnd window);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    BOOL handle_NtUserGetUpdateRect(const syscall_context& c, hwnd hwnd, emulator_object<RECT> rect, BOOL erase);
}

namespace sogen::test
{
    namespace
    {
        struct relationship
        {
            hwnd window;
            hwnd parent;
            hwnd owner;
            bool top_level;

            bool operator==(const relationship&) const = default;
        };

        class recording_ui_backend final : public ui_backend
        {
          public:
            explicit recording_ui_backend(std::vector<relationship>& relationships)
                : relationships_(relationships)
            {
            }

            void set_event_sink(event_sink /*sink*/) override
            {
            }

            void pump_events() override
            {
            }

            void set_window_relationship(const hwnd window, const hwnd parent, const hwnd owner, const bool top_level) override
            {
                this->relationships_.push_back({window, parent, owner, top_level});
            }

          private:
            std::vector<relationship>& relationships_;
        };

        windows_emulator create_recording_emulator(std::vector<relationship>& relationships)
        {
            emulator_interfaces interfaces{};
            interfaces.ui = std::make_unique<recording_ui_backend>(relationships);
            return create_sample_emulator(emulator_settings{.use_relative_time = true}, {}, {}, std::move(interfaces));
        }

        class WindowRelationshipTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->desktop = this->emu.process.default_desktop_window_handle.bits;
                this->top_level = this->create_window(0, WS_POPUP | WS_VISIBLE, {.left = 0, .top = 0, .right = 40, .bottom = 30});
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            hwnd create_window(const hwnd parent, const uint32_t style, const RECT& rect)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.parent_handle = parent != 0 ? parent : this->desktop;
                win.thread_id = this->emu.vcpu(0).active_thread->id;
                win.style = style;
                win.x = rect.left;
                win.y = rect.top;
                win.width = rect.right - rect.left;
                win.height = rect.bottom - rect.top;
                win.host_surface_window = true;
                win.guest.access([&](USER_WINDOW& guest) { guest.hWnd = handle.bits; });
                return win.handle;
            }

            bool is_surface_root(const hwnd window)
            {
                return this->emu.process.is_surface_root(*this->emu.process.windows.get(window));
            }

            std::optional<std::tuple<LONG, LONG, LONG, LONG>> update_rect(const hwnd window)
            {
                const emulator_object<RECT> rect{this->emu.memory, this->emu.memory.allocate_memory(0x1000, memory_permission::read_write)};
                if (syscalls::handle_NtUserGetUpdateRect(this->context(), window, rect, FALSE) == FALSE)
                {
                    return std::nullopt;
                }
                const auto value = rect.read();
                return std::tuple{value.left, value.top, value.right, value.bottom};
            }

            bool has_surface(const hwnd window)
            {
                return this->emu.process.gdi_window_surfaces.contains(static_cast<uint32_t>(window));
            }

            std::vector<relationship> relationships{};
            windows_emulator emu = create_recording_emulator(this->relationships);
            hwnd desktop{};
            hwnd top_level{};
        };
    }

    TEST_F(WindowRelationshipTest, OnlyChildrenOfARealWindowShareItsSurface)
    {
        const auto child = this->create_window(this->top_level, WS_CHILD, {});
        const auto child_of_desktop = this->create_window(0, WS_CHILD, {});
        const auto owned_popup = this->create_window(0, WS_POPUP, {});
        const auto child_and_popup = this->create_window(this->top_level, WS_CHILD | WS_POPUP, {});

        EXPECT_TRUE(this->is_surface_root(this->top_level));
        EXPECT_FALSE(this->is_surface_root(child));
        EXPECT_TRUE(this->is_surface_root(child_of_desktop));
        EXPECT_TRUE(this->is_surface_root(owned_popup));
        EXPECT_TRUE(this->is_surface_root(child_and_popup));
    }

    TEST_F(WindowRelationshipTest, AChildMovedUnderTheDesktopGetsAHostAndItsOldParentRepaintsWhereItWas)
    {
        const auto child = this->create_window(this->top_level, WS_CHILD | WS_VISIBLE, {.left = 10, .top = 10, .right = 20, .bottom = 25});

        EXPECT_EQ(this->top_level, syscalls::handle_NtUserSetParent(this->context(), child, 0));

        EXPECT_TRUE(this->is_surface_root(child));
        EXPECT_EQ((std::vector<relationship>{{child, 0, 0, true}}), this->relationships);
        EXPECT_EQ(std::tuple(10, 10, 20, 25), this->update_rect(this->top_level));
        EXPECT_TRUE(this->update_rect(child));
    }

    TEST_F(WindowRelationshipTest, AWindowMovedUnderAParentLosesItsOwnSurface)
    {
        const auto window = this->create_window(0, WS_CHILD | WS_VISIBLE, {.left = 0, .top = 0, .right = 10, .bottom = 10});
        const auto dc = syscalls::handle_NtUserGetDC(this->context(), window);
        syscalls::handle_NtGdiSetPixel(this->context(), dc, 1, 1, 0x0000FF);
        ASSERT_TRUE(this->has_surface(window));

        syscalls::handle_NtUserSetParent(this->context(), window, this->top_level);

        EXPECT_FALSE(this->is_surface_root(window));
        EXPECT_FALSE(this->has_surface(window));
        EXPECT_EQ((std::vector<relationship>{{window, this->top_level, 0, false}}), this->relationships);
    }
}

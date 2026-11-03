#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreatePen(const syscall_context& c, uint32_t style, uint32_t width, uint32_t color);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
    DWORD handle_NtUserGetGuiResources(const syscall_context& c, handle process, DWORD flags);
    BOOL handle_NtUserGetWindowDisplayAffinity(const syscall_context& c, hwnd window, emulator_object<DWORD> affinity);
    BOOL handle_NtUserShowOwnedPopups(const syscall_context& c, hwnd owner, BOOL show);
    BOOL handle_NtUserGetCurrentInputMessageSource(const syscall_context& c, emulator_pointer source);
}

namespace sogen::test
{
    namespace
    {
        constexpr DWORD untouched_error = 0xDEAD;
        constexpr DWORD untouched_value = 0xAAAAAAAA;

        class UserQueryTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->set_last_error(untouched_error);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            DWORD last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void set_last_error(const DWORD error)
            {
                this->emu.vcpu(0).active_thread->teb64->access([&](TEB64& teb) { teb.LastErrorValue = error; });
            }

            hwnd create_window(const hwnd parent, const hwnd owner, const uint32_t style)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.thread_id = this->emu.vcpu(0).active_thread->id;
                win.parent_handle = parent;
                win.owner_handle = owner;
                win.style = style;
                return handle.bits;
            }

            hwnd create_top_level_window(const hwnd owner = 0, const uint32_t style = 0)
            {
                return this->create_window(this->emu.process.default_desktop_window_handle.bits, owner, style);
            }

            DWORD gui_resources(const handle process, const DWORD flags)
            {
                return syscalls::handle_NtUserGetGuiResources(this->context(), process, flags);
            }

            BOOL affinity(const hwnd window)
            {
                this->emu.memory.write_memory(this->storage, untouched_value);
                return syscalls::handle_NtUserGetWindowDisplayAffinity(this->context(), window, {this->emu.memory, this->storage});
            }

            DWORD affinity_value()
            {
                return this->emu.memory.read_memory<DWORD>(this->storage);
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t storage{};
        };
    }

    TEST_F(UserQueryTest, GuiResourcesRejectUnknownFlagsAndNonProcessHandles)
    {
        for (const DWORD flags : {5u, 8u, 0x10u, 0xFFFFFFFFu})
        {
            this->set_last_error(untouched_error);
            EXPECT_EQ(0u, this->gui_resources(CURRENT_PROCESS, flags));
            EXPECT_EQ(87u, this->last_error());
        }

        const auto event_handle = this->emu.process.events.store(event{});
        for (const auto process : {handle{}, make_handle(0x1234), event_handle})
        {
            this->set_last_error(untouched_error);
            EXPECT_EQ(0u, this->gui_resources(process, gui_resources_gdiobjects));
            EXPECT_EQ(87u, this->last_error());
        }

        for (const DWORD flags : {0u, 1u, 2u, 3u, 4u})
        {
            this->set_last_error(untouched_error);
            EXPECT_EQ(0u, this->gui_resources(CURRENT_PROCESS, flags));
            EXPECT_EQ(untouched_error, this->last_error());
        }

        for (const DWORD flags : {0u, 1u, 2u})
        {
            this->set_last_error(untouched_error);
            this->gui_resources(make_handle(gui_resources_global_process), flags);
            EXPECT_EQ(untouched_error, this->last_error());
        }
    }

    TEST_F(UserQueryTest, GuiResourcesAcceptAnotherProcessWithoutError)
    {
        emulator_process child{};
        child.process.value = 1;
        const auto child_handle = this->emu.process.processes.store(std::move(child));

        for (const auto other_process : {child_handle, handle{STEAM_PROCESS_HANDLE}})
        {
            for (const DWORD flags : {0u, 1u, 2u, 3u, 4u})
            {
                this->set_last_error(untouched_error);
                this->gui_resources(other_process, flags);
                EXPECT_EQ(untouched_error, this->last_error());
            }
        }

        ASSERT_TRUE(this->emu.process.processes.erase(child_handle));
        this->set_last_error(untouched_error);
        EXPECT_EQ(0u, this->gui_resources(child_handle, gui_resources_gdiobjects));
        EXPECT_EQ(87u, this->last_error());
    }

    TEST_F(UserQueryTest, GuiResourcesCountProcessObjectsAndKeepPeaks)
    {
        const auto counts = [&] {
            return std::array{this->gui_resources(CURRENT_PROCESS, gui_resources_gdiobjects),
                              this->gui_resources(CURRENT_PROCESS, gui_resources_userobjects),
                              this->gui_resources(CURRENT_PROCESS, gui_resources_gdiobjects_peak),
                              this->gui_resources(CURRENT_PROCESS, gui_resources_userobjects_peak)};
        };

        ASSERT_NE(STATUS_UNSUCCESSFUL, syscalls::handle_NtGdiInit(this->context()));
        EXPECT_EQ((std::array<DWORD, 4>{0, 0, 0, 0}), counts());

        std::array<uint32_t, 10> pens{};
        for (auto& pen : pens)
        {
            pen = static_cast<uint32_t>(syscalls::handle_NtGdiCreatePen(this->context(), 0, 1, 0x030201));
            ASSERT_NE(0u, pen);
        }
        EXPECT_EQ((std::array<DWORD, 4>{10, 0, 10, 0}), counts());

        const auto first = this->create_top_level_window();
        const auto second = this->create_top_level_window();
        EXPECT_EQ((std::array<DWORD, 4>{10, 2, 10, 2}), counts());

        for (const auto pen : pens)
        {
            EXPECT_EQ(1u, syscalls::handle_NtGdiDeleteObjectApp(this->context(), pen));
        }
        EXPECT_TRUE(this->emu.process.windows.erase(first));
        EXPECT_TRUE(this->emu.process.windows.erase(second));

        this->set_last_error(untouched_error);
        EXPECT_EQ((std::array<DWORD, 4>{0, 0, 10, 2}), counts());
        EXPECT_EQ(untouched_error, this->last_error());
    }

    TEST_F(UserQueryTest, DisplayAffinityIsNoneForTopLevelWindowsOnly)
    {
        const auto top_level = this->create_top_level_window();
        const auto child = this->create_window(top_level, 0, WS_CHILD);

        ASSERT_EQ(TRUE, this->affinity(top_level));
        EXPECT_EQ(0u, this->affinity_value());
        EXPECT_EQ(untouched_error, this->last_error());

        for (const auto window : {child, this->emu.process.default_desktop_window_handle.bits})
        {
            EXPECT_EQ(FALSE, this->affinity(window));
            EXPECT_EQ(87u, this->last_error());
            EXPECT_EQ(untouched_value, this->affinity_value());
        }

        const auto destroyed = this->create_top_level_window();
        EXPECT_TRUE(this->emu.process.windows.erase(destroyed));
        for (const hwnd window : {hwnd{0}, hwnd{0x1234}, destroyed})
        {
            this->set_last_error(untouched_error);
            EXPECT_EQ(FALSE, this->affinity(window));
            EXPECT_EQ(1400u, this->last_error());
            EXPECT_EQ(untouched_value, this->affinity_value());
        }

        EXPECT_EQ(FALSE, syscalls::handle_NtUserGetWindowDisplayAffinity(this->context(), top_level, {this->emu.memory, 0}));
        EXPECT_EQ(998u, this->last_error());
    }

    TEST_F(UserQueryTest, ShowOwnedPopupsSucceedsWhenNoOwnedWindowChanges)
    {
        const auto owner = this->create_top_level_window();
        const auto hidden_popup = this->create_top_level_window(owner, WS_POPUP);

        for (const BOOL show : {FALSE, FALSE, TRUE, TRUE})
        {
            EXPECT_EQ(TRUE, syscalls::handle_NtUserShowOwnedPopups(this->context(), owner, show));
        }
        EXPECT_EQ(0u, this->emu.process.windows.get(hidden_popup)->style & WS_VISIBLE);

        const auto popup = this->create_top_level_window(owner, WS_POPUP | WS_VISIBLE);
        EXPECT_EQ(TRUE, syscalls::handle_NtUserShowOwnedPopups(this->context(), owner, TRUE));
        EXPECT_EQ(TRUE, syscalls::handle_NtUserShowOwnedPopups(this->context(), popup, FALSE));
        EXPECT_NE(0u, this->emu.process.windows.get(popup)->style & WS_VISIBLE);

        EXPECT_EQ(untouched_error, this->last_error());

        for (const hwnd window : {hwnd{0}, hwnd{0x1234}})
        {
            this->set_last_error(untouched_error);
            EXPECT_EQ(FALSE, syscalls::handle_NtUserShowOwnedPopups(this->context(), window, FALSE));
            EXPECT_EQ(1400u, this->last_error());
        }
    }

    TEST_F(UserQueryTest, CurrentInputMessageSourceIsUnavailableOutsideInput)
    {
        this->emu.memory.write_memory(this->storage, std::array<DWORD, 2>{untouched_value, untouched_value});
        ASSERT_EQ(TRUE, syscalls::handle_NtUserGetCurrentInputMessageSource(this->context(), this->storage));
        EXPECT_EQ((std::array<DWORD, 2>{0, 0}), (this->emu.memory.read_memory<std::array<DWORD, 2>>(this->storage)));

        EXPECT_EQ(untouched_error, this->last_error());

        EXPECT_EQ(FALSE, syscalls::handle_NtUserGetCurrentInputMessageSource(this->context(), 0));
        EXPECT_EQ(87u, this->last_error());

        EXPECT_EQ(FALSE, syscalls::handle_NtUserGetCurrentInputMessageSource(this->context(), 0x1234));
        EXPECT_EQ(998u, this->last_error());
    }
}

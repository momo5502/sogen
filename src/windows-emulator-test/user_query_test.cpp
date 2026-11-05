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
    BOOL handle_NtUserGetTitleBarInfo(const syscall_context& c, hwnd window_handle, emulator_pointer info_address);
    BOOL handle_NtUserRegisterRawInputDevices(const syscall_context& c, emulator_pointer devices, uint32_t device_count, uint32_t size);
    uint32_t handle_NtUserGetRegisteredRawInputDevices(const syscall_context& c, emulator_pointer devices, emulator_pointer device_count,
                                                       uint32_t size);
}

namespace sogen::test
{
    namespace
    {
        constexpr DWORD untouched_error = 0xDEAD;
        constexpr DWORD untouched_value = 0xAAAAAAAA;
        constexpr uint32_t k_overlapped_window = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

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

            BOOL title_bar_info(const hwnd window, const DWORD size)
            {
                EMU_TITLEBARINFO info{};
                info.cbSize = size;
                this->emu.memory.write_memory(this->storage, info);
                return syscalls::handle_NtUserGetTitleBarInfo(this->context(), window, this->storage);
            }

            EMU_TITLEBARINFO read_title_bar_info()
            {
                return this->emu.memory.read_memory<EMU_TITLEBARINFO>(this->storage);
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

    TEST_F(UserQueryTest, TitleBarInfoRejectsAWrongSizeAndAnInvalidWindow)
    {
        const auto window = this->create_top_level_window(0, k_overlapped_window);

        EXPECT_EQ(FALSE, this->title_bar_info(window, sizeof(EMU_TITLEBARINFO) + 4));
        EXPECT_EQ(87u, this->last_error());

        this->set_last_error(untouched_error);
        EXPECT_EQ(FALSE, this->title_bar_info(0x12345, sizeof(EMU_TITLEBARINFO)));
        EXPECT_EQ(1400u, this->last_error());
    }

    // The states were measured on Windows 11 for windows with the same styles.
    TEST_F(UserQueryTest, TitleBarStatesFollowTheWindowStyle)
    {
        struct style_case
        {
            uint32_t style;
            uint32_t ex_style;
            std::array<DWORD, 6> states;
        };

        const std::array cases{
            style_case{k_overlapped_window, 0, {0x100000, 0x8000, 0, 0, 0x8000, 0}},
            style_case{WS_CAPTION | WS_SYSMENU, 0, {0x100000, 0x8000, 0x8000, 0x8000, 0x8000, 0}},
            style_case{WS_CAPTION, 0, {0x100000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000}},
            style_case{WS_POPUP, 0, {0x108000, 0, 0, 0, 0, 0}},
            style_case{k_overlapped_window, WS_EX_TOOLWINDOW, {0x100000, 0x8000, 0, 0, 0x8000, 0}},
            style_case{WS_CAPTION | WS_SYSMENU, WS_EX_CONTEXTHELP, {0x100000, 0x8000, 0x8000, 0x8000, 0, 0}},
        };

        for (const auto& [style, ex_style, states] : cases)
        {
            const auto window = this->create_top_level_window(0, style);
            this->emu.process.windows.get(window)->ex_style = ex_style;

            ASSERT_EQ(TRUE, this->title_bar_info(window, sizeof(EMU_TITLEBARINFO))) << std::hex << style;
            const auto info = this->read_title_bar_info();
            EXPECT_EQ(states, std::to_array(info.rgstate)) << std::hex << style << " " << ex_style;
        }
    }

    TEST_F(UserQueryTest, TitleBarOfAWindowWithoutCaptionIsEmpty)
    {
        const auto window = this->create_top_level_window(0, WS_POPUP);
        auto* win = this->emu.process.windows.get(window);
        win->x = 100;
        win->y = 100;
        win->width = 400;
        win->height = 300;

        ASSERT_EQ(TRUE, this->title_bar_info(window, sizeof(EMU_TITLEBARINFO)));
        const auto rect = this->read_title_bar_info().rcTitleBar;
        EXPECT_EQ(0, rect.left);
        EXPECT_EQ(0, rect.top);
        EXPECT_EQ(0, rect.right);
        EXPECT_EQ(0, rect.bottom);
    }

    // The results were measured on Windows 11 with GetRegisteredRawInputDevices in a 64-bit process, whose calls go to the
    // syscall unchanged.
    TEST_F(UserQueryTest, RegisteredRawInputDevicesFollowTheWindowsSemantics)
    {
        constexpr uint32_t k_ridev_nolegacy = 0x30;
        constexpr uint64_t count_address_offset = 0x800;
        constexpr uint32_t device_size = sizeof(RAWINPUTDEVICE64);
        constexpr auto failed = static_cast<uint32_t>(-1);
        const auto window = this->create_top_level_window(0, k_overlapped_window);
        const uint64_t count_address = this->storage + count_address_offset;

        const auto registered = [&](const emulator_pointer devices, const uint32_t capacity, const uint32_t size) {
            this->emu.memory.write_memory(count_address, capacity);
            this->set_last_error(untouched_error);
            return syscalls::handle_NtUserGetRegisteredRawInputDevices(this->context(), devices, count_address, size);
        };
        const auto count = [&] { return this->emu.memory.read_memory<uint32_t>(count_address); };
        const auto register_devices = [&](const std::vector<RAWINPUTDEVICE64>& devices) {
            this->emu.memory.write_memory(this->storage, devices.data(), devices.size() * sizeof(RAWINPUTDEVICE64));
            return syscalls::handle_NtUserRegisterRawInputDevices(this->context(), this->storage, static_cast<uint32_t>(devices.size()),
                                                                  device_size);
        };

        EXPECT_EQ(0u, registered(0, 5, device_size));
        EXPECT_EQ(0u, count());
        EXPECT_EQ(failed, registered(this->storage, 0, device_size));
        EXPECT_EQ(87u, this->last_error());

        ASSERT_EQ(TRUE, register_devices({{.usUsagePage = 1, .usUsage = 6, .dwFlags = k_ridev_nolegacy, .hwndTarget = window},
                                          {.usUsagePage = 1, .usUsage = 2, .dwFlags = 0, .hwndTarget = 0},
                                          {.usUsagePage = 1, .usUsage = 5, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window}}));

        EXPECT_EQ(0u, registered(0, 0, device_size));
        EXPECT_EQ(3u, count());
        EXPECT_EQ(failed, registered(this->storage, 2, device_size));
        EXPECT_EQ(3u, count());
        EXPECT_EQ(122u, this->last_error());
        EXPECT_EQ(failed, registered(this->storage, 8, device_size + 4));
        EXPECT_EQ(8u, count());
        EXPECT_EQ(87u, this->last_error());

        const uint64_t untouched_device = 0xAAAAAAAAAAAAAAAAULL;
        this->emu.memory.write_memory(this->storage + (3 * device_size), untouched_device);
        ASSERT_EQ(3u, registered(this->storage, 8, device_size));
        EXPECT_EQ(8u, count());
        const auto device = [&](const uint32_t index) {
            return this->emu.memory.read_memory<RAWINPUTDEVICE64>(this->storage + (index * device_size));
        };
        EXPECT_EQ(2, device(0).usUsage);
        EXPECT_EQ(0u, device(0).dwFlags);
        EXPECT_EQ(0u, device(0).hwndTarget);
        EXPECT_EQ(5, device(1).usUsage);
        EXPECT_EQ(static_cast<uint32_t>(RIDEV_INPUTSINK), device(1).dwFlags);
        EXPECT_EQ(window, device(1).hwndTarget);
        EXPECT_EQ(6, device(2).usUsage);
        EXPECT_EQ(k_ridev_nolegacy, device(2).dwFlags);
        EXPECT_EQ(untouched_device, this->emu.memory.read_memory<uint64_t>(this->storage + (3 * device_size)));

        ASSERT_EQ(TRUE, register_devices({{.usUsagePage = 1, .usUsage = 2, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window},
                                          {.usUsagePage = 1, .usUsage = 6, .dwFlags = RIDEV_REMOVE, .hwndTarget = 0}}));
        ASSERT_EQ(2u, registered(this->storage, 8, device_size));
        EXPECT_EQ(2, device(0).usUsage);
        EXPECT_EQ(static_cast<uint32_t>(RIDEV_INPUTSINK), device(0).dwFlags);
        EXPECT_EQ(window, device(0).hwndTarget);
        EXPECT_EQ(5, device(1).usUsage);
    }

    // Measured on Windows 11 like the test above: the count only has to be writable when it is written, which a successful call
    // does not do, and the whole buffer the count names must be writable before anything is written into it.
    TEST_F(UserQueryTest, RegisteredRawInputDevicesNeedWritableOutput)
    {
        constexpr uint32_t device_size = sizeof(RAWINPUTDEVICE64);
        constexpr auto failed = static_cast<uint32_t>(-1);
        constexpr DWORD error_noaccess = 998;

        const std::array registrations{RAWINPUTDEVICE64{.usUsagePage = 1, .usUsage = 2, .dwFlags = 0, .hwndTarget = 0},
                                       RAWINPUTDEVICE64{.usUsagePage = 1, .usUsage = 6, .dwFlags = 0, .hwndTarget = 0}};
        this->emu.memory.write_memory(this->storage, registrations.data(), sizeof(registrations));
        ASSERT_EQ(TRUE, syscalls::handle_NtUserRegisterRawInputDevices(this->context(), this->storage, 2, device_size));

        const auto writable = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        const auto registered = [&](const emulator_pointer devices, const emulator_pointer count, const uint32_t capacity) {
            this->emu.memory.write_memory(count, capacity);
            this->set_last_error(untouched_error);
            return syscalls::handle_NtUserGetRegisteredRawInputDevices(this->context(), devices, count, device_size);
        };
        const auto count_at = [&](const emulator_pointer count) { return this->emu.memory.read_memory<uint32_t>(count); };

        EXPECT_EQ(failed, registered(0, read_only, 0));
        EXPECT_EQ(error_noaccess, this->last_error());
        EXPECT_EQ(0u, count_at(read_only));

        EXPECT_EQ(failed, registered(writable, read_only, 1));
        EXPECT_EQ(error_noaccess, this->last_error());
        EXPECT_EQ(1u, count_at(read_only));

        EXPECT_EQ(2u, registered(writable, read_only, 8));
        EXPECT_EQ(8u, count_at(read_only));

        const auto count = this->storage + 0x800;
        for (const uint32_t capacity : {1u, 8u})
        {
            EXPECT_EQ(failed, registered(read_only, count, capacity));
            EXPECT_EQ(error_noaccess, this->last_error());
            EXPECT_EQ(capacity, count_at(count));
        }

        const auto pages = this->emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.protect_memory(pages + 0x1000, 0x1000, memory_permission::read));
        const auto ending_before_read_only = pages + 0x1000 - (2 * device_size);
        const std::array<uint8_t, 2 * device_size> untouched{};
        this->emu.memory.write_memory(ending_before_read_only, untouched.data(), untouched.size());
        EXPECT_EQ(failed, registered(ending_before_read_only, count, 8));
        EXPECT_EQ(error_noaccess, this->last_error());
        EXPECT_EQ(0, this->emu.memory.read_memory<RAWINPUTDEVICE64>(ending_before_read_only).usUsage);
    }
}

#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtUserCreateWindowStation(const syscall_context& c,
                                              emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                              ACCESS_MASK desired_access);
    uint64_t handle_NtUserCreateDesktopEx(const syscall_context& c,
                                          emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                          emulator_pointer device, emulator_pointer devmode, DWORD flags, ACCESS_MASK desired_access,
                                          ULONG heap_size);
    BOOL handle_NtUserSetProcessWindowStation(const syscall_context& c, handle station);
    BOOL handle_NtUserCloseWindowStation(const syscall_context& c, handle station);
    BOOL handle_NtUserCloseDesktop(const syscall_context& c, handle desktop_handle);
    hdesk handle_NtUserOpenInputDesktop(const syscall_context& c, DWORD flags, BOOL inherit, ACCESS_MASK desired_access);
    hdesk handle_NtUserGetThreadDesktop(const syscall_context& c, ULONG thread_id);
    emulator_pointer handle_NtUserMapDesktopObject(const syscall_context& c, handle handle);
    BOOL handle_NtUserGetObjectInformation(const syscall_context& c, handle object, int index, uint64_t information, DWORD length,
                                           emulator_object<DWORD> needed);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG obj_openif = 0x80;
        constexpr uint64_t interactive_window_station = 1;
        constexpr uint32_t wsf_visible = 1;

        class WindowStationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));

                this->storage = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);

                desktop default_desktop{};
                default_desktop.name = u"Default";
                default_desktop.window_station = interactive_window_station;
                this->emu->process.default_desktop = this->emu->process.desktops.store(std::move(default_desktop));
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> attributes(const std::optional<std::u16string_view>& name,
                                                                                 const ULONG flags = obj_openif)
            {
                emulator_allocator allocator{this->emu->memory, this->storage, 0x2000};
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.Attributes = flags;
                if (name)
                {
                    values.ObjectName = allocator.make_unicode_string(*name).value();
                }

                const auto object = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                object.write(values);
                return object;
            }

            uint64_t create_station(const std::optional<std::u16string_view>& name, const ULONG flags = obj_openif)
            {
                return syscalls::handle_NtUserCreateWindowStation(this->context(), this->attributes(name, flags), 0);
            }

            uint64_t create_desktop(const std::u16string_view name)
            {
                return syscalls::handle_NtUserCreateDesktopEx(this->context(), this->attributes(name), 0, 0, 0, 0, 0);
            }

            DWORD last_error()
            {
                return this->emu->vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            struct user_object_flags
            {
                BOOL inherit;
                BOOL reserved;
                DWORD flags;
            };

            user_object_flags object_flags(const uint64_t object)
            {
                EXPECT_EQ(TRUE, syscalls::handle_NtUserGetObjectInformation(this->context(), make_handle(object), 1, this->storage + 0x1800,
                                                                            sizeof(user_object_flags), {this->emu->memory}));
                return this->emu->memory.read_memory<user_object_flags>(this->storage + 0x1800);
            }

            BOOL receives_input(const uint64_t desktop_object)
            {
                constexpr int uoi_io = 6;
                EXPECT_EQ(TRUE, syscalls::handle_NtUserGetObjectInformation(this->context(), make_handle(desktop_object), uoi_io,
                                                                            this->storage + 0x1800, sizeof(BOOL), {this->emu->memory}));
                return this->emu->memory.read_memory<BOOL>(this->storage + 0x1800);
            }

            std::u16string object_name(const uint64_t object)
            {
                constexpr int uoi_name = 2;
                std::array<char16_t, 64> name{};
                EXPECT_EQ(TRUE, syscalls::handle_NtUserGetObjectInformation(this->context(), make_handle(object), uoi_name,
                                                                            this->storage + 0x1800, sizeof(name), {this->emu->memory}));
                this->emu->memory.read_memory(this->storage + 0x1800, name.data(), sizeof(name));
                return name.data();
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(WindowStationTest, CreatingAnExistingStationOpensIt)
    {
        EXPECT_EQ(interactive_window_station, this->create_station(u"WinSta0"));
        EXPECT_EQ(interactive_window_station, this->create_station(u"winsta0"));

        const auto created = this->create_station(u"Sandbox");
        ASSERT_NE(0u, created);
        EXPECT_EQ(created, this->create_station(u"SANDBOX"));
    }

    TEST_F(WindowStationTest, CreateOnlyFailsOnAnExistingStation)
    {
        EXPECT_EQ(0u, this->create_station(u"WinSta0", 0));
        EXPECT_EQ(183u, this->last_error());

        const auto created = this->create_station(u"Sandbox");
        ASSERT_NE(0u, created);
        EXPECT_EQ(0u, this->create_station(u"Sandbox", 0));
        EXPECT_EQ(183u, this->last_error());
    }

    TEST_F(WindowStationTest, UnnamedStationsNameTheLogonSession)
    {
        const auto first = this->create_station(std::nullopt);
        ASSERT_NE(0u, first);
        EXPECT_EQ(first, this->create_station(std::nullopt));
    }

    TEST_F(WindowStationTest, NamesWithABackslashAreRejected)
    {
        EXPECT_EQ(0u, this->create_station(u"Sandbox\\Nested"));
        EXPECT_EQ(87u, this->last_error());
        EXPECT_EQ(0u, this->create_desktop(u"Desk\\Nested"));
    }

    TEST_F(WindowStationTest, CreatingAnExistingDesktopOpensIt)
    {
        EXPECT_EQ(this->emu->process.default_desktop.bits, this->create_desktop(u"Default"));
        EXPECT_EQ(this->emu->process.default_desktop.bits, this->create_desktop(u"DEFAULT"));
    }

    TEST_F(WindowStationTest, DesktopNamesAreScopedToTheirStation)
    {
        const auto first = this->create_desktop(u"Shared");
        ASSERT_NE(0u, first);

        const auto station = this->create_station(u"Sandbox");
        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetProcessWindowStation(this->context(), make_handle(station)));

        const auto second = this->create_desktop(u"Shared");
        ASSERT_NE(0u, second);
        EXPECT_NE(first, second);
        EXPECT_EQ(second, this->create_desktop(u"Shared"));
    }

    TEST_F(WindowStationTest, EachOpenOfAStationIsItsOwnReference)
    {
        const auto first = this->create_station(u"Sandbox");
        ASSERT_NE(0u, first);
        const auto second = this->create_station(u"Sandbox");
        ASSERT_EQ(first, second);

        ASSERT_EQ(TRUE, syscalls::handle_NtUserCloseWindowStation(this->context(), make_handle(first)));
        EXPECT_NE(nullptr, this->emu->process.window_stations.get(make_handle(second)));

        ASSERT_EQ(TRUE, syscalls::handle_NtUserCloseWindowStation(this->context(), make_handle(second)));
        EXPECT_EQ(nullptr, this->emu->process.window_stations.get(make_handle(second)));
    }

    TEST_F(WindowStationTest, EachOpenOfADesktopIsItsOwnReference)
    {
        const auto first = this->create_desktop(u"Shared");
        ASSERT_NE(0u, first);
        const auto second = this->create_desktop(u"Shared");
        ASSERT_EQ(first, second);

        ASSERT_EQ(TRUE, syscalls::handle_NtUserCloseDesktop(this->context(), make_handle(first)));
        EXPECT_NE(nullptr, this->emu->process.desktops.get(make_handle(second)));

        ASSERT_EQ(TRUE, syscalls::handle_NtUserCloseDesktop(this->context(), make_handle(second)));
        EXPECT_EQ(nullptr, this->emu->process.desktops.get(make_handle(second)));
    }

    TEST_F(WindowStationTest, OnlyTheInteractiveStationIsVisible)
    {
        EXPECT_EQ(wsf_visible, this->object_flags(interactive_window_station).flags);

        const auto created = this->create_station(u"Sandbox");
        ASSERT_NE(0u, created);
        EXPECT_EQ(0u, this->object_flags(created).flags);
    }

    // Measured on Windows 11: every flag and access combination opens a new handle to the "Default" desktop, inheritable as
    // requested, which closes without affecting the thread's desktop, whose own handle cannot be closed. UOI_IO is TRUE for a
    // handle to the desktop that receives input.
    TEST_F(WindowStationTest, OpeningTheInputDesktopGivesANewHandleToTheDefaultDesktop)
    {
        constexpr ACCESS_MASK desktop_switchdesktop = 0x100;
        const auto thread_desktop = syscalls::handle_NtUserGetThreadDesktop(this->context(), 0);

        const auto first = syscalls::handle_NtUserOpenInputDesktop(this->context(), 0, FALSE, desktop_switchdesktop);
        const auto second = syscalls::handle_NtUserOpenInputDesktop(this->context(), 1, TRUE, GENERIC_ALL);
        ASSERT_NE(0u, first);
        ASSERT_NE(0u, second);
        EXPECT_NE(thread_desktop, first);
        EXPECT_NE(first, second);
        EXPECT_EQ(u"Default", this->object_name(first));
        EXPECT_EQ(FALSE, this->object_flags(first).inherit);
        EXPECT_EQ(TRUE, this->object_flags(second).inherit);
        EXPECT_EQ(TRUE, this->receives_input(first));

        const auto mapped_through_opened_handle = syscalls::handle_NtUserMapDesktopObject(this->context(), make_handle(first));
        EXPECT_NE(0u, mapped_through_opened_handle);
        EXPECT_EQ(mapped_through_opened_handle, syscalls::handle_NtUserMapDesktopObject(this->context(), make_handle(thread_desktop)));

        EXPECT_EQ(TRUE, syscalls::handle_NtUserCloseDesktop(this->context(), make_handle(first)));
        EXPECT_EQ(TRUE, syscalls::handle_NtUserCloseDesktop(this->context(), make_handle(second)));
        EXPECT_EQ(FALSE, syscalls::handle_NtUserCloseDesktop(this->context(), make_handle(thread_desktop)));
        EXPECT_EQ(thread_desktop, syscalls::handle_NtUserGetThreadDesktop(this->context(), 0));
    }
}

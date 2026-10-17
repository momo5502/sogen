#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    BOOL handle_NtUserGetObjectInformation(const syscall_context& c, handle object, int index, uint64_t information, DWORD length,
                                           emulator_object<DWORD> needed);
    BOOL handle_NtUserThreadMessageQueueAttached(const syscall_context& c, DWORD thread_id);
}

namespace sogen::test
{
    namespace
    {
        class UserObjectInformationTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                const auto thread_handle =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread_handle);
                desktop object{};
                object.name = u"Default";
                object.inherit = true;
                this->desktop_handle = this->emu->process.desktops.store(std::move(object));
                this->emu->process.default_desktop = this->desktop_handle;
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            BOOL query(handle object, int index, DWORD length = 512)
            {
                return syscalls::handle_NtUserGetObjectInformation(this->context(), object, index, this->storage + 0x100, length,
                                                                   {this->emu->memory, this->storage});
            }

            DWORD last_error()
            {
                return this->emu->vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void set_last_error(DWORD error)
            {
                this->emu->vcpu(0).active_thread->teb64->access([&](TEB64& teb) { teb.LastErrorValue = error; });
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle desktop_handle{};
        };
    }

    TEST_F(UserObjectInformationTest, NamesAndTypesIncludeTerminatorAndPreserveLastError)
    {
        for (const auto index : {2, 3})
        {
            this->set_last_error(0x1234);
            EXPECT_EQ(FALSE, this->query(this->desktop_handle, index, 0));
            EXPECT_EQ(122u, this->last_error());
            EXPECT_EQ(16u, this->emu->memory.read_memory<DWORD>(this->storage));
            this->set_last_error(0x1234);
            ASSERT_EQ(TRUE, this->query(this->desktop_handle, index));
            EXPECT_EQ(0x1234u, this->last_error());
            EXPECT_EQ(index == 2 ? u"Default" : u"Desktop", read_string<char16_t>(this->emu->memory, this->storage + 0x100));
        }
        ASSERT_EQ(TRUE, this->query(make_handle(1), 2));
        EXPECT_EQ(u"WinSta0", read_string<char16_t>(this->emu->memory, this->storage + 0x100));
        ASSERT_EQ(TRUE, this->query(make_handle(1), 3));
        EXPECT_EQ(u"WindowStation", read_string<char16_t>(this->emu->memory, this->storage + 0x100));
    }

    TEST_F(UserObjectInformationTest, FlagsAndInputDesktopReflectGuestObjects)
    {
        ASSERT_EQ(TRUE, this->query(this->desktop_handle, 1));
        EXPECT_EQ((std::array<DWORD, 3>{1, 0, 0}), (this->emu->memory.read_memory<std::array<DWORD, 3>>(this->storage + 0x100)));
        ASSERT_EQ(TRUE, this->query(make_handle(1), 1));
        EXPECT_EQ((std::array<DWORD, 3>{0, 0, 1}), (this->emu->memory.read_memory<std::array<DWORD, 3>>(this->storage + 0x100)));
        ASSERT_EQ(TRUE, this->query(this->desktop_handle, 6));
        EXPECT_EQ(TRUE, this->emu->memory.read_memory<BOOL>(this->storage + 0x100));
        EXPECT_EQ(FALSE, this->query(make_handle(1), 6));
        EXPECT_EQ(87u, this->last_error());
    }

    TEST_F(UserObjectInformationTest, InvalidHandlesAndBuffersReturnGuestErrors)
    {
        EXPECT_EQ(FALSE, this->query(make_handle(0x12345678), 2));
        EXPECT_EQ(6u, this->last_error());
        EXPECT_EQ(FALSE, this->query(this->desktop_handle, 99));
        EXPECT_EQ(87u, this->last_error());
        EXPECT_EQ(FALSE, syscalls::handle_NtUserGetObjectInformation(this->context(), this->desktop_handle, 2, 0x12345678, 512,
                                                                     {this->emu->memory, this->storage}));
        EXPECT_EQ(998u, this->last_error());
    }

    TEST_F(UserObjectInformationTest, QueueAttachmentConvertsCurrentThreadAndPreservesZeroIdLastError)
    {
        auto* thread = this->emu->vcpu(0).active_thread;
        EXPECT_EQ(0u, thread->win32k_thread_info);
        this->set_last_error(0x1234);
        EXPECT_EQ(TRUE, syscalls::handle_NtUserThreadMessageQueueAttached(this->context(), thread->id));
        EXPECT_NE(0u, thread->win32k_thread_info);
        EXPECT_EQ(thread->win32k_thread_info, thread->teb64->read().Win32ThreadInfo);
        EXPECT_EQ(0x1234u, this->last_error());
        EXPECT_EQ(FALSE, syscalls::handle_NtUserThreadMessageQueueAttached(this->context(), 0));
        EXPECT_EQ(0x1234u, this->last_error());
        EXPECT_EQ(FALSE, syscalls::handle_NtUserThreadMessageQueueAttached(this->context(), 0x7fffffff));
        EXPECT_EQ(87u, this->last_error());
    }
}

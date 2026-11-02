#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
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

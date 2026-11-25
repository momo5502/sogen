#include "std_include.hpp"

namespace sogen::syscalls
{
    BOOL handle_NtUserLockWindowUpdate(const syscall_context& c, hwnd window);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t untouched_error = 0xDEAD;
        constexpr uint32_t error_invalid_parameter = 87;
        constexpr uint32_t error_invalid_window_handle = 1400;
        constexpr uint32_t error_screen_already_locked = 1440;

        enum class caller
        {
            first,
            second,
            third,
        };

        class WindowUpdateLockTest : public testing::Test
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

                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                this->emu->process.user_handles.setup(false);

                this->first_thread = this->add_thread(8);
                this->second_thread = this->add_thread(12);
                this->third_thread = this->add_thread(16);
            }

            handle add_thread(const uint32_t id)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            emulator_thread& thread_of(const caller who)
            {
                const auto thread = who == caller::first    ? this->first_thread
                                    : who == caller::second ? this->second_thread
                                                            : this->third_thread;
                return *this->emu->process.threads.get(thread);
            }

            hwnd create_window(const caller owner)
            {
                auto [window_handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.thread_id = this->thread_of(owner).id;
                win.handle = window_handle.bits;
                return win.handle;
            }

            struct result
            {
                BOOL returned{};
                uint32_t error{};
            };

            result lock_window_update(const caller who, const hwnd window)
            {
                auto& thread = this->thread_of(who);
                this->emu->vcpu(0).active_thread = &thread;
                thread.teb64->access([](TEB64& teb) { teb.LastErrorValue = untouched_error; });

                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                const auto returned = syscalls::handle_NtUserLockWindowUpdate(context, window);
                return {.returned = returned, .error = thread.teb64->read().LastErrorValue};
            }

            std::unique_ptr<windows_emulator> emu{};
            handle first_thread{};
            handle second_thread{};
            handle third_thread{};
        };
    }

    TEST_F(WindowUpdateLockTest, OneLockIsHeldAcrossThreads)
    {
        const auto a = this->create_window(caller::first);
        const auto b = this->create_window(caller::first);
        const auto c = this->create_window(caller::second);
        const auto desktop = this->create_window(caller::first);
        this->emu->process.windows.get(desktop)->thread_id = 0;
        this->emu->process.windows.get(desktop)->class_name = u"#32769";
        this->emu->process.default_desktop_window_handle.bits = desktop;

        struct step
        {
            caller who;
            hwnd window;
            BOOL returned;
            uint32_t error;
        };

        const std::vector<step> steps{
            {caller::first, 0, FALSE, error_invalid_parameter},
            {caller::first, a, TRUE, untouched_error},
            {caller::first, a, FALSE, error_invalid_parameter},
            {caller::first, b, FALSE, error_invalid_parameter},
            {caller::second, c, FALSE, error_screen_already_locked},
            {caller::second, a, FALSE, error_screen_already_locked},
            {caller::second, 0, FALSE, error_screen_already_locked},
            {caller::first, 0, TRUE, untouched_error},
            {caller::first, 0, FALSE, error_invalid_parameter},
            {caller::second, c, TRUE, untouched_error},
            {caller::first, a, FALSE, error_screen_already_locked},
            {caller::second, 0, TRUE, untouched_error},
            {caller::first, c, TRUE, untouched_error},
            {caller::first, 0, TRUE, untouched_error},
            {caller::second, desktop, TRUE, untouched_error},
            {caller::second, desktop, FALSE, error_invalid_parameter},
            {caller::second, 0, TRUE, untouched_error},
        };

        for (size_t i = 0; i < steps.size(); ++i)
        {
            const auto& expected = steps[i];
            const auto actual = this->lock_window_update(expected.who, expected.window);
            EXPECT_EQ(expected.returned, actual.returned) << "step " << i;
            EXPECT_EQ(expected.error, actual.error) << "step " << i;
        }
    }

    TEST_F(WindowUpdateLockTest, OnlyALiveWindowCanBeLockedAndItsEndOrItsLockersEndReleasesTheLock)
    {
        const auto a = this->create_window(caller::first);
        const auto c = this->create_window(caller::second);
        const auto destroyed = this->create_window(caller::first);
        EXPECT_TRUE(this->emu->process.windows.erase(destroyed));

        const std::vector<hwnd> invalid{0x1,   0x2,   0xFFFF,   0xDEAD, 0xDEADBEEF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE,
                                        a + 1, a - 1, destroyed};
        for (const auto window : invalid)
        {
            const auto locked = this->lock_window_update(caller::first, window);
            EXPECT_EQ(FALSE, locked.returned) << std::hex << window;
            EXPECT_EQ(error_invalid_window_handle, locked.error) << std::hex << window;

            const auto unlocked = this->lock_window_update(caller::first, 0);
            EXPECT_EQ(FALSE, unlocked.returned) << std::hex << window;
            EXPECT_EQ(error_invalid_parameter, unlocked.error) << std::hex << window;
        }

        for (const auto window : {a | (0xFFFFFFFFull << 32), a | (1ull << 32)})
        {
            EXPECT_EQ(TRUE, this->lock_window_update(caller::first, window).returned) << std::hex << window;
            EXPECT_EQ(TRUE, this->lock_window_update(caller::first, 0).returned) << std::hex << window;
        }

        ASSERT_EQ(TRUE, this->lock_window_update(caller::third, a).returned);
        this->emu->process.terminate_thread(this->thread_of(caller::third), STATUS_SUCCESS);

        EXPECT_EQ(TRUE, this->lock_window_update(caller::first, a).returned);
        EXPECT_EQ(TRUE, this->lock_window_update(caller::first, 0).returned);

        ASSERT_EQ(TRUE, this->lock_window_update(caller::second, c).returned);
        this->emu->process.terminate_thread(this->thread_of(caller::second), STATUS_SUCCESS);

        EXPECT_EQ(TRUE, this->lock_window_update(caller::first, a).returned);
        EXPECT_EQ(TRUE, this->lock_window_update(caller::first, 0).returned);
    }
}

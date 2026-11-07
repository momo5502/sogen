#include "emulation_test_utils.hpp"
#include <syscall_dispatcher.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    hwnd handle_NtUserSetFocus(const syscall_context& c, hwnd hwnd);
    hwnd completion_NtUserSetFocus(const syscall_context& c, hwnd hwnd);
}

namespace sogen::test
{
    namespace
    {
        // The arguments of the fnDWORD user callback that delivers WM_SETFOCUS and WM_KILLFOCUS.
        struct dword_callback_args
        {
            uint64_t pwnd;
            uint32_t msg;
            uint64_t w_param;
            uint64_t l_param;
            uint64_t x_param;
            uint64_t xpfn_proc;
        };

        struct focus_message
        {
            hwnd window;
            uint32_t message;
            hwnd other;

            bool operator==(const focus_message&) const = default;
        };

        constexpr uint32_t k_error_access_denied = 5;
        constexpr uint32_t k_error_invalid_parameter = 87;
        constexpr uint32_t k_error_invalid_window_handle = 1400;

        class SetFocusTest : public testing::Test
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
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                const auto stack = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->context().x64_cpu().reg(x86_register::rsp, stack + 0x10000);
                const auto code = this->emu->memory.allocate_memory(0x1000, memory_permission::read_exec);
                this->context().x64_cpu().reg(x86_register::rip, code);

                this->top = this->create_window(0, WS_POPUP | WS_VISIBLE);
                this->first = this->create_window(this->top, WS_CHILD | WS_VISIBLE);
                this->second = this->create_window(this->top, WS_CHILD | WS_VISIBLE);
                this->third = this->create_window(this->top, WS_CHILD | WS_VISIBLE);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_thread& thread()
            {
                return *this->emu->vcpu(0).active_thread;
            }

            hwnd create_window(const hwnd parent, const uint32_t style)
            {
                auto [handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.handle = handle.bits;
                win.parent_handle = parent != 0 ? parent : this->emu->process.default_desktop_window_handle.bits;
                win.thread_id = this->thread().id;
                win.style = style;
                win.width = 50;
                win.height = 50;
                win.guest.access([&](USER_WINDOW& guest) { guest.hWnd = handle.bits; });
                this->windows_by_guest[win.guest.value()] = win.handle;
                return win.handle;
            }

            window& get(const hwnd handle)
            {
                return *this->emu->process.windows.get(handle);
            }

            uint32_t last_error()
            {
                return this->thread().teb64->read().LastErrorValue;
            }

            // The message the callback that was just dispatched delivers.
            focus_message dispatched_message()
            {
                const auto rsp = this->context().x64_cpu().read_stack_pointer();
                const auto args = this->emu->memory.read_memory<dword_callback_args>(rsp + 0x30);
                return {.window = this->windows_by_guest.at(args.pwnd), .message = args.msg, .other = static_cast<hwnd>(args.w_param)};
            }

            // Calls SetFocus and runs the callbacks it dispatches, as if each window procedure returned right away. A window
            // procedure can change the focus itself: on_message runs while its message is being handled.
            hwnd set_focus(const hwnd target, const std::function<void(const focus_message&)>& on_message = {})
            {
                auto call = this->context();
                auto result = syscalls::handle_NtUserSetFocus(call, target);
                bool callback_pending = call.run_callback;
                while (callback_pending)
                {
                    const auto message = this->dispatched_message();
                    this->messages.push_back(message);

                    auto& callbacks = this->thread().callback_stack;
                    auto frame = std::move(callbacks.back());
                    callbacks.pop_back();
                    frame.restore_registers(this->emu->vcpu(0).cpu);

                    if (on_message)
                    {
                        on_message(message);
                    }

                    auto completion = this->context();
                    completion.is_callback_completion = true;
                    completion.current_completion_state = frame.state.get();
                    result = syscalls::completion_NtUserSetFocus(completion, target);
                    callback_pending = completion.run_callback;
                }
                return result;
            }

            std::unique_ptr<windows_emulator> emu{};
            std::map<uint64_t, hwnd> windows_by_guest{};
            std::vector<focus_message> messages{};
            hwnd top{};
            hwnd first{};
            hwnd second{};
            hwnd third{};
        };
    }

    TEST_F(SetFocusTest, TheOldWindowHearsOfTheNewOneBeforeTheNewOneHearsOfTheOld)
    {
        this->set_focus(this->first);
        this->messages.clear();

        EXPECT_EQ(this->first, this->set_focus(this->second));
        EXPECT_EQ(this->second, this->thread().focus_window);
        EXPECT_EQ((std::vector<focus_message>{{this->first, WM_KILLFOCUS, this->second}, {this->second, WM_SETFOCUS, this->first}}),
                  this->messages);
    }

    TEST_F(SetFocusTest, FocusingTheFocusedWindowSendsNothing)
    {
        this->set_focus(this->second);
        this->messages.clear();

        EXPECT_EQ(this->second, this->set_focus(this->second));
        EXPECT_TRUE(this->messages.empty());
    }

    TEST_F(SetFocusTest, ClearingTheFocusOnlyKillsIt)
    {
        this->set_focus(this->third);
        this->messages.clear();

        EXPECT_EQ(this->third, this->set_focus(0));
        EXPECT_EQ(0u, this->thread().focus_window);
        EXPECT_EQ((std::vector<focus_message>{{this->third, WM_KILLFOCUS, 0}}), this->messages);
    }

    TEST_F(SetFocusTest, AWindowProcedureThatMovesTheFocusWhileLosingItWins)
    {
        this->set_focus(this->second);
        this->messages.clear();

        bool refocused = false;
        const auto result = this->set_focus(this->first, [&](const focus_message& message) {
            if (!refocused && message.message == WM_KILLFOCUS && message.window == this->second)
            {
                refocused = true;
                this->set_focus(this->third);
            }
        });

        EXPECT_EQ(this->second, result);
        EXPECT_EQ(this->third, this->thread().focus_window);
        EXPECT_EQ((std::vector<focus_message>{{this->second, WM_KILLFOCUS, this->first},
                                              {this->first, WM_KILLFOCUS, this->third},
                                              {this->third, WM_SETFOCUS, this->first}}),
                  this->messages);
    }

    TEST_F(SetFocusTest, WindowsThatCannotTakeTheFocusLeaveItUnchanged)
    {
        this->set_focus(this->second);
        this->messages.clear();

        const auto expect_rejected = [&](const hwnd target, const uint32_t error) {
            EXPECT_EQ(0u, this->set_focus(target));
            EXPECT_EQ(error, this->last_error());
            EXPECT_EQ(this->second, this->thread().focus_window);
        };

        this->get(this->third).style |= WS_DISABLED;
        expect_rejected(this->third, k_error_invalid_parameter);
        this->get(this->third).style &= ~WS_DISABLED;

        this->get(this->top).style |= WS_DISABLED;
        expect_rejected(this->first, k_error_invalid_parameter);
        this->get(this->top).style &= ~WS_DISABLED;

        this->get(this->top).style |= WS_MINIMIZE;
        expect_rejected(this->first, k_error_invalid_parameter);
        this->get(this->top).style &= ~WS_MINIMIZE;

        this->get(this->third).thread_id = this->thread().id + 4;
        expect_rejected(this->third, k_error_access_denied);

        expect_rejected(0x1234, k_error_invalid_window_handle);
        EXPECT_TRUE(this->messages.empty());
    }
}

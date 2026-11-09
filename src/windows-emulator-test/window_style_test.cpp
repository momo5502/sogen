#include "emulation_test_utils.hpp"
#include <syscall_dispatcher.hpp>
#include <syscall_utils.hpp>

#include <tuple>
#include <unordered_map>

namespace sogen::syscalls
{
    emulator_pointer handle_NtUserSetWindowLongPtr(const syscall_context& c, handle window, int index, emulator_pointer value, BOOL ansi);
    emulator_pointer completion_NtUserSetWindowLongPtr(const syscall_context& c, handle window, int index, emulator_pointer value,
                                                       BOOL ansi);
    uint32_t handle_NtUserSetWindowLong(const syscall_context& c, handle window, int index, uint32_t value, BOOL ansi);
    uint32_t completion_NtUserSetWindowLong(const syscall_context& c, handle window, int index, uint32_t value, BOOL ansi);
}

namespace sogen::test
{
    namespace
    {
        class RecordingWindowUiBackend final : public ui_backend
        {
          public:
            void set_event_sink(event_sink /*sink*/) override
            {
            }

            void pump_events() override
            {
            }

            void create_window(const ui_window_desc& desc) override
            {
                this->windows[desc.handle] = {.visible = desc.visible, .enabled = desc.enabled};
            }

            void set_window_visible(const hwnd window, const bool visible, const bool activate) override
            {
                this->windows.at(window).visible = visible;
                if (!activate)
                {
                    ++this->nonactivating_visibility_changes;
                }
            }

            void set_window_enabled(const hwnd window, const bool enabled) override
            {
                this->windows.at(window).enabled = enabled;
            }

            bool is_visible(const hwnd window) const
            {
                return this->windows.at(window).visible;
            }

            bool is_enabled(const hwnd window) const
            {
                return this->windows.at(window).enabled;
            }

            size_t nonactivating_visibility_changes{};

          private:
            struct window_state
            {
                bool visible{};
                bool enabled{};
            };

            std::unordered_map<hwnd, window_state> windows{};
        };

        class WindowStyleTest : public testing::TestWithParam<std::tuple<bool, bool>>
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<RecordingWindowUiBackend>();
                this->ui = dynamic_cast<RecordingWindowUiBackend*>(interfaces.ui.get());
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
                this->stack_top = stack + 0x10000;
                this->context().x64_cpu().reg(x86_register::rsp, this->stack_top);
                const auto code = this->emu->memory.allocate_memory(0x1000, memory_permission::read_exec);
                this->context().x64_cpu().reg(x86_register::rip, code);

                auto [created_handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.handle = created_handle.bits;
                win.thread_id = this->emu->vcpu(0).active_thread->id;
                win.style = (this->is_child() ? WS_CHILD : WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX) | WS_CAPTION;
                win.ex_style = WS_EX_CLIENTEDGE;
                win.width = 100;
                win.height = 50;
                win.guest.access([&](USER_WINDOW& guest) {
                    guest.dwStyle = win.style;
                    guest.dwExStyle = win.ex_style;
                });
                this->emu->process.sync_guest_window_rects(win);
                this->window_handle = created_handle;
            }

            bool is_child() const
            {
                return std::get<0>(GetParam());
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            window& guest_window()
            {
                return *this->emu->process.windows.get(this->window_handle);
            }

            void attach_host_window()
            {
                auto& win = this->guest_window();
                win.host_surface_window = true;
                this->ui->create_window({
                    .handle = win.handle,
                    .visible = (win.style & WS_VISIBLE) != 0,
                    .enabled = (win.style & WS_DISABLED) == 0,
                });
            }

            callback_frame finish_callback()
            {
                auto& callbacks = this->emu->vcpu(0).active_thread->callback_stack;
                auto frame = std::move(callbacks.at(callbacks.size() - 1));
                callbacks.pop_back();
                frame.restore_registers(this->emu->vcpu(0).cpu);
                return frame;
            }

            uint32_t complete(callback_frame& frame, const int index)
            {
                auto c = this->context();
                c.is_callback_completion = true;
                c.current_completion_state = frame.state.get();
                if (std::get<1>(GetParam()))
                {
                    return syscalls::completion_NtUserSetWindowLong(c, this->window_handle, index, 0, FALSE);
                }
                return static_cast<uint32_t>(syscalls::completion_NtUserSetWindowLongPtr(c, this->window_handle, index, 0, FALSE));
            }

            void change_style(const int index, const uint32_t requested, const uint32_t edited, const uint32_t expected)
            {
                const auto old_style = index == GWL_STYLE ? this->guest_window().style : this->guest_window().ex_style;
                const auto old_client = this->guest_window().guest.read().rcClient;
                auto c = this->context();
                if (std::get<1>(GetParam()))
                {
                    (void)syscalls::handle_NtUserSetWindowLong(c, this->window_handle, index, requested, FALSE);
                }
                else
                {
                    (void)syscalls::handle_NtUserSetWindowLongPtr(c, this->window_handle, index, requested, FALSE);
                }
                ASSERT_TRUE(c.run_callback);
                auto changing = this->finish_callback();
                auto& state = dynamic_cast<window_style_change_state&>(*changing.state);
                const auto address = state.style_alloc.address();
                auto styles = this->emu->memory.read_memory<STYLESTRUCT>(address);
                EXPECT_EQ(old_style, styles.styleOld);
                EXPECT_EQ(requested, styles.styleNew);
                styles.styleNew = edited;
                this->emu->memory.write_memory(address, styles);
                (void)this->complete(changing, index);

                const auto guest = this->guest_window().guest.read();
                EXPECT_EQ(expected, index == GWL_STYLE ? this->guest_window().style : this->guest_window().ex_style);
                EXPECT_EQ(expected, index == GWL_STYLE ? guest.dwStyle : guest.dwExStyle);
                EXPECT_EQ(old_client.right - old_client.left, this->guest_window().client_width());
                EXPECT_EQ(old_client.bottom - old_client.top, this->guest_window().client_height());
                EXPECT_EQ(old_client.left, guest.rcClient.left);
                EXPECT_EQ(old_client.top, guest.rcClient.top);
                EXPECT_EQ(old_client.right, guest.rcClient.right);
                EXPECT_EQ(old_client.bottom, guest.rcClient.bottom);
                auto changed = this->finish_callback();
                styles = this->emu->memory.read_memory<STYLESTRUCT>(address);
                EXPECT_EQ(expected, styles.styleNew);
                EXPECT_EQ(old_style, this->complete(changed, index));
                EXPECT_EQ(this->stack_top, this->emu->vcpu(0).cpu.read_stack_pointer());
            }

            std::unique_ptr<windows_emulator> emu{};
            RecordingWindowUiBackend* ui{};
            handle window_handle{};
            uint64_t stack_top{};
        };
    }

    TEST_P(WindowStyleTest, RequestedFrameAndHierarchyChangesKeepClientGeometryConsistent)
    {
        const auto original = this->guest_window().style;
        const uint32_t requested =
            (original ^ (WS_CHILD | WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_HSCROLL | WS_VSCROLL)) | WS_CLIPCHILDREN;
        this->change_style(GWL_STYLE, requested, requested, original | WS_CLIPCHILDREN);
    }

    TEST_P(WindowStyleTest, CallbackEditsCannotChangeFrameOrHierarchy)
    {
        const auto original = this->guest_window().style;
        const uint32_t requested = original | WS_CLIPCHILDREN;
        const uint32_t edited =
            (requested ^ (WS_CHILD | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_CLIPSIBLINGS;
        const uint32_t control_bits = this->is_child() ? WS_MINIMIZEBOX | WS_MAXIMIZEBOX : 0;
        this->change_style(GWL_STYLE, requested, edited, original | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | control_bits);
    }

    TEST_P(WindowStyleTest, ExtendedFrameChangesAreFilteredFromRequestsAndCallbackEdits)
    {
        const auto original = this->guest_window().ex_style;
        const uint32_t requested = (original ^ WS_EX_CLIENTEDGE) | WS_EX_TRANSPARENT;
        const uint32_t edited =
            requested | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE | WS_EX_TOOLWINDOW | WS_EX_CONTEXTHELP;
        this->change_style(GWL_EXSTYLE, requested, edited, original | WS_EX_TRANSPARENT);
    }

    TEST_P(WindowStyleTest, UnchangedStylesStillCompleteBothNotifications)
    {
        for (const auto index : {GWL_STYLE, GWL_EXSTYLE})
        {
            const auto original = index == GWL_STYLE ? this->guest_window().style : this->guest_window().ex_style;
            this->change_style(index, original, original, original);
            const uint32_t rejected_bit = index == GWL_STYLE ? WS_BORDER : WS_EX_CLIENTEDGE;
            this->change_style(index, original ^ rejected_bit, original, original);
            this->change_style(index, original ^ rejected_bit, original ^ rejected_bit, original);
        }
    }

    TEST_P(WindowStyleTest, VisibilityStyleChangesDoNotRunShowSequenceOrInvalidate)
    {
        const auto original = this->guest_window().style;
        ASSERT_FALSE(this->guest_window().update_pending);
        this->attach_host_window();
        EXPECT_FALSE(this->ui->is_visible(this->guest_window().handle));

        const auto visible = original | WS_VISIBLE;
        this->change_style(GWL_STYLE, visible, visible, visible);
        EXPECT_NE(0u, this->guest_window().style & WS_VISIBLE);
        EXPECT_NE(0u, this->guest_window().guest.read().dwStyle & WS_VISIBLE);
        EXPECT_TRUE(this->ui->is_visible(this->guest_window().handle));
        EXPECT_EQ(1u, this->ui->nonactivating_visibility_changes);
        EXPECT_FALSE(this->guest_window().update_pending);

        const auto hidden = visible & ~static_cast<uint32_t>(WS_VISIBLE);
        this->change_style(GWL_STYLE, hidden, hidden, hidden);
        EXPECT_EQ(0u, this->guest_window().style & WS_VISIBLE);
        EXPECT_EQ(0u, this->guest_window().guest.read().dwStyle & WS_VISIBLE);
        EXPECT_FALSE(this->ui->is_visible(this->guest_window().handle));
        EXPECT_EQ(2u, this->ui->nonactivating_visibility_changes);
        EXPECT_FALSE(this->guest_window().update_pending);
    }

    TEST_P(WindowStyleTest, DisabledStyleChangesDoNotRunEnableSequenceOrInvalidate)
    {
        const auto original = this->guest_window().style;
        ASSERT_FALSE(this->guest_window().update_pending);
        this->attach_host_window();
        EXPECT_TRUE(this->ui->is_enabled(this->guest_window().handle));

        const auto disabled = original | WS_DISABLED;
        this->change_style(GWL_STYLE, disabled, disabled, disabled);
        EXPECT_NE(0u, this->guest_window().style & WS_DISABLED);
        EXPECT_NE(0u, this->guest_window().guest.read().dwStyle & WS_DISABLED);
        EXPECT_FALSE(this->ui->is_enabled(this->guest_window().handle));
        EXPECT_FALSE(this->guest_window().update_pending);

        const auto enabled = disabled & ~static_cast<uint32_t>(WS_DISABLED);
        this->change_style(GWL_STYLE, enabled, enabled, enabled);
        EXPECT_EQ(0u, this->guest_window().style & WS_DISABLED);
        EXPECT_EQ(0u, this->guest_window().guest.read().dwStyle & WS_DISABLED);
        EXPECT_TRUE(this->ui->is_enabled(this->guest_window().handle));
        EXPECT_FALSE(this->guest_window().update_pending);
    }

    INSTANTIATE_TEST_SUITE_P(WindowKindsAndSetters, WindowStyleTest, testing::Combine(testing::Bool(), testing::Bool()));
}

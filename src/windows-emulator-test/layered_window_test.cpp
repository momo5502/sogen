#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    BOOL handle_NtUserUpdateLayeredWindow(const syscall_context& c, hwnd window, hdc destination_dc, emulator_object<POINT> destination,
                                          emulator_object<SIZE> size, hdc source_dc, emulator_object<POINT> source, COLORREF color_key,
                                          emulator_pointer blend, DWORD flags, emulator_pointer dirty_rect);
}

namespace sogen::test
{
    namespace
    {
        constexpr DWORD ulw_colorkey = 0x1;
        constexpr DWORD ulw_alpha = 0x2;

        class LayeredWindowTest : public testing::Test
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
                this->emu->process.user_handles.setup(false);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);

                auto [window_handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.handle = window_handle.bits;
                win.thread_id = this->emu->vcpu(0).active_thread->id;
                win.ex_style = WS_EX_LAYERED;
                win.style = WS_POPUP | WS_VISIBLE;
                win.x = 10;
                win.y = 20;
                win.width = 100;
                win.height = 50;
                this->window = win.handle;
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<POINT> destination(const int32_t x, const int32_t y)
            {
                const emulator_object<POINT> object{this->emu->memory, this->storage};
                object.write(POINT{.x = x, .y = y});
                return object;
            }

            emulator_pointer blend_function(const uint8_t constant_alpha, const uint8_t alpha_format)
            {
                const std::array<uint8_t, 4> value{0, 0, constant_alpha, alpha_format};
                this->emu->memory.write_memory(this->storage + 0x100, value);
                return this->storage + 0x100;
            }

            BOOL update(const hdc source_dc, const emulator_object<POINT> position, const DWORD flags, const emulator_pointer blend = 0)
            {
                return syscalls::handle_NtUserUpdateLayeredWindow(this->context(), this->window, 0, position, {this->emu->memory},
                                                                  source_dc, {this->emu->memory}, 0, blend, flags, 0);
            }

            const sogen::window& guest_window()
            {
                const auto* win = this->emu->process.windows.get(this->window);
                EXPECT_NE(nullptr, win);
                return *win;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            hwnd window{};
        };
    }

    TEST_F(LayeredWindowTest, ColorKeyIsRejectedInsteadOfBeingIgnored)
    {
        EXPECT_EQ(FALSE, this->update(0, this->destination(500, 600), ulw_colorkey));
        EXPECT_EQ(10, this->guest_window().x);
        EXPECT_EQ(20, this->guest_window().y);
    }

    TEST_F(LayeredWindowTest, AlphaWithoutABlendFunctionIsRejected)
    {
        EXPECT_EQ(FALSE, this->update(0, this->destination(500, 600), ulw_alpha));
        EXPECT_EQ(10, this->guest_window().x);
    }

    TEST_F(LayeredWindowTest, AnInvalidSourceDcLeavesTheGeometryAlone)
    {
        constexpr hdc missing_dc = 0x1234;
        EXPECT_EQ(FALSE, this->update(missing_dc, this->destination(500, 600), 0));
        EXPECT_EQ(10, this->guest_window().x);
        EXPECT_EQ(20, this->guest_window().y);
    }

    TEST_F(LayeredWindowTest, AlphaModeIsRememberedForPresentation)
    {
        ASSERT_EQ(TRUE, this->update(0, this->destination(30, 40), ulw_alpha, this->blend_function(128, 1)));
        EXPECT_TRUE(this->guest_window().layered_blend);
        EXPECT_TRUE(this->guest_window().layered_premultiplied_alpha);
        EXPECT_EQ(128, this->guest_window().layered_constant_alpha);

        ASSERT_EQ(TRUE, this->update(0, this->destination(30, 40), ulw_alpha, this->blend_function(64, 0)));
        EXPECT_TRUE(this->guest_window().layered_blend);
        EXPECT_FALSE(this->guest_window().layered_premultiplied_alpha);
        EXPECT_EQ(64, this->guest_window().layered_constant_alpha);

        ASSERT_EQ(TRUE, this->update(0, this->destination(30, 40), 0));
        EXPECT_FALSE(this->guest_window().layered_blend);
        EXPECT_FALSE(this->guest_window().layered_premultiplied_alpha);
        EXPECT_EQ(255, this->guest_window().layered_constant_alpha);
    }
}

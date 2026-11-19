#include "std_include.hpp"

namespace sogen::syscalls
{
    uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                             emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name, DWORD function_id,
                                             DWORD flags, emulator_pointer wow);
    hwnd handle_NtUserCreateWindowEx(const syscall_context& c, DWORD ex_style, emulator_object<LARGE_STRING> class_name,
                                     emulator_object<LARGE_STRING> cls_version, emulator_object<LARGE_STRING> window_name, DWORD style,
                                     int x, int y, int width, int height, hwnd parent, hmenu menu, hinstance instance, pointer l_param,
                                     DWORD flags, pointer acbi_buffer);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::u16string_view class_name = u"SogenStyleTestClass";
        constexpr uint32_t unicode_class_flags = 0xC0;

        class WindowCreationStyleTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->atom = this->register_class();
                ASSERT_NE(this->atom, 0);
            }

            uint16_t register_class()
            {
                const emulator_object<EMU_WNDCLASSEX> wnd_class{this->emu.memory, this->storage};
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> name{this->emu.memory, this->storage + 0x100};
                const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name{this->emu.memory, this->storage + 0x200};
                const auto name_buffer = this->storage + 0x300;

                wnd_class.write({.cbSize = sizeof(EMU_WNDCLASSEX),
                                 .style = 0,
                                 .lpfnWndProc = 0x140001000,
                                 .cbClsExtra = 0,
                                 .cbWndExtra = 0,
                                 .hInstance = 0x140000000,
                                 .hIcon = 0,
                                 .hCursor = 0,
                                 .hbrBackground = 0,
                                 .lpszMenuName = 0,
                                 .lpszClassName = 0,
                                 .hIconSm = 0});
                this->emu.memory.write_memory(name_buffer, class_name.data(), class_name.size() * sizeof(char16_t));
                name.write({.Length = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .MaximumLength = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .Buffer = name_buffer});
                class_menu_name.write({});

                return syscalls::handle_NtUserRegisterClassExWOW(this->context(), wnd_class, name, {this->emu.memory}, class_menu_name, 0,
                                                                 unicode_class_flags, 0);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            // The creation messages run as guest callbacks; the window and its style exist before the first one.
            DWORD style_after_creation(const DWORD style)
            {
                std::set<std::remove_cvref_t<decltype(this->emu.process.windows.begin()->first)>> existing{};
                for (const auto& [index, win] : this->emu.process.windows)
                {
                    existing.insert(index);
                }

                (void)syscalls::handle_NtUserCreateWindowEx(this->context(), 0, {this->emu.memory, this->atom}, {this->emu.memory},
                                                            {this->emu.memory}, style, 0, 0, 100, 100, 0, 0, 0x140000000, 0, 0, 0);

                for (const auto& [index, win] : this->emu.process.windows)
                {
                    if (!existing.contains(index))
                    {
                        const auto guest_style = win.guest.read().dwStyle;
                        EXPECT_EQ(win.style, guest_style);
                        return guest_style;
                    }
                }

                ADD_FAILURE() << "No window was created";
                return 0;
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t storage{};
            uint16_t atom{};
        };
    }

    TEST_F(WindowCreationStyleTest, AnOverlappedWindowGetsACaptionAndClipsSiblings)
    {
        EXPECT_EQ(static_cast<DWORD>(WS_CAPTION | WS_CLIPSIBLINGS), this->style_after_creation(WS_OVERLAPPED));
    }

    TEST_F(WindowCreationStyleTest, APopupWindowClipsSiblingsWithoutACaption)
    {
        EXPECT_EQ(static_cast<DWORD>(WS_POPUP | WS_CLIPSIBLINGS), this->style_after_creation(WS_POPUP));
    }
}

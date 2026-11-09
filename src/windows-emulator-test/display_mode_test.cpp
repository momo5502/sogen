#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtUserEnumDisplaySettings(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> device_name,
                                              DWORD mode_num, emulator_object<EMU_DEVMODEW> dev_mode, DWORD flags);
    LONG handle_NtUserChangeDisplaySettings(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> device_name,
                                            emulator_object<EMU_DEVMODEW> dev_mode, hwnd window, DWORD flags, uint64_t param);
}

namespace sogen::test
{
    namespace
    {
        constexpr LONG disp_change_successful = 0;
        constexpr LONG disp_change_badmode = -2;
        constexpr DWORD dm_pelswidth = 0x80000;
        constexpr DWORD dm_pelsheight = 0x100000;
        constexpr display_mode capped_mode{.width = 1280, .height = 720};

        windows_emulator create_capped_emulator()
        {
            return create_sample_emulator(emulator_settings{
                .use_relative_time = true,
                .max_display_mode = capped_mode,
            });
        }

        class DisplayModeTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->dev_mode = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            std::optional<display_mode> enumerate(const DWORD mode_num)
            {
                if (syscalls::handle_NtUserEnumDisplaySettings(this->context, {this->emu.memory, 0}, mode_num,
                                                               {this->emu.memory, this->dev_mode}, 0) != STATUS_SUCCESS)
                {
                    return std::nullopt;
                }

                const auto mode = this->emu.memory.read_memory<EMU_DEVMODEW>(this->dev_mode);
                return display_mode{.width = mode.dmPelsWidth, .height = mode.dmPelsHeight};
            }

            LONG change_to(const display_mode& mode)
            {
                EMU_DEVMODEW requested{};
                requested.dmSize = sizeof(requested);
                requested.dmFields = dm_pelswidth | dm_pelsheight;
                requested.dmPelsWidth = mode.width;
                requested.dmPelsHeight = mode.height;
                this->emu.memory.write_memory(this->dev_mode, requested);

                return syscalls::handle_NtUserChangeDisplaySettings(this->context, {this->emu.memory, 0},
                                                                    {this->emu.memory, this->dev_mode}, {}, 0, 0);
            }

            windows_emulator emu = create_capped_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t dev_mode{};
        };
    }

    TEST_F(DisplayModeTest, TheDisplayStartsInTheMaximumMode)
    {
        EXPECT_EQ(capped_mode, this->enumerate(ENUM_CURRENT_SETTINGS));
        EXPECT_EQ(capped_mode, this->enumerate(ENUM_REGISTRY_SETTINGS));
    }

    TEST_F(DisplayModeTest, OnlyModesWithinTheMaximumAreOffered)
    {
        std::vector<display_mode> offered{};
        for (DWORD mode_num = 0; const auto mode = this->enumerate(mode_num); ++mode_num)
        {
            offered.push_back(*mode);
        }

        const std::vector<display_mode> expected{{.width = 640, .height = 480}, {.width = 800, .height = 600}, capped_mode};
        EXPECT_EQ(expected, offered);
    }

    TEST_F(DisplayModeTest, ModesAboveTheMaximumCannotBeSet)
    {
        EXPECT_EQ(disp_change_badmode, this->change_to(default_display_mode));
        EXPECT_EQ(capped_mode, this->enumerate(ENUM_CURRENT_SETTINGS));

        constexpr display_mode smaller_mode{.width = 800, .height = 600};
        EXPECT_EQ(disp_change_successful, this->change_to(smaller_mode));
        EXPECT_EQ(smaller_mode, this->enumerate(ENUM_CURRENT_SETTINGS));
    }

    TEST(DisplayModeSettingsTest, AMaximumThatIsNotADisplayModeIsRejected)
    {
        EXPECT_THROW(create_sample_emulator(emulator_settings{.use_relative_time = true, .max_display_mode = display_mode{1000, 700}}),
                     std::invalid_argument);
    }
}

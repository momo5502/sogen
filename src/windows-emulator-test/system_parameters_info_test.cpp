#include "std_include.hpp"

namespace sogen::syscalls
{
    BOOL handle_NtUserSystemParametersInfo(const syscall_context& c, uint32_t action, uint32_t param, emulator_pointer value,
                                           uint32_t win_ini);
    BOOL handle_NtUserSystemParametersInfoForDpi(const syscall_context& c, uint32_t action, uint32_t param, emulator_pointer value,
                                                 uint32_t win_ini, uint32_t dpi);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t spi_get_icontitle_logfont = 0x1F;
        constexpr uint32_t spi_get_nonclientmetrics = 0x29;
        constexpr uint32_t spi_get_iconmetrics = 0x2D;
        constexpr uint32_t spi_get_minimizedmetrics = 0x2B;
        constexpr uint32_t spi_get_workarea = 0x30;
        constexpr uint32_t spi_get_filterkeys = 0x32;
        constexpr uint32_t spi_get_togglekeys = 0x34;
        constexpr uint32_t spi_get_mousekeys = 0x36;
        constexpr uint32_t spi_get_stickykeys = 0x3A;
        constexpr uint32_t spi_get_accesstimeout = 0x3C;
        constexpr uint32_t spi_get_soundsentry = 0x40;
        constexpr uint32_t spi_get_animation = 0x48;
        constexpr uint32_t error_invalid_spi_value = 1439;
        constexpr uint32_t error_invalid_parameter = 87;
        constexpr uint32_t error_noaccess = 998;
        constexpr auto ncm_size = static_cast<uint32_t>(sizeof(EMU_NONCLIENTMETRICS));
        constexpr uint32_t ncm_without_padded_border_size = 500;
        constexpr auto icon_metrics_size = static_cast<uint32_t>(sizeof(EMU_ICONMETRICS));
        constexpr auto logfont_size = static_cast<uint32_t>(sizeof(EMU_LOGFONTW));

        struct scalar_get
        {
            uint32_t action;
            uint32_t value;
            bool rejects_param;
        };

        struct struct_get
        {
            uint32_t action;
            uint32_t param;
            uint32_t struct_size;
            bool succeeds;
            uint32_t error;
            std::array<uint8_t, 16> bytes;
        };

        struct wallpaper_get
        {
            uint32_t capacity;
            uint32_t written_bytes;
        };

        constexpr uint32_t last_error_sentinel = 0xDEAD;

        constexpr auto added_scalar_gets = std::to_array<scalar_get>({
            {.action = 0x0038, .value = 0, .rejects_param = false},    {.action = 0x005E, .value = 0, .rejects_param = false},
            {.action = 0x0076, .value = 0, .rejects_param = false},    {.action = 0x0078, .value = 5000, .rejects_param = false},
            {.action = 0x007A, .value = 5000, .rejects_param = false}, {.action = 0x007C, .value = 5000, .rejects_param = false},
            {.action = 0x007E, .value = 1, .rejects_param = false},    {.action = 0x0080, .value = 30, .rejects_param = false},
            {.action = 0x0084, .value = 20, .rejects_param = false},   {.action = 0x0086, .value = 30, .rejects_param = false},
            {.action = 0x0088, .value = 50, .rejects_param = false},   {.action = 0x008A, .value = 50, .rejects_param = false},
            {.action = 0x008C, .value = 1, .rejects_param = false},    {.action = 0x00A0, .value = 6, .rejects_param = false},
            {.action = 0x101C, .value = 0, .rejects_param = true},     {.action = 0x101E, .value = 0, .rejects_param = true},
            {.action = 0x1020, .value = 1, .rejects_param = true},     {.action = 0x104C, .value = 0, .rejects_param = true},
            {.action = 0x2014, .value = 0, .rejects_param = true},     {.action = 0x201E, .value = 35, .rejects_param = true},
            {.action = 0x2020, .value = 3, .rejects_param = true},     {.action = 0x2022, .value = 5000, .rejects_param = true},
            {.action = 0x2024, .value = 0, .rejects_param = true},
        });

        constexpr auto recaptured_scalar_gets = std::to_array<scalar_get>({
            {.action = 0x005F, .value = 0, .rejects_param = false},
            {.action = 0x0068, .value = 3, .rejects_param = false},
            {.action = 0x006C, .value = 3, .rejects_param = false},
            {.action = 0x200E, .value = 1, .rejects_param = true},
            {.action = 0x2010, .value = 1, .rejects_param = true},
            {.action = 0x004A, .value = 1, .rejects_param = false},
            {.action = 0x200A, .value = 2, .rejects_param = true},
            {.action = 0x200C, .value = 1200, .rejects_param = true},
            {.action = 0x2012, .value = 1, .rejects_param = true},
            {.action = 0x1048, .value = 1, .rejects_param = true},
            {.action = 0x006A, .value = 400, .rejects_param = false},
            {.action = 0x100A, .value = 0, .rejects_param = true},
            {.action = 0x1042, .value = 1, .rejects_param = true},
            {.action = 0x1040, .value = 0, .rejects_param = true},
            {.action = 0x2016, .value = 5, .rejects_param = true},
            {.action = 0x2018, .value = 0, .rejects_param = true},
            {.action = 0x201A, .value = 24, .rejects_param = true},
        });

        constexpr auto added_struct_gets = std::to_array<struct_get>({
            {.action = 0x74,
             .param = 0,
             .struct_size = 12,
             .succeeds = true,
             .error = last_error_sentinel,
             .bytes = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 12,
             .struct_size = 12,
             .succeeds = true,
             .error = last_error_sentinel,
             .bytes = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 12,
             .struct_size = 0,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 12,
             .struct_size = 16,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 0,
             .struct_size = 0,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 1,
             .struct_size = 12,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 16,
             .struct_size = 12,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 24,
             .struct_size = 12,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 0,
             .struct_size = 16,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x74,
             .param = 12,
             .struct_size = 100,
             .succeeds = false,
             .error = last_error_sentinel,
             .bytes = {0x64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 0,
             .struct_size = 16,
             .succeeds = false,
             .error = error_invalid_parameter,
             .bytes = {0x10, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 16,
             .struct_size = 16,
             .succeeds = true,
             .error = last_error_sentinel,
             .bytes = {0x10, 0, 0, 0, 0x08, 0, 0, 0, 0x08, 0, 0, 0, 0x01, 0, 0, 0}},
            {.action = 0x9C,
             .param = 16,
             .struct_size = 0,
             .succeeds = true,
             .error = last_error_sentinel,
             .bytes = {0x00, 0, 0, 0, 0x08, 0, 0, 0, 0x08, 0, 0, 0, 0x01, 0, 0, 0}},
            {.action = 0x9C,
             .param = 16,
             .struct_size = 20,
             .succeeds = true,
             .error = last_error_sentinel,
             .bytes = {0x14, 0, 0, 0, 0x08, 0, 0, 0, 0x08, 0, 0, 0, 0x01, 0, 0, 0}},
            {.action = 0x9C,
             .param = 0,
             .struct_size = 0,
             .succeeds = false,
             .error = error_invalid_parameter,
             .bytes = {0x00, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 1,
             .struct_size = 16,
             .succeeds = false,
             .error = error_invalid_parameter,
             .bytes = {0x10, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 24,
             .struct_size = 16,
             .succeeds = false,
             .error = error_invalid_parameter,
             .bytes = {0x10, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 20,
             .struct_size = 16,
             .succeeds = false,
             .error = error_invalid_parameter,
             .bytes = {0x10, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 0,
             .struct_size = 20,
             .succeeds = false,
             .error = error_invalid_parameter,
             .bytes = {0x14, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
            {.action = 0x9C,
             .param = 16,
             .struct_size = 100,
             .succeeds = true,
             .error = last_error_sentinel,
             .bytes = {0x64, 0, 0, 0, 0x08, 0, 0, 0, 0x08, 0, 0, 0, 0x01, 0, 0, 0}},
        });

        constexpr std::array<wallpaper_get, 6> wallpaper_gets{{
            {.capacity = 260, .written_bytes = 520},
            {.capacity = 0, .written_bytes = 0},
            {.capacity = 1, .written_bytes = 2},
            {.capacity = 2, .written_bytes = 4},
            {.capacity = 4, .written_bytes = 8},
            {.capacity = 16, .written_bytes = 32},
        }};

        class SystemParametersInfoTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->out = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            BOOL query(const uint32_t action, const uint32_t param, const uint64_t value, const uint32_t win_ini = 0)
            {
                return syscalls::handle_NtUserSystemParametersInfo(this->context, action, param, value, win_ini);
            }

            BOOL query_for_dpi(const uint32_t action, const uint32_t param, const uint64_t value, const uint32_t dpi)
            {
                return syscalls::handle_NtUserSystemParametersInfoForDpi(this->context, action, param, value, 0, dpi);
            }

            void set_struct_size(const uint32_t size)
            {
                this->emu.memory.write_memory(this->out, size);
            }

            void set_subsystem_version(const uint16_t major)
            {
                this->emu.mod_manager.executable->subsystem_major_version = major;
            }

            uint64_t raw_output()
            {
                return this->emu.memory.read_memory<uint64_t>(this->out);
            }

            static uint64_t written_value(const uint32_t value, const size_t size)
            {
                uint64_t expected = 0xCCCCCCCCCCCCCCCC;
                std::memcpy(&expected, &value, size);
                return expected;
            }

            void expect_scalar_get(const scalar_get& row)
            {
                this->clear_output(last_error_sentinel);
                EXPECT_TRUE(this->query(row.action, 0, this->out)) << row.action;
                EXPECT_EQ(last_error_sentinel, this->last_error()) << row.action;
                EXPECT_EQ(written_value(row.value, sizeof(row.value)), this->raw_output()) << row.action;

                this->clear_output(last_error_sentinel);
                if (row.rejects_param)
                {
                    EXPECT_FALSE(this->query(row.action, 4, this->out)) << row.action;
                    EXPECT_EQ(error_invalid_parameter, this->last_error()) << row.action;
                    EXPECT_EQ(0xCCCCCCCCCCCCCCCC, this->raw_output()) << row.action;
                }
                else
                {
                    EXPECT_TRUE(this->query(row.action, 4, this->out)) << row.action;
                    EXPECT_EQ(last_error_sentinel, this->last_error()) << row.action;
                    EXPECT_EQ(written_value(row.value, sizeof(row.value)), this->raw_output()) << row.action;
                }

                this->clear_output(last_error_sentinel);
                EXPECT_FALSE(this->query(row.action, 0, 0)) << row.action;
                EXPECT_EQ(error_noaccess, this->last_error()) << row.action;
            }

            uint32_t last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void clear_output(const uint32_t last_error_before = 0)
            {
                this->emu.memory.set_memory(this->out, 0xCC, 0x1000);
                this->emu.vcpu(0).active_thread->teb64->access([last_error_before](TEB64& teb) { teb.LastErrorValue = last_error_before; });
            }

            windows_emulator emu = create_sample_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t out{};
        };

        struct dpi_metrics
        {
            uint32_t dpi;
            LONG border;
            LONG scroll;
            LONG caption;
            LONG menu;
            LONG padded_border;
            LONG font_height;
        };
    }

    // Expected values captured on Windows 11 build 26300 (x64).
    TEST_F(SystemParametersInfoTest, MetricsScaleWithTheDpiTheCallerAsksFor)
    {
        constexpr std::array<dpi_metrics, 5> captured{{
            {.dpi = 1, .border = 0, .scroll = 0, .caption = 0, .menu = 0, .padded_border = 0, .font_height = 0},
            {.dpi = 96, .border = 1, .scroll = 17, .caption = 22, .menu = 19, .padded_border = 4, .font_height = -12},
            {.dpi = 120, .border = 1, .scroll = 21, .caption = 28, .menu = 24, .padded_border = 5, .font_height = -15},
            {.dpi = 144, .border = 2, .scroll = 26, .caption = 33, .menu = 29, .padded_border = 6, .font_height = -18},
            {.dpi = 192, .border = 2, .scroll = 34, .caption = 44, .menu = 38, .padded_border = 8, .font_height = -24},
        }};

        for (const auto& expected : captured)
        {
            this->clear_output();
            this->set_struct_size(ncm_size);
            ASSERT_TRUE(this->query_for_dpi(spi_get_nonclientmetrics, ncm_size, this->out, expected.dpi)) << expected.dpi;

            const auto metrics = this->emu.memory.read_memory<EMU_NONCLIENTMETRICS>(this->out);
            EXPECT_EQ(ncm_size, metrics.cbSize) << expected.dpi;
            EXPECT_EQ(expected.border, metrics.iBorderWidth) << expected.dpi;
            EXPECT_EQ(expected.scroll, metrics.iScrollHeight) << expected.dpi;
            EXPECT_EQ(expected.caption, metrics.iCaptionWidth) << expected.dpi;
            EXPECT_EQ(expected.menu, metrics.iMenuHeight) << expected.dpi;
            EXPECT_EQ(expected.padded_border, metrics.iPaddedBorderWidth) << expected.dpi;
            EXPECT_EQ(expected.font_height, metrics.lfMenuFont.lfHeight) << expected.dpi;
        }

        this->clear_output();
        this->set_struct_size(icon_metrics_size);
        ASSERT_TRUE(this->query_for_dpi(spi_get_iconmetrics, icon_metrics_size, this->out, 144));
        const auto icons = this->emu.memory.read_memory<EMU_ICONMETRICS>(this->out);
        EXPECT_EQ(113, icons.iHorzSpacing);
        EXPECT_EQ(113, icons.iVertSpacing);
        EXPECT_EQ(-18, icons.lfFont.lfHeight);
    }

    // Expected values captured on Windows 11 build 26300 (x64).
    TEST_F(SystemParametersInfoTest, SystemMetricsAndFailuresFollowTheCapture)
    {
        this->clear_output();
        ASSERT_TRUE(this->query(spi_get_nonclientmetrics, ncm_without_padded_border_size, this->out));
        const auto system_metrics = this->emu.memory.read_memory<EMU_NONCLIENTMETRICS>(this->out);
        EXPECT_EQ(ncm_without_padded_border_size, system_metrics.cbSize);
        EXPECT_EQ(19, system_metrics.iMenuWidth);
        EXPECT_EQ(0xCCCCCCCC, static_cast<uint32_t>(system_metrics.iPaddedBorderWidth));

        for (const uint32_t ui : {0u, 1u, 91u, 200u})
        {
            this->clear_output();
            EXPECT_TRUE(this->query(spi_get_icontitle_logfont, ui, this->out)) << ui;
            EXPECT_EQ(-12, this->emu.memory.read_memory<EMU_LOGFONTW>(this->out).lfHeight) << ui;
        }

        const auto unmapped = this->emu.memory.find_free_allocation_base(0x10000);
        ASSERT_NE(0u, unmapped);
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        for (const uint64_t pointer : {uint64_t{0}, uint64_t{0x10}, unmapped, read_only})
        {
            this->clear_output();
            EXPECT_FALSE(this->query(spi_get_workarea, 0, pointer)) << pointer;
            EXPECT_EQ(error_noaccess, this->last_error()) << pointer;
        }
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(read_only));

        struct for_dpi_failure
        {
            uint32_t action;
            uint32_t param;
            uint64_t value;
            uint32_t dpi;
            uint32_t struct_size{ncm_size};
        };

        for (const auto& failure :
             {for_dpi_failure{spi_get_nonclientmetrics, ncm_size, this->out, 0},
              for_dpi_failure{spi_get_nonclientmetrics, 0, this->out, 96},
              for_dpi_failure{spi_get_nonclientmetrics, ncm_without_padded_border_size, this->out, 96},
              for_dpi_failure{spi_get_nonclientmetrics, ncm_size, 0, 96},
              for_dpi_failure{spi_get_iconmetrics, 0, this->out, 96, icon_metrics_size},
              for_dpi_failure{spi_get_icontitle_logfont, 0, this->out, 96}, for_dpi_failure{spi_get_icontitle_logfont, 1, this->out, 96},
              for_dpi_failure{spi_get_icontitle_logfont, logfont_size - 1, this->out, 96},
              for_dpi_failure{spi_get_icontitle_logfont, 200, this->out, 96}, for_dpi_failure{spi_get_workarea, 0, this->out, 96},
              for_dpi_failure{spi_get_animation, 8, this->out, 96}})
        {
            this->clear_output();
            this->set_struct_size(failure.struct_size);
            EXPECT_FALSE(this->query_for_dpi(failure.action, failure.param, failure.value, failure.dpi)) << failure.action;
            EXPECT_EQ(error_invalid_parameter, this->last_error()) << failure.action;
            EXPECT_EQ(failure.struct_size, this->emu.memory.read_memory<uint32_t>(this->out)) << failure.action;
            EXPECT_EQ(0xCC, this->emu.memory.read_memory<uint8_t>(this->out + sizeof(uint32_t))) << failure.action;
        }

        this->clear_output();
        ASSERT_TRUE(this->query(spi_get_animation, 8, this->out));
        EXPECT_EQ(1, this->emu.memory.read_memory<int32_t>(this->out + 4));

        this->clear_output();
        EXPECT_FALSE(this->query(spi_get_animation, 0, this->out));
        EXPECT_EQ(0, this->emu.memory.read_memory<int32_t>(this->out + 4));
        EXPECT_EQ(0u, this->last_error());

        for (const uint32_t action : {0u, 0x12u})
        {
            this->clear_output();
            EXPECT_FALSE(this->query(action, 0, this->out)) << action;
            EXPECT_EQ(error_invalid_spi_value, this->last_error()) << action;
        }

        for (const uint32_t action : {0x7FFFu, 0xFFFFu, 0x1FFFu, 0xFFFFFFFFu, 0x10000u})
        {
            this->clear_output();
            EXPECT_FALSE(this->query(action, 0, this->out)) << action;
            EXPECT_EQ(error_invalid_parameter, this->last_error()) << action;
            EXPECT_EQ(0xCC, this->emu.memory.read_memory<uint8_t>(this->out)) << action;
        }
    }

    // Expected values captured on Windows 11 build 26100 (x64) with a 0xCC-filled buffer.
    TEST_F(SystemParametersInfoTest, WrongStructSizeFailsButWritesBackTheZeroedStructure)
    {
        struct size_mismatch
        {
            uint32_t action;
            uint32_t size;
            uint32_t struct_size;
        };

        for (const auto& failure :
             {size_mismatch{spi_get_nonclientmetrics, ncm_size, ncm_without_padded_border_size},
              size_mismatch{spi_get_nonclientmetrics, ncm_size, 0}, size_mismatch{spi_get_iconmetrics, icon_metrics_size, 0},
              size_mismatch{spi_get_iconmetrics, icon_metrics_size, 100}})
        {
            this->clear_output();
            this->set_struct_size(failure.struct_size);
            EXPECT_FALSE(this->query_for_dpi(failure.action, failure.size, this->out, 96)) << failure.action;
            EXPECT_EQ(error_invalid_parameter, this->last_error()) << failure.action;
            EXPECT_EQ(failure.struct_size, this->emu.memory.read_memory<uint32_t>(this->out)) << failure.action;

            for (uint32_t offset = sizeof(uint32_t); offset < failure.size; ++offset)
            {
                ASSERT_EQ(0x00, this->emu.memory.read_memory<uint8_t>(this->out + offset)) << failure.action << " +" << offset;
            }

            EXPECT_EQ(0xCC, this->emu.memory.read_memory<uint8_t>(this->out + failure.size)) << failure.action;
        }
    }

    // Expected values captured on Windows 11 build 26300 (x64) by calling win32u!NtUserSystemParametersInfo directly.
    TEST_F(SystemParametersInfoTest, SizedStructureGetsCheckTheStructSizeInsteadOfTheParameter)
    {
        struct sized_structure
        {
            uint32_t action;
            uint32_t size;
        };

        for (const auto& [action, size] :
             {sized_structure{spi_get_filterkeys, 24}, sized_structure{spi_get_togglekeys, 8}, sized_structure{spi_get_mousekeys, 28},
              sized_structure{spi_get_stickykeys, 8}, sized_structure{spi_get_accesstimeout, 12}, sized_structure{spi_get_soundsentry, 56}})
        {
            struct call
            {
                uint32_t param;
                uint32_t struct_size;
                bool succeeds;
            };

            for (const auto& [param, struct_size, succeeds] :
                 {call{0, size, true}, call{size, size, true}, call{size, 0, false}, call{size, size + 4, false}, call{0, 0, false},
                  call{1, size, false}, call{16, size, false}, call{size + 4, size, false}, call{0, size + 4, false},
                  call{size, 100, false}})
            {
                this->clear_output();
                this->set_struct_size(struct_size);
                EXPECT_EQ(succeeds, this->query(action, param, this->out) == TRUE) << action << " " << param << " " << struct_size;
                EXPECT_EQ(0u, this->last_error()) << action << " " << param << " " << struct_size;
                EXPECT_EQ(struct_size, this->emu.memory.read_memory<uint32_t>(this->out)) << action << " " << param << " " << struct_size;
                EXPECT_EQ(!succeeds, this->emu.memory.read_memory<uint32_t>(this->out + sizeof(uint32_t)) == 0)
                    << action << " " << param << " " << struct_size;
                EXPECT_EQ(0xCC, this->emu.memory.read_memory<uint8_t>(this->out + size)) << action << " " << param << " " << struct_size;
            }

            this->clear_output();
            const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
            EXPECT_FALSE(this->query(action, size, read_only)) << action;
            EXPECT_EQ(error_noaccess, this->last_error()) << action;
        }

        for (const auto& [param, struct_size] : {std::pair{0u, 20u}, std::pair{20u, 0u}, std::pair{1u, 24u}, std::pair{24u, 100u}})
        {
            this->clear_output();
            this->set_struct_size(struct_size);
            EXPECT_TRUE(this->query(spi_get_minimizedmetrics, param, this->out)) << param << " " << struct_size;
            EXPECT_EQ(20u, this->emu.memory.read_memory<uint32_t>(this->out)) << param << " " << struct_size;
            EXPECT_EQ(154u, this->emu.memory.read_memory<uint32_t>(this->out + sizeof(uint32_t))) << param << " " << struct_size;
            EXPECT_EQ(0xCC, this->emu.memory.read_memory<uint8_t>(this->out + 20)) << param << " " << struct_size;
        }
    }

    // Expected values captured on Windows 11 build 26100 (x64): the whole output structure is probed before cbSize is looked at.
    TEST_F(SystemParametersInfoTest, UnwritableOutputFailsWithNoAccessEvenWhenTheStructSizeIsWrong)
    {
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        const auto split = this->emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.release_memory(split + 0x1000, 0x1000));
        const auto tail = split + 0x1000 - sizeof(uint32_t);

        for (const auto& [action, size] :
             {std::pair{spi_get_nonclientmetrics, ncm_size}, std::pair{spi_get_iconmetrics, icon_metrics_size}})
        {
            for (const uint64_t pointer : {read_only, tail})
            {
                this->clear_output();
                this->emu.memory.write_memory(tail, uint32_t{0x12345678});
                EXPECT_FALSE(this->query_for_dpi(action, size, pointer, 96)) << action << " " << pointer;
                EXPECT_EQ(error_noaccess, this->last_error()) << action << " " << pointer;
                EXPECT_EQ(0x12345678u, this->emu.memory.read_memory<uint32_t>(tail)) << action << " " << pointer;
            }
        }

        EXPECT_EQ(0u, this->emu.memory.read_memory<uint32_t>(read_only));
    }

    // Expected values captured on Windows 11 build 26100 (x64) for a program linked at subsystem 6.0 and the same program patched
    // to subsystem 5.2.
    TEST_F(SystemParametersInfoTest, SystemBorderWidthsFollowTheExecutableSubsystemVersion)
    {
        struct border_widths
        {
            uint16_t major;
            LONG border;
            LONG padded_border;
        };

        for (const auto& expected : {border_widths{6, 1, 4}, border_widths{5, 5, 0}})
        {
            this->set_subsystem_version(expected.major);
            this->clear_output();
            ASSERT_TRUE(this->query(spi_get_nonclientmetrics, ncm_size, this->out)) << expected.major;

            const auto metrics = this->emu.memory.read_memory<EMU_NONCLIENTMETRICS>(this->out);
            EXPECT_EQ(expected.border, metrics.iBorderWidth) << expected.major;
            EXPECT_EQ(expected.padded_border, metrics.iPaddedBorderWidth) << expected.major;
        }
    }

    TEST_F(SystemParametersInfoTest, SystemBorderWidthsSurviveTheExecutableErasingItsHeaders)
    {
        this->set_subsystem_version(6);

        const auto image_base = this->emu.mod_manager.executable->image_base;
        const auto headers_size = this->emu.memory.read_memory<PEDosHeader_t>(image_base).e_lfanew + sizeof(PENTHeaders_t<std::uint64_t>);
        this->emu.memory.set_memory(image_base, 0, headers_size);

        this->clear_output();
        ASSERT_TRUE(this->query(spi_get_nonclientmetrics, ncm_size, this->out));
        const auto metrics = this->emu.memory.read_memory<EMU_NONCLIENTMETRICS>(this->out);
        EXPECT_EQ(1, metrics.iBorderWidth);
        EXPECT_EQ(4, metrics.iPaddedBorderWidth);
    }

    // Expected values captured on Windows 11 build 26300 (x64): uiParam through SystemParametersInfoW, the rest through
    // win32u!NtUserSystemParametersInfo.
    TEST_F(SystemParametersInfoTest, AddedGetsFollowTheCapture)
    {
        for (const auto& row : added_scalar_gets)
        {
            this->expect_scalar_get(row);
        }

        for (const auto& row : added_struct_gets)
        {
            this->clear_output(last_error_sentinel);
            this->set_struct_size(row.struct_size);
            EXPECT_EQ(row.succeeds, this->query(row.action, row.param, this->out) == TRUE)
                << row.action << " " << row.param << " " << row.struct_size;
            EXPECT_EQ(row.error, this->last_error()) << row.action << " " << row.param << " " << row.struct_size;

            std::array<uint8_t, 16> actual{};
            this->emu.memory.read_memory(this->out, actual.data(), actual.size());
            EXPECT_EQ(row.bytes, actual) << row.action << " " << row.param << " " << row.struct_size;
        }

        for (const uint32_t action : {0x74u, 0x9Cu})
        {
            this->clear_output(last_error_sentinel);
            EXPECT_FALSE(this->query(action, action == 0x74 ? 12 : 16, 0)) << action;
            EXPECT_EQ(error_noaccess, this->last_error()) << action;
        }

        for (const uint32_t action : {0x3Eu, 0x5Cu})
        {
            for (const uint64_t pointer : {this->out, uint64_t{0}})
            {
                this->clear_output(last_error_sentinel);
                EXPECT_FALSE(this->query(action, 0, pointer)) << action << " " << pointer;
                EXPECT_EQ(error_invalid_parameter, this->last_error()) << action << " " << pointer;
                EXPECT_EQ(0xCCCCCCCCCCCCCCCC, this->raw_output()) << action << " " << pointer;
            }
        }

        this->clear_output(last_error_sentinel);
        EXPECT_TRUE(this->query(0x59, 0, this->out));
        EXPECT_EQ(last_error_sentinel, this->last_error());
        EXPECT_EQ(0x04090409u, this->raw_output());
    }

    // Expected values captured on Windows 11 build 26300 (x64): SPI_GETLOGICALDPIOVERRIDE reports failure after writing a zero,
    // and SPI_GETDESKWALLPAPER fills up to 260 characters of the buffer the caller names.
    TEST_F(SystemParametersInfoTest, RecapturedGetsFollowTheCapture)
    {
        for (const auto& row : recaptured_scalar_gets)
        {
            this->expect_scalar_get(row);
        }

        for (const uint32_t param : {0u, 4u, 8u})
        {
            this->clear_output(last_error_sentinel);
            EXPECT_FALSE(this->query(0x9E, param, this->out)) << param;
            EXPECT_EQ(last_error_sentinel, this->last_error()) << param;
            EXPECT_EQ(written_value(0, sizeof(uint32_t)), this->raw_output()) << param;
        }

        this->clear_output(last_error_sentinel);
        EXPECT_FALSE(this->query(0x9E, 0, 0));
        EXPECT_EQ(error_noaccess, this->last_error());

        for (const auto& [capacity, written_bytes] : wallpaper_gets)
        {
            this->clear_output(last_error_sentinel);
            EXPECT_TRUE(this->query(0x73, capacity, this->out)) << capacity;
            EXPECT_EQ(last_error_sentinel, this->last_error()) << capacity;

            for (uint32_t offset = 0; offset < written_bytes + sizeof(uint32_t); ++offset)
            {
                EXPECT_EQ(offset < written_bytes ? 0x00 : 0xCC, this->emu.memory.read_memory<uint8_t>(this->out + offset))
                    << capacity << " +" << offset;
            }
        }
    }

    // Expected values read from NtUserSystemParametersInfo in win32kbase 10.0.26100.9974; the capture has no row for them.
    TEST_F(SystemParametersInfoTest, GetsFollowTheRulesTheCaptureDoesNotCover)
    {
        for (const uint32_t action : {0x1028u, 0x2038u})
        {
            this->clear_output(last_error_sentinel);
            EXPECT_FALSE(this->query(action, 4, this->out)) << action;
            EXPECT_EQ(error_invalid_parameter, this->last_error()) << action;
            EXPECT_EQ(0xCCCCCCCCCCCCCCCC, this->raw_output()) << action;
        }

        this->clear_output(last_error_sentinel);
        EXPECT_TRUE(this->query(0x1026, 4, this->out));
        EXPECT_EQ(last_error_sentinel, this->last_error());
        EXPECT_EQ(written_value(0, sizeof(uint32_t)), this->raw_output());

        this->clear_output(last_error_sentinel);
        EXPECT_TRUE(this->query(0x73, 261, this->out));
        EXPECT_EQ(last_error_sentinel, this->last_error());
        for (uint32_t offset = 0; offset < 522; ++offset)
        {
            EXPECT_EQ(offset < 520 ? 0x00 : 0xCC, this->emu.memory.read_memory<uint8_t>(this->out + offset)) << offset;
        }

        const auto split = this->emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.release_memory(split + 0x1000, 0x1000));
        const auto tail = split + 0x1000 - 520;
        this->clear_output(last_error_sentinel);
        this->emu.memory.set_memory(tail, 0xCC, 520);
        EXPECT_FALSE(this->query(0x73, 261, tail));
        EXPECT_EQ(error_noaccess, this->last_error());
        EXPECT_EQ(0xCC, this->emu.memory.read_memory<uint8_t>(tail));

        for (const uint32_t param : {0u, 20u})
        {
            this->clear_output(last_error_sentinel);
            EXPECT_FALSE(this->query(0x9C, param, 0)) << param;
            EXPECT_EQ(error_invalid_parameter, this->last_error()) << param;
        }

        constexpr std::array<std::pair<uint32_t, uint32_t>, 5> update_ini_file_gets{
            {{0x201E, 0x23}, {0x2020, 3}, {0x2026, 0}, {0x2030, 1}, {0x2034, 0}}};
        for (const auto& [action, value] : update_ini_file_gets)
        {
            for (const uint32_t win_ini : {1u, 2u, 3u})
            {
                this->clear_output(last_error_sentinel);
                EXPECT_EQ((win_ini & 1) == 0, this->query(action, 0, this->out, win_ini) == TRUE) << action << " " << win_ini;
                EXPECT_EQ(last_error_sentinel, this->last_error()) << action << " " << win_ini;
                EXPECT_EQ(written_value(value, sizeof(value)), this->raw_output()) << action << " " << win_ini;
            }
        }

        for (const uint32_t action : {0x1020u, 0x2028u, 0x2024u})
        {
            this->clear_output(last_error_sentinel);
            EXPECT_TRUE(this->query(action, 0, this->out, 1)) << action;
            EXPECT_EQ(last_error_sentinel, this->last_error()) << action;
        }
    }

    // Expected values read from NtUserSystemParametersInfo in win32kbase 10.0.26100.9974; the capture has no row for them.
    TEST_F(SystemParametersInfoTest, MisalignedOutputsFailBeforeAnythingIsWritten)
    {
        constexpr std::array<std::pair<uint32_t, uint32_t>, 3> getters{{{0x9C, 16}, {0x74, 12}, {0x59, 0}}};

        for (const auto& [action, param] : getters)
        {
            for (const uint64_t offset : {1u, 2u, 3u})
            {
                this->clear_output(last_error_sentinel);
                this->emu.memory.write_memory(this->out + offset, uint32_t{action == 0x74 ? 12u : 0u});
                EXPECT_FALSE(this->query(action, param, this->out + offset)) << action << " +" << offset;
                EXPECT_EQ(error_noaccess, this->last_error()) << action << " +" << offset;
                EXPECT_EQ(action == 0x74 ? uint32_t{12} : 0u, this->emu.memory.read_memory<uint32_t>(this->out + offset))
                    << action << " +" << offset;
                EXPECT_EQ(0xCCCCCCCCCCCCCCCC, this->emu.memory.read_memory<uint64_t>(this->out + offset + 4)) << action << " +" << offset;
            }

            this->clear_output(last_error_sentinel);
            this->emu.memory.write_memory(this->out + 4, uint32_t{action == 0x74 ? 12u : 0u});
            EXPECT_TRUE(this->query(action, param, this->out + 4)) << action;
            EXPECT_EQ(last_error_sentinel, this->last_error()) << action;
        }

        this->emu.process.is_wow64_process = true;
        this->clear_output(last_error_sentinel);
        EXPECT_TRUE(this->query(0x59, 0, this->out + 1));
        EXPECT_EQ(0x04090409u, this->emu.memory.read_memory<uint64_t>(this->out + 1));
    }
}

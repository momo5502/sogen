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

    namespace
    {
        constexpr uint32_t spi_set_filterkeys = 0x33;
        constexpr uint32_t spi_set_togglekeys = 0x35;
        constexpr uint32_t spi_set_mousekeys = 0x37;
        constexpr uint32_t spi_get_showsounds = 0x38;
        constexpr uint32_t spi_set_showsounds = 0x39;
        constexpr uint32_t spi_set_stickykeys = 0x3B;
        constexpr uint32_t spi_set_accesstimeout = 0x3D;
        constexpr uint32_t spi_set_soundsentry = 0x41;
        constexpr uint32_t spi_get_highcontrast = 0x42;
        constexpr uint32_t spi_set_highcontrast = 0x43;
        constexpr uint32_t spi_get_keyboardpref = 0x44;
        constexpr uint32_t spi_set_keyboardpref = 0x45;
        constexpr uint32_t spi_get_screenreader = 0x46;
        constexpr uint32_t spi_set_screenreader = 0x47;
        constexpr uint32_t spi_get_audiodescription = 0x74;
        constexpr uint32_t spi_set_audiodescription = 0x75;
        constexpr uint32_t spif_update_ini_file = 0x1;
        constexpr size_t sm_showsounds = 70;
        constexpr size_t high_contrast_scheme_size = 0x100;
        constexpr DWORD mouse_keys_on = 0x1;
        constexpr DWORD mouse_keys_replace_numbers = 0x80;
        constexpr DWORD mouse_keys_left_button_selected = 0x10000000;
        constexpr DWORD mouse_keys_mouse_mode = 0x80000000;
        constexpr uint8_t key_toggled = 0x01;
        constexpr uint64_t left_shift_scan_code = 0x2A;
        constexpr uint64_t left_alt_scan_code = 0x38;
        constexpr int scan_code_shift = 16;
        constexpr int wh_keyboard_ll = 13;
        constexpr uint64_t keyboard_hook_handle = 0x10001;
        constexpr uint64_t keyboard_hook_proc = 0x140001000;

        constexpr std::u16string_view sticky_keys_key = u"Control Panel\\Accessibility\\StickyKeys";
        constexpr std::u16string_view keyboard_response_key = u"Control Panel\\Accessibility\\Keyboard Response";
        constexpr std::u16string_view mouse_keys_key = u"Control Panel\\Accessibility\\MouseKeys";
        constexpr std::u16string_view toggle_keys_key = u"Control Panel\\Accessibility\\ToggleKeys";
        constexpr std::u16string_view time_out_key = u"Control Panel\\Accessibility\\TimeOut";
        constexpr std::u16string_view sound_sentry_key = u"Control Panel\\Accessibility\\SoundSentry";
        constexpr std::u16string_view audio_description_key = u"Control Panel\\Accessibility\\AudioDescription";

        struct internal_set_high_contrast
        {
            UINT cbSize;
            DWORD dwFlags;
            USHORT Length;
            USHORT MaximumLength;
            uint32_t padding;
            uint64_t Buffer;
        };

        std::optional<registry_key> user_key(windows_emulator& emu, const std::u16string_view key)
        {
            return emu.registry.get_key(utils::path_key{std::filesystem::path{u"\\Registry\\User"} / key});
        }

        void set_user_value(windows_emulator& emu, const std::u16string_view key, const std::string_view name,
                            const std::u16string_view text)
        {
            const auto registry_key = user_key(emu, key);
            ASSERT_TRUE(registry_key.has_value());

            std::vector<std::byte> data((text.size() + 1) * sizeof(char16_t));
            std::memcpy(data.data(), text.data(), text.size() * sizeof(char16_t));
            emu.registry.set_value(*registry_key, std::string{name}, REG_SZ, data);
        }

        std::optional<std::u16string> user_string(windows_emulator& emu, const std::u16string_view key, const std::string_view name)
        {
            const auto registry_key = user_key(emu, key);
            if (!registry_key)
            {
                return std::nullopt;
            }

            const auto value = emu.registry.get_value(*registry_key, name);
            return value ? value->as_string() : std::nullopt;
        }

        EMU_FILTERKEYS make_filter_keys(const DWORD flags, const DWORD wait_ms = 0, const DWORD delay_ms = 0, const DWORD repeat_ms = 0,
                                        const DWORD bounce_ms = 0)
        {
            return {.cbSize = sizeof(EMU_FILTERKEYS),
                    .dwFlags = flags,
                    .iWaitMSec = wait_ms,
                    .iDelayMSec = delay_ms,
                    .iRepeatMSec = repeat_ms,
                    .iBounceMSec = bounce_ms};
        }

        EMU_MOUSEKEYS make_mouse_keys(const DWORD flags, const DWORD max_speed, const DWORD time_to_max_speed, const DWORD ctrl_speed = 0)
        {
            return {.cbSize = sizeof(EMU_MOUSEKEYS),
                    .dwFlags = flags,
                    .iMaxSpeed = max_speed,
                    .iTimeToMaxSpeed = time_to_max_speed,
                    .iCtrlSpeed = ctrl_speed,
                    .dwReserved1 = 0,
                    .dwReserved2 = 0};
        }

        EMU_SOUNDSENTRY make_sound_sentry(const DWORD flags, const DWORD windows_effect, const DWORD text_effect = 0,
                                          const DWORD graphics_effect = 0)
        {
            return {.cbSize = sizeof(EMU_SOUNDSENTRY),
                    .dwFlags = flags,
                    .iFSTextEffect = text_effect,
                    .iFSTextEffectMSec = 0,
                    .iFSTextEffectColorBits = 0,
                    .iFSGrafEffect = graphics_effect,
                    .iFSGrafEffectMSec = 0,
                    .iFSGrafEffectColor = 0,
                    .iWindowsEffect = windows_effect,
                    .iWindowsEffectMSec = 0,
                    .lpszWindowsEffectDLL = 0,
                    .iWindowsEffectOrdinal = 0};
        }

        class SystemParametersInfoAccessibilityTest : public SystemParametersInfoTest
        {
          protected:
            template <typename T>
            BOOL set(const uint32_t action, const T& settings, const uint32_t win_ini = 0)
            {
                this->emu.memory.write_memory(this->out, settings);
                return this->query(action, sizeof(T), this->out, win_ini);
            }

            template <typename T>
            T get(const uint32_t action)
            {
                this->emu.memory.set_memory(this->out, 0, sizeof(T));
                this->emu.memory.write_memory(this->out, static_cast<uint32_t>(sizeof(T)));
                EXPECT_TRUE(this->query(action, sizeof(T), this->out)) << action;
                return this->emu.memory.read_memory<T>(this->out);
            }

            bool reports_mouse_mode()
            {
                return (this->get<EMU_MOUSEKEYS>(spi_get_mousekeys).dwFlags & mouse_keys_mouse_mode) != 0;
            }

            bool num_lock_toggled()
            {
                return (this->emu.process.key_state[VK_NUMLOCK] & key_toggled) != 0;
            }

            hwnd create_input_window()
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.thread_id = this->emu.vcpu(0).active_thread->id;
                return handle.bits;
            }
        };
    }

    // win32kbase 10.0.19041 xxxUpdatePerUserAccessPackSettings reads these values when a user logs on, MouseKeys' speeds
    // only within 10-360 and 1000-5000, and sets the mouse mode whether or not MouseKeys is on.
    TEST(SystemParametersInfoLogonTest, AccessibilitySettingsAreTheUsersRegistryValuesAtLogon)
    {
        auto emu = create_sample_emulator();
        set_user_value(emu, sticky_keys_key, "Flags", u"26");
        set_user_value(emu, keyboard_response_key, "Flags", u"3");
        set_user_value(emu, keyboard_response_key, "DelayBeforeAcceptance", u"500");
        set_user_value(emu, keyboard_response_key, "AutoRepeatDelay", u"300");
        set_user_value(emu, keyboard_response_key, "AutoRepeatRate", u"100");
        set_user_value(emu, mouse_keys_key, "Flags", u"128");
        set_user_value(emu, mouse_keys_key, "MaximumSpeed", u"5");
        set_user_value(emu, mouse_keys_key, "TimeToMaximumSpeed", u" 2000");
        set_user_value(emu, toggle_keys_key, "Flags", u"5");
        set_user_value(emu, time_out_key, "Flags", u"3");
        set_user_value(emu, time_out_key, "TimeToWait", u"600000");
        emu.setup_process_if_necessary();

        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        const auto out = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto get = [&]<typename T>(const uint32_t action, T result) {
            emu.memory.write_memory(out, static_cast<uint32_t>(sizeof(T)));
            EXPECT_TRUE(syscalls::handle_NtUserSystemParametersInfo(context, action, sizeof(T), out, 0)) << action;
            emu.memory.read_memory(out, &result, sizeof(T));
            return result;
        };

        const auto sticky_keys = get(spi_get_stickykeys, EMU_STICKYKEYS{});
        EXPECT_EQ(26u, sticky_keys.dwFlags);

        const auto filter_keys = get(spi_get_filterkeys, EMU_FILTERKEYS{});
        EXPECT_EQ(3u, filter_keys.dwFlags);
        EXPECT_EQ(500u, filter_keys.iWaitMSec);
        EXPECT_EQ(300u, filter_keys.iDelayMSec);
        EXPECT_EQ(100u, filter_keys.iRepeatMSec);

        const auto mouse_keys = get(spi_get_mousekeys, EMU_MOUSEKEYS{});
        EXPECT_EQ(mouse_keys_replace_numbers | mouse_keys_left_button_selected | mouse_keys_mouse_mode, mouse_keys.dwFlags);
        EXPECT_EQ(40u, mouse_keys.iMaxSpeed);
        EXPECT_EQ(2000u, mouse_keys.iTimeToMaxSpeed);

        EXPECT_EQ(5u, get(spi_get_togglekeys, EMU_TOGGLEKEYS{}).dwFlags);

        const auto timeout = get(spi_get_accesstimeout, EMU_ACCESSTIMEOUT{});
        EXPECT_EQ(3u, timeout.dwFlags);
        EXPECT_EQ(600000u, timeout.iTimeOutMSec);
    }

    // win32kbase 10.0.19041 SpiSetFilterKeys, SpiSetToggleKeys, SpiSetStickyKeys and SpiSetAccessTimeOut, and win32kfull's
    // SPI_SETMOUSEKEYS, SPI_SETSOUNDSENTRY and SPI_SETAUDIODESCRIPTION: the *_AVAILABLE flag stays what it was, StickyKeys
    // drops its state word and MouseKeys the flags only its GET reports.
    TEST_F(SystemParametersInfoAccessibilityTest, SetsChangeWhatTheNextGetReports)
    {
        ASSERT_TRUE(this->set(spi_set_filterkeys, make_filter_keys(0x5C, 0, 700, 300, 200)));
        const auto filter_keys = this->get<EMU_FILTERKEYS>(spi_get_filterkeys);
        EXPECT_EQ(0x5Eu, filter_keys.dwFlags);
        EXPECT_EQ(0u, filter_keys.iWaitMSec);
        EXPECT_EQ(700u, filter_keys.iDelayMSec);
        EXPECT_EQ(300u, filter_keys.iRepeatMSec);
        EXPECT_EQ(200u, filter_keys.iBounceMSec);

        ASSERT_TRUE(this->set(spi_set_togglekeys, EMU_TOGGLEKEYS{.cbSize = 8, .dwFlags = 0x38}));
        EXPECT_EQ(0x3Au, this->get<EMU_TOGGLEKEYS>(spi_get_togglekeys).dwFlags);

        ASSERT_TRUE(this->set(spi_set_stickykeys, EMU_STICKYKEYS{.cbSize = 8, .dwFlags = 0x101F8}));
        EXPECT_EQ(0x1FAu, this->get<EMU_STICKYKEYS>(spi_get_stickykeys).dwFlags);

        ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(mouse_keys_left_button_selected | 0x3C, 200, 2000, 5)));
        const auto mouse_keys = this->get<EMU_MOUSEKEYS>(spi_get_mousekeys);
        EXPECT_EQ(mouse_keys_left_button_selected | 0x3E, mouse_keys.dwFlags);
        EXPECT_EQ(200u, mouse_keys.iMaxSpeed);
        EXPECT_EQ(2000u, mouse_keys.iTimeToMaxSpeed);
        EXPECT_EQ(5u, mouse_keys.iCtrlSpeed);

        ASSERT_TRUE(this->set(spi_set_accesstimeout, EMU_ACCESSTIMEOUT{.cbSize = 12, .dwFlags = 1, .iTimeOutMSec = 120000}));
        const auto timeout = this->get<EMU_ACCESSTIMEOUT>(spi_get_accesstimeout);
        EXPECT_EQ(1u, timeout.dwFlags);
        EXPECT_EQ(120000u, timeout.iTimeOutMSec);

        ASSERT_TRUE(this->set(spi_set_soundsentry, make_sound_sentry(1, 2)));
        const auto sentry = this->get<EMU_SOUNDSENTRY>(spi_get_soundsentry);
        EXPECT_EQ(3u, sentry.dwFlags);
        EXPECT_EQ(2u, sentry.iWindowsEffect);

        ASSERT_TRUE(this->set(spi_set_audiodescription, EMU_AUDIODESCRIPTION{.cbSize = 12, .Enabled = TRUE, .Locale = 0x409}));
        const auto description = this->get<EMU_AUDIODESCRIPTION>(spi_get_audiodescription);
        EXPECT_EQ(TRUE, description.Enabled);
        EXPECT_EQ(0x409u, description.Locale);
    }

    TEST_F(SystemParametersInfoAccessibilityTest, SetsRejectWhatWin32kRejects)
    {
        const auto rejected = [&]<typename T>(const uint32_t action, const T& settings, const uint32_t param = sizeof(T)) {
            this->clear_output(last_error_sentinel);
            this->emu.memory.write_memory(this->out, settings);
            return !this->query(action, param, this->out) && this->last_error() == last_error_sentinel;
        };

        const auto filter_keys = this->get<EMU_FILTERKEYS>(spi_get_filterkeys);
        auto short_filter_keys = make_filter_keys(0x7E);
        short_filter_keys.cbSize = 20;
        EXPECT_TRUE(rejected(spi_set_filterkeys, short_filter_keys));
        EXPECT_TRUE(rejected(spi_set_filterkeys, make_filter_keys(0x7E), 4));
        EXPECT_TRUE(rejected(spi_set_filterkeys, make_filter_keys(0x7E, 100, 0, 0, 100)));
        EXPECT_TRUE(rejected(spi_set_filterkeys, make_filter_keys(0x80)));
        EXPECT_TRUE(rejected(spi_set_filterkeys, make_filter_keys(0x7E, 0, 20001)));
        EXPECT_EQ(filter_keys.dwFlags, this->get<EMU_FILTERKEYS>(spi_get_filterkeys).dwFlags);

        EXPECT_TRUE(rejected(spi_set_togglekeys, EMU_TOGGLEKEYS{.cbSize = 8, .dwFlags = 0x40}));
        EXPECT_TRUE(rejected(spi_set_togglekeys, EMU_TOGGLEKEYS{.cbSize = 8, .dwFlags = 0x3E}, 16));
        EXPECT_TRUE(rejected(spi_set_stickykeys, EMU_STICKYKEYS{.cbSize = 8, .dwFlags = 0x200}));

        EXPECT_TRUE(rejected(spi_set_mousekeys, make_mouse_keys(0x100, 80, 3000)));
        EXPECT_TRUE(rejected(spi_set_mousekeys, make_mouse_keys(0x3E, 9, 3000)));
        EXPECT_TRUE(rejected(spi_set_mousekeys, make_mouse_keys(0x3E, 361, 3000)));
        EXPECT_TRUE(rejected(spi_set_mousekeys, make_mouse_keys(0x3E, 80, 999)));
        EXPECT_TRUE(rejected(spi_set_mousekeys, make_mouse_keys(0x3E, 80, 5001)));

        EXPECT_TRUE(rejected(spi_set_accesstimeout, EMU_ACCESSTIMEOUT{.cbSize = 12, .dwFlags = 4, .iTimeOutMSec = 0}));
        EXPECT_TRUE(rejected(spi_set_accesstimeout, EMU_ACCESSTIMEOUT{.cbSize = 12, .dwFlags = 1, .iTimeOutMSec = 3600001}));

        EXPECT_TRUE(rejected(spi_set_soundsentry, make_sound_sentry(8, 0)));
        EXPECT_TRUE(rejected(spi_set_soundsentry, make_sound_sentry(2, 4)));
        EXPECT_TRUE(rejected(spi_set_soundsentry, make_sound_sentry(2, 0, 1)));
        EXPECT_TRUE(rejected(spi_set_soundsentry, make_sound_sentry(2, 0, 0, 1)));

        EXPECT_TRUE(rejected(spi_set_audiodescription, EMU_AUDIODESCRIPTION{.cbSize = 16, .Enabled = TRUE, .Locale = 0}));
    }

    // win32kfull 10.0.19041 SPI_SETMOUSEKEYS, which leaves the mouse mode alone while MouseKeys stays off.
    TEST_F(SystemParametersInfoAccessibilityTest, MouseKeysSetsTakeTheMouseModeFromReplaceNumbersAndNumLock)
    {
        struct mouse_keys_set
        {
            bool num_lock_on;
            DWORD flags;
            bool mouse_mode;
        };

        for (const auto& [num_lock_on, flags, mouse_mode] :
             {mouse_keys_set{false, mouse_keys_on | mouse_keys_replace_numbers, true}, mouse_keys_set{false, mouse_keys_on, false},
              mouse_keys_set{true, mouse_keys_on | mouse_keys_replace_numbers, false}, mouse_keys_set{true, mouse_keys_on, true},
              mouse_keys_set{false, 0, true}})
        {
            this->emu.process.key_state[VK_NUMLOCK] = num_lock_on ? key_toggled : uint8_t{0};
            ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(flags, 80, 3000))) << flags;
            EXPECT_EQ(mouse_mode, this->reports_mouse_mode()) << flags;
        }
    }

    // win32kbase 10.0.19041 xxxMKToggleMouseKeys, which leaves the mouse mode alone for the MouseKeys hotkey, Left Alt + Left
    // Shift + NumLock; it sees only the keyboard's own keys, never SendInput's.
    TEST_F(SystemParametersInfoAccessibilityTest, KeyboardNumLockPressesSetTheMouseModeWhileMouseKeysIsOn)
    {
        const auto window = this->create_input_window();
        const auto key = [&](const uint32_t message, const uint32_t virtual_key, const uint64_t scan_code, const bool injected) {
            this->emu.handle_ui_event(ui_event{
                .window = window, .message = message, .wParam = virtual_key, .lParam = scan_code << scan_code_shift, .injected = injected});
        };
        const auto press_num_lock = [&](const bool injected) {
            key(WM_KEYDOWN, VK_NUMLOCK, 0, injected);
            key(WM_KEYUP, VK_NUMLOCK, 0, injected);
        };
        const auto press_mouse_keys_hotkey = [&](const bool injected_modifiers) {
            key(WM_KEYDOWN, VK_SHIFT, left_shift_scan_code, injected_modifiers);
            key(WM_SYSKEYDOWN, VK_MENU, left_alt_scan_code, injected_modifiers);
            press_num_lock(false);
            key(WM_SYSKEYUP, VK_MENU, left_alt_scan_code, injected_modifiers);
            key(WM_KEYUP, VK_SHIFT, left_shift_scan_code, injected_modifiers);
        };

        ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(mouse_keys_on, 80, 3000)));
        press_num_lock(false);
        EXPECT_TRUE(this->reports_mouse_mode());
        press_num_lock(false);
        EXPECT_FALSE(this->reports_mouse_mode());

        press_num_lock(true);
        EXPECT_FALSE(this->reports_mouse_mode());

        press_num_lock(false);
        press_mouse_keys_hotkey(true);
        EXPECT_TRUE(this->reports_mouse_mode());

        press_num_lock(false);
        press_mouse_keys_hotkey(false);
        EXPECT_FALSE(this->reports_mouse_mode());

        ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(mouse_keys_replace_numbers, 80, 3000)));
        press_num_lock(false);
        EXPECT_FALSE(this->reports_mouse_mode());
    }

    // win32kbase 10.0.19041 ProcessKeyboardInputWorker runs the MouseKeys filter, one input at a time, before xxxKeyEventEx
    // calls the WH_KEYBOARD_LL hooks and changes the key state for the input they pass on.
    TEST_F(SystemParametersInfoAccessibilityTest, NumLockPressesSetTheMouseModeBeforeTheLowLevelHooksRun)
    {
        const auto window = this->create_input_window();
        auto& thread = *this->emu.vcpu(0).active_thread;
        thread.low_level_hooks.push_back(
            user_low_level_hook{.handle = keyboard_hook_handle, .id = wh_keyboard_ll, .proc = keyboard_hook_proc});
        const auto num_lock = [&](const uint32_t message) {
            this->emu.handle_ui_event(ui_event{.window = window, .message = message, .wParam = VK_NUMLOCK});
        };
        const auto mouse_mode_in_hook = [&](const bool swallow) {
            this->emu.synchronize([&] { EXPECT_TRUE(this->emu.begin_low_level_hook(thread).has_value()); });
            const bool reported = this->reports_mouse_mode();
            this->emu.synchronize([&] { this->emu.finish_low_level_hook(thread, swallow); });
            return reported;
        };

        ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(mouse_keys_on, 80, 3000)));
        num_lock(WM_KEYDOWN);
        EXPECT_TRUE(mouse_mode_in_hook(true));
        EXPECT_TRUE(this->reports_mouse_mode());
        EXPECT_FALSE(this->num_lock_toggled());

        num_lock(WM_KEYUP);
        EXPECT_TRUE(mouse_mode_in_hook(false));

        num_lock(WM_KEYDOWN);
        num_lock(WM_KEYUP);
        num_lock(WM_KEYDOWN);
        EXPECT_TRUE(mouse_mode_in_hook(false));
        EXPECT_TRUE(this->num_lock_toggled());
        EXPECT_TRUE(mouse_mode_in_hook(true));
        EXPECT_FALSE(mouse_mode_in_hook(false));
    }

    // NtUserSystemParametersInfo copies these structures in with a 4-byte aligned probe; a fault sets ERROR_NOACCESS.
    TEST_F(SystemParametersInfoAccessibilityTest, MisalignedOrUnreadableSettingsFailWithNoAccess)
    {
        const auto unmapped = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.release_memory(unmapped, 0x1000));

        for (const uint64_t pointer : {this->out + 2, unmapped, uint64_t{0}})
        {
            this->clear_output(last_error_sentinel);
            EXPECT_FALSE(this->query(spi_set_stickykeys, sizeof(EMU_STICKYKEYS), pointer)) << pointer;
            EXPECT_EQ(error_noaccess, this->last_error()) << pointer;
        }
    }

    // SetFilterKeys, SpiSetStickyKeys, SetMouseKeys, SetSoundSentry and the other writers store each value as a decimal REG_SZ
    // under HKCU\Control Panel\Accessibility; SetSoundSentry writes TextEffect, not the FSTextEffect logon reads.
    TEST_F(SystemParametersInfoAccessibilityTest, UpdateIniFileWritesTheSettingsAsDecimalStrings)
    {
        ASSERT_TRUE(this->set(spi_set_stickykeys, EMU_STICKYKEYS{.cbSize = 8, .dwFlags = 0x1F8}, spif_update_ini_file));
        EXPECT_EQ(u"506", user_string(this->emu, sticky_keys_key, "Flags"));

        ASSERT_TRUE(this->set(spi_set_filterkeys, make_filter_keys(0x7E, 1500, 700, 300), spif_update_ini_file));
        EXPECT_EQ(u"126", user_string(this->emu, keyboard_response_key, "Flags"));
        EXPECT_EQ(u"1500", user_string(this->emu, keyboard_response_key, "DelayBeforeAcceptance"));
        EXPECT_EQ(u"700", user_string(this->emu, keyboard_response_key, "AutoRepeatDelay"));
        EXPECT_EQ(u"300", user_string(this->emu, keyboard_response_key, "AutoRepeatRate"));
        EXPECT_EQ(u"0", user_string(this->emu, keyboard_response_key, "BounceTime"));

        ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(0x3E, 100, 2500), spif_update_ini_file));
        EXPECT_EQ(u"62", user_string(this->emu, mouse_keys_key, "Flags"));
        EXPECT_EQ(u"100", user_string(this->emu, mouse_keys_key, "MaximumSpeed"));
        EXPECT_EQ(u"2500", user_string(this->emu, mouse_keys_key, "TimeToMaximumSpeed"));

        ASSERT_TRUE(this->set(spi_set_togglekeys, EMU_TOGGLEKEYS{.cbSize = 8, .dwFlags = 0x3A}, spif_update_ini_file));
        EXPECT_EQ(u"58", user_string(this->emu, toggle_keys_key, "Flags"));

        ASSERT_TRUE(
            this->set(spi_set_accesstimeout, EMU_ACCESSTIMEOUT{.cbSize = 12, .dwFlags = 3, .iTimeOutMSec = 60000}, spif_update_ini_file));
        EXPECT_EQ(u"3", user_string(this->emu, time_out_key, "Flags"));
        EXPECT_EQ(u"60000", user_string(this->emu, time_out_key, "TimeToWait"));

        ASSERT_TRUE(this->set(spi_set_soundsentry, make_sound_sentry(3, 3), spif_update_ini_file));
        EXPECT_EQ(u"3", user_string(this->emu, sound_sentry_key, "Flags"));
        EXPECT_EQ(u"0", user_string(this->emu, sound_sentry_key, "TextEffect"));
        EXPECT_EQ(u"3", user_string(this->emu, sound_sentry_key, "WindowsEffect"));

        ASSERT_TRUE(
            this->set(spi_set_audiodescription, EMU_AUDIODESCRIPTION{.cbSize = 12, .Enabled = TRUE, .Locale = 1033}, spif_update_ini_file));
        EXPECT_EQ(u"1", user_string(this->emu, audio_description_key, "On"));
        EXPECT_EQ(u"1033", user_string(this->emu, audio_description_key, "Locale"));

        EXPECT_TRUE(this->query(spi_set_keyboardpref, 1, 0, spif_update_ini_file));
        EXPECT_EQ(u"1", user_string(this->emu, u"Control Panel\\Accessibility\\Keyboard Preference", "On"));
        EXPECT_TRUE(this->query(spi_set_screenreader, 1, 0, spif_update_ini_file));
        EXPECT_EQ(u"1", user_string(this->emu, u"Control Panel\\Accessibility\\Blind Access", "On"));
    }

    // SPI_SETSHOWSOUNDS turns ShowSounds on only for 1 and keeps SM_SHOWSOUNDS, which user32 answers itself, in step.
    TEST_F(SystemParametersInfoAccessibilityTest, ShowSoundsKeepsTheSystemMetricInStep)
    {
        const auto show_sounds_metric = [&] {
            return this->emu.process.user_handles.get_server_info().read().systemMetrics[sm_showsounds];
        };

        for (const auto& [param, on] : {std::pair{1u, 1u}, std::pair{2u, 0u}, std::pair{1u, 1u}, std::pair{0u, 0u}})
        {
            EXPECT_TRUE(this->query(spi_set_showsounds, param, 0)) << param;
            EXPECT_EQ(static_cast<int32_t>(on), show_sounds_metric()) << param;

            this->clear_output();
            EXPECT_TRUE(this->query(spi_get_showsounds, 0, this->out)) << param;
            EXPECT_EQ(on, this->emu.memory.read_memory<uint32_t>(this->out)) << param;
        }
    }

    // win32kfull 10.0.19041 SetHighContrastWorker keeps a scheme shorter than 0x100 bytes and drops HCF_OPTION_NOTHEMECHANGE from
    // the flags; SPI_GETHIGHCONTRAST writes the 256-byte scheme buffer to the caller's lpszDefaultScheme. win32kbase fails a
    // malformed scheme string with ERROR_NOACCESS.
    TEST_F(SystemParametersInfoAccessibilityTest, HighContrastKeepsItsSchemeAndFlags)
    {
        constexpr std::u16string_view scheme = u"High Contrast Black";
        const auto text = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        this->emu.memory.write_memory(text, scheme.data(), scheme.size() * sizeof(char16_t));

        const internal_set_high_contrast request{.cbSize = 16,
                                                 .dwFlags = 0x1001,
                                                 .Length = static_cast<USHORT>(scheme.size() * sizeof(char16_t)),
                                                 .MaximumLength = static_cast<USHORT>((scheme.size() + 1) * sizeof(char16_t)),
                                                 .padding = 0,
                                                 .Buffer = text};
        ASSERT_TRUE(this->set(spi_set_highcontrast, request));

        const auto scheme_buffer = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        this->emu.memory.set_memory(scheme_buffer, 0xCC, 0x1000);
        this->emu.memory.write_memory(this->out, EMU_HIGHCONTRAST{.cbSize = 16, .dwFlags = 0, .lpszDefaultScheme = scheme_buffer});
        ASSERT_TRUE(this->query(spi_get_highcontrast, 0, this->out));

        EXPECT_EQ(0x1u, this->emu.memory.read_memory<EMU_HIGHCONTRAST>(this->out).dwFlags);
        std::u16string reported(scheme.size(), u'\0');
        this->emu.memory.read_memory(scheme_buffer, reported.data(), reported.size() * sizeof(char16_t));
        EXPECT_EQ(scheme, reported);
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint16_t>(scheme_buffer + scheme.size() * sizeof(char16_t)));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint16_t>(scheme_buffer + high_contrast_scheme_size - sizeof(char16_t)));
        EXPECT_EQ(0xCCu, this->emu.memory.read_memory<uint8_t>(scheme_buffer + high_contrast_scheme_size));

        auto too_long = request;
        too_long.dwFlags = 0;
        too_long.Length = static_cast<USHORT>(high_contrast_scheme_size);
        too_long.MaximumLength = static_cast<USHORT>(high_contrast_scheme_size + sizeof(char16_t));
        EXPECT_TRUE(this->set(spi_set_highcontrast, too_long));
        EXPECT_FALSE(this->set(spi_set_highcontrast, too_long, spif_update_ini_file));
        this->emu.memory.write_memory(this->out, EMU_HIGHCONTRAST{.cbSize = 16, .dwFlags = 0, .lpszDefaultScheme = scheme_buffer});
        ASSERT_TRUE(this->query(spi_get_highcontrast, 0, this->out));
        EXPECT_EQ(0x1u, this->emu.memory.read_memory<EMU_HIGHCONTRAST>(this->out).dwFlags);

        struct malformed_scheme
        {
            uint64_t buffer;
            USHORT length;
            USHORT maximum_length;
        };

        for (const auto& [buffer, length, maximum_length] :
             {malformed_scheme{text + 1, 2, 4}, malformed_scheme{text, 4, 2}, malformed_scheme{text, 3, 4},
              malformed_scheme{this->emu.highest_user_address() - 1, 0, 0}})
        {
            auto malformed = request;
            malformed.Buffer = buffer;
            malformed.Length = length;
            malformed.MaximumLength = maximum_length;
            this->clear_output(last_error_sentinel);
            EXPECT_FALSE(this->set(spi_set_highcontrast, malformed)) << buffer << " " << length;
            EXPECT_EQ(error_noaccess, this->last_error()) << buffer << " " << length;
        }
    }

    TEST_F(SystemParametersInfoAccessibilityTest, SnapshotsKeepTheSettings)
    {
        constexpr std::u16string_view scheme = u"High Contrast White";
        const auto text = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        this->emu.memory.write_memory(text, scheme.data(), scheme.size() * sizeof(char16_t));

        ASSERT_TRUE(this->set(spi_set_filterkeys, make_filter_keys(0x5C, 0, 700, 300, 200)));
        ASSERT_TRUE(this->set(spi_set_togglekeys, EMU_TOGGLEKEYS{.cbSize = 8, .dwFlags = 0x39}));
        ASSERT_TRUE(this->set(spi_set_stickykeys, EMU_STICKYKEYS{.cbSize = 8, .dwFlags = 0x1F9}));
        ASSERT_TRUE(this->set(spi_set_mousekeys, make_mouse_keys(mouse_keys_replace_numbers | 0x3D, 200, 2000, 5)));
        ASSERT_TRUE(this->set(spi_set_accesstimeout, EMU_ACCESSTIMEOUT{.cbSize = 12, .dwFlags = 1, .iTimeOutMSec = 120000}));
        ASSERT_TRUE(this->set(spi_set_soundsentry, make_sound_sentry(1, 2)));
        ASSERT_TRUE(this->set(spi_set_audiodescription, EMU_AUDIODESCRIPTION{.cbSize = 12, .Enabled = TRUE, .Locale = 0x409}));
        ASSERT_TRUE(this->set(spi_set_highcontrast,
                              internal_set_high_contrast{.cbSize = 16,
                                                         .dwFlags = 0x1,
                                                         .Length = static_cast<USHORT>(scheme.size() * sizeof(char16_t)),
                                                         .MaximumLength = static_cast<USHORT>(scheme.size() * sizeof(char16_t)),
                                                         .padding = 0,
                                                         .Buffer = text}));
        for (const uint32_t action : {spi_set_showsounds, spi_set_keyboardpref, spi_set_screenreader})
        {
            ASSERT_TRUE(this->query(action, 1, 0)) << action;
        }

        const auto scheme_buffer = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto reported = [&] {
            struct reported_get
            {
                uint32_t action;
                uint32_t param;
                uint32_t size;
            };

            std::vector<std::byte> bytes{};
            const auto append = [&](const uint64_t address, const size_t size) {
                const auto offset = bytes.size();
                bytes.resize(offset + size);
                this->emu.memory.read_memory(address, bytes.data() + offset, size);
            };

            for (const auto& [action, param, size] :
                 {reported_get{spi_get_filterkeys, 24, 24}, reported_get{spi_get_togglekeys, 8, 8}, reported_get{spi_get_mousekeys, 28, 28},
                  reported_get{spi_get_stickykeys, 8, 8}, reported_get{spi_get_accesstimeout, 12, 12},
                  reported_get{spi_get_soundsentry, 56, 56}, reported_get{spi_get_audiodescription, 12, 12},
                  reported_get{spi_get_showsounds, 0, 4}, reported_get{spi_get_keyboardpref, 0, 4},
                  reported_get{spi_get_screenreader, 0, 4}})
            {
                this->emu.memory.set_memory(this->out, 0, size);
                this->emu.memory.write_memory(this->out, size);
                EXPECT_TRUE(this->query(action, param, this->out)) << action;
                append(this->out, size);
            }

            this->emu.memory.write_memory(this->out, EMU_HIGHCONTRAST{.cbSize = 16, .dwFlags = 0, .lpszDefaultScheme = scheme_buffer});
            EXPECT_TRUE(this->query(spi_get_highcontrast, 0, this->out));
            append(this->out, sizeof(EMU_HIGHCONTRAST));
            append(scheme_buffer, high_contrast_scheme_size);
            return bytes;
        };

        const auto before = reported();
        utils::buffer_serializer serializer{};
        this->emu.process.accessibility.serialize(serializer);

        this->emu.process.accessibility = {};
        ASSERT_NE(before, reported());

        utils::buffer_deserializer deserializer{serializer.get_buffer()};
        this->emu.process.accessibility.deserialize(deserializer);
        EXPECT_EQ(before, reported());
    }
}

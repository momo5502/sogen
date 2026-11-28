#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    uint64_t handle_NtGdiCreatePen(const syscall_context& c, uint32_t style, uint32_t width, uint32_t color);
    uint32_t handle_NtGdiExtGetObjectW(const syscall_context& c, uint32_t handle_value, uint32_t size, emulator_pointer buffer);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_stock_black_brush = 4;
        constexpr uint32_t k_stock_null_brush = 5;
        constexpr uint32_t k_stock_white_pen = 6;
        constexpr uint32_t k_stock_black_pen = 7;
        constexpr uint32_t k_stock_null_pen = 8;
        constexpr uint32_t k_stock_dc_brush = 18;
        constexpr uint32_t k_stock_dc_pen = 19;

        constexpr uint32_t k_ps_solid = 0;
        constexpr uint32_t k_ps_dash = 1;
        constexpr uint32_t k_ps_dot = 2;
        constexpr uint32_t k_ps_dashdot = 3;
        constexpr uint32_t k_ps_dashdotdot = 4;
        constexpr uint32_t k_ps_null = 5;
        constexpr uint32_t k_ps_insideframe = 6;

        constexpr uint8_t k_untouched = 0xCC;
        constexpr ULONG k_untouched_error = 0xDEAD;

        struct logbrush
        {
            uint32_t style{};
            uint32_t color{};
            uint64_t hatch{};

            bool operator==(const logbrush&) const = default;
        };

        struct logpen
        {
            uint32_t style{};
            int32_t width_x{};
            int32_t width_y{};
            uint32_t color{};

            bool operator==(const logpen&) const = default;
        };

        struct stock_brush_case
        {
            uint32_t index;
            logbrush expected;
        };

        class GdiGetObjectTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                if (this->init_gdi)
                {
                    syscalls::handle_NtGdiInit(this->context());
                }
                this->buffer = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, StockObjects) +
                                                                                    (index * sizeof(uint64_t))));
            }

            void reset_error()
            {
                this->emu.vcpu(0).active_thread->teb64->field<ULONG>(offsetof(TEB64, LastErrorValue)).write(k_untouched_error);
            }

            ULONG last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            uint32_t create_brush(const uint32_t color)
            {
                return static_cast<uint32_t>(syscalls::handle_NtGdiCreateSolidBrush(this->context(), color, 0));
            }

            uint32_t create_pen(const uint32_t style, const uint32_t width, const uint32_t color)
            {
                return static_cast<uint32_t>(syscalls::handle_NtGdiCreatePen(this->context(), style, width, color));
            }

            std::vector<uint8_t> get_object(const uint32_t object, const uint32_t size, uint32_t& returned, const uint64_t offset = 0)
            {
                std::vector<uint8_t> filled(0x40, k_untouched);
                this->emu.memory.write_memory(this->buffer, filled.data(), filled.size());
                returned = syscalls::handle_NtGdiExtGetObjectW(this->context(), object, size, this->buffer + offset);
                this->emu.memory.read_memory(this->buffer, filled.data(), filled.size());
                return filled;
            }

            static size_t written(const std::vector<uint8_t>& bytes)
            {
                size_t count = 0;
                while (count < bytes.size() && bytes[count] != k_untouched)
                {
                    ++count;
                }
                return count;
            }

            logbrush brush_of(const uint32_t object)
            {
                uint32_t returned = 0;
                const auto bytes = this->get_object(object, 16, returned);
                EXPECT_EQ(16u, returned);
                logbrush brush{};
                std::memcpy(&brush, bytes.data(), sizeof(brush));
                return brush;
            }

            logpen pen_of(const uint32_t object)
            {
                uint32_t returned = 0;
                const auto bytes = this->get_object(object, 16, returned);
                EXPECT_EQ(16u, returned);
                logpen pen{};
                std::memcpy(&pen, bytes.data(), sizeof(pen));
                return pen;
            }

            windows_emulator emu = create_sample_emulator();
            emulator_pointer buffer{};
            bool init_gdi = true;
        };

        class GdiGetObjectBeforeInitTest : public GdiGetObjectTest
        {
          public:
            GdiGetObjectBeforeInitTest()
            {
                this->init_gdi = false;
            }
        };
    }

    TEST_F(GdiGetObjectTest, StockAndCreatedObjectsReportTheirContents)
    {
        const std::vector<stock_brush_case> stock_brushes{
            {.index = 0, .expected = {.style = 0, .color = 0x00FFFFFF, .hatch = 0}},
            {.index = 1, .expected = {.style = 0, .color = 0x00C0C0C0, .hatch = 0}},
            {.index = 2, .expected = {.style = 0, .color = 0x00808080, .hatch = 0}},
            {.index = 3, .expected = {.style = 0, .color = 0x00404040, .hatch = 0}},
            {.index = k_stock_black_brush, .expected = {.style = 0, .color = 0x00000000, .hatch = 0}},
            {.index = k_stock_null_brush, .expected = {.style = 1, .color = 0x00000000, .hatch = 0}},
            {.index = k_stock_dc_brush, .expected = {.style = 0, .color = 0x00FFFFFF, .hatch = 0}},
        };

        for (const auto& test_case : stock_brushes)
        {
            SCOPED_TRACE(test_case.index);
            EXPECT_EQ(test_case.expected, this->brush_of(this->stock_object(test_case.index)));
        }

        for (const uint32_t color : {0x00000000u, 0x00FFFFFFu, 0x00CCBBAAu, 0x01000005u, 0x02AABBCCu, 0xFF123456u})
        {
            SCOPED_TRACE(color);
            EXPECT_EQ((logbrush{.style = 0, .color = color, .hatch = 0}), this->brush_of(this->create_brush(color)));
        }

        EXPECT_EQ((logpen{.style = k_ps_solid, .width_x = 0, .width_y = 0, .color = 0x00FFFFFF}),
                  this->pen_of(this->stock_object(k_stock_white_pen)));
        EXPECT_EQ((logpen{.style = k_ps_solid, .width_x = 0, .width_y = 0, .color = 0}),
                  this->pen_of(this->stock_object(k_stock_black_pen)));
        EXPECT_EQ((logpen{.style = k_ps_null, .width_x = 1, .width_y = 0, .color = 0}), this->pen_of(this->stock_object(k_stock_null_pen)));
        EXPECT_EQ((logpen{.style = k_ps_solid, .width_x = 0, .width_y = 0, .color = 0}), this->pen_of(this->stock_object(k_stock_dc_pen)));

        constexpr uint32_t color = 0x00332211;
        for (const uint32_t style : {k_ps_solid, k_ps_dash, k_ps_dot, k_ps_dashdot, k_ps_dashdotdot, k_ps_insideframe})
        {
            for (const uint32_t width : {0u, 1u, 5u})
            {
                SCOPED_TRACE(style * 100 + width);
                EXPECT_EQ((logpen{.style = style, .width_x = static_cast<int32_t>(width), .width_y = 0, .color = color}),
                          this->pen_of(this->create_pen(style, width, color)));
            }
        }

        EXPECT_EQ((logpen{.style = k_ps_solid, .width_x = 3, .width_y = 0, .color = color}),
                  this->pen_of(this->create_pen(k_ps_solid, static_cast<uint32_t>(-3), color)));

        for (const uint32_t width : {0u, 1u, 5u, 7u})
        {
            SCOPED_TRACE(width);
            const auto null_pen = this->create_pen(k_ps_null, width, 0x000000FF);
            EXPECT_EQ(this->stock_object(k_stock_null_pen), null_pen);
            EXPECT_EQ((logpen{.style = k_ps_null, .width_x = 1, .width_y = 0, .color = 0}), this->pen_of(null_pen));
        }
    }

    TEST_F(GdiGetObjectBeforeInitTest, NullPenCreatedBeforeNtGdiInitIsTheStockNullPen)
    {
        const auto null_pen = this->create_pen(k_ps_null, 7, 0x000000FF);

        EXPECT_NE(0u, null_pen);
        EXPECT_EQ(this->stock_object(k_stock_null_pen), null_pen);
        EXPECT_EQ((logpen{.style = k_ps_null, .width_x = 1, .width_y = 0, .color = 0}), this->pen_of(null_pen));
    }

    TEST_F(GdiGetObjectTest, BrushSizeSweepMatchesTheNativeCapture)
    {
        const auto brush = this->stock_object(0);
        uint32_t returned = 0;

        EXPECT_EQ(16u, syscalls::handle_NtGdiExtGetObjectW(this->context(), brush, 0, 0));

        this->reset_error();
        const auto empty = this->get_object(brush, 0, returned);
        EXPECT_EQ(0u, returned);
        EXPECT_EQ(0u, written(empty));
        EXPECT_EQ(k_untouched_error, this->last_error());

        for (const uint32_t size : {1u, 15u})
        {
            SCOPED_TRACE(size);
            const auto bytes = this->get_object(brush, size, returned);
            EXPECT_EQ(16u, returned);
            EXPECT_EQ(size, written(bytes));
        }

        for (const uint32_t size : {16u, 24u})
        {
            SCOPED_TRACE(size);
            const auto bytes = this->get_object(brush, size, returned);
            EXPECT_EQ(16u, returned);
            EXPECT_EQ(16u, written(bytes));
        }

        const auto odd = this->get_object(brush, 16, returned, 1);
        EXPECT_EQ(0u, returned);
        EXPECT_TRUE(std::all_of(odd.begin(), odd.end(), [](const uint8_t byte) { return byte == k_untouched; }));
    }

    TEST_F(GdiGetObjectTest, PenSizeSweepMatchesTheNativeCapture)
    {
        const auto pen = this->stock_object(k_stock_black_pen);
        uint32_t returned = 0;

        EXPECT_EQ(16u, syscalls::handle_NtGdiExtGetObjectW(this->context(), pen, 0, 0));

        for (const uint32_t size : {0u, 1u, 15u})
        {
            SCOPED_TRACE(size);
            this->reset_error();
            const auto bytes = this->get_object(pen, size, returned);
            EXPECT_EQ(0u, returned);
            EXPECT_EQ(0u, written(bytes));
            EXPECT_EQ(k_untouched_error, this->last_error());
        }

        for (const uint32_t size : {16u, 24u, 32u})
        {
            SCOPED_TRACE(size);
            const auto bytes = this->get_object(pen, size, returned);
            EXPECT_EQ(16u, returned);
            EXPECT_EQ(16u, written(bytes));
        }

        const auto odd = this->get_object(pen, 16, returned, 1);
        EXPECT_EQ(0u, returned);
        EXPECT_TRUE(std::all_of(odd.begin(), odd.end(), [](const uint8_t byte) { return byte == k_untouched; }));

        const auto null_pen = this->stock_object(k_stock_null_pen);
        for (const uint32_t size : {16u, 24u, 31u, 33u, 40u})
        {
            SCOPED_TRACE(size);
            const auto logpen_bytes = this->get_object(null_pen, size, returned);
            EXPECT_EQ(16u, returned);
            EXPECT_EQ(16u, written(logpen_bytes));
        }

        const auto extended = this->get_object(null_pen, 32, returned);
        EXPECT_EQ(32u, returned);
        EXPECT_EQ(k_ps_null, static_cast<uint32_t>(extended[0]));
        EXPECT_EQ(32u, written(extended));
        EXPECT_TRUE(std::all_of(extended.begin() + 1, extended.begin() + 32, [](const uint8_t byte) { return byte == 0; }));
    }
}

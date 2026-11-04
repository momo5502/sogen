#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    uint64_t handle_NtGdiCreatePen(const syscall_context& c, uint32_t style, uint32_t width, uint32_t color);
    uint64_t handle_NtGdiSelectBrushLocal(const syscall_context& c, hdc dc, uint32_t brush, emulator_pointer old_brush_ptr);
    uint64_t handle_NtGdiSelectPenLocal(const syscall_context& c, hdc dc, uint32_t pen, emulator_pointer old_pen_ptr);
    uint64_t handle_NtGdiGetDCObject(const syscall_context& c, hdc dc, int32_t object_type);
    uint32_t handle_NtGdiExtGetObjectW(const syscall_context& c, uint32_t handle_value, uint32_t size, emulator_pointer buffer);
    BOOL handle_NtGdiPatBlt(const syscall_context& c, hdc dc, LONG x, LONG y, LONG width, LONG height, DWORD rop);
    BOOL handle_NtGdiLineTo(const syscall_context& c, hdc dc, LONG x_end, LONG y_end);
    BOOL handle_NtGdiBitBlt(const syscall_context& c, hdc dst_dc, int x_dst, int y_dst, int width, int height, hdc src_dc, int x_src,
                            int y_src, DWORD rop, DWORD cr_back_color, FLONG fl);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
}

namespace sogen::test
{
    namespace
    {
        constexpr int k_side = 4;
        constexpr COLORREF k_background = 0x777777;
        constexpr COLORREF k_red = 0x0000FF;
        constexpr COLORREF k_blue = 0xFF0000;
        constexpr uint32_t k_ps_solid = 0;
        constexpr DWORD k_patcopy = 0x00F00021;
        constexpr DWORD k_patinvert = 0x005A0049;
        constexpr DWORD k_dstinvert = 0x00550009;
        constexpr DWORD k_blackness = 0x00000042;
        constexpr DWORD k_whiteness = 0x00FF0062;
        constexpr int32_t k_dc_object_brush = 0x100000;
        constexpr int32_t k_dc_object_pen = 0x300000;
        constexpr uint32_t k_stock_white_brush = 0;
        constexpr uint32_t k_stock_null_brush = 5;
        constexpr uint32_t k_stock_white_pen = 6;
        constexpr uint32_t k_stock_black_pen = 7;
        constexpr uint32_t k_stock_null_pen = 8;
        constexpr uint32_t k_stock_dc_brush = 18;
        constexpr uint32_t k_stock_dc_pen = 19;
        constexpr uint64_t k_dc_attr_brush = 0xA0;
        constexpr uint64_t k_dc_attr_pen = 0xA8;
        constexpr uint64_t k_dc_attr_dc_brush_color = 0xC4;
        constexpr uint64_t k_dc_attr_dc_pen_color = 0xCC;
        constexpr uint64_t k_dc_attr_current_position = 0xD8;
        constexpr uint32_t k_logpen_size = 16;
        constexpr uint32_t k_logbrush_size = 12;

        // The expected values were captured on Windows 11 (build 26200) on a 4x4 32bpp memory DC filled with 0x777777;
        // the handle table entries in an x64 process, everything else in x64 and WoW64 processes alike. gdi32 selects a
        // brush or pen without a system call by storing it at DC_ATTR+0xA0 or +0xA8, and keeps the SetDCBrushColor and
        // SetDCPenColor colours at +0xC4 and +0xCC, which select_like_gdi32 and set_dc_color mirror.
        class GdiSelectionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                ASSERT_NE(0u, this->dc);
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), k_side, k_side, 1, 32, 0);
                ASSERT_NE(0u, bitmap);
                syscalls::handle_NtGdiSelectBitmap(this->context(), this->dc, make_handle(bitmap));
                this->fill_background();
            }

            void fill_background()
            {
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        syscalls::handle_NtGdiSetPixel(this->context(), this->dc, x, y, k_background);
                    }
                }
            }

            bool is_background()
            {
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        if (this->pixel(x, y) != k_background)
                        {
                            return false;
                        }
                    }
                }
                return true;
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            GDI_HANDLE_ENTRY64 entry(const uint32_t handle_value)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + ((handle_value & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64)));
            }

            uint64_t dc_attr()
            {
                return this->entry(static_cast<uint32_t>(this->dc)).Object;
            }

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(
                    this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, Objects) + (index * sizeof(uint64_t))));
            }

            void select_like_gdi32(const uint64_t attr_offset, const uint32_t object)
            {
                const uint64_t value = object;
                this->emu.memory.write_memory(this->dc_attr() + attr_offset, &value, sizeof(value));
            }

            void set_dc_color(const uint64_t attr_offset, const COLORREF color)
            {
                this->emu.memory.write_memory(this->dc_attr() + attr_offset, &color, sizeof(color));
            }

            COLORREF pat_blt_color(const DWORD rop = k_patcopy)
            {
                this->fill_background();
                EXPECT_EQ(TRUE, syscalls::handle_NtGdiPatBlt(this->context(), this->dc, 0, 0, k_side, k_side, rop));
                return this->pixel(1, 1);
            }

            COLORREF line_color()
            {
                this->fill_background();
                const std::array<int32_t, 2> start{0, 1};
                this->emu.memory.write_memory(this->dc_attr() + k_dc_attr_current_position, start.data(), sizeof(start));
                EXPECT_EQ(TRUE, syscalls::handle_NtGdiLineTo(this->context(), this->dc, k_side, 1));
                return this->pixel(1, 1);
            }

            uint32_t create_brush(const COLORREF color)
            {
                return static_cast<uint32_t>(syscalls::handle_NtGdiCreateSolidBrush(this->context(), color, 0));
            }

            uint32_t create_pen(const COLORREF color)
            {
                return static_cast<uint32_t>(syscalls::handle_NtGdiCreatePen(this->context(), k_ps_solid, 1, color));
            }

            COLORREF pixel(const int x, const int y)
            {
                return syscalls::handle_NtGdiGetPixel(this->context(), this->dc, x, y);
            }

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
        };
    }

    TEST_F(GdiSelectionTest, HandleTableEntriesHoldTheBaseObjectType)
    {
        const auto brush = this->create_brush(k_red);
        const auto pen = this->create_pen(k_blue);

        EXPECT_EQ(0x10, (brush >> 16) & 0x7F);
        EXPECT_EQ(0x10, this->entry(brush).Type);
        EXPECT_EQ(brush >> 16, this->entry(brush).Unique);

        EXPECT_EQ(0x30, (pen >> 16) & 0x7F);
        EXPECT_EQ(0x10, this->entry(pen).Type);
        EXPECT_EQ(pen >> 16, this->entry(pen).Unique);

        EXPECT_EQ(0x01, this->entry(static_cast<uint32_t>(this->dc)).Type);
    }

    TEST_F(GdiSelectionTest, ObjectSizesStillFollowTheFullType)
    {
        EXPECT_EQ(k_logpen_size, syscalls::handle_NtGdiExtGetObjectW(this->context(), this->create_pen(k_blue), 0, 0));
        EXPECT_EQ(k_logbrush_size, syscalls::handle_NtGdiExtGetObjectW(this->context(), this->create_brush(k_red), 0, 0));
    }

    TEST_F(GdiSelectionTest, StockBrushesAndPensUseTheirOwnSlots)
    {
        const std::array<uint32_t, 4> objects{this->stock_object(k_stock_white_pen), this->stock_object(k_stock_black_pen),
                                              this->stock_object(k_stock_null_pen), this->stock_object(k_stock_white_brush)};
        for (size_t i = 0; i < objects.size(); ++i)
        {
            ASSERT_NE(0u, objects[i]) << i;
            for (size_t j = 0; j < i; ++j)
            {
                EXPECT_NE(objects[j], objects[i]) << j << " " << i;
            }
        }
        EXPECT_EQ(0x30, (objects[0] >> 16) & 0x7F);
        EXPECT_EQ(0x30, (objects[1] >> 16) & 0x7F);
        EXPECT_EQ(0x30, (objects[2] >> 16) & 0x7F);

        const auto null_brush = this->stock_object(k_stock_null_brush);
        ASSERT_NE(0u, null_brush);
        EXPECT_EQ(0x10, (null_brush >> 16) & 0x7F);
        EXPECT_NE(this->stock_object(k_stock_white_brush), null_brush);
    }

    TEST_F(GdiSelectionTest, FreshDcStartsWithTheWhiteBrushAndTheBlackPen)
    {
        EXPECT_EQ(this->stock_object(k_stock_white_brush), syscalls::handle_NtGdiGetDCObject(this->context(), this->dc, k_dc_object_brush));
        EXPECT_EQ(this->stock_object(k_stock_black_pen), syscalls::handle_NtGdiGetDCObject(this->context(), this->dc, k_dc_object_pen));
    }

    TEST_F(GdiSelectionTest, SelectingReturnsThePreviousObject)
    {
        const auto red = this->create_brush(k_red);
        const auto blue = this->create_pen(k_blue);

        EXPECT_EQ(this->stock_object(k_stock_white_brush), syscalls::handle_NtGdiSelectBrushLocal(this->context(), this->dc, red, 0));
        EXPECT_EQ(red, syscalls::handle_NtGdiGetDCObject(this->context(), this->dc, k_dc_object_brush));

        EXPECT_EQ(this->stock_object(k_stock_black_pen), syscalls::handle_NtGdiSelectPenLocal(this->context(), this->dc, blue, 0));
        EXPECT_EQ(blue, syscalls::handle_NtGdiGetDCObject(this->context(), this->dc, k_dc_object_pen));
    }

    TEST_F(GdiSelectionTest, PatBltUsesTheBrushGdi32Selected)
    {
        this->select_like_gdi32(k_dc_attr_brush, this->create_brush(k_red));

        EXPECT_EQ(TRUE, syscalls::handle_NtGdiPatBlt(this->context(), this->dc, 0, 0, k_side, 1, k_patcopy));

        EXPECT_EQ(k_red, this->pixel(1, 0));
        EXPECT_EQ(k_background, this->pixel(1, 1));
    }

    TEST_F(GdiSelectionTest, LineToUsesThePenGdi32Selected)
    {
        this->select_like_gdi32(k_dc_attr_pen, this->create_pen(k_blue));
        const std::array<int32_t, 2> start{0, 2};
        this->emu.memory.write_memory(this->dc_attr() + k_dc_attr_current_position, start.data(), sizeof(start));

        EXPECT_EQ(TRUE, syscalls::handle_NtGdiLineTo(this->context(), this->dc, k_side, 2));

        EXPECT_EQ(k_blue, this->pixel(1, 2));
        EXPECT_EQ(k_background, this->pixel(1, 1));
    }

    TEST_F(GdiSelectionTest, LineToWithTheNullPenDrawsNothing)
    {
        this->select_like_gdi32(k_dc_attr_pen, this->stock_object(k_stock_null_pen));
        const std::array<int32_t, 2> start{0, 2};
        this->emu.memory.write_memory(this->dc_attr() + k_dc_attr_current_position, start.data(), sizeof(start));

        EXPECT_EQ(TRUE, syscalls::handle_NtGdiLineTo(this->context(), this->dc, k_side, 2));

        EXPECT_EQ(k_background, this->pixel(1, 2));
    }

    TEST_F(GdiSelectionTest, StockBrushesPaintTheirColours)
    {
        constexpr std::array<COLORREF, 5> colours{0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
        for (uint32_t index = 0; index < colours.size(); ++index)
        {
            this->select_like_gdi32(k_dc_attr_brush, this->stock_object(index));
            EXPECT_EQ(colours[index], this->pat_blt_color()) << index;
        }
    }

    TEST_F(GdiSelectionTest, NullBrushPaintsNothing)
    {
        this->select_like_gdi32(k_dc_attr_brush, this->stock_object(k_stock_null_brush));

        for (const auto rop : {k_patcopy, k_patinvert})
        {
            EXPECT_EQ(TRUE, syscalls::handle_NtGdiPatBlt(this->context(), this->dc, 0, 0, k_side, k_side, rop));
            EXPECT_TRUE(this->is_background());
            EXPECT_EQ(TRUE, syscalls::handle_NtGdiBitBlt(this->context(), this->dc, 0, 0, k_side, k_side, 0, 0, 0, rop, 0, 0));
            EXPECT_TRUE(this->is_background());
        }
    }

    TEST_F(GdiSelectionTest, DcBrushStartsWhiteAndUsesTheDcBrushColour)
    {
        this->select_like_gdi32(k_dc_attr_brush, this->stock_object(k_stock_dc_brush));
        EXPECT_EQ(0xFFFFFFu, this->pat_blt_color());

        this->set_dc_color(k_dc_attr_dc_brush_color, k_red);
        EXPECT_EQ(k_red, this->pat_blt_color());
    }

    TEST_F(GdiSelectionTest, DcBrushColourOnlyAppliesWhileTheDcBrushIsSelected)
    {
        constexpr COLORREF green = 0x00FF00;
        this->set_dc_color(k_dc_attr_dc_brush_color, green);
        EXPECT_EQ(0xFFFFFFu, this->pat_blt_color());

        this->select_like_gdi32(k_dc_attr_brush, this->stock_object(k_stock_dc_brush));
        EXPECT_EQ(green, this->pat_blt_color());
    }

    TEST_F(GdiSelectionTest, DcPenStartsBlackAndUsesTheDcPenColour)
    {
        this->select_like_gdi32(k_dc_attr_pen, this->stock_object(k_stock_dc_pen));
        EXPECT_EQ(0x000000u, this->line_color());

        this->set_dc_color(k_dc_attr_dc_pen_color, k_blue);
        EXPECT_EQ(k_blue, this->line_color());
    }

    TEST_F(GdiSelectionTest, StockPensDrawTheirColours)
    {
        this->select_like_gdi32(k_dc_attr_pen, this->stock_object(k_stock_white_pen));
        EXPECT_EQ(0xFFFFFFu, this->line_color());

        this->select_like_gdi32(k_dc_attr_pen, this->stock_object(k_stock_black_pen));
        EXPECT_EQ(0x000000u, this->line_color());
    }

    TEST_F(GdiSelectionTest, PatBltRasterOperationsWithoutThePatternIgnoreTheBrush)
    {
        for (const auto brush : {this->stock_object(k_stock_null_brush), this->create_brush(k_red)})
        {
            this->select_like_gdi32(k_dc_attr_brush, brush);
            EXPECT_EQ(0xFFFFFFu, this->pat_blt_color(k_whiteness));
            EXPECT_EQ(0x000000u, this->pat_blt_color(k_blackness));
            EXPECT_EQ(0x888888u, this->pat_blt_color(k_dstinvert));
        }
    }

    TEST_F(GdiSelectionTest, PatBltCombinesThePatternWithTheDestination)
    {
        this->select_like_gdi32(k_dc_attr_brush, this->create_brush(k_red));
        EXPECT_EQ(0x777788u, this->pat_blt_color(k_patinvert));
    }
}

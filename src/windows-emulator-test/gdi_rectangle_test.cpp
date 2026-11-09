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
    BOOL handle_NtGdiRectangle(const syscall_context& c, hdc dc, LONG left, LONG top, LONG right, LONG bottom);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
}

namespace sogen::test
{
    namespace
    {
        constexpr int k_side = 16;
        constexpr COLORREF k_background = 0x777777;
        constexpr COLORREF k_red = 0x0000FF;
        constexpr COLORREF k_blue = 0xFF0000;
        constexpr uint32_t k_stock_null_brush = 5;
        constexpr uint32_t k_stock_null_pen = 8;
        constexpr uint64_t k_dc_attr_rop2 = 0xD4;
        constexpr uint8_t k_r2_black = 1;
        constexpr uint8_t k_r2_notcopypen = 4;
        constexpr uint8_t k_r2_not = 6;
        constexpr uint8_t k_r2_xorpen = 7;
        constexpr uint8_t k_r2_maskpen = 9;
        constexpr uint8_t k_r2_nop = 11;
        constexpr uint8_t k_r2_mergepen = 15;
        constexpr uint8_t k_r2_white = 16;
        constexpr uint32_t k_ps_solid = 0;

        using grid = std::array<std::string_view, k_side>;
        using rect = std::array<LONG, 4>;

        // Every expected grid and colour in this file reproduces a Windows 11 (build 26200) capture, x64 and WoW64
        // identical: Rectangle on a 16x16 32bpp memory DC filled with 0x777777, drawn with DC_BRUSH red and DC_PEN
        // blue (the test selects a solid brush and pen of those colours). The 1-wide shape was captured on a 12x12
        // DC. '.' is the background, 'R' red, 'B' blue.
        constexpr grid k_filled = {
            "................", "................", "................", "...BBBBBBBBB....", "...BRRRRRRRB....", "...BRRRRRRRB....",
            "...BRRRRRRRB....", "...BRRRRRRRB....", "...BRRRRRRRB....", "...BRRRRRRRB....", "...BBBBBBBBB....", "................",
            "................", "................", "................", "................",
        };

        class GdiRectangleTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
            }

            void reset_canvas()
            {
                this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                ASSERT_NE(0u, this->dc);
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), k_side, k_side, 1, 32, 0);
                ASSERT_NE(0u, bitmap);
                syscalls::handle_NtGdiSelectBitmap(this->context(), this->dc, make_handle(bitmap));

                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        syscalls::handle_NtGdiSetPixel(this->context(), this->dc, x, y, k_background);
                    }
                }

                this->select_brush(static_cast<uint32_t>(syscalls::handle_NtGdiCreateSolidBrush(this->context(), k_red, 0)));
                this->select_pen(static_cast<uint32_t>(syscalls::handle_NtGdiCreatePen(this->context(), k_ps_solid, 1, k_blue)));
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            uint64_t dc_attr()
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                const auto index = static_cast<uint32_t>(this->dc) & 0xFFFF;
                return this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + (index * sizeof(GDI_HANDLE_ENTRY64))).Object;
            }

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, StockObjects) +
                                                                                    (index * sizeof(uint64_t))));
            }

            void select_brush(const uint32_t brush)
            {
                ASSERT_NE(0u, brush);
                syscalls::handle_NtGdiSelectBrushLocal(this->context(), this->dc, brush, 0);
            }

            void select_pen(const uint32_t pen)
            {
                ASSERT_NE(0u, pen);
                syscalls::handle_NtGdiSelectPenLocal(this->context(), this->dc, pen, 0);
            }

            void set_rop2(const uint8_t rop2)
            {
                this->emu.memory.write_memory(this->dc_attr() + k_dc_attr_rop2, &rop2, sizeof(rop2));
            }

            BOOL rectangle(const LONG left, const LONG top, const LONG right, const LONG bottom)
            {
                return syscalls::handle_NtGdiRectangle(this->context(), this->dc, left, top, right, bottom);
            }

            COLORREF pixel(const int x, const int y)
            {
                return syscalls::handle_NtGdiGetPixel(this->context(), this->dc, x, y);
            }

            void expect_grid(const grid& expected)
            {
                for (int y = 0; y < k_side; ++y)
                {
                    std::string row(k_side, '?');
                    for (int x = 0; x < k_side; ++x)
                    {
                        const auto color = this->pixel(x, y);
                        row[x] = color == k_background ? '.' : color == k_red ? 'R' : color == k_blue ? 'B' : '?';
                    }
                    EXPECT_EQ(expected[y], row) << "row " << y;
                }
            }

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
        };
    }

    TEST_F(GdiRectangleTest, MatchesTheWindowsCaptureForEachShape)
    {
        struct shape_case
        {
            std::string_view name{};
            std::optional<uint32_t> stock_brush{};
            std::optional<uint32_t> stock_pen{};
            std::vector<rect> rects{};
            grid expected{};
        };

        const std::array<shape_case, 9> cases{{
            {.name = "filled", .rects = {{3, 3, 12, 11}}, .expected = k_filled},
            {.name = "inverted", .rects = {{12, 11, 3, 3}}, .expected = k_filled},
            {
                .name = "null pen",
                .stock_pen = k_stock_null_pen,
                .rects = {{3, 3, 12, 11}},
                .expected =
                    {
                        "................",
                        "................",
                        "................",
                        "...RRRRRRRR.....",
                        "...RRRRRRRR.....",
                        "...RRRRRRRR.....",
                        "...RRRRRRRR.....",
                        "...RRRRRRRR.....",
                        "...RRRRRRRR.....",
                        "...RRRRRRRR.....",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                    },
            },
            {
                .name = "null brush",
                .stock_brush = k_stock_null_brush,
                .rects = {{3, 3, 12, 11}},
                .expected =
                    {
                        "................",
                        "................",
                        "................",
                        "...BBBBBBBBB....",
                        "...B.......B....",
                        "...B.......B....",
                        "...B.......B....",
                        "...B.......B....",
                        "...B.......B....",
                        "...B.......B....",
                        "...BBBBBBBBB....",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                    },
            },
            {
                .name = "empty",
                .rects = {{3, 3, 3, 11}, {3, 3, 12, 3}, {3, 3, 4, 4}},
                .expected =
                    {
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                    },
            },
            {
                .name = "2x2",
                .rects = {{3, 3, 5, 5}},
                .expected =
                    {
                        "................",
                        "................",
                        "................",
                        "...BB...........",
                        "...BB...........",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                    },
            },
            {
                .name = "3x3",
                .rects = {{3, 3, 6, 6}},
                .expected =
                    {
                        "................",
                        "................",
                        "................",
                        "...BBB..........",
                        "...BRB..........",
                        "...BBB..........",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                    },
            },
            {
                .name = "1-wide",
                .rects = {{2, 2, 3, 9}},
                .expected =
                    {
                        "................",
                        "................",
                        "..B.............",
                        "..B.............",
                        "..B.............",
                        "..B.............",
                        "..B.............",
                        "..B.............",
                        "..B.............",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                    },
            },
            {
                .name = "clipped",
                .rects = {{-2, -2, 5, 5}, {10, 10, 20, 20}},
                .expected =
                    {
                        "RRRRB...........",
                        "RRRRB...........",
                        "RRRRB...........",
                        "RRRRB...........",
                        "BBBBB...........",
                        "................",
                        "................",
                        "................",
                        "................",
                        "................",
                        "..........BBBBBB",
                        "..........BRRRRR",
                        "..........BRRRRR",
                        "..........BRRRRR",
                        "..........BRRRRR",
                        "..........BRRRRR",
                    },
            },
        }};

        for (const auto& entry : cases)
        {
            SCOPED_TRACE(entry.name);
            this->reset_canvas();
            if (entry.stock_brush)
            {
                this->select_brush(this->stock_object(*entry.stock_brush));
            }
            if (entry.stock_pen)
            {
                this->select_pen(this->stock_object(*entry.stock_pen));
            }
            for (const auto& [left, top, right, bottom] : entry.rects)
            {
                EXPECT_EQ(TRUE, this->rectangle(left, top, right, bottom));
            }
            this->expect_grid(entry.expected);
        }
    }

    TEST_F(GdiRectangleTest, AppliesTheRop2ModeToTheFillAndTheOutline)
    {
        struct rop2_case
        {
            uint8_t rop2;
            COLORREF outline;
            COLORREF fill;
        };

        constexpr std::array<rop2_case, 8> cases{{
            {k_r2_xorpen, 0x887777, 0x777788},
            {k_r2_not, 0x888888, 0x888888},
            {k_r2_nop, k_background, k_background},
            {k_r2_maskpen, 0x770000, 0x000077},
            {k_r2_mergepen, 0xFF7777, 0x7777FF},
            {k_r2_black, 0x000000, 0x000000},
            {k_r2_white, 0xFFFFFF, 0xFFFFFF},
            {k_r2_notcopypen, 0x00FFFF, 0xFFFF00},
        }};

        for (const auto& entry : cases)
        {
            this->reset_canvas();
            this->set_rop2(entry.rop2);
            EXPECT_EQ(TRUE, this->rectangle(3, 3, 12, 11));
            EXPECT_EQ(entry.outline, this->pixel(3, 3)) << "rop2 " << static_cast<int>(entry.rop2);
            EXPECT_EQ(entry.outline, this->pixel(11, 3)) << "rop2 " << static_cast<int>(entry.rop2);
            EXPECT_EQ(entry.outline, this->pixel(3, 7)) << "rop2 " << static_cast<int>(entry.rop2);
            EXPECT_EQ(entry.outline, this->pixel(11, 10)) << "rop2 " << static_cast<int>(entry.rop2);
            EXPECT_EQ(entry.fill, this->pixel(4, 4)) << "rop2 " << static_cast<int>(entry.rop2);
            EXPECT_EQ(entry.fill, this->pixel(10, 9)) << "rop2 " << static_cast<int>(entry.rop2);
            EXPECT_EQ(k_background, this->pixel(12, 5)) << "rop2 " << static_cast<int>(entry.rop2);
        }
    }
}

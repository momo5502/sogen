#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    uint64_t handle_NtGdiCreatePen(const syscall_context& c, uint32_t style, uint32_t width, uint32_t color);
    uint64_t handle_NtGdiSelectBrushLocal(const syscall_context& c, hdc dc, uint32_t brush, emulator_pointer old_brush_ptr);
    BOOL handle_NtGdiPatBlt(const syscall_context& c, hdc dc, LONG x, LONG y, LONG width, LONG height, DWORD rop);
    BOOL handle_NtGdiPolyPatBlt(const syscall_context& c, hdc dc, DWORD rop, emulator_pointer poly, DWORD count, DWORD mode);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
}

namespace sogen::test
{
    namespace
    {
        constexpr int k_side = 16;
        constexpr COLORREF k_canvas = 0x00332211;
        constexpr COLORREF k_brush = 0x00CCBBAA;
        constexpr RECT k_rect = {.left = 2, .top = 3, .right = 8, .bottom = 8};
        constexpr int k_painted_pixels = 30;
        constexpr DWORD k_ppb_brush = 0;
        constexpr uint64_t k_entry_size = 0x18;
        constexpr DWORD k_patcopy = 0x00F00021;
        constexpr DWORD k_untouched_last_error = 0xDEAD;
        constexpr DWORD k_error_invalid_handle = 6;
        constexpr DWORD k_max_entries = 0x2000;
        constexpr DWORD k_max_count = 0xAAAAAAA;
        constexpr uint32_t k_stock_null_brush = 5;
        constexpr uint32_t k_stock_null_pen = 8;
        constexpr uint32_t k_stock_dc_brush = 18;
        constexpr uint32_t k_stock_dc_pen = 19;
        constexpr uint64_t k_dc_attr_brush = 0xA0;
        constexpr uint64_t k_dc_attr_dc_brush_color = 0xC4;
        constexpr uint64_t k_dc_attr_dc_pen_color = 0xCC;
        constexpr COLORREF k_dc_brush_color = 0x00302010;
        constexpr COLORREF k_dc_pen_color = 0x00504030;
        constexpr COLORREF k_pen_color = 0x000000FF;
        constexpr DWORD k_dstinvert = 0x00550009;

        struct rop_case
        {
            const char* name;
            DWORD rop;
            BOOL result;
            std::optional<COLORREF> painted;
        };

        constexpr std::array<rop_case, 13> k_rop_cases = {
            rop_case{.name = "PATCOPY", .rop = 0x00F00021, .result = TRUE, .painted = 0x00CCBBAA},
            rop_case{.name = "PATINVERT", .rop = 0x005A0049, .result = TRUE, .painted = 0x00FF99BB},
            rop_case{.name = "DSTINVERT", .rop = 0x00550009, .result = TRUE, .painted = 0x00CCDDEE},
            rop_case{.name = "BLACKNESS", .rop = 0x00000042, .result = TRUE, .painted = 0x00000000},
            rop_case{.name = "WHITENESS", .rop = 0x00FF0062, .result = TRUE, .painted = 0x00FFFFFF},
            rop_case{.name = "PATPAINT", .rop = 0x00FB0A09, .result = FALSE, .painted = std::nullopt},
            rop_case{.name = "DPa", .rop = 0x00A000C9, .result = TRUE, .painted = 0x00002200},
            rop_case{.name = "Pn", .rop = 0x000F0001, .result = TRUE, .painted = 0x00334455},
            rop_case{.name = "DPo", .rop = 0x00FA0089, .result = TRUE, .painted = 0x00FFBBBB},
            rop_case{.name = "NOP", .rop = 0x00AA0029, .result = TRUE, .painted = std::nullopt},
            rop_case{.name = "SRCCOPY", .rop = 0x00CC0020, .result = FALSE, .painted = std::nullopt},
            rop_case{.name = "MERGECOPY", .rop = 0x00C000CA, .result = FALSE, .painted = std::nullopt},
            rop_case{.name = "high bits set", .rop = 0xFF00F021, .result = TRUE, .painted = 0x00000000},
        };

        enum class brush_field
        {
            zero,
            invalid_handle,
            colorref,
            pen_handle,
            stock_null,
            stock_dc_brush,
            stock_null_pen,
            stock_dc_pen,
        };

        struct brush_field_case
        {
            const char* name;
            brush_field field;
            DWORD rop;
            std::optional<COLORREF> painted;
        };

        constexpr std::array<brush_field_case, 10> k_brush_field_cases = {
            brush_field_case{.name = "zero field", .field = brush_field::zero, .rop = k_patcopy, .painted = k_brush},
            brush_field_case{.name = "invalid handle", .field = brush_field::invalid_handle, .rop = k_patcopy, .painted = k_brush},
            brush_field_case{.name = "colorref", .field = brush_field::colorref, .rop = k_patcopy, .painted = k_brush},
            brush_field_case{.name = "pen handle", .field = brush_field::pen_handle, .rop = k_patcopy, .painted = k_pen_color},
            brush_field_case{.name = "stock null PATCOPY", .field = brush_field::stock_null, .rop = k_patcopy, .painted = std::nullopt},
            brush_field_case{.name = "stock null DSTINVERT", .field = brush_field::stock_null, .rop = k_dstinvert, .painted = 0x00CCDDEE},
            brush_field_case{.name = "stock DC brush", .field = brush_field::stock_dc_brush, .rop = k_patcopy, .painted = k_dc_brush_color},
            brush_field_case{
                .name = "stock null pen PATCOPY", .field = brush_field::stock_null_pen, .rop = k_patcopy, .painted = std::nullopt},
            brush_field_case{
                .name = "stock null pen DSTINVERT", .field = brush_field::stock_null_pen, .rop = k_dstinvert, .painted = 0x00CCDDEE},
            brush_field_case{.name = "stock DC pen", .field = brush_field::stock_dc_pen, .rop = k_patcopy, .painted = k_dc_pen_color},
        };

        enum class geometry_call
        {
            poly_pat_blt,
            pat_blt,
        };

        struct geometry_case
        {
            const char* name;
            geometry_call call;
            const char* capture_row;
            std::array<int32_t, 4> rect;
            RECT painted_bounds;
            int painted_pixels;
        };

        constexpr std::array<geometry_case, 4> k_geometry_cases = {
            geometry_case{.name = "negative width",
                          .call = geometry_call::pat_blt,
                          .capture_row = "q08 nongui.patblt.solid.PATCOPY_negw",
                          .rect = {8, 8, -4, 3},
                          .painted_bounds = {.left = 4, .top = 8, .right = 8, .bottom = 11},
                          .painted_pixels = 12},
            geometry_case{.name = "negative height",
                          .call = geometry_call::pat_blt,
                          .capture_row = "q08 nongui.patblt.solid.PATCOPY_negh",
                          .rect = {8, 8, 4, -3},
                          .painted_bounds = {.left = 8, .top = 5, .right = 12, .bottom = 8},
                          .painted_pixels = 12},
            geometry_case{.name = "negative width and height",
                          .call = geometry_call::poly_pat_blt,
                          .capture_row = "q08b poly.neg_size_rect.mode0",
                          .rect = {8, 8, -4, -3},
                          .painted_bounds = {.left = 4, .top = 5, .right = 8, .bottom = 8},
                          .painted_pixels = 12},
            geometry_case{.name = "negative origin",
                          .call = geometry_call::pat_blt,
                          .capture_row = "q08 nongui.patblt.solid.PATCOPY_negxy",
                          .rect = {-2, -2, 6, 6},
                          .painted_bounds = {.left = 0, .top = 0, .right = 4, .bottom = 4},
                          .painted_pixels = 16},
        };

        struct call_case
        {
            const char* name;
            DWORD rop;
            DWORD mode;
            DWORD count;
            bool null_array;
            bool invalid_dc;
            BOOL result;
            DWORD last_error;
            bool fills_canvas;
        };

        constexpr std::array<call_case, 17> k_call_cases = {
            call_case{.name = "mode 1",
                      .rop = k_patcopy,
                      .mode = 1,
                      .count = 3,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "mode 2",
                      .rop = k_patcopy,
                      .mode = 2,
                      .count = 3,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "mode 3",
                      .rop = k_patcopy,
                      .mode = 3,
                      .count = 3,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "mode 0xFFFFFFFF",
                      .rop = k_patcopy,
                      .mode = 0xFFFFFFFF,
                      .count = 3,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "count 0",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 0,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "count 0 invalid dc",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 0,
                      .null_array = false,
                      .invalid_dc = true,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "count 0 mode 1",
                      .rop = k_patcopy,
                      .mode = 1,
                      .count = 0,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "count 0 SRCCOPY",
                      .rop = 0x00CC0020,
                      .mode = 0,
                      .count = 0,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "null array",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 1,
                      .null_array = true,
                      .invalid_dc = false,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "null array invalid dc",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 1,
                      .null_array = true,
                      .invalid_dc = true,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "invalid dc",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 1,
                      .null_array = false,
                      .invalid_dc = true,
                      .result = FALSE,
                      .last_error = k_error_invalid_handle,
                      .fills_canvas = false},
            call_case{.name = "count 0xFFF",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 0xFFF,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = true},
            call_case{.name = "count 0x1000",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 0x1000,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = true},
            call_case{.name = "count 0x1001",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = 0x1001,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = true},
            call_case{.name = "count 0x2000",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = k_max_entries,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = TRUE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = true},
            call_case{.name = "count above the maximum",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = k_max_count + 1,
                      .null_array = false,
                      .invalid_dc = false,
                      .result = FALSE,
                      .last_error = k_untouched_last_error,
                      .fills_canvas = false},
            call_case{.name = "count above the maximum invalid dc",
                      .rop = k_patcopy,
                      .mode = 0,
                      .count = k_max_count + 1,
                      .null_array = false,
                      .invalid_dc = true,
                      .result = FALSE,
                      .last_error = k_error_invalid_handle,
                      .fills_canvas = false},
        };

        class GdiPolyPatBltTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                syscalls::handle_NtGdiInit(this->context());

                this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), k_side, k_side, 1, 32, 0);
                ASSERT_NE(0u, this->dc);
                ASSERT_NE(0u, bitmap);
                syscalls::handle_NtGdiSelectBitmap(this->context(), this->dc, make_handle(bitmap));

                this->brush = static_cast<uint32_t>(syscalls::handle_NtGdiCreateSolidBrush(this->context(), k_brush, 0));
                syscalls::handle_NtGdiSelectBrushLocal(this->context(), this->dc, this->brush, 0);
                this->entries = this->emu.memory.allocate_memory(k_max_entries * k_entry_size, memory_permission::read_write);
                this->fill_canvas();
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            void fill_canvas()
            {
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        syscalls::handle_NtGdiSetPixel(this->context(), this->dc, x, y, k_canvas);
                    }
                }
            }

            COLORREF pixel(const int x, const int y)
            {
                return syscalls::handle_NtGdiGetPixel(this->context(), this->dc, x, y);
            }

            std::map<COLORREF, int> pixel_histogram()
            {
                std::map<COLORREF, int> histogram{};
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        ++histogram[this->pixel(x, y)];
                    }
                }
                return histogram;
            }

            std::map<COLORREF, int> expected_histogram(const std::optional<COLORREF>& painted)
            {
                if (!painted)
                {
                    return {{k_canvas, k_side * k_side}};
                }
                return {{k_canvas, (k_side * k_side) - k_painted_pixels}, {*painted, k_painted_pixels}};
            }

            void write_entry(const uint64_t address, const std::array<int32_t, 4>& geometry, const uint64_t brush_value)
            {
                this->emu.memory.write_memory(address, geometry.data(), sizeof(geometry));
                this->emu.memory.write_memory(address + sizeof(geometry), brush_value);
            }

            uint64_t dc_attr()
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return this->emu.memory
                    .read_memory<GDI_HANDLE_ENTRY64>(table + ((static_cast<uint32_t>(this->dc) & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64)))
                    .Object;
            }

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, StockObjects) +
                                                                                    (index * sizeof(uint64_t))));
            }

            void select_like_gdi32(const uint64_t attr_offset, const uint32_t object)
            {
                const uint64_t value = object;
                this->emu.memory.write_memory(this->dc_attr() + attr_offset, &value, sizeof(value));
            }

            void set_dc_brush_color(const COLORREF color)
            {
                this->emu.memory.write_memory(this->dc_attr() + k_dc_attr_dc_brush_color, &color, sizeof(color));
            }

            void set_dc_pen_color(const COLORREF color)
            {
                this->emu.memory.write_memory(this->dc_attr() + k_dc_attr_dc_pen_color, &color, sizeof(color));
            }

            uint64_t brush_field_value(const brush_field field, const uint32_t pen)
            {
                switch (field)
                {
                case brush_field::zero:
                    return 0;
                case brush_field::invalid_handle:
                    return 0xDEAD;
                case brush_field::colorref:
                    return k_brush;
                case brush_field::pen_handle:
                    return pen;
                case brush_field::stock_null:
                    return this->stock_object(k_stock_null_brush);
                case brush_field::stock_dc_brush:
                    return this->stock_object(k_stock_dc_brush);
                case brush_field::stock_null_pen:
                    return this->stock_object(k_stock_null_pen);
                case brush_field::stock_dc_pen:
                    return this->stock_object(k_stock_dc_pen);
                }
                return 0;
            }

            std::pair<RECT, int> painted_bounds()
            {
                RECT bounds{.left = k_side, .top = k_side, .right = 0, .bottom = 0};
                int pixels = 0;
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        if (this->pixel(x, y) != k_canvas)
                        {
                            bounds = {.left = std::min<LONG>(bounds.left, x),
                                      .top = std::min<LONG>(bounds.top, y),
                                      .right = std::max<LONG>(bounds.right, x + 1),
                                      .bottom = std::max<LONG>(bounds.bottom, y + 1)};
                            ++pixels;
                        }
                    }
                }
                return {bounds, pixels};
            }

            DWORD last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void reset_last_error()
            {
                this->emu.vcpu(0).active_thread->teb64->access([](TEB64& teb) { teb.LastErrorValue = k_untouched_last_error; });
            }

            BOOL poly_pat_blt_call(const call_case& test_case)
            {
                for (DWORD i = 0; i < std::min(test_case.count, k_max_entries); ++i)
                {
                    const auto x = static_cast<LONG>(i % k_side);
                    const auto y = static_cast<LONG>((i / k_side) % k_side);
                    this->write_entry(this->entries + (i * k_entry_size), {x, y, 1, 1}, this->brush);
                }

                this->reset_last_error();
                return syscalls::handle_NtGdiPolyPatBlt(this->context(), test_case.invalid_dc ? 0 : this->dc, test_case.rop,
                                                        test_case.null_array ? 0 : this->entries, test_case.count, test_case.mode);
            }

            BOOL poly_pat_blt(const DWORD rop)
            {
                const auto half_width = (k_rect.right - k_rect.left) / 2;
                const auto height = k_rect.bottom - k_rect.top;
                this->write_entry(this->entries, {k_rect.left, k_rect.top, half_width, height}, this->brush);
                this->write_entry(this->entries + k_entry_size, {k_rect.left + half_width, k_rect.top, half_width, height}, this->brush);
                return syscalls::handle_NtGdiPolyPatBlt(this->context(), this->dc, rop, this->entries, 2, k_ppb_brush);
            }

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
            uint32_t brush{};
            emulator_pointer entries{};
        };
    }

    TEST_F(GdiPolyPatBltTest, PolyPatBltAndPatBltApplyTheRasterOperation)
    {
        for (const auto& test_case : k_rop_cases)
        {
            SCOPED_TRACE(test_case.name);
            this->fill_canvas();
            EXPECT_EQ(test_case.result, this->poly_pat_blt(test_case.rop));
            EXPECT_EQ(this->expected_histogram(test_case.painted), this->pixel_histogram());

            this->fill_canvas();
            EXPECT_EQ(test_case.result, syscalls::handle_NtGdiPatBlt(this->context(), this->dc, 2, 3, 6, 5, test_case.rop));
            EXPECT_EQ(this->expected_histogram(test_case.painted), this->pixel_histogram());
        }

        const auto pen = static_cast<uint32_t>(syscalls::handle_NtGdiCreatePen(this->context(), 0, 0, k_pen_color));
        this->set_dc_brush_color(k_dc_brush_color);
        this->set_dc_pen_color(k_dc_pen_color);
        for (const auto& test_case : k_brush_field_cases)
        {
            SCOPED_TRACE(test_case.name);
            this->fill_canvas();
            this->write_entry(this->entries, {k_rect.left, k_rect.top, k_rect.right - k_rect.left, k_rect.bottom - k_rect.top},
                              this->brush_field_value(test_case.field, pen));
            EXPECT_TRUE(syscalls::handle_NtGdiPolyPatBlt(this->context(), this->dc, test_case.rop, this->entries, 1, k_ppb_brush));
            EXPECT_EQ(this->expected_histogram(test_case.painted), this->pixel_histogram());
        }

        this->select_like_gdi32(k_dc_attr_brush, this->stock_object(k_stock_null_brush));
        for (const auto& [name, rop, painted] :
             {std::tuple{"selected null brush PATCOPY", k_patcopy, std::optional<COLORREF>{}},
              std::tuple{"selected null brush DSTINVERT", k_dstinvert, std::optional<COLORREF>{0x00CCDDEE}}})
        {
            SCOPED_TRACE(name);
            this->fill_canvas();
            EXPECT_TRUE(syscalls::handle_NtGdiPatBlt(this->context(), this->dc, 2, 3, 6, 5, rop));
            EXPECT_EQ(this->expected_histogram(painted), this->pixel_histogram());
        }
        this->select_like_gdi32(k_dc_attr_brush, this->brush);

        for (const auto& test_case : k_geometry_cases)
        {
            SCOPED_TRACE(test_case.capture_row);
            this->fill_canvas();
            if (test_case.call == geometry_call::poly_pat_blt)
            {
                this->write_entry(this->entries, test_case.rect, this->brush);
                EXPECT_TRUE(syscalls::handle_NtGdiPolyPatBlt(this->context(), this->dc, k_patcopy, this->entries, 1, k_ppb_brush));
            }
            else
            {
                EXPECT_TRUE(syscalls::handle_NtGdiPatBlt(this->context(), this->dc, test_case.rect[0], test_case.rect[1], test_case.rect[2],
                                                         test_case.rect[3], k_patcopy));
            }

            const auto [bounds, pixels] = this->painted_bounds();
            EXPECT_EQ(test_case.painted_pixels, pixels);
            EXPECT_TRUE(bounds.left == test_case.painted_bounds.left && bounds.top == test_case.painted_bounds.top &&
                        bounds.right == test_case.painted_bounds.right && bounds.bottom == test_case.painted_bounds.bottom);
        }

        for (const auto& test_case : k_call_cases)
        {
            SCOPED_TRACE(test_case.name);
            this->fill_canvas();
            EXPECT_EQ(test_case.result, this->poly_pat_blt_call(test_case));
            EXPECT_EQ(test_case.last_error, this->last_error());

            const auto expected = test_case.fills_canvas ? std::map<COLORREF, int>{{k_brush, k_side * k_side}}
                                                         : std::map<COLORREF, int>{{k_canvas, k_side * k_side}};
            EXPECT_EQ(expected, this->pixel_histogram());
        }

        SCOPED_TRACE("invalid dc");
        this->reset_last_error();
        EXPECT_FALSE(syscalls::handle_NtGdiPatBlt(this->context(), 0, 2, 3, 6, 5, k_patcopy));
        EXPECT_EQ(k_untouched_last_error, this->last_error());
    }
}

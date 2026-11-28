#include "std_include.hpp"

namespace sogen::syscalls
{
    BOOL erase_window_background(const syscall_context& c, const window& win, hdc dc);
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint64_t handle_NtGdiSelectBrushLocal(const syscall_context& c, hdc dc, uint32_t brush, emulator_pointer old_brush_ptr);
    uint64_t handle_NtUserMessageCall(const syscall_context& c, hwnd hwnd, UINT msg, uint64_t w_param, uint64_t l_param,
                                      uint64_t result_info, DWORD type, BOOL ansi);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
}

namespace sogen::test
{
    namespace
    {
        constexpr int k_side = 64;
        constexpr COLORREF k_canvas = 0x00563412;
        constexpr ULONG k_untouched_error = 0xDEAD;
        constexpr ULONG k_error_invalid_handle = 6;
        constexpr uint32_t k_stock_white_brush = 0;
        constexpr uint32_t k_stock_ltgray_brush = 1;
        constexpr uint32_t k_stock_dkgray_brush = 3;
        constexpr uint32_t k_stock_black_brush = 4;
        constexpr uint32_t k_stock_null_brush = 5;
        constexpr uint32_t k_stock_dc_brush = 18;
        constexpr uint64_t k_dc_attr_dc_brush_color_offset = 0xC4;
        constexpr int k_client_side = 16;

        struct brush_case
        {
            const char* name;
            uint64_t class_brush;
            std::optional<COLORREF> painted;
            BOOL result;
        };

        class WindowEraseBackgroundTest : public testing::Test
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

                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.class_key = class_key;
                this->window_handle = win.handle;
                this->emu.process.classes[std::u16string{class_key}] = {};
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            void set_class_brush(const uint64_t brush)
            {
                this->emu.process.classes.at(std::u16string{class_key}).wnd_class.hbrBackground = brush;
            }

            void set_class_style(const uint32_t style)
            {
                this->emu.process.classes.at(std::u16string{class_key}).wnd_class.style = style;
            }

            window& test_window()
            {
                return *this->emu.process.windows.get(this->window_handle);
            }

            uint64_t default_window_proc(const UINT msg)
            {
                return syscalls::handle_NtUserMessageCall(this->context(), this->window_handle, msg, 0, 0, 0, 0, FALSE);
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

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, StockObjects) +
                                                                                    (index * sizeof(uint64_t))));
            }

            BOOL erase(const hdc target)
            {
                return syscalls::erase_window_background(this->context(), this->test_window(), target);
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

            void set_dc_brush_color(const COLORREF color)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                const auto dc_entry = this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(
                    table + ((static_cast<uint32_t>(this->dc) & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64)));
                this->emu.memory.write_memory(dc_entry.Object + k_dc_attr_dc_brush_color_offset, color);
            }

            void reset_error()
            {
                this->emu.vcpu(0).active_thread->teb64->field<ULONG>(offsetof(TEB64, LastErrorValue)).write(k_untouched_error);
            }

            ULONG last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            static constexpr std::u16string_view class_key = u"SogenEraseTestClass";

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
            hwnd window_handle{};
        };
    }

    TEST_F(WindowEraseBackgroundTest, ClassBrushDecidesTheResultAndTheColor)
    {
        const auto red_brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), 0x000000FF, 0);
        const auto deleted_brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), 0x00FF0000, 0);
        ASSERT_EQ(1u, syscalls::handle_NtGdiDeleteObjectApp(this->context(), static_cast<uint32_t>(deleted_brush)));
        this->set_dc_brush_color(0x0034AB12);

        const std::vector<brush_case> cases{
            {.name = "null", .class_brush = 0, .painted = std::nullopt, .result = FALSE},
            {.name = "stock null brush", .class_brush = this->stock_object(k_stock_null_brush), .painted = std::nullopt, .result = TRUE},
            {.name = "stock white", .class_brush = this->stock_object(k_stock_white_brush), .painted = 0x00FFFFFF, .result = TRUE},
            {.name = "stock black", .class_brush = this->stock_object(k_stock_black_brush), .painted = 0x00000000, .result = TRUE},
            {.name = "stock ltgray", .class_brush = this->stock_object(k_stock_ltgray_brush), .painted = 0x00C0C0C0, .result = TRUE},
            {.name = "stock dkgray", .class_brush = this->stock_object(k_stock_dkgray_brush), .painted = 0x00404040, .result = TRUE},
            {.name = "stock dc brush", .class_brush = this->stock_object(k_stock_dc_brush), .painted = 0x0034AB12, .result = TRUE},
            {.name = "solid red", .class_brush = red_brush, .painted = 0x000000FF, .result = TRUE},
            {.name = "deleted solid", .class_brush = deleted_brush, .painted = 0x00FFFFFF, .result = TRUE},
            {.name = "COLOR_WINDOW + 1", .class_brush = 6, .painted = 0x00FFFFFF, .result = TRUE},
            {.name = "COLOR_WINDOWTEXT + 1", .class_brush = 9, .painted = 0x00000000, .result = TRUE},
            {.name = "COLOR_BTNFACE + 1", .class_brush = 16, .painted = 0x00F0F0F0, .result = TRUE},
        };

        for (const auto& test_case : cases)
        {
            SCOPED_TRACE(test_case.name);
            this->fill_canvas();
            this->set_class_brush(test_case.class_brush);

            EXPECT_EQ(test_case.result, this->erase(this->dc));

            const auto expected = test_case.painted.value_or(k_canvas);
            EXPECT_EQ((std::map<COLORREF, int>{{expected, k_side * k_side}}), this->pixel_histogram());
        }

        this->set_class_brush(deleted_brush);
        this->fill_canvas();
        syscalls::handle_NtGdiSelectBrushLocal(this->context(), this->dc, static_cast<uint32_t>(red_brush), 0);
        EXPECT_EQ(TRUE, this->erase(this->dc));
        EXPECT_EQ((std::map<COLORREF, int>{{0x000000FF, k_side * k_side}}), this->pixel_histogram());

        this->test_window().width = k_client_side;
        this->test_window().height = k_client_side;
        this->set_class_brush(red_brush);
        this->fill_canvas();
        this->set_class_style(CS_PARENTDC);
        EXPECT_EQ(TRUE, this->erase(this->dc));
        EXPECT_EQ((std::map<COLORREF, int>{{0x000000FF, k_client_side * k_client_side},
                                           {k_canvas, (k_side * k_side) - (k_client_side * k_client_side)}}),
                  this->pixel_histogram());
        EXPECT_EQ(0x000000FFu, this->pixel(k_client_side - 1, k_client_side - 1));
        EXPECT_EQ(k_canvas, this->pixel(k_client_side, k_client_side));

        this->fill_canvas();
        this->set_class_style(0);
        EXPECT_EQ(TRUE, this->erase(this->dc));
        EXPECT_EQ((std::map<COLORREF, int>{{0x000000FF, k_side * k_side}}), this->pixel_histogram());
    }

    TEST_F(WindowEraseBackgroundTest, InvalidDcAndIconEraseKeepTheirDefaultAnswers)
    {
        EXPECT_EQ(TRUE, this->default_window_proc(WM_ICONERASEBKGND));
        this->test_window().style |= static_cast<uint32_t>(WS_CHILD);
        EXPECT_EQ(FALSE, this->default_window_proc(WM_ICONERASEBKGND));

        const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), 4, 4, 1, 32, 0);
        const auto brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), 0x000000FF, 0);
        const auto deleted_dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
        ASSERT_EQ(1u, syscalls::handle_NtGdiDeleteObjectApp(this->context(), static_cast<uint32_t>(deleted_dc)));
        this->set_class_brush(6);
        this->fill_canvas();

        for (const uint64_t target : {uint64_t{0}, uint64_t{1}, uint64_t{0x1234}, bitmap, brush, static_cast<uint64_t>(deleted_dc)})
        {
            SCOPED_TRACE(target);
            this->reset_error();
            EXPECT_EQ(TRUE, this->erase(static_cast<hdc>(target)));
            EXPECT_EQ(k_error_invalid_handle, this->last_error());
        }

        EXPECT_EQ((std::map<COLORREF, int>{{k_canvas, k_side * k_side}}), this->pixel_histogram());
    }
}

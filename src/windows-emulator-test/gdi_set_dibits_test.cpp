#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    int handle_NtGdiSetDIBitsToDeviceInternal(const syscall_context& c, hdc dc, int x_dest, int y_dest, uint32_t width, uint32_t height,
                                              int x_src, int y_src, uint32_t start_scan, uint32_t scan_lines, emulator_pointer bits,
                                              emulator_pointer info, uint32_t color_use, uint32_t max_bits, uint32_t max_info,
                                              uint32_t transform_coordinates, uint64_t color_transform);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
}

namespace sogen::test
{
    namespace
    {
        constexpr int k_canvas_width = 8;
        constexpr COLORREF k_background = 0x777777;
        constexpr std::array<uint32_t, 4> k_dib_row = {0x00112233, 0x00445566, 0x00778899, 0x00AABBCC};

        COLORREF dib_pixel_as_colorref(const uint32_t pixel)
        {
            return ((pixel & 0xFF) << 16) | (pixel & 0xFF00) | ((pixel >> 16) & 0xFF);
        }

        class GdiSetDIBitsTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();

                this->dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                ASSERT_NE(0u, this->dc);
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), k_canvas_width, 1, 1, 32, 0);
                ASSERT_NE(0u, bitmap);
                syscalls::handle_NtGdiSelectBitmap(this->context(), this->dc, make_handle(bitmap));
                for (int x = 0; x < k_canvas_width; ++x)
                {
                    syscalls::handle_NtGdiSetPixel(this->context(), this->dc, x, 0, k_background);
                }

                this->info = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->bits = this->info + 0x100;
                const EMU_BITMAPINFOHEADER header{
                    .biSize = sizeof(EMU_BITMAPINFOHEADER),
                    .biWidth = static_cast<LONG>(k_dib_row.size()),
                    .biHeight = 1,
                    .biPlanes = 1,
                    .biBitCount = 32,
                    .biCompression = 0,
                    .biSizeImage = 0,
                    .biXPelsPerMeter = 0,
                    .biYPelsPerMeter = 0,
                    .biClrUsed = 0,
                    .biClrImportant = 0,
                };
                this->emu.memory.write_memory(this->info, &header, sizeof(header));
                this->emu.memory.write_memory(this->bits, k_dib_row.data(), sizeof(k_dib_row));
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            int set_dibits(const int x_dest, const uint32_t width, const int x_src)
            {
                return syscalls::handle_NtGdiSetDIBitsToDeviceInternal(this->context(), this->dc, x_dest, 0, width, 1, x_src, 0, 0, 1,
                                                                       this->bits, this->info, 0, sizeof(k_dib_row), 0, 0, 0);
            }

            COLORREF pixel(const int x)
            {
                return syscalls::handle_NtGdiGetPixel(this->context(), this->dc, x, 0);
            }

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
            emulator_pointer info{};
            emulator_pointer bits{};
        };
    }

    TEST_F(GdiSetDIBitsTest, SourceColumnsLeftOfTheDibAreNotDrawn)
    {
        EXPECT_EQ(1, this->set_dibits(0, 6, -2));

        EXPECT_EQ(k_background, this->pixel(0));
        EXPECT_EQ(k_background, this->pixel(1));
        for (int x = 2; x < 6; ++x)
        {
            EXPECT_EQ(dib_pixel_as_colorref(k_dib_row[static_cast<size_t>(x - 2)]), this->pixel(x)) << "column " << x;
        }
        EXPECT_EQ(k_background, this->pixel(6));
        EXPECT_EQ(k_background, this->pixel(7));
    }

    TEST_F(GdiSetDIBitsTest, SourceColumnsRightOfTheDibAreNotDrawn)
    {
        EXPECT_EQ(1, this->set_dibits(0, 6, 2));

        EXPECT_EQ(dib_pixel_as_colorref(k_dib_row[2]), this->pixel(0));
        EXPECT_EQ(dib_pixel_as_colorref(k_dib_row[3]), this->pixel(1));
        for (int x = 2; x < k_canvas_width; ++x)
        {
            EXPECT_EQ(k_background, this->pixel(x)) << "column " << x;
        }
    }
}

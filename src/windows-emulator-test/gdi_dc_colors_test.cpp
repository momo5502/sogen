#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
    BOOL handle_NtGdiExtTextOutW(const syscall_context& c, hdc dc, LONG x, LONG y, UINT options, emulator_pointer rect,
                                 emulator_pointer text, UINT count, emulator_pointer dx, DWORD code_page);
}

namespace sogen::test
{
    namespace
    {
        // gdi32 reads and writes these DC_ATTR fields directly (SetBkColor, GetBkColor, GetTextColor and the batched
        // ExtTextOut record), so they are a fixed contract with the emulated kernel.
        constexpr uint64_t k_realized_background_offset = 0xB0;
        constexpr uint64_t k_background_offset = 0xB4;
        constexpr uint64_t k_text_offset = 0xBC;

        constexpr int k_side = 16;
        constexpr COLORREF k_white = 0x00FFFFFF;
        constexpr COLORREF k_canvas = 0x00112233;
        constexpr UINT k_eto_opaque = 0x2;

        class GdiDcColorsTest : public testing::Test
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

                this->scratch = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
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

            COLORREF attr(const uint64_t offset)
            {
                return this->emu.memory.read_memory<COLORREF>(this->dc_attr() + offset);
            }

            void opaque_text_over_canvas(const RECT& rect)
            {
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        syscalls::handle_NtGdiSetPixel(this->context(), this->dc, x, y, k_canvas);
                    }
                }

                constexpr char16_t space = u' ';
                this->emu.memory.write_memory(this->scratch, &rect, sizeof(rect));
                this->emu.memory.write_memory(this->scratch + 0x100, &space, sizeof(space));
                EXPECT_EQ(TRUE, syscalls::handle_NtGdiExtTextOutW(this->context(), this->dc, rect.left, rect.top, k_eto_opaque,
                                                                  this->scratch, this->scratch + 0x100, 1, 0, 0));
            }

            COLORREF pixel(const int x, const int y)
            {
                return syscalls::handle_NtGdiGetPixel(this->context(), this->dc, x, y);
            }

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
            emulator_pointer scratch{};
        };
    }

    TEST_F(GdiDcColorsTest, NewDcDrawsBlackOnWhite)
    {
        EXPECT_EQ(k_white, this->attr(k_background_offset));
        EXPECT_EQ(k_white, this->attr(k_realized_background_offset));
        EXPECT_EQ(0u, this->attr(k_text_offset));

        this->opaque_text_over_canvas({.left = 2, .top = 2, .right = 10, .bottom = 6});

        EXPECT_EQ(k_white, this->pixel(2, 2));
        EXPECT_EQ(k_white, this->pixel(9, 5));
        EXPECT_EQ(k_canvas, this->pixel(10, 5));
    }

    TEST_F(GdiDcColorsTest, OpaqueTextUsesTheBackgroundColorSetBkColorStores)
    {
        constexpr COLORREF orange = 0x000080FF;
        this->emu.memory.write_memory(this->dc_attr() + k_background_offset, &orange, sizeof(orange));

        this->opaque_text_over_canvas({.left = 2, .top = 2, .right = 10, .bottom = 6});

        EXPECT_EQ(orange, this->pixel(5, 3));
        EXPECT_EQ(k_canvas, this->pixel(1, 1));
    }
}

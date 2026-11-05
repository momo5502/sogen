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
    BOOL handle_NtGdiFlush(const syscall_context& c);
}

namespace sogen::test
{
    namespace
    {
        constexpr int k_side = 16;
        constexpr int16_t k_text_out_rect = 3;
        constexpr uint32_t k_background = 0x112233;
        constexpr uint32_t k_fill = 0xAABBCC;
        constexpr UINT k_eto_opaque = 0x2;
        constexpr UINT k_eto_clipped = 0x4;

        struct batch_text_out_rect
        {
            int16_t size{sizeof(batch_text_out_rect)};
            int16_t command{k_text_out_rect};
            COLORREF background{};
            UINT options{};
            RECT rect{};
            POINTL viewport_org{};
            uint32_t background_attribute{};
        };

        static_assert(sizeof(batch_text_out_rect) == 0x28);

        COLORREF colorref_of(const uint32_t rgb)
        {
            return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
        }

        class GdiBatchTextOutRectTest : public testing::Test
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
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            void fill_background()
            {
                for (int y = 0; y < k_side; ++y)
                {
                    for (int x = 0; x < k_side; ++x)
                    {
                        syscalls::handle_NtGdiSetPixel(this->context(), this->dc, x, y, colorref_of(k_background));
                    }
                }
            }

            void queue_and_flush(const batch_text_out_rect& record)
            {
                this->emu.vcpu(0).active_thread->teb64->access([&](TEB64& teb) {
                    auto& batch = teb.GdiTebBatch;
                    std::memcpy(reinterpret_cast<std::byte*>(batch.Buffer) + batch.Offset, &record, sizeof(record));
                    batch.Offset += sizeof(record);
                    batch.HDC = static_cast<uint64_t>(this->dc);
                    ++teb.GdiBatchCount;
                });

                EXPECT_EQ(TRUE, syscalls::handle_NtGdiFlush(this->context()));
            }

            uint32_t pixel(const int x, const int y)
            {
                return colorref_of(syscalls::handle_NtGdiGetPixel(this->context(), this->dc, x, y));
            }

            windows_emulator emu = create_sample_emulator();
            hdc dc{};
        };
    }

    TEST_F(GdiBatchTextOutRectTest, FillsLikeTheWindowsCaptureOfOpaqueExtTextOut)
    {
        struct fill_case
        {
            std::string_view name{};
            UINT options{};
            RECT rect{};
            POINTL viewport_org{};
            RECT painted{};
        };

        const std::array<fill_case, 8> cases{{
            {.name = "opaque", .options = k_eto_opaque, .rect = {2, 3, 8, 8}, .painted = {2, 3, 8, 8}},
            {.name = "opaque and clipped", .options = k_eto_opaque | k_eto_clipped, .rect = {2, 3, 8, 8}, .painted = {2, 3, 8, 8}},
            {.name = "empty rect", .options = k_eto_opaque, .rect = {4, 4, 4, 4}, .painted = {0, 0, 0, 0}},
            {.name = "inverted rect", .options = k_eto_opaque, .rect = {8, 8, 4, 4}, .painted = {4, 4, 8, 8}},
            {.name = "inverted in x only", .options = k_eto_opaque, .rect = {8, 3, 4, 8}, .painted = {4, 3, 8, 8}},
            {.name = "inverted in y only", .options = k_eto_opaque, .rect = {3, 8, 8, 4}, .painted = {3, 4, 8, 8}},
            {.name = "past the surface", .options = k_eto_opaque, .rect = {12, 12, 24, 24}, .painted = {12, 12, 16, 16}},
            {.name = "viewport origin",
             .options = k_eto_opaque,
             .rect = {1, 1, 5, 5},
             .viewport_org = {.x = 3, .y = 2},
             .painted = {4, 3, 8, 7}},
        }};

        for (const auto& fill : cases)
        {
            SCOPED_TRACE(fill.name);
            this->fill_background();
            this->queue_and_flush(batch_text_out_rect{
                .background = colorref_of(k_fill), .options = fill.options, .rect = fill.rect, .viewport_org = fill.viewport_org});

            for (int y = 0; y < k_side; ++y)
            {
                for (int x = 0; x < k_side; ++x)
                {
                    const auto inside =
                        x >= fill.painted.left && x < fill.painted.right && y >= fill.painted.top && y < fill.painted.bottom;
                    EXPECT_EQ(inside ? k_fill : k_background, this->pixel(x, y)) << "pixel " << x << "," << y;
                }
            }
        }
    }
}

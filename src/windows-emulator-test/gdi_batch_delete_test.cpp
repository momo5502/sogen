#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    uint64_t handle_NtGdiCreatePen(const syscall_context& c, uint32_t style, uint32_t width, uint32_t color);
    uint64_t handle_NtGdiCreateRectRgn(const syscall_context& c, LONG x_left, LONG y_top, LONG x_right, LONG y_bottom);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
    BOOL handle_NtGdiFlush(const syscall_context& c);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
}

namespace sogen::test
{
    namespace
    {
        constexpr int16_t k_poly_pat_blt = 1;
        constexpr int16_t k_delete_object = 7;
        constexpr int16_t k_delete_region = 8;
        constexpr DWORD k_patcopy = 0x00F00021;
        constexpr COLORREF k_background = 0x777777;
        constexpr COLORREF k_red = 0x0000FF;
        constexpr COLORREF k_btnface = 0xF0F0F0;
        constexpr uint32_t k_ps_solid = 0;
        constexpr uint32_t k_stock_null_brush = 5;
        constexpr uint32_t k_stock_black_pen = 7;
        constexpr size_t k_color_btnface = 15;

        struct batch_delete
        {
            int16_t size{sizeof(batch_delete)};
            int16_t command{};
            uint32_t reserved{};
            uint64_t handle{};
        };

        struct batch_pat_rect
        {
            LONG x{};
            LONG y{};
            LONG width{};
            LONG height{};
            uint64_t brush{};
        };

        struct batch_poly_pat_blt
        {
            int16_t size{sizeof(batch_poly_pat_blt)};
            int16_t command{k_poly_pat_blt};
            DWORD rop{k_patcopy};
            DWORD mode{};
            DWORD count{1};
            std::array<uint32_t, 8> colors_and_origin{};
            batch_pat_rect rect{};
        };

        static_assert(sizeof(batch_delete) == 0x10);
        static_assert(offsetof(batch_poly_pat_blt, rect) == 0x30);

        class GdiBatchDeleteTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                syscalls::handle_NtGdiInit(this->context());
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            template <typename Command>
            void queue(const Command& command, const uint64_t dc = 0)
            {
                this->emu.vcpu(0).active_thread->teb64->access([&](TEB64& teb) {
                    auto& batch = teb.GdiTebBatch;
                    std::memcpy(reinterpret_cast<std::byte*>(batch.Buffer) + batch.Offset, &command, sizeof(command));
                    batch.Offset += sizeof(command);
                    if (dc != 0)
                    {
                        batch.HDC = dc;
                    }
                    ++teb.GdiBatchCount;
                });
            }

            void queue_delete(const int16_t command_id, const uint32_t target)
            {
                this->queue(batch_delete{.command = command_id, .handle = target});
            }

            void flush()
            {
                EXPECT_EQ(TRUE, syscalls::handle_NtGdiFlush(this->context()));
            }

            bool gdi32_accepts(const uint32_t handle_value)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                const auto e =
                    this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + ((handle_value & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64)));
                const auto high = handle_value >> 16;
                const auto owner = e.Owner & ~1u;
                return e.Type == (high & 0x1F) && ((e.Unique ^ high) & 0xFF7F) == 0 &&
                       (owner == this->emu.process.process_id || owner == 0);
            }

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, StockObjects) +
                                                                                    (index * sizeof(uint64_t))));
            }

            hdc create_dc()
            {
                const auto dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), 2, 2, 1, 32, 0);
                EXPECT_NE(0u, dc);
                EXPECT_NE(0u, bitmap);
                syscalls::handle_NtGdiSelectBitmap(this->context(), dc, make_handle(bitmap));
                syscalls::handle_NtGdiSetPixel(this->context(), dc, 0, 0, k_background);
                return dc;
            }

            void queue_paint(const hdc dc, const uint32_t brush)
            {
                this->queue(batch_poly_pat_blt{.rect = {.x = 0, .y = 0, .width = 1, .height = 1, .brush = brush}}, dc);
            }

            windows_emulator emu = create_sample_emulator();
        };
    }

    TEST_F(GdiBatchDeleteTest, QueuedDeletesFreeTheirObjectsAtTheFlushAfterEarlierDrawing)
    {
        const auto dc = this->create_dc();
        const auto brush = static_cast<uint32_t>(syscalls::handle_NtGdiCreateSolidBrush(this->context(), k_red, 0));
        const auto pen = static_cast<uint32_t>(syscalls::handle_NtGdiCreatePen(this->context(), k_ps_solid, 0, k_red));
        const auto region = static_cast<uint32_t>(syscalls::handle_NtGdiCreateRectRgn(this->context(), 0, 0, 2, 2));

        this->queue_delete(k_delete_object, pen);
        this->queue_delete(k_delete_region, region);
        EXPECT_TRUE(this->gdi32_accepts(pen));
        EXPECT_TRUE(this->gdi32_accepts(region));
        this->flush();
        EXPECT_FALSE(this->gdi32_accepts(pen));
        EXPECT_FALSE(this->gdi32_accepts(region));

        this->queue_paint(dc, brush);
        this->queue_delete(k_delete_object, brush);
        EXPECT_TRUE(this->gdi32_accepts(brush));
        this->flush();
        EXPECT_EQ(k_red, syscalls::handle_NtGdiGetPixel(this->context(), dc, 0, 0));
        EXPECT_FALSE(this->gdi32_accepts(brush));
    }

    TEST_F(GdiBatchDeleteTest, DeletingStockObjectsAndSystemColorBrushesKeepsThem)
    {
        const auto null_brush = this->stock_object(k_stock_null_brush);
        const auto black_pen = this->stock_object(k_stock_black_pen);
        const auto system_brush =
            static_cast<uint32_t>(this->emu.process.user_handles.get_server_info().read().ahbrSystem[k_color_btnface]);

        for (const auto object : {null_brush, black_pen, system_brush})
        {
            this->queue_delete(k_delete_object, object);
            this->flush();
            EXPECT_EQ(1u, syscalls::handle_NtGdiDeleteObjectApp(this->context(), object));
            EXPECT_TRUE(this->gdi32_accepts(object));
        }

        const auto dc = this->create_dc();
        this->queue_paint(dc, system_brush);
        this->flush();
        EXPECT_EQ(k_btnface, syscalls::handle_NtGdiGetPixel(this->context(), dc, 0, 0));
    }
}

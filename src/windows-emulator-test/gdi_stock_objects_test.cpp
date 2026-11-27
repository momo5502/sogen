#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_stock_bitmap_index = 21;

        class GdiStockObjectsTest : public testing::Test
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

            uint32_t stock_object(const uint32_t index)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                return static_cast<uint32_t>(this->emu.memory.read_memory<uint64_t>(table + offsetof(GDI_SHARED_MEMORY64, StockObjects) +
                                                                                    (index * sizeof(uint64_t))));
            }

            windows_emulator emu = create_sample_emulator();
        };
    }

    TEST_F(GdiStockObjectsTest, StockBitmapIsTheMemoryDcDefaultBitmap)
    {
        const auto stock_bitmap = this->stock_object(k_stock_bitmap_index);
        ASSERT_NE(0u, stock_bitmap);

        const auto dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
        ASSERT_NE(0u, dc);
        EXPECT_EQ(stock_bitmap, syscalls::handle_NtGdiSelectBitmap(this->context(), dc, make_handle(stock_bitmap)));
    }
}

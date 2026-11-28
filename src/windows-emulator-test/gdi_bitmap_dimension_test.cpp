#include "std_include.hpp"

#include <climits>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
    BOOL handle_NtGdiGetBitmapDimension(const syscall_context& c, handle bitmap, emulator_pointer size);
    BOOL handle_NtGdiSetBitmapDimension(const syscall_context& c, handle bitmap, int32_t width, int32_t height, emulator_pointer previous);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG k_untouched_error = 0xDEAD;
        constexpr ULONG k_error_invalid_handle = 6;

        struct dimension
        {
            int32_t width{};
            int32_t height{};

            bool operator==(const dimension&) const = default;
        };

        struct dimension_step
        {
            dimension set{};
            dimension previous{};
        };

        class GdiBitmapDimensionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                syscalls::handle_NtGdiInit(this->context());

                this->bitmap = make_handle(syscalls::handle_NtGdiCreateBitmap(this->context(), 8, 8, 1, 1, 0));
                ASSERT_NE(0u, this->bitmap.bits);

                this->scratch = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->reset_error();
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            void reset_error()
            {
                auto& thread = *this->emu.vcpu(0).active_thread;
                thread.teb64->field<ULONG>(offsetof(TEB64, LastErrorValue)).write(k_untouched_error);

                if (thread.teb32)
                {
                    thread.teb32->field<ULONG>(offsetof(TEB32, LastErrorValue)).write(k_untouched_error);
                }
            }

            void attach_wow64_teb()
            {
                this->emu.process.is_wow64_process = true;
                const auto address =
                    this->emu.memory.allocate_memory(static_cast<size_t>(page_align_up(sizeof(TEB32))), memory_permission::read_write);
                this->emu.vcpu(0).active_thread->teb32.emplace(this->emu.memory, address);
                this->reset_error();
            }

            ULONG last_error32()
            {
                return this->emu.vcpu(0).active_thread->teb32->read().LastErrorValue;
            }

            ULONG last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            BOOL get(const handle target, dimension& out)
            {
                const dimension poison{.width = 0x1111, .height = 0x1111};
                this->emu.memory.write_memory(this->scratch, poison);
                const auto result = syscalls::handle_NtGdiGetBitmapDimension(this->context(), target, this->scratch);
                out = this->emu.memory.read_memory<dimension>(this->scratch);
                return result;
            }

            BOOL set(const handle target, const dimension& size, dimension& previous)
            {
                const dimension poison{.width = 0x1111, .height = 0x1111};
                this->emu.memory.write_memory(this->scratch, poison);
                const auto result =
                    syscalls::handle_NtGdiSetBitmapDimension(this->context(), target, size.width, size.height, this->scratch);
                previous = this->emu.memory.read_memory<dimension>(this->scratch);
                return result;
            }

            dimension current(const handle target)
            {
                dimension out{};
                EXPECT_EQ(TRUE, this->get(target, out));
                return out;
            }

            void expect_invalid_handles_fail_with(const ULONG error_for_non_null, const bool wow64 = false)
            {
                const auto brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), 0x00FF0000, 0);
                const auto memory_dc = syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0);
                const auto stale_bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), 2, 2, 1, 1, 0);
                const auto deleted_bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), 2, 2, 1, 1, 0);
                ASSERT_NE(0u, brush);
                ASSERT_NE(0u, memory_dc);
                ASSERT_NE(0u, stale_bitmap);
                ASSERT_NE(0u, deleted_bitmap);
                syscalls::handle_NtGdiDeleteObjectApp(this->context(), static_cast<uint32_t>(deleted_bitmap));

                const std::vector<std::pair<handle, ULONG>> cases{
                    {make_handle(0), k_untouched_error},
                    {make_handle(1), error_for_non_null},
                    {make_handle(0xDEAD), error_for_non_null},
                    {make_handle(0xFFFFFFFF), error_for_non_null},
                    {make_handle(stale_bitmap ^ 0x10000), error_for_non_null},
                    {make_handle(brush), error_for_non_null},
                    {make_handle(memory_dc), error_for_non_null},
                    {make_handle(deleted_bitmap), error_for_non_null},
                };

                for (const auto& [target, expected_error] : cases)
                {
                    dimension out{};
                    this->reset_error();
                    EXPECT_EQ(FALSE, this->get(target, out)) << target.bits;
                    EXPECT_EQ((dimension{.width = 0x1111, .height = 0x1111}), out);
                    EXPECT_EQ(expected_error, this->last_error()) << target.bits;
                    this->expect_wow64_error_untouched(wow64);

                    this->reset_error();
                    EXPECT_EQ(FALSE, this->set(target, {.width = 5, .height = 6}, out)) << target.bits;
                    EXPECT_EQ((dimension{.width = 0x1111, .height = 0x1111}), out);
                    EXPECT_EQ(expected_error, this->last_error()) << target.bits;
                    this->expect_wow64_error_untouched(wow64);
                }
            }

            void expect_wow64_error_untouched(const bool wow64)
            {
                if (wow64)
                {
                    EXPECT_EQ(k_untouched_error, this->last_error32());
                }
            }

            windows_emulator emu = create_sample_emulator();
            handle bitmap{};
            emulator_pointer scratch{};
        };
    }

    TEST_F(GdiBitmapDimensionTest, SetAndGetFollowTheCapturedSequence)
    {
        const std::vector<dimension_step> steps{
            {.set = {.width = 10, .height = 20}, .previous = {.width = 0, .height = 0}},
            {.set = {.width = 30, .height = 40}, .previous = {.width = 10, .height = 20}},
            {.set = {.width = -5, .height = -7}, .previous = {.width = 30, .height = 40}},
            {.set = {.width = 0, .height = 0}, .previous = {.width = -5, .height = -7}},
            {.set = {.width = INT_MAX, .height = INT_MIN}, .previous = {.width = 0, .height = 0}},
            {.set = {.width = 12, .height = 0}, .previous = {.width = INT_MAX, .height = INT_MIN}},
        };

        EXPECT_EQ((dimension{.width = 0, .height = 0}), this->current(this->bitmap));

        for (const auto& step : steps)
        {
            dimension previous{};
            EXPECT_EQ(TRUE, this->set(this->bitmap, step.set, previous));
            EXPECT_EQ(step.previous, previous);
            EXPECT_EQ(step.set, this->current(this->bitmap));
        }

        EXPECT_EQ(TRUE, syscalls::handle_NtGdiSetBitmapDimension(this->context(), this->bitmap, 1, 2, 0));
        EXPECT_EQ((dimension{.width = 1, .height = 2}), this->current(this->bitmap));

        constexpr emulator_pointer unmapped = 1;
        const dimension size{.width = 7, .height = 9};
        EXPECT_EQ(FALSE, syscalls::handle_NtGdiGetBitmapDimension(this->context(), this->bitmap, 0));
        EXPECT_EQ(FALSE, syscalls::handle_NtGdiGetBitmapDimension(this->context(), this->bitmap, unmapped));
        EXPECT_EQ(FALSE, syscalls::handle_NtGdiSetBitmapDimension(this->context(), this->bitmap, size.width, size.height, unmapped));
        EXPECT_EQ(size, this->current(this->bitmap));
        EXPECT_EQ(k_untouched_error, this->last_error());
    }

    TEST_F(GdiBitmapDimensionTest, InvalidHandlesFailWithInvalidHandleExceptNull)
    {
        this->expect_invalid_handles_fail_with(k_error_invalid_handle);
    }

    TEST_F(GdiBitmapDimensionTest, InvalidHandleSetsOnlyThe64BitLastErrorForWow64Guests)
    {
        this->attach_wow64_teb();
        this->expect_invalid_handles_fail_with(k_error_invalid_handle, true);
    }
}

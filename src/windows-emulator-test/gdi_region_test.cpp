#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreateRectRgn(const syscall_context& c, LONG x_left, LONG y_top, LONG x_right, LONG y_bottom);
    int32_t handle_NtGdiCombineRgn(const syscall_context& c, handle destination, handle first_source, handle second_source, int32_t mode);
    BOOL handle_NtGdiEqualRgn(const syscall_context& c, handle first_region, handle second_region);
    uint32_t handle_NtGdiGetRegionData(const syscall_context& c, handle hrgn, ULONG buffer_size, emulator_pointer region_data);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG k_region_complexity_offset = 4;
        constexpr ULONG k_region_bounds_offset = 8;
        constexpr size_t k_storage_size = 0x10000;
        constexpr DWORD k_error_invalid_handle = 6;
        constexpr DWORD k_untouched_last_error = 0xDEAD;

        using box = std::array<LONG, 4>;
        using box_list = std::vector<box>;

        box to_box(const RECT& rect)
        {
            return {rect.left, rect.top, rect.right, rect.bottom};
        }

        bool contains_point(const box& rect, const LONG x, const LONG y)
        {
            return rect[0] <= x && x < rect[2] && rect[1] <= y && y < rect[3];
        }

        bool covers_point(const box_list& rects, const LONG x, const LONG y)
        {
            return std::ranges::any_of(rects, [&](const box& rect) { return contains_point(rect, x, y); });
        }

        bool mode_includes(const int32_t mode, const bool in_first, const bool in_second)
        {
            switch (mode)
            {
            case RGN_AND:
                return in_first && in_second;
            case RGN_OR:
                return in_first || in_second;
            case RGN_XOR:
                return in_first != in_second;
            default:
                return in_first && !in_second;
            }
        }

        class GdiRegionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(k_storage_size, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            DWORD last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void reset_last_error()
            {
                this->emu.vcpu(0).active_thread->teb64->access([](TEB64& teb) { teb.LastErrorValue = k_untouched_last_error; });
            }

            handle create(const LONG left, const LONG top, const LONG right, const LONG bottom)
            {
                const auto region = syscalls::handle_NtGdiCreateRectRgn(this->context(), left, top, right, bottom);
                EXPECT_NE(0u, region);
                return make_handle(region);
            }

            int32_t combine(const handle destination, const handle first, const handle second, const int32_t mode)
            {
                this->reset_last_error();
                const auto result = syscalls::handle_NtGdiCombineRgn(this->context(), destination, first, second, mode);
                if (result != RGN_ERROR)
                {
                    EXPECT_EQ(k_untouched_last_error, this->last_error());
                }
                return result;
            }

            uint32_t region_data(const handle region, const ULONG size, const uint64_t buffer)
            {
                this->reset_last_error();
                return syscalls::handle_NtGdiGetRegionData(this->context(), region, size, buffer);
            }

            uint64_t attribute(const handle region)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                const auto index = static_cast<uint32_t>(region.bits) & 0xFFFF;
                return this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + (index * sizeof(GDI_HANDLE_ENTRY64))).Object;
            }

            ULONG complexity(const handle region)
            {
                return this->emu.memory.read_memory<ULONG>(this->attribute(region) + k_region_complexity_offset);
            }

            box bounds(const handle region)
            {
                return to_box(this->emu.memory.read_memory<RECT>(this->attribute(region) + k_region_bounds_offset));
            }

            box_list rects(const handle region)
            {
                const auto required = this->region_data(region, 0, 0);
                EXPECT_GE(required, sizeof(RGNDATAHEADER));
                EXPECT_LE(required, k_storage_size);

                EXPECT_EQ(required, this->region_data(region, required, this->storage));
                const auto header = this->emu.memory.read_memory<RGNDATAHEADER>(this->storage);
                EXPECT_EQ(sizeof(RGNDATAHEADER), header.dwSize);
                EXPECT_EQ(static_cast<DWORD>(RDH_RECTANGLES), header.iType);
                EXPECT_EQ(header.nCount * sizeof(RECT), header.nRgnSize);
                EXPECT_EQ(sizeof(RGNDATAHEADER) + header.nRgnSize, required);

                box_list result{};
                for (DWORD i = 0; i < header.nCount; ++i)
                {
                    result.push_back(
                        to_box(this->emu.memory.read_memory<RECT>(this->storage + sizeof(RGNDATAHEADER) + (i * sizeof(RECT)))));
                }
                return result;
            }

            // The expected results in these tests were captured on Windows 11 (build 26200) for the same inputs.
            void expect_region(const handle region, const int32_t expected_complexity, const box& expected_bounds,
                               const box_list& expected_rects)
            {
                EXPECT_EQ(static_cast<ULONG>(expected_complexity), this->complexity(region));
                EXPECT_EQ(expected_bounds, this->bounds(region));

                const auto required = static_cast<uint32_t>(sizeof(RGNDATAHEADER) + (expected_rects.size() * sizeof(RECT)));
                EXPECT_EQ(required, this->region_data(region, 0, 0));
                EXPECT_EQ(k_untouched_last_error, this->last_error());

                EXPECT_EQ(0u, this->region_data(region, required - 1, this->storage));
                EXPECT_EQ(k_error_invalid_parameter, this->last_error());

                EXPECT_EQ(required, this->region_data(region, required + 64, this->storage));
                EXPECT_EQ(k_untouched_last_error, this->last_error());

                EXPECT_EQ(required, this->region_data(region, required, this->storage));
                EXPECT_EQ(k_untouched_last_error, this->last_error());
                const auto header = this->emu.memory.read_memory<RGNDATAHEADER>(this->storage);
                EXPECT_EQ(32u, header.dwSize);
                EXPECT_EQ(static_cast<DWORD>(RDH_RECTANGLES), header.iType);
                EXPECT_EQ(expected_rects.size(), header.nCount);
                EXPECT_EQ(expected_rects.size() * sizeof(RECT), header.nRgnSize);
                EXPECT_EQ(expected_bounds, to_box(header.rcBound));

                EXPECT_EQ(expected_rects, this->rects(region));
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t storage{};
        };
    }

    TEST_F(GdiRegionTest, CreateRectRgnWritesNormalizedRegionAttribute)
    {
        this->expect_region(this->create(30, 40, 10, 20), SIMPLEREGION, {10, 20, 30, 40}, {{10, 20, 30, 40}});
        this->expect_region(this->create(5, 5, 5, 9), NULLREGION, {}, {});
    }

    TEST_F(GdiRegionTest, RectangleWrittenToTheAttributeByUserModeIsUsed)
    {
        const auto region = this->create(0, 0, 1, 1);
        const auto attr = this->attribute(region);
        this->emu.memory.write_memory(attr + k_region_complexity_offset, static_cast<ULONG>(SIMPLEREGION));
        this->emu.memory.write_memory(attr + k_region_bounds_offset, RECT{2, 3, 40, 50});

        EXPECT_EQ((box_list{{2, 3, 40, 50}}), this->rects(region));
    }

    TEST_F(GdiRegionTest, CombineRgnProducesBandedRectangles)
    {
        const auto first = this->create(0, 0, 10, 10);
        const auto second = this->create(5, 5, 15, 15);
        const auto result = this->create(0, 0, 0, 0);

        EXPECT_EQ(SIMPLEREGION, this->combine(result, first, second, RGN_AND));
        this->expect_region(result, SIMPLEREGION, {5, 5, 10, 10}, {{5, 5, 10, 10}});

        EXPECT_EQ(COMPLEXREGION, this->combine(result, first, second, RGN_OR));
        this->expect_region(result, COMPLEXREGION, {0, 0, 15, 15}, {{0, 0, 10, 5}, {0, 5, 15, 10}, {5, 10, 15, 15}});

        EXPECT_EQ(COMPLEXREGION, this->combine(result, first, second, RGN_XOR));
        this->expect_region(result, COMPLEXREGION, {0, 0, 15, 15}, {{0, 0, 10, 5}, {0, 5, 5, 10}, {10, 5, 15, 10}, {5, 10, 15, 15}});

        EXPECT_EQ(COMPLEXREGION, this->combine(result, first, second, RGN_DIFF));
        this->expect_region(result, COMPLEXREGION, {0, 0, 10, 10}, {{0, 0, 10, 5}, {0, 5, 5, 10}});

        EXPECT_EQ(SIMPLEREGION, this->combine(result, second, make_handle(uint64_t{0}), RGN_COPY));
        this->expect_region(result, SIMPLEREGION, {5, 5, 15, 15}, {{5, 5, 15, 15}});

        EXPECT_EQ(SIMPLEREGION, this->combine(result, second, make_handle(uint64_t{0x12345}), RGN_COPY));
        this->expect_region(result, SIMPLEREGION, {5, 5, 15, 15}, {{5, 5, 15, 15}});
    }

    TEST_F(GdiRegionTest, CombineRgnMergesTouchingRectanglesAndReportsNullRegions)
    {
        const auto left = this->create(0, 0, 5, 10);
        const auto right = this->create(5, 0, 10, 10);
        const auto below = this->create(0, 10, 10, 20);
        const auto distant = this->create(50, 50, 60, 60);
        const auto result = this->create(0, 0, 0, 0);

        EXPECT_EQ(SIMPLEREGION, this->combine(result, left, right, RGN_OR));
        this->expect_region(result, SIMPLEREGION, {0, 0, 10, 10}, {{0, 0, 10, 10}});

        EXPECT_EQ(SIMPLEREGION, this->combine(result, result, below, RGN_OR));
        this->expect_region(result, SIMPLEREGION, {0, 0, 10, 20}, {{0, 0, 10, 20}});

        EXPECT_EQ(NULLREGION, this->combine(result, left, distant, RGN_AND));
        this->expect_region(result, NULLREGION, {}, {});

        EXPECT_EQ(NULLREGION, this->combine(result, left, left, RGN_DIFF));
        this->expect_region(result, NULLREGION, {}, {});
    }

    TEST_F(GdiRegionTest, CombineRgnRejectsInvalidModeAndHandles)
    {
        const auto first = this->create(0, 0, 10, 10);
        const auto second = this->create(5, 5, 15, 15);
        const auto keep = this->create(1, 2, 3, 4);

        for (const auto mode : {0, RGN_COPY + 1})
        {
            EXPECT_EQ(RGN_ERROR, this->combine(keep, first, second, mode));
            EXPECT_EQ(k_error_invalid_parameter, this->last_error());
            this->expect_region(keep, SIMPLEREGION, {1, 2, 3, 4}, {{1, 2, 3, 4}});
        }

        EXPECT_EQ(RGN_ERROR, this->combine(keep, first, make_handle(uint64_t{0x12345}), RGN_OR));
        EXPECT_EQ(k_error_invalid_handle, this->last_error());
        this->expect_region(keep, SIMPLEREGION, {1, 2, 3, 4}, {{1, 2, 3, 4}});

        EXPECT_EQ(RGN_ERROR, this->combine(make_handle(uint64_t{0x12345}), first, second, RGN_OR));
        EXPECT_EQ(k_error_invalid_handle, this->last_error());
    }

    TEST_F(GdiRegionTest, EqualRgnComparesCoveredArea)
    {
        const auto first = this->create(0, 0, 10, 10);
        const auto second = this->create(5, 5, 15, 15);
        const auto united = this->create(0, 0, 0, 0);
        const auto rebuilt = this->create(0, 0, 0, 0);
        const auto overlap = this->create(0, 0, 0, 0);

        ASSERT_EQ(COMPLEXREGION, this->combine(united, first, second, RGN_OR));
        ASSERT_EQ(COMPLEXREGION, this->combine(rebuilt, first, second, RGN_XOR));
        ASSERT_EQ(SIMPLEREGION, this->combine(overlap, first, second, RGN_AND));
        ASSERT_EQ(COMPLEXREGION, this->combine(rebuilt, rebuilt, overlap, RGN_OR));

        EXPECT_EQ(TRUE, syscalls::handle_NtGdiEqualRgn(this->context(), united, rebuilt));
        EXPECT_EQ(FALSE, syscalls::handle_NtGdiEqualRgn(this->context(), united, first));
        EXPECT_EQ(FALSE, syscalls::handle_NtGdiEqualRgn(this->context(), first, second));
        EXPECT_EQ(TRUE, syscalls::handle_NtGdiEqualRgn(this->context(), this->create(1, 1, 1, 5), this->create(7, 7, 9, 7)));
    }

    TEST_F(GdiRegionTest, ComplexRegionSurvivesSnapshotAndIsReleasedOnDelete)
    {
        const auto first = this->create(0, 0, 10, 10);
        const auto second = this->create(5, 5, 15, 15);
        ASSERT_EQ(COMPLEXREGION, this->combine(first, first, second, RGN_OR));

        utils::buffer_serializer snapshot{};
        this->emu.serialize(snapshot);
        ASSERT_EQ(COMPLEXREGION, this->combine(first, first, first, RGN_COPY));
        ASSERT_EQ(SIMPLEREGION, this->combine(first, second, second, RGN_COPY));
        utils::buffer_deserializer input{snapshot};
        this->emu.deserialize(input);

        EXPECT_EQ((box_list{{0, 0, 10, 5}, {0, 5, 15, 10}, {5, 10, 15, 15}}), this->rects(first));

        EXPECT_EQ(1u, syscalls::handle_NtGdiDeleteObjectApp(this->context(), static_cast<uint32_t>(first.bits)));
        EXPECT_EQ(0u, syscalls::handle_NtGdiGetRegionData(this->context(), first, 0, 0));
        EXPECT_TRUE(this->emu.process.gdi_regions.empty());
    }

    TEST_F(GdiRegionTest, AttributeRewrittenToSimpleByUserModeReplacesComplexRectangles)
    {
        const auto region = this->create(0, 0, 10, 10);
        ASSERT_EQ(COMPLEXREGION, this->combine(region, region, this->create(5, 5, 15, 15), RGN_OR));

        const auto attr = this->attribute(region);
        this->emu.memory.write_memory(attr + k_region_complexity_offset, static_cast<ULONG>(SIMPLEREGION));
        this->emu.memory.write_memory(attr + k_region_bounds_offset, RECT{20, 20, 30, 30});

        EXPECT_EQ((box_list{{20, 20, 30, 30}}), this->rects(region));
        EXPECT_EQ(TRUE, syscalls::handle_NtGdiEqualRgn(this->context(), region, this->create(20, 20, 30, 30)));
    }

    TEST_F(GdiRegionTest, CombineRgnOfManyRectanglesMatchesPointwiseResult)
    {
        constexpr LONG k_extent = 64;
        constexpr LONG k_max_size = 8;
        constexpr int k_rects_per_region = 40;

        uint32_t seed = 1;
        const auto next = [&](const LONG limit) {
            seed = (seed * 1103515245u) + 12345u;
            return static_cast<LONG>((seed >> 16) % static_cast<uint32_t>(limit));
        };

        const auto build = [&](box_list& sources) {
            const auto region = this->create(0, 0, 0, 0);
            for (int i = 0; i < k_rects_per_region; ++i)
            {
                const auto left = next(k_extent - k_max_size);
                const auto top = next(k_extent - k_max_size);
                const box source{left, top, left + 1 + next(k_max_size), top + 1 + next(k_max_size)};
                sources.push_back(source);
                EXPECT_NE(RGN_ERROR, this->combine(region, region, this->create(source[0], source[1], source[2], source[3]), RGN_OR));
            }
            return region;
        };

        box_list first_sources{};
        box_list second_sources{};
        const auto first = build(first_sources);
        const auto second = build(second_sources);
        ASSERT_EQ(static_cast<ULONG>(COMPLEXREGION), this->complexity(first));
        ASSERT_EQ(static_cast<ULONG>(COMPLEXREGION), this->complexity(second));

        const auto result = this->create(0, 0, 0, 0);
        for (const auto mode : {RGN_AND, RGN_OR, RGN_XOR, RGN_DIFF})
        {
            ASSERT_NE(RGN_ERROR, this->combine(result, first, second, mode));
            const auto combined = this->rects(result);

            for (size_t i = 1; i < combined.size(); ++i)
            {
                const auto& previous = combined[i - 1];
                const auto& current = combined[i];
                const auto same_band = current[1] == previous[1] && current[3] == previous[3] && current[0] > previous[2];
                EXPECT_TRUE(same_band || current[1] >= previous[3]) << "mode " << mode << ", rect " << i;
            }

            for (LONG y = 0; y < k_extent; ++y)
            {
                for (LONG x = 0; x < k_extent; ++x)
                {
                    const auto expected = mode_includes(mode, covers_point(first_sources, x, y), covers_point(second_sources, x, y));
                    const auto hits = std::ranges::count_if(combined, [&](const box& rect) { return contains_point(rect, x, y); });
                    ASSERT_EQ(expected ? 1 : 0, hits) << "mode " << mode << " at " << x << "," << y;
                }
            }
        }
    }
}

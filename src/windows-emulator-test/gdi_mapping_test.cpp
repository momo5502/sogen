#include "std_include.hpp"

#include <gdi_mapping.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    BOOL handle_NtGdiGetAndSetDCDword(const syscall_context& c, hdc dc, uint32_t method, uint32_t value, emulator_pointer result);
    BOOL handle_NtGdiTransformPoints(const syscall_context& c, hdc dc, emulator_pointer source, emulator_pointer destination, int32_t count,
                                     uint32_t mode);
    BOOL handle_NtGdiScaleWindowExtEx(const syscall_context& c, hdc dc, int32_t x_numerator, int32_t x_denominator, int32_t y_numerator,
                                      int32_t y_denominator, emulator_pointer previous_extent);
    BOOL handle_NtGdiScaleViewportExtEx(const syscall_context& c, hdc dc, int32_t x_numerator, int32_t x_denominator, int32_t y_numerator,
                                        int32_t y_denominator, emulator_pointer previous_extent);
}

namespace sogen::test
{
    namespace
    {
        using gdi::dc_mapping;
        using gdi::mapping_vec;

        constexpr int32_t k_int32_min = std::numeric_limits<int32_t>::min();
        constexpr uint32_t k_set_map_mode_method = 8;
        constexpr uint64_t k_dc_attr_map_mode = 0x68;
        constexpr uint64_t k_dc_attr_window_org_x = 0x130;
        constexpr uint64_t k_dc_attr_window_org_y = 0x138;
        constexpr uint64_t k_dc_attr_window_ext = 0x13C;
        constexpr uint64_t k_dc_attr_viewport_org = 0x144;
        constexpr uint64_t k_dc_attr_viewport_ext = 0x14C;
        constexpr mapping_vec k_untouched{static_cast<int32_t>(0xCCCCCCCCu), static_cast<int32_t>(0xCCCCCCCCu)};
        constexpr mapping_vec k_scale_window_ext{100, 50};
        constexpr mapping_vec k_scale_viewport_ext{200, 100};

        struct map_mode_sequence
        {
            uint32_t mode{};
            std::vector<uint32_t> previous_modes{};
            std::vector<dc_mapping> states{};
        };

        struct conversion_row
        {
            uint32_t mode{};
            mapping_vec in{};
            mapping_vec out{};
        };

        struct conversion_group
        {
            std::string_view name{};
            int32_t map_mode{};
            mapping_vec window_org{};
            mapping_vec viewport_org{};
            mapping_vec window_ext{};
            mapping_vec viewport_ext{};
            std::vector<conversion_row> rows{};
        };

        struct scale_row
        {
            int32_t map_mode{};
            bool window{};
            std::array<int32_t, 4> factors{};
            BOOL result{};
            mapping_vec previous{};
            mapping_vec window_ext{};
            mapping_vec viewport_ext{};
            mapping_vec scaled_start{};
        };

        // Rows are copied from a Windows 11 26300 capture on a 1024x768, 96 dpi display.
        // NOLINTBEGIN(modernize-use-designated-initializers)
        // clang-format off
        const std::vector<map_mode_sequence> k_map_mode_sequences{
            {0, {0, 0, 1, 0}, {
                {1, {0, 0}, {0, 0}, {1, 1}, {1, 1}}, {1, {0, 0}, {0, 0}, {1, 1}, {1, 1}},
                {1, {0, 0}, {0, 0}, {1, 1}, {1, 1}}, {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}}},
            {2, {1, 2, 2, 1}, {
                {2, {0, 0}, {0, 0}, {2709, 2032}, {1024, -768}}, {2, {0, 0}, {0, 0}, {2709, 2032}, {1024, -768}},
                {2, {0, 0}, {0, 0}, {2709, 2032}, {1024, -768}}, {2, {7, 9}, {-3, 5}, {2709, 2032}, {1024, -768}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {2, {7, 9}, {-3, 5}, {2709, 2032}, {1024, -768}}}},
            {3, {1, 3, 3, 1}, {
                {3, {0, 0}, {0, 0}, {27093, 20320}, {1024, -768}}, {3, {0, 0}, {0, 0}, {27093, 20320}, {1024, -768}},
                {3, {0, 0}, {0, 0}, {27093, 20320}, {1024, -768}}, {3, {7, 9}, {-3, 5}, {27093, 20320}, {1024, -768}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {3, {7, 9}, {-3, 5}, {27093, 20320}, {1024, -768}}}},
            {4, {1, 4, 4, 1}, {
                {4, {0, 0}, {0, 0}, {1067, 800}, {1024, -768}}, {4, {0, 0}, {0, 0}, {1067, 800}, {1024, -768}},
                {4, {0, 0}, {0, 0}, {1067, 800}, {1024, -768}}, {4, {7, 9}, {-3, 5}, {1067, 800}, {1024, -768}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {4, {7, 9}, {-3, 5}, {1067, 800}, {1024, -768}}}},
            {5, {1, 5, 5, 1}, {
                {5, {0, 0}, {0, 0}, {10667, 8000}, {1024, -768}}, {5, {0, 0}, {0, 0}, {10667, 8000}, {1024, -768}},
                {5, {0, 0}, {0, 0}, {10667, 8000}, {1024, -768}}, {5, {7, 9}, {-3, 5}, {10667, 8000}, {1024, -768}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {5, {7, 9}, {-3, 5}, {10667, 8000}, {1024, -768}}}},
            {6, {1, 6, 6, 1}, {
                {6, {0, 0}, {0, 0}, {15360, 11520}, {1024, -768}}, {6, {0, 0}, {0, 0}, {15360, 11520}, {1024, -768}},
                {6, {0, 0}, {0, 0}, {15360, 11520}, {1024, -768}}, {6, {7, 9}, {-3, 5}, {15360, 11520}, {1024, -768}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {6, {7, 9}, {-3, 5}, {15360, 11520}, {1024, -768}}}},
            {7, {1, 7, 7, 1}, {
                {7, {0, 0}, {0, 0}, {2709, 2032}, {1024, -768}}, {7, {0, 0}, {0, 0}, {2709, 2032}, {1024, -768}},
                {7, {0, 0}, {0, 0}, {200, 100}, {100, 50}}, {7, {7, 9}, {-3, 5}, {200, 100}, {100, 50}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {7, {7, 9}, {-3, 5}, {2709, 2032}, {1024, -768}}}},
            {8, {1, 8, 8, 1}, {
                {8, {0, 0}, {0, 0}, {1, 1}, {1, 1}}, {8, {0, 0}, {0, 0}, {1, 1}, {1, 1}},
                {8, {0, 0}, {0, 0}, {200, 100}, {100, 50}}, {8, {7, 9}, {-3, 5}, {200, 100}, {100, 50}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {8, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}}},
            {9, {0, 0, 1, 0}, {
                {1, {0, 0}, {0, 0}, {1, 1}, {1, 1}}, {1, {0, 0}, {0, 0}, {1, 1}, {1, 1}},
                {1, {0, 0}, {0, 0}, {1, 1}, {1, 1}}, {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}},
                {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}, {1, {7, 9}, {-3, 5}, {1, 1}, {1, 1}}}},
        };

        const std::vector<conversion_group> k_conversion_groups{
            {"mapmode_0x2.N/after_set", 2, {0, 0}, {0, 0}, {}, {}, {
                {1, {10, 20}, {4, -8}}, {0, {10, 20}, {26, -53}}, {1, {-10, -20}, {-4, 8}}, {0, {-10, -20}, {-26, 53}},
                {1, {65536, 65536}, {24773, -24769}}}},
            {"mapmode_0x2.N/after_set_again2", 2, {7, 9}, {-3, 5}, {}, {}, {
                {1, {10, 20}, {-2, 1}}, {0, {10, 20}, {41, -31}}, {1, {-10, -20}, {-9, 16}}, {0, {-10, -20}, {-11, 75}},
                {1, {100, 100}, {32, -29}}, {0, {100, 100}, {280, -243}}}},
            {"mapmode_0x8.N/after_orgs", 8, {7, 9}, {-3, 5}, {200, 100}, {100, 50}, {
                {1, {0, 0}, {-6, 1}}, {1, {10, 20}, {-1, 11}}, {0, {10, 20}, {33, 39}}, {1, {-10, -20}, {-11, -9}},
                {0, {-10, -20}, {-7, -41}}}},
            {"orgs_anisotropic.N/v2_-5,-7", 8, {-5, -7}, {-5, -7}, {100, 100}, {200, 100}, {
                {0, {0, 0}, {-3, 0}}, {1, {10, 20}, {25, 20}}, {0, {10, 20}, {2, 20}}, {1, {-10, -20}, {-15, -20}},
                {0, {-10, -20}, {-8, -20}}, {0, {100, 100}, {47, 100}}}},
            {"mapmode_0x1.N/after_set_again2", 1, {7, 9}, {-3, 5}, {}, {}, {
                {1, {10, 20}, {0, 16}}, {0, {10, 20}, {20, 24}}, {1, {-10, -20}, {-20, -24}}, {0, {-10, -20}, {0, -16}}}},
            {"extgrid_anisotropic_wnd_first.N/t02_w-200,100_v100,50", 8, {0, 0}, {0, 0}, {-200, 100}, {100, 50}, {
                {1, {10, 20}, {-5, 10}}, {0, {10, 20}, {-20, 40}}, {1, {-10, -20}, {5, -10}}, {0, {-10, -20}, {20, -40}}}},
            {"iso_mem64.N/s2_order0", 7, {0, 0}, {0, 0}, {100, 50}, {200, 200}, {
                {1, {10, 20}, {20, 40}}, {0, {10, 20}, {5, 10}}, {1, {-10, -20}, {-20, -40}}, {0, {-10, -20}, {-5, -10}}}},
            {"xpts_text_mode.N", 1, {0, 0}, {0, 0}, {}, {}, {
                {2, {10, 20}, {160, 320}}, {2, {-5, 7}, {-80, 112}}, {0x80000000, {10, 20}, {10, 20}}, {0x80000002, {10, 20}, {160, 320}}}},
            {"xpts_lometric_orgs_mode.N", 2, {7, -3}, {20, 30}, {}, {}, {
                {1, {10, 20}, {21, 21}}, {2, {10, 20}, {338, 341}}, {2, {-5, 7}, {248, 420}}, {2, {1000, 2000}, {6326, -11632}}}},
            {"xpts_aniso_scale_mode.N", 8, {3, 5}, {11, 13}, {4, 2}, {6, -3}, {
                {1, {10, 20}, {22, -9}}, {2, {10, 20}, {344, -152}}, {2, {1000, 2000}, {24104, -47672}}}},
        };

        const std::vector<scale_row> k_scale_rows{
            {2, false, {2, 1, 1, 2}, 1, {1024, -768}, {2709, 2032}, {1024, -768}},
            {2, true, {0, 1, 1, 1}, 1, {2709, 2032}, {2709, 2032}, {1024, -768}},
            {7, false, {0, 1, 1, 1}, 0, {200, 100}, {100, 50}, {200, 100}},
            {7, true, {1, 0, 1, 1}, 0, k_untouched, {100, 50}, {200, 100}},
            {8, true, {k_int32_min, 1, 1, 1}, 0, k_untouched, {100, 50}, {200, 100}},
            {8, true, {7, 3, 5, 2}, 1, {100, 50}, {233, 125}, {200, 100}},
            {8, true, {1, 2, 3, 4}, 1, {100, 50}, {50, 37}, {200, 100}},
            {8, false, {-1, 1, 1, 1}, 1, {200, 100}, {100, 50}, {-200, 100}},
            {8, false, {2147483647, 1, 1, 1}, 1, {200, 100}, {100, 50}, {-200, 100}},
            {7, true, {2, 1, 1, 2}, 1, {100, 50}, {200, 25}, {200, 25}},
            {7, false, {7, 3, 5, 2}, 1, {200, 100}, {100, 50}, {466, 233}},
            {8, true, {65536, 65536, 1, 1}, 0, k_untouched, {65536, 100}, {200, 100}, {65536, 100}},
            {8, true, {1, -1, 1, 1}, 0, k_untouched, {k_int32_min, 100}, {200, 100}, {k_int32_min, 100}},
            {8, true, {-1, 1, 1, 1}, 1, {k_int32_min, 100}, {k_int32_min, 100}, {200, 100}, {k_int32_min, 100}},
            {8, true, {100000, 100000, 1, 1}, 1, {100000, 100}, {14100, 100}, {200, 100}, {100000, 100}},
            {8, false, {1, 1, 65536, 65536}, 0, {100, 65536}, {100, 50}, {100, 65536}, {100, 65536}},
        };
        // clang-format on
        // NOLINTEND(modernize-use-designated-initializers)

        class GdiMappingTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->emu.process.display_width = 1024;
                this->emu.process.display_height = 768;
                syscalls::handle_NtGdiInit(this->context());
                this->scratch = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            uint64_t dc_attr(const hdc dc)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                const auto index = static_cast<uint32_t>(dc) & 0xFFFF;
                return this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + (index * sizeof(GDI_HANDLE_ENTRY64))).Object;
            }

            template <typename T>
            T peek(const hdc dc, const uint64_t offset)
            {
                return this->emu.memory.read_memory<T>(this->dc_attr(dc) + offset);
            }

            template <typename T>
            void poke(const hdc dc, const uint64_t offset, const T& value)
            {
                this->emu.memory.write_memory(this->dc_attr(dc) + offset, &value, sizeof(value));
            }

            hdc create_dc()
            {
                const auto dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                EXPECT_NE(0u, dc);
                return dc;
            }

            // gdi32 keeps the origins in DC_ATTR, and the extents too outside the fixed map modes, without calling the kernel.
            void set_origins(const hdc dc, const mapping_vec& window, const mapping_vec& viewport)
            {
                this->poke(dc, k_dc_attr_window_org_x, std::array<int32_t, 3>{window.x, window.x, window.y});
                this->poke(dc, k_dc_attr_viewport_org, viewport);
            }

            void set_extent(const hdc dc, const uint64_t offset, const mapping_vec& extent)
            {
                if (this->peek<int32_t>(dc, k_dc_attr_map_mode) > gdi::k_map_mode_twips && extent.x != 0 && extent.y != 0)
                {
                    this->poke(dc, offset, extent);
                }
            }

            dc_mapping mapping(const hdc dc)
            {
                return {this->peek<int32_t>(dc, k_dc_attr_map_mode),
                        {this->peek<int32_t>(dc, k_dc_attr_window_org_x), this->peek<int32_t>(dc, k_dc_attr_window_org_y)},
                        this->peek<mapping_vec>(dc, k_dc_attr_viewport_org),
                        this->peek<mapping_vec>(dc, k_dc_attr_window_ext),
                        this->peek<mapping_vec>(dc, k_dc_attr_viewport_ext)};
            }

            BOOL set_map_mode(const hdc dc, const uint32_t mode, uint32_t& previous)
            {
                const auto result = this->scratch;
                previous = 0xCCCCCCCC;
                this->emu.memory.write_memory(result, &previous, sizeof(previous));
                const auto ret = syscalls::handle_NtGdiGetAndSetDCDword(this->context(), dc, k_set_map_mode_method, mode, result);
                this->emu.memory.read_memory(result, &previous, sizeof(previous));
                return ret;
            }

            hdc dc_in_mode(const int32_t mode, const mapping_vec& window_ext = {}, const mapping_vec& viewport_ext = {},
                           const mapping_vec& window_org = {}, const mapping_vec& viewport_org = {})
            {
                const auto dc = this->create_dc();
                uint32_t previous{};
                EXPECT_EQ(TRUE, this->set_map_mode(dc, static_cast<uint32_t>(mode), previous));
                this->set_extent(dc, k_dc_attr_window_ext, window_ext);
                this->set_extent(dc, k_dc_attr_viewport_ext, viewport_ext);
                this->set_origins(dc, window_org, viewport_org);
                return dc;
            }

            BOOL transform(const hdc dc, const uint32_t mode, std::vector<mapping_vec>& points)
            {
                const auto size = points.size() * sizeof(mapping_vec);
                const auto buffer = this->scratch + 0x100;
                this->emu.memory.write_memory(buffer, points.data(), size);
                const auto ret =
                    syscalls::handle_NtGdiTransformPoints(this->context(), dc, buffer, buffer, static_cast<int32_t>(points.size()), mode);
                this->emu.memory.read_memory(buffer, points.data(), size);
                return ret;
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t scratch{};
        };
    }

    TEST_F(GdiMappingTest, SetMapModeReplaysTheCapturedSequences)
    {
        EXPECT_EQ((dc_mapping{1, {0, 0}, {0, 0}, {1, 1}, {1, 1}}), this->mapping(this->create_dc()));

        for (const auto& sequence : k_map_mode_sequences)
        {
            SCOPED_TRACE(sequence.mode);
            const auto dc = this->create_dc();
            auto result = sequence.previous_modes.begin();
            auto expected = sequence.states.begin();
            const auto apply_map_mode = [&](const uint32_t mode) {
                uint32_t previous{};
                EXPECT_EQ(TRUE, this->set_map_mode(dc, mode, previous));
                EXPECT_EQ(*result++, previous);
                EXPECT_EQ(*expected++, this->mapping(dc));
            };

            apply_map_mode(sequence.mode);
            apply_map_mode(sequence.mode);
            this->set_extent(dc, k_dc_attr_window_ext, {200, 100});
            this->set_extent(dc, k_dc_attr_viewport_ext, {100, 50});
            EXPECT_EQ(*expected++, this->mapping(dc));
            this->set_origins(dc, {7, 9}, {-3, 5});
            EXPECT_EQ(*expected++, this->mapping(dc));
            apply_map_mode(static_cast<uint32_t>(gdi::k_map_mode_text));
            apply_map_mode(sequence.mode);
        }
    }

    TEST_F(GdiMappingTest, TransformPointsReproducesTheCapturedConversions)
    {
        for (const auto& group : k_conversion_groups)
        {
            SCOPED_TRACE(group.name);
            const auto dc = this->dc_in_mode(group.map_mode, group.window_ext, group.viewport_ext, group.window_org, group.viewport_org);
            for (const auto& row : group.rows)
            {
                SCOPED_TRACE(::testing::Message() << "mode " << row.mode << " " << row.in.x << "," << row.in.y);
                std::vector<mapping_vec> points{row.in};
                ASSERT_EQ(TRUE, this->transform(dc, row.mode, points));
                EXPECT_EQ(row.out, points.front());
            }
        }

        const auto dc = this->dc_in_mode(gdi::k_map_mode_lometric);
        std::vector<mapping_vec> points{{10, 20}, {-5, 7}, {100, 100}};
        ASSERT_EQ(TRUE, this->transform(dc, 1, points));
        EXPECT_EQ((std::vector<mapping_vec>{{4, -8}, {-2, -3}, {38, -38}}), points);
        points = {{10, 20}, {-5, 7}, {100, 100}};
        ASSERT_EQ(TRUE, this->transform(dc, 0, points));
        EXPECT_EQ((std::vector<mapping_vec>{{26, -53}, {-13, -19}, {265, -265}}), points);

        const auto buffer = this->scratch + 0x100;
        this->emu.memory.write_memory(buffer, &k_untouched, sizeof(k_untouched));
        for (const uint32_t mode : {0u, 1u, 2u})
        {
            for (const int32_t count : {0, -1})
            {
                EXPECT_EQ(TRUE, syscalls::handle_NtGdiTransformPoints(this->context(), dc, buffer, buffer, count, mode));
                EXPECT_EQ(TRUE, syscalls::handle_NtGdiTransformPoints(this->context(), dc, 0, 0, count, mode));
            }
            EXPECT_EQ(FALSE, syscalls::handle_NtGdiTransformPoints(this->context(), dc, 0, 0, 1, mode));
        }
        for (const uint32_t mode : {3u, 0x7FFFFFFFu, 0xFFFFFFFFu})
        {
            EXPECT_EQ(FALSE, syscalls::handle_NtGdiTransformPoints(this->context(), dc, buffer, buffer, 1, mode));
        }
        EXPECT_EQ(k_untouched, this->emu.memory.read_memory<mapping_vec>(buffer));
    }

    TEST_F(GdiMappingTest, ScaleExtentsReproduceTheCapturedResults)
    {
        int index = 0;
        for (const auto& row : k_scale_rows)
        {
            SCOPED_TRACE(index++);
            const auto user_extents = row.map_mode >= gdi::k_map_mode_isotropic;
            auto window_ext = user_extents ? k_scale_window_ext : mapping_vec{};
            auto viewport_ext = user_extents ? k_scale_viewport_ext : mapping_vec{};
            if (row.scaled_start.x != 0)
            {
                (row.window ? window_ext : viewport_ext) = row.scaled_start;
            }

            const auto dc = this->dc_in_mode(row.map_mode, window_ext, viewport_ext);

            const auto previous_address = this->scratch + 0x200;
            this->emu.memory.write_memory(previous_address, &k_untouched, sizeof(k_untouched));
            const auto& f = row.factors;
            const auto ret = row.window
                                 ? syscalls::handle_NtGdiScaleWindowExtEx(this->context(), dc, f[0], f[1], f[2], f[3], previous_address)
                                 : syscalls::handle_NtGdiScaleViewportExtEx(this->context(), dc, f[0], f[1], f[2], f[3], previous_address);
            EXPECT_EQ(row.result, ret);
            EXPECT_EQ(row.previous, this->emu.memory.read_memory<mapping_vec>(previous_address));
            EXPECT_EQ(row.window_ext, this->peek<mapping_vec>(dc, k_dc_attr_window_ext));
            EXPECT_EQ(row.viewport_ext, this->peek<mapping_vec>(dc, k_dc_attr_viewport_ext));
        }
    }
}

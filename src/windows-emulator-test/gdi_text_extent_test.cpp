#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    BOOL handle_NtGdiGetTextExtentExW(const syscall_context& c, hdc dc, emulator_pointer text, ULONG char_count, ULONG max_extent,
                                      emulator_pointer fit_count, emulator_pointer partial_extents, emulator_pointer size, FLONG flags);
}

namespace sogen::test
{
    namespace
    {
        constexpr hdc k_dc = 0x1010;
        constexpr ULONG k_unset = 0xCCCCCCCC;
        constexpr ULONG k_text_offset = 0x000;
        constexpr ULONG k_fit_offset = 0x100;
        constexpr ULONG k_size_offset = 0x110;
        constexpr ULONG k_extents_offset = 0x200;
        constexpr ULONG k_char_count = 5;
        constexpr ULONG k_checked_extents = k_char_count + 2;
        constexpr ULONG k_width = 8;

        struct extent_request
        {
            hdc dc = k_dc;
            bool with_text = true;
            ULONG char_count = k_char_count;
            ULONG max_extent = 2 * k_width;
            bool with_fit = true;
            bool with_extents = true;
            bool with_size = true;
            FLONG flags = 0;
        };

        class GdiTextExtentTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->reset();
            }

            void reset()
            {
                const std::vector<ULONG> fill(0x1000 / sizeof(ULONG), k_unset);
                this->emu->memory.write_memory(this->storage, fill.data(), fill.size() * sizeof(ULONG));
                const std::u16string_view text = u"Hello";
                this->emu->memory.write_memory(this->storage + k_text_offset, text.data(), text.size() * sizeof(char16_t));
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            BOOL extent(const extent_request& request)
            {
                return syscalls::handle_NtGdiGetTextExtentExW(this->context(), request.dc,
                                                              request.with_text ? this->storage + k_text_offset : 0, request.char_count,
                                                              request.max_extent, request.with_fit ? this->storage + k_fit_offset : 0,
                                                              request.with_extents ? this->storage + k_extents_offset : 0,
                                                              request.with_size ? this->storage + k_size_offset : 0, request.flags);
            }

            ULONG slot(const ULONG offset)
            {
                return this->emu->memory.read_memory<ULONG>(this->storage + offset);
            }

            ULONG fit()
            {
                return this->slot(k_fit_offset);
            }

            ULONG partial_extent(const ULONG index)
            {
                return this->slot(k_extents_offset + (index * sizeof(ULONG)));
            }

            SIZE size()
            {
                return this->emu->memory.read_memory<SIZE>(this->storage + k_size_offset);
            }

            void expect_partial_extents(const ULONG written)
            {
                for (ULONG i = 0; i < k_checked_extents; ++i)
                {
                    SCOPED_TRACE(i);
                    EXPECT_EQ(i < written ? (i + 1) * k_width : k_unset, this->partial_extent(i));
                }
            }

            void expect_whole_string_size()
            {
                const auto text_size = this->size();
                EXPECT_EQ(static_cast<LONG>(k_char_count * k_width), text_size.cx);
                EXPECT_EQ(16, text_size.cy);
            }

            void expect_outputs_untouched()
            {
                EXPECT_EQ(k_unset, this->fit());
                EXPECT_EQ(k_unset, this->slot(k_size_offset));
                EXPECT_EQ(k_unset, this->slot(k_size_offset + sizeof(ULONG)));
                this->expect_partial_extents(0);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(GdiTextExtentTest, FitCountAndPartialExtentsFollowMaxExtent)
    {
        struct row
        {
            ULONG max_extent;
            ULONG fit;
        };

        constexpr std::array rows{
            row{.max_extent = 0, .fit = 0},           row{.max_extent = 1, .fit = 0},
            row{.max_extent = k_width - 1, .fit = 0}, row{.max_extent = k_width, .fit = 1},
            row{.max_extent = k_width + 1, .fit = 1}, row{.max_extent = 2 * k_width, .fit = 2},
            row{.max_extent = 5 * k_width, .fit = 5}, row{.max_extent = (5 * k_width) + 1, .fit = 5},
            row{.max_extent = 1000, .fit = 5},        row{.max_extent = 0xFFFFFFFF, .fit = 5},
            row{.max_extent = 0xFFFFFFFE, .fit = 5},  row{.max_extent = 0x80000000, .fit = 5},
        };

        for (const auto& r : rows)
        {
            SCOPED_TRACE(r.max_extent);
            this->reset();

            ASSERT_EQ(TRUE, this->extent({.max_extent = r.max_extent}));
            EXPECT_EQ(r.fit, this->fit());
            this->expect_partial_extents(r.fit);
            this->expect_whole_string_size();
        }
    }

    TEST_F(GdiTextExtentTest, PartialExtentsWithoutFitCountCoverWholeString)
    {
        ASSERT_EQ(TRUE, this->extent({.with_fit = false}));

        EXPECT_EQ(k_unset, this->fit());
        this->expect_partial_extents(k_char_count);
        this->expect_whole_string_size();
    }

    TEST_F(GdiTextExtentTest, FitCountWithoutPartialExtents)
    {
        ASSERT_EQ(TRUE, this->extent({.with_extents = false}));

        EXPECT_EQ(2u, this->fit());
        this->expect_partial_extents(0);
        this->expect_whole_string_size();
    }

    TEST_F(GdiTextExtentTest, EmptyStringHasZeroSize)
    {
        for (const bool with_text : {true, false})
        {
            SCOPED_TRACE(with_text);
            this->reset();

            ASSERT_EQ(TRUE, this->extent({.with_text = with_text, .char_count = 0}));
            EXPECT_EQ(0u, this->fit());
            this->expect_partial_extents(0);
            const auto text_size = this->size();
            EXPECT_EQ(0, text_size.cx);
            EXPECT_EQ(0, text_size.cy);
        }
    }

    TEST_F(GdiTextExtentTest, InvalidArgumentsFailWithoutWritingOutputs)
    {
        const std::array requests{
            extent_request{.with_text = false},       extent_request{.char_count = 0x80000000},
            extent_request{.char_count = 0xFFFFFFFF}, extent_request{.dc = 0},
            extent_request{.with_size = false},
        };

        for (size_t i = 0; i < requests.size(); ++i)
        {
            SCOPED_TRACE(i);
            this->reset();

            EXPECT_EQ(FALSE, this->extent(requests[i]));
            this->expect_outputs_untouched();
        }
    }

    TEST_F(GdiTextExtentTest, GlyphIndexAndUnknownFlagsMeasureLikeCharacters)
    {
        for (const FLONG flags : {FLONG{1}, FLONG{2}})
        {
            SCOPED_TRACE(flags);
            this->reset();

            ASSERT_EQ(TRUE, this->extent({.flags = flags}));
            EXPECT_EQ(2u, this->fit());
            this->expect_partial_extents(2);
            this->expect_whole_string_size();
        }
    }
}

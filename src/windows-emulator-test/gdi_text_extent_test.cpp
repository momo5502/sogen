#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <optional>

namespace sogen::syscalls
{
    BOOL handle_NtGdiGetTextExtentExW(const syscall_context& c, hdc dc, emulator_pointer text, ULONG char_count, ULONG max_extent,
                                      emulator_object<ULONG> fit, emulator_pointer extents, emulator_pointer size, ULONG flags);
}

namespace sogen::test
{
    namespace
    {
        constexpr hdc k_dc = 0x1010;
        constexpr ULONG k_unset = 0xCCCCCCCC;
        constexpr size_t k_page_size = 0x1000;
        constexpr size_t k_text_size = 2 * k_page_size;
        constexpr uint64_t k_fit_offset = 0x100;
        constexpr uint64_t k_size_offset = 0x110;
        constexpr uint64_t k_extents_offset = 0x200;
        constexpr size_t k_unmapped_offset = 2 * k_page_size;
        constexpr ULONG k_tail_extent_slots = 3;
        constexpr size_t k_tail_extents_offset = k_unmapped_offset - (k_tail_extent_slots * sizeof(ULONG));
        constexpr ULONG k_max_chars = 0x682AAA;
        constexpr ULONG k_char_count = 5;
        constexpr ULONG k_checked_extents = k_char_count + 2;
        constexpr ULONG k_width = 8;
        constexpr LONG k_height = 16;

        struct extent_request
        {
            hdc dc = k_dc;
            bool with_text = true;
            ULONG char_count = k_char_count;
            ULONG max_extent = 2 * k_width;
            bool with_fit = true;
            bool with_extents = true;
            uint64_t extents_offset = k_extents_offset;
            bool with_size = true;
            uint64_t size_offset = k_size_offset;
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
                this->storage = this->emu->memory.allocate_memory(3 * k_page_size, memory_permission::read_write, true);
                ASSERT_TRUE(this->emu->memory.commit_memory(this->storage, k_unmapped_offset, memory_permission::read_write));
                this->text = this->emu->memory.allocate_memory(k_text_size, memory_permission::read_write);
                this->reset();
            }

            void reset()
            {
                const std::vector<ULONG> fill(k_unmapped_offset / sizeof(ULONG), k_unset);
                this->emu->memory.write_memory(this->storage, fill.data(), fill.size() * sizeof(ULONG));
                const std::u16string_view hello = u"Hello";
                this->emu->memory.write_memory(this->text, hello.data(), hello.size() * sizeof(char16_t));
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            BOOL extent(const extent_request& request)
            {
                return this->extent_at(request, request.with_text ? this->text : 0,
                                       request.with_extents ? this->storage + request.extents_offset : 0);
            }

            BOOL extent_at(const extent_request& request, const uint64_t text_address, const uint64_t extents)
            {
                const emulator_object<ULONG> fit_count{this->emu->memory, request.with_fit ? this->storage + k_fit_offset : 0};
                return syscalls::handle_NtGdiGetTextExtentExW(this->context(), request.dc, text_address, request.char_count,
                                                              request.max_extent, fit_count, extents,
                                                              request.with_size ? this->storage + request.size_offset : 0, 0);
            }

            uint64_t sentinel_page(const memory_permission permission)
            {
                const auto page = this->emu->memory.allocate_memory(k_page_size, memory_permission::read_write);
                const std::vector<ULONG> fill(k_page_size / sizeof(ULONG), k_unset);
                this->emu->memory.write_memory(page, fill.data(), fill.size() * sizeof(ULONG));
                EXPECT_TRUE(this->emu->memory.protect_memory(page, k_page_size, permission));
                return page;
            }

            BOOL extent_into(const ULONG char_count, const uint64_t fit_address, const uint64_t extents, const uint64_t size)
            {
                const emulator_object<ULONG> fit_count{this->emu->memory, fit_address};
                return syscalls::handle_NtGdiGetTextExtentExW(this->context(), k_dc, this->text, char_count, 2 * k_width, fit_count,
                                                              extents, size, 0);
            }

            bool is_untouched(const uint64_t address, const ULONG slots)
            {
                for (ULONG i = 0; i < slots; ++i)
                {
                    if (this->emu->memory.read_memory<ULONG>(address + (i * sizeof(ULONG))) != k_unset)
                    {
                        return false;
                    }
                }

                return true;
            }

            ULONG slot(const uint64_t offset)
            {
                return this->emu->memory.read_memory<ULONG>(this->storage + offset);
            }

            ULONG fit()
            {
                return this->slot(k_fit_offset);
            }

            void expect_extents(const uint64_t offset, const ULONG slots, const ULONG written)
            {
                for (ULONG i = 0; i < slots; ++i)
                {
                    SCOPED_TRACE(i);
                    EXPECT_EQ(i < written ? (i + 1) * k_width : k_unset, this->slot(offset + (i * sizeof(ULONG))));
                }
            }

            void expect_partial_extents(const ULONG written)
            {
                this->expect_extents(k_extents_offset, k_checked_extents, written);
            }

            void expect_size(const LONG cx, const LONG cy)
            {
                const auto text_size = this->emu->memory.read_memory<SIZE>(this->storage + k_size_offset);
                EXPECT_EQ(cx, text_size.cx);
                EXPECT_EQ(cy, text_size.cy);
            }

            void expect_whole_string_size()
            {
                this->expect_size(static_cast<LONG>(k_char_count * k_width), k_height);
            }

            void expect_size_untouched()
            {
                EXPECT_EQ(k_unset, this->slot(k_size_offset));
                EXPECT_EQ(k_unset, this->slot(k_size_offset + sizeof(ULONG)));
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            uint64_t text{};
        };
    }

    TEST_F(GdiTextExtentTest, FitCountLimitsWrittenPartialExtents)
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
        for (const ULONG max_extent : {2 * k_width, ULONG{0xFFFFFFFF}})
        {
            SCOPED_TRACE(max_extent);
            this->reset();

            ASSERT_EQ(TRUE, this->extent({.max_extent = max_extent, .with_fit = false}));
            EXPECT_EQ(k_unset, this->fit());
            this->expect_partial_extents(k_char_count);
            this->expect_whole_string_size();
        }
    }

    TEST_F(GdiTextExtentTest, EmptyStringHasZeroSizeWithoutTextOrDc)
    {
        const std::array requests{
            extent_request{.char_count = 0},
            extent_request{.with_text = false, .char_count = 0},
            extent_request{.dc = 0, .char_count = 0},
        };

        for (size_t i = 0; i < requests.size(); ++i)
        {
            SCOPED_TRACE(i);
            this->reset();

            ASSERT_EQ(TRUE, this->extent(requests[i]));
            EXPECT_EQ(0u, this->fit());
            this->expect_partial_extents(0);
            this->expect_size(0, 0);
        }
    }

    TEST_F(GdiTextExtentTest, EmptyStringWithoutSizeFails)
    {
        EXPECT_EQ(FALSE, this->extent({.char_count = 0, .with_size = false}));
        EXPECT_EQ(k_unset, this->fit());
        this->expect_partial_extents(0);
    }

    TEST_F(GdiTextExtentTest, UnwritableSizeFailsBeforeOtherOutputs)
    {
        EXPECT_EQ(FALSE, this->extent({.size_offset = k_unmapped_offset}));
        EXPECT_EQ(k_unset, this->fit());
        this->expect_partial_extents(0);
    }

    TEST_F(GdiTextExtentTest, UnwritableFitCountFailsAfterSize)
    {
        const emulator_object<ULONG> unmapped_fit{this->emu->memory, this->storage + k_unmapped_offset};
        EXPECT_EQ(FALSE, syscalls::handle_NtGdiGetTextExtentExW(this->context(), k_dc, this->text, k_char_count, 2 * k_width, unmapped_fit,
                                                                this->storage + k_extents_offset, this->storage + k_size_offset, 0));
        this->expect_whole_string_size();
        this->expect_partial_extents(0);
    }

    TEST_F(GdiTextExtentTest, FittingPartialExtentsBeforeUnmappedMemory)
    {
        ASSERT_EQ(TRUE, this->extent({.extents_offset = k_tail_extents_offset}));
        EXPECT_EQ(2u, this->fit());
        this->expect_extents(k_tail_extents_offset, k_tail_extent_slots, 2);
        this->expect_whole_string_size();
    }

    TEST_F(GdiTextExtentTest, PartialExtentsRunningIntoUnmappedMemoryFailAfterSizeAndFit)
    {
        struct row
        {
            ULONG room;
            ULONG char_count;
            std::optional<ULONG> written;
            bool captured_with_fit;
        };

        const std::array rows{
            row{.room = 2, .char_count = 3, .written = 2, .captured_with_fit = true},
            row{.room = 4, .char_count = 5, .written = 4, .captured_with_fit = true},
            row{.room = 5, .char_count = 64, .written = 5, .captured_with_fit = true},
            row{.room = 8, .char_count = 64, .written = 8, .captured_with_fit = true},
            row{.room = 13, .char_count = 4096, .written = 13, .captured_with_fit = true},
            row{.room = 300, .char_count = 400, .written = 300, .captured_with_fit = false},
            row{.room = 1, .char_count = 2, .written = std::nullopt, .captured_with_fit = true},
            row{.room = 3, .char_count = 5, .written = std::nullopt, .captured_with_fit = true},
            row{.room = 9, .char_count = 10, .written = std::nullopt, .captured_with_fit = true},
        };

        for (const auto& r : rows)
        {
            for (const bool with_fit : {false, true})
            {
                if (with_fit && !r.captured_with_fit)
                {
                    continue;
                }

                SCOPED_TRACE(r.room);
                SCOPED_TRACE(r.char_count);
                SCOPED_TRACE(with_fit);
                this->reset();

                const auto extents_offset = k_unmapped_offset - (r.room * sizeof(ULONG));
                EXPECT_EQ(
                    FALSE,
                    this->extent(
                        {.char_count = r.char_count, .max_extent = 0xFFFFFFFF, .with_fit = with_fit, .extents_offset = extents_offset}));
                EXPECT_EQ(with_fit ? r.char_count : k_unset, this->fit());
                this->expect_size(static_cast<LONG>(r.char_count * k_width), k_height);
                if (r.written)
                {
                    this->expect_extents(extents_offset, r.room, *r.written);
                }
            }
        }
    }

    TEST_F(GdiTextExtentTest, TextUnreadableBeforeItsEndFails)
    {
        const auto holed_text = this->emu->memory.allocate_memory(3 * k_page_size, memory_permission::read_write, true);
        ASSERT_TRUE(this->emu->memory.commit_memory(holed_text, k_page_size, memory_permission::read_write));
        ASSERT_TRUE(this->emu->memory.commit_memory(holed_text + (2 * k_page_size), k_page_size, memory_permission::read_write));

        EXPECT_EQ(FALSE, this->extent_at({.char_count = 0x1008}, holed_text, this->storage + k_extents_offset));
        EXPECT_EQ(k_unset, this->fit());
        this->expect_size_untouched();
        this->expect_partial_extents(0);

        this->reset();
        ASSERT_EQ(TRUE, this->extent_at({.char_count = 0x800}, holed_text, this->storage + k_extents_offset));
        EXPECT_EQ(2u, this->fit());
        this->expect_partial_extents(2);
        this->expect_size(static_cast<LONG>(0x800 * k_width), k_height);
    }

    TEST_F(GdiTextExtentTest, CountAboveTheLimitFails)
    {
        constexpr uint64_t text_bytes = page_align_up(static_cast<uint64_t>(k_max_chars + 1) * sizeof(char16_t));
        const auto long_text = this->emu->memory.allocate_memory(static_cast<size_t>(text_bytes), memory_permission::read_write);

        for (const bool with_fit : {false, true})
        {
            SCOPED_TRACE(with_fit);
            this->reset();

            EXPECT_EQ(FALSE,
                      this->extent_at({.char_count = k_max_chars + 1, .max_extent = 0xFFFFFFFF, .with_fit = with_fit}, long_text, 0));
            EXPECT_EQ(k_unset, this->fit());
            this->expect_size_untouched();

            ASSERT_EQ(TRUE, this->extent_at({.char_count = k_max_chars, .max_extent = 0xFFFFFFFF, .with_fit = with_fit}, long_text, 0));
            EXPECT_EQ(with_fit ? k_max_chars : k_unset, this->fit());
            this->expect_size(static_cast<LONG>(k_max_chars * k_width), k_height);
        }
    }

    TEST_F(GdiTextExtentTest, InaccessibleTextFails)
    {
        const auto no_access = this->sentinel_page(memory_permission::none);

        EXPECT_EQ(FALSE, this->extent_at({}, no_access, this->storage + k_extents_offset));
        EXPECT_EQ(k_unset, this->fit());
        this->expect_size_untouched();
        this->expect_partial_extents(0);
    }

    TEST_F(GdiTextExtentTest, ReadOnlyOutputsFailWithoutBeingWritten)
    {
        const auto read_only = this->sentinel_page(memory_permission::read);
        const auto fit_address = this->storage + k_fit_offset;
        const auto extents = this->storage + k_extents_offset;
        const auto size = this->storage + k_size_offset;

        EXPECT_EQ(FALSE, this->extent_into(k_char_count, fit_address, extents, read_only));
        EXPECT_EQ(k_unset, this->fit());
        this->expect_partial_extents(0);
        EXPECT_TRUE(this->is_untouched(read_only, 2));

        this->reset();
        EXPECT_EQ(FALSE, this->extent_into(k_char_count, read_only, extents, size));
        this->expect_whole_string_size();
        this->expect_partial_extents(0);
        EXPECT_TRUE(this->is_untouched(read_only, 1));

        this->reset();
        EXPECT_EQ(FALSE, this->extent_into(k_char_count, 0, read_only, size));
        this->expect_whole_string_size();
        EXPECT_TRUE(this->is_untouched(read_only, k_checked_extents));

        this->reset();
        EXPECT_EQ(FALSE, this->extent_into(0, read_only, extents, size));
        this->expect_size(0, 0);
        this->expect_partial_extents(0);
        EXPECT_TRUE(this->is_untouched(read_only, 1));
    }
}

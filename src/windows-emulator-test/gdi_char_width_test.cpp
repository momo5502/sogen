#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <algorithm>

namespace sogen::syscalls
{
    BOOL handle_NtGdiGetCharWidthW(const syscall_context& c, hdc dc, UINT first_char, UINT char_count, emulator_pointer chars, UINT flags,
                                   emulator_pointer buffer);
    BOOL handle_NtGdiGetCharABCWidthsW(const syscall_context& c, hdc dc, UINT first_char, UINT char_count, emulator_pointer chars,
                                       UINT flags, emulator_pointer buffer);
}

namespace sogen::test
{
    namespace
    {
        constexpr size_t page_size = 0x1000;
        constexpr hdc probe_dc = 1;
        constexpr uint32_t sentinel = 0xCCCCCCCC;
        constexpr size_t output_limit = 0x2710000;
        constexpr UINT gcw_nofloat = 2;
        constexpr UINT gcabcw_nofloat = 1;

        class GdiCharWidthTest : public testing::Test
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
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            uint64_t first_page_committed(const size_t reserved_pages)
            {
                const auto base = this->emu->memory.allocate_memory(reserved_pages * page_size, memory_permission::read_write, true);
                EXPECT_TRUE(this->emu->memory.commit_memory(base, page_size, memory_permission::read_write));
                this->fill_with_sentinel(base, page_size);
                return base;
            }

            void fill_with_sentinel(const uint64_t address, const size_t size)
            {
                const std::vector<uint32_t> fill(size / sizeof(uint32_t), sentinel);
                this->emu->memory.write_memory(address, fill.data(), fill.size() * sizeof(uint32_t));
            }

            bool is_untouched(const uint64_t address, const size_t size)
            {
                std::vector<uint32_t> values(size / sizeof(uint32_t));
                this->emu->memory.read_memory(address, values.data(), values.size() * sizeof(uint32_t));
                return std::ranges::all_of(values, [](const uint32_t value) { return value == sentinel; });
            }

            BOOL widths(const UINT count, const uint64_t buffer, const uint64_t chars = 0)
            {
                return syscalls::handle_NtGdiGetCharWidthW(this->context(), probe_dc, 0, count, chars, gcw_nofloat, buffer);
            }

            BOOL abc_widths(const UINT count, const uint64_t buffer, const uint64_t chars = 0)
            {
                return syscalls::handle_NtGdiGetCharABCWidthsW(this->context(), probe_dc, 0, count, chars, gcabcw_nofloat, buffer);
            }

            std::unique_ptr<windows_emulator> emu{};
        };
    }

    TEST_F(GdiCharWidthTest, ZeroCountFails)
    {
        const auto page = this->first_page_committed(1);
        EXPECT_EQ(FALSE, this->widths(0, page));
        EXPECT_EQ(FALSE, this->abc_widths(0, page));
        EXPECT_TRUE(this->is_untouched(page, page_size));
    }

    TEST_F(GdiCharWidthTest, CountsAboveTheLimitFailBeforeWriting)
    {
        const auto page = this->first_page_committed(16);
        for (const UINT count : {0x01000000u, 0x40000001u, 0xFFFFFFFFu})
        {
            EXPECT_EQ(FALSE, this->widths(count, page));
        }
        for (const UINT count : {0x01000000u, 0x15555556u, 0xFFFFFFFFu})
        {
            EXPECT_EQ(FALSE, this->abc_widths(count, page));
        }
        EXPECT_TRUE(this->is_untouched(page, page_size));
    }

    TEST_F(GdiCharWidthTest, LargestAcceptedOutputIsWritten)
    {
        const auto buffer = this->emu->memory.allocate_memory(output_limit + page_size, memory_permission::read_write);

        constexpr auto width_count = static_cast<UINT>(output_limit / sizeof(int32_t));
        const auto last_width = buffer + ((width_count - 1) * sizeof(int32_t));
        const auto past_widths = buffer + (width_count * sizeof(int32_t));
        for (const auto slot : {buffer, last_width, past_widths})
        {
            this->fill_with_sentinel(slot, sizeof(int32_t));
        }
        ASSERT_EQ(TRUE, this->widths(width_count, buffer));
        EXPECT_FALSE(this->is_untouched(buffer, sizeof(int32_t)));
        EXPECT_FALSE(this->is_untouched(last_width, sizeof(int32_t)));
        EXPECT_TRUE(this->is_untouched(past_widths, sizeof(int32_t)));

        constexpr auto abc_count = static_cast<UINT>(output_limit / sizeof(ABC));
        const auto last_abc = buffer + ((abc_count - 1) * sizeof(ABC));
        const auto past_abc = buffer + (abc_count * sizeof(ABC));
        for (const auto slot : {buffer, last_abc, past_abc})
        {
            this->fill_with_sentinel(slot, sizeof(ABC));
        }
        ASSERT_EQ(TRUE, this->abc_widths(abc_count, buffer));
        EXPECT_FALSE(this->is_untouched(buffer, sizeof(ABC)));
        EXPECT_FALSE(this->is_untouched(last_abc, sizeof(ABC)));
        EXPECT_TRUE(this->is_untouched(past_abc, sizeof(ABC)));
    }

    TEST_F(GdiCharWidthTest, OutputAboveTheLimitFailsBeforeWriting)
    {
        const auto buffer = this->emu->memory.allocate_memory(output_limit + page_size, memory_permission::read_write);

        constexpr auto width_count = static_cast<UINT>((output_limit / sizeof(int32_t)) + 1);
        const auto last_width = buffer + ((width_count - 1) * sizeof(int32_t));
        this->fill_with_sentinel(buffer, sizeof(int32_t));
        this->fill_with_sentinel(last_width, sizeof(int32_t));
        EXPECT_EQ(FALSE, this->widths(width_count, buffer));
        EXPECT_TRUE(this->is_untouched(buffer, sizeof(int32_t)));
        EXPECT_TRUE(this->is_untouched(last_width, sizeof(int32_t)));

        constexpr auto abc_count = static_cast<UINT>((output_limit / sizeof(ABC)) + 1);
        const auto last_abc = buffer + ((abc_count - 1) * sizeof(ABC));
        this->fill_with_sentinel(buffer, sizeof(ABC));
        this->fill_with_sentinel(last_abc, sizeof(ABC));
        EXPECT_EQ(FALSE, this->abc_widths(abc_count, buffer));
        EXPECT_TRUE(this->is_untouched(buffer, sizeof(ABC)));
        EXPECT_TRUE(this->is_untouched(last_abc, sizeof(ABC)));
    }

    TEST_F(GdiCharWidthTest, ShortWidthBufferKeepsOnlyTheBlocksThatFit)
    {
        const auto page = this->first_page_committed(16);
        const auto tail = page + page_size - (3 * sizeof(int32_t));
        EXPECT_EQ(FALSE, this->widths(5, tail));
        EXPECT_TRUE(this->is_untouched(tail, 3 * sizeof(int32_t)));

        EXPECT_EQ(FALSE, this->widths(0x10000, page));
        EXPECT_FALSE(this->is_untouched(page, sizeof(int32_t)));
        EXPECT_FALSE(this->is_untouched(page + page_size - sizeof(int32_t), sizeof(int32_t)));
    }

    TEST_F(GdiCharWidthTest, ShortAbcBufferIsFilledUpToTheUnwritablePage)
    {
        const auto page = this->first_page_committed(2);
        const auto tail = page + page_size - (3 * sizeof(ABC));
        EXPECT_EQ(FALSE, this->abc_widths(5, tail));
        for (size_t i = 0; i < 3; ++i)
        {
            EXPECT_NE(sentinel, this->emu->memory.read_memory<ABC>(tail + (i * sizeof(ABC))).abcB);
        }
    }

    TEST_F(GdiCharWidthTest, ReadOnlyBufferFails)
    {
        const auto page = this->first_page_committed(1);
        ASSERT_TRUE(this->emu->memory.protect_memory(page, page_size, memory_permission::read));
        EXPECT_EQ(FALSE, this->widths(5, page));
        EXPECT_EQ(FALSE, this->abc_widths(5, page));
        EXPECT_TRUE(this->is_untouched(page, page_size));
    }

    TEST_F(GdiCharWidthTest, UnreadableCharacterListFails)
    {
        const auto buffer = this->first_page_committed(1);
        const auto chars = this->first_page_committed(2);
        const auto tail = chars + page_size - (3 * sizeof(char16_t));

        EXPECT_EQ(FALSE, this->widths(5, buffer, tail));
        EXPECT_EQ(FALSE, this->abc_widths(5, buffer, tail));
        EXPECT_TRUE(this->is_untouched(buffer, page_size));
        EXPECT_EQ(TRUE, this->widths(3, buffer, tail));

        ASSERT_TRUE(this->emu->memory.protect_memory(chars, page_size, memory_permission::none));
        EXPECT_EQ(FALSE, this->widths(5, buffer, chars));
        EXPECT_EQ(FALSE, this->abc_widths(5, buffer, chars));
    }
}

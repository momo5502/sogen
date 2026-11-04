#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtGdiCreatePaletteInternal(const syscall_context& c, emulator_pointer log_palette, uint32_t entry_count);
    LONG handle_NtGdiDoPalette(const syscall_context& c, handle object, uint16_t start, uint16_t count, emulator_pointer entries,
                               uint32_t function, BOOL inbound);
    uint32_t handle_NtGdiGetNearestPaletteIndex(const syscall_context& c, handle palette, COLORREF color);
    uint32_t handle_NtGdiExtGetObjectW(const syscall_context& c, uint32_t handle_value, uint32_t size, emulator_pointer buffer);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
}

namespace sogen::test
{
    namespace
    {
        constexpr size_t k_storage_size = 0x1000;
        constexpr uint64_t k_entries_offset = 0x800;
        constexpr uint32_t k_animate = 0;
        constexpr uint32_t k_set_entries = 1;
        constexpr uint32_t k_get_entries = 2;
        constexpr BYTE k_reserved = 0x01;
        constexpr uint32_t k_clr_invalid = 0xFFFFFFFF;

        using entry = std::array<BYTE, 4>;

        constexpr COLORREF rgb(const BYTE red, const BYTE green, const BYTE blue)
        {
            return static_cast<COLORREF>(red | (green << 8) | (blue << 16));
        }

        // The expected results were measured on Windows 11 with CreatePalette, Get/SetPaletteEntries, AnimatePalette,
        // GetNearestPaletteIndex and GetObject on the same palette.
        class GdiPaletteTest : public testing::Test
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

            handle create(const std::vector<entry>& entries)
            {
                const auto header = std::array<WORD, 2>{0x300, static_cast<WORD>(entries.size())};
                this->emu.memory.write_memory(this->storage, header.data(), sizeof(header));
                this->emu.memory.write_memory(this->storage + sizeof(header), entries.data(), entries.size() * sizeof(entry));

                const auto palette =
                    syscalls::handle_NtGdiCreatePaletteInternal(this->context(), this->storage, static_cast<uint32_t>(entries.size()));
                EXPECT_NE(0u, palette);
                return make_handle(palette);
            }

            LONG do_palette(const handle palette, const uint16_t start, const uint16_t count, const uint32_t function,
                            const std::vector<entry>& input = {})
            {
                this->emu.memory.write_memory(this->storage + k_entries_offset, input.data(), input.size() * sizeof(entry));
                return syscalls::handle_NtGdiDoPalette(this->context(), palette, start, count, this->storage + k_entries_offset, function,
                                                       function != k_get_entries);
            }

            std::vector<entry> entries(const handle palette)
            {
                const auto count = syscalls::handle_NtGdiDoPalette(this->context(), palette, 0, 0, 0, k_get_entries, FALSE);
                std::vector<entry> result(static_cast<size_t>(count));
                EXPECT_EQ(count, this->do_palette(palette, 0, static_cast<uint16_t>(count), k_get_entries));
                this->emu.memory.read_memory(this->storage + k_entries_offset, result.data(), result.size() * sizeof(entry));
                return result;
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t storage{};
        };
    }

    TEST_F(GdiPaletteTest, EntryRangesAreClampedToThePalette)
    {
        const auto palette = this->create({{10, 20, 30, 0}, {200, 0, 0, 0}, {0, 200, 0, 0}, {0, 0, 200, 0}});

        EXPECT_EQ(4, syscalls::handle_NtGdiDoPalette(this->context(), palette, 0, 0, 0, k_get_entries, FALSE));
        EXPECT_EQ(2, this->do_palette(palette, 2, 5, k_get_entries));
        EXPECT_EQ(0, this->do_palette(palette, 4, 1, k_get_entries));

        EXPECT_EQ(1, this->do_palette(palette, 3, 2, k_set_entries, {{1, 2, 3, 0}, {4, 5, 6, 0}}));
        EXPECT_EQ((std::vector<entry>{{10, 20, 30, 0}, {200, 0, 0, 0}, {0, 200, 0, 0}, {1, 2, 3, 0}}), this->entries(palette));
    }

    TEST_F(GdiPaletteTest, AnimationOnlyReplacesReservedEntries)
    {
        const auto palette = this->create({{10, 20, 30, 0}, {200, 0, 0, k_reserved}, {0, 200, 0, 0}, {0, 0, 200, k_reserved}});

        EXPECT_EQ(TRUE, this->do_palette(palette, 0, 4, k_animate,
                                         {{9, 9, 9, k_reserved}, {9, 9, 9, k_reserved}, {9, 9, 9, k_reserved}, {9, 9, 9, k_reserved}}));
        EXPECT_EQ((std::vector<entry>{{10, 20, 30, 0}, {9, 9, 9, k_reserved}, {0, 200, 0, 0}, {9, 9, 9, k_reserved}}),
                  this->entries(palette));
    }

    TEST_F(GdiPaletteTest, NearestIndexIsTheClosestEntry)
    {
        const auto palette = this->create({{10, 20, 30, 0}, {200, 0, 0, 0}, {0, 200, 0, 0}, {0, 0, 200, 0}});

        EXPECT_EQ(1u, syscalls::handle_NtGdiGetNearestPaletteIndex(this->context(), palette, rgb(190, 10, 10)));
        EXPECT_EQ(2u, syscalls::handle_NtGdiGetNearestPaletteIndex(this->context(), palette, rgb(0, 190, 5)));
        EXPECT_EQ(0u, syscalls::handle_NtGdiGetNearestPaletteIndex(this->context(), palette, rgb(10, 20, 30)));
    }

    TEST_F(GdiPaletteTest, ObjectReportsTheEntryCount)
    {
        const auto palette = this->create({{10, 20, 30, 0}, {200, 0, 0, 0}, {0, 200, 0, 0}, {0, 0, 200, 0}});
        const auto value = static_cast<uint32_t>(palette.bits);

        EXPECT_EQ(sizeof(WORD), syscalls::handle_NtGdiExtGetObjectW(this->context(), value, 0, 0));
        EXPECT_EQ(sizeof(WORD), syscalls::handle_NtGdiExtGetObjectW(this->context(), value, 16, this->storage));
        EXPECT_EQ(4, this->emu.memory.read_memory<WORD>(this->storage));
    }

    TEST_F(GdiPaletteTest, DeletedPaletteIsInvalid)
    {
        const auto palette = this->create({{10, 20, 30, 0}});

        EXPECT_EQ(1u, syscalls::handle_NtGdiDeleteObjectApp(this->context(), static_cast<uint32_t>(palette.bits)));
        EXPECT_EQ(k_clr_invalid, syscalls::handle_NtGdiGetNearestPaletteIndex(this->context(), palette, rgb(10, 20, 30)));
        EXPECT_EQ(0, syscalls::handle_NtGdiDoPalette(this->context(), palette, 0, 0, 0, k_get_entries, FALSE));
    }

    TEST_F(GdiPaletteTest, EntriesSurviveASnapshot)
    {
        const auto palette = this->create({{10, 20, 30, 0}, {200, 0, 0, 0}});

        utils::buffer_serializer snapshot{};
        this->emu.serialize(snapshot);
        ASSERT_EQ(2, this->do_palette(palette, 0, 2, k_set_entries, {{1, 1, 1, 0}, {2, 2, 2, 0}}));
        utils::buffer_deserializer input{snapshot};
        this->emu.deserialize(input);

        EXPECT_EQ((std::vector<entry>{{10, 20, 30, 0}, {200, 0, 0, 0}}), this->entries(palette));
    }
}

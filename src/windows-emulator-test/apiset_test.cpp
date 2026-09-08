#include "emulation_test_utils.hpp"

#include <apiset/apiset.hpp>
#include <memory_manager.hpp>
#include <binary_writer.hpp>

namespace sogen::test
{
    namespace
    {
        struct api_set_source_entry
        {
            std::u16string name{};
            std::vector<std::u16string> values{};
        };

        apiset::container build_api_set_map(const std::vector<api_set_source_entry>& source_entries)
        {
            const auto count = static_cast<ULONG>(source_entries.size());

            API_SET_NAMESPACE header{};
            header.Version = 6;
            header.Count = count;
            header.EntryOffset = sizeof(API_SET_NAMESPACE);
            header.HashOffset = header.EntryOffset + count * sizeof(API_SET_NAMESPACE_ENTRY);

            std::vector<uint8_t> buffer{};
            utils::aligned_binary_writer writer{buffer};
            writer.pad(header.HashOffset + count * sizeof(API_SET_HASH_ENTRY));

            for (ULONG i = 0; i < count; ++i)
            {
                const auto& source = source_entries[i];

                API_SET_NAMESPACE_ENTRY entry{};
                entry.NameOffset = static_cast<ULONG>(writer.offset());
                entry.NameLength = static_cast<ULONG>(source.name.size() * sizeof(char16_t));
                entry.HashedLength = entry.NameLength;
                entry.ValueCount = static_cast<ULONG>(source.values.size());
                writer.write(source.name.data(), entry.NameLength);

                std::vector<API_SET_VALUE_ENTRY> values{};
                for (const auto& host : source.values)
                {
                    API_SET_VALUE_ENTRY value{};
                    value.ValueOffset = static_cast<ULONG>(writer.offset());
                    value.ValueLength = static_cast<ULONG>(host.size() * sizeof(char16_t));
                    writer.write(host.data(), value.ValueLength);
                    values.push_back(value);
                }

                writer.align_to(alignof(API_SET_VALUE_ENTRY));
                entry.ValueOffset = static_cast<ULONG>(writer.offset());
                for (const auto& value : values)
                {
                    writer.write(value);
                }

                const API_SET_HASH_ENTRY hash{.Hash = (i + 1) * 0x1000, .Index = i};

                writer.write_at(header.EntryOffset + i * sizeof(API_SET_NAMESPACE_ENTRY), entry);
                writer.write_at(header.HashOffset + i * sizeof(API_SET_HASH_ENTRY), hash);
            }

            header.Size = static_cast<ULONG>(writer.offset());
            writer.write_at(0, header);

            const auto* begin = reinterpret_cast<const std::byte*>(buffer.data());
            return {.data = {begin, begin + buffer.size()}};
        }
    }

    TEST(ApiSetTest, CloneWritesEntriesWithoutValues)
    {
        const auto source = build_api_set_map({
            {.name = u"api-ms-win-core-first-l1-1-0", .values = {u"first.dll"}},
            {.name = u"api-ms-win-core-empty-l1-1-0", .values = {}},
            {.name = u"api-ms-win-core-last-l1-1-0", .values = {u"last.dll"}},
        });

        const auto emu = create_x86_64_emulator_from_environment();
        memory_manager memory{*emu};

        constexpr size_t region_size = 0x10000;
        const auto region = memory.allocate_memory(region_size, memory_permission::read_write);
        ASSERT_NE(region, 0u);

        emulator_allocator allocator{memory, region, region_size};
        const auto clone = apiset::clone(*emu, allocator, source);
        const auto header = clone.read();
        ASSERT_EQ(header.Count, 3u);

        const emulator_object<API_SET_NAMESPACE_ENTRY> entries{memory, clone.value() + header.EntryOffset};
        const emulator_object<API_SET_HASH_ENTRY> hashes{memory, clone.value() + header.HashOffset};

        const auto empty_entry = entries.read(1);
        EXPECT_EQ(empty_entry.ValueCount, 0u);
        EXPECT_EQ(empty_entry.ValueOffset, 0u);
        EXPECT_EQ(read_string<char16_t>(memory, clone.value() + empty_entry.NameOffset, empty_entry.NameLength / sizeof(char16_t)),
                  u"api-ms-win-core-empty-l1-1-0");

        const auto empty_hash = hashes.read(1);
        EXPECT_EQ(empty_hash.Hash, 0x2000u);
        EXPECT_EQ(empty_hash.Index, 1u);

        const auto cloned_data = memory.read_memory(clone.value(), static_cast<size_t>(region + region_size - clone.value()));
        const auto table = apiset::get_namespace_table(reinterpret_cast<const API_SET_NAMESPACE*>(cloned_data.data()));

        const apiset_map expected_table{
            {u"api-ms-win-core-first-l1-1-0.dll", u"first.dll"},
            {u"api-ms-win-core-last-l1-1-0.dll", u"last.dll"},
        };
        EXPECT_EQ(table, expected_table);
    }
} // namespace sogen::test

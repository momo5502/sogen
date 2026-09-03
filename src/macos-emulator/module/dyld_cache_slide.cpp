#include "../std_include.hpp"
#include "dyld_cache_slide.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace sogen
{
    namespace
    {
        // dyld_cache_slide_info5, measured on this host and matching the layout Stage 2 recorded: a
        // 24-byte header followed by one uint16 per page. 0xFFFF marks a page with nothing to rebase;
        // anything else is the byte offset of the first entry in that page's chain.
        //
        // The 0xFFFF test looks redundant at a 16 KB page size, because the offset is past the end of
        // the page and the bounds check below would skip it anyway. It is not redundant at 64 KB, where
        // 0xFFFF is a valid offset and walking from it would rewrite whatever happens to live there.
        constexpr uint32_t SLIDE_INFO_VERSION_5 = 5;
        constexpr size_t SLIDE_INFO5_HEADER_SIZE = 24;
        constexpr uint16_t SLIDE_PAGE_NO_REBASE = 0xFFFF;

        struct slide_info_header
        {
            uint32_t version{};
            uint32_t page_size{};
            uint32_t page_starts_count{};
            uint64_t value_add{};
        };

        // The chain entry, decoded from the real cache rather than a header. Every field was checked
        // against the data: a run of consecutive pointers reports next == 1, a pair 16 bytes apart
        // reports 2, and the entries that skip over hash blobs report exactly the distance to the next
        // pointer. Every runtimeOffset lands inside the cache's own span.
        struct slide_entry
        {
            uint64_t runtime_offset{};
            uint32_t next{};
            uint16_t diversity{};
            uint8_t high8{};
            bool address_diversified{};
            bool key_is_data{};
            bool authenticated{};
        };

        slide_entry decode_slide_entry(const uint64_t raw)
        {
            slide_entry entry{};
            entry.runtime_offset = raw & ((uint64_t{1} << 34) - 1);
            entry.next = static_cast<uint32_t>((raw >> 52) & 0x7FF);
            entry.authenticated = ((raw >> 63) & 1) != 0;

            if (entry.authenticated)
            {
                entry.diversity = static_cast<uint16_t>((raw >> 34) & 0xFFFF);
                entry.address_diversified = ((raw >> 50) & 1) != 0;
                entry.key_is_data = ((raw >> 51) & 1) != 0;
            }
            else
            {
                entry.high8 = static_cast<uint8_t>((raw >> 34) & 0xFF);
            }

            return entry;
        }

        // What dyld's own code does two instructions before the autda that motivated all of this:
        //   mov x17, x24 ; movk x17, #0x9abf, lsl #48
        // the address with its top 16 bits replaced by the diversity value.
        uint64_t blend_discriminator(const uint64_t address, const uint16_t diversity)
        {
            return (address & 0x0000FFFFFFFFFFFFull) | (static_cast<uint64_t>(diversity) << 48);
        }

    }

    bool apply_dyld_cache_slide_info(macos_emulator& emu, const uint64_t target, const size_t length, const uint64_t slide_start,
                                     uint64_t& applied, const uint64_t restrict_begin, const uint64_t restrict_end,
                                     const dyld_slide_metadata_reader& read_metadata)
    {
        const auto read_slide = [&](const uint64_t address, void* destination, const size_t size) {
            if (read_metadata)
            {
                return read_metadata(address, std::span{static_cast<std::byte*>(destination), size});
            }

            return emu.memory.try_read_memory(address, destination, size);
        };

        slide_info_header header{};
        if (!read_slide(slide_start, &header.version, sizeof(header.version)) ||
            !read_slide(slide_start + 4, &header.page_size, sizeof(header.page_size)) ||
            !read_slide(slide_start + 8, &header.page_starts_count, sizeof(header.page_starts_count)) ||
            !read_slide(slide_start + 16, &header.value_add, sizeof(header.value_add)))
        {
            return false;
        }

        if (header.version != SLIDE_INFO_VERSION_5 || header.page_size == 0 || (header.page_size % MACOS_PAGE_SIZE) != 0)
        {
            emu.log.warn("unsupported dyld cache slide info version %u\n", header.version);
            return false;
        }

        std::vector<uint8_t> page(header.page_size);

        for (uint32_t index = 0; index < header.page_starts_count; ++index)
        {
            const auto page_base = target + static_cast<uint64_t>(index) * header.page_size;
            if (page_base + header.page_size > target + length)
            {
                break;
            }

            // Each page's chain is self-contained -- its starting offset comes from this page's own
            // page_starts entry and every step stays inside the page -- so applying a subset of pages
            // produces exactly what applying all of them would have produced for those pages. That is
            // what lets the pager rebase a chunk at a time instead of the whole cache at once.
            //
            // Wholly inside, not merely overlapping. A page that straddles the boundary would otherwise
            // be rebased by the chunk holding either end, and the write would run past what that chunk
            // has mapped. Cache mappings and chunk boundaries are both 16 KiB aligned, so no page
            // straddles one in practice; the rule is written so that it would not matter if one did.
            if (page_base < restrict_begin || page_base + header.page_size > restrict_end)
            {
                continue;
            }

            uint16_t start = 0;
            if (!read_slide(slide_start + SLIDE_INFO5_HEADER_SIZE + index * sizeof(uint16_t), &start, sizeof(start)))
            {
                return false;
            }

            if (start == SLIDE_PAGE_NO_REBASE)
            {
                continue;
            }

            // One read and one write per page rather than per pointer: a chain can be thousands of
            // entries long and there are thousands of pages.
            if (!emu.memory.try_read_memory(page_base, page.data(), page.size()))
            {
                return false;
            }

            auto offset = static_cast<size_t>(start);
            bool dirty = false;

            while (offset + sizeof(uint64_t) <= page.size())
            {
                uint64_t raw = 0;
                std::memcpy(&raw, page.data() + offset, sizeof(raw));

                const auto entry = decode_slide_entry(raw);
                auto value = header.value_add + entry.runtime_offset;

                if (entry.authenticated)
                {
                    if (emu.pointer_authentication)
                    {
                        const auto location = page_base + offset;
                        const auto discriminator =
                            entry.address_diversified ? blend_discriminator(location, entry.diversity) : entry.diversity;

                        if (!emu.emu().sign_pointer(value, entry.key_is_data ? arm64_pauth_key::data_a : arm64_pauth_key::instruction_a,
                                                    discriminator))
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    value |= static_cast<uint64_t>(entry.high8) << 56;
                }

                std::memcpy(page.data() + offset, &value, sizeof(value));
                dirty = true;
                ++applied;

                if (entry.next == 0)
                {
                    break;
                }

                offset += static_cast<size_t>(entry.next) * sizeof(uint64_t);
            }

            if (dirty && !emu.memory.try_write_memory(page_base, page.data(), page.size()))
            {
                return false;
            }
        }

        return true;
    }

}

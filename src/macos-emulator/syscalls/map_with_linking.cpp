#include "macos_syscalls.hpp"
#include "../macos_commpage.hpp"
#include "../macos_emulator.hpp"
#include "../macos_memory_protection.hpp"

#include <address_utils.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

// Page-in linking as XNU's dyld pager does it (osfmk/vm/vm_dyld_pager.c), restricted to the pointer formats an
// x86-64 process uses and applied to every page when the call is made rather than at page-in.
namespace sogen
{
    namespace
    {
        struct mwl_region
        {
            int32_t fd;
            int32_t protections;
            uint64_t file_offset;
            uint64_t address;
            uint64_t size;
        };

        struct mwl_info_hdr
        {
            uint32_t version;
            uint16_t page_size;
            uint16_t pointer_format;
            uint32_t binds_offset;
            uint32_t binds_count;
            uint32_t chains_offset;
            uint32_t chains_size;
            uint64_t slide;
            uint64_t image_address;
        };

        struct chained_starts_in_segment
        {
            uint32_t size;
            uint16_t page_size;
            uint16_t pointer_format;
            uint64_t segment_offset;
            uint32_t max_valid_pointer;
            uint16_t page_count;
        };

        static_assert(sizeof(mwl_region) == 32);
        static_assert(sizeof(mwl_info_hdr) == 40);
        static_assert(offsetof(chained_starts_in_segment, page_count) == 20);

        constexpr size_t SEGMENT_PAGE_STARTS_OFFSET = 22;
        constexpr uint32_t MAX_REGION_COUNT = 5;
        constexpr uint32_t MAX_LINK_INFO_SIZE = 0x4000000;
        constexpr uint64_t MACOS_PAGE_SIZE = 0x1000;

        constexpr uint16_t DYLD_CHAINED_PTR_64 = 2;
        constexpr uint16_t DYLD_CHAINED_PTR_64_OFFSET = 6;
        constexpr uint16_t DYLD_CHAINED_PTR_START_NONE = 0xFFFF;

        template <typename T>
        T read_blob(const std::span<const std::byte> blob, const uint64_t offset)
        {
            if (offset > blob.size() || blob.size() - offset < sizeof(T))
            {
                throw std::runtime_error("Page-in linking info out of range");
            }

            T value{};
            std::memcpy(&value, blob.data() + offset, sizeof(T));
            return value;
        }

        struct segment_chains
        {
            chained_starts_in_segment header{};
            uint64_t page_starts_offset{};
        };

        std::optional<segment_chains> find_segment(const std::span<const std::byte> blob, const mwl_info_hdr& hdr, const uint64_t address)
        {
            const auto segment_count = read_blob<uint32_t>(blob, hdr.chains_offset);
            for (uint32_t i = 0; i < segment_count; ++i)
            {
                const uint64_t chains_offset = hdr.chains_offset;
                const auto segment_offset = chains_offset + read_blob<uint32_t>(blob, chains_offset + (sizeof(uint32_t) * (1 + i)));
                const auto segment = read_blob<chained_starts_in_segment>(blob, segment_offset);

                const auto start = hdr.image_address + segment.segment_offset;
                const auto end = start + (static_cast<uint64_t>(segment.page_count) * segment.page_size);
                if (address >= start && address < end)
                {
                    return segment_chains{.header = segment, .page_starts_offset = segment_offset + SEGMENT_PAGE_STARTS_OFFSET};
                }
            }

            return std::nullopt;
        }

        void fixup_page64(const std::span<std::byte> page, const std::span<const std::byte> blob, const mwl_info_hdr& hdr,
                          const uint16_t first_start)
        {
            if (first_start == DYLD_CHAINED_PTR_START_NONE)
            {
                return;
            }

            const auto target_adjust = hdr.pointer_format == DYLD_CHAINED_PTR_64_OFFSET ? hdr.image_address : hdr.slide;

            uint64_t offset = first_start;
            uint64_t delta = 0;
            do
            {
                if (offset + sizeof(uint64_t) > page.size())
                {
                    throw std::runtime_error("Fixup chain out of page");
                }

                uint64_t value{};
                std::memcpy(&value, page.data() + offset, sizeof(value));

                delta = (value >> 51) & 0xFFF;
                if (value & 0x8000000000000000ULL)
                {
                    const auto ordinal = static_cast<uint32_t>(value & 0x00FFFFFF);
                    if (ordinal >= hdr.binds_count)
                    {
                        throw std::runtime_error("Bind ordinal out of range");
                    }

                    value = read_blob<uint64_t>(blob, hdr.binds_offset + (static_cast<uint64_t>(ordinal) * sizeof(uint64_t))) +
                            ((value >> 24) & 0xFF);
                }
                else
                {
                    value = (value & 0xFFFFFFFFFULL) + target_adjust + (((value >> 36) & 0xFF) << 56);
                }

                std::memcpy(page.data() + offset, &value, sizeof(value));

                if (delta * 4 >= page.size())
                {
                    throw std::runtime_error("Fixup chain delta too large");
                }

                offset += delta * 4;
            } while (delta != 0);
        }

        struct linked_region
        {
            const posix_fd* file{};
            uint64_t file_offset{};
            uint64_t address{};
            size_t size{};
            memory_permission permissions{};
        };

        linked_region validate_region(macos_emulator& emu, const mwl_region& region)
        {
            const auto* file = emu.fds.get(region.fd);
            if (file == nullptr || file->type != fd_type::file || file->is_standard_stream() || region.size == 0 ||
                page_align_down(region.address) != region.address)
            {
                throw std::runtime_error("Invalid page-in linking region");
            }

            std::error_code ec{};
            const auto file_size = page_align_up(std::filesystem::file_size(path_from_utf8(file->host_path), ec));
            if (ec)
            {
                throw std::runtime_error("Page-in linking region file has no size");
            }

            if (region.size > file_size)
            {
                throw std::runtime_error("Page-in linking region exceeds its file");
            }

            const auto size = page_align_up(region.size);
            if (region.file_offset > file_size - size || !std::in_range<size_t>(size) || region.address > MACOS_USER_ADDRESS_END ||
                size > MACOS_USER_ADDRESS_END - region.address)
            {
                throw std::runtime_error("Page-in linking region exceeds its file or the user address space");
            }

            return {
                .file = file,
                .file_offset = region.file_offset,
                .address = region.address,
                .size = static_cast<size_t>(size),
                .permissions = to_memory_permission(static_cast<uint32_t>(region.protections)),
            };
        }

        template <typename PageHandler>
        void link_pages(const linked_region& region, const std::span<const std::byte> blob, const mwl_info_hdr& hdr,
                        const PageHandler& handle_page)
        {
            std::array<std::byte, MACOS_PAGE_SIZE> page{};
            for (uint64_t page_offset = 0; page_offset < region.size; page_offset += MACOS_PAGE_SIZE)
            {
                page.fill(std::byte{0});
                region.file->read_at(region.file_offset + page_offset, page);

                const auto address = region.address + page_offset;
                if (const auto segment = find_segment(blob, hdr, address))
                {
                    const auto page_index = (address - hdr.image_address - segment->header.segment_offset) / MACOS_PAGE_SIZE;
                    if (page_index >= segment->header.page_count)
                    {
                        throw std::runtime_error("Page-in linking page index out of range");
                    }

                    const auto first_start = read_blob<uint16_t>(blob, segment->page_starts_offset + (page_index * sizeof(uint16_t)));
                    fixup_page64(page, blob, hdr, first_start);
                }

                handle_page(address, std::span<const std::byte>{page});
            }
        }
    }

    void sys_map_with_linking_np(const macos_syscall_context& c)
    {
        const auto regions_pointer = get_macos_syscall_argument(c.emu, 0);
        const auto region_count = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 1));
        const auto blob_pointer = get_macos_syscall_argument(c.emu, 2);
        const auto blob_size = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 3));

        if (region_count == 0 || region_count > MAX_REGION_COUNT || blob_size < sizeof(mwl_info_hdr) || blob_size > MAX_LINK_INFO_SIZE)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        std::vector<mwl_region> regions(region_count);
        std::vector<std::byte> blob(blob_size);
        if (!c.emu.try_read_memory(regions_pointer, regions.data(), regions.size() * sizeof(mwl_region)) ||
            !c.emu.try_read_memory(blob_pointer, blob.data(), blob.size()))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto hdr = read_blob<mwl_info_hdr>(blob, 0);
        if (hdr.page_size != MACOS_PAGE_SIZE ||
            (hdr.pointer_format != DYLD_CHAINED_PTR_64 && hdr.pointer_format != DYLD_CHAINED_PTR_64_OFFSET))
        {
            c.emu_ref.log.error("Unsupported page-in linking info (page size 0x%x, pointer format %u)\n", hdr.page_size,
                                hdr.pointer_format);
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        try
        {
            std::vector<linked_region> linked_regions{};
            linked_regions.reserve(regions.size());
            for (const auto& region : regions)
            {
                linked_regions.push_back(validate_region(c.emu_ref, region));
            }

            // Every page is linked once before anything is mapped, so a malformed chain fails the call without touching
            // the address space and no region has to be held in host memory.
            for (const auto& region : linked_regions)
            {
                link_pages(region, blob, hdr, [](uint64_t, std::span<const std::byte>) {});
            }

            auto& memory = c.emu_ref.memory;
            for (const auto& region : linked_regions)
            {
                if (!memory.replace_memory(region.address, region.size, memory_permission::read_write))
                {
                    throw std::runtime_error("Failed to map page-in linking region");
                }

                link_pages(region, blob, hdr, [&memory](const uint64_t address, const std::span<const std::byte> page) {
                    memory.write_memory(address, page.data(), page.size());
                });
                memory.protect_memory(region.address, region.size, region.permissions);
            }
        }
        catch (const std::exception& e)
        {
            c.emu_ref.log.error("map_with_linking_np failed: %s\n", e.what());
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}

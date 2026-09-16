#include "../std_include.hpp"
#include "macho_mapping.hpp"

#include <utils/string.hpp>

#include <algorithm>
#include <limits>

namespace sogen
{
    using namespace macho; // NOLINT(google-build-using-namespace)

    namespace
    {
        uint64_t checked_add_or_throw(const uint64_t lhs, const uint64_t rhs, const std::string& message)
        {
            if (lhs > std::numeric_limits<uint64_t>::max() - rhs)
            {
                throw std::runtime_error(message);
            }

            return lhs + rhs;
        }

        uint64_t checked_page_align_up_or_throw(const uint64_t value, const std::string& message)
        {
            checked_add_or_throw(value, MACOS_PAGE_SIZE - 1, message);
            return page_align_up(value, MACOS_PAGE_SIZE);
        }

        bool range_within_data(const std::span<const std::byte> data, const uint64_t offset, const uint64_t size)
        {
            const auto data_size = static_cast<uint64_t>(data.size());
            return offset <= data_size && size <= data_size - offset;
        }

        // The segment's file range is bounded by the SLICE extent, not by the whole buffer. Bounding against
        // the whole buffer lets a crafted fat file place a segment inside a different slice: in bounds, but
        // sogen would then map slice B's bytes while reporting slice A. fileoff is bounded even when
        // filesize is 0, because slice_offset + fileoff is still recorded and would otherwise be free to wrap.
        void validate_segment(const std::span<const std::byte> data, const segment_command_64& segment, const uint64_t slice_offset,
                              const std::filesystem::path& path)
        {
            if (segment.filesize > segment.vmsize)
            {
                throw std::runtime_error("Mach-O segment file size exceeds memory size: " + path.string());
            }

            checked_add_or_throw(segment.vmaddr, segment.vmsize, "Invalid Mach-O segment memory range: " + path.string());

            const auto slice = get_slice(data, slice_offset);
            if (!slice || slice->empty())
            {
                throw std::runtime_error("Mach-O slice extent is invalid: " + path.string());
            }

            if (!range_within_data(*slice, segment.fileoff, segment.filesize))
            {
                throw std::runtime_error("Mach-O segment file range escapes the image: " + path.string());
            }
        }
    }

    uint64_t select_macho_slice(const std::span<const std::byte> data, const std::filesystem::path& path)
    {
        if (!has_macho_magic(data))
        {
            throw std::runtime_error("Not a Mach-O image: " + path.string());
        }

        if (!is_fat(data))
        {
            return 0;
        }

        if (const auto arm64e = find_fat_slice(data, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64E))
        {
            return *arm64e;
        }

        if (const auto arm64 = find_fat_slice(data, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL))
        {
            return *arm64;
        }

        throw std::runtime_error("No arm64 or arm64e slice in universal binary: " + path.string());
    }

    macos_mapped_module read_macho_module_metadata(const std::span<const std::byte> data, const std::filesystem::path& path,
                                                   const uint64_t slice_offset, const uint64_t image_base)
    {
        const auto* header = get_header(data, slice_offset);
        if (!header)
        {
            throw std::runtime_error("Invalid Mach-O header: " + path.string());
        }

        if (header->cputype != CPU_TYPE_ARM64)
        {
            throw std::runtime_error("Unsupported Mach-O cputype: " + std::to_string(header->cputype));
        }

        if (header->filetype != MH_EXECUTE && header->filetype != MH_DYLINKER && header->filetype != MH_DYLIB &&
            header->filetype != MH_BUNDLE)
        {
            throw std::runtime_error("Unsupported Mach-O filetype: " + std::to_string(header->filetype));
        }

        const auto preferred_base = mach_header_vmaddr(data, slice_offset);
        if (!preferred_base)
        {
            throw std::runtime_error("No file-backed segment in Mach-O: " + path.string());
        }

        // An unaligned base slides every segment off its page, which breaks the invariant map_macho_from_data
        // relies on: 16 KiB-aligned vmaddr/vmsize means no two segments ever share a page.
        if ((image_base % MACOS_PAGE_SIZE) != 0 || (*preferred_base % MACOS_PAGE_SIZE) != 0)
        {
            throw std::runtime_error("Mach-O image base is not page aligned: " + path.string());
        }

        macos_mapped_module module{};
        module.name = path.filename().string();
        module.path = path;
        module.slice_offset = slice_offset;
        module.preferred_base = *preferred_base;
        module.image_base = image_base ? image_base : *preferred_base;
        module.cpu_type = header->cputype;
        module.cpu_subtype = header->cpusubtype;
        module.file_type = header->filetype;
        module.flags = header->flags;

        const auto slide = module.image_base - module.preferred_base;

        uint64_t vaddr_min = std::numeric_limits<uint64_t>::max();
        uint64_t vaddr_max = 0;

        const auto ok = for_each_load_command(data, slice_offset, [&](const load_command& lc, const std::span<const std::byte> body) {
            switch (lc.cmd)
            {
            case LC_SEGMENT_64: {
                if (body.size() < sizeof(segment_command_64))
                {
                    throw std::runtime_error("Truncated LC_SEGMENT_64 in Mach-O: " + path.string());
                }

                const auto* segment = reinterpret_cast<const segment_command_64*>(body.data());
                validate_segment(data, *segment, slice_offset, path);

                const auto name = segment_name(*segment);
                if (name == SEG_PAGEZERO)
                {
                    module.page_zero_size = segment->vmsize;
                    return true;
                }

                macos_mapped_segment mapped{};
                mapped.name = std::string{name};
                mapped.start = segment->vmaddr + slide;
                mapped.length = static_cast<size_t>(segment->vmsize);
                mapped.file_offset = slice_offset + segment->fileoff;
                mapped.file_length = static_cast<size_t>(segment->filesize);
                mapped.initial_permissions = macho_prot_to_permission(segment->initprot);
                mapped.max_permissions = macho_prot_to_permission(segment->maxprot);
                mapped.flags = segment->flags;

                // validate_segment bounds the UNSLID vmaddr + vmsize; the slid range is what actually gets
                // mapped, and every consumer rounds its end up to a page.
                const auto message = "Invalid Mach-O segment memory range: " + path.string();
                const auto segment_end =
                    checked_page_align_up_or_throw(checked_add_or_throw(mapped.start, mapped.length, message), message);

                vaddr_min = std::min(vaddr_min, page_align_down(mapped.start, MACOS_PAGE_SIZE));
                vaddr_max = std::max(vaddr_max, segment_end);

                module.segments.push_back(std::move(mapped));

                const auto sections = body.subspan(sizeof(segment_command_64));
                for (uint32_t i = 0; i < segment->nsects; ++i)
                {
                    const auto section = read_at<section_64>(sections, static_cast<uint64_t>(i) * sizeof(section_64));
                    if (!section)
                    {
                        throw std::runtime_error("Truncated section table in Mach-O: " + path.string());
                    }

                    module.sections.push_back(macos_mapped_section{
                        .name = std::string{section_name(*section)},
                        .segment_name = std::string{name},
                        .start = section->addr + slide,
                        .length = static_cast<size_t>(section->size),
                    });
                }

                return true;
            }

            case LC_UNIXTHREAD: {
                const auto thread = read_at<thread_command>(body, 0);
                if (!thread || thread->flavor != ARM_THREAD_STATE64)
                {
                    return true;
                }

                const auto state = read_at<arm_thread_state64_t>(body, sizeof(thread_command));
                if (!state)
                {
                    throw std::runtime_error("Truncated LC_UNIXTHREAD in Mach-O: " + path.string());
                }

                module.thread_entry = state->pc;
                return true;
            }

            case LC_MAIN: {
                const auto main_cmd = read_at<entry_point_command>(body, 0);
                if (!main_cmd)
                {
                    throw std::runtime_error("Truncated LC_MAIN in Mach-O: " + path.string());
                }

                module.main_entry_offset = main_cmd->entryoff;
                module.stack_size = main_cmd->stacksize;
                return true;
            }

            case LC_LOAD_DYLINKER:
            case LC_ID_DYLINKER: {
                const auto cmd = read_at<dylinker_command>(body, 0);
                if (cmd)
                {
                    module.dylinker_path = std::string{read_lc_str(body, cmd->name)};
                }

                return true;
            }

            case LC_LOAD_DYLIB: {
                const auto cmd = read_at<dylib_command>(body, 0);
                if (cmd)
                {
                    module.dependent_libraries.emplace_back(read_lc_str(body, cmd->name));
                }

                return true;
            }

            case LC_UUID: {
                const auto cmd = read_at<uuid_command>(body, 0);
                if (cmd)
                {
                    module.uuid = cmd->uuid;
                }

                return true;
            }

            case LC_BUILD_VERSION: {
                const auto cmd = read_at<build_version_command>(body, 0);
                if (cmd)
                {
                    module.platform = cmd->platform;
                    module.min_os = cmd->minos;
                    module.sdk = cmd->sdk;
                }

                return true;
            }

            default:
                return true;
            }
        });

        if (!ok)
        {
            throw std::runtime_error("Malformed Mach-O load commands: " + path.string());
        }

        if (module.segments.empty() || vaddr_min >= vaddr_max)
        {
            throw std::runtime_error("No mappable segments in Mach-O: " + path.string());
        }

        module.image_start = vaddr_min;
        module.size_of_image = vaddr_max - vaddr_min;

        // LC_UNIXTHREAD carries an unslid VA, LC_MAIN an offset from the mach header. Deriving both the same
        // way is the classic mistake.
        if (module.thread_entry)
        {
            module.entry_point = *module.thread_entry + slide;
        }
        else if (module.main_entry_offset)
        {
            module.entry_point = module.image_base + *module.main_entry_offset;
        }
        else
        {
            throw std::runtime_error("Mach-O has neither LC_UNIXTHREAD nor LC_MAIN: " + path.string());
        }

        return module;
    }

    macos_mapped_module map_macho_from_data(macos_memory_manager& memory, const std::span<const std::byte> data,
                                            const std::filesystem::path& path, const uint64_t forced_base)
    {
        const auto slice_offset = select_macho_slice(data, path);
        auto module = read_macho_module_metadata(data, path, slice_offset, forced_base);

        if (module.page_zero_size > 0 && !memory.reserve_memory(0, static_cast<size_t>(module.page_zero_size)))
        {
            throw std::runtime_error("Failed to reserve __PAGEZERO for Mach-O: " + path.string());
        }

        for (const auto& segment : module.segments)
        {
            const auto start = page_align_down(segment.start, MACOS_PAGE_SIZE);
            const auto size = static_cast<size_t>(page_align_up(segment.start + segment.length, MACOS_PAGE_SIZE) - start);

            // Unlike ELF there is no shared-page fixup: arm64 Mach-O vmaddr/vmsize are 16 KiB aligned, so
            // two segments never land on the same page. Mapping read-write and protecting afterwards is
            // still required, because __TEXT and __LINKEDIT are unwritable in their final state.
            if (!memory.allocate_memory(start, size, memory_permission::read_write))
            {
                throw std::runtime_error("Failed to allocate Mach-O segment " + segment.name + " at 0x" +
                                         utils::string::to_hex_number(start));
            }

            if (segment.file_length > 0)
            {
                memory.write_memory(segment.start, data.data() + static_cast<size_t>(segment.file_offset), segment.file_length);
            }

            if (segment.initial_permissions != memory_permission::read_write &&
                !memory.protect_memory(start, size, segment.initial_permissions))
            {
                throw std::runtime_error("Failed to protect Mach-O segment " + segment.name + " at 0x" +
                                         utils::string::to_hex_number(start));
            }
        }

        return module;
    }
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace sogen
{
    namespace macho
    {
        constexpr uint32_t FAT_MAGIC = 0xcafebabe;
        constexpr uint32_t FAT_CIGAM = 0xbebafeca;
        constexpr uint32_t FAT_MAGIC_64 = 0xcafebabf;
        constexpr uint32_t FAT_CIGAM_64 = 0xbfbafeca;

        constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
        constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;

        constexpr uint32_t CPU_ARCH_ABI64 = 0x01000000;
        constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000c;
        constexpr uint32_t CPU_TYPE_X86_64 = 0x01000007;

        constexpr uint32_t CPU_SUBTYPE_ARM64_ALL = 0;
        constexpr uint32_t CPU_SUBTYPE_ARM64E = 2;
        constexpr uint32_t CPU_SUBTYPE_PTRAUTH_ABI = 0x80000000;
        constexpr uint32_t CPU_SUBTYPE_MASK = 0xff000000;

        constexpr uint32_t MH_EXECUTE = 2;
        constexpr uint32_t MH_DYLIB = 6;
        constexpr uint32_t MH_DYLINKER = 7;
        constexpr uint32_t MH_BUNDLE = 8;

        constexpr uint32_t MH_NOUNDEFS = 0x1;
        constexpr uint32_t MH_DYLDLINK = 0x4;
        constexpr uint32_t MH_TWOLEVEL = 0x80;
        constexpr uint32_t MH_PIE = 0x200000;

        constexpr uint32_t LC_REQ_DYLD = 0x80000000;

        constexpr uint32_t LC_SYMTAB = 0x2;
        constexpr uint32_t LC_UNIXTHREAD = 0x5;
        constexpr uint32_t LC_DYSYMTAB = 0xb;
        constexpr uint32_t LC_LOAD_DYLIB = 0xc;
        constexpr uint32_t LC_ID_DYLIB = 0xd;
        constexpr uint32_t LC_LOAD_DYLINKER = 0xe;
        constexpr uint32_t LC_ID_DYLINKER = 0xf;
        constexpr uint32_t LC_SEGMENT_64 = 0x19;
        constexpr uint32_t LC_UUID = 0x1b;
        constexpr uint32_t LC_CODE_SIGNATURE = 0x1d;
        constexpr uint32_t LC_SEGMENT_SPLIT_INFO = 0x1e;
        constexpr uint32_t LC_FUNCTION_STARTS = 0x26;
        constexpr uint32_t LC_DATA_IN_CODE = 0x29;
        constexpr uint32_t LC_SOURCE_VERSION = 0x2a;
        constexpr uint32_t LC_BUILD_VERSION = 0x32;
        constexpr uint32_t LC_RPATH = 0x8000001c;
        constexpr uint32_t LC_MAIN = 0x80000028;
        constexpr uint32_t LC_DYLD_EXPORTS_TRIE = 0x80000033;
        constexpr uint32_t LC_DYLD_CHAINED_FIXUPS = 0x80000034;

        constexpr uint32_t VM_PROT_READ = 1;
        constexpr uint32_t VM_PROT_WRITE = 2;
        constexpr uint32_t VM_PROT_EXECUTE = 4;

        constexpr uint32_t SG_HIGHVM = 0x1;
        constexpr uint32_t SG_NORELOC = 0x4;
        constexpr uint32_t SG_PROTECTED_VERSION_1 = 0x8;
        constexpr uint32_t SG_READ_ONLY = 0x10;

        constexpr uint32_t PLATFORM_MACOS = 1;
        constexpr uint32_t PLATFORM_MACCATALYST = 6;

        constexpr uint32_t ARM_THREAD_STATE64 = 6;
        constexpr uint32_t ARM_THREAD_STATE64_COUNT = 68;

        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E = 1;
        constexpr uint16_t DYLD_CHAINED_PTR_64 = 2;
        constexpr uint16_t DYLD_CHAINED_PTR_32 = 3;
        constexpr uint16_t DYLD_CHAINED_PTR_32_CACHE = 4;
        constexpr uint16_t DYLD_CHAINED_PTR_32_FIRMWARE = 5;
        constexpr uint16_t DYLD_CHAINED_PTR_64_OFFSET = 6;
        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E_KERNEL = 7;
        constexpr uint16_t DYLD_CHAINED_PTR_64_KERNEL_CACHE = 8;
        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E_USERLAND = 9;
        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E_FIRMWARE = 10;
        constexpr uint16_t DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE = 11;
        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E_USERLAND24 = 12;
        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE = 13;
        constexpr uint16_t DYLD_CHAINED_PTR_ARM64E_SEGMENTED = 14;

        constexpr uint32_t DYLD_CHAINED_IMPORT = 1;
        constexpr uint32_t DYLD_CHAINED_IMPORT_ADDEND = 2;
        constexpr uint32_t DYLD_CHAINED_IMPORT_ADDEND64 = 3;

        constexpr uint16_t DYLD_CHAINED_PTR_START_NONE = 0xFFFF;
        constexpr uint16_t DYLD_CACHE_SLIDE_V5_PAGE_ATTR_NO_REBASE = 0xFFFF;

        constexpr std::string_view SEG_PAGEZERO = "__PAGEZERO";
        constexpr std::string_view SEG_TEXT = "__TEXT";
        constexpr std::string_view SEG_LINKEDIT = "__LINKEDIT";

#pragma pack(push, 1)

        struct fat_header
        {
            uint32_t magic;
            uint32_t nfat_arch;
        };

        static_assert(sizeof(fat_header) == 8);

        struct fat_arch
        {
            uint32_t cputype;
            uint32_t cpusubtype;
            uint32_t offset;
            uint32_t size;
            uint32_t align;
        };

        static_assert(sizeof(fat_arch) == 20);

        struct fat_arch_64
        {
            uint32_t cputype;
            uint32_t cpusubtype;
            uint64_t offset;
            uint64_t size;
            uint32_t align;
            uint32_t reserved;
        };

        static_assert(sizeof(fat_arch_64) == 32);

        struct mach_header_64
        {
            uint32_t magic;
            uint32_t cputype;
            uint32_t cpusubtype;
            uint32_t filetype;
            uint32_t ncmds;
            uint32_t sizeofcmds;
            uint32_t flags;
            uint32_t reserved;
        };

        static_assert(sizeof(mach_header_64) == 32);

        struct load_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
        };

        static_assert(sizeof(load_command) == 8);

        struct segment_command_64
        {
            uint32_t cmd;
            uint32_t cmdsize;
            std::array<char, 16> segname;
            uint64_t vmaddr;
            uint64_t vmsize;
            uint64_t fileoff;
            uint64_t filesize;
            uint32_t maxprot;
            uint32_t initprot;
            uint32_t nsects;
            uint32_t flags;
        };

        static_assert(sizeof(segment_command_64) == 72);

        struct section_64
        {
            std::array<char, 16> sectname;
            std::array<char, 16> segname;
            uint64_t addr;
            uint64_t size;
            uint32_t offset;
            uint32_t align;
            uint32_t reloff;
            uint32_t nreloc;
            uint32_t flags;
            uint32_t reserved1;
            uint32_t reserved2;
            uint32_t reserved3;
        };

        static_assert(sizeof(section_64) == 80);

        struct entry_point_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint64_t entryoff;
            uint64_t stacksize;
        };

        static_assert(sizeof(entry_point_command) == 24);

        struct thread_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint32_t flavor;
            uint32_t count;
        };

        static_assert(sizeof(thread_command) == 16);

        struct arm_thread_state64_t
        {
            std::array<uint64_t, 29> x;
            uint64_t fp;
            uint64_t lr;
            uint64_t sp;
            uint64_t pc;
            uint32_t cpsr;
            uint32_t pad;
        };

        static_assert(sizeof(arm_thread_state64_t) == 272);
        static_assert(sizeof(arm_thread_state64_t) / sizeof(uint32_t) == ARM_THREAD_STATE64_COUNT);

        struct dylinker_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint32_t name;
        };

        static_assert(sizeof(dylinker_command) == 12);

        struct dylib_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint32_t name;
            uint32_t timestamp;
            uint32_t current_version;
            uint32_t compatibility_version;
        };

        static_assert(sizeof(dylib_command) == 24);

        struct uuid_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            std::array<uint8_t, 16> uuid;
        };

        static_assert(sizeof(uuid_command) == 24);

        struct build_version_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint32_t platform;
            uint32_t minos;
            uint32_t sdk;
            uint32_t ntools;
        };

        static_assert(sizeof(build_version_command) == 24);

        struct linkedit_data_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint32_t dataoff;
            uint32_t datasize;
        };

        static_assert(sizeof(linkedit_data_command) == 16);

        struct symtab_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            uint32_t symoff;
            uint32_t nsyms;
            uint32_t stroff;
            uint32_t strsize;
        };

        static_assert(sizeof(symtab_command) == 24);

        struct dysymtab_command
        {
            uint32_t cmd;
            uint32_t cmdsize;
            std::array<uint32_t, 18> fields;
        };

        static_assert(sizeof(dysymtab_command) == 80);

        struct dyld_chained_fixups_header
        {
            uint32_t fixups_version;
            uint32_t starts_offset;
            uint32_t imports_offset;
            uint32_t symbols_offset;
            uint32_t imports_count;
            uint32_t imports_format;
            uint32_t symbols_format;
        };

        static_assert(sizeof(dyld_chained_fixups_header) == 28);

        // max_valid_pointer precedes page_count, and page_start is a trailing array of page_count entries of
        // which only the first is declared here. Swapping the two leaves page_count reading as 0 and a chain
        // walk silently finding nothing.
        struct dyld_chained_starts_in_segment
        {
            uint32_t size;
            uint16_t page_size;
            uint16_t pointer_format;
            uint64_t segment_offset;
            uint32_t max_valid_pointer;
            uint16_t page_count;
            std::array<uint16_t, 1> page_start;
        };

        static_assert(sizeof(dyld_chained_starts_in_segment) == 24);

#pragma pack(pop)

        constexpr uint32_t bswap32(const uint32_t value)
        {
            return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) | ((value & 0x00ff0000u) >> 8) |
                   ((value & 0xff000000u) >> 24);
        }

        constexpr uint64_t bswap64(const uint64_t value)
        {
            return (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(value))) << 32) | bswap32(static_cast<uint32_t>(value >> 32));
        }

        template <typename T>
        std::optional<T> read_at(const std::span<const std::byte> data, const uint64_t offset)
        {
            static_assert(std::is_trivially_copyable_v<T>);

            if (offset > data.size() || sizeof(T) > data.size() - offset)
            {
                return std::nullopt;
            }

            T value{};
            std::memcpy(&value, data.data() + static_cast<size_t>(offset), sizeof(T));
            return value;
        }

        // Fat headers are stored big-endian regardless of the endianness of the slices they carry, so on a
        // little-endian host a conforming file's magic reads back as FAT_CIGAM rather than FAT_MAGIC.
        inline bool is_fat(const std::span<const std::byte> data)
        {
            const auto magic = read_at<uint32_t>(data, 0);
            return magic && (*magic == FAT_MAGIC || *magic == FAT_CIGAM || *magic == FAT_MAGIC_64 || *magic == FAT_CIGAM_64);
        }

        inline bool has_macho_magic(const std::span<const std::byte> data)
        {
            const auto magic = read_at<uint32_t>(data, 0);
            if (!magic)
            {
                return false;
            }

            return *magic == MH_MAGIC_64 || is_fat(data);
        }

        struct fat_slice
        {
            uint32_t cputype;
            uint32_t cpusubtype;
            uint64_t offset;
            uint64_t size;
        };

        template <typename Callback>
        bool for_each_fat_arch(const std::span<const std::byte> data, Callback&& callback)
        {
            const auto magic = read_at<uint32_t>(data, 0);
            if (!magic || !is_fat(data))
            {
                return false;
            }

            const auto wide = *magic == FAT_MAGIC_64 || *magic == FAT_CIGAM_64;
            const auto swapped = *magic == FAT_CIGAM || *magic == FAT_CIGAM_64;

            const auto to_host32 = [swapped](const uint32_t value) { return swapped ? bswap32(value) : value; };
            const auto to_host64 = [swapped](const uint64_t value) { return swapped ? bswap64(value) : value; };

            const auto count_field = read_at<uint32_t>(data, 4);
            if (!count_field)
            {
                return false;
            }

            const auto count = to_host32(*count_field);
            const uint64_t entry_size = wide ? sizeof(fat_arch_64) : sizeof(fat_arch);

            for (uint32_t i = 0; i < count; ++i)
            {
                const auto entry_offset = sizeof(fat_header) + static_cast<uint64_t>(i) * entry_size;

                fat_slice slice{};

                if (wide)
                {
                    const auto entry = read_at<fat_arch_64>(data, entry_offset);
                    if (!entry)
                    {
                        return false;
                    }

                    slice.cputype = to_host32(entry->cputype);
                    slice.cpusubtype = to_host32(entry->cpusubtype);
                    slice.offset = to_host64(entry->offset);
                    slice.size = to_host64(entry->size);
                }
                else
                {
                    const auto entry = read_at<fat_arch>(data, entry_offset);
                    if (!entry)
                    {
                        return false;
                    }

                    slice.cputype = to_host32(entry->cputype);
                    slice.cpusubtype = to_host32(entry->cpusubtype);
                    slice.offset = to_host32(entry->offset);
                    slice.size = to_host32(entry->size);
                }

                if (slice.offset > data.size() || slice.size > data.size() - slice.offset)
                {
                    return false;
                }

                if (!callback(slice))
                {
                    return true;
                }
            }

            return true;
        }

        inline std::optional<uint64_t> find_fat_slice(const std::span<const std::byte> data, const uint32_t cputype,
                                                      const uint32_t cpusubtype)
        {
            std::optional<uint64_t> result{};

            for_each_fat_arch(data, [&](const fat_slice& slice) {
                if (slice.cputype != cputype || (slice.cpusubtype & ~CPU_SUBTYPE_MASK) != (cpusubtype & ~CPU_SUBTYPE_MASK))
                {
                    return true;
                }

                result = slice.offset;
                return false;
            });

            return result;
        }

        // A slice must be parsed within its own extent: bounding against the whole buffer would let a crafted fat
        // file point one slice's command table into the bytes of another and be parsed as if they were its own.
        inline std::optional<std::span<const std::byte>> get_slice(const std::span<const std::byte> data, const uint64_t slice_offset)
        {
            if (slice_offset > data.size())
            {
                return std::nullopt;
            }

            if (!is_fat(data))
            {
                return data.subspan(static_cast<size_t>(slice_offset));
            }

            std::optional<std::span<const std::byte>> result{};

            for_each_fat_arch(data, [&](const fat_slice& slice) {
                if (slice.offset != slice_offset)
                {
                    return true;
                }

                result = data.subspan(static_cast<size_t>(slice.offset), static_cast<size_t>(slice.size));
                return false;
            });

            return result;
        }

        inline const mach_header_64* get_header(const std::span<const std::byte> data, const uint64_t slice_offset)
        {
            const auto slice = get_slice(data, slice_offset);
            if (!slice || sizeof(mach_header_64) > slice->size())
            {
                return nullptr;
            }

            const auto* header = reinterpret_cast<const mach_header_64*>(slice->data());
            if (header->magic != MH_MAGIC_64)
            {
                return nullptr;
            }

            return header;
        }

        inline std::optional<std::span<const std::byte>> get_load_commands(const std::span<const std::byte> data,
                                                                           const uint64_t slice_offset)
        {
            const auto* header = get_header(data, slice_offset);
            const auto slice = get_slice(data, slice_offset);
            if (!header || !slice)
            {
                return std::nullopt;
            }

            constexpr uint64_t commands_offset = sizeof(mach_header_64);
            if (commands_offset > slice->size() || header->sizeofcmds > slice->size() - commands_offset)
            {
                return std::nullopt;
            }

            return slice->subspan(static_cast<size_t>(commands_offset), header->sizeofcmds);
        }

        template <typename Callback>
        bool for_each_load_command(const std::span<const std::byte> data, const uint64_t slice_offset, Callback&& callback)
        {
            const auto* header = get_header(data, slice_offset);
            const auto commands = get_load_commands(data, slice_offset);
            if (!header || !commands)
            {
                return false;
            }

            uint64_t offset = 0;
            for (uint32_t i = 0; i < header->ncmds; ++i)
            {
                const auto command = read_at<load_command>(*commands, offset);
                if (!command)
                {
                    return false;
                }

                if (command->cmdsize < sizeof(load_command) || (command->cmdsize % 8) != 0)
                {
                    return false;
                }

                if (command->cmdsize > commands->size() - offset)
                {
                    return false;
                }

                const auto body = commands->subspan(static_cast<size_t>(offset), command->cmdsize);
                const auto* typed = reinterpret_cast<const load_command*>(body.data());

                if (!callback(*typed, body))
                {
                    return true;
                }

                offset += command->cmdsize;
            }

            return true;
        }

        inline std::string_view fixed_name(const std::array<char, 16>& name)
        {
            const auto length = ::strnlen(name.data(), name.size());
            return std::string_view{name.data(), length};
        }

        inline std::string_view segment_name(const segment_command_64& segment)
        {
            return fixed_name(segment.segname);
        }

        inline std::string_view section_name(const section_64& section)
        {
            return fixed_name(section.sectname);
        }

        inline std::string_view read_lc_str(const std::span<const std::byte> command_body, const uint32_t str_offset)
        {
            if (str_offset >= command_body.size())
            {
                return {};
            }

            const auto* start = reinterpret_cast<const char*>(command_body.data()) + str_offset;
            const auto length = ::strnlen(start, command_body.size() - str_offset);
            return std::string_view{start, length};
        }

        inline const segment_command_64* find_segment(const std::span<const std::byte> data, const uint64_t slice_offset,
                                                      const std::string_view name)
        {
            const segment_command_64* result = nullptr;

            for_each_load_command(data, slice_offset, [&](const load_command& lc, const std::span<const std::byte> body) {
                if (lc.cmd != LC_SEGMENT_64 || body.size() < sizeof(segment_command_64))
                {
                    return true;
                }

                const auto* segment = reinterpret_cast<const segment_command_64*>(body.data());
                if (segment_name(*segment) != name)
                {
                    return true;
                }

                result = segment;
                return false;
            });

            return result;
        }

        // Every chained-fixup target is relative to the mach-header vmaddr, which is the first file-backed
        // segment's vmaddr and not segment 0's: with __PAGEZERO present, segment 0 sits at vmaddr 0.
        inline std::optional<uint64_t> mach_header_vmaddr(const std::span<const std::byte> data, const uint64_t slice_offset)
        {
            std::optional<uint64_t> result{};

            for_each_load_command(data, slice_offset, [&](const load_command& lc, const std::span<const std::byte> body) {
                if (lc.cmd != LC_SEGMENT_64 || body.size() < sizeof(segment_command_64))
                {
                    return true;
                }

                const auto* segment = reinterpret_cast<const segment_command_64*>(body.data());
                if (segment->fileoff != 0 || segment->filesize == 0)
                {
                    return true;
                }

                result = segment->vmaddr;
                return false;
            });

            return result;
        }

        struct chained_ptr_arm64e
        {
            bool auth;
            bool bind;
            uint32_t next;
            uint64_t target;
            uint8_t high8;
            uint32_t ordinal;
            int32_t addend;
            uint16_t diversity;
            bool addr_div;
            uint8_t key;
        };

        // The formats decode_arm64e_pointer understands, which is not every arm64e format. Format 13 places
        // next at 52..62 and has no bind bit, format 14 is a pair of 32-bit words whose bit 62 belongs to next
        // rather than bind; both would decode into confident nonsense here, and an all-zero result would be no
        // better since it reads as a well-formed terminal rebase to offset 0. Callers must gate on this.
        constexpr bool is_arm64e_chained_format(const uint16_t pointer_format)
        {
            switch (pointer_format)
            {
            case DYLD_CHAINED_PTR_ARM64E:
            case DYLD_CHAINED_PTR_ARM64E_KERNEL:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND:
            case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                return true;
            default:
                return false;
            }
        }

        constexpr chained_ptr_arm64e decode_arm64e_pointer(const uint64_t raw, const uint16_t pointer_format)
        {
            chained_ptr_arm64e value{};

            value.auth = ((raw >> 63) & 1) != 0;
            value.bind = ((raw >> 62) & 1) != 0;
            value.next = static_cast<uint32_t>((raw >> 51) & 0x7ff);

            const auto wide_ordinal = pointer_format == DYLD_CHAINED_PTR_ARM64E_USERLAND24;

            if (value.auth)
            {
                value.diversity = static_cast<uint16_t>((raw >> 32) & 0xffff);
                value.addr_div = ((raw >> 48) & 1) != 0;
                value.key = static_cast<uint8_t>((raw >> 49) & 0x3);
            }

            if (value.bind)
            {
                value.ordinal = static_cast<uint32_t>(raw & (wide_ordinal ? 0xffffffu : 0xffffu));

                if (!value.auth)
                {
                    const auto encoded = static_cast<uint32_t>((raw >> 32) & 0x7ffff);
                    value.addend = static_cast<int32_t>(encoded << 13) >> 13;
                }
            }
            else if (value.auth)
            {
                value.target = raw & 0xffffffffu;
            }
            else
            {
                value.target = raw & 0x7ffffffffffull;
                value.high8 = static_cast<uint8_t>((raw >> 43) & 0xff);
            }

            return value;
        }

        struct chained_ptr_64
        {
            bool bind;
            uint32_t next;
            uint64_t target;
            uint8_t high8;
            uint32_t ordinal;
            uint8_t addend;
        };

        // next is 12 bits (51..62) and bind is bit 63, unlike the arm64e formats where next is 11 bits and
        // bind is bit 62. Per dyld_chained_ptr_64_rebase / _bind in <mach-o/fixup-chains.h>; published
        // descriptions giving an 11-bit next here are wrong.
        constexpr chained_ptr_64 decode_64_pointer(const uint64_t raw)
        {
            chained_ptr_64 value{};

            value.bind = ((raw >> 63) & 1) != 0;
            value.next = static_cast<uint32_t>((raw >> 51) & 0xfff);

            if (value.bind)
            {
                value.ordinal = static_cast<uint32_t>(raw & 0xffffffu);
                value.addend = static_cast<uint8_t>((raw >> 24) & 0xff);
            }
            else
            {
                value.target = raw & 0xfffffffffull;
                value.high8 = static_cast<uint8_t>((raw >> 36) & 0xff);
            }

            return value;
        }

        struct cache_slide_ptr5
        {
            bool auth;
            uint32_t next;
            uint64_t runtime_offset;
            uint8_t high8;
            uint16_t diversity;
            bool addr_div;
            bool key_is_data;
        };

        constexpr cache_slide_ptr5 decode_shared_cache_pointer(const uint64_t raw)
        {
            cache_slide_ptr5 value{};

            value.auth = ((raw >> 63) & 1) != 0;
            value.next = static_cast<uint32_t>((raw >> 52) & 0x7ff);
            value.runtime_offset = raw & 0x3ffffffffull;

            if (value.auth)
            {
                value.diversity = static_cast<uint16_t>((raw >> 34) & 0xffff);
                value.addr_div = ((raw >> 50) & 1) != 0;
                value.key_is_data = ((raw >> 51) & 1) != 0;
            }
            else
            {
                value.high8 = static_cast<uint8_t>((raw >> 34) & 0xff);
            }

            return value;
        }

        struct chained_import
        {
            uint8_t lib_ordinal;
            bool weak_import;
            uint32_t name_offset;
        };

        constexpr chained_import decode_chained_import(const uint32_t raw)
        {
            return chained_import{
                .lib_ordinal = static_cast<uint8_t>(raw & 0xff),
                .weak_import = ((raw >> 8) & 1) != 0,
                .name_offset = raw >> 9,
            };
        }

        // The strides are not deducible from the format names: three arm64e formats stride by 4 rather than 8
        // and the x86_64 kernel cache strides by 1. Values are the ones annotated on each enumerator and each
        // next field in <mach-o/fixup-chains.h>. A 0 return means the format is unknown and no chain may be
        // walked with it, since a zero stride would never advance.
        constexpr uint32_t chained_pointer_stride(const uint16_t pointer_format)
        {
            switch (pointer_format)
            {
            case DYLD_CHAINED_PTR_ARM64E:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            case DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE:
                return 8;
            case DYLD_CHAINED_PTR_64:
            case DYLD_CHAINED_PTR_64_OFFSET:
            case DYLD_CHAINED_PTR_32:
            case DYLD_CHAINED_PTR_32_CACHE:
            case DYLD_CHAINED_PTR_32_FIRMWARE:
            case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
            case DYLD_CHAINED_PTR_ARM64E_KERNEL:
            case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
            case DYLD_CHAINED_PTR_ARM64E_SEGMENTED:
                return 4;
            case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
                return 1;
            default:
                return 0;
            }
        }

        // The cache header is not modelled as a struct: it is naturally aligned rather than packed, and Apple
        // appends fields to it every release without a version stamp to gate on (formatVersion is always 0).
        // Named offsets read through read_at give the same access without depending on a sizeof that shifts
        // between OS releases.
        namespace dyld_cache
        {
            constexpr uint64_t MAGIC = 0x00;
            constexpr uint64_t MAPPING_OFFSET = 0x10;
            constexpr uint64_t MAPPING_COUNT = 0x14;
            constexpr uint64_t CODE_SIGNATURE_OFFSET = 0x28;
            constexpr uint64_t CODE_SIGNATURE_SIZE = 0x30;
            constexpr uint64_t UUID = 0x58;
            constexpr uint64_t CACHE_TYPE = 0x68;
            constexpr uint64_t DYLD_IN_CACHE_MH = 0x78;
            constexpr uint64_t DYLD_IN_CACHE_ENTRY = 0x80;
            constexpr uint64_t IMAGES_TEXT_OFFSET = 0x88;
            constexpr uint64_t IMAGES_TEXT_COUNT = 0x90;
            constexpr uint64_t PATCH_INFO_ADDR = 0x98;
            constexpr uint64_t PLATFORM = 0xd8;
            constexpr uint64_t FORMAT_FLAGS = 0xdc;
            constexpr uint64_t SHARED_REGION_START = 0xe0;
            constexpr uint64_t SHARED_REGION_SIZE = 0xe8;
            constexpr uint64_t MAX_SLIDE = 0xf0;
            constexpr uint64_t DYLIBS_TRIE_ADDR = 0x108;
            constexpr uint64_t DYLIBS_TRIE_SIZE = 0x110;
            constexpr uint64_t MAPPING_WITH_SLIDE_OFFSET = 0x138;
            constexpr uint64_t MAPPING_WITH_SLIDE_COUNT = 0x13c;
            constexpr uint64_t OS_VERSION = 0x16c;
            constexpr uint64_t SUBCACHE_ARRAY_OFFSET = 0x188;
            constexpr uint64_t SUBCACHE_ARRAY_COUNT = 0x18c;
            constexpr uint64_t SYMBOL_FILE_UUID = 0x190;
            constexpr uint64_t IMAGES_OFFSET = 0x1c0;
            constexpr uint64_t IMAGES_COUNT = 0x1c4;
            constexpr uint64_t CACHE_SUB_TYPE = 0x1c8;
            constexpr uint64_t OBJC_OPTS_OFFSET = 0x1d0;
            constexpr uint64_t TPRO_MAPPINGS_OFFSET = 0x200;
            constexpr uint64_t TPRO_MAPPINGS_COUNT = 0x204;

            constexpr uint32_t FORMAT_FLAG_DYLIBS_EXPECTED_ON_DISK = 0x100;

            constexpr uint64_t MAPPING_FLAG_AUTH_DATA = 0x1;
            constexpr uint64_t MAPPING_FLAG_CONST_DATA = 0x4;
            constexpr uint64_t MAPPING_FLAG_READ_ONLY_DATA = 0x20;
            constexpr uint64_t MAPPING_FLAG_TPRO = 0x40;

            constexpr uint32_t SLIDE_INFO5_VERSION = 5;
            constexpr uint64_t SLIDE_INFO5_HEADER_SIZE = 24;

            constexpr uint64_t MAGIC_SIZE = 16;
            constexpr std::string_view MAGIC_PREFIX = "dyld_v1";

#pragma pack(push, 1)

            struct dyld_cache_mapping_info
            {
                uint64_t address;
                uint64_t size;
                uint64_t file_offset;
                uint32_t max_prot;
                uint32_t init_prot;
            };

            static_assert(sizeof(dyld_cache_mapping_info) == 32);

            struct dyld_cache_mapping_and_slide_info
            {
                uint64_t address;
                uint64_t size;
                uint64_t file_offset;
                uint64_t slide_info_file_offset;
                uint64_t slide_info_file_size;
                uint64_t flags;
                uint32_t max_prot;
                uint32_t init_prot;
            };

            static_assert(sizeof(dyld_cache_mapping_and_slide_info) == 56);

            struct dyld_cache_image_info
            {
                uint64_t address;
                uint64_t mod_time;
                uint64_t inode;
                uint32_t path_file_offset;
                uint32_t pad;
            };

            static_assert(sizeof(dyld_cache_image_info) == 32);

            // 32, not the 40 some references give. At 40 the image walk decodes garbage from entry 1 onwards and
            // never finds the entry whose load_address equals the header's dyldInCacheMH.
            struct dyld_cache_image_text_info
            {
                std::array<uint8_t, 16> uuid;
                uint64_t load_address;
                uint32_t text_segment_size;
                uint32_t path_offset;
            };

            static_assert(sizeof(dyld_cache_image_text_info) == 32);

            struct dyld_subcache_entry
            {
                std::array<uint8_t, 16> uuid;
                uint64_t cache_vm_offset;
                std::array<char, 32> file_suffix;
            };

            static_assert(sizeof(dyld_subcache_entry) == 56);

            struct dyld_cache_slide_info5
            {
                uint32_t version;
                uint32_t page_size;
                uint32_t page_starts_count;
                uint32_t pad;
                uint64_t value_add;
            };

            static_assert(sizeof(dyld_cache_slide_info5) == SLIDE_INFO5_HEADER_SIZE);

#pragma pack(pop)

            inline bool has_magic(const std::span<const std::byte> data)
            {
                if (data.size() < MAGIC_SIZE)
                {
                    return false;
                }

                const std::string_view text{reinterpret_cast<const char*>(data.data()), MAGIC_PREFIX.size()};
                return text == MAGIC_PREFIX;
            }

            inline std::string_view architecture(const std::span<const std::byte> data)
            {
                if (!has_magic(data))
                {
                    return {};
                }

                std::string_view text{reinterpret_cast<const char*>(data.data()), MAGIC_SIZE};
                text = text.substr(0, text.find('\0'));
                text.remove_prefix(MAGIC_PREFIX.size());

                const auto start = text.find_first_not_of(' ');
                if (start == std::string_view::npos)
                {
                    return {};
                }

                return text.substr(start);
            }

        } // namespace dyld_cache

    } // namespace macho
} // namespace sogen

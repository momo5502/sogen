#include "macos_emulator.hpp"
#include "macho.hpp"
#include "macos_commpage.hpp"
#include "macos_memory_protection.hpp"

#include <address_utils.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace sogen
{
    namespace
    {
        // MACH_VM_MAX_ADDRESS for x86-64; the commpage starts right above it.
        constexpr memory_layout MACOS_MEMORY_LAYOUT{
            .allocation_granularity = 0x1000,
            .min_mmap_address = 0x100000000,
            .max_mmap_end = 0x7FFFFFE00000,
            .default_mmap_base = 0x100000000,
        };

        // VM_USRSTACK64 and MAXSSIZ: XNU reserves 64 MiB for the main thread stack below this top.
        constexpr uint64_t STACK_TOP = 0x7FF7BFF00000;
        constexpr uint64_t STACK_SIZE = 0x4000000;
        constexpr uint64_t STACK_RLIMIT = 0x7FC000;

        constexpr uint64_t INITIAL_RFLAGS = 0x202;

        constexpr std::string_view SHARED_CACHE_PATH = "/System/Library/dyld/dyld_shared_cache_x86_64";

        std::vector<std::byte> read_binary_file(const std::filesystem::path& path)
        {
            std::vector<std::byte> image{};
            if (!utils::io::read_file(path, &image))
            {
                throw std::runtime_error("Failed to read " + path.string());
            }

            return image;
        }

        template <typename T>
        T read_struct(const std::span<const std::byte> image, const uint64_t offset)
        {
            if (offset > image.size() || image.size() - offset < sizeof(T))
            {
                throw std::runtime_error("Truncated Mach-O image");
            }

            T value{};
            std::memcpy(&value, image.data() + offset, sizeof(T));
            return value;
        }

        // Universal (fat) headers are stored big-endian regardless of the slices they describe.
        uint64_t read_big_endian(const std::span<const std::byte> image, const uint64_t offset, const size_t size)
        {
            if (offset > image.size() || image.size() - offset < size)
            {
                throw std::runtime_error("Truncated universal binary header");
            }

            uint64_t value = 0;
            for (const auto byte : image.subspan(static_cast<size_t>(offset), size))
            {
                value = (value << 8) | static_cast<uint8_t>(byte);
            }

            return value;
        }

        std::span<const std::byte> select_x86_64_slice(const std::span<const std::byte> image)
        {
            if (image.size() < sizeof(uint32_t))
            {
                return image;
            }

            const auto magic = read_big_endian(image, 0, sizeof(uint32_t));
            if (magic != macho::FAT_MAGIC && magic != macho::FAT_MAGIC_64)
            {
                return image;
            }

            const auto is_fat_64 = magic == macho::FAT_MAGIC_64;
            const auto arch_size = is_fat_64 ? macho::FAT_ARCH_64_SIZE : macho::FAT_ARCH_SIZE;
            const auto field_size = is_fat_64 ? sizeof(uint64_t) : sizeof(uint32_t);
            const auto arch_count = read_big_endian(image, sizeof(uint32_t), sizeof(uint32_t));

            for (uint64_t i = 0; i < arch_count; ++i)
            {
                const auto arch_offset = macho::FAT_HEADER_SIZE + (i * arch_size);
                if (read_big_endian(image, arch_offset, sizeof(uint32_t)) != macho::CPU_TYPE_X86_64)
                {
                    continue;
                }

                const auto slice_offset = read_big_endian(image, arch_offset + (2 * sizeof(uint32_t)), field_size);
                const auto slice_size = read_big_endian(image, arch_offset + (2 * sizeof(uint32_t)) + field_size, field_size);
                if (slice_offset > image.size() || image.size() - slice_offset < slice_size)
                {
                    throw std::runtime_error("Universal binary slice exceeds file bounds");
                }

                return image.subspan(static_cast<size_t>(slice_offset), static_cast<size_t>(slice_size));
            }

            throw std::runtime_error("Universal binary has no x86-64 slice");
        }

        struct mapped_image
        {
            uint64_t header_address{};
            uint64_t end_address{};
            std::optional<uint64_t> unixthread_entry_point{};
            std::optional<std::string> dylinker_path{};
            std::array<uint8_t, 16> uuid{};
        };

        uint64_t validate_segment_range(const macho::segment_command_64& segment, const uint64_t slide)
        {
            if (slide > MACOS_USER_ADDRESS_END || segment.vmaddr > MACOS_USER_ADDRESS_END - slide ||
                segment.vmsize > MACOS_USER_ADDRESS_END)
            {
                throw std::runtime_error("Mach-O segment lies outside the user address space");
            }

            const auto address = segment.vmaddr + slide;
            const auto size = page_align_up(segment.vmsize);
            if (size > MACOS_USER_ADDRESS_END - address || !std::in_range<size_t>(size))
            {
                throw std::runtime_error("Mach-O segment lies outside the user address space");
            }

            return address + size;
        }

        void map_segment(posix_memory_manager& memory, const std::span<const std::byte> image, const macho::segment_command_64& segment,
                         const uint64_t slide)
        {
            if (segment.vmsize == 0 || segment.initprot == 0)
            {
                return;
            }

            if (segment.filesize > segment.vmsize || segment.fileoff > image.size() || image.size() - segment.fileoff < segment.filesize)
            {
                throw std::runtime_error("Mach-O segment exceeds file bounds");
            }

            const auto address = segment.vmaddr + slide;
            const auto size = page_align_up(segment.vmsize);
            if (!memory.allocate_memory(address, static_cast<size_t>(size), memory_permission::read_write))
            {
                throw std::runtime_error("Mach-O segment overlaps existing memory");
            }

            memory.write_memory(address, image.data() + static_cast<size_t>(segment.fileoff), static_cast<size_t>(segment.filesize));
            memory.protect_memory(address, static_cast<size_t>(size), to_memory_permission(segment.initprot));
        }

        uint64_t read_unixthread_entry_point(const std::span<const std::byte> command_data)
        {
            const auto thread = read_struct<macho::thread_command>(command_data, 0);
            if (thread.flavor != macho::X86_THREAD_STATE64)
            {
                throw std::runtime_error("Unsupported LC_UNIXTHREAD flavor");
            }

            return read_struct<macho::x86_thread_state64>(command_data, sizeof(thread)).rip;
        }

        std::string read_dylinker_path(const std::span<const std::byte> command_data)
        {
            const auto command = read_struct<macho::dylinker_command>(command_data, 0);
            if (command.name_offset >= command_data.size())
            {
                throw std::runtime_error("Invalid LC_LOAD_DYLINKER name offset");
            }

            std::string path{};
            for (const auto byte : command_data.subspan(command.name_offset))
            {
                if (byte == std::byte{0})
                {
                    break;
                }

                path.push_back(static_cast<char>(byte));
            }

            return path;
        }

        mapped_image map_image(posix_memory_manager& memory, const std::span<const std::byte> image, const uint32_t expected_file_type,
                               const uint64_t slide)
        {
            const auto header = read_struct<macho::mach_header_64>(image, 0);
            if (header.magic != macho::MH_MAGIC_64 || header.cputype != macho::CPU_TYPE_X86_64)
            {
                throw std::runtime_error("Not an x86-64 Mach-O image");
            }

            if (header.filetype != expected_file_type)
            {
                throw std::runtime_error("Unexpected Mach-O file type");
            }

            if (header.sizeofcmds > image.size() - sizeof(header))
            {
                throw std::runtime_error("Mach-O load commands exceed the file");
            }

            const auto commands = image.subspan(sizeof(header), header.sizeofcmds);

            mapped_image result{};
            std::optional<uint64_t> header_address{};
            uint64_t offset = 0;

            for (uint32_t i = 0; i < header.ncmds; ++i)
            {
                const auto command = read_struct<macho::load_command>(commands, offset);
                if (command.cmdsize < sizeof(command) || command.cmdsize > commands.size() - offset)
                {
                    throw std::runtime_error("Invalid Mach-O load command size");
                }

                const auto command_data = commands.subspan(static_cast<size_t>(offset), command.cmdsize);
                switch (command.cmd)
                {
                case macho::LC_SEGMENT_64: {
                    const auto segment = read_struct<macho::segment_command_64>(command_data, 0);
                    const auto segment_end = validate_segment_range(segment, slide);
                    map_segment(memory, image, segment, slide);
                    result.end_address = std::max(result.end_address, segment_end);
                    if (segment.fileoff == 0 && segment.filesize != 0)
                    {
                        header_address = segment.vmaddr + slide;
                    }
                    break;
                }
                case macho::LC_UNIXTHREAD:
                    result.unixthread_entry_point = read_unixthread_entry_point(command_data) + slide;
                    break;
                case macho::LC_UUID:
                    result.uuid = read_struct<std::array<uint8_t, 16>>(command_data, sizeof(command));
                    break;
                case macho::LC_LOAD_DYLINKER:
                    result.dylinker_path = read_dylinker_path(command_data);
                    break;
                default:
                    break;
                }

                offset += command.cmdsize;
            }

            if (!header_address)
            {
                throw std::runtime_error("Mach-O image has no segment mapping its header");
            }

            result.header_address = *header_address;
            return result;
        }

        std::string to_hex(const uint64_t value)
        {
            return "0x" + utils::string::to_hex_number(value);
        }

        // XNU randomizes stack_guard, malloc_entropy and ptr_munge per process. Fixed values keep emulation
        // deterministic; ptr_munge must be non-zero or libpthread aborts, and stack_guard keeps its second byte zero.
        std::vector<std::string> make_apple_strings(const std::string& executable_path)
        {
            return {
                "executable_path=" + executable_path,
                "stack_guard=0x2f8b6c1d4e9a0057",
                "malloc_entropy=0x6a09e667f3bcc908,0xbb67ae8584caa73b",
                "ptr_munge=0x3c6ef372fe94f82b",
                "main_stack=" + to_hex(STACK_TOP) + "," + to_hex(STACK_RLIMIT) + "," + to_hex(STACK_TOP - STACK_SIZE) + "," +
                    to_hex(STACK_SIZE),
                "security_config=0x0",
            };
        }

        // XNU's exec_copyout_strings layout: [mach_header of the executable when a dynamic linker runs], argc,
        // argv[], NULL, envp[], NULL, apple[], NULL, then the strings (apple[0] first) ending at the stack top.
        // Unlike Linux there is no auxiliary vector.
        uint64_t setup_stack(posix_memory_manager& memory, const std::optional<uint64_t> main_header, const std::vector<std::string>& argv,
                             const std::vector<std::string>& envp, const std::vector<std::string>& apple)
        {
            if (!memory.allocate_memory(STACK_TOP - STACK_SIZE, STACK_SIZE, memory_permission::read_write))
            {
                throw std::runtime_error("Failed to allocate the initial stack");
            }

            std::vector<const std::string*> strings{};
            strings.push_back(&apple.front());
            for (const auto* group : {&argv, &envp})
            {
                for (const auto& string : *group)
                {
                    strings.push_back(&string);
                }
            }
            for (size_t i = 1; i < apple.size(); ++i)
            {
                strings.push_back(&apple[i]);
            }

            uint64_t strings_size = 0;
            for (const auto* string : strings)
            {
                strings_size += string->size() + 1;
            }

            const auto strings_start = align_down(STACK_TOP - strings_size, sizeof(uint64_t));

            std::vector<uint64_t> string_addresses{};
            auto string_address = strings_start;
            for (const auto* string : strings)
            {
                memory.write_memory(string_address, string->c_str(), string->size() + 1);
                string_addresses.push_back(string_address);
                string_address += string->size() + 1;
            }

            const auto argv_addresses = std::span{string_addresses}.subspan(1, argv.size());
            const auto envp_addresses = std::span{string_addresses}.subspan(1 + argv.size(), envp.size());

            std::vector<uint64_t> words{};
            if (main_header)
            {
                words.push_back(*main_header);
            }

            words.push_back(argv.size());
            std::ranges::copy(argv_addresses, std::back_inserter(words));
            words.push_back(0);
            std::ranges::copy(envp_addresses, std::back_inserter(words));
            words.push_back(0);
            words.push_back(string_addresses.front());
            std::ranges::copy(std::span{string_addresses}.last(apple.size() - 1), std::back_inserter(words));
            words.push_back(0);

            const auto stack_pointer = strings_start - (words.size() * sizeof(uint64_t));
            memory.write_memory(stack_pointer, words.data(), words.size() * sizeof(uint64_t));

            return stack_pointer;
        }

    }

    macos_emulator::macos_emulator(std::unique_ptr<x86_64_emulator> emu, std::filesystem::path emulation_root,
                                   const std::filesystem::path& executable, const std::vector<std::string>& argv,
                                   const std::vector<std::string>& envp)
        : macos_emulator(std::move(emu), std::move(emulation_root), read_binary_file(executable), executable.string(), argv, envp)
    {
    }

    macos_emulator::macos_emulator(std::unique_ptr<x86_64_emulator> emu, std::filesystem::path emulation_root,
                                   const std::span<const std::byte> image, const std::string& executable_path,
                                   const std::vector<std::string>& argv, const std::vector<std::string>& envp)
        : emu_(std::move(emu)),
          memory(*this->emu_, MACOS_MEMORY_LAYOUT),
          file_system(std::move(emulation_root))
    {
        const auto executable_image = map_image(this->memory, select_x86_64_slice(image), macho::MH_EXECUTE, 0);

        this->executable_uuid = executable_image.uuid;
        this->process_name = std::filesystem::path{executable_path}.filename().string();

        std::optional<uint64_t> entry_point = executable_image.unixthread_entry_point;
        std::optional<uint64_t> main_header{};

        if (executable_image.dylinker_path)
        {
            const auto dyld_file = read_binary_file(this->file_system.translate(*executable_image.dylinker_path));
            const auto dyld_slide = page_align_up(executable_image.end_address);
            const auto dyld_image = map_image(this->memory, select_x86_64_slice(dyld_file), macho::MH_DYLINKER, dyld_slide);

            entry_point = dyld_image.unixthread_entry_point;
            main_header = executable_image.header_address;

            const auto shared_cache_file = this->file_system.translate(SHARED_CACHE_PATH);
            if (std::filesystem::exists(shared_cache_file))
            {
                this->shared_region.emplace(this->memory, shared_cache_file, std::string{SHARED_CACHE_PATH},
                                            this->file_ids.id_for(std::string{SHARED_CACHE_PATH}));
            }
        }

        if (!entry_point)
        {
            throw std::runtime_error("No LC_UNIXTHREAD entry point to start from");
        }

        map_macos_commpage(this->memory);

        const auto stack_pointer = setup_stack(this->memory, main_header, argv, envp, make_apple_strings(executable_path));

        this->emu().reg(x86_register::rip, *entry_point);
        this->emu().reg(x86_register::rsp, stack_pointer);
        this->emu().reg(x86_register::rflags, INITIAL_RFLAGS);

        this->setup_hooks();
    }

    void macos_emulator::start()
    {
        this->should_stop_ = false;

        while (!this->should_stop_)
        {
            try
            {
                this->emu().start();
            }
            catch (const std::exception& e)
            {
                const auto rip = this->emu().read_instruction_pointer();
                std::array<uint8_t, 8> code{};
                this->emu().try_read_memory(rip, code.data(), code.size());
                this->log.error("%s at 0x%" PRIx64 " (%02x %02x %02x %02x %02x %02x %02x %02x)\n", e.what(), rip, code[0], code[1], code[2],
                                code[3], code[4], code[5], code[6], code[7]);
                throw;
            }

            if (!this->emu().has_violation())
            {
                break;
            }
        }
    }

    void macos_emulator::stop()
    {
        this->should_stop_ = true;
        this->emu().stop();
    }

    // SplitMix64: entropy stays reproducible so emulation is deterministic.
    uint64_t macos_emulator::next_entropy()
    {
        this->entropy_state_ += 0x9E3779B97F4A7C15;
        auto value = this->entropy_state_;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EB;
        return value ^ (value >> 31);
    }

    void macos_emulator::setup_hooks()
    {
        this->emu().hook_instruction(x86_hookable_instructions::syscall,
                                     [this](cpu_interface&, uint64_t) { return this->dispatcher.dispatch(*this); });

        this->emu().hook_memory_violation(
            [this](cpu_interface&, const uint64_t address, const size_t size, memory_operation, const memory_violation_type type) {
                // Backends disagree on how they report a fetch from a page that is not mapped yet (Icicle reports a
                // protection fault), so any fault inside the shared region is offered to it; it declines pages that are
                // already mapped.
                if (this->shared_region && this->shared_region->handle_fault(address, size))
                {
                    return memory_violation_continuation::restart;
                }

                this->log.error("%s memory access at 0x%" PRIx64 " (size %zu)\n",
                                type == memory_violation_type::unmapped ? "Unmapped" : "Protected", address, size);
                this->stop();
                return memory_violation_continuation::stop;
            });
    }
}

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

        uint64_t validate_segment_range(const macho::segment_command_64& segment, const uint64_t slide)
        {
            if (page_align_down(segment.vmaddr) != segment.vmaddr)
            {
                throw std::runtime_error("Mach-O segment is not page-aligned");
            }

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

        bool is_mapped_segment(const macho::segment_command_64& segment)
        {
            return segment.vmsize != 0 && segment.initprot != 0;
        }

        void validate_segment_contents(const std::span<const std::byte> image, const macho::segment_command_64& segment)
        {
            if (segment.filesize > segment.vmsize || segment.fileoff > image.size() || image.size() - segment.fileoff < segment.filesize)
            {
                throw std::runtime_error("Mach-O segment exceeds file bounds");
            }
        }

        void map_segment(posix_memory_manager& memory, const std::span<const std::byte> image, const macho::segment_command_64& segment,
                         const uint64_t slide)
        {
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

        macos_macho_image parse_image(const std::span<const std::byte> image, const uint32_t expected_file_type, const uint64_t slide)
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

            macos_macho_image result{.file = {image.begin(), image.end()}, .slide = slide};
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
                    if (is_mapped_segment(segment))
                    {
                        validate_segment_contents(image, segment);
                        result.segments.push_back(segment);
                    }
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

        void map_image(posix_memory_manager& memory, const macos_macho_image& image)
        {
            for (const auto& segment : image.segments)
            {
                map_segment(memory, image.file, segment, image.slide);
            }
        }

        struct address_range
        {
            uint64_t start{};
            uint64_t end{};
        };

        void add_segment_ranges(std::vector<address_range>& ranges, const macos_macho_image& image)
        {
            for (const auto& segment : image.segments)
            {
                const auto start = segment.vmaddr + image.slide;
                ranges.push_back({.start = start, .end = start + page_align_up(segment.vmsize)});
            }
        }

        // XNU loads into a fresh map and fails exec with LOAD_NOSPACE when the image collides with what it maps
        // itself, so everything placed at a fixed address must be disjoint before the old image is dropped.
        void validate_process_layout(const macos_process_image& process_image)
        {
            std::vector<address_range> ranges{{.start = STACK_TOP - STACK_SIZE, .end = STACK_TOP}};
            add_segment_ranges(ranges, process_image.executable);
            if (process_image.dylinker)
            {
                add_segment_ranges(ranges, *process_image.dylinker);
            }
            if (process_image.shared_region)
            {
                const auto base = process_image.shared_region->base_address();
                ranges.push_back({.start = base, .end = base + process_image.shared_region->size()});
            }

            std::ranges::sort(ranges, {}, &address_range::start);
            for (size_t i = 1; i < ranges.size(); ++i)
            {
                if (ranges[i].start < ranges[i - 1].end)
                {
                    throw std::runtime_error("Mach-O image overlaps memory the loader maps");
                }
            }
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

        // A new image starts from a clean thread state, as XNU's thread_state_initialize leaves it.
        void reset_thread_state(x86_64_emulator& emu)
        {
            constexpr std::array registers{
                x86_register::rax, x86_register::rbx, x86_register::rcx, x86_register::rdx,     x86_register::rsi,    x86_register::rdi,
                x86_register::rbp, x86_register::r8,  x86_register::r9,  x86_register::r10,     x86_register::r11,    x86_register::r12,
                x86_register::r13, x86_register::r14, x86_register::r15, x86_register::fs_base, x86_register::gs_base};
            for (const auto reg : registers)
            {
                emu.reg(reg, uint64_t{0});
            }
        }
    }

    bool is_macos_executable(const std::span<const std::byte> image)
    {
        try
        {
            const auto slice = select_x86_64_slice(image);
            const auto header = read_struct<macho::mach_header_64>(slice, 0);
            return header.magic == macho::MH_MAGIC_64 && header.cputype == macho::CPU_TYPE_X86_64 && header.filetype == macho::MH_EXECUTE;
        }
        catch (const std::runtime_error&)
        {
            return false;
        }
    }

    macos_emulator::macos_emulator(x86_64_emulator_factory create_backend, std::filesystem::path emulation_root,
                                   const std::filesystem::path& executable, const std::vector<std::string>& argv,
                                   const std::vector<std::string>& envp)
        : macos_emulator(std::move(create_backend), std::move(emulation_root), read_binary_file(executable), executable.string(), argv,
                         envp)
    {
    }

    macos_emulator::macos_emulator(x86_64_emulator_factory create_backend, std::filesystem::path emulation_root,
                                   const std::span<const std::byte> image, const std::string& executable_path,
                                   const std::vector<std::string>& argv, const std::vector<std::string>& envp)
        : system_(std::make_shared<macos_system>(macos_system{.create_backend = std::move(create_backend)})),
          emu_(this->system_->create_backend()),
          memory(*this->emu_, MACOS_MEMORY_LAYOUT),
          file_system(std::move(emulation_root))
    {
        this->load_image(this->prepare_process_image(image), executable_path, argv, envp);
        this->setup_hooks();
    }

    // XNU's fork: the child gets a copy of the address space and of the calling thread, shares the parent's open file
    // descriptions, signal dispositions and working directory, and starts with a fresh Mach IPC space.
    macos_emulator::macos_emulator(forked_tag, macos_emulator& parent)
        : system_(parent.system_),
          emu_(this->system_->create_backend()),
          memory(*this->emu_, MACOS_MEMORY_LAYOUT),
          file_system(parent.file_system),
          fds(parent.fds.fork()),
          file_ids(parent.file_ids),
          mach_ports(parent.mach_ports.for_new_task()),
          shared_region(parent.shared_region),
          signals(parent.signals),
          identity{
              .process_id = ++this->system_->last_process_id,
              .parent_process_id = parent.identity.process_id,
              .process_group_id = parent.identity.process_group_id,
              .unique_id = ++this->system_->last_unique_id,
              .parent_unique_id = parent.identity.unique_id,
          },
          executable_uuid(parent.executable_uuid),
          process_name(parent.process_name),
          working_directory(parent.working_directory),
          on_stdout(parent.on_stdout),
          on_stderr(parent.on_stderr)
    {
        this->log.disable_output(parent.log.is_output_disabled());
        this->log.set_silent(parent.log.is_silent());

        this->memory.copy_address_space(parent.memory);

        this->emu().restore_registers(parent.emu().save_registers());

        this->setup_hooks();
    }

    macos_process_image macos_emulator::prepare_process_image(const std::span<const std::byte> image)
    {
        macos_process_image process_image{.executable = parse_image(select_x86_64_slice(image), macho::MH_EXECUTE, 0)};
        const auto& executable = process_image.executable;

        if (executable.dylinker_path)
        {
            const auto dyld_file = read_binary_file(this->file_system.translate(*executable.dylinker_path));
            const auto dyld_slide = page_align_up(executable.end_address);
            process_image.dylinker = parse_image(select_x86_64_slice(dyld_file), macho::MH_DYLINKER, dyld_slide);

            const auto shared_cache_file = this->file_system.translate(SHARED_CACHE_PATH);
            if (std::filesystem::exists(shared_cache_file))
            {
                process_image.shared_region = std::make_shared<macos_shared_region>(shared_cache_file, std::string{SHARED_CACHE_PATH},
                                                                                    this->file_ids.id_for(std::string{SHARED_CACHE_PATH}));
            }
        }

        const auto& entry_image = process_image.dylinker ? *process_image.dylinker : executable;
        if (!entry_image.unixthread_entry_point)
        {
            throw std::runtime_error("No LC_UNIXTHREAD entry point to start from");
        }

        process_image.entry_point = *entry_image.unixthread_entry_point;
        validate_process_layout(process_image);
        return process_image;
    }

    void macos_emulator::load_image(const macos_process_image& process_image, const std::string& executable_path,
                                    const std::vector<std::string>& argv, const std::vector<std::string>& envp)
    {
        const auto& executable = process_image.executable;

        map_image(this->memory, executable);
        this->executable_uuid = executable.uuid;
        this->process_name = std::filesystem::path{executable_path}.filename().string();

        std::optional<uint64_t> main_header{};
        if (process_image.dylinker)
        {
            map_image(this->memory, *process_image.dylinker);
            main_header = executable.header_address;
        }

        this->shared_region = process_image.shared_region;
        if (this->shared_region)
        {
            this->shared_region->map(this->memory);
        }

        map_macos_commpage(this->memory);

        const auto stack_pointer = setup_stack(this->memory, main_header, argv, envp, make_apple_strings(executable_path));

        this->emu().reg(x86_register::rip, process_image.entry_point);
        this->emu().reg(x86_register::rsp, stack_pointer);
        this->emu().reg(x86_register::rflags, INITIAL_RFLAGS);
    }

    // The process keeps its identity, working directory, signal mask and the descriptors not marked close-on-exec;
    // everything else belongs to the image being replaced.
    void macos_emulator::exec_image(const macos_process_image& process_image, const std::string& executable_path,
                                    const std::vector<std::string>& argv, const std::vector<std::string>& envp)
    {
        this->memory.unmap_all_memory();
        this->mach_ports = this->mach_ports.for_new_task();
        this->signals.reset_on_exec();
        this->fds.close_descriptors_on_exec();
        reset_thread_state(this->emu());

        this->load_image(process_image, executable_path, argv, envp);
    }

    // Processes never run concurrently: the child runs until it terminates before the parent resumes, so it is
    // already waitable when fork returns in the parent.
    uint32_t macos_emulator::fork_process(const std::function<void(macos_emulator& child)>& prepare_child)
    {
        macos_emulator child{forked_tag{}, *this};
        prepare_child(child);
        child.start();

        this->children.add_terminated({
            .process_id = child.identity.process_id,
            .process_group_id = child.identity.process_group_id,
            .wait_status = child.wait_status(),
        });

        return child.identity.process_id;
    }

    // A process the emulator had to stop without an exit status is reported as killed.
    int macos_emulator::wait_status() const
    {
        return this->exit_status ? posix_exited_wait_status(*this->exit_status) : posix_signaled_wait_status(MACOS_SIGKILL);
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

            if (this->after_syscall_)
            {
                std::exchange(this->after_syscall_, {})();
                continue;
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

    void macos_emulator::after_syscall(std::function<void()> operation)
    {
        this->after_syscall_ = std::move(operation);
        this->emu().stop();
    }

    // SplitMix64: entropy stays reproducible so emulation is deterministic.
    uint64_t macos_emulator::next_entropy()
    {
        this->system_->entropy_state += 0x9E3779B97F4A7C15;
        auto value = this->system_->entropy_state;
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
                if (this->shared_region && this->shared_region->handle_fault(this->memory, address, size))
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

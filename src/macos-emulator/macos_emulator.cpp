#include "macos_emulator.hpp"
#include "macho.hpp"
#include "macos_memory_manager.hpp"

#include <address_utils.hpp>
#include <utils/io.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>

namespace sogen
{
    namespace
    {
        constexpr uint64_t STACK_TOP = 0x7FF7C0000000;
        constexpr uint64_t STACK_SIZE = 0x800000;

        // XNU encodes the trap class in the top byte of rax: 1 = Mach, 2 = Unix (BSD), 3 = machine-dependent.
        constexpr uint64_t SYSCALL_CLASS_SHIFT = 24;
        constexpr uint64_t SYSCALL_NUMBER_MASK = 0xFFFFFF;
        constexpr uint64_t SYSCALL_CLASS_UNIX = 2;

        constexpr uint64_t SYS_EXIT = 1;
        constexpr uint64_t SYS_WRITE = 4;

        constexpr uint64_t MACOS_EBADF = 9;
        constexpr uint64_t MACOS_EFAULT = 14;

        constexpr uint64_t RFLAGS_CF = 1;

        std::vector<std::byte> read_executable(const std::filesystem::path& path)
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

        memory_permission to_memory_permission(const uint32_t protection)
        {
            auto permission = memory_permission::none;

            if (protection & macho::VM_PROT_READ)
            {
                permission |= memory_permission::read;
            }

            if (protection & macho::VM_PROT_WRITE)
            {
                permission |= memory_permission::write;
            }

            if (protection & macho::VM_PROT_EXECUTE)
            {
                permission |= memory_permission::exec;
            }

            return permission;
        }

        void map_segment(x86_64_emulator& emu, macos_memory_manager& memory, const std::span<const std::byte> image,
                         const macho::segment_command_64& segment)
        {
            if (segment.vmsize == 0 || segment.initprot == 0)
            {
                return;
            }

            if (segment.filesize > segment.vmsize || segment.fileoff > image.size() || image.size() - segment.fileoff < segment.filesize)
            {
                throw std::runtime_error("Mach-O segment exceeds file bounds");
            }

            const auto size = page_align_up(segment.vmsize);
            memory.map(segment.vmaddr, size, memory_permission::read_write);
            emu.write_memory(segment.vmaddr, image.data() + segment.fileoff, segment.filesize);
            memory.protect(segment.vmaddr, size, to_memory_permission(segment.initprot));
        }

        uint64_t read_unixthread_entry_point(const std::span<const std::byte> image, const uint64_t offset)
        {
            const auto thread = read_struct<macho::thread_command>(image, offset);
            if (thread.flavor != macho::X86_THREAD_STATE64)
            {
                throw std::runtime_error("Unsupported LC_UNIXTHREAD flavor");
            }

            return read_struct<macho::x86_thread_state64>(image, offset + sizeof(thread)).rip;
        }

        uint64_t map_executable(x86_64_emulator& emu, macos_memory_manager& memory, const std::span<const std::byte> image)
        {
            const auto header = read_struct<macho::mach_header_64>(image, 0);
            if (header.magic != macho::MH_MAGIC_64 || header.cputype != macho::CPU_TYPE_X86_64)
            {
                throw std::runtime_error("Not an x86-64 Mach-O image");
            }

            if (header.filetype != macho::MH_EXECUTE)
            {
                throw std::runtime_error("Only MH_EXECUTE Mach-O images are supported");
            }

            std::optional<uint64_t> entry_point{};
            uint64_t offset = sizeof(header);

            for (uint32_t i = 0; i < header.ncmds; ++i)
            {
                const auto command = read_struct<macho::load_command>(image, offset);
                if (command.cmdsize < sizeof(command))
                {
                    throw std::runtime_error("Invalid Mach-O load command size");
                }

                switch (command.cmd)
                {
                case macho::LC_SEGMENT_64:
                    map_segment(emu, memory, image, read_struct<macho::segment_command_64>(image, offset));
                    break;
                case macho::LC_UNIXTHREAD:
                    entry_point = read_unixthread_entry_point(image, offset);
                    break;
                case macho::LC_MAIN:
                case macho::LC_LOAD_DYLINKER:
                    throw std::runtime_error("Dynamically linked Mach-O executables are not supported");
                default:
                    break;
                }

                offset += command.cmdsize;
            }

            if (!entry_point)
            {
                throw std::runtime_error("Mach-O image has no LC_UNIXTHREAD");
            }

            return *entry_point;
        }

        // XNU's exec_copyout_strings layout for executables without a dynamic linker:
        // argc, argv[], NULL, envp[], NULL, apple[], NULL. Unlike Linux there is no auxiliary vector.
        uint64_t setup_stack(x86_64_emulator& emu, macos_memory_manager& memory, const std::vector<std::string>& argv,
                             const std::vector<std::string>& envp, const std::vector<std::string>& apple)
        {
            memory.map(STACK_TOP - STACK_SIZE, STACK_SIZE, memory_permission::read_write);

            auto stack_pointer = STACK_TOP;
            const auto push_strings = [&](const std::vector<std::string>& strings) {
                std::vector<uint64_t> pointers{};
                for (const auto& string : strings)
                {
                    stack_pointer -= string.size() + 1;
                    emu.write_memory(stack_pointer, string.c_str(), string.size() + 1);
                    pointers.push_back(stack_pointer);
                }

                pointers.push_back(0);
                return pointers;
            };

            const auto argv_pointers = push_strings(argv);
            const auto envp_pointers = push_strings(envp);
            const auto apple_pointers = push_strings(apple);

            std::vector<uint64_t> words{static_cast<uint64_t>(argv.size())};
            words.insert(words.end(), argv_pointers.begin(), argv_pointers.end());
            words.insert(words.end(), envp_pointers.begin(), envp_pointers.end());
            words.insert(words.end(), apple_pointers.begin(), apple_pointers.end());

            stack_pointer = align_down(stack_pointer - (words.size() * sizeof(uint64_t)), 16);
            emu.write_memory(stack_pointer, words.data(), words.size() * sizeof(uint64_t));

            return stack_pointer;
        }

        void set_syscall_result(x86_64_emulator& emu, const uint64_t value, const bool failed)
        {
            const auto rflags = emu.reg(x86_register::rflags);

            emu.reg(x86_register::rax, value);
            emu.reg(x86_register::rflags, failed ? (rflags | RFLAGS_CF) : (rflags & ~RFLAGS_CF));
        }
    }

    macos_emulator::macos_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& executable,
                                   const std::vector<std::string>& argv, const std::vector<std::string>& envp)
        : macos_emulator(std::move(emu), read_executable(executable), executable.string(), argv, envp)
    {
    }

    macos_emulator::macos_emulator(std::unique_ptr<x86_64_emulator> emu, const std::span<const std::byte> image,
                                   const std::string& executable_path, const std::vector<std::string>& argv,
                                   const std::vector<std::string>& envp)
        : emu_(std::move(emu))
    {
        macos_memory_manager memory{this->emu()};

        const auto entry_point = map_executable(this->emu(), memory, image);
        const auto stack_pointer = setup_stack(this->emu(), memory, argv, envp, {"executable_path=" + executable_path});

        this->emu().reg(x86_register::rip, entry_point);
        this->emu().reg(x86_register::rsp, stack_pointer);

        this->setup_hooks();
    }

    void macos_emulator::start()
    {
        this->emu().start();
    }

    void macos_emulator::stop()
    {
        this->emu().stop();
    }

    void macos_emulator::setup_hooks()
    {
        this->emu().hook_instruction(x86_hookable_instructions::syscall,
                                     [this](cpu_interface&, uint64_t) { return this->handle_syscall(); });

        this->emu().hook_memory_violation(
            [this](cpu_interface&, const uint64_t address, const size_t size, memory_operation, const memory_violation_type type) {
                this->log.error("%s memory access at 0x%" PRIx64 " (size %zu)\n",
                                type == memory_violation_type::unmapped ? "Unmapped" : "Protected", address, size);
                this->stop();
                return memory_violation_continuation::stop;
            });
    }

    instruction_hook_continuation macos_emulator::handle_syscall()
    {
        const auto rax = this->emu().reg(x86_register::rax);
        const auto syscall_class = rax >> SYSCALL_CLASS_SHIFT;
        const auto number = rax & SYSCALL_NUMBER_MASK;

        if (syscall_class != SYSCALL_CLASS_UNIX)
        {
            this->log.error("Unsupported syscall class %" PRIu64 " (rax=0x%" PRIx64 ")\n", syscall_class, rax);
            this->stop();
            return instruction_hook_continuation::skip_instruction;
        }

        switch (number)
        {
        case SYS_EXIT:
            this->exit_status = static_cast<int>(this->emu().reg(x86_register::rdi));
            this->stop();
            break;
        case SYS_WRITE:
            this->handle_write();
            break;
        default:
            this->log.error("Unimplemented syscall %" PRIu64 "\n", number);
            this->stop();
            break;
        }

        return instruction_hook_continuation::skip_instruction;
    }

    void macos_emulator::handle_write()
    {
        auto& e = this->emu();
        const auto fd = e.reg(x86_register::rdi);
        const auto buffer = e.reg(x86_register::rsi);
        const auto size = static_cast<size_t>(e.reg(x86_register::rdx));

        if (fd != 1 && fd != 2)
        {
            set_syscall_result(e, MACOS_EBADF, true);
            return;
        }

        std::vector<char> data(size);
        if (!e.try_read_memory(buffer, data.data(), size))
        {
            set_syscall_result(e, MACOS_EFAULT, true);
            return;
        }

        const auto& callback = fd == 1 ? this->on_stdout : this->on_stderr;
        if (callback)
        {
            callback(std::string_view{data.data(), size});
        }
        else
        {
            auto* stream = fd == 1 ? stdout : stderr;
            fwrite(data.data(), 1, size, stream);
            fflush(stream);
        }

        set_syscall_result(e, size, false);
    }
}

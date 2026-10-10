#include <gtest/gtest.h>

#include <backend_selection.hpp>
#include <macho.hpp>
#include <macos_emulator.hpp>
#include <utils/io.hpp>

#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::macos_test
{
    namespace
    {
        constexpr uint64_t IMAGE_BASE = 0x100000000;
        constexpr uint64_t IMAGE_SIZE = 0x1000;
        constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000C;

        const std::vector<uint8_t> HELLO_CODE{
            0xB8, 0x04, 0x00, 0x00, 0x02,             // mov eax, 0x2000004 (write)
            0xBF, 0x01, 0x00, 0x00, 0x00,             // mov edi, 1
            0x48, 0x8D, 0x35, 0x13, 0x00, 0x00, 0x00, // lea rsi, [rip + message]
            0xBA, 0x06, 0x00, 0x00, 0x00,             // mov edx, 6
            0x0F, 0x05,                               // syscall
            0xB8, 0x01, 0x00, 0x00, 0x02,             // mov eax, 0x2000001 (exit)
            0xBF, 0x07, 0x00, 0x00, 0x00,             // mov edi, 7
            0x0F, 0x05,                               // syscall
            'h',  'e',  'l',  'l',  'o',  '\n',       // message
        };

        const std::vector<uint8_t> BAD_FD_WRITE_CODE{
            0xB8, 0x04, 0x00, 0x00, 0x02, // mov eax, 0x2000004 (write)
            0xBF, 0x05, 0x00, 0x00, 0x00, // mov edi, 5
            0x31, 0xF6,                   // xor esi, esi
            0x31, 0xD2,                   // xor edx, edx
            0x0F, 0x05,                   // syscall
            0x89, 0xC7,                   // mov edi, eax
            0x11, 0xFF,                   // adc edi, edi
            0xB8, 0x01, 0x00, 0x00, 0x02, // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                   // syscall
        };

        const std::vector<uint8_t> EXIT_WITH_ARGC_CODE{
            0x48, 0x8B, 0x3C, 0x24,       // mov rdi, [rsp]
            0xB8, 0x01, 0x00, 0x00, 0x02, // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                   // syscall
        };

        const std::vector<uint8_t> MACH_TRAP_CODE{
            0xB8, 0x1A, 0x00, 0x00, 0x01, // mov eax, 0x100001A (mach_reply_port)
            0x0F, 0x05,                   // syscall
        };

        const std::vector<uint8_t> READ_COMMPAGE_PAGE_SHIFT_CODE{
            0xA0, 0x4E, 0x00, 0xE0, 0xFF, 0xFF, 0x7F, 0x00, 0x00, // mov al, [0x7FFFFFE0004E] (user page shift)
            0x0F, 0xB6, 0xF8,                                     // movzx edi, al
            0xB8, 0x01, 0x00, 0x00, 0x02,                         // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                           // syscall
        };

        const std::vector<uint8_t> FAKE_DYLD_CODE{
            0x48, 0x8B, 0x04, 0x24,                                     // mov rax, [rsp]
            0x48, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov rcx, 0x100000000
            0x31, 0xFF,                                                 // xor edi, edi
            0x48, 0x39, 0xC8,                                           // cmp rax, rcx
            0x40, 0x0F, 0x95, 0xC7,                                     // setne dil
            0x6B, 0xFF, 0x64,                                           // imul edi, edi, 100
            0x48, 0x03, 0x7C, 0x24, 0x08,                               // add rdi, [rsp + 8]
            0xB8, 0x01, 0x00, 0x00, 0x02,                               // mov eax, 0x2000001 (exit)
            0x0F, 0x05,                                                 // syscall
        };

        struct test_image
        {
            macho::mach_header_64 header{};
            macho::segment_command_64 text{};
            macho::thread_command thread{};
            macho::x86_thread_state64 thread_state{};
            std::vector<std::vector<std::byte>> extra_commands{};
            std::vector<uint8_t> code{};

            std::vector<std::byte> serialize() const
            {
                std::vector<std::byte> data{};
                const auto append = [&data](const void* value, const size_t size) {
                    const auto* bytes = static_cast<const std::byte*>(value);
                    data.insert(data.end(), bytes, bytes + size);
                };

                auto final_header = this->header;
                final_header.ncmds = static_cast<uint32_t>(2 + this->extra_commands.size());
                final_header.sizeofcmds = static_cast<uint32_t>(sizeof(this->text) + sizeof(this->thread) + sizeof(this->thread_state));
                for (const auto& command : this->extra_commands)
                {
                    final_header.sizeofcmds += static_cast<uint32_t>(command.size());
                }

                append(&final_header, sizeof(final_header));
                append(&this->text, sizeof(this->text));
                append(&this->thread, sizeof(this->thread));
                append(&this->thread_state, sizeof(this->thread_state));
                for (const auto& command : this->extra_commands)
                {
                    append(command.data(), command.size());
                }
                append(this->code.data(), this->code.size());

                return data;
            }
        };

        struct image_options
        {
            uint64_t base{IMAGE_BASE};
            uint32_t file_type{macho::MH_EXECUTE};
            std::vector<std::vector<std::byte>> extra_commands{};
        };

        test_image make_test_image(std::vector<uint8_t> code, image_options options = {})
        {
            test_image image{};
            image.code = std::move(code);
            image.extra_commands = std::move(options.extra_commands);

            auto code_offset = sizeof(image.header) + sizeof(image.text) + sizeof(image.thread) + sizeof(image.thread_state);
            for (const auto& command : image.extra_commands)
            {
                code_offset += command.size();
            }

            image.header.magic = macho::MH_MAGIC_64;
            image.header.cputype = macho::CPU_TYPE_X86_64;
            image.header.filetype = options.file_type;

            image.text.cmd = macho::LC_SEGMENT_64;
            image.text.cmdsize = sizeof(image.text);
            std::memcpy(image.text.segname.data(), "__TEXT", sizeof("__TEXT"));
            image.text.vmaddr = options.base;
            image.text.vmsize = IMAGE_SIZE;
            image.text.filesize = code_offset + image.code.size();
            image.text.maxprot = macho::VM_PROT_READ | macho::VM_PROT_EXECUTE;
            image.text.initprot = macho::VM_PROT_READ | macho::VM_PROT_EXECUTE;

            image.thread.cmd = macho::LC_UNIXTHREAD;
            image.thread.cmdsize = sizeof(image.thread) + sizeof(image.thread_state);
            image.thread.flavor = macho::X86_THREAD_STATE64;
            image.thread.count = sizeof(image.thread_state) / sizeof(uint32_t);
            image.thread_state.rip = options.base + code_offset;

            return image;
        }

        std::vector<std::byte> make_dylinker_command(const std::string_view path)
        {
            macho::dylinker_command command{};
            command.cmd = macho::LC_LOAD_DYLINKER;
            command.name_offset = sizeof(command);
            command.cmdsize = static_cast<uint32_t>((sizeof(command) + path.size() + 1 + 7) & ~size_t{7});

            std::vector<std::byte> data(command.cmdsize);
            std::memcpy(data.data(), &command, sizeof(command));
            std::memcpy(data.data() + sizeof(command), path.data(), path.size());
            return data;
        }

        class temporary_root
        {
          public:
            temporary_root()
                : path_(std::filesystem::temp_directory_path() /
                        ("sogen-macos-root-" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()}))
            {
                std::filesystem::remove_all(this->path_);
                std::filesystem::create_directories(this->path_);
            }

            temporary_root(const temporary_root&) = delete;
            temporary_root& operator=(const temporary_root&) = delete;
            temporary_root(temporary_root&&) = delete;
            temporary_root& operator=(temporary_root&&) = delete;

            ~temporary_root()
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->path_, ec);
            }

            void add_file(const std::string_view guest_path, const std::span<const std::byte> data) const
            {
                const auto host_path = this->path_ / guest_path.substr(1);
                std::filesystem::create_directories(host_path.parent_path());
                ASSERT_TRUE(utils::io::write_file(host_path, data));
            }

            const std::filesystem::path& path() const
            {
                return this->path_;
            }

          private:
            std::filesystem::path path_{};
        };

        struct run_result
        {
            std::string stdout_data{};
            std::string stderr_data{};
            std::optional<int> exit_status{};
        };

        struct universal_slice
        {
            uint32_t cputype{};
            std::vector<std::byte> data{};
        };

        std::vector<std::byte> make_universal_binary(const std::vector<universal_slice>& slices)
        {
            constexpr uint64_t slice_alignment = 0x1000;

            std::vector<std::byte> data{};
            const auto append_big_endian_u32 = [&data](const uint64_t value) {
                for (int shift = 24; shift >= 0; shift -= 8)
                {
                    data.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
                }
            };

            append_big_endian_u32(macho::FAT_MAGIC);
            append_big_endian_u32(slices.size());

            uint64_t slice_offset = slice_alignment;
            for (const auto& slice : slices)
            {
                append_big_endian_u32(slice.cputype);
                append_big_endian_u32(0);
                append_big_endian_u32(slice_offset);
                append_big_endian_u32(slice.data.size());
                append_big_endian_u32(12);
                slice_offset += (slice.data.size() + slice_alignment - 1) & ~(slice_alignment - 1);
            }

            for (const auto& slice : slices)
            {
                data.resize((data.size() + slice_alignment - 1) & ~(slice_alignment - 1));
                data.insert(data.end(), slice.data.begin(), slice.data.end());
            }

            return data;
        }

        run_result run_binary(const std::span<const std::byte> data, const std::vector<std::string>& argv = {"test"},
                              const std::filesystem::path& emulation_root = {})
        {
            macos_emulator emu{create_x86_64_emulator_from_environment(), emulation_root, data, "/test", argv, {}};
            emu.log.disable_output(true);

            run_result result{};
            emu.on_stdout = [&result](const std::string_view output) { result.stdout_data += output; };
            emu.on_stderr = [&result](const std::string_view output) { result.stderr_data += output; };

            emu.start();

            result.exit_status = emu.exit_status;
            return result;
        }

        run_result run_image(const test_image& image, const std::vector<std::string>& argv = {"test"},
                             const std::filesystem::path& emulation_root = {})
        {
            return run_binary(image.serialize(), argv, emulation_root);
        }

        void expect_rejected(const std::span<const std::byte> data, const std::filesystem::path& emulation_root = {})
        {
            auto backend = create_x86_64_emulator_from_environment();

            EXPECT_THROW(macos_emulator(std::move(backend), emulation_root, data, "/test", {"test"}, {}), std::runtime_error);
        }

        void expect_rejected(const test_image& image, const std::filesystem::path& emulation_root = {})
        {
            expect_rejected(image.serialize(), emulation_root);
        }
    }

    TEST(MacosEmulationTest, RunsStaticExecutable)
    {
        const auto result = run_image(make_test_image(HELLO_CODE));

        EXPECT_EQ(result.stdout_data, "hello\n");
        EXPECT_TRUE(result.stderr_data.empty());
        EXPECT_EQ(result.exit_status, 7);
    }

    TEST(MacosEmulationTest, ReportsSyscallErrorsThroughCarryFlag)
    {
        const auto result = run_image(make_test_image(BAD_FD_WRITE_CODE));

        constexpr int ebadf = 9;
        EXPECT_EQ(result.exit_status, (ebadf * 2) + 1);
    }

    TEST(MacosEmulationTest, PassesArgumentCountOnInitialStack)
    {
        const auto result = run_image(make_test_image(EXIT_WITH_ARGC_CODE), {"test", "a", "b"});

        EXPECT_EQ(result.exit_status, 3);
    }

    TEST(MacosEmulationTest, StopsOnMachTrap)
    {
        const auto result = run_image(make_test_image(MACH_TRAP_CODE));

        EXPECT_FALSE(result.exit_status.has_value());
    }

    TEST(MacosEmulationTest, MapsCommpage)
    {
        const auto result = run_image(make_test_image(READ_COMMPAGE_PAGE_SHIFT_CODE));

        EXPECT_EQ(result.exit_status, 12);
    }

    TEST(MacosEmulationTest, StartsDyldWithMainHeaderAndArguments)
    {
        const temporary_root root{};
        root.add_file("/usr/lib/dyld", make_test_image(FAKE_DYLD_CODE, {.base = 0, .file_type = macho::MH_DYLINKER}).serialize());

        const auto executable = make_test_image(HELLO_CODE, {.extra_commands = {make_dylinker_command("/usr/lib/dyld")}});
        const auto result = run_image(executable, {"test", "a"}, root.path());

        EXPECT_TRUE(result.stdout_data.empty());
        EXPECT_EQ(result.exit_status, 2);
    }

    TEST(MacosLoaderTest, RejectsDynamicExecutableWithoutDyld)
    {
        const temporary_root root{};

        expect_rejected(make_test_image(HELLO_CODE, {.extra_commands = {make_dylinker_command("/usr/lib/dyld")}}), root.path());
    }

    TEST(MacosLoaderTest, RejectsNonX86_64Image)
    {
        auto image = make_test_image(HELLO_CODE);
        image.header.cputype = CPU_TYPE_ARM64;

        expect_rejected(image);
    }

    TEST(MacosLoaderTest, RejectsSegmentExceedingFile)
    {
        auto image = make_test_image(HELLO_CODE);
        image.text.filesize = IMAGE_SIZE;

        expect_rejected(image);
    }

    TEST(MacosLoaderTest, RejectsUnsupportedThreadStateFlavor)
    {
        auto image = make_test_image(HELLO_CODE);
        image.thread.flavor = 1;

        expect_rejected(image);
    }

    TEST(MacosEmulationTest, RunsX86_64SliceOfUniversalBinary)
    {
        const auto data = make_universal_binary({
            {.cputype = CPU_TYPE_ARM64, .data = std::vector<std::byte>(0x40)},
            {.cputype = macho::CPU_TYPE_X86_64, .data = make_test_image(HELLO_CODE).serialize()},
        });

        const auto result = run_binary(data);

        EXPECT_EQ(result.stdout_data, "hello\n");
        EXPECT_EQ(result.exit_status, 7);
    }

    TEST(MacosLoaderTest, RejectsUniversalBinaryWithoutX86_64Slice)
    {
        expect_rejected(make_universal_binary({{.cputype = CPU_TYPE_ARM64, .data = std::vector<std::byte>(0x40)}}));
    }
}

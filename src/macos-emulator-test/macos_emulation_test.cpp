#include <gtest/gtest.h>

#include <backend_selection.hpp>
#include <macho.hpp>
#include <macos_emulator.hpp>

#include <cstring>
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

        struct test_image
        {
            macho::mach_header_64 header{};
            macho::segment_command_64 text{};
            macho::thread_command thread{};
            macho::x86_thread_state64 thread_state{};
            std::vector<macho::load_command> extra_commands{};
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
                final_header.sizeofcmds = static_cast<uint32_t>(sizeof(this->text) + sizeof(this->thread) + sizeof(this->thread_state) +
                                                                (this->extra_commands.size() * sizeof(macho::load_command)));

                append(&final_header, sizeof(final_header));
                append(&this->text, sizeof(this->text));
                append(&this->thread, sizeof(this->thread));
                append(&this->thread_state, sizeof(this->thread_state));
                append(this->extra_commands.data(), this->extra_commands.size() * sizeof(macho::load_command));
                append(this->code.data(), this->code.size());

                return data;
            }
        };

        test_image make_test_image(std::vector<uint8_t> code)
        {
            test_image image{};
            image.code = std::move(code);

            const auto code_offset = sizeof(image.header) + sizeof(image.text) + sizeof(image.thread) + sizeof(image.thread_state);

            image.header.magic = macho::MH_MAGIC_64;
            image.header.cputype = macho::CPU_TYPE_X86_64;
            image.header.filetype = macho::MH_EXECUTE;

            image.text.cmd = macho::LC_SEGMENT_64;
            image.text.cmdsize = sizeof(image.text);
            std::memcpy(image.text.segname.data(), "__TEXT", sizeof("__TEXT"));
            image.text.vmaddr = IMAGE_BASE;
            image.text.vmsize = IMAGE_SIZE;
            image.text.filesize = code_offset + image.code.size();
            image.text.maxprot = macho::VM_PROT_READ | macho::VM_PROT_EXECUTE;
            image.text.initprot = macho::VM_PROT_READ | macho::VM_PROT_EXECUTE;

            image.thread.cmd = macho::LC_UNIXTHREAD;
            image.thread.cmdsize = sizeof(image.thread) + sizeof(image.thread_state);
            image.thread.flavor = macho::X86_THREAD_STATE64;
            image.thread.count = sizeof(image.thread_state) / sizeof(uint32_t);
            image.thread_state.rip = IMAGE_BASE + code_offset;

            return image;
        }

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

        run_result run_binary(const std::span<const std::byte> data, const std::vector<std::string>& argv = {"test"})
        {
            macos_emulator emu{create_x86_64_emulator_from_environment(), data, "/test", argv, {}};
            emu.log.disable_output(true);

            run_result result{};
            emu.on_stdout = [&result](const std::string_view output) { result.stdout_data += output; };
            emu.on_stderr = [&result](const std::string_view output) { result.stderr_data += output; };

            emu.start();

            result.exit_status = emu.exit_status;
            return result;
        }

        run_result run_image(const test_image& image, const std::vector<std::string>& argv = {"test"})
        {
            return run_binary(image.serialize(), argv);
        }

        void expect_rejected(const std::span<const std::byte> data)
        {
            auto backend = create_x86_64_emulator_from_environment();

            EXPECT_THROW(macos_emulator(std::move(backend), data, "/test", {"test"}, {}), std::runtime_error);
        }

        void expect_rejected(const test_image& image)
        {
            expect_rejected(image.serialize());
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

    TEST(MacosLoaderTest, RejectsDynamicallyLinkedExecutable)
    {
        auto image = make_test_image(HELLO_CODE);
        image.extra_commands.push_back({.cmd = macho::LC_LOAD_DYLINKER, .cmdsize = sizeof(macho::load_command)});

        expect_rejected(image);
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

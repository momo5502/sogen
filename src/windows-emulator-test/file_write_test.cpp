#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <utils/finally.hpp>

#include <random>

namespace sogen::syscalls
{
    NTSTATUS handle_NtWriteFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                                emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                                emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtReadFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                               emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
}

namespace sogen::test
{
    TEST(FileWriteTest, CompletedWritesAreVisibleThroughOtherHandles)
    {
        const auto directory = std::filesystem::temp_directory_path() /
                               ("sogen-file-write-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto cleanup = utils::finally([&] {
            std::error_code error{};
            std::filesystem::remove(directory / "write.bin", error);
            std::filesystem::remove(directory / "append.bin", error);
            std::filesystem::remove(directory, error);
        });

        for (const auto append : {false, true})
        {
            SCOPED_TRACE(append);
            auto emu = create_empty_emulator();
            const auto path = directory / (append ? "append.bin" : "write.bin");
            file writer{};
            FILE* stream{};
            open_unicode(&stream, path, append ? u"a+b" : u"w+b");
            writer.handle = stream;
            ASSERT_TRUE(writer.handle);
            ASSERT_EQ(0, setvbuf(writer.handle, nullptr, _IOFBF, 8192));
            const auto writer_handle = emu.process.files.store(std::move(writer));

            const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            const syscall_context context{.win_emu = emu, .emu = emu.emu(), .vcpu = emu.vcpu(0), .proc = emu.process};
            const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{emu.memory, storage};
            constexpr std::string_view contents = "{\"type\":\"session_meta\"}\n";
            emu.memory.write_memory(storage + 0x100, contents.data(), contents.size());

            for (size_t i = 1; i <= 2; ++i)
            {
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtWriteFile(context, writer_handle, 0, 0, 0, status, storage + 0x100,
                                                                       static_cast<ULONG>(contents.size()), {emu.memory}, {emu.memory}));
                EXPECT_EQ(STATUS_SUCCESS, status.read().Status);
                EXPECT_EQ(contents.size(), status.read().Information);

                file reader{};
                stream = nullptr;
                open_unicode(&stream, path, u"rb");
                reader.handle = stream;
                ASSERT_TRUE(reader.handle);
                const auto reader_handle = emu.process.files.store(std::move(reader));
                const auto length = static_cast<ULONG>(i * contents.size());
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtReadFile(context, reader_handle, 0, 0, 0, status, storage + 0x200, length,
                                                                      {emu.memory}, {emu.memory}));
                EXPECT_EQ(length, status.read().Information);
                const auto actual = emu.memory.read_memory(storage + 0x200, length);
                const auto expected = i == 1 ? std::string(contents) : std::string(contents) + std::string(contents);
                EXPECT_EQ(expected, std::string(reinterpret_cast<const char*>(actual.data()), actual.size()));
            }
        }
    }
}

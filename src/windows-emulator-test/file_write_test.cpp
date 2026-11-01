#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <utils/finally.hpp>

#include <fstream>
#include <random>

namespace sogen::syscalls
{
    NTSTATUS handle_NtWriteFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                                emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                                emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtReadFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                               emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtCreateFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                 emulator_object<LARGE_INTEGER> allocation_size, ULONG file_attributes, ULONG share_access,
                                 ULONG create_disposition, ULONG create_options, uint64_t ea_buffer, ULONG ea_length);
    NTSTATUS handle_NtLockFile(const syscall_context& c, handle file_handle, handle event_handle, uint64_t apc_routine,
                               uint64_t apc_context, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                               emulator_object<LARGE_INTEGER> byte_offset, emulator_object<LARGE_INTEGER> length, ULONG key,
                               BOOLEAN fail_immediately, BOOLEAN exclusive_lock);
}

namespace sogen::test
{
    namespace
    {
        std::filesystem::path create_test_directory()
        {
            auto directory = std::filesystem::temp_directory_path() /
                             ("sogen-file-write-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
            std::filesystem::create_directory(directory);
            return directory;
        }

        handle create_guest_file(windows_emulator& emu, const syscall_context& context, const uint64_t storage,
                                 const std::u16string_view name, const ACCESS_MASK access)
        {
            const auto name_bytes = static_cast<USHORT>(name.size() * sizeof(char16_t));
            emu.memory.write_memory(storage + 0x400, name.data(), name_bytes);
            emu.memory.write_memory(storage + 0x300, UNICODE_STRING<EmulatorTraits<Emu64>>{
                                                         .Length = name_bytes,
                                                         .MaximumLength = name_bytes,
                                                         .Buffer = storage + 0x400,
                                                     });
            emu.memory.write_memory(storage + 0x200, OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>{
                                                         .Length = sizeof(OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>),
                                                         .RootDirectory = 0,
                                                         .ObjectName = storage + 0x300,
                                                     });

            const emulator_object<handle> file_handle{emu.memory, storage};
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateFile(context, file_handle, access, {emu.memory, storage + 0x200},
                                                                    {emu.memory, storage + 0x10}, {emu.memory}, 0, 0, FILE_OVERWRITE_IF,
                                                                    FILE_SYNCHRONOUS_IO_NONALERT, 0, 0));
            return file_handle.read();
        }
    }

    TEST(FileWriteTest, GenericWriteHandlesWriteAtTheOffsetTheyName)
    {
        const auto directory = create_test_directory();
        const auto cleanup = utils::finally([&] {
            std::error_code error{};
            std::filesystem::remove_all(directory, error);
        });

        auto emu = create_empty_emulator();
        emu.file_sys.map(windows_path{'x', {}}, directory, filesystem_mode::read_write);
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};

        constexpr ACCESS_MASK generic_write =
            READ_CONTROL | SYNCHRONIZE | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | FILE_APPEND_DATA;
        const auto file_handle = create_guest_file(emu, context, storage, u"\\??\\x:\\offsets.bin", generic_write);

        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{emu.memory, storage + 0x10};
        const emulator_object<LARGE_INTEGER> offset{emu.memory, storage + 0x20};
        emu.memory.write_memory(storage + 0x100, "BBBBAAAA", 8);

        offset.write(LARGE_INTEGER{.QuadPart = 8});
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtWriteFile(context, file_handle, 0, 0, 0, status, storage + 0x100, 4, offset, {emu.memory}));
        offset.write(LARGE_INTEGER{.QuadPart = 0});
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtWriteFile(context, file_handle, 0, 0, 0, status, storage + 0x104, 4, offset, {emu.memory}));

        std::ifstream written{directory / "offsets.bin", std::ios::binary};
        const std::string contents{std::istreambuf_iterator<char>(written), {}};
        EXPECT_EQ(std::string("AAAA\0\0\0\0BBBB", 12), contents);
    }

    TEST(FileWriteTest, LocksFarPastTheEndOfTheFileAreGranted)
    {
        const auto directory = create_test_directory();
        const auto cleanup = utils::finally([&] {
            std::error_code error{};
            std::filesystem::remove_all(directory, error);
        });

        auto emu = create_empty_emulator();
        emu.file_sys.map(windows_path{'x', {}}, directory, filesystem_mode::read_write);
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        const auto file_handle = create_guest_file(emu, context, storage, u"\\??\\x:\\locked.bin", GENERIC_READ | GENERIC_WRITE);

        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{emu.memory, storage + 0x10};
        const emulator_object<LARGE_INTEGER> offset{emu.memory, storage + 0x20};
        const emulator_object<LARGE_INTEGER> length{emu.memory, storage + 0x30};
        offset.write(LARGE_INTEGER{.QuadPart = static_cast<LONGLONG>(0xFFFFFFFF00000000ULL)});
        length.write(LARGE_INTEGER{.QuadPart = 1});

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtLockFile(context, file_handle, {}, 0, 0, status, offset, length, 0, TRUE, FALSE));
    }

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
            emu.file_sys.map(windows_path{'x', {}}, directory, filesystem_mode::read_write);
            const auto path = windows_path{'x', {append ? u"append.bin" : u"write.bin"}};
            const auto* const mode = append ? u"a+b" : u"w+b";
            file writer{};
            writer.handle = guest_file{emu.file_sys, path, mode, emu.file_sys.open(path, mode).file};
            ASSERT_TRUE(writer.handle);
            const auto writer_handle = emu.process.files.store(std::move(writer));

            const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
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
                reader.handle = guest_file{emu.file_sys, path, u"rb", emu.file_sys.open(path, u"rb").file};
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

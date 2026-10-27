#include "emulation_test_utils.hpp"

#include <syscall_utils.hpp>
#include <utils/finally.hpp>

#include <fstream>
#include <random>

namespace sogen::syscalls
{
    NTSTATUS handle_NtFsControlFile(const syscall_context& c, handle file_handle, handle event, emulator_pointer apc_routine,
                                    emulator_pointer apc_context, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                    ULONG fs_control_code, emulator_pointer input_buffer, ULONG input_buffer_length,
                                    emulator_pointer output_buffer, ULONG output_buffer_length);
}

namespace sogen::test
{
    TEST(FileControlTest, IsVolumeMountedAcceptsFileAndDirectoryHandles)
    {
        const auto directory =
            std::filesystem::temp_directory_path() / ("sogen-file-control-test-" + std::to_string(std::random_device{}()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto cleanup = utils::finally([&] {
            std::error_code error{};
            std::filesystem::remove(directory / "sample.bin", error);
            std::filesystem::remove(directory, error);
        });
        std::ofstream{directory / "sample.bin", std::ios::binary}.put('x');

        auto emu = create_empty_emulator();
        emu.file_sys.map(windows_path{'x', {}}, directory);

        const auto path = windows_path{'x', {u"sample.bin"}};
        file regular_file{};
        regular_file.handle = guest_file{emu.file_sys, path, u"rb", emu.file_sys.open(path, u"rb").file};
        ASSERT_TRUE(regular_file.handle);
        const auto regular_handle = emu.process.files.store(std::move(regular_file));
        file opened_directory{};
        opened_directory.handle = guest_file{emu.file_sys, windows_path{'x', {}}};
        const auto directory_handle = emu.process.files.store(std::move(opened_directory));

        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{emu.memory, storage};
        constexpr ULONG is_volume_mounted = 0x90028;

        for (const auto file_handle : {regular_handle, directory_handle})
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial_status{};
            initial_status.Status = STATUS_UNSUCCESSFUL;
            initial_status.Information = 123;
            status.write(initial_status);

            EXPECT_EQ(STATUS_SUCCESS,
                      syscalls::handle_NtFsControlFile(context, file_handle, {}, 0, 0, status, is_volume_mounted, 0, 0, 0, 0));
            EXPECT_EQ(STATUS_SUCCESS, status.read().Status);
            EXPECT_EQ(0, status.read().Information);
        }

        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtFsControlFile(context, {}, {}, 0, 0, status, is_volume_mounted, 0, 0, 0, 0));
    }
}

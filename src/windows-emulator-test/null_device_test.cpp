#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, handle file_handle,
                                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                                 uint64_t fs_information, ULONG length, FS_INFORMATION_CLASS fs_information_class);
}

namespace sogen::test
{
    TEST(NullDeviceTest, ReportsNullDeviceTypeAndNativeCharacteristics)
    {
        auto emu = create_empty_emulator();
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.emu(), .vcpu = emu.vcpu(0), .proc = emu.process};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status{emu.memory, storage};
        const emulator_object<FILE_FS_DEVICE_INFORMATION> information{emu.memory, storage + 0x100};
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtQueryVolumeInformationFile(context, NUL_HANDLE, status, information.value(),
                                                                sizeof(FILE_FS_DEVICE_INFORMATION), FileFsDeviceInformation));
        EXPECT_EQ(FILE_DEVICE_NULL, information.read().DeviceType);
        EXPECT_EQ(FILE_DEVICE_SECURE_OPEN, information.read().Characteristics);
        EXPECT_EQ(STATUS_SUCCESS, status.read().Status);
        EXPECT_EQ(sizeof(FILE_FS_DEVICE_INFORMATION), status.read().Information);
    }
}

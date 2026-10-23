#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                               ULONG open_options);
    NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, handle file_handle,
                                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                                 uint64_t fs_information, ULONG length, FS_INFORMATION_CLASS fs_information_class);
}

namespace sogen::test
{
    TEST(SrpDeviceTest, ReportsTheAbsentPolicyDriverWithoutCreatingAHandle)
    {
        windows_emulator emu{create_arm64_emulator(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false}};
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        emulator_allocator allocator{emu.memory, storage, 0x1000};
        const auto name = allocator.make_unicode_string(u"\\Device\\SrpDevice");
        const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
        OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> object_attributes{};
        object_attributes.Length = sizeof(object_attributes);
        object_attributes.ObjectName = name.value();
        attributes.write(object_attributes);
        const auto output = allocator.reserve<handle>();
        constexpr handle sentinel{.bits = 0xDEADBEEF};
        output.write(sentinel);
        const auto status = allocator.reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};

        EXPECT_EQ(STATUS_NO_SUCH_DEVICE, syscalls::handle_NtOpenFile(context, output, GENERIC_READ, attributes, status, 0, 0));
        EXPECT_EQ(output.read(), sentinel);
    }

    TEST(NullDeviceTest, ReportsNullDeviceTypeAndNativeCharacteristics)
    {
        auto emu = create_empty_emulator();
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const syscall_context context{.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
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

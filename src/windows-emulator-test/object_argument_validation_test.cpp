#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtRaiseHardError(const syscall_context& c, NTSTATUS error_status, ULONG number_of_parameters,
                                     emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> unicode_string_parameter_mask,
                                     uint64_t parameters, HARDERROR_RESPONSE_OPTION valid_response_option,
                                     emulator_object<HARDERROR_RESPONSE> response);
    NTSTATUS handle_NtQueryObject(const syscall_context& c, handle handle, OBJECT_INFORMATION_CLASS object_information_class,
                                  emulator_pointer object_information, ULONG object_information_length,
                                  emulator_object<ULONG> return_length);
    NTSTATUS handle_NtQuerySecurityObject(const syscall_context& c, handle h, SECURITY_INFORMATION security_information,
                                          emulator_pointer security_descriptor, ULONG length, emulator_object<ULONG> length_needed);
    NTSTATUS handle_NtAreMappedFilesTheSame(const syscall_context& c, uint64_t file1_mapped_as_an_image, uint64_t file2_mapped_as_file);
    NTSTATUS handle_NtCreateIoCompletion(const syscall_context& c, emulator_object<handle> io_completion_handle, ACCESS_MASK desired_access,
                                         emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                         ULONG number_of_concurrent_threads);
    NTSTATUS handle_NtRemoveIoCompletionEx(const syscall_context& c, handle io_completion_handle,
                                           emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> io_completion_information,
                                           ULONG count, emulator_object<ULONG> num_entries_removed, emulator_object<LARGE_INTEGER> timeout,
                                           BOOLEAN alertable);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t untouched = 0xAAAAAAAA;
        constexpr auto ntdll_path = R"(C:\Windows\System32\ntdll.dll)";
        constexpr auto kernel32_path = R"(C:\Windows\System32\kernel32.dll)";

        class ObjectArgumentValidationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<ULONG> untouched_slot(const uint64_t offset)
            {
                const emulator_object<ULONG> slot{this->emu->memory, this->storage + offset};
                slot.write(untouched);
                return slot;
            }

            uint64_t map_image(const char* path)
            {
                const auto* mod = this->emu->mod_manager.map_module(path, this->emu->log);
                if (!mod)
                {
                    throw std::runtime_error(std::string("Failed to map ") + path);
                }

                return mod->image_base;
            }

            uint64_t map_data_view(const std::u16string& filename)
            {
                const auto address =
                    this->emu->memory.allocate_memory(0x1000, memory_permission::read, false, 0, memory_region_kind::file_section_view);
                this->emu->memory.set_region_mapped_filename(address, filename);
                return address;
            }

            NTSTATUS same(const uint64_t first, const uint64_t second)
            {
                return syscalls::handle_NtAreMappedFilesTheSame(this->context(), first, second);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(ObjectArgumentValidationTest, RaiseHardErrorRejectsTooManyParametersWithoutStopping)
    {
        for (const ULONG count : {6u, 32u, 0xFFFFFFFFu})
        {
            const auto response = this->untouched_slot(0);
            const emulator_object<HARDERROR_RESPONSE> response_object{this->emu->memory, response.value()};
            EXPECT_EQ(STATUS_INVALID_PARAMETER_2, syscalls::handle_NtRaiseHardError(this->context(), 0x40000000, count, {this->emu->memory},
                                                                                    0, OptionOk, response_object))
                << count;
            EXPECT_EQ(untouched, response.read()) << count;
            EXPECT_FALSE(this->emu->process.exit_status.has_value()) << count;
        }
    }

    TEST_F(ObjectArgumentValidationTest, QueryObjectBasicInformationRequiresTheExactLength)
    {
        const auto information = this->storage + 0x100;
        for (const ULONG length : {0u, 48u, 55u, 57u, 64u})
        {
            const auto returned = this->untouched_slot(0);
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, syscalls::handle_NtQueryObject(this->context(), CURRENT_PROCESS, ObjectBasicInformation,
                                                                                  information, length, returned))
                << length;
            EXPECT_EQ(untouched, returned.read()) << length;
        }

        const auto returned = this->untouched_slot(0);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryObject(this->context(), CURRENT_PROCESS, ObjectBasicInformation, information,
                                                                 sizeof(OBJECT_BASIC_INFORMATION), returned));
        EXPECT_EQ(sizeof(OBJECT_BASIC_INFORMATION), returned.read());
    }

    TEST_F(ObjectArgumentValidationTest, QuerySecurityObjectWithoutInformationReturnsAnEmptyDescriptor)
    {
        const auto needed = this->untouched_slot(0);
        const auto descriptor = this->storage + 0x100;
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQuerySecurityObject(this->context(), CURRENT_PROCESS, 0, descriptor, 0x200, needed));
        EXPECT_EQ(sizeof(SECURITY_DESCRIPTOR_RELATIVE), needed.read());

        const auto header = emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor}.read();
        EXPECT_EQ(SECURITY_DESCRIPTOR_REVISION, header.Revision);
        EXPECT_EQ(SE_SELF_RELATIVE, header.Control);
        EXPECT_EQ(0u, header.Owner);
        EXPECT_EQ(0u, header.Group);
        EXPECT_EQ(0u, header.Sacl);
        EXPECT_EQ(0u, header.Dacl);
    }

    TEST_F(ObjectArgumentValidationTest, QuerySecurityObjectDeniesSaclAccess)
    {
        for (const SECURITY_INFORMATION information :
             {SACL_SECURITY_INFORMATION, SACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
              SACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, BACKUP_SECURITY_INFORMATION})
        {
            const auto needed = this->untouched_slot(0);
            EXPECT_EQ(STATUS_ACCESS_DENIED, syscalls::handle_NtQuerySecurityObject(this->context(), CURRENT_PROCESS, information,
                                                                                   this->storage + 0x100, 0x200, needed))
                << information;
            EXPECT_EQ(untouched, needed.read()) << information;
        }

        for (const SECURITY_INFORMATION information : {OWNER_SECURITY_INFORMATION, DACL_SECURITY_INFORMATION})
        {
            const auto needed = this->untouched_slot(0);
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQuerySecurityObject(this->context(), CURRENT_PROCESS, information,
                                                                             this->storage + 0x100, 0x200, needed))
                << information;
        }
    }

    TEST_F(ObjectArgumentValidationTest, AreMappedFilesTheSameRejectsUnmappedAddresses)
    {
        const auto image = this->map_image(ntdll_path);
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->same(0, 0));
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->same(image, 0));
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->same(0, image));
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->same(image, 0x10000));
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->same(image, 0xFFFF800000000000ULL));
        EXPECT_EQ(STATUS_INVALID_ADDRESS, this->same(this->storage, 0));
    }

    TEST_F(ObjectArgumentValidationTest, AreMappedFilesTheSameAcceptsAddressesInOneView)
    {
        const auto image = this->map_image(ntdll_path);
        EXPECT_EQ(STATUS_SUCCESS, this->same(image, image + 0x1000));
        EXPECT_EQ(STATUS_SUCCESS, this->same(this->storage, this->storage + 0x10));

        const auto data = this->map_data_view(u"C:\\Windows\\System32\\ntdll.dll");
        EXPECT_EQ(STATUS_SUCCESS, this->same(data, data + 1));
    }

    TEST_F(ObjectArgumentValidationTest, AreMappedFilesTheSameRejectsMemoryNotBackedByAFile)
    {
        const auto image = this->map_image(ntdll_path);
        const auto pagefile =
            this->emu->memory.allocate_memory(0x1000, memory_permission::read_write, false, 0, memory_region_kind::pagefile_section_view);
        EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->same(image, this->storage));
        EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->same(this->storage, image));
        EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->same(image, pagefile));
        EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->same(pagefile, image));
        EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->same(pagefile, this->storage));
    }

    TEST_F(ObjectArgumentValidationTest, AreMappedFilesTheSameComparesTheImageAgainstTheMappedFile)
    {
        const auto image = this->map_image(ntdll_path);
        const auto other_image = this->map_image(kernel32_path);
        const auto data = this->map_data_view(u"C:\\Windows\\System32\\NTDLL.dll");
        const auto other_data = this->map_data_view(u"C:\\Windows\\System32\\kernel32.dll");

        EXPECT_EQ(STATUS_SUCCESS, this->same(image, data));
        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, this->same(data, image));
        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, this->same(image, other_image));
        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, this->same(image, other_data));
        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, this->same(data, other_data));
        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, this->same(data, this->map_data_view(u"C:\\Windows\\System32\\ntdll.dll")));
    }

    TEST_F(ObjectArgumentValidationTest, RemoveIoCompletionExRejectsEntryCountsThatOverflowTheBuffer)
    {
        const emulator_object<handle> port_slot{this->emu->memory, this->storage};
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateIoCompletion(this->context(), port_slot, 0, {this->emu->memory}, 0));
        const auto port = port_slot.read();

        const emulator_object<LARGE_INTEGER> zero_timeout{this->emu->memory, this->storage + 0x10};
        zero_timeout.write(LARGE_INTEGER{});
        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> entries{this->emu->memory, this->storage + 0x100};

        for (const ULONG count : {0u, 0x8000000u, 0xFFFFFFFFu})
        {
            const auto removed = this->untouched_slot(0x20);
            EXPECT_EQ(STATUS_INVALID_PARAMETER,
                      syscalls::handle_NtRemoveIoCompletionEx(this->context(), port, entries, count, removed, zero_timeout, FALSE))
                << count;
            EXPECT_EQ(untouched, removed.read()) << count;
        }

        const auto removed = this->untouched_slot(0x20);
        EXPECT_EQ(STATUS_TIMEOUT, syscalls::handle_NtRemoveIoCompletionEx(this->context(), port, entries, 1, removed, zero_timeout, FALSE));
        EXPECT_EQ(0u, removed.read());
    }
}

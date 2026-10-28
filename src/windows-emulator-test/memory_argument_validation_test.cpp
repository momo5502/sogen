#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                           emulator_object<uint32_t> bytes_to_protect, uint32_t protection,
                                           emulator_object<uint32_t> old_protection);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, handle process_handle, uint64_t base_address);
    NTSTATUS handle_NtCreateSection(const syscall_context& c, emulator_object<handle> section_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                    emulator_object<ULARGE_INTEGER> maximum_size, ULONG section_page_protection,
                                    ULONG allocation_attributes, handle file_handle);
    NTSTATUS handle_NtQuerySection(const syscall_context& c, handle section_handle, SECTION_INFORMATION_CLASS section_information_class,
                                   uint64_t section_information, EmulatorTraits<Emu64>::SIZE_T section_information_length,
                                   emulator_object<EmulatorTraits<Emu64>::SIZE_T> result_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t untouched = 0xAAAAAAAA;

        struct protect_result
        {
            NTSTATUS status;
            uint64_t base;
            uint32_t size;
            uint32_t old_protection;
        };

        class MemoryArgumentValidationTest : public testing::Test
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

            protect_result protect(const uint64_t base, const uint32_t size, const uint32_t protection)
            {
                const emulator_object<uint64_t> base_address{this->emu->memory, this->storage};
                const emulator_object<uint32_t> bytes_to_protect{this->emu->memory, this->storage + 0x100};
                const emulator_object<uint32_t> old_protection{this->emu->memory, this->storage + 0x200};
                base_address.write(base);
                bytes_to_protect.write(size);
                old_protection.write(untouched);

                const auto status = syscalls::handle_NtProtectVirtualMemory(this->context(), CURRENT_PROCESS, base_address,
                                                                            bytes_to_protect, protection, old_protection);
                return {status, base_address.read(), bytes_to_protect.read(), old_protection.read()};
            }

            handle create_data_section()
            {
                const emulator_object<handle> output{this->emu->memory, this->storage + 0x300};
                const emulator_object<ULARGE_INTEGER> maximum_size{this->emu->memory, this->storage + 0x400};
                maximum_size.write(ULARGE_INTEGER{.QuadPart = 0x2000});

                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSection(this->context(), output, 0, {this->emu->memory}, maximum_size,
                                                                           PAGE_READWRITE, SEC_COMMIT, {}));
                return output.read();
            }

            handle create_image_section()
            {
                section image{};
                image.maximum_size = 0x2000;
                image.allocation_attributes = SEC_IMAGE;
                return this->emu->process.sections.store(std::move(image));
            }

            NTSTATUS query(const handle section_handle, const uint32_t info_class, const uint64_t length, uint64_t& return_length)
            {
                const emulator_object<EmulatorTraits<Emu64>::SIZE_T> returned{this->emu->memory, this->storage + 0x500};
                returned.write(untouched);
                const auto status =
                    syscalls::handle_NtQuerySection(this->context(), section_handle, static_cast<SECTION_INFORMATION_CLASS>(info_class),
                                                    this->storage + 0x800, length, returned);
                return_length = returned.read();
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(MemoryArgumentValidationTest, UnmappingTheNullAddressReportsThatNoViewIsMapped)
    {
        EXPECT_EQ(STATUS_NOT_MAPPED_VIEW, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, 0));
    }

    TEST_F(MemoryArgumentValidationTest, ProtectRejectsEmptyAndOutOfUserSpaceRangesWithoutWritingBack)
    {
        const auto committed = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);

        struct range
        {
            uint64_t base;
            uint32_t size;
        };

        for (const auto [base, size] : {range{0x10000, 0}, range{0x10123, 0}, range{committed, 0}, range{0xFFFFFFFFFFFFF000, 0x2000},
                                        range{0x7FFFFFFE0000, 0x100000}, range{0x7FFFFFFE0000, 0x10001}, range{0x7FFFFFFF0000, 0x1000}})
        {
            const auto result = this->protect(base, size, PAGE_READWRITE);
            EXPECT_EQ(STATUS_INVALID_PARAMETER, result.status) << std::hex << base << " " << size;
            EXPECT_EQ(base, result.base) << std::hex << base << " " << size;
            EXPECT_EQ(size, result.size) << std::hex << base << " " << size;
            EXPECT_EQ(untouched, result.old_protection) << std::hex << base << " " << size;
        }
    }

    TEST_F(MemoryArgumentValidationTest, ProtectChecksTheProtectionBeforeTheRange)
    {
        EXPECT_EQ(STATUS_INVALID_PAGE_PROTECTION, this->protect(0x10000, 0, 0).status);
    }

    TEST_F(MemoryArgumentValidationTest, ProtectOfAnUnallocatedRangeUpToTheCeilingConflicts)
    {
        struct range
        {
            uint64_t base;
            uint32_t size;
        };

        for (const auto [base, size] : {range{0x7FFFFFFE0000, 0x1000}, range{0x7FFFFFFE0000, 0x10000}, range{0x7FFFFFFEFFFF, 1}})
        {
            EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->protect(base, size, PAGE_READWRITE).status) << std::hex << base << " " << size;
        }
    }

    TEST_F(MemoryArgumentValidationTest, ProtectOfACommittedRangeStillSucceeds)
    {
        const auto committed = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);

        const auto result = this->protect(committed + 0x123, 0x10, PAGE_READONLY);
        EXPECT_EQ(STATUS_SUCCESS, result.status);
        EXPECT_EQ(committed, result.base);
        EXPECT_EQ(0x1000u, result.size);
        EXPECT_EQ(static_cast<uint32_t>(PAGE_READWRITE), result.old_protection);
    }

    TEST_F(MemoryArgumentValidationTest, QueryingADataSectionRefusesImageAndUnknownClasses)
    {
        const auto section = this->create_data_section();

        uint64_t return_length{};
        EXPECT_EQ(STATUS_SUCCESS, this->query(section, 0, 128, return_length));

        for (const uint32_t info_class : {1u, 2u, 3u})
        {
            EXPECT_EQ(STATUS_SECTION_NOT_IMAGE, this->query(section, info_class, 128, return_length)) << info_class;
            EXPECT_EQ(untouched, return_length) << info_class;
        }

        for (const uint32_t info_class : {4u, 5u, 0x7FFFu})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query(section, info_class, 128, return_length)) << info_class;
            EXPECT_EQ(untouched, return_length) << info_class;
        }
    }

    TEST_F(MemoryArgumentValidationTest, QueryingASectionChecksTheLengthBeforeTheSectionKind)
    {
        const auto section = this->create_data_section();

        uint64_t return_length{};
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(section, 1, 63, return_length));
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(section, 2, 7, return_length));
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(section, 3, 7, return_length));
        EXPECT_EQ(STATUS_SECTION_NOT_IMAGE, this->query(section, 2, 8, return_length));
        EXPECT_EQ(STATUS_SECTION_NOT_IMAGE, this->query(section, 3, 8, return_length));
        EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query(section, 4, 0, return_length));
        EXPECT_EQ(untouched, return_length);
    }

    TEST_F(MemoryArgumentValidationTest, QueryingAnImageSectionRefusesUnknownClasses)
    {
        const auto section = this->create_image_section();

        uint64_t return_length{};
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(section, 2, 7, return_length));
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(section, 3, 7, return_length));

        for (const uint32_t info_class : {4u, 5u, 0x7FFFu})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query(section, info_class, 128, return_length)) << info_class;
            EXPECT_EQ(untouched, return_length) << info_class;
        }
    }
}

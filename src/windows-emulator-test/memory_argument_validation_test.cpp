#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                           emulator_object<uint32_t> bytes_to_protect, uint32_t protection,
                                           emulator_object<uint32_t> old_protection);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, handle process_handle, uint64_t base_address);
    NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                        emulator_object<uint64_t> bytes_to_allocate, uint32_t free_type);
    NTSTATUS handle_NtCreateSection(const syscall_context& c, emulator_object<handle> section_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                    emulator_object<ULARGE_INTEGER> maximum_size, ULONG section_page_protection,
                                    ULONG allocation_attributes, handle file_handle);
    NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, handle section_handle, handle process_handle,
                                       emulator_object<uint64_t> base_address,
                                       EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) zero_bits,
                                       EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) commit_size,
                                       emulator_object<LARGE_INTEGER> section_offset,
                                       emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                       SECTION_INHERIT inherit_disposition, ULONG allocation_type, ULONG win32_protect);
    NTSTATUS handle_NtFlushVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                         emulator_object<uint64_t> region_size,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);
    NTSTATUS handle_NtQuerySection(const syscall_context& c, handle section_handle, SECTION_INFORMATION_CLASS section_information_class,
                                   uint64_t section_information, EmulatorTraits<Emu64>::SIZE_T section_information_length,
                                   emulator_object<EmulatorTraits<Emu64>::SIZE_T> result_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t untouched = 0xAAAAAAAA;

        struct flush_result
        {
            NTSTATUS status;
            uint64_t base;
            uint64_t size;
            NTSTATUS io_status;
            uint64_t io_information;
        };

        struct protect_result
        {
            NTSTATUS status;
            uint64_t base;
            uint32_t size;
            uint32_t old_protection;
        };

        struct release_result
        {
            NTSTATUS status;
            uint64_t base;
            uint64_t size;
        };

        class free_recording_process_manager final : public process_manager
        {
          public:
            size_t free_calls{};

            process_create_result create_process(process_create_request) override
            {
                return {.error = process_error::unavailable};
            }

            process_error resume_process(managed_process) override
            {
                return process_error::unavailable;
            }

            process_error terminate_process(managed_process, uint64_t) override
            {
                return process_error::unavailable;
            }

            process_exit_status_result exit_status(managed_process) const override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_result allocate_memory(managed_process, uint64_t, uint64_t, memory_permission, bool, bool) override
            {
                return {.error = process_error::unavailable};
            }

            process_error free_memory(managed_process, uint64_t, uint64_t, bool) override
            {
                ++this->free_calls;
                return process_error::none;
            }

            process_memory_result protect_memory(managed_process, uint64_t, uint64_t, memory_permission) override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_read_result read_memory(managed_process, uint64_t, uint64_t) const override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_result write_memory(managed_process, uint64_t, std::span<const uint8_t>) override
            {
                return {.error = process_error::unavailable};
            }
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
                interfaces.processes = &this->manager;
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

            release_result release(const uint64_t base, const uint64_t size, const handle process = CURRENT_PROCESS,
                                   const uint32_t free_type = MEM_RELEASE)
            {
                const emulator_object<uint64_t> base_address{this->emu->memory, this->storage};
                const emulator_object<uint64_t> region_size{this->emu->memory, this->storage + 0x100};
                base_address.write(base);
                region_size.write(size);

                const auto status = syscalls::handle_NtFreeVirtualMemory(this->context(), process, base_address, region_size, free_type);
                return {status, base_address.read(), region_size.read()};
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

            uint64_t map_pagefile_view(const uint64_t size)
            {
                const emulator_object<handle> section{this->emu->memory, this->storage + 0x300};
                const emulator_object<ULARGE_INTEGER> maximum_size{this->emu->memory, this->storage + 0x400};
                maximum_size.write(ULARGE_INTEGER{.QuadPart = size});
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSection(this->context(), section, 0, {this->emu->memory}, maximum_size,
                                                                           PAGE_READWRITE, SEC_COMMIT, {}));

                const emulator_object<uint64_t> base{this->emu->memory, this->storage + 0x600};
                const emulator_object<EmulatorTraits<Emu64>::SIZE_T> view_size{this->emu->memory, this->storage + 0x608};
                base.write(0);
                view_size.write(0);
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtMapViewOfSection(this->context(), section.read(), CURRENT_PROCESS, base, 0, 0,
                                                              {this->emu->memory}, view_size, ViewUnmap, 0, PAGE_READWRITE));
                return base.read();
            }

            flush_result flush(const uint64_t base, const uint64_t size)
            {
                const emulator_object<uint64_t> base_address{this->emu->memory, this->storage + 0x900};
                const emulator_object<uint64_t> region_size{this->emu->memory, this->storage + 0x908};
                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{this->emu->memory, this->storage + 0x910};
                base_address.write(base);
                region_size.write(size);
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> sentinel{};
                sentinel.Status = untouched;
                sentinel.Information = untouched;
                io_status.write(sentinel);

                const auto status =
                    syscalls::handle_NtFlushVirtualMemory(this->context(), CURRENT_PROCESS, base_address, region_size, io_status);
                const auto block = io_status.read();
                return {status, base_address.read(), region_size.read(), block.Status, block.Information};
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

            free_recording_process_manager manager{};
            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(MemoryArgumentValidationTest, UnmappingTheNullAddressReportsThatNoViewIsMapped)
    {
        EXPECT_EQ(STATUS_NOT_MAPPED_VIEW, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, 0));
    }

    TEST_F(MemoryArgumentValidationTest, ReleasingAPageRangeSplitsThePrivateAllocation)
    {
        struct piece
        {
            uint64_t offset;
            bool reserved;
            uint64_t allocation_base;
        };

        struct release_case
        {
            uint64_t offset;
            uint64_t size;
            release_result expected;
            std::vector<piece> pieces;
        };

        const std::vector<release_case> cases{
            {0x0, 0x4000, {STATUS_SUCCESS, 0x0, 0x4000}, {{0x0, false, 0}, {0x4000, true, 0x4000}}},
            {0x4000, 0x4000, {STATUS_SUCCESS, 0x4000, 0x4000}, {{0x0, true, 0x0}, {0x4000, false, 0}, {0x8000, true, 0x8000}}},
            {0xC000, 0x4000, {STATUS_SUCCESS, 0xC000, 0x4000}, {{0x0, true, 0x0}, {0xC000, false, 0}}},
            {0x4FFF, 0x2, {STATUS_SUCCESS, 0x4000, 0x2000}, {{0x0, true, 0x0}, {0x4000, false, 0}, {0x6000, true, 0x6000}}},
            {0x8000, 0x10000, {STATUS_UNABLE_TO_FREE_VM, 0x8000, 0x10000}, {{0x0, true, 0x0}, {0x8000, true, 0x0}}},
        };

        for (const auto& test_case : cases)
        {
            const auto allocation = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
            ASSERT_NE(0u, allocation);

            const auto result = this->release(allocation + test_case.offset, test_case.size);
            EXPECT_EQ(test_case.expected.status, result.status) << test_case.offset;
            EXPECT_EQ(allocation + test_case.expected.base, result.base) << test_case.offset;
            EXPECT_EQ(test_case.expected.size, result.size) << test_case.offset;

            for (const auto& expected : test_case.pieces)
            {
                const auto info = this->emu->memory.get_region_info(allocation + expected.offset);
                EXPECT_EQ(expected.reserved, info.is_reserved) << test_case.offset << " " << expected.offset;
                if (expected.reserved)
                {
                    EXPECT_EQ(allocation + expected.allocation_base, info.allocation_base) << test_case.offset << " " << expected.offset;
                }
            }
        }
    }

    TEST_F(MemoryArgumentValidationTest, ReleaseWithDecommitIsRejectedForTheCurrentAndAChildProcess)
    {
        const auto allocation = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
        ASSERT_NE(0u, allocation);
        EXPECT_EQ(STATUS_INVALID_PARAMETER_4, this->release(allocation, 0, CURRENT_PROCESS, MEM_RELEASE | MEM_DECOMMIT).status);
        EXPECT_TRUE(this->emu->memory.get_region_info(allocation).is_reserved);

        emulator_process child{};
        child.process.value = 1;
        const auto child_handle = this->emu->process.processes.store(std::move(child));
        EXPECT_EQ(STATUS_INVALID_PARAMETER_4, this->release(allocation, 0, child_handle, MEM_RELEASE | MEM_DECOMMIT).status);
        EXPECT_EQ(0u, this->manager.free_calls);
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

    TEST_F(MemoryArgumentValidationTest, FlushChecksTheSizeAgainstTheViewWithoutWrapping)
    {
        constexpr uint64_t view_length = 0x10000;
        constexpr uint64_t offset = 0xFFF;
        const auto view = this->map_pagefile_view(view_length);
        ASSERT_NE(0u, view);

        struct flush_case
        {
            uint64_t size;
            flush_result expected;
        };

        const std::vector<flush_case> cases{
            {UINT64_MAX, {STATUS_INVALID_PARAMETER, view + offset, UINT64_MAX, untouched, untouched}},
            {view_length + 1, {STATUS_NOT_MAPPED_VIEW, view, view_length + 1, STATUS_NOT_MAPPED_VIEW, 0}},
            {view_length - offset, {STATUS_NOT_MAPPED_DATA, view, view_length - offset, STATUS_NOT_MAPPED_DATA, 0}},
        };

        for (const auto& test_case : cases)
        {
            const auto result = this->flush(view + offset, test_case.size);
            EXPECT_EQ(test_case.expected.status, result.status) << test_case.size;
            EXPECT_EQ(test_case.expected.base, result.base) << test_case.size;
            EXPECT_EQ(test_case.expected.size, result.size) << test_case.size;
            EXPECT_EQ(test_case.expected.io_status, result.io_status) << test_case.size;
            EXPECT_EQ(test_case.expected.io_information, result.io_information) << test_case.size;
        }
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

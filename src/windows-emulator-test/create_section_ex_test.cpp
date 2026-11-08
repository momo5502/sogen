#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateSectionEx(const syscall_context& c, emulator_object<handle> section_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                      emulator_object<ULARGE_INTEGER> maximum_size, ULONG section_page_protection,
                                      ULONG allocation_attributes, handle file_handle,
                                      emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters, ULONG extended_parameter_count);
    NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, handle section_handle, handle process_handle,
                                       emulator_object<uint64_t> base_address, EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR),
                                       EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T), emulator_object<LARGE_INTEGER> section_offset,
                                       emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size, SECTION_INHERIT,
                                       ULONG allocation_type, ULONG win32_protect);
    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, handle process_handle, uint64_t base_address);
    NTSTATUS handle_NtClose(const syscall_context& c, handle handle);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t untouched = 0xAAAAAAAA;
        constexpr uint64_t section_size = 0x10000;
        constexpr uint64_t parameters_offset = 0x800;

        class CreateSectionExTest : public testing::Test
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

            emulator_object<MEM_EXTENDED_PARAMETER64> parameters() const
            {
                return {this->emu->memory, this->storage + parameters_offset};
            }

            void set_parameter(const size_t index, const uint64_t type, const uint64_t value, const uint64_t reserved = 0) const
            {
                MEM_EXTENDED_PARAMETER64 parameter{};
                parameter.Type = type;
                parameter.Reserved = reserved;
                parameter.ULong64 = value;
                this->parameters().write(parameter, index);
            }

            NTSTATUS create(const emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters, const ULONG count)
            {
                const emulator_object<handle> output{this->emu->memory, this->storage};
                const emulator_object<ULARGE_INTEGER> maximum_size{this->emu->memory, this->storage + 0x100};
                this->emu->memory.write_memory<uint64_t>(this->storage, untouched);
                maximum_size.write(ULARGE_INTEGER{.QuadPart = section_size});

                return syscalls::handle_NtCreateSectionEx(this->context(), output, 0, {this->emu->memory}, maximum_size, PAGE_READWRITE,
                                                          SEC_COMMIT, {}, extended_parameters, count);
            }

            uint64_t output_bits() const
            {
                return this->emu->memory.read_memory<uint64_t>(this->storage);
            }

            handle output_handle() const
            {
                return this->emu->memory.read_memory<handle>(this->storage);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(CreateSectionExTest, WithoutParametersCreatesAMappableSection)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->create({this->emu->memory}, 0));
        const auto section = this->output_handle();

        const emulator_object<uint64_t> base_address{this->emu->memory, this->storage + 0x200};
        const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size{this->emu->memory, this->storage + 0x300};
        base_address.write(0);
        view_size.write(0);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtMapViewOfSection(this->context(), section, CURRENT_PROCESS, base_address, 0, 0,
                                                                      {this->emu->memory}, view_size, ViewUnmap, 0, PAGE_READWRITE));

        const auto view = base_address.read();
        const uint64_t mapped_size = view_size.read();
        EXPECT_EQ(section_size, mapped_size);
        this->emu->memory.write_memory<uint8_t>(view, 0x5A);
        EXPECT_EQ(0x5A, this->emu->memory.read_memory<uint8_t>(view));

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, view));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), section));
    }

    TEST_F(CreateSectionExTest, NumaNodeZeroIsAccepted)
    {
        this->set_parameter(0, MemSectionExtendedParameterNumaNode, 0);

        ASSERT_EQ(STATUS_SUCCESS, this->create(this->parameters(), 1));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), this->output_handle()));
    }

    TEST_F(CreateSectionExTest, ArrayAndCountMustAgree)
    {
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->parameters(), 0));
        EXPECT_EQ(untouched, this->output_bits());

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create({this->emu->memory}, 1));
        EXPECT_EQ(untouched, this->output_bits());
    }

    TEST_F(CreateSectionExTest, OnlyNumaNodeIsAcceptedForSections)
    {
        constexpr std::array<uint64_t, 8> rejected_types{MemSectionExtendedParameterInvalidType,
                                                         MemSectionExtendedParameterUserPhysicalFlags,
                                                         MemSectionExtendedParameterSigningLevel,
                                                         MemSectionExtendedParameterMax,
                                                         5,
                                                         6,
                                                         7,
                                                         0xFF};

        for (const auto type : rejected_types)
        {
            this->set_parameter(0, type, 0);
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->parameters(), 1)) << "type " << type;
            EXPECT_EQ(untouched, this->output_bits()) << "type " << type;
        }
    }

    TEST_F(CreateSectionExTest, InvalidNumaNodeEntriesAreRejected)
    {
        this->set_parameter(0, MemSectionExtendedParameterNumaNode, 0, 1);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->parameters(), 1));
        EXPECT_EQ(untouched, this->output_bits());

        this->set_parameter(0, MemSectionExtendedParameterNumaNode, 1);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->parameters(), 1));
        EXPECT_EQ(untouched, this->output_bits());

        this->set_parameter(0, MemSectionExtendedParameterNumaNode, 0);
        this->set_parameter(1, MemSectionExtendedParameterNumaNode, 0);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->parameters(), 2));
        EXPECT_EQ(untouched, this->output_bits());
    }

    TEST_F(CreateSectionExTest, ImageViewsRespectZeroBitsWhenTheSystemChoosesTheAddress)
    {
        section image{};
        image.file_name = u"C:\\Windows\\System32\\kernel32.dll";
        image.allocation_attributes = SEC_IMAGE;
        const auto section_handle = this->emu->process.sections.store(std::move(image));
        const emulator_object<uint64_t> base{this->emu->memory, this->storage};
        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> size{this->emu->memory, this->storage + 8};
        for (const uint64_t bits : {1, 2})
        {
            SCOPED_TRACE(bits);
            base.write(0);
            size.write(0);
            const auto status = syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, bits, 0,
                                                                    {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY);
            ASSERT_TRUE(NT_SUCCESS(status)) << std::hex << status;
            ASSERT_NE(base.read(), 0u);
            ASSERT_NE(size.read(), 0u);
            EXPECT_LE(base.read() + size.read() - 1, highest_address_for_zero_bits(bits));
            EXPECT_TRUE(NT_SUCCESS(syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, base.read())));
        }
    }

    TEST_F(CreateSectionExTest, AWin64ImageViewInAWow64ProcessCanUseTheNativeAddressSpace)
    {
        this->emu->process.is_wow64_process = true;
        section image{};
        image.file_name = u"C:\\Windows\\System32\\kernel32.dll";
        image.allocation_attributes = SEC_IMAGE;
        const auto section_handle = this->emu->process.sections.store(std::move(image));
        const emulator_object<uint64_t> base{this->emu->memory, this->storage};
        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> size{this->emu->memory, this->storage + 8};
        base.write(0);
        size.write(0);
        const auto status = syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, 0, 0,
                                                                {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY);
        ASSERT_TRUE(NT_SUCCESS(status)) << std::hex << status;
        EXPECT_GT(base.read(), uint64_t{0xFFFFFFFF});
    }

    TEST_F(CreateSectionExTest, ImageViewStatusDistinguishesInitialPlacementFromSecondaryViews)
    {
        this->emu->process.peb64 = emulator_object<PEB64>{this->emu->memory, this->storage + 0x100};
        this->emu->process.peb64.write(PEB64{});
        section image{};
        image.file_name = u"C:\\Windows\\System32\\kernel32.dll";
        image.allocation_attributes = SEC_IMAGE;
        const auto section_handle = this->emu->process.sections.store(std::move(image));
        const emulator_object<uint64_t> base{this->emu->memory, this->storage};
        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> size{this->emu->memory, this->storage + 8};
        for (const auto expected : {STATUS_SUCCESS, STATUS_IMAGE_NOT_AT_BASE})
        {
            SCOPED_TRACE(expected);
            ASSERT_NO_THROW(base.write(0));
            ASSERT_NO_THROW(size.write(0));
            NTSTATUS status{};
            ASSERT_NO_THROW(status = syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, 0, 0,
                                                                         {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY));
            ASSERT_TRUE(NT_SUCCESS(status));
            const auto* module = this->emu->mod_manager.find_by_address(base.read());
            ASSERT_NE(module, nullptr);
            EXPECT_EQ(status, expected);

            EXPECT_EQ(size.read(), module->size_of_image);
        }
    }

    TEST_F(CreateSectionExTest, AConstrainedImageFallsBackBelow4GiBAndReportsRelocation)
    {
        constexpr uint64_t high_start = 0x100000000;
        ASSERT_TRUE(this->emu->memory.release_memory(this->storage, 0));
        this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write, false, 0x10000);
        ASSERT_NE(this->storage, 0u);
        constexpr size_t reservation_size = 0x80000000;
        ASSERT_TRUE(this->emu->memory.allocate_memory(high_start, reservation_size, memory_permission::read_write, true));
        ASSERT_TRUE(
            this->emu->memory.allocate_memory(high_start + reservation_size, reservation_size, memory_permission::read_write, true));
        section image{};
        image.file_name = u"C:\\Windows\\System32\\kernel32.dll";
        image.allocation_attributes = SEC_IMAGE;
        const auto section_handle = this->emu->process.sections.store(std::move(image));
        const emulator_object<uint64_t> base{this->emu->memory, this->storage};
        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> size{this->emu->memory, this->storage + 8};
        base.write(0);
        size.write(0);
        EXPECT_EQ(STATUS_IMAGE_NOT_AT_BASE,
                  syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, high_start, 0,
                                                      {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY));
        ASSERT_NE(base.read(), 0u);
        ASSERT_NE(size.read(), 0u);
        EXPECT_LT(base.read() + size.read() - 1, high_start);
        ASSERT_NE(this->emu->mod_manager.find_by_address(base.read()), nullptr);
    }

    TEST_F(CreateSectionExTest, CachedImagesRespectTheAddressCeiling)
    {
        const windows_path file{"C:\\Windows\\System32\\kernel32.dll"};
        const auto* original = this->emu->mod_manager.map_module(file, this->emu->log);
        ASSERT_NE(original, nullptr);
        ASSERT_GT(original->image_base, uint64_t{0x7FFFFFFF});
        const auto count = this->emu->mod_manager.modules().size();
        NTSTATUS status{};
        EXPECT_EQ(this->emu->mod_manager.map_module(file, this->emu->log, false, false, 0, 0x7FFFFFFF, &status), nullptr);
        EXPECT_EQ(status, STATUS_NO_MEMORY);
        EXPECT_EQ(this->emu->mod_manager.modules().size(), count);
        EXPECT_EQ(this->emu->mod_manager.map_module(file, this->emu->log, false, false, 0, 0, &status), original);
        EXPECT_EQ(status, STATUS_SUCCESS);
    }

    TEST_F(CreateSectionExTest, AnImpossibleImageViewConstraintDoesNotPublishAModule)
    {
        section image{};
        image.file_name = u"C:\\Windows\\System32\\kernel32.dll";
        image.allocation_attributes = SEC_IMAGE;
        const auto section_handle = this->emu->process.sections.store(std::move(image));
        const emulator_object<uint64_t> base{this->emu->memory, this->storage};
        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> size{this->emu->memory, this->storage + 8};
        const auto module_count = this->emu->mod_manager.modules().size();

        struct constraint_case
        {
            uint64_t zero_bits;
            NTSTATUS expected;
        };

        const std::array cases{
            constraint_case{16, STATUS_NO_MEMORY},
            constraint_case{17, STATUS_NO_MEMORY},
            constraint_case{20, STATUS_NO_MEMORY},
            constraint_case{21, STATUS_NO_MEMORY},
            constraint_case{22, STATUS_INVALID_PARAMETER},
            constraint_case{31, STATUS_INVALID_PARAMETER},
            constraint_case{32, STATUS_INVALID_PARAMETER},
            constraint_case{0x10000, STATUS_NO_MEMORY},
            constraint_case{0x3FF, STATUS_INVALID_PARAMETER},
            constraint_case{0x400, STATUS_NO_MEMORY},
        };
        for (const auto& configuration : cases)
        {
            SCOPED_TRACE(configuration.zero_bits);
            base.write(0);
            size.write(0);
            const auto status =
                syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, configuration.zero_bits, 0,
                                                    {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY);
            EXPECT_EQ(configuration.expected, status);
            EXPECT_EQ(0u, base.read());
            EXPECT_EQ(0u, size.read());
            EXPECT_EQ(module_count, this->emu->mod_manager.modules().size());
        }
    }

    TEST_F(CreateSectionExTest, ExplicitImageBasesMustSatisfyZeroBits)
    {
        section image{};
        image.allocation_attributes = SEC_IMAGE;
        image.file_name = u"C:\\Windows\\System32\\kernel32.dll";
        const auto section_handle = this->emu->process.sections.store(std::move(image));
        const emulator_object<uint64_t> base{this->emu->memory, this->storage};
        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> size{this->emu->memory, this->storage + 8};
        const auto count = this->emu->mod_manager.modules().size();
        for (const uint64_t bits : {3, 20, 22, 32, 0x400})
        {
            SCOPED_TRACE(bits);
            base.write(0x20000000);
            size.write(0);
            EXPECT_EQ(STATUS_INVALID_PARAMETER,
                      syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, bits, 0,
                                                          {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY));
            EXPECT_EQ(base.read(), 0x20000000u);
            EXPECT_EQ(size.read(), 0u);
            EXPECT_EQ(this->emu->mod_manager.modules().size(), count);
        }
        for (const uint64_t bits : {1, 2})
        {
            SCOPED_TRACE(bits);
            base.write(0x20000000);
            size.write(0);
            ASSERT_TRUE(NT_SUCCESS(syscalls::handle_NtMapViewOfSection(this->context(), section_handle, CURRENT_PROCESS, base, bits, 0,
                                                                       {this->emu->memory}, size, ViewUnmap, 0, PAGE_READONLY)));
            EXPECT_LE(base.read() + size.read() - 1, highest_address_for_zero_bits(bits));
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, base.read()));
        }
    }

    TEST_F(CreateSectionExTest, AFileErrorDoesNotBecomeAnAllocationError)
    {
        NTSTATUS status = STATUS_NO_MEMORY;
        const auto count = this->emu->mod_manager.modules().size();
        EXPECT_EQ(
            this->emu->mod_manager.map_module(windows_path{"C:\\sogen-missing-image.dll"}, this->emu->log, false, false, 0, 0, &status),
            nullptr);
        EXPECT_EQ(STATUS_FILE_INVALID, status);
        EXPECT_EQ(count, this->emu->mod_manager.modules().size());
    }

    TEST_F(CreateSectionExTest, UnreadableEntryIsAnAccessViolation)
    {
        const auto region = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write, true);
        ASSERT_NE(region, 0u);
        ASSERT_TRUE(this->emu->memory.commit_memory(region, 0x1000, memory_permission::read_write));

        const emulator_object<MEM_EXTENDED_PARAMETER64> last_entry{this->emu->memory, region + 0x1000 - sizeof(MEM_EXTENDED_PARAMETER64)};
        last_entry.write(MEM_EXTENDED_PARAMETER64{});

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->create(last_entry, 2));
        EXPECT_EQ(untouched, this->output_bits());
    }
}

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

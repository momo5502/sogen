#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateSection(const syscall_context& c, emulator_object<handle> section_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                    emulator_object<ULARGE_INTEGER> maximum_size, ULONG section_page_protection,
                                    ULONG allocation_attributes, handle file_handle);
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
        constexpr size_t section_size = 0x2000;

        class SectionViewLifetimeTest : public testing::Test
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

            handle create_section()
            {
                const emulator_object<handle> output{this->emu->memory, this->storage};
                const emulator_object<ULARGE_INTEGER> maximum_size{this->emu->memory, this->storage + 0x100};
                maximum_size.write(ULARGE_INTEGER{.QuadPart = section_size});

                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSection(this->context(), output, 0, {this->emu->memory}, maximum_size,
                                                                           PAGE_READWRITE, SEC_COMMIT, {}));
                return output.read();
            }

            uint64_t map_view(const handle section, const uint64_t offset = 0, const uint64_t size = 0)
            {
                const emulator_object<uint64_t> base_address{this->emu->memory, this->storage + 0x200};
                const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size{this->emu->memory,
                                                                                                      this->storage + 0x300};
                const emulator_object<LARGE_INTEGER> section_offset{this->emu->memory, this->storage + 0x400};
                base_address.write(0);
                view_size.write(size);
                section_offset.write(LARGE_INTEGER{.QuadPart = static_cast<int64_t>(offset)});

                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtMapViewOfSection(this->context(), section, CURRENT_PROCESS, base_address, 0, 0,
                                                                              section_offset, view_size, ViewUnmap, 0, PAGE_READWRITE));
                return base_address.read();
            }

            bool is_committed(const uint64_t address)
            {
                return this->emu->memory.get_region_info(address).is_committed;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(SectionViewLifetimeTest, ViewSurvivesClosingTheSectionHandle)
    {
        const auto section = this->create_section();
        const auto view = this->map_view(section);
        ASSERT_NE(view, 0u);

        this->emu->memory.write_memory<uint32_t>(view, 0x600df00d);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), section));

        ASSERT_TRUE(this->is_committed(view));
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(view));
        this->emu->memory.write_memory<uint32_t>(view, 0x1badb002);
        EXPECT_EQ(0x1badb002u, this->emu->memory.read_memory<uint32_t>(view));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, view));
        EXPECT_FALSE(this->is_committed(view));
    }

    TEST_F(SectionViewLifetimeTest, BackingIsReleasedWithTheLastViewOnly)
    {
        const auto section = this->create_section();
        const auto first = this->map_view(section, 0, 0x1000);
        const auto second = this->map_view(section, 0x1000, 0x1000);
        ASSERT_NE(first, 0u);
        ASSERT_NE(second, 0u);
        ASSERT_NE(first, second);

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), section));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, first));
        EXPECT_TRUE(this->is_committed(second));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, second));
        EXPECT_FALSE(this->is_committed(second));
    }

    TEST_F(SectionViewLifetimeTest, BackingIsReleasedWhenTheHandleIsClosedAfterTheViews)
    {
        const auto section = this->create_section();
        const auto view = this->map_view(section);
        ASSERT_NE(view, 0u);

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, view));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), section));
        EXPECT_FALSE(this->is_committed(view));
    }

    TEST_F(SectionViewLifetimeTest, OverlappingViewsShareMemory)
    {
        const auto section = this->create_section();
        const auto whole = this->map_view(section, 0, section_size);
        const auto tail = this->map_view(section, 0x1000, 0x1000);
        ASSERT_NE(whole, 0u);
        ASSERT_NE(tail, 0u);

        this->emu->memory.write_memory<uint32_t>(whole + 0x1000, 0x600df00d);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(tail));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, whole));
        this->emu->memory.write_memory<uint32_t>(tail + 4, 0x1badb002);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(tail));
        EXPECT_EQ(0x1badb002u, this->emu->memory.read_memory<uint32_t>(tail + 4));
    }

    TEST_F(SectionViewLifetimeTest, OverlappingViewsSurviveASnapshot)
    {
        const auto section = this->create_section();
        const auto whole = this->map_view(section, 0, section_size);
        const auto tail = this->map_view(section, 0x1000, 0x1000);
        ASSERT_NE(whole, 0u);
        ASSERT_NE(tail, 0u);
        this->emu->memory.write_memory<uint32_t>(tail, 0x600df00d);
        ASSERT_TRUE(this->emu->memory.protect_memory(whole, 0x1000, memory_permission::read, nullptr));

        this->emu->save_snapshot();
        this->emu->memory.write_memory<uint32_t>(tail, 0x1badb002);
        this->emu->restore_snapshot();

        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(whole + 0x1000));
        EXPECT_EQ(nt_memory_permission{memory_permission::read}, this->emu->memory.get_region_info(whole).permissions);
        EXPECT_EQ(nt_memory_permission{memory_permission::read_write}, this->emu->memory.get_region_info(tail).permissions);
        this->emu->memory.write_memory<uint32_t>(whole + 0x1004, 0xcafef00d);
        EXPECT_EQ(0xcafef00du, this->emu->memory.read_memory<uint32_t>(tail + 4));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), section));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, whole));
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(tail));
    }
}

#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                 emulator_object<LARGE_INTEGER> allocation_size, ULONG file_attributes, ULONG share_access,
                                 ULONG create_disposition, ULONG create_options, uint64_t ea_buffer, ULONG ea_length);
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
    NTSTATUS handle_NtWriteFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                                emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                                emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                            uint64_t zero_bits, emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                            uint32_t page_protection);
    NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                        emulator_object<uint64_t> bytes_to_allocate, uint32_t free_type);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t data_file_size = 3 * ALLOCATION_GRANULARITY + 0x123;

        std::byte data_file_byte(const uint64_t offset)
        {
            return static_cast<std::byte>((offset ^ (offset >> 8) ^ (offset >> 16)) & 0xFF);
        }

        std::vector<std::byte> data_file_bytes(const uint64_t offset, const uint64_t size)
        {
            std::vector<std::byte> bytes(static_cast<size_t>(size));
            for (uint64_t i = 0; i < size; ++i)
            {
                bytes[static_cast<size_t>(i)] = offset + i < data_file_size ? data_file_byte(offset + i) : std::byte{};
            }
            return bytes;
        }

        struct mapped_view
        {
            NTSTATUS status{};
            uint64_t address{};
            uint64_t size{};
            int64_t offset{};
        };

        class FileSectionViewTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->directory = std::filesystem::temp_directory_path() / ("sogen-file-section-view-test-" + std::to_string(getpid()) +
                                                                            "-" + std::to_string(std::random_device{}()));
                std::filesystem::create_directory(this->directory);

                const auto contents = data_file_bytes(0, data_file_size);
                std::ofstream{this->directory / "data.bin", std::ios::binary}.write(reinterpret_cast<const char*>(contents.data()),
                                                                                    static_cast<std::streamsize>(contents.size()));

                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->emu->file_sys.map(windows_path{'x', {}}, this->directory, filesystem_mode::read_write);
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            void TearDown() override
            {
                this->emu.reset();
                std::error_code error{};
                std::filesystem::remove_all(this->directory, error);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            handle open_data_file()
            {
                constexpr std::u16string_view name = u"\\??\\x:\\data.bin";
                const auto name_bytes = static_cast<USHORT>(name.size() * sizeof(char16_t));
                this->emu->memory.write_memory(this->storage + 0x800, name.data(), name_bytes);
                this->emu->memory.write_memory(this->storage + 0x700, UNICODE_STRING<EmulatorTraits<Emu64>>{
                                                                          .Length = name_bytes,
                                                                          .MaximumLength = name_bytes,
                                                                          .Buffer = this->storage + 0x800,
                                                                      });
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attributes{};
                attributes.Length = sizeof(attributes);
                attributes.ObjectName = this->storage + 0x700;
                this->emu->memory.write_memory(this->storage + 0x600, attributes);

                const emulator_object<handle> file_handle{this->emu->memory, this->storage};
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateFile(this->context(), file_handle, GENERIC_READ | GENERIC_WRITE,
                                                                        {this->emu->memory, this->storage + 0x600},
                                                                        {this->emu->memory, this->storage + 0x10}, {this->emu->memory}, 0,
                                                                        0, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, 0, 0));
                return file_handle.read();
            }

            handle create_section(const handle file, const ULONG protection, const uint64_t maximum_size = 0)
            {
                const emulator_object<handle> section_handle{this->emu->memory, this->storage + 0x20};
                const emulator_object<ULARGE_INTEGER> section_size{this->emu->memory, maximum_size ? this->storage + 0x28 : 0};
                if (maximum_size)
                {
                    section_size.write(ULARGE_INTEGER{.QuadPart = maximum_size});
                }

                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtCreateSection(this->context(), section_handle, section::all_access, {this->emu->memory},
                                                           section_size, protection, SEC_COMMIT, file));
                return section_handle.read();
            }

            mapped_view map_view(const handle section, const int64_t offset, const uint64_t size, const ULONG protection,
                                 const uint64_t base = 0, const ULONG allocation_type = 0)
            {
                const emulator_object<uint64_t> base_address{this->emu->memory, this->storage + 0x30};
                const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size{this->emu->memory,
                                                                                                      this->storage + 0x38};
                const emulator_object<LARGE_INTEGER> section_offset{this->emu->memory, this->storage + 0x40};
                base_address.write(base);
                view_size.write(size);
                section_offset.write(LARGE_INTEGER{.QuadPart = offset});

                mapped_view view{};
                view.status = syscalls::handle_NtMapViewOfSection(this->context(), section, CURRENT_PROCESS, base_address, 0, 0,
                                                                  section_offset, view_size, ViewUnmap, allocation_type, protection);
                view.address = base_address.read();
                view.size = view_size.read();
                view.offset = section_offset.read().QuadPart;
                return view;
            }

            std::vector<std::byte> read_guest(const uint64_t address, const uint64_t size)
            {
                std::vector<std::byte> bytes(static_cast<size_t>(size));
                this->emu->memory.read_memory(address, bytes.data(), bytes.size());
                return bytes;
            }

            void write_file(const handle file, const uint64_t offset, const uint32_t value)
            {
                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{this->emu->memory, this->storage + 0x50};
                const emulator_object<LARGE_INTEGER> byte_offset{this->emu->memory, this->storage + 0x70};
                byte_offset.write(LARGE_INTEGER{.QuadPart = static_cast<int64_t>(offset)});
                this->emu->memory.write_memory<uint32_t>(this->storage + 0x80, value);
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtWriteFile(this->context(), file, 0, 0, 0, io_status, this->storage + 0x80,
                                                                       sizeof(uint32_t), byte_offset, {this->emu->memory}));
            }

            void map_and_release_whole_file(const handle file)
            {
                const auto section = this->create_section(file, PAGE_READONLY);
                const auto view = this->map_view(section, 0, 0, PAGE_READONLY);
                ASSERT_EQ(STATUS_SUCCESS, view.status);
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, view.address));
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), section));
            }

            std::filesystem::path directory{};
            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(FileSectionViewTest, AViewMapsOnlyTheRequestedRangeOfTheFile)
    {
        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READONLY);

        const auto view = this->map_view(section, ALLOCATION_GRANULARITY, 0x1800, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);
        EXPECT_EQ(0x2000u, view.size);
        EXPECT_EQ(static_cast<int64_t>(ALLOCATION_GRANULARITY), view.offset);
        EXPECT_EQ(data_file_bytes(ALLOCATION_GRANULARITY, 0x2000), this->read_guest(view.address, 0x2000));

        const auto region = this->emu->memory.get_region_info(view.address);
        EXPECT_EQ(view.address, region.allocation_base);
        EXPECT_EQ(0x2000u, region.allocation_length);
    }

    TEST_F(FileSectionViewTest, AnOffsetIsRoundedDownToTheAllocationGranularity)
    {
        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READONLY);

        const auto view = this->map_view(section, ALLOCATION_GRANULARITY + 0x10, 0x10, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);
        EXPECT_EQ(static_cast<int64_t>(ALLOCATION_GRANULARITY), view.offset);
        EXPECT_EQ(0x1000u, view.size);
        EXPECT_EQ(data_file_bytes(ALLOCATION_GRANULARITY, 0x1000), this->read_guest(view.address, 0x1000));
    }

    TEST_F(FileSectionViewTest, AViewPastTheEndOfTheSectionIsRefused)
    {
        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READONLY);

        const auto view = this->map_view(section, 2 * ALLOCATION_GRANULARITY, 2 * ALLOCATION_GRANULARITY, PAGE_READONLY);
        EXPECT_EQ(STATUS_INVALID_VIEW_SIZE, view.status);
    }

    TEST_F(FileSectionViewTest, ViewsOfOneSectionShareTheirBytes)
    {
        if (!this->emu->memory.can_alias_host_memory_anywhere())
        {
            GTEST_SKIP() << "The backend cannot map host memory at a second address";
        }

        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READWRITE);

        const auto whole = this->map_view(section, 0, 0, PAGE_READWRITE);
        const auto middle = this->map_view(section, ALLOCATION_GRANULARITY, ALLOCATION_GRANULARITY, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, whole.status);
        ASSERT_EQ(STATUS_SUCCESS, middle.status);
        ASSERT_NE(whole.address, middle.address);

        this->emu->memory.write_memory<uint32_t>(whole.address + ALLOCATION_GRANULARITY + 8, 0x600df00d);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(middle.address + 8));

        this->emu->memory.write_memory<uint32_t>(middle.address + 0x10, 0x1badb002);
        EXPECT_EQ(0x1badb002u, this->emu->memory.read_memory<uint32_t>(whole.address + ALLOCATION_GRANULARITY + 0x10));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtUnmapViewOfSection(this->context(), CURRENT_PROCESS, whole.address));
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(middle.address + 8));
    }

    TEST_F(FileSectionViewTest, AWriteToTheFileReachesItsViews)
    {
        if (!this->emu->memory.can_alias_host_memory_anywhere())
        {
            GTEST_SKIP() << "The backend cannot map host memory at a second address";
        }

        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READONLY);
        const auto view = this->map_view(section, 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);

        constexpr uint64_t written_offset = ALLOCATION_GRANULARITY + 4;
        this->write_file(file, written_offset, 0x600df00d);

        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(view.address + written_offset));
    }

    TEST_F(FileSectionViewTest, AViewOfAFileWrittenSinceItsLastViewShowsTheNewBytes)
    {
        const auto file = this->open_data_file();
        this->map_and_release_whole_file(file);

        constexpr uint64_t written_offset = ALLOCATION_GRANULARITY + 4;
        this->write_file(file, written_offset, 0x600df00d);

        const auto view = this->map_view(this->create_section(file, PAGE_READONLY), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(view.address + written_offset));
    }

    TEST_F(FileSectionViewTest, AViewOfAFileAnotherProcessChangedShowsItsBytes)
    {
        const auto file = this->open_data_file();
        this->map_and_release_whole_file(file);

        constexpr uint64_t written_offset = 2 * ALLOCATION_GRANULARITY + 8;
        constexpr uint32_t written_value = 0x1badb002;
        const auto path = this->directory / "data.bin";
        const auto previous_write_time = std::filesystem::last_write_time(path);
        {
            std::fstream other_process{path, std::ios::binary | std::ios::in | std::ios::out};
            other_process.seekp(static_cast<std::streamoff>(written_offset));
            other_process.write(reinterpret_cast<const char*>(&written_value), sizeof(written_value));
        }
        std::filesystem::last_write_time(path, previous_write_time + std::chrono::seconds{1});

        const auto view = this->map_view(this->create_section(file, PAGE_READONLY), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);
        EXPECT_EQ(written_value, this->emu->memory.read_memory<uint32_t>(view.address + written_offset));
    }

    TEST_F(FileSectionViewTest, AFileViewCannotBeReset)
    {
        const auto file = this->open_data_file();
        const auto view = this->map_view(this->create_section(file, PAGE_READWRITE), 0, 0, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, view.status);

        const emulator_object<uint64_t> base_address{this->emu->memory, this->storage + 0x90};
        const emulator_object<uint64_t> region_size{this->emu->memory, this->storage + 0x98};
        base_address.write(view.address);
        region_size.write(0x1000);
        EXPECT_EQ(STATUS_USER_MAPPED_FILE, syscalls::handle_NtAllocateVirtualMemory(this->context(), CURRENT_PROCESS, base_address, 0,
                                                                                    region_size, MEM_RESET, PAGE_READWRITE));
    }

    TEST_F(FileSectionViewTest, AFileViewCannotBeFreed)
    {
        const auto file = this->open_data_file();
        const auto view = this->map_view(this->create_section(file, PAGE_READONLY), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);

        const emulator_object<uint64_t> base_address{this->emu->memory, this->storage + 0x90};
        const emulator_object<uint64_t> region_size{this->emu->memory, this->storage + 0x98};
        base_address.write(view.address);
        region_size.write(0);
        EXPECT_EQ(STATUS_UNABLE_TO_DELETE_SECTION,
                  syscalls::handle_NtFreeVirtualMemory(this->context(), CURRENT_PROCESS, base_address, region_size, MEM_RELEASE));
    }

    TEST_F(FileSectionViewTest, AViewTakesItsProtectionFromWin32Protect)
    {
        const auto file = this->open_data_file();
        const auto view = this->map_view(this->create_section(file, PAGE_READWRITE), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, view.status);
        EXPECT_EQ(static_cast<uint32_t>(PAGE_READONLY),
                  map_emulator_to_nt_protection(this->emu->memory.get_region_info(view.address).permissions));
    }

    TEST_F(FileSectionViewTest, AWritableViewOfACopyOnWriteSectionIsRefused)
    {
        const auto file = this->open_data_file();
        EXPECT_EQ(STATUS_SECTION_PROTECTION, this->map_view(this->create_section(file, PAGE_WRITECOPY), 0, 0, PAGE_READWRITE).status);
    }

    TEST_F(FileSectionViewTest, ACopyOnWriteViewKeepsItsWritesToItself)
    {
        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READWRITE);
        const auto copy_on_write = this->map_view(section, 0, 0, PAGE_WRITECOPY);
        const auto shared = this->map_view(section, 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, copy_on_write.status);
        ASSERT_EQ(STATUS_SUCCESS, shared.status);

        this->emu->memory.write_memory<uint32_t>(copy_on_write.address + ALLOCATION_GRANULARITY + 8, 0x600df00d);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(copy_on_write.address + ALLOCATION_GRANULARITY + 8));
        EXPECT_EQ(data_file_bytes(ALLOCATION_GRANULARITY + 8, sizeof(uint32_t)),
                  this->read_guest(shared.address + ALLOCATION_GRANULARITY + 8, sizeof(uint32_t)));

        const auto other_section = this->map_view(this->create_section(file, PAGE_READONLY), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, other_section.status);
        EXPECT_EQ(data_file_bytes(ALLOCATION_GRANULARITY + 8, sizeof(uint32_t)),
                  this->read_guest(other_section.address + ALLOCATION_GRANULARITY + 8, sizeof(uint32_t)));
    }

    TEST_F(FileSectionViewTest, ViewsOfSectionsOfDifferentSizesShareTheFilesBytes)
    {
        if (!this->emu->memory.can_alias_host_memory_anywhere())
        {
            GTEST_SKIP() << "The backend cannot map host memory at a second address";
        }

        const auto file = this->open_data_file();
        const auto first_page = this->map_view(this->create_section(file, PAGE_READWRITE, 0x1000), 0, 0, PAGE_READWRITE);
        const auto whole_file = this->map_view(this->create_section(file, PAGE_READWRITE), 0, 0, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, first_page.status);
        ASSERT_EQ(STATUS_SUCCESS, whole_file.status);
        EXPECT_EQ(0x1000u, first_page.size);

        this->emu->memory.write_memory<uint32_t>(first_page.address + 8, 0x600df00d);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(whole_file.address + 8));

        this->emu->memory.write_memory<uint32_t>(whole_file.address + 0x10, 0x1badb002);
        EXPECT_EQ(0x1badb002u, this->emu->memory.read_memory<uint32_t>(first_page.address + 0x10));

        this->write_file(file, 0x20, 0x0ddba11u);
        EXPECT_EQ(0x0ddba11u, this->emu->memory.read_memory<uint32_t>(first_page.address + 0x20));
        EXPECT_EQ(0x0ddba11u, this->emu->memory.read_memory<uint32_t>(whole_file.address + 0x20));
    }

    TEST_F(FileSectionViewTest, ASectionOfAFileThatGrewKeepsSharingWithItsEarlierViews)
    {
        if (!this->emu->memory.can_alias_host_memory_anywhere())
        {
            GTEST_SKIP() << "The backend cannot map host memory at a second address";
        }

        const auto file = this->open_data_file();
        const auto earlier = this->map_view(this->create_section(file, PAGE_READWRITE), 0, 0, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, earlier.status);

        constexpr uint64_t appended_offset = 3 * ALLOCATION_GRANULARITY + 0x2000;
        this->write_file(file, appended_offset, 0x0ddba11u);

        const auto later = this->map_view(this->create_section(file, PAGE_READWRITE), 0, 0, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, later.status);
        EXPECT_EQ(0x0ddba11u, this->emu->memory.read_memory<uint32_t>(later.address + appended_offset));

        this->emu->memory.write_memory<uint32_t>(earlier.address + 8, 0x600df00d);
        EXPECT_EQ(0x600df00du, this->emu->memory.read_memory<uint32_t>(later.address + 8));

        this->emu->memory.write_memory<uint32_t>(later.address + 0x10, 0x1badb002);
        EXPECT_EQ(0x1badb002u, this->emu->memory.read_memory<uint32_t>(earlier.address + 0x10));
    }

    TEST_F(FileSectionViewTest, ASectionOfAFileReplacedAtTheSameNameMapsTheNewFile)
    {
        const auto first_file = this->open_data_file();
        const auto first_view = this->map_view(this->create_section(first_file, PAGE_READONLY), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, first_view.status);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), first_file));

        std::filesystem::rename(this->directory / "data.bin", this->directory / "old.bin");
        auto replacement = data_file_bytes(0, data_file_size);
        for (auto& value : replacement)
        {
            value = ~value;
        }
        std::ofstream{this->directory / "data.bin", std::ios::binary}.write(reinterpret_cast<const char*>(replacement.data()),
                                                                            static_cast<std::streamsize>(replacement.size()));

        const auto second_view = this->map_view(this->create_section(this->open_data_file(), PAGE_READONLY), 0, 0, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, second_view.status);
        EXPECT_EQ(std::vector(replacement.begin(), replacement.begin() + 0x100), this->read_guest(second_view.address, 0x100));
        EXPECT_EQ(data_file_bytes(0, 0x100), this->read_guest(first_view.address, 0x100));
    }

    TEST_F(FileSectionViewTest, AViewReplacesAPlaceholderOnlyWhenAskedTo)
    {
        if (!this->emu->memory.can_alias_host_memory_anywhere())
        {
            GTEST_SKIP() << "The backend cannot map host memory at a second address";
        }

        const auto file = this->open_data_file();
        const auto section = this->create_section(file, PAGE_READONLY);
        const auto placeholder =
            this->emu->memory.allocate_memory(ALLOCATION_GRANULARITY, memory_permission::none, true, 0, memory_region_kind::placeholder);
        ASSERT_NE(0u, placeholder);

        EXPECT_EQ(STATUS_CONFLICTING_ADDRESSES, this->map_view(section, 0, ALLOCATION_GRANULARITY, PAGE_READONLY, placeholder).status);
        EXPECT_EQ(memory_region_kind::placeholder, this->emu->memory.get_region_kind(placeholder));

        const auto view = this->map_view(section, 0, ALLOCATION_GRANULARITY, PAGE_READONLY, placeholder, MEM_REPLACE_PLACEHOLDER);
        ASSERT_EQ(STATUS_SUCCESS, view.status);
        EXPECT_EQ(placeholder, view.address);

        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  this->map_view(section, 0, ALLOCATION_GRANULARITY, PAGE_READONLY, view.address, MEM_REPLACE_PLACEHOLDER).status);
    }
}

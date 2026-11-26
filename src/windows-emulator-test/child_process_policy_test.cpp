#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateUserProcess(const syscall_context& c, emulator_object<handle> process_handle,
                                        emulator_object<handle> thread_handle, ACCESS_MASK process_desired_access,
                                        ACCESS_MASK thread_desired_access,
                                        emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> process_object_attributes,
                                        emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> thread_object_attributes,
                                        ULONG process_flags, ULONG thread_flags,
                                        emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_parameters,
                                        emulator_object<PS_CREATE_INFO<EmulatorTraits<Emu64>>> create_info,
                                        emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list);
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class,
                                              uint64_t process_information, uint32_t process_information_length,
                                              emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        class counting_process_manager final : public process_manager
        {
          public:
            size_t creations{};

            process_create_result create_process(process_create_request) override
            {
                ++this->creations;
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
                return process_error::unavailable;
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

        constexpr std::u16string_view existing_image = u"C:\\Windows\\System32\\cmd.exe";

        constexpr uint8_t ne_target_os2 = 1;
        constexpr uint8_t ne_target_windows = 2;
        constexpr uint16_t ne_module_table = 0x40;
        constexpr uint16_t ne_imported_names = 0x50;
        constexpr uint16_t ne_entry_table = 0x60;
        constexpr uint16_t ne_first_imported_name = 1;
        constexpr std::string_view os2_api_module = "DOSCALLS";
        constexpr std::string_view os2_bind_stub = "16STUB";
        constexpr size_t os2_bind_stub_offset = 0x200;
        constexpr uint32_t new_header_offset = sizeof(PEDosHeader_t);
        constexpr uint32_t late_new_header_offset = 0x400;
        constexpr size_t test_image_size = 0x600;
        constexpr uint64_t pe_image_base = 0x140000000;
        constexpr uint32_t pe_section_alignment = 0x1000;
        constexpr uint32_t pe_file_alignment = 0x200;
        constexpr uint32_t pe_optional_header_offset = new_header_offset + sizeof(uint32_t) + sizeof(PEFileHeader_t);
        constexpr uint32_t pe_section_header_offset = new_header_offset + sizeof(PENTHeaders_t<std::uint64_t>);
        constexpr uint16_t largest_optional_header_size = 0xFFF8;
        constexpr uint32_t large_image_size = 0x20000;
        constexpr uint32_t page_size = 0x1000;

        template <typename T>
        void write_at(std::vector<std::byte>& image, const size_t offset, const T& value)
        {
            std::memcpy(image.data() + offset, &value, sizeof(value));
        }

        void write_at(std::vector<std::byte>& image, const size_t offset, const std::string_view text)
        {
            std::memcpy(image.data() + offset, text.data(), text.size());
        }

        std::vector<std::byte> dos_image(const uint32_t new_header)
        {
            std::vector<std::byte> image(test_image_size);
            PEDosHeader_t dos_header{};
            dos_header.e_magic = PEDosHeader_t::k_Magic;
            dos_header.e_lfanew = new_header;
            write_at(image, 0, dos_header);
            return image;
        }

        std::vector<std::byte> pe_image_without_executable_flag()
        {
            auto image = dos_image(new_header_offset);
            PENTHeaders_t<std::uint64_t> nt_headers{};
            nt_headers.Signature = PENTHeaders_t<std::uint64_t>::k_Signature;
            nt_headers.FileHeader.Machine = PEMachineType::AMD64;
            nt_headers.FileHeader.SizeOfOptionalHeader = sizeof(nt_headers.OptionalHeader);
            nt_headers.FileHeader.Characteristics = IMAGE_FILE_LARGE_ADDRESS_AWARE;
            nt_headers.OptionalHeader.Magic = PEOptionalHeader_t<std::uint64_t>::k_Magic;
            write_at(image, new_header_offset, nt_headers);
            return image;
        }

        template <typename Change>
        std::vector<std::byte> pe_image(const Change& change)
        {
            auto image = dos_image(new_header_offset);
            PENTHeaders_t<std::uint64_t> nt_headers{};
            nt_headers.Signature = PENTHeaders_t<std::uint64_t>::k_Signature;
            nt_headers.FileHeader.Machine = PEMachineType::AMD64;
            nt_headers.FileHeader.NumberOfSections = 1;
            nt_headers.FileHeader.SizeOfOptionalHeader = sizeof(nt_headers.OptionalHeader);
            nt_headers.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
            auto& optional_header = nt_headers.OptionalHeader;
            optional_header.Magic = PEOptionalHeader_t<std::uint64_t>::k_Magic;
            optional_header.ImageBase = pe_image_base;
            optional_header.SectionAlignment = pe_section_alignment;
            optional_header.FileAlignment = pe_file_alignment;
            optional_header.SizeOfImage = 2 * pe_section_alignment;
            optional_header.SizeOfHeaders = pe_file_alignment;
            optional_header.NumberOfRvaAndSizes = PEOptionalHeader_t<std::uint64_t>::k_NumberOfDataDirectors;

            IMAGE_SECTION_HEADER section{};
            section.VirtualAddress = pe_section_alignment;
            section.Misc.VirtualSize = pe_file_alignment;
            section.PointerToRawData = pe_file_alignment;
            section.SizeOfRawData = pe_file_alignment;

            change(nt_headers, section);
            write_at(image, new_header_offset, nt_headers);
            write_at(image, pe_section_header_offset, section);
            return image;
        }

        std::vector<std::byte> resized(std::vector<std::byte> image, const size_t size)
        {
            image.resize(size);
            return image;
        }

        PEOs2Header_t os2_header(const uint8_t target)
        {
            PEOs2Header_t header{};
            header.ne_magic = PEOs2Header_t::k_Magic;
            header.ne_enttab = ne_entry_table;
            header.ne_modtab = ne_module_table;
            header.ne_imptab = ne_imported_names;
            header.ne_exetyp = target;
            return header;
        }

        std::vector<std::byte> ne_image(const PEOs2Header_t& header, const uint32_t new_header = new_header_offset)
        {
            auto image = dos_image(new_header);
            write_at(image, new_header, header);
            return image;
        }

        std::vector<std::byte> windows_ne_image_importing_doscalls()
        {
            auto header = os2_header(ne_target_windows);
            header.ne_cmod = 1;
            auto image = ne_image(header);
            const auto name = new_header_offset + ne_imported_names + ne_first_imported_name;
            write_at(image, new_header_offset + ne_module_table, ne_first_imported_name);
            write_at(image, name, static_cast<uint8_t>(os2_api_module.size()));
            write_at(image, name + 1, os2_api_module);
            return image;
        }

        std::vector<std::byte> bound_os2_ne_image()
        {
            auto image = ne_image(os2_header(ne_target_os2), late_new_header_offset);
            write_at(image, os2_bind_stub_offset, os2_bind_stub);
            return image;
        }

        class ChildProcessPolicyTest : public testing::Test
        {
          protected:
            void TearDown() override
            {
                if (!this->images.empty())
                {
                    std::error_code error{};
                    std::filesystem::remove_all(this->images, error);
                }
            }

            std::u16string write_image(const std::u16string_view name, const std::vector<std::byte>& data)
            {
                if (this->images.empty())
                {
                    this->images =
                        std::filesystem::temp_directory_path() / ("sogen-child-image-test-" + std::to_string(std::random_device{}()));
                    std::filesystem::create_directory(this->images);
                    this->emu->file_sys.map(windows_path{'x', {}}, this->images);
                }

                std::ofstream{this->images / name, std::ios::binary}.write(reinterpret_cast<const char*>(data.data()),
                                                                           static_cast<std::streamsize>(data.size()));
                return u"X:\\" + std::u16string{name};
            }

            void start(const bool allow_child_processes)
            {
                emulator_interfaces interfaces{};
                interfaces.processes = &this->manager;
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true,
                                                                                 .emulation_root = get_emulator_root(),
                                                                                 .load_registry = false,
                                                                                 .allow_child_processes = allow_child_processes},
                                                               emulator_callbacks{}, std::move(interfaces));
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                this->storage = this->emu->memory.allocate_memory(0x20000, memory_permission::read_write);
            }

            syscall_context context() const
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS create_process(const std::u16string_view image)
            {
                emulator_allocator allocator{this->emu->memory, this->storage, 0x10000};
                const auto name = allocator.make_unicode_string(image);
                const auto parameters = allocator.reserve<RTL_USER_PROCESS_PARAMETERS64>();
                RTL_USER_PROCESS_PARAMETERS64 values{};
                values.ImagePathName = name.read();
                values.CommandLine = name.read();
                parameters.write(values);
                const auto creation = allocator.reserve<PS_CREATE_INFO<EmulatorTraits<Emu64>>>();
                PS_CREATE_INFO<EmulatorTraits<Emu64>> information{};
                information.Size = sizeof(information);
                creation.write(information);
                const auto process = allocator.reserve<handle>();
                const auto thread = allocator.reserve<handle>();
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtCreateUserProcess(this->context(), process, thread, 0, 0, {this->emu->memory},
                                                                {this->emu->memory}, 0, 0, parameters, creation, {this->emu->memory});
                });
            }

            PROCESS_MITIGATION_CHILD_PROCESS_POLICY query_mitigation_policy()
            {
                const emulator_object<PROCESS_MITIGATION_POLICY_RAW_DATA> policy{this->emu->memory, this->storage + 0x10000};
                policy.write({.Policy = ProcessChildProcessPolicy, .Value = 0});
                EXPECT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtQueryInformationProcess(this->context(), CURRENT_PROCESS, ProcessMitigationPolicy,
                                                                      policy.value(), sizeof(PROCESS_MITIGATION_POLICY_RAW_DATA),
                                                                      {this->emu->memory});
                }));
                EXPECT_EQ(policy.read().Policy, ProcessChildProcessPolicy);
                return {.Flags = policy.read().Value};
            }

            PROCESS_CHILD_PROCESS_INFORMATION query_child_process_information()
            {
                const emulator_object<PROCESS_CHILD_PROCESS_INFORMATION> information{this->emu->memory, this->storage + 0x11000};
                const emulator_object<uint32_t> length{this->emu->memory, this->storage + 0x12000};
                EXPECT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtQueryInformationProcess(this->context(), CURRENT_PROCESS, ProcessChildProcessInformation,
                                                                      information.value(), sizeof(PROCESS_CHILD_PROCESS_INFORMATION),
                                                                      length);
                }));
                EXPECT_EQ(length.read(), sizeof(PROCESS_CHILD_PROCESS_INFORMATION));
                return information.read();
            }

            counting_process_manager manager{};
            std::unique_ptr<windows_emulator> emu;
            uint64_t storage{};
            std::filesystem::path images{};
        };
    }

    TEST_F(ChildProcessPolicyTest, CreatingAProcessFailsWithChildProcessBlocked)
    {
        this->start(false);

        EXPECT_EQ(this->create_process(existing_image), STATUS_CHILD_PROCESS_BLOCKED);
        EXPECT_EQ(this->manager.creations, 0u);
    }

    TEST_F(ChildProcessPolicyTest, AMissingImageFailsOnItsOwnBeforeThePolicy)
    {
        this->start(false);

        EXPECT_EQ(this->create_process(u"C:\\Windows\\System32\\no-such-image.exe"), STATUS_OBJECT_NAME_NOT_FOUND);
        EXPECT_EQ(this->manager.creations, 0u);
    }

    TEST_F(ChildProcessPolicyTest, AFileThatIsNoImageFailsOnItsOwnBeforeThePolicy)
    {
        this->start(false);

        EXPECT_EQ(this->create_process(u"C:\\Windows\\System32\\locale.nls"), STATUS_INVALID_IMAGE_NOT_MZ);
        EXPECT_EQ(this->manager.creations, 0u);
    }

    TEST_F(ChildProcessPolicyTest, ImagesFailWithTheStatusOfTheirSectionBeforeThePolicy)
    {
        this->start(false);

        std::vector<std::byte> short_dos_image(sizeof(PEDosHeader_t) / 2);
        write_at(short_dos_image, 0, uint16_t{PEDosHeader_t::k_Magic});

        const std::vector<std::tuple<std::u16string_view, std::vector<std::byte>, NTSTATUS>> cases{
            {u"empty.exe", {}, STATUS_INVALID_FILE_FOR_SECTION},
            {u"short.exe", short_dos_image, STATUS_INVALID_IMAGE_PROTECT},
            {u"not-executable.exe", pe_image_without_executable_flag(), STATUS_INVALID_IMAGE_FORMAT},
            {u"windows.exe", ne_image(os2_header(ne_target_windows)), STATUS_INVALID_IMAGE_WIN_16},
            {u"doscalls.exe", windows_ne_image_importing_doscalls(), STATUS_INVALID_IMAGE_NE_FORMAT},
            {u"bound-os2.exe", bound_os2_ne_image(), STATUS_INVALID_IMAGE_PROTECT},
            {u"pe.exe", pe_image([](auto&, auto&) {}), STATUS_CHILD_PROCESS_BLOCKED},
            {u"no-file-alignment.exe", pe_image([](auto& nt_headers, auto&) { nt_headers.OptionalHeader.FileAlignment = 0; }),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"i386-pe32-plus.exe", pe_image([](auto& nt_headers, auto&) { nt_headers.FileHeader.Machine = PEMachineType::I386; }),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"section-gap.exe", pe_image([](auto&, auto& section) { section.VirtualAddress += pe_section_alignment; }),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"long-optional-header.exe",
             pe_image([](auto& nt_headers, auto&) { nt_headers.FileHeader.SizeOfOptionalHeader = largest_optional_header_size; }),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"unread-section-table.exe", pe_image([](auto& nt_headers, auto&) {
                 nt_headers.FileHeader.SizeOfOptionalHeader = largest_optional_header_size;
                 nt_headers.OptionalHeader.SizeOfImage = large_image_size;
             }),
             STATUS_INVALID_FILE_FOR_SECTION},
            {u"short-section-table.exe",
             resized(
                 pe_image([](auto& nt_headers, auto&) { nt_headers.FileHeader.SizeOfOptionalHeader = static_cast<uint16_t>(page_size); }),
                 pe_optional_header_offset + page_size + offsetof(IMAGE_SECTION_HEADER, Misc)),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"no-section-table.exe", pe_image([](auto& nt_headers, auto&) {
                 nt_headers.FileHeader.NumberOfSections = 0;
                 nt_headers.FileHeader.SizeOfOptionalHeader = static_cast<uint16_t>(2 * page_size - pe_optional_header_offset);
                 nt_headers.OptionalHeader.SizeOfImage = 3 * page_size;
             }),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"cut-section-header.exe",
             resized(pe_image([](auto&, auto&) {}), pe_section_header_offset + offsetof(IMAGE_SECTION_HEADER, Misc)),
             STATUS_INVALID_IMAGE_FORMAT},
            {u"cut-raw-data.exe",
             resized(pe_image([](auto&, auto&) {}), pe_section_header_offset + offsetof(IMAGE_SECTION_HEADER, SizeOfRawData)),
             STATUS_CHILD_PROCESS_BLOCKED},
            {u"amd64-sub-page.exe", pe_image([](auto& nt_headers, auto& section) {
                 nt_headers.OptionalHeader.SectionAlignment = pe_file_alignment;
                 section.VirtualAddress = pe_file_alignment;
             }),
             STATUS_CHILD_PROCESS_BLOCKED},
            {u"arm64-sub-page.exe", pe_image([](auto& nt_headers, auto& section) {
                 nt_headers.FileHeader.Machine = PEMachineType::ARM64;
                 nt_headers.OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
                 nt_headers.OptionalHeader.SectionAlignment = pe_file_alignment;
                 section.VirtualAddress = pe_file_alignment;
             }),
             STATUS_INVALID_IMAGE_FORMAT},
        };

        for (const auto& [name, data, status] : cases)
        {
            EXPECT_EQ(this->create_process(this->write_image(name, data)), status) << u16_to_u8(name);
        }

        EXPECT_EQ(this->manager.creations, 0u);
    }

    TEST_F(ChildProcessPolicyTest, AllowedChildProcessesAreCreated)
    {
        this->start(true);

        (void)this->create_process(existing_image);
        EXPECT_EQ(this->manager.creations, 1u);
    }

    TEST_F(ChildProcessPolicyTest, TheProcessReportsTheRestriction)
    {
        this->start(false);

        const auto policy = this->query_mitigation_policy();
        EXPECT_EQ(policy.NoChildProcessCreation, 1u);
        EXPECT_EQ(policy.AuditNoChildProcessCreation, 0u);
        EXPECT_EQ(policy.AllowSecureProcessCreation, 0u);

        const auto information = this->query_child_process_information();
        EXPECT_EQ(information.ProhibitChildProcesses, TRUE);
        EXPECT_EQ(information.AlwaysAllowSecureChildProcess, FALSE);
        EXPECT_EQ(information.AuditProhibitChildProcesses, FALSE);
    }

    TEST_F(ChildProcessPolicyTest, AnUnrestrictedProcessReportsNoPolicy)
    {
        this->start(true);

        EXPECT_EQ(this->query_mitigation_policy().Flags, 0u);

        const auto information = this->query_child_process_information();
        EXPECT_EQ(information.ProhibitChildProcesses, FALSE);
        EXPECT_EQ(information.AlwaysAllowSecureChildProcess, FALSE);
        EXPECT_EQ(information.AuditProhibitChildProcesses, FALSE);
    }
}

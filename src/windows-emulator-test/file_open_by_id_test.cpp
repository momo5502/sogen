#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                 emulator_object<LARGE_INTEGER> allocation_size, ULONG file_attributes, ULONG share_access,
                                 ULONG create_disposition, ULONG create_options, uint64_t ea_buffer, ULONG ea_length);
    NTSTATUS handle_NtQueryInformationFile(const syscall_context& c, handle file_handle,
                                           emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                           uint64_t file_information, uint32_t length, uint32_t info_class);
    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         ULONG length, FILE_INFORMATION_CLASS info_class);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        enum class hint_kind
        {
            windows_directory,
            volume_root,
            dos_volume,
            target_itself,
            file_handle,
            event_handle,
            invalid_value,
            none,
        };

        enum class target_kind
        {
            directory,
            file,
            volume_root,
        };

        enum class id_kind
        {
            real,
            zero,
            unknown,
            wrong_sequence,
            upper_half_set,
            null_attributes,
        };

        struct open_by_id_row
        {
            std::string_view name{};
            hint_kind hint{};
            target_kind target{};
            id_kind id{};
            ULONG id_length{};
            ULONG disposition{};
            ULONG extra_options{};
            NTSTATUS status{};
        };

        constexpr uint64_t sequence_bit = 0x0001000000000000ULL;
        constexpr uint64_t unknown_id = 0x0000123456789abcULL;

        const std::array<open_by_id_row, 27> rows{{
            {"dir id via the Windows directory hint, FILE_OPEN", hint_kind::windows_directory, target_kind::directory, id_kind::real, 8,
             FILE_OPEN, 0, STATUS_SUCCESS},
            {"dir id via the Windows directory hint, FILE_OPEN_IF", hint_kind::windows_directory, target_kind::directory, id_kind::real, 8,
             FILE_OPEN_IF, 0, STATUS_SUCCESS},
            {"dir id via the volume root hint, FILE_OPEN_IF", hint_kind::volume_root, target_kind::directory, id_kind::real, 8,
             FILE_OPEN_IF, 0, STATUS_SUCCESS},
            {"dir id via the \\??\\c: volume hint", hint_kind::dos_volume, target_kind::directory, id_kind::real, 8, FILE_OPEN, 0,
             STATUS_SUCCESS},
            {"dir id via the directory itself, FILE_OPEN_IF", hint_kind::target_itself, target_kind::directory, id_kind::real, 8,
             FILE_OPEN_IF, 0, STATUS_SUCCESS},
            {"file id via a file handle, FILE_OPEN", hint_kind::file_handle, target_kind::file, id_kind::real, 8, FILE_OPEN, 0,
             STATUS_SUCCESS},
            {"volume root id", hint_kind::windows_directory, target_kind::volume_root, id_kind::real, 8, FILE_OPEN, 0, STATUS_SUCCESS},
            {"dir id, 16-byte name", hint_kind::windows_directory, target_kind::directory, id_kind::real, 16, FILE_OPEN, 0, STATUS_SUCCESS},
            {"dir id, 16-byte name with the upper half set", hint_kind::windows_directory, target_kind::directory, id_kind::upper_half_set,
             16, FILE_OPEN, 0, STATUS_OBJECT_NAME_NOT_FOUND},
            {"dir id, 12-byte name", hint_kind::windows_directory, target_kind::directory, id_kind::real, 12, FILE_OPEN, 0,
             STATUS_INVALID_PARAMETER},
            {"dir id, 4-byte name", hint_kind::windows_directory, target_kind::directory, id_kind::real, 4, FILE_OPEN, 0,
             STATUS_INVALID_PARAMETER},
            {"dir id, 0-byte name", hint_kind::windows_directory, target_kind::directory, id_kind::real, 0, FILE_OPEN, 0,
             STATUS_OBJECT_NAME_INVALID},
            {"dir id, 9-byte name", hint_kind::windows_directory, target_kind::directory, id_kind::real, 9, FILE_OPEN, 0,
             STATUS_OBJECT_NAME_INVALID},
            {"dir id, 9-byte name, hint is an event", hint_kind::event_handle, target_kind::directory, id_kind::real, 9, FILE_OPEN, 0,
             STATUS_OBJECT_NAME_INVALID},
            {"dir id, 9-byte name, FILE_DELETE_ON_CLOSE without DELETE", hint_kind::windows_directory, target_kind::directory,
             id_kind::real, 9, FILE_OPEN, FILE_DELETE_ON_CLOSE, STATUS_INVALID_PARAMETER},
            {"dir id, 0xFFFE-byte name", hint_kind::windows_directory, target_kind::directory, id_kind::real, 0xFFFE, FILE_OPEN, 0,
             STATUS_OBJECT_NAME_INVALID},
            {"id 0", hint_kind::windows_directory, target_kind::directory, id_kind::zero, 8, FILE_OPEN, 0, STATUS_INVALID_PARAMETER},
            {"unknown id", hint_kind::windows_directory, target_kind::directory, id_kind::unknown, 8, FILE_OPEN, 0,
             STATUS_INVALID_PARAMETER},
            {"unknown id, FILE_CREATE", hint_kind::windows_directory, target_kind::directory, id_kind::unknown, 8, FILE_CREATE, 0,
             STATUS_INVALID_PARAMETER},
            {"dir id, wrong sequence number", hint_kind::windows_directory, target_kind::directory, id_kind::wrong_sequence, 8, FILE_OPEN,
             0, STATUS_INVALID_PARAMETER},
            {"dir id, no hint", hint_kind::none, target_kind::directory, id_kind::real, 8, FILE_OPEN, 0, STATUS_OBJECT_PATH_SYNTAX_BAD},
            {"dir id, hint is an event", hint_kind::event_handle, target_kind::directory, id_kind::real, 8, FILE_OPEN, 0,
             STATUS_OBJECT_TYPE_MISMATCH},
            {"dir id, hint is not a handle", hint_kind::invalid_value, target_kind::directory, id_kind::real, 8, FILE_OPEN, 0,
             STATUS_INVALID_HANDLE},
            {"dir id, FILE_CREATE", hint_kind::windows_directory, target_kind::directory, id_kind::real, 8, FILE_CREATE, 0,
             STATUS_OBJECT_NAME_COLLISION},
            {"dir id, FILE_NON_DIRECTORY_FILE", hint_kind::windows_directory, target_kind::directory, id_kind::real, 8, FILE_OPEN,
             FILE_NON_DIRECTORY_FILE, STATUS_FILE_IS_A_DIRECTORY},
            {"file id, FILE_DIRECTORY_FILE", hint_kind::windows_directory, target_kind::file, id_kind::real, 8, FILE_OPEN,
             FILE_DIRECTORY_FILE, STATUS_NOT_A_DIRECTORY},
            {"NULL ObjectAttributes, FILE_DELETE_ON_CLOSE without DELETE", hint_kind::none, target_kind::directory,
             id_kind::null_attributes, 8, FILE_OPEN, FILE_DELETE_ON_CLOSE, STATUS_INVALID_PARAMETER},
        }};

        class FileOpenByIdTest : public testing::Test
        {
          protected:
            std::filesystem::path directory =
                std::filesystem::temp_directory_path() /
                ("sogen-file-open-by-id-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
            windows_emulator emu = create_empty_emulator();
            uint64_t storage{};

            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directories(this->directory / "Windows" / "Globalization"));
                std::ofstream(this->directory / "Windows" / "win.ini") << "[fonts]";
                this->emu.file_sys.map(windows_path{'c', {}}, this->directory, filesystem_mode::read_write);
                this->storage = this->emu.memory.allocate_memory(0x4000, memory_permission::read_write);
            }

            void TearDown() override
            {
                std::error_code error{};
                std::filesystem::remove_all(this->directory, error);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            NTSTATUS create(const std::u16string_view name, const uint64_t root_directory, const ACCESS_MASK access,
                            const ULONG disposition, const ULONG options, handle& result)
            {
                const auto name_bytes = static_cast<USHORT>(name.size() * sizeof(char16_t));
                this->emu.memory.write_memory(this->storage + 0x400, name.data(), name_bytes);
                this->write_attributes(name_bytes, this->storage + 0x400, root_directory);

                return this->create_from_attributes(access, disposition, options, result);
            }

            void write_attributes(const USHORT name_bytes, const uint64_t name_buffer, const uint64_t root_directory)
            {
                this->emu.memory.write_memory(this->storage + 0x300, UNICODE_STRING<EmulatorTraits<Emu64>>{
                                                                         .Length = name_bytes,
                                                                         .MaximumLength = name_bytes,
                                                                         .Buffer = name_buffer,
                                                                     });
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attributes{};
                attributes.Length = sizeof(attributes);
                attributes.RootDirectory = root_directory;
                attributes.ObjectName = this->storage + 0x300;
                this->emu.memory.write_memory(this->storage + 0x200, attributes);
            }

            NTSTATUS create_from_attributes(const ACCESS_MASK access, const ULONG disposition, const ULONG options, handle& result)
            {
                return this->create_from_attributes_at(this->storage + 0x200, access, disposition, options, result);
            }

            NTSTATUS create_from_attributes_at(const uint64_t attributes, const ACCESS_MASK access, const ULONG disposition,
                                               const ULONG options, handle& result)
            {
                const emulator_object<handle> file_handle{this->emu.memory, this->storage};
                file_handle.write({});

                const auto status = syscalls::handle_NtCreateFile(this->context(), file_handle, access, {this->emu.memory, attributes},
                                                                  {this->emu.memory, this->storage + 0x10}, {this->emu.memory}, 0, 0x7,
                                                                  disposition, options | FILE_SYNCHRONOUS_IO_NONALERT, 0, 0);
                result = file_handle.read();
                return status;
            }

            handle open(const std::u16string_view path, const ULONG options)
            {
                handle result{};
                EXPECT_EQ(STATUS_SUCCESS, this->create(path, 0, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_OPEN, options, result));
                return result;
            }

            template <typename Info>
            Info query(const handle file, const uint32_t info_class)
            {
                const auto address = this->storage + 0x800;
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtQueryInformationFile(this->context(), file, {this->emu.memory, this->storage + 0x20}, address,
                                                                  sizeof(Info), info_class));
                return this->emu.memory.read_memory<Info>(address);
            }

            uint64_t file_id_of(const handle file)
            {
                const auto id = this->query<FILE_ID_INFORMATION>(file, FileIdInformation);
                const auto index = this->query<FILE_INTERNAL_INFORMATION>(file, FileInternalInformation);

                uint64_t file_id{};
                std::memcpy(&file_id, id.FileId.Identifier, sizeof(file_id));
                EXPECT_EQ(static_cast<uint64_t>(index.IndexNumber.QuadPart), file_id);
                return file_id;
            }

            static std::u16string name_with_id(std::u16string prefix, const uint64_t id_low, const uint64_t id_high, const size_t id_bytes)
            {
                const std::array<uint64_t, 2> id{id_low, id_high};
                const auto prefix_chars = prefix.size();
                prefix.resize(prefix_chars + id_bytes / sizeof(char16_t));
                std::memcpy(prefix.data() + prefix_chars, id.data(), id_bytes);
                return prefix;
            }

            std::u16string name_of(const handle file)
            {
                const auto address = this->storage + 0x800;
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtQueryInformationFile(this->context(), file, {this->emu.memory, this->storage + 0x20}, address,
                                                                  0x400, FileNameInformation));
                std::u16string name(this->emu.memory.read_memory<ULONG>(address) / sizeof(char16_t), u'\0');
                this->emu.memory.read_memory(address + sizeof(ULONG), name.data(), name.size() * sizeof(char16_t));
                return name;
            }
        };
    }

    TEST_F(FileOpenByIdTest, FollowsTheCapturedWindowsRows)
    {
        constexpr ULONG open_directory = FILE_DIRECTORY_FILE;
        constexpr ULONG open_file = FILE_NON_DIRECTORY_FILE;
        constexpr ACCESS_MASK by_id_access = FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

        const auto windows_directory = this->open(u"\\??\\c:\\Windows", open_directory);
        const auto volume_root = this->open(u"\\??\\c:\\", open_directory);
        const auto dos_volume = this->open(u"\\??\\c:", open_directory);
        const auto globalization = this->open(u"\\??\\c:\\Windows\\Globalization", open_directory);
        const auto win_ini = this->open(u"\\??\\c:\\Windows\\win.ini", open_file);
        const auto event_handle = this->emu.process.events.store(event{});

        const auto directory_id = this->file_id_of(globalization);
        const auto file_id = this->file_id_of(win_ini);
        const auto volume_root_id = this->file_id_of(volume_root);

        for (const auto& row : rows)
        {
            SCOPED_TRACE(std::string(row.name));

            const auto hint = [&]() -> handle {
                switch (row.hint)
                {
                case hint_kind::windows_directory:
                    return windows_directory;
                case hint_kind::volume_root:
                    return volume_root;
                case hint_kind::dos_volume:
                    return dos_volume;
                case hint_kind::target_itself:
                    return globalization;
                case hint_kind::file_handle:
                    return win_ini;
                case hint_kind::event_handle:
                    return event_handle;
                case hint_kind::invalid_value:
                    return handle{.bits = 0x1234};
                case hint_kind::none:
                    return {};
                }
                return {};
            }();

            const auto target_handle = [&]() -> handle {
                switch (row.target)
                {
                case target_kind::directory:
                    return globalization;
                case target_kind::file:
                    return win_ini;
                case target_kind::volume_root:
                    return volume_root;
                }
                return {};
            }();
            const auto target_id = row.target == target_kind::directory ? directory_id
                                   : row.target == target_kind::file    ? file_id
                                                                        : volume_root_id;

            const auto id_low = [&]() -> uint64_t {
                switch (row.id)
                {
                case id_kind::real:
                case id_kind::upper_half_set:
                    return target_id;
                case id_kind::zero:
                case id_kind::null_attributes:
                    return 0;
                case id_kind::unknown:
                    return unknown_id;
                case id_kind::wrong_sequence:
                    return target_id ^ sequence_bit;
                }
                return 0;
            }();

            std::array<uint64_t, 2> name_buffer{id_low, row.id == id_kind::upper_half_set ? 1u : 0u};
            this->emu.memory.write_memory(this->storage + 0x400, name_buffer.data(), sizeof(name_buffer));
            this->write_attributes(static_cast<USHORT>(row.id_length), this->storage + 0x400, hint.bits);

            handle opened{};
            const auto attributes_address = row.id == id_kind::null_attributes ? uint64_t{0} : this->storage + 0x200;
            const auto create = [&] {
                return this->create_from_attributes_at(attributes_address, by_id_access, row.disposition,
                                                       row.extra_options | FILE_OPEN_BY_FILE_ID, opened);
            };

            EXPECT_EQ(row.status, create());

            if (row.status == STATUS_SUCCESS)
            {
                EXPECT_EQ(target_id, this->file_id_of(opened));
                EXPECT_EQ(this->name_of(target_handle), this->name_of(opened));

                const auto* opened_file = this->emu.process.files.get(opened);
                ASSERT_NE(nullptr, opened_file);
                EXPECT_EQ(std::u16string::npos, opened_file->name.find(u"\\\\"));
            }
        }
    }

    TEST_F(FileOpenByIdTest, ReadsTheIdAfterTheLeadingBackslash)
    {
        constexpr ACCESS_MASK by_id_access = FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

        const auto windows_directory = this->open(u"\\??\\c:\\Windows", FILE_DIRECTORY_FILE);
        const auto volume_root = this->open(u"\\??\\c:\\", FILE_DIRECTORY_FILE);
        const auto win_ini = this->open(u"\\??\\c:\\Windows\\win.ini", FILE_NON_DIRECTORY_FILE);
        const auto win_ini_id = this->file_id_of(win_ini);
        const auto volume_root_id = this->file_id_of(volume_root);

        const auto expect_opens = [&](const std::u16string& name, const handle hint, const handle target, const uint64_t target_id) {
            SCOPED_TRACE(u16_to_u8(name));

            handle opened{};
            ASSERT_EQ(STATUS_SUCCESS, this->create(name, hint.bits, by_id_access, FILE_OPEN, FILE_OPEN_BY_FILE_ID, opened));
            EXPECT_EQ(target_id, this->file_id_of(opened));
            EXPECT_EQ(this->name_of(target), this->name_of(opened));

            const auto* opened_file = this->emu.process.files.get(opened);
            ASSERT_NE(nullptr, opened_file);
            EXPECT_EQ(std::u16string::npos, opened_file->name.find(u"\\\\"));
        };

        expect_opens(name_with_id(u"\\??\\C:\\", win_ini_id, 0, 8), {}, win_ini, win_ini_id);
        expect_opens(name_with_id(u"\\??\\c:\\", win_ini_id, 0, 16), {}, win_ini, win_ini_id);
        expect_opens(name_with_id(u"\\??\\C:\\", volume_root_id, 0, 8), {}, volume_root, volume_root_id);
        expect_opens(name_with_id(u"\\", win_ini_id, 0, 8), windows_directory, win_ini, win_ini_id);
        expect_opens(name_with_id(u"\\", win_ini_id, 0, 16), windows_directory, win_ini, win_ini_id);

        handle opened{};
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->create(name_with_id(u"\\", win_ini_id, 1, 16), windows_directory.bits, by_id_access,
                                                             FILE_OPEN, FILE_OPEN_BY_FILE_ID, opened));
    }

    TEST_F(FileOpenByIdTest, DirectoryHandleKeepsTheIdItOpened)
    {
        ASSERT_TRUE(std::filesystem::create_directory(this->directory / "A"));

        const auto first = this->open(u"\\??\\c:\\A", FILE_DIRECTORY_FILE);
        const auto second = this->open(u"\\??\\c:\\A", FILE_DIRECTORY_FILE);
        const auto opened_id = this->file_id_of(first);

        const std::u16string_view target = u"\\??\\c:\\B";
        const auto name_bytes = static_cast<ULONG>(target.size() * sizeof(char16_t));
        const auto info_address = this->storage + 0x1000;
        FILE_RENAME_INFORMATION info{};
        info.FileNameLength = name_bytes;
        this->emu.memory.write_memory(info_address, &info, sizeof(info));
        this->emu.memory.write_memory(info_address + offsetof(FILE_RENAME_INFORMATION, FileName), target.data(), name_bytes);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetInformationFile(this->context(), second, {this->emu.memory, this->storage + 0x20},
                                                                        info_address, sizeof(info) + name_bytes, FileRenameInformation));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), second));
        ASSERT_TRUE(std::filesystem::exists(this->directory / "B"));

        ASSERT_TRUE(std::filesystem::create_directory(this->directory / "A"));
        const auto recreated = this->open(u"\\??\\c:\\A", FILE_DIRECTORY_FILE);

        EXPECT_NE(opened_id, this->file_id_of(recreated));
        EXPECT_EQ(opened_id, this->file_id_of(first));

        const auto* opened_file = this->emu.process.files.get(first);
        ASSERT_NE(nullptr, opened_file);

        utils::buffer_serializer serializer{};
        serializer.write(opened_file->handle);

        utils::buffer_deserializer deserializer{serializer};
        deserializer.register_factory<file_system_wrapper>([&] { return file_system_wrapper{this->emu.file_sys}; });

        guest_file restored{};
        deserializer.read(restored);

        const auto restored_stat = restored.stat();
        ASSERT_TRUE(restored_stat.has_value());
        EXPECT_EQ(opened_id, restored_stat->st_ino);

        const auto volume_root = this->open(u"\\??\\c:\\", FILE_DIRECTORY_FILE);
        handle reopened{};
        ASSERT_EQ(STATUS_SUCCESS, this->create(name_with_id({}, restored_stat->st_ino, 0, 8), volume_root.bits,
                                               FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_OPEN, FILE_OPEN_BY_FILE_ID, reopened));
        EXPECT_EQ(this->name_of(this->open(u"\\??\\c:\\B", FILE_DIRECTORY_FILE)), this->name_of(reopened));
    }
}

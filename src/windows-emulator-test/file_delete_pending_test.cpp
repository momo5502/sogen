#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <fstream>
#include <random>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                 emulator_object<LARGE_INTEGER> allocation_size, ULONG file_attributes, ULONG share_access,
                                 ULONG create_disposition, ULONG create_options, uint64_t ea_buffer, ULONG ea_length);
    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         ULONG length, FILE_INFORMATION_CLASS info_class);
    NTSTATUS handle_NtQueryInformationFile(const syscall_context& c, handle file_handle,
                                           emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                           uint64_t file_information, uint32_t length, uint32_t info_class);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG share_all = 0x7;
        constexpr ACCESS_MASK delete_access = DELETE | SYNCHRONIZE;
        constexpr ACCESS_MASK attribute_access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;

        struct open_result
        {
            NTSTATUS status{};
            handle file{};
        };

        class FileDeletePendingTest : public testing::Test
        {
          protected:
            std::filesystem::path directory =
                std::filesystem::temp_directory_path() /
                ("sogen-file-delete-pending-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
            windows_emulator emu = create_empty_emulator();
            uint64_t storage{};

            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directory(this->directory));
                this->emu.file_sys.map(windows_path{'x', {}}, this->directory, filesystem_mode::read_write);
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
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

            void make_file(const std::string_view name)
            {
                std::ofstream(this->directory / name) << "data";
            }

            bool exists(const std::string_view name) const
            {
                return std::filesystem::exists(this->directory / name);
            }

            open_result open(const std::u16string_view name, const ACCESS_MASK access, const ULONG disposition = FILE_OPEN,
                             const ULONG options = FILE_NON_DIRECTORY_FILE)
            {
                const auto full_name = u"\\??\\x:\\" + std::u16string(name);
                const auto name_bytes = static_cast<USHORT>(full_name.size() * sizeof(char16_t));
                this->emu.memory.write_memory(this->storage + 0x400, full_name.data(), name_bytes);
                this->emu.memory.write_memory(this->storage + 0x300, UNICODE_STRING<EmulatorTraits<Emu64>>{
                                                                         .Length = name_bytes,
                                                                         .MaximumLength = name_bytes,
                                                                         .Buffer = this->storage + 0x400,
                                                                     });
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attributes{};
                attributes.Length = sizeof(attributes);
                attributes.ObjectName = this->storage + 0x300;
                this->emu.memory.write_memory(this->storage + 0x200, attributes);

                const emulator_object<handle> file_handle{this->emu.memory, this->storage};
                file_handle.write({});

                const auto status =
                    syscalls::handle_NtCreateFile(this->context(), file_handle, access, {this->emu.memory, this->storage + 0x200},
                                                  {this->emu.memory, this->storage + 0x10}, {this->emu.memory}, 0, share_all, disposition,
                                                  options | FILE_SYNCHRONOUS_IO_NONALERT, 0, 0);

                return {status, file_handle.read()};
            }

            handle open_expecting_success(const std::u16string_view name, const ACCESS_MASK access,
                                          const ULONG options = FILE_NON_DIRECTORY_FILE)
            {
                const auto result = this->open(name, access, FILE_OPEN, options);
                EXPECT_EQ(STATUS_SUCCESS, result.status);
                return result.file;
            }

            NTSTATUS set_information_at(const handle file, const FILE_INFORMATION_CLASS info_class, const uint64_t address,
                                        const ULONG length)
            {
                return syscalls::handle_NtSetInformationFile(this->context(), file, {this->emu.memory, this->storage}, address, length,
                                                             info_class);
            }

            NTSTATUS set_information(const handle file, const FILE_INFORMATION_CLASS info_class, const ULONG value, const ULONG length)
            {
                const auto info_address = this->storage + 0x800;
                this->emu.memory.write_memory(info_address, &value, sizeof(value));

                return this->set_information_at(file, info_class, info_address, length);
            }

            NTSTATUS set_delete(const handle file, const bool delete_file)
            {
                return this->set_information(file, FileDispositionInformation, delete_file ? 1 : 0, sizeof(FILE_DISPOSITION_INFORMATION));
            }

            NTSTATUS set_delete_ex(const handle file, const ULONG flags)
            {
                return this->set_information(file, FileDispositionInformationEx, flags, sizeof(FILE_DISPOSITION_INFORMATION_EX));
            }

            bool delete_pending(const handle file)
            {
                const auto info_address = this->storage + 0x900;
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtQueryInformationFile(this->context(), file, {this->emu.memory, this->storage}, info_address,
                                                                  sizeof(FILE_STANDARD_INFORMATION), FileStandardInformation));

                return this->emu.memory.read_memory<FILE_STANDARD_INFORMATION>(info_address).DeletePending != FALSE;
            }

            void close(const handle file)
            {
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), file));
            }
        };
    }

    TEST_F(FileDeletePendingTest, FilesAndDirectoriesFollowTheCapturedWindowsDeleteLifecycle)
    {
        this->make_file("plain.txt");
        const auto setter = this->open_expecting_success(u"plain.txt", delete_access);
        const auto other = this->open_expecting_success(u"plain.txt", attribute_access);

        EXPECT_FALSE(this->delete_pending(other));
        ASSERT_EQ(STATUS_SUCCESS, this->set_delete(setter, true));
        EXPECT_TRUE(this->delete_pending(setter));
        EXPECT_TRUE(this->delete_pending(other));
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"plain.txt", attribute_access).status);
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"plain.txt", delete_access, FILE_CREATE).status);

        this->close(setter);
        EXPECT_TRUE(this->exists("plain.txt"));
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"plain.txt", attribute_access).status);

        this->close(other);
        EXPECT_FALSE(this->exists("plain.txt"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->open(u"plain.txt", attribute_access).status);

        this->make_file("posix.txt");
        const auto posix_setter = this->open_expecting_success(u"posix.txt", delete_access);
        const auto posix_other = this->open_expecting_success(u"posix.txt", attribute_access);

        ASSERT_EQ(STATUS_SUCCESS, this->set_delete_ex(posix_setter, FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS));
        EXPECT_TRUE(this->delete_pending(posix_other));
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"posix.txt", attribute_access).status);

        this->close(posix_setter);
        EXPECT_FALSE(this->exists("posix.txt"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->open(u"posix.txt", attribute_access).status);

        this->close(posix_other);

        this->make_file("cancel.txt");
        const auto cancel_setter = this->open_expecting_success(u"cancel.txt", delete_access);

        ASSERT_EQ(STATUS_SUCCESS, this->set_delete_ex(cancel_setter, FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS));
        ASSERT_EQ(STATUS_SUCCESS, this->set_delete(cancel_setter, false));
        EXPECT_FALSE(this->delete_pending(cancel_setter));

        ASSERT_EQ(STATUS_SUCCESS, this->set_delete(cancel_setter, true));
        ASSERT_EQ(STATUS_SUCCESS, this->set_delete_ex(cancel_setter, FILE_DISPOSITION_DO_NOT_DELETE));
        EXPECT_FALSE(this->delete_pending(cancel_setter));

        const auto reopened = this->open(u"cancel.txt", attribute_access);
        EXPECT_EQ(STATUS_SUCCESS, reopened.status);

        this->close(reopened.file);
        this->close(cancel_setter);
        EXPECT_TRUE(this->exists("cancel.txt"));

        ASSERT_TRUE(std::filesystem::create_directory(this->directory / "full"));
        make_file("full/child.txt");
        ASSERT_TRUE(std::filesystem::create_directory(this->directory / "empty"));

        const auto full = this->open_expecting_success(u"full", delete_access, FILE_DIRECTORY_FILE);
        EXPECT_EQ(STATUS_DIRECTORY_NOT_EMPTY, this->set_delete(full, true));
        EXPECT_EQ(STATUS_DIRECTORY_NOT_EMPTY, this->set_delete_ex(full, FILE_DISPOSITION_DELETE));
        EXPECT_FALSE(this->delete_pending(full));
        this->close(full);

        const auto empty = this->open_expecting_success(u"empty", delete_access, FILE_DIRECTORY_FILE);
        ASSERT_EQ(STATUS_SUCCESS, this->set_delete(empty, true));
        EXPECT_TRUE(this->delete_pending(empty));
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"empty", attribute_access, FILE_OPEN, FILE_DIRECTORY_FILE).status);
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"empty\\child.txt", delete_access, FILE_CREATE).status);
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"empty\\child", delete_access, FILE_CREATE, FILE_DIRECTORY_FILE).status);
        EXPECT_EQ(STATUS_DELETE_PENDING, this->open(u"empty\\missing.txt", attribute_access).status);

        ASSERT_EQ(STATUS_SUCCESS, this->set_delete(empty, false));
        const auto child = this->open(u"empty\\child.txt", delete_access, FILE_CREATE);
        EXPECT_EQ(STATUS_SUCCESS, child.status);
        this->close(child.file);
        this->close(empty);
        EXPECT_TRUE(this->exists("empty/child.txt"));

        this->make_file("buffer.txt");
        const auto target = this->open_expecting_success(u"buffer.txt", delete_access);

        const auto pages = this->emu.memory.allocate_memory(0x3000, memory_permission::read_write);
        const auto no_access = pages + 0x1000;
        const auto unmapped = pages + 0x2000;
        ASSERT_TRUE(this->emu.memory.protect_memory(no_access, 0x1000, memory_permission::none));
        ASSERT_TRUE(this->emu.memory.release_memory(unmapped, 0x1000));
        const auto last_readable_byte = no_access - 1;
        const uint8_t delete_requested = 1;
        this->emu.memory.write_memory(last_readable_byte, &delete_requested, sizeof(delete_requested));

        struct buffer_row
        {
            FILE_INFORMATION_CLASS info_class;
            uint64_t address;
            ULONG length;
            NTSTATUS status;
        };

        const buffer_row rows[] = {
            {FileDispositionInformation, no_access, 1, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformation, unmapped, 1, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformation, 0, 1, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformation, unmapped, 2, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformation, unmapped, 8, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformation, last_readable_byte, 4, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformation, unmapped, 0, STATUS_INFO_LENGTH_MISMATCH},
            {FileDispositionInformation, 0, 0, STATUS_INFO_LENGTH_MISMATCH},
            {FileDispositionInformationEx, no_access, 4, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformationEx, unmapped, 4, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformationEx, 0, 4, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformationEx, unmapped, 8, STATUS_ACCESS_VIOLATION},
            {FileDispositionInformationEx, unmapped, 0, STATUS_INFO_LENGTH_MISMATCH},
            {FileDispositionInformationEx, unmapped, 2, STATUS_INFO_LENGTH_MISMATCH},
            {FileDispositionInformationEx, no_access, 2, STATUS_INFO_LENGTH_MISMATCH},
        };

        for (const auto& row : rows)
        {
            EXPECT_EQ(row.status, this->set_information_at(target, row.info_class, row.address, row.length));
            EXPECT_FALSE(this->delete_pending(target));
        }

        EXPECT_EQ(STATUS_SUCCESS, this->set_information_at(target, FileDispositionInformation, last_readable_byte, 1));
        EXPECT_TRUE(this->delete_pending(target));
        ASSERT_EQ(STATUS_SUCCESS, this->set_delete(target, false));

        this->close(target);
        EXPECT_TRUE(this->exists("buffer.txt"));
    }
}

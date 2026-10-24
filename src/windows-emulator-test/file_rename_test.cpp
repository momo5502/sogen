#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <fstream>
#include <random>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         ULONG length, FILE_INFORMATION_CLASS info_class);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        class FileRenameTest : public testing::Test
        {
          protected:
            std::filesystem::path directory =
                std::filesystem::temp_directory_path() /
                ("sogen-file-rename-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
            windows_emulator emu = create_empty_emulator();
            uint64_t storage{};
            handle source_handle{};

            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directory(this->directory));
                std::ofstream(this->directory / "source.txt") << "source";
                std::ofstream(this->directory / "existing.txt") << "existing";

                this->emu.file_sys.map(windows_path{'x', {}}, this->directory, filesystem_mode::read_write);
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);

                const windows_path source{'x', {u"source.txt"}};
                file f{};
                f.name = source.u16string();
                f.access_mask = DELETE;
                f.handle = guest_file{this->emu.file_sys, source, u"rb", this->emu.file_sys.open(source, u"rb").file};
                ASSERT_TRUE(f.handle);
                this->source_handle = this->emu.process.files.store(std::move(f));
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

            NTSTATUS rename(const std::u16string_view target, const bool replace_if_exists)
            {
                const auto info_address = this->storage + 0x100;
                const auto name_length = static_cast<ULONG>(target.size() * sizeof(char16_t));

                FILE_RENAME_INFORMATION info{};
                info.ReplaceIfExists = static_cast<BOOLEAN>(replace_if_exists);
                info.FileNameLength = name_length;
                this->emu.memory.write_memory(info_address, &info, sizeof(info));
                this->emu.memory.write_memory(info_address + offsetof(FILE_RENAME_INFORMATION, FileName), target.data(), name_length);

                return syscalls::handle_NtSetInformationFile(this->context(), this->source_handle, {this->emu.memory, this->storage},
                                                             info_address, sizeof(info) + name_length, FileRenameInformation);
            }

            std::string read_host_file(const std::string_view name) const
            {
                std::ifstream stream(this->directory / name);
                return {std::istreambuf_iterator<char>(stream), {}};
            }

            void close_source()
            {
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), this->source_handle));
            }
        };
    }

    TEST_F(FileRenameTest, RenameOntoItselfSucceeds)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->rename(u"x:\\source.txt", false));
        EXPECT_EQ(STATUS_SUCCESS, this->rename(u"source.txt", false));
        EXPECT_EQ(STATUS_SUCCESS, this->rename(u"x:\\SOURCE.TXT", false));
        this->close_source();

        EXPECT_EQ("source", this->read_host_file("source.txt"));
    }

    TEST_F(FileRenameTest, NoReplaceOntoExistingFileIsRefused)
    {
        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, this->rename(u"x:\\existing.txt", false));
        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, this->rename(u"existing.txt", false));
        this->close_source();

        EXPECT_EQ("source", this->read_host_file("source.txt"));
        EXPECT_EQ("existing", this->read_host_file("existing.txt"));
    }

    TEST_F(FileRenameTest, MissingParentIsRefused)
    {
        EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, this->rename(u"x:\\missing\\target.txt", false));
        EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, this->rename(u"x:\\existing.txt\\target.txt", true));
        this->close_source();

        EXPECT_EQ("source", this->read_host_file("source.txt"));
    }

    TEST_F(FileRenameTest, ReplaceOntoExistingFileRenamesAtClose)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->rename(u"x:\\existing.txt", true));
        this->close_source();

        EXPECT_FALSE(std::filesystem::exists(this->directory / "source.txt"));
        EXPECT_EQ("source", this->read_host_file("existing.txt"));
    }

    TEST_F(FileRenameTest, RenameToFreshNameRenamesAtClose)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->rename(u"fresh.txt", false));
        this->close_source();

        EXPECT_FALSE(std::filesystem::exists(this->directory / "source.txt"));
        EXPECT_EQ("source", this->read_host_file("fresh.txt"));
    }
}

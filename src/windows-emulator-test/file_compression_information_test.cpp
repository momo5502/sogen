#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <fstream>
#include <random>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationFile(const syscall_context& c, handle file_handle,
                                           emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                           uint64_t file_information, uint32_t length, uint32_t info_class);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::byte untouched{0xAA};
        constexpr uint32_t information_size = sizeof(FILE_COMPRESSION_INFORMATION);

        class FileCompressionInformationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directory(this->directory));
                std::ofstream{this->directory / "data.bin", std::ios::binary} << std::string(5000, 'x');
                std::ofstream{this->directory / "empty.bin", std::ios::binary}.flush();

                this->emu.file_sys.map(windows_path{'x', {}}, this->directory);
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            void TearDown() override
            {
                std::error_code error{};
                std::filesystem::remove_all(this->directory, error);
            }

            handle open_file(const char16_t* name)
            {
                const auto path = windows_path{'x', {name}};
                file f{};
                f.handle = guest_file{this->emu.file_sys, path, u"rb", this->emu.file_sys.open(path, u"rb").file};
                EXPECT_TRUE(f.handle);
                return this->emu.process.files.store(std::move(f));
            }

            handle open_directory()
            {
                file f{};
                f.handle = guest_file{this->emu.file_sys, windows_path{'x', {}}};
                return this->emu.process.files.store(std::move(f));
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status_block()
            {
                return {this->emu.memory, this->storage};
            }

            uint64_t buffer() const
            {
                return this->storage + 0x100;
            }

            NTSTATUS query(const handle file_handle, const uint32_t length)
            {
                const std::vector filler(0x200, untouched);
                this->emu.memory.write_memory(this->storage, filler.data(), filler.size());
                const syscall_context context{
                    .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
                return syscalls::handle_NtQueryInformationFile(context, file_handle, this->status_block(), this->buffer(), length,
                                                               FileCompressionInformation);
            }

            bool is_untouched(const uint64_t address, const size_t size) const
            {
                const auto bytes = this->emu.memory.read_memory(address, size);
                return std::ranges::all_of(bytes, [](const std::byte b) { return b == untouched; });
            }

            void expect_size(const handle file_handle, const int64_t expected_size)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->query(file_handle, information_size));
                const auto block = this->status_block().read();
                EXPECT_EQ(STATUS_SUCCESS, block.Status);
                EXPECT_EQ(information_size, block.Information);

                const auto information = this->emu.memory.read_memory<FILE_COMPRESSION_INFORMATION>(this->buffer());
                EXPECT_EQ(expected_size, information.CompressedFileSize.QuadPart);
                EXPECT_EQ(0, information.CompressionFormat);
                EXPECT_EQ(0, information.CompressionUnitShift);
                EXPECT_EQ(0, information.ChunkShift);
                EXPECT_EQ(0, information.ClusterShift);
                EXPECT_EQ(0, information.Reserved[0]);
                EXPECT_EQ(0, information.Reserved[1]);
                EXPECT_EQ(0, information.Reserved[2]);
            }

            std::filesystem::path directory =
                std::filesystem::temp_directory_path() / ("sogen-file-compression-test-" + std::to_string(std::random_device{}()));
            windows_emulator emu = create_empty_emulator();
            uint64_t storage{};
        };
    }

    TEST_F(FileCompressionInformationTest, ReportsEndOfFileAsCompressedSize)
    {
        this->expect_size(this->open_file(u"data.bin"), 5000);
    }

    TEST_F(FileCompressionInformationTest, ReportsZeroForEmptyFile)
    {
        this->expect_size(this->open_file(u"empty.bin"), 0);
    }

    TEST_F(FileCompressionInformationTest, ReportsZeroForDirectory)
    {
        this->expect_size(this->open_directory(), 0);
    }

    TEST_F(FileCompressionInformationTest, WritesOnlySixteenBytesForLargerBuffers)
    {
        const auto file_handle = this->open_file(u"data.bin");
        for (const uint32_t length : {17u, 32u})
        {
            SCOPED_TRACE(length);
            ASSERT_EQ(STATUS_SUCCESS, this->query(file_handle, length));
            EXPECT_EQ(information_size, this->status_block().read().Information);
            EXPECT_EQ(5000, this->emu.memory.read_memory<LONGLONG>(this->buffer()));
            EXPECT_TRUE(this->is_untouched(this->buffer() + information_size, length - information_size));
        }
    }

    TEST_F(FileCompressionInformationTest, ShortBufferLeavesStatusBlockAndBufferUntouched)
    {
        const auto file_handle = this->open_file(u"data.bin");
        for (const uint32_t length : {0u, 1u, 8u, 15u})
        {
            SCOPED_TRACE(length);
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(file_handle, length));
            EXPECT_TRUE(this->is_untouched(this->storage, sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>)));
            EXPECT_TRUE(this->is_untouched(this->buffer(), information_size));
        }
    }

    TEST_F(FileCompressionInformationTest, ChecksLengthBeforeHandle)
    {
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query({}, 8));
        EXPECT_TRUE(this->is_untouched(this->storage, sizeof(IO_STATUS_BLOCK<EmulatorTraits<Emu64>>)));
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->query({}, information_size));
    }

    TEST_F(FileCompressionInformationTest, RejectsHandlesOfOtherObjectTypes)
    {
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->query(this->emu.process.events.store(event{}), information_size));
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->query(CURRENT_PROCESS, information_size));
    }
}

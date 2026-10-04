#include <gtest/gtest.h>

#include <guest/guest_fd_table.hpp>
#include <macos_platform.hpp>

#include <filesystem>
#include <fstream>

namespace
{
    TEST(GuestFdTable, PrepopulatesTheStandardStreams)
    {
        const sogen::guest_fd_table fds{};

        ASSERT_EQ(fds.get_fds().size(), 3u);
        EXPECT_EQ(fds.get_fds().at(0).host_path, "/dev/stdin");
        EXPECT_EQ(fds.get_fds().at(1).host_path, "/dev/stdout");
        EXPECT_EQ(fds.get_fds().at(2).host_path, "/dev/stderr");
    }

    TEST(GuestFdTable, AllocatesTheLowestFreeDescriptor)
    {
        sogen::guest_fd_table fds{};

        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::memory_file;
        entry.memory_file = std::make_shared<sogen::guest_memory_fd>();
        entry.memory_file->content = "payload";

        EXPECT_EQ(fds.allocate(std::move(entry)), 3);

        sogen::guest_fd second{};
        second.type = sogen::fd_type::memory_file;
        second.memory_file = std::make_shared<sogen::guest_memory_fd>();
        EXPECT_EQ(fds.allocate(std::move(second)), 4);

        ASSERT_TRUE(fds.close(3));
        sogen::guest_fd third{};
        third.type = sogen::fd_type::memory_file;
        third.memory_file = std::make_shared<sogen::guest_memory_fd>();
        EXPECT_EQ(fds.allocate(std::move(third)), 3);
    }

    TEST(GuestFdTable, DuplicationSharesTheBackingStoreAndClearsCloseOnExec)
    {
        sogen::guest_fd_table fds{};

        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::memory_file;
        entry.close_on_exec = true;
        entry.memory_file = std::make_shared<sogen::guest_memory_fd>();
        entry.memory_file->content = "payload";

        const auto original = fds.allocate(std::move(entry));
        const auto duplicate = fds.dup_fd(original);

        ASSERT_NE(duplicate, -1);
        ASSERT_NE(fds.get(duplicate), nullptr);
        EXPECT_FALSE(fds.get(duplicate)->close_on_exec);
        EXPECT_EQ(fds.get(duplicate)->memory_file, fds.get(original)->memory_file);
    }

    TEST(GuestFdTable, ClosingAnUnknownDescriptorReportsFailure)
    {
        sogen::guest_fd_table fds{};
        EXPECT_FALSE(fds.close(99));
        EXPECT_EQ(fds.get(99), nullptr);
    }

    // Darwin spends 0x0008 on O_APPEND and 0x0400 on O_TRUNC; Linux spends 02000 (0x400) on O_APPEND.
    // Decoding a Darwin descriptor with the Linux mask therefore reopens an appending file truncating.
    TEST(GuestFdTable, RestoresADarwinAppendingDescriptorInAppendMode)
    {
        const auto path = std::filesystem::temp_directory_path() / "sogen-append-roundtrip.txt";
        {
            std::ofstream seed{path, std::ios::binary | std::ios::trunc};
            seed << "seed";
        }

        sogen::guest_fd_table fds{sogen::macos_open::MACOS_O_APPEND};

        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::file;
        entry.host_path = path.string();
        entry.flags = sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_APPEND;
        entry.handle = std::fopen(path.string().c_str(), "a+b");
        ASSERT_NE(entry.handle, nullptr);
        fds.allocate(std::move(entry));

        // The snapshot carries the file offset, so leaving it at end-of-file would let a non-appending
        // reopen produce the same bytes and the test would pass either way. Rewinding first is what
        // makes append and overwrite observably different.
        std::fseek(fds.get(3)->handle, 0, SEEK_SET);

        sogen::utils::buffer_serializer serializer{};
        fds.serialize(serializer);

        sogen::guest_fd_table restored{sogen::macos_open::MACOS_O_APPEND};
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        auto* reopened = restored.get(3);
        ASSERT_NE(reopened, nullptr);
        ASSERT_NE(reopened->handle, nullptr);

        std::fputs("-tail", reopened->handle);
        std::fflush(reopened->handle);

        std::ifstream check{path, std::ios::binary};
        std::string contents{std::istreambuf_iterator<char>{check}, std::istreambuf_iterator<char>{}};
        std::filesystem::remove(path);

        EXPECT_EQ(contents, "seed-tail") << "the descriptor was reopened truncating, not appending";
    }
}

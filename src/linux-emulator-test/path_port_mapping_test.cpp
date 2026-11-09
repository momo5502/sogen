#include <gtest/gtest.h>

#include <linux_emulator.hpp>
#include <backend_selection.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <array>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(__MINGW64__)
#include <process.h>
#else
#include <unistd.h>
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace sogen
{
    namespace
    {
        class temp_tree
        {
          public:
            temp_tree()
            {
                const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                this->path_ = std::filesystem::temp_directory_path() / ("sogen-linux-fs-test-" + std::to_string(getpid()) + "-" +
                                                                        std::to_string(thread_id) + "-" + std::to_string(timestamp));
                std::filesystem::create_directories(this->path_);
            }

            ~temp_tree()
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->path_, ec);
            }

            const std::filesystem::path& path() const
            {
                return this->path_;
            }

          private:
            std::filesystem::path path_{};
        };

        constexpr uint64_t guest_memory_base = 0x100000;
        constexpr size_t guest_memory_size = 0x1000;
        constexpr int linux_o_RDONLY = 0;
        constexpr int linux_o_DIRECTORY = 0200000;
        constexpr int64_t linux_ebadf = 9;
        constexpr int linux_af_unix = 1;
        constexpr int linux_af_inet = 2;
        constexpr int linux_sock_stream = 1;
        constexpr int64_t linux_eopnotsupp = 95;
        constexpr int64_t linux_enetunreach = 101;

        void write_guest_sockaddr_in(linux_emulator& emu, const uint64_t address, const uint16_t port, const uint32_t ipv4_address)
        {
            std::array<uint8_t, 16> sockaddr{};
            sockaddr.at(0) = static_cast<uint8_t>(linux_af_inet & 0xFF);
            sockaddr.at(1) = static_cast<uint8_t>((linux_af_inet >> 8) & 0xFF);
            sockaddr.at(2) = static_cast<uint8_t>((port >> 8U) & 0xFFU);
            sockaddr.at(3) = static_cast<uint8_t>(port & 0xFFU);
            sockaddr.at(4) = static_cast<uint8_t>((ipv4_address >> 24U) & 0xFFU);
            sockaddr.at(5) = static_cast<uint8_t>((ipv4_address >> 16U) & 0xFFU);
            sockaddr.at(6) = static_cast<uint8_t>((ipv4_address >> 8U) & 0xFFU);
            sockaddr.at(7) = static_cast<uint8_t>(ipv4_address & 0xFFU);
            emu.memory.write_memory(address, sockaddr.data(), sockaddr.size());
        }

        void write_guest_string(linux_emulator& emu, const uint64_t address, const std::string_view value)
        {
            emu.memory.write_memory(address, value.data(), value.size());
            constexpr char nul = '\0';
            emu.memory.write_memory(address + value.size(), &nul, sizeof(nul));
        }

        int64_t invoke_syscall(linux_emulator& emu, const uint64_t number, const uint64_t arg0 = 0, const uint64_t arg1 = 0,
                               const uint64_t arg2 = 0, const uint64_t arg3 = 0)
        {
            emu.emu().reg(x86_register::rdi, arg0);
            emu.emu().reg(x86_register::rsi, arg1);
            emu.emu().reg(x86_register::rdx, arg2);
            emu.emu().reg(x86_register::r10, arg3);

            const auto* entry = emu.dispatcher.get_entry(number);
            if (!entry || !entry->handler)
            {
                ADD_FAILURE() << "missing syscall handler " << number;
                return -1;
            }

            entry->handler({.emu_ref = emu, .emu = emu.emu(), .proc = emu.process});
            return static_cast<int64_t>(emu.emu().reg(x86_register::rax));
        }

        std::string read_guest_file(linux_emulator& emu, const int64_t fd, const size_t count)
        {
            constexpr auto buffer_addr = guest_memory_base + 0x100;
            const auto bytes_read = invoke_syscall(emu, linux_syscalls::LINUX_SYS_read, static_cast<uint64_t>(fd), buffer_addr, count);
            if (bytes_read < 0)
            {
                ADD_FAILURE() << "read failed with " << bytes_read;
                return {};
            }

            std::string data(static_cast<size_t>(bytes_read), '\0');
            emu.memory.read_memory(buffer_addr, data.data(), data.size());
            return data;
        }

        std::optional<std::string> read_guest_directory_entry(linux_emulator& emu, const int64_t fd)
        {
            constexpr auto buffer_addr = guest_memory_base + 0x100;
            constexpr size_t single_entry_size = 24;
            constexpr size_t name_offset = 19;
            const auto bytes_read =
                invoke_syscall(emu, linux_syscalls::LINUX_SYS_getdents64, static_cast<uint64_t>(fd), buffer_addr, single_entry_size);
            if (bytes_read <= 0)
            {
                return std::nullopt;
            }

            std::array<char, single_entry_size - name_offset> name{};
            emu.memory.read_memory(buffer_addr + name_offset, name.data(), name.size());
            return std::string{name.data()};
        }

        std::string read_host_file(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }
    }

    TEST(LinuxFileSystemMappingTest, UsesLongestPrefixPathMapping)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        const auto broad_host = temp.path() / "broad";
        const auto narrow_host = temp.path() / "narrow";
        std::filesystem::create_directories(emulation_root);
        std::filesystem::create_directories(broad_host);
        std::filesystem::create_directories(narrow_host);

        posix_file_system fs{emulation_root};
        fs.add_path_mapping("/guest", broad_host, false);
        fs.add_path_mapping("/guest/sub", narrow_host, false);

        EXPECT_EQ(fs.translate("/guest/file.txt"), broad_host / "file.txt");
        EXPECT_EQ(fs.translate("/guest/sub/file.txt"), narrow_host / "file.txt");
    }

    TEST(LinuxFileSystemMappingTest, DetectsReadOnlyMappings)
    {
        temp_tree temp{};
        posix_file_system fs{temp.path() / "root"};
        fs.add_path_mapping("/ro", temp.path() / "readonly", true);
        fs.add_path_mapping("/rw", temp.path() / "writable", false);

        EXPECT_TRUE(fs.is_read_only_guest_path("/ro/file.txt"));
        EXPECT_TRUE(fs.is_read_only_guest_path("/ro/nested/../file.txt"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/rw/file.txt"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/other/file.txt"));
    }

    TEST(LinuxFileSystemMappingTest, ReadOnlyMappingAllowsReadBeforeRejectingWrite)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        const auto mapped_host = temp.path() / "readonly";
        const auto host_file = mapped_host / "file.txt";
        std::filesystem::create_directories(mapped_host);
        {
            std::ofstream file(host_file, std::ios::binary);
            file << "data";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        emu.file_sys.add_path_mapping("/ro", mapped_host, true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        constexpr auto path_addr = guest_memory_base;
        constexpr auto buffer_addr = guest_memory_base + 0x100;
        write_guest_string(emu, path_addr, "/ro/file.txt");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, path_addr, linux_o_RDONLY);
        ASSERT_GE(fd, 0);

        auto* fd_entry = emu.process.fds.get(static_cast<int>(fd));
        ASSERT_NE(fd_entry, nullptr);
        EXPECT_TRUE(fd_entry->read_only_mapping);
        EXPECT_EQ(fd_entry->guest_path, "/ro/file.txt");
        EXPECT_EQ(fd_entry->host_path, host_file.string());

        fd_entry->guest_path = "\\ro\\file.txt";
        const auto fd_link = "/proc/self/fd/" + std::to_string(fd);
        write_guest_string(emu, path_addr, fd_link);
        const std::string expected_fd_target = "/ro/file.txt";
        const auto fd_link_len = invoke_syscall(emu, linux_syscalls::LINUX_SYS_readlink, path_addr, buffer_addr, expected_fd_target.size());
        ASSERT_EQ(fd_link_len, static_cast<int64_t>(expected_fd_target.size()));

        std::string fd_target(expected_fd_target.size(), '\0');
        emu.memory.read_memory(buffer_addr, fd_target.data(), fd_target.size());
        EXPECT_EQ(fd_target, expected_fd_target);

        const auto bytes_read = invoke_syscall(emu, linux_syscalls::LINUX_SYS_read, static_cast<uint64_t>(fd), buffer_addr, 4);
        ASSERT_EQ(bytes_read, 4);

        std::array<char, 4> read_buffer{};
        emu.memory.read_memory(buffer_addr, read_buffer.data(), read_buffer.size());
        EXPECT_EQ(std::string(read_buffer.data(), read_buffer.size()), "data");

        write_guest_string(emu, buffer_addr, "XX");
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_write, static_cast<uint64_t>(fd), buffer_addr, 2), -linux_ebadf);
        EXPECT_EQ(read_host_file(host_file), "data");

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(fd)), 0);
    }

    TEST(LinuxFileSystemMappingTest, OpeningReadOnlyMappingForWriteFailsWithReadOnlyFileSystem)
    {
        constexpr int linux_o_WRONLY = 1;
        constexpr int64_t linux_erofs = 30;

        temp_tree temp{};
        const auto mapped_host = temp.path() / "readonly";
        std::filesystem::create_directories(mapped_host);
        {
            std::ofstream file(mapped_host / "file.txt", std::ios::binary);
            file << "data";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), temp.path() / "root");
        emu.log.disable_output(true);
        emu.file_sys.add_path_mapping("/ro", mapped_host, true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/ro/file.txt");

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_WRONLY), -linux_erofs);
        EXPECT_EQ(read_host_file(mapped_host / "file.txt"), "data");
    }

    TEST(LinuxFileDescriptorTest, ExclusiveCreateOfExistingFileFails)
    {
        constexpr int linux_o_WRONLY = 1;
        constexpr int linux_o_CREAT = 0100;
        constexpr int linux_o_EXCL = 0200;
        constexpr int64_t linux_eexist = 17;
        constexpr uint64_t file_mode = 0644;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "data";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_WRONLY | linux_o_CREAT | linux_o_EXCL,
                                 file_mode),
                  -linux_eexist);
        EXPECT_EQ(read_host_file(emulation_root / "file.txt"), "data");
    }

    TEST(LinuxFileSystemMappingTest, ProcfsRelativeMapsAndCwdStayGuestLocal)
    {
        temp_tree temp{};
        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), temp.path() / "root");
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        constexpr auto path_addr = guest_memory_base;
        constexpr auto buffer_addr = guest_memory_base + 0x100;
        emu.process.current_working_directory = "/proc/self";
        write_guest_string(emu, path_addr, "maps");

        const auto maps_fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, path_addr, linux_o_RDONLY);
        ASSERT_GE(maps_fd, 0);

        const auto* maps_entry = emu.process.fds.get(static_cast<int>(maps_fd));
        ASSERT_NE(maps_entry, nullptr);
        EXPECT_EQ(maps_entry->type, fd_type::memory_file);
        EXPECT_EQ(maps_entry->guest_path, "/proc/self/maps");
        EXPECT_EQ(maps_entry->host_path, "/proc/self/maps");
        ASSERT_NE(maps_entry->memory_file, nullptr);
        EXPECT_EQ(maps_entry->memory_file->content, procfs::generate_content(emu, "/proc/self/maps").value_or(std::string{}));
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(maps_fd)), 0);

        emu.process.current_working_directory = "\\guest\\work";
        write_guest_string(emu, path_addr, "/proc/self/cwd");

        const auto cwd_len =
            invoke_syscall(emu, linux_syscalls::LINUX_SYS_readlink, path_addr, buffer_addr, guest_memory_size - (buffer_addr - path_addr));
        ASSERT_EQ(cwd_len, 11);

        std::array<char, 11> cwd_buffer{};
        emu.memory.read_memory(buffer_addr, cwd_buffer.data(), cwd_buffer.size());
        EXPECT_EQ(std::string(cwd_buffer.data(), cwd_buffer.size()), "/guest/work");
    }

    TEST(LinuxFileSystemMappingTest, ResolvesRelativeGuestPathThroughWorkingDirectory)
    {
        temp_tree temp{};
        const auto host_root = temp.path() / "host";
        posix_file_system fs{temp.path() / "root"};
        fs.add_path_mapping("/mnt", host_root, false);

        EXPECT_EQ(fs.translate_guest_relative_to("/mnt/work", "data.txt"), host_root / "work" / "data.txt");
        EXPECT_EQ(fs.translate_guest_relative_to("/mnt/work", "../data.txt"), host_root / "data.txt");
        EXPECT_EQ(fs.translate_guest_relative_to("\\mnt\\work", "nested\\..\\data.txt"), host_root / "work" / "data.txt");
    }

    TEST(LinuxFileSystemMappingTest, NormalizesGuestPathStringsWithLinuxSeparators)
    {
        EXPECT_EQ(posix_file_system::normalize_guest_path_string("\\guest\\work\\..\\file.txt"), "/guest/file.txt");
        EXPECT_EQ(posix_file_system::resolve_guest_path_string("\\guest\\work", "nested\\..\\file.txt"), "/guest/work/file.txt");
        EXPECT_EQ(posix_file_system::resolve_guest_path_string("/guest/work", "\\proc\\self\\cwd"), "/proc/self/cwd");
    }

    TEST(LinuxFileSystemMappingTest, DotDotCannotEscapeMappedHostRoot)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        const auto mapped_host = temp.path() / "mapped";
        std::filesystem::create_directories(emulation_root);
        std::filesystem::create_directories(mapped_host);

        posix_file_system fs{emulation_root};
        fs.add_path_mapping("/mapped", mapped_host, false);

        EXPECT_EQ(fs.translate("/mapped/../../outside.txt"), emulation_root / "outside.txt");
        EXPECT_NE(fs.translate("/mapped/../../outside.txt"), mapped_host.parent_path() / "outside.txt");
    }

    TEST(LinuxFileDescriptorTest, DuplicateSharesFilePosition)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "ABCDEF";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ASSERT_GE(fd, 0);
        const auto duplicate = invoke_syscall(emu, linux_syscalls::LINUX_SYS_dup, static_cast<uint64_t>(fd));
        ASSERT_GE(duplicate, 0);

        EXPECT_EQ(read_guest_file(emu, fd, 1), "A");
        EXPECT_EQ(read_guest_file(emu, duplicate, 1), "B");
        EXPECT_EQ(read_guest_file(emu, fd, 2), "CD");

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(fd)), 0);
        EXPECT_EQ(read_guest_file(emu, duplicate, 8), "EF");
    }

    TEST(LinuxFileDescriptorTest, DuplicateSharesDirectoryPosition)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root / "dir");
        std::ofstream(emulation_root / "dir" / "a").close();
        std::ofstream(emulation_root / "dir" / "b").close();

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/dir");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY | linux_o_DIRECTORY);
        ASSERT_GE(fd, 0);
        const auto duplicate = invoke_syscall(emu, linux_syscalls::LINUX_SYS_dup, static_cast<uint64_t>(fd));
        ASSERT_GE(duplicate, 0);

        std::vector<std::string> names{};
        for (auto reader = fd; auto name = read_guest_directory_entry(emu, reader); reader = reader == fd ? duplicate : fd)
        {
            names.push_back(std::move(*name));
        }

        std::ranges::sort(names);
        EXPECT_EQ(names, (std::vector<std::string>{".", "..", "a", "b"}));
    }

    TEST(LinuxFileDescriptorTest, RestoredDuplicateSharesFilePosition)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "ABCD";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ASSERT_GE(fd, 0);
        const auto duplicate = invoke_syscall(emu, linux_syscalls::LINUX_SYS_dup, static_cast<uint64_t>(fd));
        ASSERT_GE(duplicate, 0);
        EXPECT_EQ(read_guest_file(emu, fd, 1), "A");

        utils::buffer_serializer serializer{};
        emu.process.fds.serialize(serializer);
        utils::buffer_deserializer deserializer{serializer.get_buffer()};
        emu.process.fds.deserialize(deserializer);

        EXPECT_EQ(read_guest_file(emu, duplicate, 1), "B");
        EXPECT_EQ(read_guest_file(emu, fd, 1), "C");
    }

    TEST(LinuxFileDescriptorTest, RestoredDuplicateKeepsEpollInterestList)
    {
        constexpr uint32_t epollin = 0x1;
        constexpr uint64_t epoll_ctl_add = 1;
        constexpr uint64_t registered_data = 0x1122334455667788;
        constexpr auto event_address = guest_memory_base + 0x200;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "data";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        const auto file = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ASSERT_GE(file, 0);
        const auto epoll = invoke_syscall(emu, linux_syscalls::LINUX_SYS_epoll_create1);
        ASSERT_GE(epoll, 0);

        emu.memory.write_memory(event_address, &epollin, sizeof(epollin));
        emu.memory.write_memory(event_address + sizeof(epollin), &registered_data, sizeof(registered_data));
        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_epoll_ctl, static_cast<uint64_t>(epoll), epoll_ctl_add,
                                 static_cast<uint64_t>(file), event_address),
                  0);

        const auto duplicate = invoke_syscall(emu, linux_syscalls::LINUX_SYS_dup, static_cast<uint64_t>(epoll));
        ASSERT_GE(duplicate, 0);
        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(epoll)), 0);

        utils::buffer_serializer serializer{};
        emu.process.fds.serialize(serializer);
        utils::buffer_deserializer deserializer{serializer.get_buffer()};
        emu.process.fds.deserialize(deserializer);

        constexpr auto ready_address = guest_memory_base + 0x300;
        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_epoll_wait, static_cast<uint64_t>(duplicate), ready_address, 1), 1);

        uint32_t ready_events{};
        uint64_t ready_data{};
        emu.memory.read_memory(ready_address, &ready_events, sizeof(ready_events));
        emu.memory.read_memory(ready_address + sizeof(ready_events), &ready_data, sizeof(ready_data));
        EXPECT_EQ(ready_events, epollin);
        EXPECT_EQ(ready_data, registered_data);
    }

    TEST(LinuxFileDescriptorTest, DescriptorReusedAfterRestoreIsNotWatched)
    {
        constexpr uint32_t epollin = 0x1;
        constexpr uint64_t epoll_ctl_add = 1;
        constexpr auto event_address = guest_memory_base + 0x200;
        constexpr auto ready_address = guest_memory_base + 0x300;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "data";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        const auto file = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ASSERT_GE(file, 0);
        const auto epoll = invoke_syscall(emu, linux_syscalls::LINUX_SYS_epoll_create1);
        ASSERT_GE(epoll, 0);
        emu.memory.write_memory(event_address, &epollin, sizeof(epollin));
        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_epoll_ctl, static_cast<uint64_t>(epoll), epoll_ctl_add,
                                 static_cast<uint64_t>(file), event_address),
                  0);

        utils::buffer_serializer serializer{};
        emu.process.fds.serialize(serializer);
        utils::buffer_deserializer deserializer{serializer.get_buffer()};
        emu.process.fds.deserialize(deserializer);

        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(file)), 0);
        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY), file);
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_epoll_wait, static_cast<uint64_t>(epoll), ready_address, 1), 0);
    }

    TEST(LinuxFileDescriptorTest, SeekingDirectoryToStartRestartsListing)
    {
        constexpr uint64_t seek_set = 0;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root / "dir");
        std::ofstream(emulation_root / "dir" / "a").close();

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/dir");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY | linux_o_DIRECTORY);
        ASSERT_GE(fd, 0);

        std::vector<std::string> first_pass{};
        while (auto name = read_guest_directory_entry(emu, fd))
        {
            first_pass.push_back(std::move(*name));
        }

        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_lseek, static_cast<uint64_t>(fd), 0, seek_set), 0);

        std::vector<std::string> second_pass{};
        while (auto name = read_guest_directory_entry(emu, fd))
        {
            second_pass.push_back(std::move(*name));
        }

        EXPECT_EQ(first_pass.size(), 3);
        EXPECT_EQ(second_pass, first_pass);
    }

    TEST(LinuxFileDescriptorTest, WriteToStandardOutputFollowsRedirectedDescriptor)
    {
        constexpr int linux_o_WRONLY = 1;
        constexpr int linux_o_CREAT = 0100;
        constexpr uint64_t file_mode = 0644;
        constexpr uint64_t standard_output = 1;
        constexpr std::string_view message = "redirected";

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        std::string captured_stdout{};
        emu.on_stdout = [&captured_stdout](const std::string_view data) { captured_stdout.append(data); };
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        constexpr auto message_address = guest_memory_base + 0x100;
        write_guest_string(emu, guest_memory_base, "/out.txt");
        write_guest_string(emu, message_address, message);

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_WRONLY | linux_o_CREAT, file_mode);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_dup2, static_cast<uint64_t>(fd), standard_output),
                  static_cast<int64_t>(standard_output));

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_write, standard_output, message_address, message.size()),
                  static_cast<int64_t>(message.size()));
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(fd)), 0);
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, standard_output), 0);

        EXPECT_TRUE(captured_stdout.empty());
        EXPECT_EQ(read_host_file(emulation_root / "out.txt"), message);
    }

    TEST(LinuxFileDescriptorTest, WriteFromInaccessibleBufferFailsWithBadAddress)
    {
        constexpr int64_t linux_efault = 14;
        constexpr int linux_o_WRONLY = 1;
        constexpr int linux_o_CREAT = 0100;
        constexpr uint64_t file_mode = 0644;
        constexpr auto guard_page = guest_memory_base + guest_memory_size;
        constexpr auto iovec_address = guest_memory_base + 0x100;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        ASSERT_TRUE(emu.memory.allocate_memory(guard_page, guest_memory_size, memory_permission::none));
        write_guest_string(emu, guest_memory_base, "/out.txt");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_WRONLY | linux_o_CREAT, file_mode);
        ASSERT_GE(fd, 0);

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_write, static_cast<uint64_t>(fd), guard_page, 4), -linux_efault);
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_writev, static_cast<uint64_t>(fd), guard_page, 1), -linux_efault);

        const std::array<uint64_t, 2> guarded_vector{guard_page, 4};
        emu.memory.write_memory(iovec_address, guarded_vector.data(), sizeof(guarded_vector));
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_writev, static_cast<uint64_t>(fd), iovec_address, 1), -linux_efault);

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(fd)), 0);
        EXPECT_TRUE(read_host_file(emulation_root / "out.txt").empty());
    }

    TEST(LinuxFileDescriptorTest, ReadAtEndOfFileIntoUnmappedBufferReturnsZero)
    {
        constexpr uint64_t unmapped_address = guest_memory_base + (guest_memory_size * 4);

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "AB";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ASSERT_GE(fd, 0);
        EXPECT_EQ(read_guest_file(emu, fd, 2), "AB");
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_read, static_cast<uint64_t>(fd), unmapped_address, 2), 0);
    }

    TEST(LinuxFileDescriptorTest, ReadIntoPartlyWritableBufferStopsAtItsEnd)
    {
        constexpr auto last_writable_bytes = guest_memory_base + guest_memory_size - 2;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "ABCD";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base + guest_memory_size, guest_memory_size, memory_permission::read));
        write_guest_string(emu, guest_memory_base, "/file.txt");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ASSERT_GE(fd, 0);

        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_read, static_cast<uint64_t>(fd), last_writable_bytes, 4), 2);
        std::array<char, 2> copied{};
        emu.memory.read_memory(last_writable_bytes, copied.data(), copied.size());
        EXPECT_EQ(std::string_view(copied.data(), copied.size()), "AB");
        EXPECT_EQ(read_guest_file(emu, fd, 4), "CD");
    }

    TEST(LinuxFileDescriptorTest, ReadFromFifoIntoPartlyWritableBufferKeepsTheRest)
    {
#if defined(_WIN32)
        GTEST_SKIP() << "Windows has no FIFOs that can be opened by path";
#else
        constexpr auto last_writable_bytes = guest_memory_base + guest_memory_size - 2;
        constexpr std::string_view queued = "ABCD";

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        const auto fifo_path = emulation_root / "fifo";
        ASSERT_EQ(::mkfifo(fifo_path.c_str(), 0600), 0);

        const auto writer = ::open(fifo_path.c_str(), O_RDWR);
        ASSERT_GE(writer, 0);
        ASSERT_EQ(::write(writer, queued.data(), queued.size()), static_cast<ssize_t>(queued.size()));

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base + guest_memory_size, guest_memory_size, memory_permission::read));
        write_guest_string(emu, guest_memory_base, "/fifo");

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY);
        ::close(writer);
        ASSERT_GE(fd, 0);

        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_read, static_cast<uint64_t>(fd), last_writable_bytes, queued.size()), 2);
        std::array<char, 2> copied{};
        emu.memory.read_memory(last_writable_bytes, copied.data(), copied.size());
        EXPECT_EQ(std::string_view(copied.data(), copied.size()), "AB");
        EXPECT_EQ(read_guest_file(emu, fd, queued.size()), "CD");
#endif
    }

    TEST(LinuxFileDescriptorTest, PipeIntoReadOnlyArrayFailsAndReleasesBothEnds)
    {
        constexpr int64_t linux_efault = 14;
        constexpr auto read_only_page = guest_memory_base + guest_memory_size;

        temp_tree temp{};
        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), temp.path() / "root");
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));
        ASSERT_TRUE(emu.memory.allocate_memory(read_only_page, guest_memory_size, memory_permission::read));

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_pipe, read_only_page), -linux_efault);
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_pipe2, read_only_page, 0), -linux_efault);

        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_pipe, guest_memory_base), 0);
        std::array<int32_t, 2> descriptors{};
        emu.memory.read_memory(guest_memory_base, descriptors.data(), sizeof(descriptors));
        EXPECT_EQ(descriptors, (std::array<int32_t, 2>{3, 4}));
    }

    TEST(LinuxFileDescriptorTest, OpeningPathBelowRegularFileFailsWithNotDirectory)
    {
        constexpr int64_t linux_enotdir = 20;
        constexpr int64_t linux_enoent = 2;

        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        std::filesystem::create_directories(emulation_root);
        {
            std::ofstream file(emulation_root / "file.txt", std::ios::binary);
            file << "A";
        }

        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), emulation_root);
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        write_guest_string(emu, guest_memory_base, "/file.txt/child");
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY), -linux_enotdir);
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_DIRECTORY), -linux_enotdir);

        write_guest_string(emu, guest_memory_base, "/missing/child");
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_open, guest_memory_base, linux_o_RDONLY), -linux_enoent);
    }

    TEST(LinuxFileDescriptorTest, SeekingPipeFailsWithIllegalSeek)
    {
        constexpr int64_t linux_espipe = 29;
        constexpr uint64_t seek_cur = 1;

        temp_tree temp{};
        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), temp.path() / "root");
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        ASSERT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_pipe, guest_memory_base), 0);
        std::array<int32_t, 2> descriptors{};
        emu.memory.read_memory(guest_memory_base, descriptors.data(), sizeof(descriptors));

        for (const auto descriptor : descriptors)
        {
            EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_lseek, static_cast<uint64_t>(descriptor), 0, seek_cur), -linux_espipe);
        }
    }

    TEST(LinuxSocketSyscallTest, SocketpairIsExplicitlyUnsupported)
    {
        temp_tree temp{};
        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), temp.path() / "root");
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        std::array<int32_t, 2> descriptors{-1, -1};
        emu.memory.write_memory(guest_memory_base, descriptors.data(), sizeof(int32_t) * descriptors.size());

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_socketpair, linux_af_unix, linux_sock_stream, 0, guest_memory_base),
                  -linux_eopnotsupp);

        descriptors = {};
        emu.memory.read_memory(guest_memory_base, descriptors.data(), sizeof(int32_t) * descriptors.size());
        EXPECT_EQ(descriptors.at(0), -1);
        EXPECT_EQ(descriptors.at(1), -1);
    }

    TEST(LinuxSocketSyscallTest, NonLoopbackConnectReturnsNetworkUnreachable)
    {
        temp_tree temp{};
        auto backend = create_x86_64_emulator();
        linux_emulator emu(std::move(backend), temp.path() / "root");
        emu.log.disable_output(true);
        ASSERT_TRUE(emu.memory.allocate_memory(guest_memory_base, guest_memory_size, memory_permission::read_write));

        const auto fd = invoke_syscall(emu, linux_syscalls::LINUX_SYS_socket, linux_af_inet, linux_sock_stream);
        ASSERT_GE(fd, 0);

        write_guest_sockaddr_in(emu, guest_memory_base, 40000, 0x08080808U);
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_connect, static_cast<uint64_t>(fd), guest_memory_base, 16),
                  -linux_enetunreach);
    }

    TEST(LinuxPortMapperTest, MapsPortsInBothDirectionsAndReplacesOldEntries)
    {
        linux_port_mapper mapper{};

        EXPECT_EQ(mapper.get_host_port(8080), 0);
        EXPECT_EQ(mapper.get_emulator_port(18080), 0);

        mapper.map_port(8080, 18080);
        EXPECT_EQ(mapper.get_host_port(8080), 18080);
        EXPECT_EQ(mapper.get_emulator_port(18080), 8080);

        mapper.map_port(8080, 28080);
        EXPECT_EQ(mapper.get_host_port(8080), 28080);
        EXPECT_EQ(mapper.get_emulator_port(18080), 0);
        EXPECT_EQ(mapper.get_emulator_port(28080), 8080);

        mapper.map_port(9090, 28080);
        EXPECT_EQ(mapper.get_host_port(8080), 0);
        EXPECT_EQ(mapper.get_host_port(9090), 28080);
        EXPECT_EQ(mapper.get_emulator_port(28080), 9090);
    }

    TEST(LinuxPortMapperTest, RejectsZeroPortMappings)
    {
        linux_port_mapper mapper{};

        EXPECT_THROW(mapper.map_port(0, 18080), std::runtime_error);
        EXPECT_THROW(mapper.map_port(8080, 0), std::runtime_error);
        EXPECT_EQ(mapper.get_host_port(8080), 0);
        EXPECT_EQ(mapper.get_emulator_port(18080), 0);
    }
} // namespace sogen

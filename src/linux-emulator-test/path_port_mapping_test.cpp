#include <gtest/gtest.h>

#include <linux_emulator.hpp>
#include <backend_selection.hpp>

#include <chrono>
#include <filesystem>
#include <array>
#include <fstream>
#include <iterator>
#include <string_view>

namespace sogen
{
    namespace
    {
        class temp_tree
        {
          public:
            temp_tree()
            {
                const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
                this->path_ = std::filesystem::temp_directory_path() / ("sogen-linux-fs-test-" + std::to_string(suffix));
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
        constexpr int64_t linux_eacces = 13;
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

            entry->handler({emu, emu.emu(), emu.process});
            return static_cast<int64_t>(emu.emu().reg(x86_register::rax));
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

        linux_file_system fs{emulation_root};
        fs.add_path_mapping("/guest", broad_host, false);
        fs.add_path_mapping("/guest/sub", narrow_host, false);

        EXPECT_EQ(fs.translate("/guest/file.txt"), broad_host / "file.txt");
        EXPECT_EQ(fs.translate("/guest/sub/file.txt"), narrow_host / "file.txt");
    }

    TEST(LinuxFileSystemMappingTest, DetectsReadOnlyMappings)
    {
        temp_tree temp{};
        linux_file_system fs{temp.path() / "root"};
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
        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_write, static_cast<uint64_t>(fd), buffer_addr, 2), -linux_eacces);
        EXPECT_EQ(read_host_file(host_file), "data");

        EXPECT_EQ(invoke_syscall(emu, linux_syscalls::LINUX_SYS_close, static_cast<uint64_t>(fd)), 0);
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
        linux_file_system fs{temp.path() / "root"};
        fs.add_path_mapping("/mnt", host_root, false);

        EXPECT_EQ(fs.translate_guest_relative_to("/mnt/work", "data.txt"), host_root / "work" / "data.txt");
        EXPECT_EQ(fs.translate_guest_relative_to("/mnt/work", "../data.txt"), host_root / "data.txt");
        EXPECT_EQ(fs.translate_guest_relative_to("\\mnt\\work", "nested\\..\\data.txt"), host_root / "work" / "data.txt");
    }

    TEST(LinuxFileSystemMappingTest, NormalizesGuestPathStringsWithLinuxSeparators)
    {
        EXPECT_EQ(linux_file_system::normalize_guest_path_string("\\guest\\work\\..\\file.txt"), "/guest/file.txt");
        EXPECT_EQ(linux_file_system::resolve_guest_path_string("\\guest\\work", "nested\\..\\file.txt"), "/guest/work/file.txt");
        EXPECT_EQ(linux_file_system::resolve_guest_path_string("/guest/work", "\\proc\\self\\cwd"), "/proc/self/cwd");
    }

    TEST(LinuxFileSystemMappingTest, DotDotCannotEscapeMappedHostRoot)
    {
        temp_tree temp{};
        const auto emulation_root = temp.path() / "root";
        const auto mapped_host = temp.path() / "mapped";
        std::filesystem::create_directories(emulation_root);
        std::filesystem::create_directories(mapped_host);

        linux_file_system fs{emulation_root};
        fs.add_path_mapping("/mapped", mapped_host, false);

        EXPECT_EQ(fs.translate("/mapped/../../outside.txt"), emulation_root / "outside.txt");
        EXPECT_NE(fs.translate("/mapped/../../outside.txt"), mapped_host.parent_path() / "outside.txt");
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

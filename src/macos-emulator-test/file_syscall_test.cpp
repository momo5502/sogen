#include <gtest/gtest.h>

#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

#include <macos_file_identity.hpp>
#include <macos_stat.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <ranges>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    constexpr uint32_t mov_x16_open = 0xD28000B0;
    constexpr uint32_t mov_x16_read = 0xD2800070;
    constexpr uint32_t mov_x16_write = 0xD2800090;
    constexpr uint32_t mov_x16_close = 0xD28000D0;
    constexpr uint32_t mov_x16_unlink = 0xD2800150;
    constexpr uint32_t mov_x16_mkdir = 0xD2801110;   // mov x16, #136
    constexpr uint32_t mov_x16_mkdirat = 0xD2803B70; // mov x16, #475
    constexpr uint32_t mov_x16_access = 0xD2800430;
    constexpr uint32_t mov_x16_readlink = 0xD2800750;
    constexpr uint32_t mov_x16_stat64 = 0xD2802A50;
    constexpr uint32_t mov_x16_fstat64 = 0xD2802A70;
    constexpr uint32_t mov_x16_lstat64 = 0xD2802A90;
    constexpr uint32_t mov_x16_getdirentries64 = 0xD2802B10;
    constexpr uint32_t mov_x16_statfs64 = 0xD2802B30;
    constexpr uint32_t mov_x16_fstatfs64 = 0xD2802B50;
    constexpr uint32_t mov_x16_openat = 0xD28039F0;
    constexpr uint32_t mov_x16_pread = 0xD2801330;                      // mov x16, #153
    constexpr uint32_t mov_x16_fcntl = 0xD2800B90;                      // mov x16, #92
    constexpr uint32_t mov_x16_pwrite = 0xD2801350;                     // mov x16, #154
    constexpr uint32_t mov_x16_dup = 0xD2800530;                        // mov x16, #41
    constexpr uint32_t mov_x16_guarded_open_np = 0xD2803730;            // mov x16, #441
    constexpr uint32_t mov_x16_guarded_close_np = 0xD2803750;           // mov x16, #442
    constexpr uint32_t mov_x16_guarded_open_dprotected_np = 0xD2803C90; // mov x16, #484
    constexpr uint32_t mov_x16_guarded_write_np = 0xD2803CB0;           // mov x16, #485
    constexpr uint32_t mov_x16_guarded_pwrite_np = 0xD2803CD0;          // mov x16, #486
    constexpr uint32_t mov_x16_fsync = 0xD2800BF0;                      // mov x16, #95
    constexpr uint32_t mov_x16_fchown = 0xD2800F70;                     // mov x16, #123
    constexpr uint32_t mov_x16_fchmod = 0xD2800F90;                     // mov x16, #124
    constexpr uint32_t mov_x16_rename = 0xD2801010;                     // mov x16, #128
    constexpr uint32_t mov_x16_rmdir = 0xD2801130;                      // mov x16, #137
    constexpr uint32_t mov_x16_futimes = 0xD2801170;                    // mov x16, #139
    constexpr uint32_t mov_x16_truncate = 0xD2801910;                   // mov x16, #200
    constexpr uint32_t mov_x16_ftruncate = 0xD2801930;                  // mov x16, #201

    struct scratch_root
    {
        std::filesystem::path path{};
        bool has_symlink{};

        scratch_root()
            : path(std::filesystem::temp_directory_path() / sogen::test::unique_temp_name("sogen-macos"))
        {
            std::filesystem::create_directories(this->path / "usr" / "lib");
            std::filesystem::create_directories(this->path / "etc");
            std::filesystem::create_directories(this->path / "listing");

            std::ofstream{this->path / "usr" / "lib" / "marker", std::ios::binary} << "0123456789";
            std::ofstream{this->path / "usr" / "lib" / "writable", std::ios::binary} << "LONGER-ORIGINAL-CONTENT";
            std::ofstream{this->path / "etc" / "passwd", std::ios::binary} << "sandboxed";
            std::ofstream{this->path / "listing" / "alpha", std::ios::binary} << "a";
            std::ofstream{this->path / "listing" / "beta", std::ios::binary} << "bb";

            std::error_code error{};
            std::filesystem::create_symlink("marker", this->path / "usr" / "lib" / "link", error);
            this->has_symlink = !error;
        }

        ~scratch_root()
        {
            std::error_code error{};
            std::filesystem::remove_all(this->path, error);
        }

        scratch_root(const scratch_root&) = delete;
        scratch_root& operator=(const scratch_root&) = delete;
    };

    std::unique_ptr<sogen::macos_emulator> make_rooted_emulator(const scratch_root& root)
    {
        return std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path);
    }

    void allocate_data(sogen::macos_emulator& emu)
    {
        if (!emu.memory.get_region_info(data_base).has_value())
        {
            ASSERT_TRUE(emu.memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE * 4, sogen::memory_permission::read_write));
        }
    }

    uint64_t write_guest_path(sogen::macos_emulator& emu, const std::string_view path, const uint64_t offset = 0)
    {
        allocate_data(emu);

        const std::string terminated{path};
        const auto address = data_base + offset;
        emu.memory.write_memory(address, terminated.c_str(), terminated.size() + 1);
        return address;
    }

    // Unicorn caches translated blocks, so replaying a syscall at an address that already ran would
    // execute the first block again; every invocation in a test gets a site of its own.
    void run_syscall(sogen::macos_emulator& emu, const size_t site, const uint32_t mov_x16, const std::vector<uint64_t>& arguments)
    {
        macos_test::write_guest_code(emu, code_base + (site * 0x40), {mov_x16, 0xD4001001});

        for (size_t i = 0; i < arguments.size(); ++i)
        {
            emu.emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::x0) + i), arguments[i]);
        }

        emu.start(2);
    }

    void run_syscall_at(sogen::macos_emulator& emu, const uint64_t address, const uint32_t mov_x16)
    {
        const std::array<uint32_t, 2> words{mov_x16, 0xD4001001};
        emu.memory.write_memory(address, words.data(), sizeof(words));
        emu.emu().reg(sogen::arm64_register::pc, address);
        emu.start(2);
    }

    bool failed(sogen::macos_emulator& emu)
    {
        return (emu.emu().reg(sogen::arm64_register::nzcv) & carry) == carry;
    }

    uint64_t result_of(sogen::macos_emulator& emu)
    {
        return emu.emu().reg(sogen::arm64_register::x0);
    }

    std::string read_host_file(const std::filesystem::path& path)
    {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    uint64_t write_guest_bytes(sogen::macos_emulator& emu, const std::string_view data, const uint64_t offset)
    {
        allocate_data(emu);
        const auto address = data_base + offset;
        emu.memory.write_memory(address, data.data(), data.size());
        return address;
    }

    uint64_t write_guest_guard(sogen::macos_emulator& emu, const uint64_t id, const uint64_t offset)
    {
        allocate_data(emu);
        const auto address = data_base + offset;
        emu.memory.write_memory(address, &id, sizeof(id));
        return address;
    }

    struct guest_dirent
    {
        uint64_t inode{};
        uint64_t seek_offset{};
        uint16_t record_length{};
        uint16_t name_length{};
        uint8_t type{};
        std::string name{};
    };

    std::vector<guest_dirent> parse_dirents(sogen::macos_emulator& emu, const uint64_t address, const uint64_t length)
    {
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (length > 0)
        {
            emu.memory.read_memory(address, bytes.data(), bytes.size());
        }

        std::vector<guest_dirent> entries{};
        size_t offset = 0;
        while (offset + sizeof(sogen::macos_dirent64_header) <= bytes.size())
        {
            sogen::macos_dirent64_header header{};
            memcpy(&header, bytes.data() + offset, sizeof(header));
            if (header.d_reclen == 0 || offset + header.d_reclen > bytes.size())
            {
                break;
            }

            guest_dirent entry{};
            entry.inode = header.d_ino;
            entry.seek_offset = header.d_seekoff;
            entry.record_length = header.d_reclen;
            entry.name_length = header.d_namlen;
            entry.type = header.d_type;
            entry.name.assign(reinterpret_cast<const char*>(bytes.data() + offset + sizeof(header)), header.d_namlen);
            entries.push_back(std::move(entry));

            offset += header.d_reclen;
        }

        return entries;
    }

    TEST(MacosFileSyscalls, StatReportsTheDarwinLayout)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        constexpr uint64_t stat_buffer = data_base + 0x800;

        run_syscall(*emu, 0, mov_x16_stat64, {path, stat_buffer});

        ASSERT_FALSE(failed(*emu));

        sogen::macos_stat64 stat{};
        emu->memory.read_memory(stat_buffer, &stat, sizeof(stat));
        EXPECT_EQ(stat.st_size, 10);
        EXPECT_EQ(stat.st_mode & sogen::macos_stat_mode::MACOS_S_IFMT, sogen::macos_stat_mode::MACOS_S_IFREG);
        EXPECT_NE(stat.st_blksize, 0);
        EXPECT_EQ(stat.st_nlink, 1);
        EXPECT_EQ(stat.st_uid, emu->process.uid);
        EXPECT_EQ(stat.st_gid, emu->process.gid);
        EXPECT_EQ(stat.st_blocks, 1);
    }

    // Reading through the raw offsets instead of the structure catches a layout regression that the
    // static_asserts in macos_stat.hpp would no longer be there to catch.
    TEST(MacosFileSyscalls, StatPlacesTheFieldsAtTheDarwinOffsets)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        constexpr uint64_t stat_buffer = data_base + 0x800;
        emu->memory.set_memory(stat_buffer, 0xCC, 256);

        run_syscall(*emu, 0, mov_x16_stat64, {path, stat_buffer});
        ASSERT_FALSE(failed(*emu));

        int64_t size{};
        emu->memory.read_memory(stat_buffer + 96, &size, sizeof(size));
        EXPECT_EQ(size, 10);

        int32_t block_size{};
        emu->memory.read_memory(stat_buffer + 112, &block_size, sizeof(block_size));
        EXPECT_EQ(block_size, static_cast<int32_t>(sogen::MACOS_PAGE_SIZE));

        uint16_t mode{};
        emu->memory.read_memory(stat_buffer + 4, &mode, sizeof(mode));
        EXPECT_EQ(mode & sogen::macos_stat_mode::MACOS_S_IFMT, sogen::macos_stat_mode::MACOS_S_IFREG);

        std::array<uint8_t, 8> trailing{};
        emu->memory.read_memory(stat_buffer + 136, trailing.data(), trailing.size());
        EXPECT_EQ(trailing, (std::array<uint8_t, 8>{})) << "st_qspare must be written, not left as guest garbage";
    }

    TEST(MacosFileSyscalls, StatOnADirectorySetsTheDirectoryBit)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib");
        constexpr uint64_t stat_buffer = data_base + 0x800;

        run_syscall(*emu, 0, mov_x16_stat64, {path, stat_buffer});
        ASSERT_FALSE(failed(*emu));

        sogen::macos_stat64 stat{};
        emu->memory.read_memory(stat_buffer, &stat, sizeof(stat));
        EXPECT_EQ(stat.st_mode & sogen::macos_stat_mode::MACOS_S_IFMT, sogen::macos_stat_mode::MACOS_S_IFDIR);
    }

    TEST(MacosFileSyscalls, StatOnAMissingPathFailsWithEnoent)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/absent");

        run_syscall(*emu, 0, mov_x16_stat64, {path, data_base + 0x800});

        EXPECT_EQ(result_of(*emu), 2u) << "ENOENT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, StatIntoAnUnmappedBufferIsEfault)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_stat64, {path, 0x700000000ULL});

        EXPECT_EQ(result_of(*emu), 14u) << "EFAULT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, StatOfAnUnmappedPathIsEfault)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        allocate_data(*emu);
        run_syscall(*emu, 0, mov_x16_stat64, {0x700000000ULL, data_base + 0x800});

        EXPECT_EQ(result_of(*emu), 14u) << "EFAULT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, StatOfAnUnterminatedPathIsEnametoolong)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        allocate_data(*emu);
        emu->memory.set_memory(data_base, 'a', sogen::MACOS_PAGE_SIZE * 4);

        run_syscall(*emu, 0, mov_x16_stat64, {data_base, data_base + 0x800});

        EXPECT_EQ(result_of(*emu), 63u) << "ENAMETOOLONG";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, OpenReadCloseRoundTripsThroughTheRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));

        const auto fd = result_of(*emu);
        EXPECT_GE(fd, 3u);

        constexpr uint64_t read_buffer = data_base + 0x800;
        run_syscall(*emu, 1, mov_x16_read, {fd, read_buffer, 10});

        EXPECT_EQ(result_of(*emu), 10u);
        std::array<char, 11> content{};
        emu->memory.read_memory(read_buffer, content.data(), 10);
        EXPECT_STREQ(content.data(), "0123456789");

        run_syscall(*emu, 2, mov_x16_close, {fd});
        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(emu->process.fds.get(static_cast<int>(fd)), nullptr);
    }

    TEST(MacosFileSyscalls, OpenCannotEscapeTheEmulationRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/../../../../etc/hosts");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});

        EXPECT_TRUE(failed(*emu));
    }

    // The negative test above only proves the host file was not found under that name. This one proves
    // the escaping path was resolved inside the root: it reads back the scratch content, never the
    // machine's own /etc/passwd.
    TEST(MacosFileSyscalls, OpenResolvesAnEscapingPathInsideTheRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/../../../../etc/passwd");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));

        constexpr uint64_t read_buffer = data_base + 0x800;
        run_syscall(*emu, 1, mov_x16_read, {result_of(*emu), read_buffer, 64});

        const auto count = result_of(*emu);
        ASSERT_EQ(count, 9u);

        std::array<char, 10> content{};
        emu->memory.read_memory(read_buffer, content.data(), 9);
        EXPECT_STREQ(content.data(), "sandboxed");
    }

    TEST(MacosFileSyscalls, OpenOfAMissingPathIsEnoent)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/absent");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});

        EXPECT_EQ(result_of(*emu), 2u) << "ENOENT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, OpenOfADirectoryYieldsADirectoryDescriptor)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));

        const auto* entry = emu->process.fds.get(static_cast<int>(result_of(*emu)));
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->type, sogen::fd_type::directory);
        EXPECT_EQ(entry->handle, nullptr) << "fopen on a directory must never happen";
    }

    TEST(MacosFileSyscalls, OpenWithODirectoryOnAFileIsEnotdir)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_DIRECTORY)});

        EXPECT_EQ(result_of(*emu), 20u) << "ENOTDIR";
        EXPECT_TRUE(failed(*emu));
    }

    // Darwin and Linux disagree on these two bits and swapping them is a live bug class in this tree:
    // Linux's O_APPEND is 0x0400, which on Darwin is O_TRUNC. The write-mode tests below are only
    // meaningful if the constants they pass are the Darwin ones.
    TEST(MacosFileSyscalls, TheDarwinOpenFlagsAreNotTheLinuxOnes)
    {
        EXPECT_EQ(sogen::macos_open::MACOS_O_APPEND, 0x0008);
        EXPECT_EQ(sogen::macos_open::MACOS_O_TRUNC, 0x0400);
        EXPECT_EQ(sogen::macos_open::MACOS_O_CREAT, 0x0200);
        EXPECT_EQ(sogen::macos_open::MACOS_O_EXCL, 0x0800);
    }

    // POSIX open(O_WRONLY) preserves the file and writes from offset zero. fopen("wb") empties it -
    // measured on this host, an eight byte file becomes zero bytes - which would destroy data inside
    // the user's real emulation root.
    TEST(MacosFileSyscalls, WriteOnlyOpenWithoutOTruncPreservesTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto payload = write_guest_bytes(*emu, "SHORT", 0x800);

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_write, {fd, payload, 5});
        ASSERT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 5u);

        run_syscall(*emu, 2, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "SHORTR-ORIGINAL-CONTENT");
    }

    TEST(MacosFileSyscalls, WriteOnlyOpenWithOTruncEmptiesTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");

        run_syscall(*emu, 0, mov_x16_open,
                    {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_TRUNC)});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_close, {result_of(*emu)});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "");
    }

    TEST(MacosFileSyscalls, AppendOpenWritesAtTheEnd)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto payload = write_guest_bytes(*emu, "-MORE", 0x800);

        run_syscall(*emu, 0, mov_x16_open,
                    {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_APPEND)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_write, {fd, payload, 5});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 2, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "LONGER-ORIGINAL-CONTENT-MORE");
    }

    TEST(MacosFileSyscalls, ReadWriteOpenWithoutOTruncKeepsTheContentReadable)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t read_buffer = data_base + 0x800;
        run_syscall(*emu, 1, mov_x16_read, {fd, read_buffer, 6});
        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(result_of(*emu), 6u);

        std::array<char, 7> content{};
        emu->memory.read_memory(read_buffer, content.data(), 6);
        EXPECT_STREQ(content.data(), "LONGER");
        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "LONGER-ORIGINAL-CONTENT");
    }

    TEST(MacosFileSyscalls, ReadWriteOpenWithOTruncEmptiesTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");

        run_syscall(*emu, 0, mov_x16_open,
                    {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_TRUNC)});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_close, {result_of(*emu)});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "");
    }

    TEST(MacosFileSyscalls, OpenWithOCreatCreatesTheMissingFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/created");
        const auto payload = write_guest_bytes(*emu, "NEW", 0x800);

        run_syscall(*emu, 0, mov_x16_open,
                    {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_CREAT)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_write, {fd, payload, 3});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 2, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "created"), "NEW");
    }

    TEST(MacosFileSyscalls, OpenWithOCreatAndOExclOnAnExistingFileIsEexist)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");

        run_syscall(*emu, 0, mov_x16_open,
                    {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_CREAT |
                                                 sogen::macos_open::MACOS_O_EXCL)});

        EXPECT_EQ(result_of(*emu), 17u) << "EEXIST";
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "LONGER-ORIGINAL-CONTENT");
    }

    TEST(MacosFileSyscalls, ReadOnlyOpenNeverDisturbsTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDONLY)});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_close, {result_of(*emu)});

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "LONGER-ORIGINAL-CONTENT");
    }

    // The emulation root is the guest's "/", so its ".." must describe the root itself rather than the
    // host directory above it - the one place in this file that would otherwise report something
    // translate did not produce.
    TEST(MacosFileSyscalls, DotDotAtTheEmulationRootDescribesTheRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x2000;
        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, buffer, 0x1000, 0});
        ASSERT_FALSE(failed(*emu));

        const auto entries = parse_dirents(*emu, buffer, result_of(*emu));
        ASSERT_GE(entries.size(), 2u);
        ASSERT_EQ(entries[0].name, ".");
        ASSERT_EQ(entries[1].name, "..");
        EXPECT_EQ(entries[1].inode, entries[0].inode) << "the host directory above the emulation root leaked out";
    }

    // The other side of the clamp: a subdirectory's ".." must still name its real parent rather than
    // itself, so the guard above cannot be satisfied by echoing "." everywhere.
    TEST(MacosFileSyscalls, DotDotInASubdirectoryNamesTheParent)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x2000;
        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, buffer, 0x1000, 0});
        ASSERT_FALSE(failed(*emu));

        const auto entries = parse_dirents(*emu, buffer, result_of(*emu));
        ASSERT_GE(entries.size(), 2u);
        ASSERT_EQ(entries[1].name, "..");
        EXPECT_NE(entries[1].inode, entries[0].inode) << R"(".." collapsed onto ".")";
        EXPECT_NE(entries[1].inode, 0u);
    }

    // Opening a character device would park the emulator inside a blocking host read that nothing can
    // resume; a previous incarnation of this path took the suite to a 137 kill.
    TEST(MacosFileSyscalls, OpenOfACharacterDeviceIsRefused)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/dev/urandom");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});

        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, OpenPastTheDescriptorLimitIsEmfile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        while (static_cast<int>(emu->process.fds.get_fds().size()) < sogen::MACOS_MAX_OPEN_DESCRIPTORS)
        {
            sogen::guest_fd filler{};
            filler.type = sogen::fd_type::memory_file;
            filler.memory_file = std::make_shared<sogen::guest_memory_fd>();
            emu->process.fds.allocate(std::move(filler));
        }

        run_syscall(*emu, 0, mov_x16_open, {path, 0});

        EXPECT_EQ(result_of(*emu), 24u) << "EMFILE";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, OpenatWithAtFdcwdBehavesLikeOpen)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_openat, {static_cast<uint64_t>(static_cast<int64_t>(sogen::macos_open::MACOS_AT_FDCWD)), path, 0});

        ASSERT_FALSE(failed(*emu));
        EXPECT_GE(result_of(*emu), 3u);
    }

    // dyld opens /System/Library/dyld and then asks for "dyld_shared_cache_arm64e" relative to that
    // descriptor; it is the only way it ever names the cache, so a relative openat has to resolve.
    TEST(MacosFileSyscalls, OpenatResolvesARelativePathAgainstADirectoryDescriptor)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto directory = write_guest_path(*emu, "/usr/lib");
        run_syscall(*emu, 0, mov_x16_open, {directory, 0});
        ASSERT_FALSE(failed(*emu));
        const auto directory_fd = result_of(*emu);

        const auto relative = write_guest_path(*emu, "marker", 0x100);
        run_syscall(*emu, 1, mov_x16_openat, {directory_fd, relative, 0});
        ASSERT_FALSE(failed(*emu)) << "openat against a directory descriptor must resolve";
        const auto fd = result_of(*emu);

        constexpr uint64_t read_buffer = data_base + 0x800;
        run_syscall(*emu, 2, mov_x16_read, {fd, read_buffer, 10});
        ASSERT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 10u);

        std::array<char, 11> content{};
        emu->memory.read_memory(read_buffer, content.data(), 10);
        EXPECT_STREQ(content.data(), "0123456789");
    }

    TEST(MacosFileSyscalls, OpenatRejectsADescriptorThatIsNotADirectory)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto file_fd = result_of(*emu);

        const auto relative = write_guest_path(*emu, "marker", 0x100);

        run_syscall(*emu, 1, mov_x16_openat, {file_fd, relative, 0});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 20u) << "ENOTDIR";

        run_syscall(*emu, 2, mov_x16_openat, {999, relative, 0});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 9u) << "EBADF";
    }

    TEST(MacosFileSyscalls, AccessReportsExistence)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_access, {path, 0});

        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_FALSE(failed(*emu));
    }

    TEST(MacosFileSyscalls, AccessOnAMissingPathIsEnoent)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/absent");

        run_syscall(*emu, 0, mov_x16_access, {path, 0});

        EXPECT_EQ(result_of(*emu), 2u) << "ENOENT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, FstatMatchesStatForTheSameFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t stat_buffer = data_base + 0x800;
        constexpr uint64_t fstat_buffer = data_base + 0x1000;

        run_syscall(*emu, 1, mov_x16_stat64, {path, stat_buffer});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 2, mov_x16_fstat64, {fd, fstat_buffer});
        ASSERT_FALSE(failed(*emu));

        sogen::macos_stat64 from_path{};
        sogen::macos_stat64 from_fd{};
        emu->memory.read_memory(stat_buffer, &from_path, sizeof(from_path));
        emu->memory.read_memory(fstat_buffer, &from_fd, sizeof(from_fd));

        EXPECT_EQ(from_fd.st_size, 10);
        EXPECT_EQ(from_fd.st_size, from_path.st_size);
        EXPECT_EQ(from_fd.st_ino, from_path.st_ino);
        EXPECT_EQ(from_fd.st_mode, from_path.st_mode);
    }

    TEST(MacosFileSyscalls, FstatOnABadDescriptorIsEbadf)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        allocate_data(*emu);
        run_syscall(*emu, 0, mov_x16_fstat64, {999, data_base + 0x800});

        EXPECT_EQ(result_of(*emu), 9u) << "EBADF";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, LstatReportsTheSymlinkAndStatReportsItsTarget)
    {
        const scratch_root root{};
        if (!root.has_symlink)
        {
            GTEST_SKIP() << "the host refused to create a symlink in the scratch root";
        }

        const auto emu = make_rooted_emulator(root);
        const auto path = write_guest_path(*emu, "/usr/lib/link");

        constexpr uint64_t link_buffer = data_base + 0x800;
        constexpr uint64_t target_buffer = data_base + 0x1000;

        run_syscall(*emu, 0, mov_x16_lstat64, {path, link_buffer});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_stat64, {path, target_buffer});
        ASSERT_FALSE(failed(*emu));

        sogen::macos_stat64 link{};
        sogen::macos_stat64 target{};
        emu->memory.read_memory(link_buffer, &link, sizeof(link));
        emu->memory.read_memory(target_buffer, &target, sizeof(target));

        EXPECT_EQ(link.st_mode & sogen::macos_stat_mode::MACOS_S_IFMT, sogen::macos_stat_mode::MACOS_S_IFLNK);
        EXPECT_EQ(link.st_size, 6) << "the length of \"marker\"";
        EXPECT_EQ(target.st_mode & sogen::macos_stat_mode::MACOS_S_IFMT, sogen::macos_stat_mode::MACOS_S_IFREG);
        EXPECT_EQ(target.st_size, 10);
    }

    TEST(MacosFileSyscalls, ReadlinkReturnsTheTargetWithoutATerminator)
    {
        const scratch_root root{};
        if (!root.has_symlink)
        {
            GTEST_SKIP() << "the host refused to create a symlink in the scratch root";
        }

        const auto emu = make_rooted_emulator(root);
        const auto path = write_guest_path(*emu, "/usr/lib/link");

        constexpr uint64_t buffer = data_base + 0x800;
        emu->memory.set_memory(buffer, 0xCC, 64);

        run_syscall(*emu, 0, mov_x16_readlink, {path, buffer, 64});

        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(result_of(*emu), 6u);

        std::array<char, 7> content{};
        emu->memory.read_memory(buffer, content.data(), 6);
        EXPECT_STREQ(content.data(), "marker");

        uint8_t after{};
        emu->memory.read_memory(buffer + 6, &after, sizeof(after));
        EXPECT_EQ(after, 0xCC) << "readlink must not write a terminator";
    }

    TEST(MacosFileSyscalls, ReadlinkTruncatesToTheBufferSize)
    {
        const scratch_root root{};
        if (!root.has_symlink)
        {
            GTEST_SKIP() << "the host refused to create a symlink in the scratch root";
        }

        const auto emu = make_rooted_emulator(root);
        const auto path = write_guest_path(*emu, "/usr/lib/link");

        constexpr uint64_t buffer = data_base + 0x800;
        emu->memory.set_memory(buffer, 0xCC, 64);

        run_syscall(*emu, 0, mov_x16_readlink, {path, buffer, 3});

        ASSERT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 3u);

        std::array<uint8_t, 4> content{};
        emu->memory.read_memory(buffer, content.data(), content.size());
        EXPECT_EQ(content[3], 0xCC) << "readlink wrote past the caller's buffer";
    }

    TEST(MacosFileSyscalls, ReadlinkWithAHugeBufferSizeDoesNotAllocateIt)
    {
        const scratch_root root{};
        if (!root.has_symlink)
        {
            GTEST_SKIP() << "the host refused to create a symlink in the scratch root";
        }

        const auto emu = make_rooted_emulator(root);
        const auto path = write_guest_path(*emu, "/usr/lib/link");

        run_syscall(*emu, 0, mov_x16_readlink, {path, data_base + 0x800, 0xFFFFFFFFFFFFULL});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 6u);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosFileSyscalls, ReadlinkOnARegularFileIsEinval)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_readlink, {path, data_base + 0x800, 64});

        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, UnlinkRemovesTheFileFromTheRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing/alpha");

        run_syscall(*emu, 0, mov_x16_unlink, {path});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_FALSE(std::filesystem::exists(root.path / "listing" / "alpha"));
        EXPECT_TRUE(std::filesystem::exists(root.path / "listing" / "beta"));
    }

    TEST(MacosFileSyscalls, UnlinkOnADirectoryIsEperm)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_unlink, {path});

        EXPECT_EQ(result_of(*emu), 1u) << "EPERM";
        EXPECT_TRUE(failed(*emu));
        EXPECT_TRUE(std::filesystem::exists(root.path / "listing"));
    }

    TEST(MacosFileSyscalls, MkdirCreatesTheDirectoryInTheRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing/nested");

        run_syscall(*emu, 0, mov_x16_mkdir, {path, 0755});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_TRUE(std::filesystem::is_directory(root.path / "listing" / "nested"));
    }

    TEST(MacosFileSyscalls, MkdirOnAnExistingNameIsEexist)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing/alpha");

        run_syscall(*emu, 0, mov_x16_mkdir, {path, 0755});

        EXPECT_EQ(result_of(*emu), 17u) << "EEXIST";
        EXPECT_TRUE(failed(*emu));
        EXPECT_TRUE(std::filesystem::is_regular_file(root.path / "listing" / "alpha"));
    }

    TEST(MacosFileSyscalls, MkdirWithoutItsParentIsEnoent)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing/absent/nested");

        run_syscall(*emu, 0, mov_x16_mkdir, {path, 0755});

        EXPECT_EQ(result_of(*emu), 2u) << "ENOENT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, MkdiratResolvesARelativePathAgainstADirectoryDescriptor)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto directory = write_guest_path(*emu, "/listing");
        run_syscall(*emu, 0, mov_x16_open, {directory, 0});
        ASSERT_FALSE(failed(*emu));
        const auto directory_fd = result_of(*emu);

        const auto relative = write_guest_path(*emu, "nested", 0x100);
        run_syscall(*emu, 1, mov_x16_mkdirat, {directory_fd, relative, 0755});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_TRUE(std::filesystem::is_directory(root.path / "listing" / "nested"));
    }

    TEST(MacosFileSyscalls, MkdiratRejectsADescriptorThatIsNotADirectory)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto file = write_guest_path(*emu, "/listing/alpha");
        run_syscall(*emu, 0, mov_x16_open, {file, 0});
        ASSERT_FALSE(failed(*emu));
        const auto file_fd = result_of(*emu);

        const auto relative = write_guest_path(*emu, "nested", 0x100);
        run_syscall(*emu, 1, mov_x16_mkdirat, {file_fd, relative, 0755});

        EXPECT_EQ(result_of(*emu), 20u) << "ENOTDIR";
        EXPECT_TRUE(failed(*emu));
        EXPECT_FALSE(std::filesystem::exists(root.path / "listing" / "nested"));
    }

    TEST(MacosFileSyscalls, GetdirentriesEnumeratesTheDirectory)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x2000;
        constexpr uint64_t position = data_base + 0x1000;

        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, buffer, 0x1000, position});
        ASSERT_FALSE(failed(*emu));

        const auto written = result_of(*emu);
        ASSERT_GT(written, 0u);

        const auto entries = parse_dirents(*emu, buffer, written);
        ASSERT_EQ(entries.size(), 4u);

        std::vector<std::string> names{};
        for (const auto& entry : entries)
        {
            EXPECT_EQ(entry.record_length % 8, 0u);
            EXPECT_EQ(entry.name_length, entry.name.size());
            EXPECT_EQ(entry.record_length, (32 + entry.name_length) & ~7u) << "xnu's DIRENT64_LEN for " << entry.name;
            names.push_back(entry.name);
        }

        EXPECT_EQ(names[0], ".");
        EXPECT_EQ(names[1], "..");
        EXPECT_EQ(entries[0].type, sogen::macos_dirent_type::MACOS_DT_DIR);

        EXPECT_NE(std::ranges::find(names, "alpha"), names.end());
        EXPECT_NE(std::ranges::find(names, "beta"), names.end());

        int64_t reported_position{};
        emu->memory.read_memory(position, &reported_position, sizeof(reported_position));
        EXPECT_EQ(reported_position, 4);
    }

    TEST(MacosFileSyscalls, GetdirentriesResumesWhereItStopped)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x2000;

        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, buffer, 32, 0});
        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(result_of(*emu), 32u);

        const auto first = parse_dirents(*emu, buffer, 32);
        ASSERT_EQ(first.size(), 1u);
        EXPECT_EQ(first[0].name, ".");

        run_syscall(*emu, 2, mov_x16_getdirentries64, {fd, buffer, 32, 0});
        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(result_of(*emu), 32u);

        const auto second = parse_dirents(*emu, buffer, 32);
        ASSERT_EQ(second.size(), 1u);
        EXPECT_EQ(second[0].name, "..") << "the second call replayed the first record";
    }

    TEST(MacosFileSyscalls, GetdirentriesReportsZeroWhenTheBufferIsTooSmall)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, data_base + 0x2000, 4, 0});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosFileSyscalls, GetdirentriesWithAHugeBufferSizeDoesNotAllocateIt)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, data_base + 0x2000, 0xFFFFFFFFFFFFULL, 0});

        EXPECT_FALSE(failed(*emu));
        EXPECT_GT(result_of(*emu), 0u);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosFileSyscalls, GetdirentriesOnAFileIsEnotdir)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_getdirentries64, {result_of(*emu), data_base + 0x2000, 0x1000, 0});

        EXPECT_EQ(result_of(*emu), 20u) << "ENOTDIR";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, ClosingADirectoryDropsItsCachedListing)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/listing");

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_getdirentries64, {fd, data_base + 0x2000, 32, 0});
        ASSERT_EQ(result_of(*emu), 32u);
        ASSERT_TRUE(emu->process.directory_entries.contains(static_cast<int>(fd)));

        run_syscall(*emu, 2, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));

        EXPECT_FALSE(emu->process.directory_entries.contains(static_cast<int>(fd)))
            << "a reused descriptor would replay the previous directory";
        EXPECT_FALSE(emu->process.directory_offsets.contains(static_cast<int>(fd)));
    }

    TEST(MacosFileSyscalls, StatfsReportsThePageSizedBlock)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib");
        constexpr uint64_t buffer = data_base + 0x800;

        run_syscall(*emu, 0, mov_x16_statfs64, {path, buffer});

        ASSERT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);

        sogen::macos_statfs64 statfs{};
        emu->memory.read_memory(buffer, &statfs, sizeof(statfs));
        EXPECT_EQ(statfs.f_bsize, sogen::MACOS_PAGE_SIZE);
        EXPECT_STREQ(statfs.f_mntonname.data(), "/");
    }

    TEST(MacosFileSyscalls, StatfsOnAMissingPathIsEnoent)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/absent");

        run_syscall(*emu, 0, mov_x16_statfs64, {path, data_base + 0x800});

        EXPECT_EQ(result_of(*emu), 2u) << "ENOENT";
        EXPECT_TRUE(failed(*emu));
    }

    TEST(MacosFileSyscalls, FstatfsOnABadDescriptorIsEbadf)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        allocate_data(*emu);
        run_syscall(*emu, 0, mov_x16_fstatfs64, {999, data_base + 0x800});

        EXPECT_EQ(result_of(*emu), 9u) << "EBADF";
        EXPECT_TRUE(failed(*emu));
    }

    // dyld cross-checks the st_ino it gets from stat64 against the one it gets from fstat64 on the
    // descriptor it already opened, and concludes the image moved underneath it when they disagree.
    // Reporting host inodes made them agree by accident; a synthetic namespace has to make them agree
    // on purpose.
    TEST(MacosFileSyscalls, StatAndFstatAgreeOnTheIdentityOfOneFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        constexpr uint64_t stat_buffer = data_base + 0x800;
        constexpr uint64_t fstat_buffer = data_base + 0x900;

        run_syscall(*emu, 0, mov_x16_stat64, {path, stat_buffer});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 2, mov_x16_fstat64, {fd, fstat_buffer});
        ASSERT_FALSE(failed(*emu));

        sogen::macos_stat64 by_path{};
        sogen::macos_stat64 by_fd{};
        emu->memory.read_memory(stat_buffer, &by_path, sizeof(by_path));
        emu->memory.read_memory(fstat_buffer, &by_fd, sizeof(by_fd));

        EXPECT_EQ(by_path.st_ino, by_fd.st_ino);
        EXPECT_EQ(by_path.st_dev, by_fd.st_dev);
        EXPECT_EQ(by_path.st_dev, static_cast<int32_t>(sogen::MACOS_SYNTHETIC_FSID_DEV));
        EXPECT_NE(by_path.st_ino, 0u);

        const auto resolved = emu->identities.resolve(sogen::MACOS_SYNTHETIC_FSID_DEV, sogen::MACOS_SYNTHETIC_FSID_VFSTYPE, by_path.st_ino);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*resolved, "/usr/lib/marker");
    }

    TEST(MacosFileSyscalls, TwoDifferentFilesNeverShareAnObjectId)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto first = write_guest_path(*emu, "/usr/lib/marker");
        const auto second = write_guest_path(*emu, "/usr/lib/writable", 0x100);
        constexpr uint64_t first_buffer = data_base + 0x800;
        constexpr uint64_t second_buffer = data_base + 0x900;

        run_syscall(*emu, 0, mov_x16_stat64, {first, first_buffer});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 1, mov_x16_stat64, {second, second_buffer});
        if (failed(*emu))
        {
            FAIL() << "the scratch root should have a second file";
        }

        sogen::macos_stat64 one{};
        sogen::macos_stat64 two{};
        emu->memory.read_memory(first_buffer, &one, sizeof(one));
        emu->memory.read_memory(second_buffer, &two, sizeof(two));

        EXPECT_NE(one.st_ino, two.st_ino);
    }

    // dyld keeps one descriptor on the shared cache and reads many offsets from it, so pread has to
    // leave the file offset exactly where it found it.
    TEST(MacosFileSyscalls, PreadReadsAtAnOffsetWithoutMovingTheDescriptor)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x800;

        run_syscall(*emu, 1, mov_x16_read, {fd, buffer, 3});
        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(result_of(*emu), 3u);

        run_syscall(*emu, 2, mov_x16_pread, {fd, buffer + 0x40, 4, 6});
        ASSERT_FALSE(failed(*emu)) << "pread at offset 6";
        EXPECT_EQ(result_of(*emu), 4u);

        std::array<char, 5> at_offset{};
        emu->memory.read_memory(buffer + 0x40, at_offset.data(), 4);
        EXPECT_STREQ(at_offset.data(), "6789");

        // The sequential read must resume where it was, not where pread went.
        run_syscall(*emu, 3, mov_x16_read, {fd, buffer + 0x80, 3});
        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(result_of(*emu), 3u);

        std::array<char, 4> resumed{};
        emu->memory.read_memory(buffer + 0x80, resumed.data(), 3);
        EXPECT_STREQ(resumed.data(), "345") << "pread moved the descriptor";
    }

    TEST(MacosFileSyscalls, PreadRefusesANegativeOffsetAndADirectory)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_pread, {fd, data_base + 0x800, 4, static_cast<uint64_t>(-1)});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";

        const auto directory = write_guest_path(*emu, "/usr/lib", 0x100);
        run_syscall(*emu, 2, mov_x16_open, {directory, 0});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 3, mov_x16_pread, {result_of(*emu), data_base + 0x800, 4, 0});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 29u) << "ESPIPE";
    }

    // dyld asks the shared cache descriptor for its own path and abandons the cache when the answer
    // does not come back. It is the guest path that has to come out: the guest has never seen the host
    // one, and handing it back would leak the emulation root into the guest's own view of itself.
    TEST(MacosFileSyscalls, FcntlGetPathReturnsTheGuestPathNotTheHostPath)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x800;
        run_syscall(*emu, 1, mov_x16_fcntl, {fd, 50, buffer});
        ASSERT_FALSE(failed(*emu)) << "F_GETPATH";
        EXPECT_EQ(result_of(*emu), 0u);

        std::array<char, static_cast<size_t>(sogen::MACOS_PATH_MAX)> reported{};
        emu->memory.read_memory(buffer, reported.data(), reported.size());
        EXPECT_STREQ(reported.data(), "/usr/lib/marker");
        EXPECT_EQ(std::string{reported.data()}.find(root.path.string()), std::string::npos) << "the host path must not leak";

        run_syscall(*emu, 2, mov_x16_fcntl, {fd, 102, buffer + 0x400});
        ASSERT_FALSE(failed(*emu)) << "F_GETPATH_NOFIRMLINK";
        emu->memory.read_memory(buffer + 0x400, reported.data(), reported.size());
        EXPECT_STREQ(reported.data(), "/usr/lib/marker") << "a synthetic root has no firmlinks to strip";
    }

    // xnu answers F_GETPATH with copyout(path, argp, strlen(path) + 1) -- vn_getpath sets the length
    // from the path it built, not from MAXPATHLEN. Splatting a whole 1 KiB scratch buffer instead writes
    // up to 1023 bytes past what the kernel would have touched, into whatever followed the caller's
    // buffer.
    TEST(MacosFileSyscalls, FcntlGetPathWritesTheStringAndItsTerminatorAndNothingMore)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x800;
        constexpr size_t guard_length = 64;
        constexpr std::string_view expected = "/usr/lib/marker";

        emu->memory.set_memory(buffer, 0x5A, expected.size() + 1 + guard_length);

        run_syscall(*emu, 1, mov_x16_fcntl, {fd, 50, buffer});
        ASSERT_FALSE(failed(*emu)) << "F_GETPATH";

        std::array<char, expected.size() + 1> reported{};
        emu->memory.read_memory(buffer, reported.data(), reported.size());
        EXPECT_STREQ(reported.data(), expected.data());

        std::array<uint8_t, guard_length> guard{};
        emu->memory.read_memory(buffer + expected.size() + 1, guard.data(), guard.size());
        EXPECT_EQ(std::ranges::count(guard, uint8_t{0x5A}), static_cast<ptrdiff_t>(guard.size()))
            << "the bytes past the terminator belong to the caller, not to the kernel";
    }

    // getcwd(3) is open(".") plus F_GETPATH, so a descriptor that remembers the relative path the caller
    // typed makes getcwd() answer ".". CFURL resolves a relative path against the current directory URL,
    // which it builds from getcwd(), so a relative answer there recurses until the stack is gone --
    // measured as Calculator dying in _CFURLCreateCurrentDirectoryURL.
    TEST(MacosFileSyscalls, FcntlGetPathAnswersAnAbsolutePathForADescriptorOpenedRelatively)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);
        emu->process.current_working_directory = "/usr/lib";

        const auto path = write_guest_path(*emu, ".");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto directory_fd = result_of(*emu);

        constexpr uint64_t buffer = data_base + 0x800;
        run_syscall(*emu, 1, mov_x16_fcntl, {directory_fd, 50, buffer});
        ASSERT_FALSE(failed(*emu)) << "F_GETPATH";

        std::array<char, static_cast<size_t>(sogen::MACOS_PATH_MAX)> reported{};
        emu->memory.read_memory(buffer, reported.data(), reported.size());
        EXPECT_STREQ(reported.data(), "/usr/lib");

        const auto relative = write_guest_path(*emu, "marker", 0x100);
        run_syscall(*emu, 2, mov_x16_open, {relative, 0});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 3, mov_x16_fcntl, {result_of(*emu), 50, buffer + 0x400});
        ASSERT_FALSE(failed(*emu)) << "F_GETPATH";
        emu->memory.read_memory(buffer + 0x400, reported.data(), reported.size());
        EXPECT_STREQ(reported.data(), "/usr/lib/marker");
    }

    // dyld registers the shared cache's code signature with F_ADDFILESIGS_RETURN and abandons the cache
    // when it fails, so this call is what decides whether a guest gets a cache at all.
    TEST(MacosFileSyscalls, FcntlAddFileSigsReturnsTheEndOfTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        constexpr uint64_t signatures = data_base + 0x800;
        run_syscall(*emu, 1, mov_x16_fcntl, {fd, 97, signatures});
        ASSERT_FALSE(failed(*emu)) << "F_ADDFILESIGS_RETURN";
        EXPECT_EQ(result_of(*emu), 0u);

        int64_t reported_end{};
        emu->memory.read_memory(signatures, &reported_end, sizeof(reported_end));
        EXPECT_EQ(reported_end, 10) << "the caller bounds its own reads with this offset";

        const auto directory = write_guest_path(*emu, "/usr/lib", 0x100);
        run_syscall(*emu, 2, mov_x16_open, {directory, 0});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 3, mov_x16_fcntl, {result_of(*emu), 97, signatures});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 9u) << "EBADF for a directory";
    }

    // libSystem routes through the _nocancel entry whenever the thread is not a cancellation point, which
    // is most of the time. The variant differs only by skipping a pthread cancellation check, and an
    // emulator with no cancellable threads has nothing extra to skip -- so it is the same handler, and a
    // guest must not be able to tell the two apart.
    TEST(MacosFileSyscalls, WritevNocancelBehavesExactlyLikeWritev)
    {
        const auto emu = macos_test::make_emulator();
        emu->process.setup(emu->emu(), emu->memory, code_base, {"/bin/hello"}, {}, "/bin/hello");

        constexpr uint64_t scratch = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        // Two iovecs so the call is a real gather, not a disguised write.
        const uint64_t iov = scratch + 0x800;
        const uint64_t first = scratch + 0x900;
        const uint64_t second = scratch + 0x910;

        ASSERT_TRUE(emu->memory.try_write_memory(first, "abc", 3));
        ASSERT_TRUE(emu->memory.try_write_memory(second, "de", 2));

        const std::array<uint64_t, 4> vectors{first, 3, second, 2};
        ASSERT_TRUE(emu->memory.try_write_memory(iov, vectors.data(), sizeof(vectors)));

        const auto call = [&](const uint64_t offset, const uint32_t number) {
            emu->emu().reg(sogen::arm64_register::x0, uint64_t{1}); // stdout
            emu->emu().reg(sogen::arm64_register::x1, iov);
            emu->emu().reg(sogen::arm64_register::x2, uint64_t{2});
            run_syscall_at(*emu, scratch + offset, macos_test::movz_x(16, number, 0));
            return emu->emu().reg(sogen::arm64_register::x0);
        };

        const auto plain = call(0x00, 121);    // writev
        const auto nocancel = call(0x20, 412); // writev_nocancel

        EXPECT_EQ(plain, 5u) << "both iovecs are written";
        EXPECT_EQ(nocancel, plain) << "the _nocancel variant is the same syscall";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
    }

    TEST(MacosFileSyscalls, FtruncateAndTruncateResizeTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_ftruncate, {fd, 6});
        ASSERT_FALSE(failed(*emu)) << "ftruncate reported errno " << result_of(*emu);
        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "LONGER");

        run_syscall(*emu, 2, mov_x16_ftruncate, {fd, static_cast<uint64_t>(-1)});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));

        run_syscall(*emu, 3, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 4, mov_x16_truncate, {path, 3});
        ASSERT_FALSE(failed(*emu)) << "truncate reported errno " << result_of(*emu);
        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "LON");
    }

    TEST(MacosFileSyscalls, FsyncFlushesAWrittenFileAndToleratesAStream)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto payload = write_guest_bytes(*emu, "abc", 0x200);

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_write, {fd, payload, 3});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 2, mov_x16_fsync, {fd});
        EXPECT_FALSE(failed(*emu)) << "fsync reported errno " << result_of(*emu);
        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "abcGER-ORIGINAL-CONTENT");

        run_syscall(*emu, 3, mov_x16_fsync, {1});
        EXPECT_FALSE(failed(*emu)) << "stdout has nothing to flush to a disk, and fsync on it is not an error";

        run_syscall(*emu, 4, mov_x16_fsync, {99});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EBADF));
    }

    // libsqlite3 touches the proxy conch with a null tptr to say the lock is still live.
    TEST(MacosFileSyscalls, FutimesSetsTheModificationTime)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto marker = root.path / "usr" / "lib" / "writable";
        const auto before = std::filesystem::last_write_time(marker).time_since_epoch().count();

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        // Two timevals: the access time sogen does not keep, then the modification time it does.
        const std::array<sogen::macos_timeval, 2> requested{sogen::macos_timeval{.tv_sec = 1000000, .tv_usec = 0},
                                                            sogen::macos_timeval{.tv_sec = 1000000, .tv_usec = 0}};
        allocate_data(*emu);
        emu->memory.write_memory(data_base + 0x300, requested.data(), sizeof(requested));

        run_syscall(*emu, 1, mov_x16_futimes, {fd, data_base + 0x300});
        ASSERT_FALSE(failed(*emu)) << "futimes reported errno " << result_of(*emu);
        EXPECT_NE(std::filesystem::last_write_time(marker).time_since_epoch().count(), before);

        run_syscall(*emu, 2, mov_x16_futimes, {fd, 0});
        EXPECT_FALSE(failed(*emu)) << "a null tptr means now";

        run_syscall(*emu, 3, mov_x16_futimes, {99, 0});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EBADF));
    }

    TEST(MacosFileSyscalls, RenameMovesAFileAndStaysInsideTheRoot)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto from = write_guest_path(*emu, "/usr/lib/writable", 0x000);
        const auto to = write_guest_path(*emu, "/usr/lib/renamed", 0x080);

        run_syscall(*emu, 0, mov_x16_rename, {from, to});
        ASSERT_FALSE(failed(*emu)) << "rename reported errno " << result_of(*emu);
        EXPECT_FALSE(std::filesystem::exists(root.path / "usr" / "lib" / "writable"));
        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "renamed"), "LONGER-ORIGINAL-CONTENT");

        run_syscall(*emu, 1, mov_x16_rename, {from, to});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOENT));
    }

    TEST(MacosFileSyscalls, RmdirRemovesAnEmptyDirectoryOnly)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        std::filesystem::create_directory(root.path / "empty");

        const auto empty = write_guest_path(*emu, "/empty", 0x000);
        const auto listing = write_guest_path(*emu, "/listing", 0x080);
        const auto file = write_guest_path(*emu, "/usr/lib/marker", 0x100);

        run_syscall(*emu, 0, mov_x16_rmdir, {listing});
        EXPECT_TRUE(failed(*emu)) << "/listing holds two files";
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOTEMPTY));

        run_syscall(*emu, 1, mov_x16_rmdir, {file});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOTDIR));

        run_syscall(*emu, 2, mov_x16_rmdir, {empty});
        EXPECT_FALSE(failed(*emu)) << "rmdir reported errno " << result_of(*emu);
        EXPECT_FALSE(std::filesystem::exists(root.path / "empty"));
    }

    // Both are accepted and dropped, for the reason mkdir drops its mode: the emulation root is the
    // analyst's own tree, and a guest is not allowed to make it unreadable to the emulator running it.
    TEST(MacosFileSyscalls, FchmodAndFchownAreAcceptedWithoutTouchingTheHost)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto marker = root.path / "usr" / "lib" / "writable";
        const auto before = static_cast<uint32_t>(std::filesystem::status(marker).permissions());

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_fchmod, {fd, 0});
        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(static_cast<uint32_t>(std::filesystem::status(marker).permissions()), before);

        run_syscall(*emu, 2, mov_x16_fchown, {fd, 0, 0});
        EXPECT_FALSE(failed(*emu));

        run_syscall(*emu, 3, mov_x16_fchmod, {99, 0});
        EXPECT_TRUE(failed(*emu)) << "an errno reaches the guest through the carry flag, not through x0 alone";
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EBADF));

        run_syscall(*emu, 4, mov_x16_fchown, {99, 0, 0});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EBADF));
    }

    // libsqlite3 will not take a lock on a database it cannot lock the .store-conch of, and CoreData
    // turns the refusal into SQLITE_IOERR_LOCK, so an unimplemented F_OFD_SETLK reads to the guest as a
    // failing disk rather than as a missing feature.
    uint64_t write_guest_flock(sogen::macos_emulator& emu, const int16_t type, const int64_t start, const int64_t length,
                               const uint64_t offset)
    {
        allocate_data(emu);

        const sogen::macos_flock lock{
            .l_start = start, .l_len = length, .l_pid = 0, .l_type = type, .l_whence = sogen::macos_fcntl::MACOS_SEEK_SET};

        const auto address = data_base + offset;
        emu.memory.write_memory(address, &lock, sizeof(lock));
        return address;
    }

    sogen::macos_flock read_guest_flock(sogen::macos_emulator& emu, const uint64_t address)
    {
        sogen::macos_flock lock{};
        emu.memory.read_memory(address, &lock, sizeof(lock));
        return lock;
    }

    TEST(MacosFileSyscalls, AnOpenFileDescriptionLockIsTakenAndReleased)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        const auto request = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_WRLCK, 1024, 16, 0x100);
        run_syscall(*emu, 1, mov_x16_fcntl, {fd, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), request});
        ASSERT_FALSE(failed(*emu)) << "F_OFD_SETLK reported errno " << result_of(*emu);
        ASSERT_EQ(emu->process.file_locks.size(), 1u);
        EXPECT_EQ(emu->process.file_locks.front().start, 1024u);
        EXPECT_EQ(emu->process.file_locks.front().length, 16u);
        EXPECT_TRUE(emu->process.file_locks.front().exclusive);

        const auto release = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_UNLCK, 1024, 16, 0x140);
        run_syscall(*emu, 2, mov_x16_fcntl, {fd, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), release});
        EXPECT_FALSE(failed(*emu));
        EXPECT_TRUE(emu->process.file_locks.empty());
    }

    TEST(MacosFileSyscalls, ASecondDescriptorContendsForAnExclusiveRange)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto first = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto second = result_of(*emu);
        ASSERT_NE(first, second);

        const auto exclusive = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_WRLCK, 100, 10, 0x100);
        run_syscall(*emu, 2, mov_x16_fcntl, {first, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), exclusive});
        ASSERT_FALSE(failed(*emu));

        const auto overlapping = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_RDLCK, 105, 10, 0x140);
        run_syscall(*emu, 3, mov_x16_fcntl, {second, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), overlapping});
        EXPECT_TRUE(failed(*emu)) << "an overlapping range held exclusively by another descriptor is a conflict";
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EAGAIN));

        const auto clear = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_RDLCK, 200, 10, 0x180);
        run_syscall(*emu, 4, mov_x16_fcntl, {second, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), clear});
        EXPECT_FALSE(failed(*emu)) << "a range nobody else holds is free";

        // F_GETLK reports the holder rather than taking anything, and an OFD lock has no owning pid.
        const auto query = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_WRLCK, 100, 10, 0x1C0);
        run_syscall(*emu, 5, mov_x16_fcntl, {second, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_GETLK), query});
        ASSERT_FALSE(failed(*emu));
        const auto reported = read_guest_flock(*emu, query);
        EXPECT_EQ(reported.l_type, sogen::macos_fcntl::MACOS_F_WRLCK);
        EXPECT_EQ(reported.l_start, 100);
        EXPECT_EQ(reported.l_len, 10);
        EXPECT_EQ(reported.l_pid, -1);
    }

    TEST(MacosFileSyscalls, TwoSharedLocksOnOneRangeCoexist)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto first = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto second = result_of(*emu);

        const auto shared = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_RDLCK, 0, 0, 0x100);
        run_syscall(*emu, 2, mov_x16_fcntl, {first, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), shared});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 3, mov_x16_fcntl, {second, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), shared});
        EXPECT_FALSE(failed(*emu)) << "two readers of one range do not conflict";
        EXPECT_EQ(emu->process.file_locks.size(), 2u);

        // A zero length reaches the end of the file, so an exclusive request anywhere in it conflicts.
        const auto exclusive = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_WRLCK, 4096, 1, 0x140);
        run_syscall(*emu, 4, mov_x16_fcntl, {second, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), exclusive});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EAGAIN));
    }

    TEST(MacosFileSyscalls, UnlockingPartOfARangeLeavesTheRest)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        const auto whole = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_WRLCK, 0, 300, 0x100);
        run_syscall(*emu, 1, mov_x16_fcntl, {fd, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), whole});
        ASSERT_FALSE(failed(*emu));

        const auto middle = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_UNLCK, 100, 100, 0x140);
        run_syscall(*emu, 2, mov_x16_fcntl, {fd, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), middle});
        ASSERT_FALSE(failed(*emu));

        ASSERT_EQ(emu->process.file_locks.size(), 2u);
        auto ranges = std::vector<std::pair<uint64_t, uint64_t>>{};
        for (const auto& held : emu->process.file_locks)
        {
            ranges.emplace_back(held.start, held.length);
        }
        std::ranges::sort(ranges);
        EXPECT_EQ(ranges[0], std::make_pair(uint64_t{0}, uint64_t{100}));
        EXPECT_EQ(ranges[1], std::make_pair(uint64_t{200}, uint64_t{100}));
    }

    TEST(MacosFileSyscalls, ClosingADescriptorDropsTheLocksItHeld)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        const auto request = write_guest_flock(*emu, sogen::macos_fcntl::MACOS_F_WRLCK, 0, 0, 0x100);
        run_syscall(*emu, 1, mov_x16_fcntl, {fd, static_cast<uint64_t>(sogen::macos_fcntl::MACOS_F_OFD_SETLK), request});
        ASSERT_FALSE(failed(*emu));
        ASSERT_EQ(emu->process.file_locks.size(), 1u);

        run_syscall(*emu, 2, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));
        EXPECT_TRUE(emu->process.file_locks.empty()) << "a closed description holds nothing";
    }

    // libsqlite3 opens every database file through guarded_open_np and closes it only through the
    // guarded call, so CoreData's persistent store is unreachable without the pair.
    TEST(MacosFileSyscalls, GuardedOpenPinsAGuardThatGuardedCloseNamesBack)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto guard = write_guest_guard(*emu, 0xC0FFEE0000000001ULL, 0x100);
        constexpr auto guardflags = sogen::macos_guard::MACOS_GUARD_REQUIRED | sogen::macos_guard::MACOS_GUARD_CLOSE |
                                    sogen::macos_guard::MACOS_GUARD_DUP | sogen::macos_guard::MACOS_GUARD_WRITE;

        run_syscall(
            *emu, 0, mov_x16_guarded_open_np,
            {path, guard, guardflags, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_CLOEXEC), 0});
        ASSERT_FALSE(failed(*emu)) << "guarded_open_np reported errno " << result_of(*emu);

        const auto fd = static_cast<int>(result_of(*emu));
        ASSERT_NE(emu->process.fds.get(fd), nullptr);
        ASSERT_NE(emu->process.guard_of(fd), nullptr);
        EXPECT_EQ(emu->process.guard_of(fd)->id, 0xC0FFEE0000000001ULL);
        EXPECT_EQ(emu->process.guard_of(fd)->flags, guardflags);

        run_syscall(*emu, 1, mov_x16_guarded_close_np, {result_of(*emu), guard});
        EXPECT_FALSE(failed(*emu)) << "guarded_close_np reported errno " << result_of(*emu);
        EXPECT_EQ(emu->process.fds.get(fd), nullptr);
        EXPECT_EQ(emu->process.guard_of(fd), nullptr) << "a guard must not outlive the descriptor it guarded";
    }

    // The three refusals measured against this host's kernel. The O_CLOEXEC one is the surprise: an
    // otherwise valid guarded open without it is EINVAL, because a guard cannot survive an exec.
    TEST(MacosFileSyscalls, GuardedOpenRefusesAGuardTheKernelWouldNotAccept)
    {
        constexpr auto cloexec = static_cast<uint64_t>(sogen::macos_open::MACOS_O_CLOEXEC);

        const std::vector<std::pair<uint64_t, uint64_t>> refusals{
            {sogen::macos_guard::MACOS_GUARD_CLOSE, cloexec},
            {sogen::macos_guard::MACOS_GUARD_REQUIRED | (sogen::macos_guard::MACOS_GUARD_ALL + 1), cloexec},
            {sogen::macos_guard::MACOS_GUARD_REQUIRED, 0},
        };

        for (const auto& [guardflags, open_flags] : refusals)
        {
            const scratch_root root{};
            const auto emu = make_rooted_emulator(root);

            const auto path = write_guest_path(*emu, "/usr/lib/marker");
            const auto guard = write_guest_guard(*emu, 0x1122334455667788ULL, 0x100);

            run_syscall(*emu, 0, mov_x16_guarded_open_np, {path, guard, guardflags, open_flags, 0});
            EXPECT_TRUE(failed(*emu)) << "guardflags " << guardflags << " open flags " << open_flags;
            EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
            EXPECT_TRUE(emu->process.fd_guards.empty());
        }
    }

    // The bit values themselves, pinned against the kernel they were measured from. Reordering them
    // silently turns a close guard into a dup guard, which no functional test would notice.
    TEST(MacosFileSyscalls, TheGuardFlagsAreTheOnesXnuDefines)
    {
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_CLOSE, 0x01u);
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_DUP, 0x02u);
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_SOCKET_IPC, 0x04u);
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_FILEPORT, 0x08u);
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_WRITE, 0x10u);
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_ALL, 0x1Fu);
        EXPECT_EQ(sogen::macos_guard::MACOS_GUARD_REQUIRED, sogen::macos_guard::MACOS_GUARD_DUP)
            << "xnu spends no bit on GUARD_REQUIRED; it requires GUARD_DUP";
    }

    // dpclass and dpflags name an iOS content protection class, which a Mac volume does not have. The
    // descriptor has to come back identical to the one guarded_open_np would have produced.
    TEST(MacosFileSyscalls, GuardedOpenDprotectedIgnoresTheDataProtectionClass)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        const auto guard = write_guest_guard(*emu, 0xABCDEF01ULL, 0x100);
        constexpr auto guardflags = sogen::macos_guard::MACOS_GUARD_REQUIRED | sogen::macos_guard::MACOS_GUARD_CLOSE;

        run_syscall(*emu, 0, mov_x16_guarded_open_dprotected_np,
                    {path, guard, guardflags, static_cast<uint64_t>(sogen::macos_open::MACOS_O_CLOEXEC), 4, 0, 0});
        ASSERT_FALSE(failed(*emu)) << "guarded_open_dprotected_np reported errno " << result_of(*emu);

        const auto fd = static_cast<int>(result_of(*emu));
        ASSERT_NE(emu->process.guard_of(fd), nullptr);
        EXPECT_EQ(emu->process.guard_of(fd)->id, 0xABCDEF01ULL);

        run_syscall(*emu, 1, mov_x16_read, {result_of(*emu), data_base + 0x200, 4});
        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 4u);
    }

    TEST(MacosFileSyscalls, GuardedCloseNeedsTheGuardTheOpenNamed)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        const auto guard = write_guest_guard(*emu, 0x5150ULL, 0x100);
        const auto impostor = write_guest_guard(*emu, 0x5151ULL, 0x110);

        run_syscall(*emu, 0, mov_x16_guarded_open_np,
                    {path, guard, sogen::macos_guard::MACOS_GUARD_REQUIRED | sogen::macos_guard::MACOS_GUARD_CLOSE,
                     static_cast<uint64_t>(sogen::macos_open::MACOS_O_CLOEXEC), 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_guarded_close_np, {fd, impostor});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));
        EXPECT_NE(emu->process.fds.get(static_cast<int>(fd)), nullptr) << "a refused close leaves the descriptor open";

        run_syscall(*emu, 2, mov_x16_guarded_close_np, {fd, guard});
        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(emu->process.fds.get(static_cast<int>(fd)), nullptr);
    }

    // xnu answers EINVAL before it ever compares ids: an unguarded descriptor has nothing to compare
    // against, and reporting EPERM would tell the caller its id was wrong when it had none.
    TEST(MacosFileSyscalls, GuardedCloseOfAnUnguardedDescriptorIsEinval)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/marker");
        const auto guard = write_guest_guard(*emu, 0x99ULL, 0x100);

        run_syscall(*emu, 0, mov_x16_open, {path, 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_guarded_close_np, {fd, guard});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
        EXPECT_NE(emu->process.fds.get(static_cast<int>(fd)), nullptr);
    }

    // The three operations a guard names, reached through the plain syscall. xnu answers each with a
    // fatal EXC_GUARD_FD rather than an errno, so the emulator's equivalent is to end the run and say
    // which call did it -- a guest that violates a guard it set itself has a bug worth reporting.
    TEST(MacosFileSyscalls, APlainCallOnAGuardedDescriptorEndsTheRun)
    {
        struct violation
        {
            uint32_t operation{};
            uint32_t syscall{};
            std::vector<uint64_t> extra_arguments{};
        };

        const std::vector<violation> violations{
            {sogen::macos_guard::MACOS_GUARD_CLOSE, mov_x16_close, {}},
            {sogen::macos_guard::MACOS_GUARD_DUP, mov_x16_dup, {}},
            {sogen::macos_guard::MACOS_GUARD_WRITE, mov_x16_write, {data_base + 0x200, 2}},
        };

        for (const auto& attempt : violations)
        {
            const scratch_root root{};
            const auto emu = make_rooted_emulator(root);

            const auto path = write_guest_path(*emu, "/usr/lib/writable");
            const auto guard = write_guest_guard(*emu, 0xDEADBEEFULL, 0x100);
            write_guest_bytes(*emu, "XY", 0x200);

            run_syscall(*emu, 0, mov_x16_guarded_open_np,
                        {path, guard, sogen::macos_guard::MACOS_GUARD_REQUIRED | attempt.operation,
                         static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_CLOEXEC), 0});
            ASSERT_FALSE(failed(*emu)) << "guarded_open_np reported errno " << result_of(*emu);

            std::vector<uint64_t> arguments{result_of(*emu)};
            arguments.insert(arguments.end(), attempt.extra_arguments.begin(), attempt.extra_arguments.end());

            run_syscall(*emu, 1, attempt.syscall, arguments);

            EXPECT_TRUE(failed(*emu)) << "guardflags " << attempt.operation;
            EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));
            EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::syscall_exception) << emu->last_stop_detail();
            EXPECT_NE(emu->last_stop_detail().find("guarded_open_np"), std::string::npos) << emu->last_stop_detail();
        }
    }

    // The same operations are fine when the guard does not name them; only the bits the open asked for
    // are enforced.
    TEST(MacosFileSyscalls, AGuardOnlyStopsTheOperationsItNames)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto guard = write_guest_guard(*emu, 0x7777ULL, 0x100);
        write_guest_bytes(*emu, "XY", 0x200);

        run_syscall(*emu, 0, mov_x16_guarded_open_np,
                    {path, guard, sogen::macos_guard::MACOS_GUARD_REQUIRED | sogen::macos_guard::MACOS_GUARD_DUP,
                     static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_CLOEXEC), 0});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_write, {fd, data_base + 0x200, 2});
        EXPECT_FALSE(failed(*emu)) << "GUARD_DUP says nothing about writing";
        EXPECT_EQ(result_of(*emu), 2u);

        run_syscall(*emu, 2, mov_x16_close, {fd});
        EXPECT_FALSE(failed(*emu)) << "GUARD_DUP says nothing about closing";
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::syscall_exception) << emu->last_stop_detail();
    }

    TEST(MacosFileSyscalls, GuardedWriteAndGuardedPwriteReachTheFile)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto guard = write_guest_guard(*emu, 0x2468ULL, 0x100);
        const auto impostor = write_guest_guard(*emu, 0x2469ULL, 0x110);
        const auto payload = write_guest_bytes(*emu, "abcXY", 0x200);

        run_syscall(*emu, 0, mov_x16_guarded_open_np,
                    {path, guard, sogen::macos_guard::MACOS_GUARD_REQUIRED | sogen::macos_guard::MACOS_GUARD_WRITE,
                     static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR | sogen::macos_open::MACOS_O_CLOEXEC), 0});
        ASSERT_FALSE(failed(*emu)) << "guarded_open_np reported errno " << result_of(*emu);
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_guarded_write_np, {fd, impostor, payload, 3});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));

        run_syscall(*emu, 2, mov_x16_guarded_write_np, {fd, guard, payload, 3});
        ASSERT_FALSE(failed(*emu)) << "guarded_write_np reported errno " << result_of(*emu);
        EXPECT_EQ(result_of(*emu), 3u);

        run_syscall(*emu, 3, mov_x16_guarded_pwrite_np, {fd, guard, payload + 3, 2, 10});
        ASSERT_FALSE(failed(*emu)) << "guarded_pwrite_np reported errno " << result_of(*emu);
        EXPECT_EQ(result_of(*emu), 2u);

        // pwrite must not move the descriptor's own offset, so this lands at 3 rather than at 12.
        run_syscall(*emu, 4, mov_x16_guarded_write_np, {fd, guard, payload + 4, 1});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 5, mov_x16_guarded_close_np, {fd, guard});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "abcYER-ORIXYNAL-CONTENT");
    }

    TEST(MacosFileSyscalls, PwriteWritesAtAnOffsetWithoutMovingTheDescriptor)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto payload = write_guest_bytes(*emu, "abcXYZ", 0x200);

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_write, {fd, payload, 3});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 2, mov_x16_pwrite, {fd, payload + 3, 2, 10});
        ASSERT_FALSE(failed(*emu)) << "pwrite reported errno " << result_of(*emu);
        EXPECT_EQ(result_of(*emu), 2u);

        run_syscall(*emu, 3, mov_x16_write, {fd, payload + 5, 1});
        ASSERT_FALSE(failed(*emu));

        run_syscall(*emu, 4, mov_x16_close, {fd});
        ASSERT_FALSE(failed(*emu));

        EXPECT_EQ(read_host_file(root.path / "usr" / "lib" / "writable"), "abcZER-ORIXYNAL-CONTENT");
    }

    TEST(MacosFileSyscalls, PwriteRefusesANegativeOffsetAndAStream)
    {
        const scratch_root root{};
        const auto emu = make_rooted_emulator(root);

        const auto path = write_guest_path(*emu, "/usr/lib/writable");
        const auto payload = write_guest_bytes(*emu, "ab", 0x200);

        run_syscall(*emu, 0, mov_x16_open, {path, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDWR)});
        ASSERT_FALSE(failed(*emu));
        const auto fd = result_of(*emu);

        run_syscall(*emu, 1, mov_x16_pwrite, {fd, payload, 2, static_cast<uint64_t>(-1)});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));

        run_syscall(*emu, 2, mov_x16_pwrite, {1, payload, 2, 0});
        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_ESPIPE)) << "stdout has no offset to write at";
    }
}

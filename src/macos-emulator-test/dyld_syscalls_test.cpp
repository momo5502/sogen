#include <gtest/gtest.h>

#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"
#include <macos_symbolizer.hpp>
#include <module/dyld_shared_cache.hpp>
#include "dyld_fixture.hpp"

#include <macos_file_identity.hpp>
#include <cstddef>
#include <fstream>
#include <array>
#include <macos_syscall_utils.hpp>
#include <macos_stat.hpp>
#include <serialization.hpp>

namespace
{
    TEST(MacosFileIdentity, PacksTheFsidTheWayTheKernelPrintsIt)
    {
        sogen::macos_file_identity identity{};
        identity.object_id = 0xB426014ull;

        // The kernel renders apple[]'s executable_file= as one %llx of fsid.val[1]<<32 | fsid.val[0];
        // the measured value on build 25G76 is 0x1a0100000f.
        EXPECT_EQ(identity.packed_fsid(), 0x1A0100000Full);
    }

    TEST(MacosFileIdentity, TheSamePathAlwaysGetsTheSameObjectId)
    {
        sogen::macos_file_identity_table table{};

        const auto first = table.acquire("/usr/lib/dyld");
        const auto again = table.acquire("/usr/lib/dyld");
        const auto other = table.acquire("/bin/hello");

        EXPECT_EQ(first.object_id, again.object_id);
        EXPECT_NE(first.object_id, other.object_id);
        EXPECT_NE(first.object_id, 0u);
        EXPECT_EQ(table.size(), 2u);
    }

    TEST(MacosFileIdentity, ResolvesAnIdentityBackToItsGuestPath)
    {
        sogen::macos_file_identity_table table{};
        const auto identity = table.acquire("/usr/lib/dyld");

        const auto resolved = table.resolve(identity.fsid_dev, identity.fsid_vfstype, identity.object_id);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*resolved, "/usr/lib/dyld");

        EXPECT_FALSE(table.resolve(identity.fsid_dev, identity.fsid_vfstype, identity.object_id + 1).has_value());
        EXPECT_FALSE(table.resolve(identity.fsid_dev + 1, identity.fsid_vfstype, identity.object_id).has_value());
    }

    TEST(MacosFileIdentity, SurvivesASerializationRoundTrip)
    {
        sogen::macos_file_identity_table table{};
        const auto identity = table.acquire("/usr/lib/dyld");

        sogen::utils::buffer_serializer serializer{};
        table.serialize(serializer);

        sogen::macos_file_identity_table restored{};
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored.deserialize(deserializer);

        ASSERT_EQ(restored.size(), 1u);
        const auto resolved = restored.resolve(identity.fsid_dev, identity.fsid_vfstype, identity.object_id);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*resolved, "/usr/lib/dyld");
        EXPECT_EQ(restored.acquire("/bin/hello").object_id, identity.object_id + 1);
    }

    TEST(DyldSyscalls, StructureSizesMatchTheDarwinSdk)
    {
        EXPECT_EQ(sizeof(sogen::macos_attrlist), 24u);
        EXPECT_EQ(sizeof(sogen::macos_statfs64), 2168u);
        EXPECT_EQ(offsetof(sogen::macos_statfs64, f_fsid), 48u);
        EXPECT_EQ(offsetof(sogen::macos_statfs64, f_fstypename), 72u);
        EXPECT_EQ(offsetof(sogen::macos_statfs64, f_mntonname), 88u);
        EXPECT_EQ(offsetof(sogen::macos_statfs64, f_mntfromname), 1112u);
    }

    TEST(DyldSyscalls, FsgetpathTurnsAnIdentityBackIntoAGuestPath)
    {
        const auto emu = macos_test::make_emulator();
        const auto identity = emu->identities.acquire("/usr/lib/dyld");

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::array<int32_t, 2> fsid{static_cast<int32_t>(identity.fsid_dev), static_cast<int32_t>(identity.fsid_vfstype)};
        emu->memory.write_memory(scratch + 0x800, fsid.data(), sizeof(fsid));

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(427, {scratch, 1024, scratch + 0x800, identity.object_id}));
        emu->start(16);

        const auto written = emu->emu().reg(sogen::arm64_register::x0);
        ASSERT_EQ(written, std::string_view{"/usr/lib/dyld"}.size() + 1);

        std::array<char, 32> path{};
        emu->memory.read_memory(scratch, path.data(), path.size());
        EXPECT_STREQ(path.data(), "/usr/lib/dyld");
    }

    TEST(DyldSyscalls, FsgetpathRejectsAnUnknownObjectId)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::array<int32_t, 2> fsid{static_cast<int32_t>(sogen::MACOS_SYNTHETIC_FSID_DEV),
                                          static_cast<int32_t>(sogen::MACOS_SYNTHETIC_FSID_VFSTYPE)};
        emu->memory.write_memory(scratch + 0x800, fsid.data(), sizeof(fsid));

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(427, {scratch, 1024, scratch + 0x800, 0xDEAD}));
        emu->start(16);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOENT));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
    }

    TEST(DyldSyscalls, GetfsstatReportsOneApfsVolume)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE * 2, sogen::memory_permission::read_write);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(347, {0, 0, 2}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 1u);

        macos_test::write_guest_code(*emu, 0x320000000ULL, macos_test::syscall_sequence(347, {scratch, sizeof(sogen::macos_statfs64), 2}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 1u);

        sogen::macos_statfs64 fs{};
        emu->memory.read_memory(scratch, &fs, sizeof(fs));
        EXPECT_STREQ(fs.f_fstypename.data(), "apfs");
        EXPECT_STREQ(fs.f_mntonname.data(), "/");
        EXPECT_EQ(static_cast<uint32_t>(fs.f_fsid[0]), sogen::MACOS_SYNTHETIC_FSID_DEV);
    }

    TEST(DyldSyscalls, GetattrlistReturnsTheIdentityAttributesInBitmapOrder)
    {
        const sogen::test::temp_directory dir{"getattrlist"};
        const auto file = dir.path() / "image";
        {
            std::ofstream stream{file, std::ios::binary};
            stream << "x";
        }

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string guest_path = "/image";
        emu->memory.write_memory(scratch, guest_path.c_str(), guest_path.size() + 1);

        sogen::macos_attrlist request{};
        request.bitmapcount = 5;
        request.commonattr = sogen::macos_attr::CMN_RETURNED_ATTRS | sogen::macos_attr::CMN_FSID | sogen::macos_attr::CMN_OBJTYPE |
                             sogen::macos_attr::CMN_FILEID;
        emu->memory.write_memory(scratch + 0x100, &request, sizeof(request));

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(220, {scratch, scratch + 0x100, scratch + 0x200, 256, 0}));
        emu->start(16);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        uint32_t length{};
        emu->memory.read_memory(scratch + 0x200, &length, sizeof(length));
        EXPECT_EQ(length, 4u + 20u + 8u + 4u + 8u);

        // dyld reads this bitmap to learn which attributes actually followed; a zeroed one makes it
        // skip every value the reply carries.
        std::array<uint32_t, 5> returned{};
        emu->memory.read_memory(scratch + 0x200 + 4, returned.data(), sizeof(returned));
        EXPECT_EQ(returned[0], request.commonattr);
        EXPECT_EQ(returned[1], 0u);

        uint32_t fsid_dev{};
        emu->memory.read_memory(scratch + 0x200 + 4 + 20, &fsid_dev, sizeof(fsid_dev));
        EXPECT_EQ(fsid_dev, sogen::MACOS_SYNTHETIC_FSID_DEV);

        uint32_t objtype{};
        emu->memory.read_memory(scratch + 0x200 + 4 + 20 + 8, &objtype, sizeof(objtype));
        EXPECT_EQ(objtype, sogen::macos_attr::OBJ_TYPE_VREG);

        uint64_t file_id{};
        emu->memory.read_memory(scratch + 0x200 + 4 + 20 + 8 + 4, &file_id, sizeof(file_id));
        EXPECT_EQ(file_id, emu->identities.acquire("/image").object_id);
    }

    // dyld asks for ATTR_CMN_FULLPATH on the executable while it works out what it is running. Refusing
    // it makes dyld halt before a single initialiser runs, which is what stopped every CoreFoundation
    // binary: the path is not optional, and a variable-length attribute is not encoded like a fixed one.
    TEST(DyldSyscalls, GetattrlistReturnsTheFullPathAsAReference)
    {
        const sogen::test::temp_directory dir{"getattrlist-fullpath"};
        std::filesystem::create_directories(dir.path() / "bin");
        {
            std::ofstream stream{dir.path() / "bin" / "tool", std::ios::binary};
            stream << "x";
        }

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string guest_path = "/bin/tool";
        emu->memory.write_memory(scratch, guest_path.c_str(), guest_path.size() + 1);

        sogen::macos_attrlist request{};
        request.bitmapcount = 5;
        request.commonattr = sogen::macos_attr::CMN_RETURNED_ATTRS | sogen::macos_attr::CMN_FULLPATH;
        emu->memory.write_memory(scratch + 0x100, &request, sizeof(request));

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(220, {scratch, scratch + 0x100, scratch + 0x200, 512, 0}));
        emu->start(16);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        std::array<uint32_t, 5> returned{};
        emu->memory.read_memory(scratch + 0x200 + 4, returned.data(), sizeof(returned));
        EXPECT_EQ(returned[0], request.commonattr) << "the reply must say the path is present";

        // attrreference_t: an offset measured from the reference's own address, and a length that counts
        // the terminating NUL. Anything else and the caller reads from the wrong place.
        const auto reference_at = scratch + 0x200 + 4 + 20;
        int32_t data_offset{};
        uint32_t data_length{};
        emu->memory.read_memory(reference_at, &data_offset, sizeof(data_offset));
        emu->memory.read_memory(reference_at + 4, &data_length, sizeof(data_length));

        EXPECT_EQ(data_length, guest_path.size() + 1);
        EXPECT_GT(data_offset, 0) << "the string follows the fixed fields";

        std::vector<char> path(data_length);
        emu->memory.read_memory(reference_at + static_cast<uint64_t>(data_offset), path.data(), path.size());
        EXPECT_STREQ(path.data(), guest_path.c_str());

        uint32_t total{};
        emu->memory.read_memory(scratch + 0x200, &total, sizeof(total));
        EXPECT_EQ(total, 4u + 20u + 8u + data_length) << "the length has to cover the variable data too";
    }

    TEST(DyldSyscalls, GetattrlistRefusesAFullPathThatDoesNotFit)
    {
        const sogen::test::temp_directory dir{"getattrlist-fullpath-small"};
        std::filesystem::create_directories(dir.path() / "bin");
        {
            std::ofstream stream{dir.path() / "bin" / "tool", std::ios::binary};
            stream << "x";
        }

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string guest_path = "/bin/tool";
        emu->memory.write_memory(scratch, guest_path.c_str(), guest_path.size() + 1);

        sogen::macos_attrlist request{};
        request.bitmapcount = 5;
        request.commonattr = sogen::macos_attr::CMN_RETURNED_ATTRS | sogen::macos_attr::CMN_FULLPATH;
        emu->memory.write_memory(scratch + 0x100, &request, sizeof(request));

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(220, {scratch, scratch + 0x100, scratch + 0x200, 28, 0}));
        emu->start(16);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u)
            << "a buffer too small for the string must fail rather than truncate it";
    }

    TEST(DyldSyscalls, GetattrlistRefusesAnAttributeItCannotEncode)
    {
        const sogen::test::temp_directory dir{"getattrlist-unsupported"};
        const auto file = dir.path() / "image";
        {
            std::ofstream stream{file, std::ios::binary};
            stream << "x";
        }

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string guest_path = "/image";
        emu->memory.write_memory(scratch, guest_path.c_str(), guest_path.size() + 1);

        // An ACL blob. It is variable-length, its contents are the volume's own security store, and a
        // synthetic root has none -- so it is refused rather than answered with an empty one, which a
        // caller would read as "this file has no access control" instead of "this was not answered".
        sogen::macos_attrlist request{};
        request.bitmapcount = 5;
        request.commonattr = 0x00400000u; // ATTR_CMN_EXTENDED_SECURITY
        emu->memory.write_memory(scratch + 0x100, &request, sizeof(request));

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(220, {scratch, scratch + 0x100, scratch + 0x200, 256, 0}));
        emu->start(16);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
    }

    // The request AppKit's process registration makes, verbatim. Its two variable-length attributes are
    // what an attrreference gets wrong most easily: the offset is measured from the reference's own
    // address, and the bytes of every reference follow the whole fixed section in reference order.
    TEST(DyldSyscalls, GetattrlistEncodesTheNameAndPathAppKitAsksFor)
    {
        const sogen::test::temp_directory dir{"getattrlist-appkit"};
        const auto file = dir.path() / "image";
        {
            std::ofstream stream{file, std::ios::binary};
            stream << "x";
        }

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string guest_path = "/image";
        emu->memory.write_memory(scratch, guest_path.c_str(), guest_path.size() + 1);

        sogen::macos_attrlist request{};
        request.bitmapcount = 5;
        request.commonattr = sogen::macos_attr::CMN_RETURNED_ATTRS | sogen::macos_attr::CMN_NAME | sogen::macos_attr::CMN_OBJTYPE |
                             sogen::macos_attr::CMN_FULLPATH;
        request.fileattr = sogen::macos_attr::FILE_LINKCOUNT;
        request.forkattr = sogen::macos_attr::CMNEXT_REALDEVID | sogen::macos_attr::CMNEXT_EXT_FLAGS;
        emu->memory.write_memory(scratch + 0x100, &request, sizeof(request));

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(220, {scratch, scratch + 0x100, scratch + 0x200, 512, 0}));
        emu->start(16);
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        std::array<uint8_t, 512> encoded{};
        ASSERT_TRUE(emu->memory.try_read_memory(scratch + 0x200, encoded.data(), encoded.size()));

        const auto word_at = [&encoded](const size_t offset) {
            uint32_t value = 0;
            std::memcpy(&value, encoded.data() + offset, sizeof(value));
            return value;
        };

        const auto string_at = [&encoded, &word_at](const size_t reference) {
            int32_t offset = 0;
            std::memcpy(&offset, encoded.data() + reference, sizeof(offset));
            const auto length = word_at(reference + 4);
            return std::string{reinterpret_cast<const char*>(encoded.data()) + reference + offset, length - 1};
        };

        // total length, then the returned-attributes set, then the two references, then the fixed
        // attributes of each following group.
        EXPECT_EQ(word_at(4), request.commonattr);
        EXPECT_EQ(word_at(8), 0u) << "no volume attributes were asked for";
        EXPECT_EQ(word_at(16), request.fileattr);
        EXPECT_EQ(word_at(20), request.forkattr);
        EXPECT_EQ(string_at(4 + 20), "image");
        EXPECT_EQ(word_at(4 + 20 + 8), sogen::macos_attr::OBJ_TYPE_VREG);
        EXPECT_EQ(string_at(4 + 20 + 8 + 4), guest_path);
        EXPECT_EQ(word_at(4 + 20 + 8 + 4 + 8), 1u) << "one link";
    }

    TEST(DyldSyscalls, TheRefusedSyscallsFailTheWayARealSipEnabledMacDoes)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(483, {0, 0, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EPERM));

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(550, {0, 0, 0, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOSYS));

        macos_test::write_guest_code(*emu, 0x320000000ULL, macos_test::syscall_sequence(489, {0, 0, 0, 0, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOSYS));

        macos_test::write_guest_code(*emu, 0x330000000ULL, macos_test::syscall_sequence(38, {0, 0, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
    }

    TEST(DyldSyscalls, FsgetpathRefusesABufferTooSmallForThePath)
    {
        const auto emu = macos_test::make_emulator();
        const auto identity = emu->identities.acquire("/usr/lib/dyld");

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::array<int32_t, 2> fsid{static_cast<int32_t>(identity.fsid_dev), static_cast<int32_t>(identity.fsid_vfstype)};
        emu->memory.write_memory(scratch + 0x800, fsid.data(), sizeof(fsid));

        const std::array<char, 4> poison{'#', '#', '#', '#'};
        emu->memory.write_memory(scratch, poison.data(), poison.size());

        macos_test::write_guest_code(*emu, 0x310000000ULL,
                                     macos_test::syscall_sequence(427, {scratch, 4, scratch + 0x800, identity.object_id}));
        emu->start(16);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOSPC));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);

        std::array<char, 4> after{};
        emu->memory.read_memory(scratch, after.data(), after.size());
        EXPECT_EQ(after, poison) << "a refused fsgetpath must not have written past the caller's buffer";
    }

    // The libSystem syscalls beyond dyld's measured set are not enumerated anywhere. A silent ENOSYS
    // turns each one into an unexplained crash hundreds of thousands of instructions later, so the run
    // halts and names the syscall and the module that issued it instead.
    TEST(DyldBringUpReporter, AnUnregisteredSyscallHaltsWithAnAttributedDetail)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(511, {}));
        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unimplemented_syscall);
        EXPECT_NE(emu->last_stop_detail().find("bsd 511"), std::string::npos) << emu->last_stop_detail();
        EXPECT_NE(emu->last_stop_detail().find("at 0x3000000"), std::string::npos)
            << "the detail names where the call came from: " << emu->last_stop_detail();

        // The guest still sees Darwin's ENOSYS contract; the halt is in addition to it, not instead.
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOSYS));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
    }

    TEST(DyldBringUpReporter, AnUnregisteredMachTrapIsNamedAsATrapNotAsABsdCall)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(-13, {}));
        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unimplemented_syscall);
        EXPECT_NE(emu->last_stop_detail().find("mach trap 13"), std::string::npos) << emu->last_stop_detail();
    }

    TEST(DyldBringUpReporter, DescribesAnAddressInsideAMappedModule)
    {
        const std::filesystem::path host_dyld{MACOS_DYLD_HOST_PATH};
        if (!std::filesystem::is_regular_file(host_dyld))
        {
            GTEST_SKIP() << "no host /usr/lib/dyld";
        }

        const sogen::test::temp_directory scratch{"symbolize"};

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = MACOS_DYLD_HOST_PATH;
        executable.code = {0xD4200000u};
        macos_test::write_image(scratch.path() / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), std::filesystem::path{"/"});
        ASSERT_TRUE(emu->load_dyld_application(scratch.path() / "hello", {"/bin/hello"}, {})) << emu->last_stop_detail();

        const auto address = emu->mod_manager.dylinker->image_base + 0x49c0;
        const auto origin = emu->symbolizer.describe(address);
        ASSERT_TRUE(origin.has_value());
        EXPECT_EQ(origin->offset, 0x49c0u);
        EXPECT_FALSE(origin->in_shared_cache);
        EXPECT_FALSE(origin->module.empty());

        EXPECT_NE(emu->symbolizer.format(address).find("+0x49c0"), std::string::npos) << emu->symbolizer.format(address);
        EXPECT_FALSE(emu->symbolizer.describe(0x7000000000000000ull).has_value());
    }

    TEST(DyldBringUpReporter, DescribesAnAddressInsideTheSharedCache)
    {
        const std::filesystem::path cache{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::is_regular_file(cache))
        {
            GTEST_SKIP() << "no host dyld shared cache";
        }

        sogen::macos_symbolizer symbolizer{};
        ASSERT_TRUE(symbolizer.attach_shared_cache(cache));
        ASSERT_TRUE(symbolizer.has_shared_cache());

        const auto origin = symbolizer.describe(0x180114000ull);
        ASSERT_TRUE(origin.has_value());
        EXPECT_TRUE(origin->in_shared_cache);
        EXPECT_FALSE(origin->module.empty());

        EXPECT_NE(symbolizer.format(0x180114000ull).find("shared cache"), std::string::npos);

        // The span bound is what stops every unmapped address being attributed to the cache, so it has
        // to be checked against the real end rather than against an address far outside it.
        const auto reader = sogen::dyld_shared_cache_reader::open(cache);
        const auto span_end = reader.shared_region_start() + reader.shared_region_size();
        EXPECT_TRUE(symbolizer.describe(span_end - 1).has_value());
        EXPECT_FALSE(symbolizer.describe(span_end).has_value()) << "one byte past the cache is not in the cache";
    }

    TEST(DyldBringUpReporter, AMissingSharedCacheIsRefusedRatherThanThrown)
    {
        sogen::macos_symbolizer symbolizer{};
        EXPECT_FALSE(symbolizer.attach_shared_cache("/nonexistent/dyld_shared_cache_arm64e"));
        EXPECT_FALSE(symbolizer.has_shared_cache());
        EXPECT_FALSE(symbolizer.describe(0x180114000ull).has_value());
    }

    // dyld opens /dev/null to reserve descriptors 0, 1 and 2 and calls abort_with_payload
    // ("failed to reserve stdin descriptor: /dev/null: 6") if it cannot. Opening the host character
    // device would park the emulator in a blocking read, so it gets an empty memory file instead.
    TEST(DyldSyscalls, TheNullDeviceReadsEofAndDiscardsWrites)
    {
        const sogen::test::temp_directory dir{"devnull"};
        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string path = "/dev/null";
        emu->memory.write_memory(scratch, path.c_str(), path.size() + 1);

        const std::string payload = "discard me";
        emu->memory.write_memory(scratch + 0x100, payload.c_str(), payload.size());

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(5, {scratch, 2}));
        emu->start(16);

        const auto fd = emu->emu().reg(sogen::arm64_register::x0);
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << "open(/dev/null) must succeed";
        ASSERT_GE(fd, 3u);

        macos_test::write_guest_code(*emu, 0x320000000ULL, macos_test::syscall_sequence(4, {fd, scratch + 0x100, payload.size()}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), payload.size()) << "a write to /dev/null reports the whole length";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);

        macos_test::write_guest_code(*emu, 0x330000000ULL, macos_test::syscall_sequence(3, {fd, scratch + 0x200, 16}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "a read of /dev/null is immediate EOF";

        // Discarded, not accumulated: a guest that writes forever must not grow the emulator's heap.
        const auto* entry = emu->process.fds.get(static_cast<int>(fd));
        ASSERT_NE(entry, nullptr);
        ASSERT_NE(entry->memory_file, nullptr);
        EXPECT_TRUE(entry->memory_file->content.empty());
    }

    TEST(DyldSyscalls, SigprocmaskAccumulatesAndReportsThePreviousMask)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const uint32_t first = 0x00000005;
        emu->memory.write_memory(scratch, &first, sizeof(first));

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(48, {1, scratch, scratch + 0x100}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->process.signal_mask, first);

        uint32_t previous{};
        emu->memory.read_memory(scratch + 0x100, &previous, sizeof(previous));
        EXPECT_EQ(previous, 0u) << "the mask before the first block was empty";

        const uint32_t second = 0x00000002;
        emu->memory.write_memory(scratch + 0x200, &second, sizeof(second));

        macos_test::write_guest_code(*emu, 0x320000000ULL, macos_test::syscall_sequence(48, {1, scratch + 0x200, scratch + 0x300}));
        emu->start(16);
        EXPECT_EQ(emu->process.signal_mask, first | second) << "SIG_BLOCK unions rather than replaces";

        emu->memory.read_memory(scratch + 0x300, &previous, sizeof(previous));
        EXPECT_EQ(previous, first) << "dyld restores what it was told, so oldset has to be the real mask";

        macos_test::write_guest_code(*emu, 0x330000000ULL, macos_test::syscall_sequence(48, {3, scratch + 0x200, 0}));
        emu->start(16);
        EXPECT_EQ(emu->process.signal_mask, second) << "SIG_SETMASK replaces";

        macos_test::write_guest_code(*emu, 0x340000000ULL, macos_test::syscall_sequence(48, {99, scratch, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
    }

    namespace shared_region
    {
        constexpr size_t FILE_ENTRY_SIZE = 12;
        constexpr size_t MAPPING_ENTRY_SIZE = 48;

        void write_file_entry(sogen::macos_emulator& emu, const uint64_t address, const int32_t fd, const uint32_t count,
                              const uint32_t slide)
        {
            emu.memory.write_memory(address, &fd, sizeof(fd));
            emu.memory.write_memory(address + 4, &count, sizeof(count));
            emu.memory.write_memory(address + 8, &slide, sizeof(slide));
        }

        void write_mapping(sogen::macos_emulator& emu, const uint64_t address, const uint64_t target, const uint64_t size,
                           const uint64_t file_offset, const int32_t protection)
        {
            const std::array<uint64_t, 5> words{target, size, file_offset, 0, 0};
            emu.memory.write_memory(address, words.data(), words.size() * sizeof(uint64_t));
            emu.memory.write_memory(address + 40, &protection, sizeof(protection));
            emu.memory.write_memory(address + 44, &protection, sizeof(protection));
        }
    }

    // dyld mmaps every subcache to read it and then asks the kernel to install them all at the
    // addresses the cache was built for. Refusing this call is not an option that leaves the cache
    // usable: dyld falls straight back to searching the filesystem and reports "no dyld cache".
    TEST(DyldSyscalls, SharedRegionInstallMapsEachFileWhereTheCacheWantsIt)
    {
        const sogen::test::temp_directory dir{"shared-region"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE) * 2, 0x00);
        content[0] = 0xAB;
        content[static_cast<size_t>(sogen::MACOS_PAGE_SIZE)] = 0xCD;

        const auto path = dir.path() / "cache";
        {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        }

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        constexpr uint64_t target = 0x380000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        sogen::guest_fd fd{};
        fd.host_path = path.string();
        fd.guest_path = "/cache";
        fd.type = sogen::fd_type::file;
        const auto guest_fd = emu->process.fds.allocate(std::move(fd));
        ASSERT_GE(guest_fd, 0);

        // Two mappings from one descriptor, plus a reservation with no file behind it -- the shape the
        // real cache uses, where the last descriptor is -1 and holds the tail of the region.
        shared_region::write_file_entry(*emu, scratch, static_cast<int32_t>(guest_fd), 2, 0);
        shared_region::write_file_entry(*emu, scratch + shared_region::FILE_ENTRY_SIZE, -1, 1, 0);

        constexpr uint64_t maps = scratch + 0x100;
        shared_region::write_mapping(*emu, maps, target, sogen::MACOS_PAGE_SIZE, 0, 5);
        shared_region::write_mapping(*emu, maps + shared_region::MAPPING_ENTRY_SIZE, target + sogen::MACOS_PAGE_SIZE,
                                     sogen::MACOS_PAGE_SIZE, sogen::MACOS_PAGE_SIZE, 0x63);
        shared_region::write_mapping(*emu, maps + 2 * shared_region::MAPPING_ENTRY_SIZE, target + 2 * sogen::MACOS_PAGE_SIZE,
                                     sogen::MACOS_PAGE_SIZE, 0, 3);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(536, {2, scratch, 3, maps}));
        emu->start(24);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        uint8_t first{};
        uint8_t second{};
        emu->memory.read_memory(target, &first, sizeof(first));
        emu->memory.read_memory(target + sogen::MACOS_PAGE_SIZE, &second, sizeof(second));
        EXPECT_EQ(first, 0xABu);
        EXPECT_EQ(second, 0xCDu) << "the second mapping reads from its own file offset";

        // 0x63 is not a protection: it is read/write plus the slide and no-auth flags the cache marks
        // its data regions with, and only the low three bits describe the page.
        EXPECT_TRUE(emu->memory.try_write_memory(target + sogen::MACOS_PAGE_SIZE, &first, sizeof(first)));

        uint8_t reserved{};
        EXPECT_TRUE(emu->memory.try_read_memory(target + 2 * sogen::MACOS_PAGE_SIZE, &reserved, sizeof(reserved)))
            << "the reservation has no file behind it but still has to be memory";

        EXPECT_EQ(emu->process.shared_region_base, target);
    }

    // sf_slide is not an address delta. On a real cache the first descriptor carries a non-zero value
    // there while every other carries zero, and adding it relocates the cache header out from under the
    // pointer dyld is already holding -- which it then reads as garbage rather than as a header.
    TEST(DyldSyscalls, SharedRegionInstallIgnoresTheDescriptorSlide)
    {
        const sogen::test::temp_directory dir{"shared-region-slide"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x00);
        content[0] = 0x5A;

        const auto path = dir.path() / "cache";
        {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        }

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        constexpr uint64_t target = 0x380000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        sogen::guest_fd fd{};
        fd.host_path = path.string();
        fd.guest_path = "/cache";
        fd.type = sogen::fd_type::file;
        const auto guest_fd = emu->process.fds.allocate(std::move(fd));
        ASSERT_GE(guest_fd, 0);

        shared_region::write_file_entry(*emu, scratch, static_cast<int32_t>(guest_fd), 1, 0x10000000);

        constexpr uint64_t maps = scratch + 0x100;
        shared_region::write_mapping(*emu, maps, target, sogen::MACOS_PAGE_SIZE, 0, 5);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(536, {1, scratch, 1, maps}));
        emu->start(24);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << emu->last_stop_detail();

        uint8_t marker{};
        ASSERT_TRUE(emu->memory.try_read_memory(target, &marker, sizeof(marker))) << "the mapping belongs at sms_address";
        EXPECT_EQ(marker, 0x5Au);

        EXPECT_FALSE(emu->memory.try_read_memory(target + 0x10000000, &marker, sizeof(marker)))
            << "nothing may be installed at sms_address + sf_slide";
        EXPECT_EQ(emu->process.shared_region_base, target);
    }

    TEST(DyldSyscalls, SharedRegionInstallRefusesAMappingThatIsNotOne)
    {
        const sogen::test::temp_directory dir{"shared-region-bad"};
        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        shared_region::write_file_entry(*emu, scratch, -1, 1, 0);

        constexpr uint64_t maps = scratch + 0x100;
        shared_region::write_mapping(*emu, maps, 0x380000123ULL, sogen::MACOS_PAGE_SIZE, 0, 5);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(536, {1, scratch, 1, maps}));
        emu->start(24);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL))
            << "an unaligned address is not a mapping, and aliasing file bytes there would be silent";
        EXPECT_EQ(emu->process.shared_region_base, 0u);
    }

    // dyld asks whether a shared region exists and treats the failure as "this process has no cache",
    // so the answer has to change once one has been installed.
    TEST(DyldSyscalls, SharedRegionCheckReportsNothingUntilARegionIsInstalled)
    {
        const sogen::test::temp_directory dir{"shared-region-check"};
        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(294, {scratch}));
        emu->start(16);
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << "no region yet";

        emu->process.shared_region_base = 0x180000000ULL;

        macos_test::write_guest_code(*emu, 0x320000000ULL, macos_test::syscall_sequence(294, {scratch}));
        emu->start(16);
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        uint64_t reported{};
        emu->memory.read_memory(scratch, &reported, sizeof(reported));
        EXPECT_EQ(reported, 0x180000000ULL) << "dyld builds every cache-relative address from this";
    }

    namespace shared_region
    {
        constexpr uint64_t VALUE_ADD = 0x180000000ULL;

        uint64_t regular_entry(const uint64_t runtime_offset, const uint32_t next, const uint8_t high8)
        {
            return runtime_offset | (static_cast<uint64_t>(high8) << 34) | (static_cast<uint64_t>(next) << 52);
        }

        uint64_t auth_entry(const uint64_t runtime_offset, const uint32_t next, const uint16_t diversity, const bool address_div,
                            const bool key_is_data)
        {
            return runtime_offset | (static_cast<uint64_t>(diversity) << 34) | (static_cast<uint64_t>(address_div ? 1 : 0) << 50) |
                   (static_cast<uint64_t>(key_is_data ? 1 : 0) << 51) | (static_cast<uint64_t>(next) << 52) | (uint64_t{1} << 63);
        }
    }

    // "and_slide" is the second half of the syscall's name and the reason dyld can use the cache at all:
    // the packed chain entries in every data page have to become real pointers, and the authenticated
    // ones have to be signed so the guest's own autda accepts them. A plain address does not survive
    // authentication -- that is what real dyld faulted on.
    TEST(DyldSyscalls, SharedRegionInstallAppliesTheSlideInfoChain)
    {
        const sogen::test::temp_directory dir{"slide-info"};

        const auto page = static_cast<size_t>(sogen::MACOS_PAGE_SIZE);
        std::vector<uint8_t> content(page * 3, 0x00);

        // Page 0 carries a three-link chain: two words apart, then eight, then the terminator. The gap
        // is filled with a value that is not an entry, so a walk that ignored `next` would rewrite it.
        const auto write_word = [&content](const size_t offset, const uint64_t value) {
            std::memcpy(content.data() + offset, &value, sizeof(value));
        };

        write_word(0x40, shared_region::regular_entry(0x1000, 2, 0x00));
        write_word(0x48, 0xDEADBEEFDEADBEEFULL);
        write_word(0x50, shared_region::regular_entry(0x2000, 1, 0xAB));
        write_word(0x58, shared_region::auth_entry(0x3000, 1, 0x6AE1, true, true));
        write_word(0x60, shared_region::auth_entry(0x4000, 0, 0x1357, false, false));

        // Page 1 is marked NO_REBASE and must come back untouched.
        write_word(page + 0x40, 0x1122334455667788ULL);

        // Page 2 is the slide info itself: header, then one uint16 per page.
        const size_t slide_offset = page * 2;
        const uint32_t version = 5;
        const auto page_size32 = static_cast<uint32_t>(page);
        const uint32_t starts_count = 2;
        std::memcpy(content.data() + slide_offset, &version, sizeof(version));
        std::memcpy(content.data() + slide_offset + 4, &page_size32, sizeof(page_size32));
        std::memcpy(content.data() + slide_offset + 8, &starts_count, sizeof(starts_count));
        const uint64_t value_add = shared_region::VALUE_ADD;
        std::memcpy(content.data() + slide_offset + 16, &value_add, sizeof(value_add));

        const std::array<uint16_t, 2> starts{0x40, 0xFFFF};
        std::memcpy(content.data() + slide_offset + 24, starts.data(), sizeof(starts));

        const auto path = dir.path() / "cache";
        {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        }

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        constexpr uint64_t target = 0x380000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        sogen::guest_fd fd{};
        fd.host_path = path.string();
        fd.guest_path = "/cache";
        fd.type = sogen::fd_type::file;
        const auto guest_fd = emu->process.fds.allocate(std::move(fd));
        ASSERT_GE(guest_fd, 0);

        shared_region::write_file_entry(*emu, scratch, static_cast<int32_t>(guest_fd), 2, 0);

        const uint64_t maps = scratch + 0x100;
        shared_region::write_mapping(*emu, maps, target, page * 2, 0, 3);
        shared_region::write_mapping(*emu, maps + shared_region::MAPPING_ENTRY_SIZE, target + page * 2, page, slide_offset, 1);

        // The slide info lives in the second mapping, which is only readable once both are installed.
        const uint64_t slide_start = target + page * 2;
        const uint64_t slide_size = 24 + sizeof(starts);
        emu->memory.write_memory(maps + 24, &slide_size, sizeof(slide_size));
        emu->memory.write_memory(maps + 32, &slide_start, sizeof(slide_start));

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(536, {2, scratch, 2, maps}));
        emu->start(24);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << emu->last_stop_detail();

        const auto read_word = [&](const uint64_t address) {
            uint64_t value{};
            emu->memory.read_memory(address, &value, sizeof(value));
            return value;
        };

        EXPECT_EQ(read_word(target + 0x40), shared_region::VALUE_ADD + 0x1000);
        EXPECT_EQ(read_word(target + 0x48), 0xDEADBEEFDEADBEEFULL) << "the chain skips this word, so nothing may touch it";
        EXPECT_EQ(read_word(target + 0x50), (shared_region::VALUE_ADD + 0x2000) | (uint64_t{0xAB} << 56)) << "high8 rides above";

        auto expected = shared_region::VALUE_ADD + 0x3000;
        const auto location = target + 0x58;
        const auto discriminator = (location & 0x0000FFFFFFFFFFFFULL) | (uint64_t{0x6AE1} << 48);
        ASSERT_TRUE(emu->emu().sign_pointer(expected, sogen::arm64_pauth_key::data_a, discriminator));
        EXPECT_EQ(read_word(target + 0x58), expected) << "an authenticated entry has to come out signed, not as a plain address";
        EXPECT_NE(read_word(target + 0x58), shared_region::VALUE_ADD + 0x3000);

        // keyIsData = false picks the instruction key, and addrDiv = false leaves the diversity alone.
        auto instruction_signed = shared_region::VALUE_ADD + 0x4000;
        ASSERT_TRUE(emu->emu().sign_pointer(instruction_signed, sogen::arm64_pauth_key::instruction_a, 0x1357));
        EXPECT_EQ(read_word(target + 0x60), instruction_signed);

        // The two keys are deliberately not compared: the backend's PAC key registers are all zero, so
        // IA and DA produce the same signature today. Selecting the right one still matters -- a guest
        // that sets its own keys would see the difference immediately -- but no assertion here can
        // observe it.

        EXPECT_EQ(read_word(target + static_cast<uint64_t>(page) + 0x40), 0x1122334455667788ULL) << "a page marked NO_REBASE is left alone";
    }

    // The dynamic region is a page the kernel synthesises at the top of the shared region; it is in no
    // cache file. dyld locates it at cache_base + header[0x1f0] and validates it by comparing the first
    // fourteen bytes against "dyld_data    v" -- so a page of zeroes reads as absent, dynamicRegion()
    // returns null, and its caller dereferences that null immediately.
    TEST(DyldSyscalls, SharedRegionInstallPublishesTheDynamicRegion)
    {
        const sogen::test::temp_directory dir{"dynamic-region"};

        const auto page = static_cast<size_t>(sogen::MACOS_PAGE_SIZE);
        std::vector<uint8_t> content(page, 0x00);

        constexpr uint64_t region_offset = 0x8000;
        std::memcpy(content.data() + 0x1F0, &region_offset, sizeof(region_offset));

        const auto path = dir.path() / "cache";
        {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        }

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        constexpr uint64_t target = 0x380000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        sogen::guest_fd fd{};
        fd.host_path = path.string();
        fd.guest_path = "/System/Library/dyld/dyld_shared_cache_arm64e";
        fd.type = sogen::fd_type::file;
        const auto guest_fd = emu->process.fds.allocate(std::move(fd));
        ASSERT_GE(guest_fd, 0);

        shared_region::write_file_entry(*emu, scratch, static_cast<int32_t>(guest_fd), 1, 0);

        const uint64_t maps = scratch + 0x100;
        shared_region::write_mapping(*emu, maps, target, page, 0, 5);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(536, {1, scratch, 1, maps}));
        emu->start(24);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << emu->last_stop_detail();

        const auto region = target + region_offset;

        std::array<char, 16> magic{};
        ASSERT_TRUE(emu->memory.try_read_memory(region, magic.data(), 15));
        EXPECT_EQ(std::string_view(magic.data(), 14), "dyld_data    v") << "this is the whole of dyld's validity test";
        EXPECT_EQ(magic[14], '1') << "version() reads this byte and subtracts '0'";

        uint32_t cryptex_offset{};
        uint32_t path_offset{};
        emu->memory.read_memory(region + 0x20, &cryptex_offset, sizeof(cryptex_offset));
        emu->memory.read_memory(region + 0x24, &path_offset, sizeof(path_offset));
        EXPECT_EQ(cryptex_offset, 0u) << "zero is how dyld spells \"no cryptex path\"";
        ASSERT_NE(path_offset, 0u) << "a zero here makes cachePath() return null";

        std::array<char, 64> reported{};
        emu->memory.read_memory(region + path_offset, reported.data(), reported.size() - 1);
        EXPECT_STREQ(reported.data(), "/System/Library/dyld/dyld_shared_cache_arm64e");

        // setDyldCacheFileID stores the pair here, and it has to be the same identity the stat family
        // reports for that path or dyld concludes the cache moved.
        const auto identity = emu->identities.acquire("/System/Library/dyld/dyld_shared_cache_arm64e");
        uint64_t fsid{};
        uint64_t object_id{};
        emu->memory.read_memory(region + 0x10, &fsid, sizeof(fsid));
        emu->memory.read_memory(region + 0x18, &object_id, sizeof(object_id));
        EXPECT_EQ(fsid, identity.packed_fsid());
        EXPECT_EQ(object_id, identity.object_id);
    }

    // libpthread's initialisation calls this once. The command is named by value because the
    // enumeration lives in xnu's private pthread headers, not the SDK.
    TEST(DyldSyscalls, BsdthreadCtlAllowsTheWorkqueueKillAndNamesWhatItCannotDo)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(478, {0x1000, 1, 0, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        // An unimplemented command inside an implemented syscall would otherwise be silent, which is
        // how a missing MIG routine stayed hidden until it burned the guest's stack.
        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(478, {0x99, 0, 0, 0}));
        emu->start(16);
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOTSUP));
    }

    // __pthread_sigmask sets the calling thread's mask; with one thread that is the process mask, so it
    // shares sigprocmask's handler. This pins that it is actually reachable under its own number.
    TEST(DyldSyscalls, PthreadSigmaskSharesTheProcessMask)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const uint32_t blocked = 0x00000009;
        emu->memory.write_memory(scratch, &blocked, sizeof(blocked));

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(329, {1, scratch, 0}));
        emu->start(16);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->process.signal_mask, blocked);
    }

    // Nothing here delivers signals, so a thread signalling itself can only be terminated -- which is
    // what really happens for the fatal ones. abort() reaches the kernel exactly this way, and a guest
    // handed a success return would carry on past its own abort.
    TEST(DyldSyscalls, PthreadKillTerminatesRatherThanPretendingToDeliver)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(328, {0x101, 0}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "signal 0 only asks whether the thread exists";
        EXPECT_FALSE(emu->process.exit_status.has_value());

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(328, {0x101, 6}));
        emu->start(16);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::signal_termination);
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 6);
        EXPECT_NE(emu->last_stop_detail().find("SIGABRT"), std::string::npos) << emu->last_stop_detail();

        ASSERT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_EQ(emu->mach.last_exception->type, sogen::mach::exception_type::software);
    }

    TEST(DyldSyscalls, PthreadKillRefusesASignalThatDoesNotExist)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(328, {0x101, 99}));
        emu->start(16);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
        EXPECT_FALSE(emu->process.exit_status.has_value());
    }

    // The first object libSystem asks for is com.apple.featureflags.shm, whose contents decide which
    // features the process believes are on. A guest with no featureflagsd genuinely does not have it.
    TEST(DyldSyscalls, ShmOpenReportsNoSharedMemoryNamespace)
    {
        const sogen::test::temp_directory dir{"shm"};
        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const std::string name = "com.apple.featureflags.shm";
        emu->memory.write_memory(scratch, name.c_str(), name.size() + 1);

        macos_test::write_guest_code(*emu, 0x310000000ULL, macos_test::syscall_sequence(266, {scratch, 0, 0}));
        emu->start(16);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOENT));
    }

    // Creating a local socket costs the guest nothing and touches nothing outside it. Every other
    // address family is refused deliberately: this is an analysis sandbox, and handing a sample a real
    // AF_INET socket would let it reach the network from inside what is meant to be a container.
    TEST(DyldSyscalls, SocketAllowsLocalOnesAndRefusesTheNetwork)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(97, {1, 2, 0}));
        emu->start(16);
        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << emu->last_stop_detail();

        const auto fd = emu->emu().reg(sogen::arm64_register::x0);
        ASSERT_GE(fd, 3u);
        const auto* entry = emu->process.fds.get(static_cast<int>(fd));
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->type, sogen::fd_type::socket);
        ASSERT_NE(entry->socket_state, nullptr);
        EXPECT_EQ(entry->socket_state->host_socket, -1) << "no host socket may back a guest one";

        for (const auto family : {2, 30, 32})
        {
            macos_test::write_guest_code(*emu, 0x310000000ULL + static_cast<uint64_t>(family) * 0x10000,
                                         macos_test::syscall_sequence(97, {static_cast<uint64_t>(family), 1, 0}));
            emu->start(16);
            EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u) << "family " << family;
            EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EAFNOSUPPORT));
        }
    }

    TEST(DyldSyscalls, ConnectRefusesAHostSocketAndReportsAMissingDaemonHonestly)
    {
        const sogen::test::temp_directory dir{"connect"};
        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), dir.path());

        constexpr uint64_t scratch = 0x300000000ULL;
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        macos_test::write_guest_code(*emu, 0x300000000ULL + 0x10000, macos_test::syscall_sequence(97, {1, 2, 0}));
        emu->start(16);
        const auto fd = emu->emu().reg(sogen::arm64_register::x0);

        const auto write_sockaddr = [&](const uint64_t at, const std::string& path) {
            std::vector<char> raw(2 + path.size() + 1, 0);
            raw[0] = static_cast<char>(raw.size());
            raw[1] = 1;
            std::ranges::copy(path, raw.begin() + 2);
            emu->memory.write_memory(at, raw.data(), raw.size());
            return raw.size();
        };

        const auto missing = write_sockaddr(scratch, "/var/run/syslog");
        macos_test::write_guest_code(*emu, 0x320000000ULL, macos_test::syscall_sequence(98, {fd, scratch, missing}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOENT))
            << "a daemon that is not there is simply not there";

        // A path that *does* resolve inside the root names a real host object, and connecting to it
        // would take the guest out of the container.
        std::filesystem::create_directories(dir.path() / "var" / "run");
        {
            std::ofstream stream{dir.path() / "var" / "run" / "present"};
            stream << "x";
        }

        const auto present = write_sockaddr(scratch + 0x200, "/var/run/present");
        macos_test::write_guest_code(*emu, 0x330000000ULL, macos_test::syscall_sequence(98, {fd, scratch + 0x200, present}));
        emu->start(16);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ECONNREFUSED));
    }

    TEST(DyldSyscalls, TheObjcBranchPredictorHintIsRefused)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::syscall_sequence(535, {0, 0, 0}));
        emu->start(16);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & 0x20000000u, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_ENOTSUP))
            << "an optimisation the runtime carries on without";
    }
}

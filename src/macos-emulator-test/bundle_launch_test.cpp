#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <macos_launch_target.hpp>
#include <utils/io.hpp>

#include "fixture_utils.hpp"
#include "macho_fixture.hpp"
#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t probe_code_base = 0x400000000ULL;
    constexpr uint64_t probe_data_base = 0x400100000ULL;
    constexpr uint64_t carry_flag = 0x20000000ULL;

    constexpr uint32_t mov_x16_open = 0xD28000B0;
    constexpr uint32_t mov_x16_write = 0xD2800090;

    struct guest_syscall_result
    {
        bool failed{};
        uint64_t value{};
    };

    uint64_t write_guest_string(sogen::macos_emulator& emu, const std::string& text, const uint64_t offset)
    {
        if (!emu.memory.get_region_info(probe_data_base).has_value())
        {
            emu.memory.allocate_memory(probe_data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        }

        const auto address = probe_data_base + offset;
        emu.memory.write_memory(address, text.c_str(), text.size() + 1);
        return address;
    }

    // Unicorn caches translated blocks, so replaying a syscall at an address that already ran would
    // execute the first block again; every invocation in a test gets a site of its own.
    guest_syscall_result run_guest_syscall(sogen::macos_emulator& emu, const size_t site, const uint32_t mov_x16,
                                           const std::vector<uint64_t>& arguments)
    {
        macos_test::write_guest_code(emu, probe_code_base + (site * 0x40), {mov_x16, 0xD4001001});

        for (size_t i = 0; i < arguments.size(); ++i)
        {
            emu.emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::x0) + i), arguments[i]);
        }

        emu.start(2);

        return {.failed = (emu.emu().reg(sogen::arm64_register::nzcv) & carry_flag) == carry_flag,
                .value = emu.emu().reg(sogen::arm64_register::x0)};
    }

    std::string read_host_file(const std::filesystem::path& path)
    {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    std::filesystem::path add_bundle_resource(const std::filesystem::path& bundle, const std::string& name, const std::string& content)
    {
        const auto resource = bundle / "Contents" / "Resources" / name;
        std::filesystem::create_directories(resource.parent_path());
        std::ofstream stream{resource, std::ios::binary | std::ios::trunc};
        stream << content;
        return resource;
    }

    std::unique_ptr<sogen::macos_emulator> launch_bundle(const sogen::macos_launch_target& target)
    {
        auto emu = macos_test::make_emulator();
        sogen::apply_macos_launch_target(target, emu->file_sys);
        emu->load_application(target.guest_executable, {target.guest_executable}, {});
        emu->process.current_working_directory = target.working_directory;
        return emu;
    }

    std::filesystem::path make_runnable_bundle(const std::filesystem::path& parent, const std::string& name, const uint16_t exit_status = 0)
    {
        const auto bundle = parent / (name + ".app");
        std::filesystem::create_directories(bundle / "Contents" / "MacOS");

        std::ofstream plist{bundle / "Contents" / "Info.plist", std::ios::binary | std::ios::trunc};
        plist << R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>)" << name
              << R"(</string>
	<key>CFBundleIdentifier</key>
	<string>dev.sogen.probe</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
</dict>
</plist>
)";
        plist.close();

        const auto image = macos_test::build_hello_world_macho(exit_status);
        if (!sogen::utils::io::write_file(bundle / "Contents" / "MacOS" / name, image))
        {
            throw std::runtime_error("failed to write the bundle executable");
        }

        return bundle;
    }

    TEST(MacosBundleLaunch, RunsTheExecutableInsideAnAppBundle)
    {
        const sogen::test::temp_directory dir{"bundle-run"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;
        EXPECT_EQ(target.guest_executable, "/Applications/SogenProbe.app/Contents/MacOS/SogenProbe");

        const auto emu = macos_test::make_emulator();
        sogen::apply_macos_launch_target(target, emu->file_sys);

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        emu->load_application(target.guest_executable, {target.guest_executable}, {});
        emu->process.current_working_directory = target.working_directory;
        emu->start(64);

        EXPECT_EQ(captured, "Hello, sogen!\n");
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0);
        EXPECT_EQ(emu->process.current_working_directory, "/");
    }

    TEST(MacosBundleLaunch, RunsTheSameBundleWhenGivenItsInnerExecutable)
    {
        const sogen::test::temp_directory dir{"bundle-run-inner"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");

        const auto target = sogen::resolve_macos_launch_target(bundle / "Contents" / "MacOS" / "SogenProbe");
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = macos_test::make_emulator();
        sogen::apply_macos_launch_target(target, emu->file_sys);

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        emu->load_application(target.guest_executable, {target.guest_executable}, {});
        emu->start(64);

        EXPECT_EQ(captured, "Hello, sogen!\n");
    }

    // RunsTheExecutableInsideAnAppBundle asserts an exit status of 0, which a handler that hardcodes zero
    // would satisfy by accident, and it never pins where the running image came from. A non-zero status
    // that only the bundle's own copy produces closes both.
    TEST(MacosBundleLaunch, RunsTheBundleCopyAndPropagatesItsExitStatus)
    {
        const sogen::test::temp_directory dir{"bundle-run-status"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe", 7);

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = macos_test::make_emulator();
        sogen::apply_macos_launch_target(target, emu->file_sys);

        EXPECT_EQ(emu->file_sys.translate(target.guest_executable),
                  std::filesystem::weakly_canonical(bundle) / "Contents" / "MacOS" / "SogenProbe");

        std::string captured{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { captured.append(data); };

        emu->load_application(target.guest_executable, {target.guest_executable}, {});
        emu->start(64);

        EXPECT_EQ(captured, "Hello, sogen!\n");
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 7);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << emu->last_stop_detail();
        EXPECT_EQ(emu->process.executable_path, "/Applications/SogenProbe.app/Contents/MacOS/SogenProbe");
    }

    TEST(MacosBundleLaunch, ResourcesInsideTheBundleAreReachableThroughGuestPaths)
    {
        const sogen::test::temp_directory dir{"bundle-resources"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");
        std::filesystem::create_directories(bundle / "Contents" / "Resources");
        {
            std::ofstream resource{bundle / "Contents" / "Resources" / "greeting.txt", std::ios::binary | std::ios::trunc};
            resource << "hello resource";
        }

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = macos_test::make_emulator();
        sogen::apply_macos_launch_target(target, emu->file_sys);

        const auto host = emu->file_sys.translate("/Applications/SogenProbe.app/Contents/Resources/greeting.txt");
        EXPECT_TRUE(std::filesystem::is_regular_file(host));
    }

    // load_application registers the executable's real host directory as a passthrough prefix, so the
    // guest can name the bundle by its host path and reach the same files the read-only mapping covers.
    // The mapping has to hold on that route too: this write lands on the analyst's own disk otherwise.
    TEST(MacosBundleLaunch, DeniesAGuestWriteToTheBundleNamedByItsHostPath)
    {
        const sogen::test::temp_directory dir{"bundle-readonly-host"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");
        const auto resource = add_bundle_resource(bundle, "greeting.txt", "hello resource");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = launch_bundle(target);

        const auto host_alias = write_guest_string(*emu, resource.generic_string(), 0);
        const auto opened =
            run_guest_syscall(*emu, 0, mov_x16_open,
                              {host_alias, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_TRUNC)});

        EXPECT_TRUE(opened.failed);
        EXPECT_EQ(opened.value, static_cast<uint64_t>(sogen::macos_errno::MACOS_EACCES));
        EXPECT_EQ(read_host_file(resource), "hello resource");
    }

    // The bundle root is canonical while the path the guest learns from its own environment need not be:
    // $TMPDIR and /tmp are symlinks into /private on macOS.
    TEST(MacosBundleLaunch, DeniesAGuestWriteToTheBundleNamedByItsCanonicalHostPath)
    {
        const sogen::test::temp_directory dir{"bundle-readonly-canonical"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");
        const auto resource = add_bundle_resource(bundle, "greeting.txt", "hello resource");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = launch_bundle(target);

        std::error_code error{};
        const auto canonical_resource = std::filesystem::weakly_canonical(resource, error);
        ASSERT_FALSE(error) << error.message();

        const auto host_alias = write_guest_string(*emu, canonical_resource.generic_string(), 0);
        const auto opened =
            run_guest_syscall(*emu, 0, mov_x16_open,
                              {host_alias, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_TRUNC)});

        EXPECT_TRUE(opened.failed);
        EXPECT_EQ(opened.value, static_cast<uint64_t>(sogen::macos_errno::MACOS_EACCES));
        EXPECT_EQ(read_host_file(resource), "hello resource");
    }

    // A descriptor opened for reading carries the mapping's read-only flag, which is what stops a write
    // that the open itself had no reason to refuse.
    TEST(MacosBundleLaunch, DeniesAGuestWriteThroughAReadOnlyBundleDescriptor)
    {
        const sogen::test::temp_directory dir{"bundle-readonly-fd"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");
        const auto resource = add_bundle_resource(bundle, "greeting.txt", "hello resource");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = launch_bundle(target);

        const auto host_alias = write_guest_string(*emu, resource.generic_string(), 0);
        const auto payload = write_guest_string(*emu, "PWNED", 0x800);

        const auto opened =
            run_guest_syscall(*emu, 0, mov_x16_open, {host_alias, static_cast<uint64_t>(sogen::macos_open::MACOS_O_RDONLY)});
        ASSERT_FALSE(opened.failed) << opened.value;

        const auto* fd_entry = emu->process.fds.get(static_cast<int>(opened.value));
        ASSERT_NE(fd_entry, nullptr);
        EXPECT_TRUE(fd_entry->read_only_mapping);

        const auto written = run_guest_syscall(*emu, 1, mov_x16_write, {opened.value, payload, 5});
        EXPECT_TRUE(written.failed);
        EXPECT_EQ(written.value, static_cast<uint64_t>(sogen::macos_errno::MACOS_EACCES));
        EXPECT_EQ(read_host_file(resource), "hello resource");
    }

    // The guest path is resolved against the working directory before it reaches the host, so the
    // read-only decision has to be made on the resolved path rather than on the relative spelling.
    TEST(MacosBundleLaunch, DeniesAGuestWriteToTheBundleNamedRelativeToTheWorkingDirectory)
    {
        const sogen::test::temp_directory dir{"bundle-readonly-relative"};
        const auto bundle = make_runnable_bundle(dir.path(), "SogenProbe");
        const auto resource = add_bundle_resource(bundle, "greeting.txt", "hello resource");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        const auto emu = launch_bundle(target);
        emu->process.current_working_directory = "/Applications/SogenProbe.app/Contents/Resources";

        const auto relative = write_guest_string(*emu, "greeting.txt", 0);
        const auto opened = run_guest_syscall(
            *emu, 0, mov_x16_open, {relative, static_cast<uint64_t>(sogen::macos_open::MACOS_O_WRONLY | sogen::macos_open::MACOS_O_TRUNC)});

        EXPECT_TRUE(opened.failed);
        EXPECT_EQ(opened.value, static_cast<uint64_t>(sogen::macos_errno::MACOS_EACCES));
        EXPECT_EQ(read_host_file(resource), "hello resource");
    }
}

#include <gtest/gtest.h>

#include <backend_selection.hpp>
#include <macos_emulator.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::macos_dyld_test
{
    namespace
    {
        // Built by src/tools/create-macos-root.sh on the machine running the tests; it is never distributed.
        std::optional<std::filesystem::path> get_emulation_root()
        {
            const auto* root = std::getenv("MACOS_EMULATOR_ROOT");
            if (root == nullptr)
            {
                return std::nullopt;
            }

            return root;
        }

        struct run_result
        {
            std::string stdout_data{};
            std::optional<int> exit_status{};
        };

        run_result run_root_binary(const std::filesystem::path& root, const std::string_view guest_path,
                                   const std::vector<std::string>& argv, const std::function<void(macos_emulator&)>& prepare = {})
        {
            macos_emulator emu{create_x86_64_emulator_from_environment(), root, root / guest_path.substr(1), argv, {}};
            emu.log.disable_output(true);

            if (prepare)
            {
                prepare(emu);
            }

            run_result result{};
            emu.on_stdout = [&result](const std::string_view output) { result.stdout_data += output; };

            emu.start();

            result.exit_status = emu.exit_status;
            return result;
        }
    }

    TEST(MacosDyldTest, RunsEcho)
    {
        const auto root = get_emulation_root();
        if (!root)
        {
            GTEST_SKIP() << "MACOS_EMULATOR_ROOT is not set";
        }

        const auto result = run_root_binary(*root, "/bin/echo", {"/bin/echo", "hello"});

        EXPECT_EQ(result.stdout_data, "hello\n");
        EXPECT_EQ(result.exit_status, 0);
    }

    TEST(MacosDyldTest, ListsDirectoryWithLs)
    {
        const auto root = get_emulation_root();
        if (!root)
        {
            GTEST_SKIP() << "MACOS_EMULATOR_ROOT is not set";
        }

        const auto listed_directory = std::filesystem::path{::testing::TempDir()} / "macos-dyld-test-ls";
        std::filesystem::remove_all(listed_directory);
        std::filesystem::create_directories(listed_directory / "subdirectory");
        for (const auto* name : {"beta", "alpha"})
        {
            const std::ofstream file{listed_directory / name};
        }

        const auto result = run_root_binary(*root, "/bin/ls", {"/bin/ls", "/listed"}, [&](macos_emulator& emu) {
            emu.file_system.add_path_mapping("/listed", listed_directory, true);
        });
        std::filesystem::remove_all(listed_directory);

        EXPECT_EQ(result.stdout_data, "alpha\nbeta\nsubdirectory\n");
        EXPECT_EQ(result.exit_status, 0);
    }

    TEST(MacosDyldTest, RunsBashBuiltins)
    {
        const auto root = get_emulation_root();
        if (!root)
        {
            GTEST_SKIP() << "MACOS_EMULATOR_ROOT is not set";
        }

        const auto listed_directory = std::filesystem::path{::testing::TempDir()} / "macos-dyld-test-bash";
        std::filesystem::remove_all(listed_directory);
        std::filesystem::create_directories(listed_directory / "subdirectory");
        for (const auto* name : {"beta", "alpha"})
        {
            const std::ofstream file{listed_directory / name};
        }

        const auto result =
            run_root_binary(*root, "/bin/bash", {"/bin/bash", "-c", "cd /listed/subdirectory/.. && pwd && echo *; exit 3"},
                            [&](macos_emulator& emu) { emu.file_system.add_path_mapping("/listed", listed_directory, true); });
        std::filesystem::remove_all(listed_directory);

        EXPECT_EQ(result.stdout_data, "/listed\nalpha beta subdirectory\n");
        EXPECT_EQ(result.exit_status, 3);
    }
}

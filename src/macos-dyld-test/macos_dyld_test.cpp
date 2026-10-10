#include <gtest/gtest.h>

#include <backend_selection.hpp>
#include <macos_emulator.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::macos_dyld_test
{
    namespace
    {
        // Built by src/tools/create-macos-root.sh on the machine running the tests; it is never distributed.
        std::filesystem::path get_emulation_root()
        {
            const auto* root = std::getenv("MACOS_EMULATOR_ROOT");
            if (root == nullptr)
            {
                throw std::runtime_error("No MACOS_EMULATOR_ROOT set!");
            }

            return root;
        }

        struct run_result
        {
            std::string stdout_data{};
            std::optional<int> exit_status{};
        };

        run_result run_root_binary(const std::string_view guest_path, const std::vector<std::string>& argv)
        {
            const auto root = get_emulation_root();

            macos_emulator emu{create_x86_64_emulator_from_environment(), root, root / guest_path.substr(1), argv, {}};
            emu.log.disable_output(true);

            run_result result{};
            emu.on_stdout = [&result](const std::string_view output) { result.stdout_data += output; };

            emu.start();

            result.exit_status = emu.exit_status;
            return result;
        }
    }

    // Disabled until Mach messages are modelled: libSystem's initialization stops at its first mach_msg2 (host_info).
    TEST(MacosDyldTest, DISABLED_RunsEcho)
    {
        const auto result = run_root_binary("/bin/echo", {"/bin/echo", "hello"});

        EXPECT_EQ(result.stdout_data, "hello\n");
        EXPECT_EQ(result.exit_status, 0);
    }
}

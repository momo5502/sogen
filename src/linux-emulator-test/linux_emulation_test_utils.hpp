#pragma once

#ifdef __MINGW64__
#include <unistd.h>
#endif

#include <cstdlib>
#include <string>
#include <filesystem>
#include <gtest/gtest.h>
#include <linux_emulator.hpp>
#include <backend_selection.hpp>

#define ASSERT_LINUX_NOT_TERMINATED(linux_emu)                     \
    do                                                             \
    {                                                              \
        ASSERT_FALSE((linux_emu).process.exit_status.has_value()); \
    } while (false)

#define ASSERT_LINUX_TERMINATED_WITH_STATUS(linux_emu, status)    \
    do                                                            \
    {                                                             \
        ASSERT_TRUE((linux_emu).process.exit_status.has_value()); \
        ASSERT_EQ(*(linux_emu).process.exit_status, status);      \
    } while (false)

#define ASSERT_LINUX_TERMINATED_SUCCESSFULLY(linux_emu) ASSERT_LINUX_TERMINATED_WITH_STATUS(linux_emu, 0)

namespace linux_test
{
    inline bool enable_verbose_logging()
    {
        const auto* env = getenv("EMULATOR_VERBOSE");
        return env && (env == "1"sv || env == "true"sv);
    }

    inline std::filesystem::path get_linux_test_root()
    {
        const auto* env = getenv("LINUX_TEST_ROOT");
        if (!env)
        {
            throw std::runtime_error("No LINUX_TEST_ROOT set!");
        }

        return env;
    }

    inline std::vector<std::string> default_envp()
    {
        return {
            "PATH=/usr/bin:/bin",
            "HOME=/root",
            "TERM=xterm",
        };
    }

    struct linux_emulator_result
    {
        std::optional<int> exit_status{};
        std::string captured_stdout{};
        std::string captured_stderr{};
        uint64_t instructions_executed{0};
    };

    struct linux_loader_fixture_result
    {
        bool loader_rejected{false};
        std::optional<int> exit_status{};
        uint64_t instructions_executed{0};
        std::string diagnostics{};

        bool rejected() const
        {
            return this->loader_rejected || (this->exit_status.has_value() && *this->exit_status != 0);
        }
    };

    inline std::filesystem::path get_linux_torture_fixture_root()
    {
#ifdef LINUX_TORTURE_FIXTURE_ROOT
        return std::filesystem::path(LINUX_TORTURE_FIXTURE_ROOT);
#else
        return get_linux_test_root() / "test" / "linux" / "torture" / "fixtures";
#endif
    }

    inline linux_emulator_result run_linux_binary(const std::filesystem::path& binary, std::vector<std::string> argv = {},
                                                  std::vector<std::string> envp = {}, const std::filesystem::path& emulation_root = {})
    {
        if (argv.empty())
        {
            argv.push_back(binary.string());
        }

        if (envp.empty())
        {
            envp = default_envp();
        }

        auto emu_backend = create_x86_64_emulator();

        linux_emulator linux_emu(std::move(emu_backend), emulation_root, binary, argv, envp);

        linux_emulator_result result{};

        linux_emu.on_stdout = [&result](const std::string_view data) { result.captured_stdout += data; };

        linux_emu.on_stderr = [&result](const std::string_view data) { result.captured_stderr += data; };

        if (!enable_verbose_logging())
        {
            linux_emu.log.disable_output(true);
        }

        linux_emu.start();

        result.exit_status = linux_emu.process.exit_status;
        result.instructions_executed = linux_emu.get_executed_instructions();

        return result;
    }

    inline linux_loader_fixture_result run_linux_loader_fixture(const std::filesystem::path& fixture,
                                                                const std::filesystem::path& emulation_root = {},
                                                                const size_t instruction_budget = 200000)
    {
        linux_loader_fixture_result result{};

        try
        {
            auto emu_backend = create_x86_64_emulator();
            linux_emulator linux_emu(std::move(emu_backend), emulation_root, fixture, {fixture.string()}, default_envp());
            linux_emu.log.disable_output(true);

            linux_emu.start(instruction_budget);

            result.exit_status = linux_emu.process.exit_status;
            result.instructions_executed = linux_emu.get_executed_instructions();
        }
        catch (const std::exception& ex)
        {
            result.loader_rejected = true;
            result.diagnostics = ex.what();
            return result;
        }

        if (result.exit_status.has_value() && *result.exit_status != 0)
        {
            result.diagnostics = "Process terminated with non-zero status " + std::to_string(*result.exit_status);
        }

        return result;
    }
}

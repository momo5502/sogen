#include "emulation_test_utils.hpp"

#include <cstdlib>

namespace sogen::test
{
    namespace
    {
        // ARM64 counterpart of EMULATOR_ROOT: a root grabbed with create-root.bat on Windows ARM64, with the
        // ARM64 samples copied into its C:\ as CI does for the x64 test-sample.
        std::optional<std::filesystem::path> get_arm64_emulator_root()
        {
            const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
            if (!root || !*root)
            {
                return std::nullopt;
            }

            return std::filesystem::path{root};
        }

        struct arm64_run
        {
            std::optional<NTSTATUS> exit_status{};
            std::optional<uint32_t> fast_fail_code{};
            std::string output{};
        };

        arm64_run run_arm64_application(const std::filesystem::path& root, application_settings app_settings)
        {
            emulator_settings settings{};
            settings.emulation_root = root;
            settings.mounts = {
                mount_spec{
                    .host_path = std::filesystem::temp_directory_path() / ("emulator-arm64-test-file-" + std::to_string(getpid()) + ".txt"),
                    .guest_path = windows_path{"C:\\a.txt"},
                    .mode = filesystem_mode::read_write,
                },
                mount_spec{
                    .host_path =
                        std::filesystem::temp_directory_path() / ("emulator-arm64-crt-static-" + std::to_string(getpid()) + ".tmp"),
                    .guest_path = windows_path{"C:\\crt_static.tmp"},
                    .mode = filesystem_mode::read_write,
                },
            };

            arm64_run result{};
            emulator_callbacks callbacks{};
            callbacks.on_stdout = [&](const std::string_view data) {
                if (enable_verbose_logging())
                {
                    std::cout << data;
                }
                result.output.append(data);
            };
            callbacks.on_fast_fail = [&](const uint32_t code) { result.fast_fail_code = code; };

            emulator_interfaces interfaces{};
            interfaces.ui = std::make_unique<null_ui_backend>();
            interfaces.socket_factory = network::create_static_socket_factory();
            interfaces.dns_lookup = create_sample_dns_lookup();

            const auto machine = get_application_machine(settings, app_settings);
            windows_emulator emu{create_emulator_for_machine(get_machine_type(machine)), std::move(app_settings), settings,
                                 std::move(callbacks), std::move(interfaces)};
            emu.log.disable_output(!enable_verbose_logging());
            emu.start();

            result.exit_status = emu.process.exit_status;
            return result;
        }

        class Arm64EmulationTest : public testing::Test
        {
          protected:
            std::filesystem::path root_{};

            void SetUp() override
            {
                const auto root = get_arm64_emulator_root();
                if (!root)
                {
                    GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
                }

                root_ = *root;
            }

            arm64_run run_application(application_settings app_settings) const
            {
                return run_arm64_application(root_, std::move(app_settings));
            }
        };
    }

    TEST_F(Arm64EmulationTest, KernelOnlySampleWritesToTheConsole)
    {
        const auto run = this->run_application({.application = "C:\\hello_world.exe"});

        ASSERT_EQ(run.exit_status, STATUS_SUCCESS);
        EXPECT_EQ(run.output, "Hello, world!\r\n");
    }

    TEST_F(Arm64EmulationTest, CrtMainReceivesArgumentsAndPrintsThroughTheCrt)
    {
        const auto run = this->run_application({.application = "C:\\hello_crt.exe"});

        ASSERT_EQ(run.exit_status, STATUS_SUCCESS);
        EXPECT_EQ(run.output, "Hello, world! argc=1 argv[0]=c:\\hello_crt.exe\r\n");
    }

    TEST_F(Arm64EmulationTest, RelativeApplicationRunsFromTheWorkingDirectory)
    {
        const auto run = this->run_application({.application = "hello_crt.exe", .working_directory = "C:\\"});

        ASSERT_EQ(run.exit_status, STATUS_SUCCESS);
        EXPECT_EQ(run.output, "Hello, world! argc=1 argv[0]=c:\\hello_crt.exe\r\n");
    }

    TEST_F(Arm64EmulationTest, StaticallyLinkedCrtRunsWithoutCrtDlls)
    {
        const auto run = this->run_application({.application = "C:\\crt_static.exe", .arguments = {u"arg"}});

        ASSERT_EQ(run.exit_status, STATUS_SUCCESS) << run.output;
        EXPECT_EQ(run.output, "static CRT: argc=2 argv[0]=c:\\crt_static.exe\r\n"
                              "format: 3.142 1.234500e+03 beef\r\n"
                              "sort: 1 3 5 7 9, found 7 at 3\r\n"
                              "parse: 2500.0 rest=xyz scanned=17\r\n"
                              "locale: C\r\n"
                              "heap: ok\r\n"
                              "file: ok\r\n"
                              "thread errno: ok\r\n"
                              "time: ok\r\n"
                              "environment: ok\r\n"
                              "atexit handler ran\r\n");
    }

    TEST_F(Arm64EmulationTest, InstrumentationCallbackResumesThroughX16)
    {
        const auto run = this->run_application({.application = "C:\\instrumentation_callback.exe"});

        ASSERT_EQ(run.exit_status, STATUS_SUCCESS) << run.output;
        EXPECT_EQ(run.output, "System call returns observed: yes\r\n");
    }

    TEST_F(Arm64EmulationTest, TestSampleSucceeds)
    {
        const auto run = this->run_application({.application = "C:\\test-sample.exe"});

        ASSERT_EQ(run.exit_status, STATUS_SUCCESS) << run.output;
    }

    TEST_F(Arm64EmulationTest, RaiseFailFastExceptionTerminatesWithItsCode)
    {
        const auto run = this->run_application({.application = "C:\\test-sample.exe", .arguments = {u"-fail-fast"}});

        EXPECT_EQ(run.exit_status, 0xE0001234u);
    }

    TEST_F(Arm64EmulationTest, FastFailInstructionTerminatesTheProcess)
    {
        const auto run = this->run_application({.application = "C:\\test-sample.exe", .arguments = {u"-fastfail"}});

        EXPECT_EQ(run.exit_status, STATUS_FAIL_FAST_EXCEPTION);
        constexpr uint32_t fast_fail_fatal_app_exit = 7;
        EXPECT_EQ(run.fast_fail_code, fast_fail_fatal_app_exit);
    }
}

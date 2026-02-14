#include "linux_emulation_test_utils.hpp"

namespace linux_test
{
    // --- Raw syscall tests (assembly ELFs) ---

    TEST(LinuxEmulationTest, HelloWorldRawSyscall)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "hello");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        EXPECT_NE(result.captured_stdout.find("Hello"), std::string::npos);
    }

    TEST(LinuxEmulationTest, Exit42RawSyscall)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "exit42");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 42);
    }

    TEST(LinuxEmulationTest, MultitestRawSyscall)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "multitest");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        // multitest calls uname() and verifies it returns "Linux"
        EXPECT_NE(result.captured_stdout.find("Linux"), std::string::npos);
    }

    TEST(LinuxEmulationTest, SignalHandlerRecovery)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "test_signal");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        // test_signal installs SIGSEGV handler, triggers fault, recovers
        EXPECT_NE(result.captured_stdout.find("Signal recovery OK"), std::string::npos);
    }

    // --- Musl-static tests ---

    TEST(LinuxMuslStaticTest, HelloMusl)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "musl-tests" / "hello_musl");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 42);
        EXPECT_NE(result.captured_stdout.find("Hello"), std::string::npos);
    }

    TEST(LinuxMuslStaticTest, StringOperations)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "musl-tests" / "test_strings");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
    }

    TEST(LinuxMuslStaticTest, MathOperations)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "musl-tests" / "test_math");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
    }

    TEST(LinuxMuslStaticTest, FileIO)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "musl-tests" / "test_fileio");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
    }

    TEST(LinuxMuslStaticTest, ProcfsEmulation)
    {
        const auto root = get_linux_test_root();
        auto result =
            run_linux_binary(root / "musl-tests" / "test_procfs", {(root / "musl-tests" / "test_procfs").string(), "arg1", "arg2"});

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        // test_procfs outputs "Results: 7/7 passed"
        EXPECT_NE(result.captured_stdout.find("7/7 passed"), std::string::npos);
    }

    TEST(LinuxMuslStaticTest, VdsoEmulation)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "musl-tests" / "test_vdso");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        // test_vdso outputs "Results: 7/7 passed"
        EXPECT_NE(result.captured_stdout.find("7/7 passed"), std::string::npos);
    }

    // --- Dynamic linking tests ---

    TEST(LinuxDynamicLinkTest, HelloDynamic)
    {
        const auto root = get_linux_test_root();
        auto result =
            run_linux_binary(root / "dyn-tests" / "hello_dyn", {(root / "dyn-tests" / "hello_dyn").string()}, {}, root / "dynroot");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        EXPECT_NE(result.captured_stdout.find("Hello"), std::string::npos);
    }

    TEST(LinuxDynamicLinkTest, HelloPIE)
    {
        const auto root = get_linux_test_root();
        auto result =
            run_linux_binary(root / "dyn-tests" / "hello_pie", {(root / "dyn-tests" / "hello_pie").string()}, {}, root / "dynroot");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
        EXPECT_NE(result.captured_stdout.find("Hello"), std::string::npos);
    }

    TEST(LinuxDynamicLinkTest, DynamicFeatures)
    {
        const auto root = get_linux_test_root();
        auto result = run_linux_binary(root / "dyn-tests" / "test_dyn_features", {(root / "dyn-tests" / "test_dyn_features").string()}, {},
                                       root / "dynroot");

        ASSERT_TRUE(result.exit_status.has_value());
        ASSERT_EQ(*result.exit_status, 0);
    }

    // --- Instruction counting ---

    TEST(LinuxEmulationTest, CountedEmulationWorks)
    {
        const auto root = get_linux_test_root();
        constexpr size_t count = 1000;

        // Use hello_musl which executes 10K+ instructions, so we can stop mid-execution
        auto emu_backend = create_x86_64_emulator();
        const auto binary = root / "musl-tests" / "hello_musl";
        linux_emulator linux_emu(std::move(emu_backend), {}, binary, {binary.string()}, default_envp());
        linux_emu.log.disable_output(true);

        linux_emu.start(count);

        ASSERT_EQ(linux_emu.get_executed_instructions(), count);
        // Should not have terminated yet (hello_musl needs ~10K+ instructions)
        ASSERT_LINUX_NOT_TERMINATED(linux_emu);
    }
}

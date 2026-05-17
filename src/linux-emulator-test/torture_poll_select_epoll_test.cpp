#include "linux_emulation_test_utils.hpp"

namespace linux_test
{
    TEST(LinuxSyscallTortureTest, PollSelectEpollConsistency)
    {
        const auto root = get_linux_test_root();
        const auto binary = root / "torture" / "bin" / "poll_select_epoll_torture";

        ASSERT_TRUE(std::filesystem::exists(binary));

        const auto result = run_linux_binary(binary);

        ASSERT_TRUE(result.exit_status.has_value());
        EXPECT_EQ(*result.exit_status, 0) << result.captured_stdout << result.captured_stderr;
        EXPECT_NE(result.captured_stdout.find("ALL PASS poll_select_epoll_torture"), std::string::npos)
            << result.captured_stdout << result.captured_stderr;
    }
}

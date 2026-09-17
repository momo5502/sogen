#include "emulation_test_utils.hpp"

namespace sogen::test
{
    TEST(ExceptionTest, SecondChanceExceptionCodeBecomesExitStatus)
    {
        constexpr NTSTATUS sample_fail_fast_code = 0xE0001234;

        auto emu = create_sample_emulator({.fail_fast = true});
        emu.start();

        ASSERT_TERMINATED_WITH_STATUS(emu, sample_fail_fast_code);
    }
} // namespace sogen::test

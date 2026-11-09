#include "emulation_test_utils.hpp"
#include <exception_dispatch.hpp>

namespace sogen::test
{
    namespace
    {
        NTSTATUS status_of(const std::initializer_list<uint8_t> bytes)
        {
            const std::vector<uint8_t> instruction(bytes);
            return get_general_protection_fault_status(instruction, false);
        }
    }

    // Each expectation was observed on Windows 10 19045 by running the instruction in user mode under SEH.
    TEST(GeneralProtectionFaultTest, PrivilegedInstructionsRaisePrivilegedInstruction)
    {
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x0F, 0x20, 0xD0}));       // mov rax, cr2
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x0F, 0x22, 0xC0}));       // mov cr0, rax
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x66, 0x0F, 0x20, 0xD0})); // o16 mov rax, cr2
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x0F, 0x32}));             // rdmsr
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x0F, 0x09}));             // wbinvd
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x0F, 0x01, 0xF8}));       // swapgs
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x0F, 0x01, 0x10}));       // lgdt [rax]
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0xF4}));                   // hlt
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0x40, 0xF4}));             // rex hlt
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0xFA}));                   // cli
        EXPECT_EQ(STATUS_PRIVILEGED_INSTRUCTION, status_of({0xEC}));                   // in al, dx
    }

    TEST(GeneralProtectionFaultTest, OtherFaultsStayAccessViolations)
    {
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, status_of({0x48, 0x8B, 0x00})); // mov rax, [non-canonical rax]
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, status_of({0x2E, 0x2E, 0xF4})); // the repeated prefix fails the decode
    }
}

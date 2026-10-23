#include <arch/syscall_abi.hpp>
#include <backend_selection.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace sogen::test
{
#if !defined(_M_ARM64)
    TEST(SyscallAbiTest, Arm64BackendFailsExplicitlyOnUnsupportedHost)
    {
        EXPECT_THROW(create_arm64_emulator(backend_type::whp), std::runtime_error);
    }
#endif

    namespace
    {
        exported_symbol make_symbol(std::string name, const uint64_t rva)
        {
            exported_symbol s{};
            s.name = std::move(name);
            s.rva = rva;
            return s;
        }

        // Append an x86-64 syscall stub (mov r10,rcx; mov eax,id; test; jne; syscall; ret) and
        // return the rva of its first byte.
        uint64_t append_x64_stub(std::vector<std::byte>& image, const uint32_t id)
        {
            const auto rva = image.size();
            const std::array<uint8_t, 8> head{0x4C, 0x8B, 0xD1, 0xB8, 0, 0, 0, 0};
            for (const auto b : head)
            {
                image.push_back(static_cast<std::byte>(b));
            }
            std::memcpy(image.data() + rva + 4, &id, sizeof(id));
            // a few trailing bytes so the stub is realistically sized
            for (int i = 0; i < 8; ++i)
            {
                image.push_back(static_cast<std::byte>(0x90));
            }
            return rva;
        }

        // Append an AArch64 stub (svc #id; ret) and return the rva of its first byte.
        uint64_t append_arm64_stub(std::vector<std::byte>& image, const uint32_t id)
        {
            const auto rva = image.size();
            const uint32_t svc = 0xD4000001u | (id << 5);
            const uint32_t ret = 0xD65F03C0u;
            for (const auto word : {svc, ret})
            {
                for (int i = 0; i < 4; ++i)
                {
                    image.push_back(static_cast<std::byte>((word >> (i * 8)) & 0xFF));
                }
            }
            return rva;
        }
    }

    TEST(SyscallAbiTest, X64DecodesItsOwnStub)
    {
        std::vector<std::byte> image{};
        const auto rva = append_x64_stub(image, 0x0055);

        const auto id = x86_64_syscall_abi_instance().scan_syscall_id(make_symbol("NtTestCall", rva), image);
        ASSERT_TRUE(id.has_value());
        EXPECT_EQ(*id, 0x0055u);
    }

    TEST(SyscallAbiTest, Arm64DecodesItsOwnStub)
    {
        std::vector<std::byte> image{};
        const auto rva = append_arm64_stub(image, 0x002C); // NtTerminateProcess on real arm64 ntdll

        const auto id = arm64_syscall_abi_instance().scan_syscall_id(make_symbol("NtTerminateProcess", rva), image);
        ASSERT_TRUE(id.has_value());
        EXPECT_EQ(*id, 0x002Cu);
    }

    TEST(SyscallAbiTest, EachArchRejectsTheOthersStub)
    {
        std::vector<std::byte> x64{};
        const auto x64_rva = append_x64_stub(x64, 0x10);
        std::vector<std::byte> arm{};
        const auto arm_rva = append_arm64_stub(arm, 0x10);

        EXPECT_FALSE(arm64_syscall_abi_instance().scan_syscall_id(make_symbol("NtFoo", x64_rva), x64).has_value());
        EXPECT_FALSE(x86_64_syscall_abi_instance().scan_syscall_id(make_symbol("NtFoo", arm_rva), arm).has_value());
    }

    TEST(SyscallAbiTest, NonSyscallNamesAreIgnored)
    {
        std::vector<std::byte> image{};
        const auto rva = append_arm64_stub(image, 0x10);

        // Correct bytes, but the name is not an Nt*/Zw* syscall export.
        EXPECT_FALSE(arm64_syscall_abi_instance().scan_syscall_id(make_symbol("RtlFoo", rva), image).has_value());
        EXPECT_FALSE(arm64_syscall_abi_instance().scan_syscall_id(make_symbol("Nt", rva), image).has_value());
    }

    TEST(SyscallAbiTest, ZwAliasesAreRejected)
    {
        std::vector<std::byte> image{};
        const auto rva = append_arm64_stub(image, 0x42);

        // Zw* aliases share IDs with their Nt* counterparts; find_syscalls rejects a duplicate ID.
        EXPECT_FALSE(arm64_syscall_abi_instance().scan_syscall_id(make_symbol("ZwTerminateProcess", rva), image).has_value());
    }

    TEST(SyscallAbiTest, TruncatedImageDoesNotRead)
    {
        std::vector<std::byte> image{};
        append_arm64_stub(image, 0x10);
        image.resize(4); // keep the svc word, lose the ret

        EXPECT_FALSE(arm64_syscall_abi_instance().scan_syscall_id(make_symbol("NtFoo", 0), image).has_value());
    }

    TEST(SyscallAbiTest, InstructionSizesMatchTheArchitecture)
    {
        EXPECT_EQ(x86_64_syscall_abi_instance().syscall_instruction_size(), 2u);
        EXPECT_EQ(arm64_syscall_abi_instance().syscall_instruction_size(), 4u);
    }

    TEST(SyscallAbiTest, MachineTypeSelectsTheAbi)
    {
        EXPECT_STREQ(syscall_abi_for(PEMachineType::AMD64).name(), "x86_64");
        EXPECT_STREQ(syscall_abi_for(PEMachineType::I386).name(), "x86_64");
        EXPECT_STREQ(syscall_abi_for(PEMachineType::ARM64).name(), "arm64");
    }
}

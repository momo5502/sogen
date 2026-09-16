#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <bsd_syscall_dispatcher.hpp>

#include <array>
#include <utility>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    // libSystem routes through the _nocancel entry whenever the thread is not a cancellation point,
    // which is the common case -- libsystem_trace reaches open_nocancel before main(). Their numbers are
    // not adjacent to the cancellable forms they mirror, so a transcription slip is invisible until a
    // guest dies deep in library startup; these were read off this host's libsystem_kernel stubs.
    TEST(MacosSyscallDispatch, TheNocancelVariantsReachTheirHandlers)
    {
        constexpr std::array<std::pair<uint16_t, const char*>, 8> variants{{
            {396, "read_nocancel"},
            {397, "write_nocancel"},
            {398, "open_nocancel"},
            {399, "close_nocancel"},
            {406, "fcntl_nocancel"},
            {409, "connect_nocancel"},
            {414, "pread_nocancel"},
            {464, "openat_nocancel"},
        }};

        for (const auto& [number, name] : variants)
        {
            const auto emu = macos_test::make_emulator();
            macos_test::write_guest_code(*emu, code_base, {macos_test::movz_x(16, number, 0), 0xD4001001});

            emu->start(2);

            EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unimplemented_syscall) << name;
            EXPECT_NE(static_cast<int64_t>(emu->emu().reg(sogen::arm64_register::x0)), sogen::macos_errno::MACOS_ENOSYS) << name;
        }
    }

    TEST(MacosSyscallDispatch, GetpidSucceedsAndClearsCarry)
    {
        const auto emu = macos_test::make_emulator();
        emu->emu().reg(sogen::arm64_register::nzcv, carry);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800290, // mov x16, #20  (getpid)
                                         0xD4001001, // svc #0x80
                                     });

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), emu->process.pid);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u) << "success must clear the carry flag";
    }

    TEST(MacosSyscallDispatch, UnimplementedSyscallSetsCarryAndEnosys)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2802A70, // mov x16, #339 -> replaced below
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        constexpr uint32_t unknown_number = 0xD280F7D0; // mov x16, #0x7be (1982, no such syscall)
        emu->memory.write_memory(code_base, &unknown_number, sizeof(unknown_number));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 78u) << "Darwin ENOSYS is 78, not Linux's 38";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosSyscallDispatch, GuestSeesTheCarryFlagInPstate)
    {
        const auto emu = macos_test::make_emulator();

        // mmap with a zero length, which is a registered handler returning EINVAL. An unregistered
        // number would do as well for the flag, but it now halts the run, and this test needs the guest
        // to reach the cset that reads the flag back.
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD28018B0, // mov  x16, #197 (mmap)
                                         0xD4001001, // svc  #0x80
                                         0x9A9F37E3, // cset x3, cs
                                     });

        emu->start(3);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL for a zero length";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x3), 1u)
            << "the carry flag must reach guest-visible PSTATE, not just the NZCV shadow";
    }

    TEST(MacosSyscallDispatch, SuccessIsVisibleToACsetToo)
    {
        const auto emu = macos_test::make_emulator();
        emu->emu().reg(sogen::arm64_register::nzcv, carry);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800290, // mov  x16, #20 (getpid)
                                         0xD4001001, // svc  #0x80
                                         0x9A9F37E3, // cset x3, cs
                                     });

        emu->start(3);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x3), 0u);
    }

    TEST(MacosSyscallDispatch, IndirectSyscallShiftsArgumentsByOne)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800010, // mov x16, #0   (indirect)
                                         0xD2800290, // mov x0,  #20  (getpid) -- see note
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        constexpr uint32_t mov_x0_20 = 0xD2800280;
        emu->memory.write_memory(code_base + 4, &mov_x0_20, sizeof(mov_x0_20));

        emu->start(3);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), emu->process.pid);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
    }

    TEST(MacosSyscallDispatch, UnhandledMachTrapIsReportedNotMisrouted)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x92800190, // mov x16, #-13 (task_dyld_process_info_notify_get, which sogen does not register)
                                         0xD4001001, // svc #0x80
                                     });

        uint64_t observed_id = 0;
        std::string observed_name{};
        emu->callbacks.on_syscall = [&](const uint64_t id, const std::string_view name) {
            observed_id = id;
            observed_name = name;
            return sogen::instruction_hook_continuation::run_instruction;
        };

        emu->start(2);

        EXPECT_EQ(observed_id, 13u) << "mach trap index must be reported as -x16, not as the raw x16";
        EXPECT_NE(observed_name.find("mach"), std::string::npos);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosSyscallDispatch, PlatformSyscallSentinelIsNotTreatedAsAMachTrap)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800010, // mov x16, #0 -- overwritten below
                                         0xD4001001, // svc #0x80
                                     });
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        constexpr uint32_t movz_x16_0x8000_lsl16 = 0xD2B00010; // mov x16, #0x80000000
        emu->memory.write_memory(code_base, &movz_x16_0x8000_lsl16, sizeof(movz_x16_0x8000_lsl16));

        emu->start(2);

        // The sentinel now reaches sys_platform_syscall, which succeeds: the arm64 operations it
        // carries are legacy TLS and cache-maintenance assists that this backend needs no help with.
        // What this test is for is the negation -- it must not be negated into a Mach trap index, and
        // it must not fault.
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
    }

    TEST(MacosSyscallDispatch, PlatformSyscallSentinelIsReportedAsItself)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2B00010, // mov x16, #0x80000000
                                         0xD4001001, // svc #0x80
                                     });

        uint64_t observed_id = 1;
        std::string observed_name{};
        emu->callbacks.on_syscall = [&](const uint64_t id, const std::string_view name) {
            observed_id = id;
            observed_name = name;
            return sogen::instruction_hook_continuation::run_instruction;
        };

        emu->start(2);

        EXPECT_EQ(observed_id, 0u);
        EXPECT_EQ(observed_name, "platform_syscall");
    }

    TEST(MacosSyscallDispatch, CarryIsClearedBeforeAnyHandlerRuns)
    {
        const auto emu = macos_test::make_emulator();
        emu->emu().reg(sogen::arm64_register::nzcv, carry);

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800290, // mov x16, #20 (getpid)
                                         0xD4001001, // svc #0x80
                                     });

        emu->callbacks.on_syscall = [](uint64_t, std::string_view) { return sogen::instruction_hook_continuation::skip_instruction; };

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u)
            << "the trap path must clear carry before dispatch, not only inside a handler";
    }

    sogen::macos_syscall_context make_context(sogen::macos_emulator& emu, const size_t argument_offset)
    {
        return sogen::macos_syscall_context{.emu_ref = emu, .emu = emu.emu(), .proc = emu.process, .argument_offset = argument_offset};
    }

    TEST(MacosSyscallDispatch, IndirectArgumentOffsetShiftsTheRegisterWindowToX1ThroughX8)
    {
        const auto emu = macos_test::make_emulator();
        for (uint32_t i = 0; i <= 8; ++i)
        {
            emu->emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::x0) + i), 0x1000ULL + i);
        }

        const auto direct = make_context(*emu, 0);
        const auto indirect = make_context(*emu, 1);

        EXPECT_EQ(sogen::get_macos_syscall_argument(direct, 0), 0x1000u);
        EXPECT_EQ(sogen::get_macos_syscall_argument(direct, 7), 0x1007u);
        EXPECT_EQ(sogen::get_macos_syscall_argument(indirect, 0), 0x1001u);
        EXPECT_EQ(sogen::get_macos_syscall_argument(indirect, 7), 0x1008u);
    }

    TEST(MacosSyscallDispatch, ArgumentPastTheRegisterWindowStopsInsteadOfThrowing)
    {
        const auto emu = macos_test::make_emulator();
        const auto indirect = make_context(*emu, 1);

        EXPECT_EQ(sogen::get_macos_syscall_argument(indirect, 7), 0u);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);

        EXPECT_EQ(sogen::get_macos_syscall_argument(indirect, 8), 0u);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::syscall_exception);
    }

    TEST(MacosSyscallDispatch, ArgumentRegistersAreContiguousFromX0)
    {
        static_assert(static_cast<uint32_t>(sogen::arm64_register::x7) == static_cast<uint32_t>(sogen::arm64_register::x0) + 7);
        static_assert(static_cast<uint32_t>(sogen::arm64_register::x8) == static_cast<uint32_t>(sogen::arm64_register::x0) + 8);
        SUCCEED();
    }

    TEST(MacosSyscallDispatch, ANegatedErrnoReachesTheGuestUnrepaired)
    {
        const auto emu = macos_test::make_emulator();
        const sogen::macos_syscall_context context{.emu_ref = *emu, .emu = emu->emu(), .proc = emu->process, .argument_offset = 0};

        sogen::write_macos_syscall_error(context, -sogen::macos_errno::MACOS_EINVAL);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(-sogen::macos_errno::MACOS_EINVAL))
            << "the negated value must reach the guest unchanged; a guard that repairs is not a guard that detects";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);

        sogen::write_macos_syscall_error(context, sogen::macos_errno::MACOS_EINVAL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
    }
}

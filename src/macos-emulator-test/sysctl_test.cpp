#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <syscalls/sysctl_table.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    constexpr uint32_t mov_x16_sysctl = 0xD2801950;
    constexpr uint32_t mov_x16_sysctlbyname = 0xD2802250;

    TEST(MacosSysctl, HardwareIdentityComesFromTheSystemInfo)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        const auto ncpu = sogen::resolve_macos_sysctl("hw.ncpu", info, proc);
        ASSERT_TRUE(ncpu.has_value());
        EXPECT_EQ(ncpu->number, info.ncpus);

        const auto memsize = sogen::resolve_macos_sysctl("hw.memsize", info, proc);
        ASSERT_TRUE(memsize.has_value());
        EXPECT_EQ(memsize->number, info.memory_size) << "hw.memsize and _COMM_PAGE_MEMORY_SIZE must agree; a guest cross-checks them";

        const auto pagesize = sogen::resolve_macos_sysctl("hw.pagesize", info, proc);
        ASSERT_TRUE(pagesize.has_value());
        EXPECT_EQ(pagesize->number, sogen::MACOS_PAGE_SIZE);

        const auto cpufamily = sogen::resolve_macos_sysctl("hw.cpufamily", info, proc);
        ASSERT_TRUE(cpufamily.has_value());
        EXPECT_EQ(cpufamily->number, info.cpufamily);
    }

    TEST(MacosSysctl, PointerAuthenticationIsReportedFromOurOwnTable)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        const auto pauth = sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_PAuth", info, proc);
        ASSERT_TRUE(pauth.has_value());
        EXPECT_EQ(pauth->number, 1u);

        const auto lse = sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_LSE", info, proc);
        ASSERT_TRUE(lse.has_value());
        EXPECT_EQ(lse->number, 1u);

        const auto arm64 = sogen::resolve_macos_sysctl("hw.optional.arm64", info, proc);
        ASSERT_TRUE(arm64.has_value());
        EXPECT_EQ(arm64->number, 1u);
    }

    TEST(MacosSysctl, KernelStringsAreReportedAsStrings)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        const auto ostype = sogen::resolve_macos_sysctl("kern.ostype", info, proc);
        ASSERT_TRUE(ostype.has_value());
        EXPECT_EQ(ostype->value_kind, sogen::macos_sysctl_value::kind::string);
        EXPECT_EQ(ostype->text, "Darwin");

        const auto machine = sogen::resolve_macos_sysctl("hw.machine", info, proc);
        ASSERT_TRUE(machine.has_value());
        EXPECT_EQ(machine->text, "arm64");
    }

    TEST(MacosSysctl, MibsResolveToTheSameNames)
    {
        constexpr auto hw_memsize =
            std::to_array<int32_t>({sogen::macos_sysctl_mib::MACOS_CTL_HW, sogen::macos_sysctl_mib::MACOS_HW_MEMSIZE});
        constexpr auto kern_osrelease =
            std::to_array<int32_t>({sogen::macos_sysctl_mib::MACOS_CTL_KERN, sogen::macos_sysctl_mib::MACOS_KERN_OSRELEASE});

        EXPECT_EQ(sogen::macos_sysctl_mib_to_name(hw_memsize).value_or(""), "hw.memsize");
        EXPECT_EQ(sogen::macos_sysctl_mib_to_name(kern_osrelease).value_or(""), "kern.osrelease");
    }

    TEST(MacosSysctl, UnknownNamesAreReportedAsAbsent)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        EXPECT_FALSE(sogen::resolve_macos_sysctl("kern.does_not_exist", info, proc).has_value());
    }

    TEST(MacosSysctl, EveryMappedMibResolvesToItsOwnName)
    {
        using namespace sogen::macos_sysctl_mib; // NOLINT(google-build-using-namespace)

        const std::vector<std::pair<std::array<int32_t, 2>, std::string>> expected{
            {{MACOS_CTL_HW, MACOS_HW_MACHINE}, "hw.machine"},
            {{MACOS_CTL_HW, MACOS_HW_MODEL}, "hw.model"},
            {{MACOS_CTL_HW, MACOS_HW_NCPU}, "hw.ncpu"},
            {{MACOS_CTL_HW, MACOS_HW_BYTEORDER}, "hw.byteorder"},
            {{MACOS_CTL_HW, MACOS_HW_PAGESIZE}, "hw.pagesize"},
            {{MACOS_CTL_HW, MACOS_HW_CACHELINE}, "hw.cachelinesize"},
            {{MACOS_CTL_HW, MACOS_HW_MEMSIZE}, "hw.memsize"},
            {{MACOS_CTL_HW, MACOS_HW_AVAILCPU}, "hw.availcpu"},
            {{MACOS_CTL_KERN, MACOS_KERN_OSTYPE}, "kern.ostype"},
            {{MACOS_CTL_KERN, MACOS_KERN_OSRELEASE}, "kern.osrelease"},
            {{MACOS_CTL_KERN, MACOS_KERN_OSREV}, "kern.osrevision"},
            {{MACOS_CTL_KERN, MACOS_KERN_VERSION}, "kern.version"},
            {{MACOS_CTL_KERN, MACOS_KERN_ARGMAX}, "kern.argmax"},
            {{MACOS_CTL_KERN, MACOS_KERN_HOSTNAME}, "kern.hostname"},
            {{MACOS_CTL_KERN, MACOS_KERN_USRSTACK64}, "kern.usrstack64"},
            {{MACOS_CTL_KERN, MACOS_KERN_OSVERSION}, "kern.osversion"},
        };

        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        for (const auto& [mib, name] : expected)
        {
            EXPECT_EQ(sogen::macos_sysctl_mib_to_name(mib).value_or(""), name);
            EXPECT_TRUE(sogen::resolve_macos_sysctl(name, info, proc).has_value()) << name << " resolves to a MIB but has no value";
        }
    }

    TEST(MacosSysctl, UnmappedMibsAndMalformedLengthsResolveToNothing)
    {
        using namespace sogen::macos_sysctl_mib; // NOLINT(google-build-using-namespace)

        constexpr auto machdep = std::to_array<int32_t>({MACOS_CTL_MACHDEP, 1});
        constexpr auto unmapped_kern = std::to_array<int32_t>({MACOS_CTL_KERN, MACOS_KERN_PROCARGS2});
        constexpr auto unmapped_hw = std::to_array<int32_t>({MACOS_CTL_HW, 99});
        constexpr auto single = std::to_array<int32_t>({MACOS_CTL_HW});
        constexpr auto triple = std::to_array<int32_t>({MACOS_CTL_KERN, MACOS_KERN_PROC, 1});

        EXPECT_FALSE(sogen::macos_sysctl_mib_to_name(machdep).has_value());
        EXPECT_FALSE(sogen::macos_sysctl_mib_to_name(unmapped_kern).has_value());
        EXPECT_FALSE(sogen::macos_sysctl_mib_to_name(unmapped_hw).has_value());
        EXPECT_FALSE(sogen::macos_sysctl_mib_to_name(single).has_value());
        EXPECT_FALSE(sogen::macos_sysctl_mib_to_name(triple).has_value());
        EXPECT_FALSE(sogen::macos_sysctl_mib_to_name(std::span<const int32_t>{}).has_value());
    }

    TEST(MacosSysctl, UnmodelledOptionalFeaturesAnswerZeroInsteadOfAbsent)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        const auto sme = sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_SME", info, proc);
        ASSERT_TRUE(sme.has_value()) << "Darwin answers an unset optional feature with zero, not ENOENT";
        EXPECT_EQ(sme->number, 0u);

        const auto bti = sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_BTI", info, proc);
        ASSERT_TRUE(bti.has_value());
        EXPECT_EQ(bti->number, 0u) << "FEAT_BTI comes from the system info, which does not model it";

        sogen::macos_system_info bti_capable{};
        bti_capable.has_feat_bti = true;
        const auto enabled = sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_BTI", bti_capable, proc);
        ASSERT_TRUE(enabled.has_value());
        EXPECT_EQ(enabled->number, 1u);
    }

    TEST(MacosSysctl, FeatureFlagsTrackTheSystemInfoRatherThanConstants)
    {
        const sogen::macos_process_context proc{};

        sogen::macos_system_info stripped{};
        stripped.has_feat_pauth = false;
        stripped.has_feat_lse = false;
        stripped.has_feat_fp16 = false;

        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_PAuth", stripped, proc)->number, 0u);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_LSE", stripped, proc)->number, 0u);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.optional.arm.FEAT_FP16", stripped, proc)->number, 0u);

        sogen::macos_system_info renamed{};
        renamed.machine = "arm64e";
        renamed.model = "Mac99,9";
        renamed.os_release = "1.2.3";
        renamed.memory_size = 0x1234000ULL;
        renamed.cpufamily = 0xDEADBEEFU;

        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.machine", renamed, proc)->text, "arm64e");
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.model", renamed, proc)->text, "Mac99,9");
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.osrelease", renamed, proc)->text, "1.2.3");
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.memsize", renamed, proc)->number, 0x1234000ULL);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.cpufamily", renamed, proc)->number, 0xDEADBEEFU);
    }

    // The line is not "unimplemented", it is "would describe the analyst's machine". A name answered from
    // a constant tells the guest about the emulated machine and leaks nothing; a name that could only be
    // answered by asking the host is refused however convenient it would be. kern.bootargs sits on the
    // near side of that line and is answered as empty elsewhere in this file, which is true of the
    // machine being emulated and true of nobody's real one in particular.
    TEST(MacosSysctl, NamesOnlyTheHostCouldAnswerAreNotInOurTable)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        for (const std::string_view name :
             {"kern.boottime", "machdep.cpu.brand_string", "hw.targettype", "hw.perflevel0.physicalcpu", "vm.swapusage",
              "net.inet.ip.forwarding", "security.mac.amfi.developer_mode_status", "user.cs_path"})
        {
            EXPECT_FALSE(sogen::resolve_macos_sysctl(name, info, proc).has_value())
                << name << " must never resolve; the guest would be reading the analyst's own machine";
        }
    }

    TEST(MacosSysctl, ValueKindsFollowTheDarwinTypes)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.memsize", info, proc)->value_kind, sogen::macos_sysctl_value::kind::integer64);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.cachelinesize", info, proc)->value_kind, sogen::macos_sysctl_value::kind::integer64);
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.usrstack64", info, proc)->value_kind, sogen::macos_sysctl_value::kind::integer64);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.ncpu", info, proc)->value_kind, sogen::macos_sysctl_value::kind::integer32);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.pagesize", info, proc)->value_kind, sogen::macos_sysctl_value::kind::integer32);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.cpufamily", info, proc)->value_kind, sogen::macos_sysctl_value::kind::integer32);
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.version", info, proc)->value_kind, sogen::macos_sysctl_value::kind::string);
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.version", info, proc)->text, "Darwin Kernel Version " + info.os_release);
        EXPECT_EQ(sogen::resolve_macos_sysctl("hw.byteorder", info, proc)->number, 1234u);
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.argmax", info, proc)->number, 1048576u);
    }

    TEST(MacosSysctl, UserStackTopFollowsTheProcessStack)
    {
        const sogen::macos_system_info info{};

        sogen::macos_process_context unstarted{};
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.usrstack64", info, unstarted)->number, sogen::MACOS_MAIN_STACK_TOP);

        sogen::macos_process_context started{};
        started.stack_base = 0x40000000ULL;
        started.stack_size = 0x8000ULL;
        EXPECT_EQ(sogen::resolve_macos_sysctl("kern.usrstack64", info, started)->number, 0x40008000ULL);
    }

    TEST(MacosSysctl, GuestSysctlbynameWritesTheValueAndLength)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr std::string_view name = "hw.memsize";
        emu->memory.write_memory(data_base, name.data(), name.size() + 1);

        constexpr uint64_t out_value = data_base + 0x100;
        constexpr uint64_t out_length = data_base + 0x200;
        const uint64_t initial_length = sizeof(uint64_t);
        emu->memory.write_memory(out_length, &initial_length, sizeof(initial_length));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctlbyname, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(name.size()));
        emu->emu().reg(sogen::arm64_register::x2, out_value);
        emu->emu().reg(sogen::arm64_register::x3, out_length);
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        uint64_t reported{};
        emu->memory.read_memory(out_value, &reported, sizeof(reported));
        EXPECT_EQ(reported, emu->system_info.memory_size);

        uint64_t reported_length{};
        emu->memory.read_memory(out_length, &reported_length, sizeof(reported_length));
        EXPECT_EQ(reported_length, sizeof(uint64_t));
    }

    TEST(MacosSysctl, GuestSysctlbynameOnAnUnknownNameFailsWithEnoent)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr std::string_view name = "kern.nonsense";
        emu->memory.write_memory(data_base, name.data(), name.size() + 1);

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctlbyname, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(name.size()));
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 2u) << "ENOENT";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
    }

    TEST(MacosSysctl, GuestSysctlAnswersAMibFromTheSameTable)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr auto mib = std::to_array<int32_t>({sogen::macos_sysctl_mib::MACOS_CTL_HW, sogen::macos_sysctl_mib::MACOS_HW_MEMSIZE});
        emu->memory.write_memory(data_base, mib.data(), sizeof(mib));

        constexpr uint64_t out_value = data_base + 0x100;
        constexpr uint64_t out_length = data_base + 0x200;
        const uint64_t initial_length = sizeof(uint64_t);
        emu->memory.write_memory(out_length, &initial_length, sizeof(initial_length));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctl, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{2});
        emu->emu().reg(sogen::arm64_register::x2, out_value);
        emu->emu().reg(sogen::arm64_register::x3, out_length);
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});

        emu->start(2);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        uint64_t reported{};
        emu->memory.read_memory(out_value, &reported, sizeof(reported));
        EXPECT_EQ(reported, emu->system_info.memory_size);
    }

    TEST(MacosSysctl, GuestSysctlOnAnUnmappedMibFailsWithEnoent)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr auto mib = std::to_array<int32_t>({sogen::macos_sysctl_mib::MACOS_CTL_MACHDEP, 1});
        emu->memory.write_memory(data_base, mib.data(), sizeof(mib));

        constexpr uint64_t out_value = data_base + 0x100;
        constexpr uint64_t out_length = data_base + 0x200;
        const uint64_t initial_length = 64;
        emu->memory.write_memory(out_length, &initial_length, sizeof(initial_length));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctl, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{2});
        emu->emu().reg(sogen::arm64_register::x2, out_value);
        emu->emu().reg(sogen::arm64_register::x3, out_length);
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 2u) << "ENOENT; a guest MIB must never reach the host sysctl";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);

        uint64_t untouched{};
        emu->memory.read_memory(out_value, &untouched, sizeof(untouched));
        EXPECT_EQ(untouched, 0u);
    }

    TEST(MacosSysctl, GuestSysctlRejectsAnOutOfRangeMibLength)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctl, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0xFFFFFFFFULL});
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 22u) << "EINVAL";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::syscall_exception);
    }

    TEST(MacosSysctl, GuestSysctlbynameSizeQueryReportsTheLengthWithoutWritingTheValue)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr std::string_view name = "hw.machine";
        emu->memory.write_memory(data_base, name.data(), name.size() + 1);

        constexpr uint64_t out_value = data_base + 0x100;
        constexpr uint64_t out_length = data_base + 0x200;
        const uint64_t initial_length = 0;
        emu->memory.write_memory(out_length, &initial_length, sizeof(initial_length));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctlbyname, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(name.size()));
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x3, out_length);
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});

        emu->start(2);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u);

        uint64_t reported_length{};
        emu->memory.read_memory(out_length, &reported_length, sizeof(reported_length));
        EXPECT_EQ(reported_length, sizeof("arm64")) << "a string sysctl reports its length including the terminator";

        uint64_t untouched{};
        emu->memory.read_memory(out_value, &untouched, sizeof(untouched));
        EXPECT_EQ(untouched, 0u);
    }

    TEST(MacosSysctl, GuestSysctlbynameWithATooSmallBufferReportsEnomemAndTheNeededSize)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr std::string_view name = "hw.memsize";
        emu->memory.write_memory(data_base, name.data(), name.size() + 1);

        constexpr uint64_t out_value = data_base + 0x100;
        constexpr uint64_t out_length = data_base + 0x200;
        const uint64_t initial_length = sizeof(uint32_t);
        emu->memory.write_memory(out_length, &initial_length, sizeof(initial_length));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctlbyname, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(name.size()));
        emu->emu().reg(sogen::arm64_register::x2, out_value);
        emu->emu().reg(sogen::arm64_register::x3, out_length);
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 12u) << "ENOMEM";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);

        uint64_t reported_length{};
        emu->memory.read_memory(out_length, &reported_length, sizeof(reported_length));
        EXPECT_EQ(reported_length, sizeof(uint64_t));

        uint64_t untouched{};
        emu->memory.read_memory(out_value, &untouched, sizeof(untouched));
        EXPECT_EQ(untouched, 0u);
    }

    TEST(MacosSysctl, GuestSysctlbynameRefusesToWriteEmulatorIdentity)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr std::string_view name = "kern.hostname";
        emu->memory.write_memory(data_base, name.data(), name.size() + 1);

        constexpr uint64_t new_value = data_base + 0x100;
        constexpr std::string_view replacement = "attacker";
        emu->memory.write_memory(new_value, replacement.data(), replacement.size() + 1);

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctlbyname, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, static_cast<uint64_t>(name.size()));
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x4, new_value);
        emu->emu().reg(sogen::arm64_register::x5, static_cast<uint64_t>(replacement.size() + 1));

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 1u) << "EPERM";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);

        const auto hostname = sogen::resolve_macos_sysctl("kern.hostname", emu->system_info, emu->process);
        ASSERT_TRUE(hostname.has_value());
        EXPECT_EQ(hostname->text, "sogen");
    }

    TEST(MacosSysctl, GuestSysctlbynameWithAnUnboundedNameLengthIsRejected)
    {
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        macos_test::write_guest_code(*emu, code_base, {mov_x16_sysctlbyname, 0xD4001001});
        emu->emu().reg(sogen::arm64_register::x0, data_base);
        emu->emu().reg(sogen::arm64_register::x1, uint64_t{0xFFFFFFFFFFFFFFFFULL});
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});

        emu->start(2);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 63u) << "ENAMETOOLONG";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & carry, carry);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::syscall_exception);
    }

    // CoreFoundation asks kern.proc.pid for its own process while it works out who it is, and treats a
    // failure as fatal: __CFGetUGIDs aborts, which looks from outside like the program calling abort()
    // for its own reasons. The offsets below were measured from the SDK's own headers rather than
    // reconstructed, because kinfo_proc is 648 bytes of nested unions and getting one field wrong gives
    // a plausible answer that is silently untrue.
    TEST(MacosSysctl, KernProcPidDescribesThisProcess)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = data_base;
        ASSERT_TRUE(emu->memory.allocate_memory(out, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        // CTL_KERN, KERN_PROC, KERN_PROC_PID, pid
        const std::array<int32_t, 4> mib{1, 14, 1, static_cast<int32_t>(emu->process.pid)};
        emu->memory.write_memory(out, mib.data(), sizeof(mib));

        constexpr uint64_t size_at = out + 0x100;
        constexpr uint64_t buffer_at = out + 0x200;
        uint64_t size = 648;
        emu->memory.write_memory(size_at, &size, sizeof(size));

        macos_test::write_guest_code(*emu, code_base, macos_test::syscall_sequence(202, {out, 4, buffer_at, size_at, 0, 0}));
        emu->start(16);

        ASSERT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "kern.proc.pid must answer for this process";

        emu->memory.read_memory(size_at, &size, sizeof(size));
        EXPECT_EQ(size, 648u) << "one kinfo_proc";

        const auto read32 = [&](const uint64_t offset) {
            uint32_t value{};
            emu->memory.read_memory(buffer_at + offset, &value, sizeof(value));
            return value;
        };

        EXPECT_EQ(read32(40), emu->process.pid) << "kp_proc.p_pid";
        EXPECT_EQ(read32(560), emu->process.ppid) << "kp_eproc.e_ppid";
        EXPECT_EQ(read32(420), emu->process.euid) << "kp_eproc.e_ucred.cr_uid";
        EXPECT_EQ(read32(392), emu->process.uid) << "kp_eproc.e_pcred.p_ruid";
        EXPECT_EQ(read32(400), emu->process.gid) << "kp_eproc.e_pcred.p_rgid";

        uint8_t stat{};
        emu->memory.read_memory(buffer_at + 36, &stat, sizeof(stat));
        EXPECT_EQ(stat, 2u) << "SRUN: a process reported as anything else is not running";
    }

    // The size query comes first in every caller: pass a null buffer, learn how much to allocate, then
    // ask again. Reporting nothing there makes the caller allocate nothing and read into it.
    TEST(MacosSysctl, KernProcPidReportsItsSizeWhenAskedWithNoBuffer)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = data_base;
        ASSERT_TRUE(emu->memory.allocate_memory(out, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const std::array<int32_t, 4> mib{1, 14, 1, static_cast<int32_t>(emu->process.pid)};
        emu->memory.write_memory(out, mib.data(), sizeof(mib));

        constexpr uint64_t size_at = out + 0x100;
        uint64_t size = 0;
        emu->memory.write_memory(size_at, &size, sizeof(size));

        macos_test::write_guest_code(*emu, code_base, macos_test::syscall_sequence(202, {out, 4, 0, size_at, 0, 0}));
        emu->start(16);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);

        emu->memory.read_memory(size_at, &size, sizeof(size));
        EXPECT_EQ(size, 648u);
    }

    TEST(MacosSysctl, KernProcPidRefusesAnotherProcess)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t out = data_base;
        ASSERT_TRUE(emu->memory.allocate_memory(out, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const std::array<int32_t, 4> mib{1, 14, 1, static_cast<int32_t>(emu->process.pid) + 1};
        emu->memory.write_memory(out, mib.data(), sizeof(mib));

        constexpr uint64_t size_at = out + 0x100;
        uint64_t size = 648;
        emu->memory.write_memory(size_at, &size, sizeof(size));

        macos_test::write_guest_code(*emu, code_base, macos_test::syscall_sequence(202, {out, 4, out + 0x200, size_at, 0, 0}));
        emu->start(16);

        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & carry, 0u)
            << "a pid that is not this one is a question about a machine that is not being modelled";
    }

    // Both appeared as ENOENT in a real run: dyld reads kern.bootargs looking for anything that would
    // change how it behaves, and libsystem asks whether Lockdown Mode is on. An absent node is not the
    // same answer as a present one holding nothing -- the first says the kernel has no such concept.
    TEST(MacosSysctl, BootArgsAndLockdownModeAreAnswered)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        const auto bootargs = sogen::resolve_macos_sysctl("kern.bootargs", info, proc);
        ASSERT_TRUE(bootargs.has_value()) << "an absent node reads as a kernel without boot-args at all";
        EXPECT_TRUE(bootargs->text.empty()) << "a machine booted normally has none";

        const auto lockdown = sogen::resolve_macos_sysctl("security.mac.lockdown_mode_state", info, proc);
        ASSERT_TRUE(lockdown.has_value());
        EXPECT_EQ(lockdown->number, 0u) << "off";
    }

    // A name with no static oid still has to survive the round trip a guest actually makes: ask
    // name2oid, then query by the number it was given. If the two halves disagree the guest gets ENOENT
    // for a name the emulator can answer, which is what kern.bootargs did.
    TEST(MacosSysctl, DynamicNamesSurviveTheNumberTheGuestIsGiven)
    {
        for (const auto name : sogen::MACOS_SYSCTL_DYNAMIC_NAMES)
        {
            const sogen::macos_system_info info{};
            const sogen::macos_process_context proc{};

            ASSERT_TRUE(sogen::resolve_macos_sysctl(name, info, proc).has_value()) << name << " is registered as dynamic but has no answer";

            const auto emu = macos_test::make_emulator();

            constexpr uint64_t out = data_base;
            ASSERT_TRUE(emu->memory.allocate_memory(out, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

            const std::array<int32_t, 2> name_to_oid{0, 3};
            emu->memory.write_memory(out, name_to_oid.data(), sizeof(name_to_oid));
            emu->memory.write_memory(out + 0x80, name.data(), name.size() + 1);

            uint64_t capacity = 64;
            emu->memory.write_memory(out + 0x100, &capacity, sizeof(capacity));

            macos_test::write_guest_code(
                *emu, code_base, macos_test::syscall_sequence(202, {out, 2, out + 0x200, out + 0x100, out + 0x80, name.size() + 1}));
            emu->start(16);

            ASSERT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "name2oid refused " << name;

            uint64_t written = 0;
            emu->memory.read_memory(out + 0x100, &written, sizeof(written));
            ASSERT_EQ(written, 2 * sizeof(int32_t)) << "a two-part oid";

            std::array<int32_t, 2> oid{};
            emu->memory.read_memory(out + 0x200, oid.data(), sizeof(oid));

            const auto back = sogen::macos_sysctl_mib_to_name(oid);
            ASSERT_TRUE(back.has_value()) << "the number handed out does not resolve back";
            EXPECT_EQ(*back, name) << "the round trip landed on a different node";
        }
    }

    // The invariant behind the dynamic list: a name the resolver answers must also be findable by
    // name2oid, or a guest that looks it up by name gets ENOENT for something the emulator knows. Every
    // entry below was seen in a real trace doing exactly that.
    TEST(MacosSysctl, EveryNameSeenInARealRunIsReachableByName)
    {
        const sogen::macos_system_info info{};
        const sogen::macos_process_context proc{};

        for (const std::string_view name :
             {"kern.bootargs", "kern.osproductversion", "kern.osvariant_status", "kern.iossupportversion", "kern.hv_support",
              "kern.secure_kernel", "security.mac.lockdown_mode_state", "hw.optional.arm.caps", "hw.ephemeral_storage", "hw.cpufamily",
              "hw.activecpu", "hw.physicalcpu", "hw.logicalcpu", "hw.optional.arm64", "hw.optional.neon"})
        {
            EXPECT_TRUE(sogen::resolve_macos_sysctl(name, info, proc).has_value()) << name << " has no answer";

            const auto listed = std::ranges::find(sogen::MACOS_SYSCTL_DYNAMIC_NAMES, name) != sogen::MACOS_SYSCTL_DYNAMIC_NAMES.end();
            const auto has_static_mib = [&] {
                for (const int32_t root : {1, 6})
                {
                    for (int32_t leaf = 0; leaf < 256; ++leaf)
                    {
                        const std::array<int32_t, 2> mib{root, leaf};
                        const auto resolved = sogen::macos_sysctl_mib_to_name(mib);
                        if (resolved.has_value() && *resolved == name)
                        {
                            return true;
                        }
                    }
                }
                return false;
            }();

            EXPECT_TRUE(listed || has_static_mib) << name << " resolves but name2oid cannot find it, so a guest asking by name gets ENOENT";
        }
    }
}

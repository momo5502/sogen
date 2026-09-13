#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <guest/guest_memory_object.hpp>
#include <serialization.hpp>

namespace
{
    TEST(MacosProcessContext, SetsUpAnAlignedStackWithArgcAtTheStackPointer)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello", "one", "two"}, {"PATH=/usr/bin"},
                           "/bin/hello");

        const auto sp = emu->emu().reg(sogen::arm64_register::sp);
        EXPECT_NE(sp, 0u);
        EXPECT_EQ(sp % 16, 0u);
        EXPECT_LT(sp, sogen::MACOS_MAIN_STACK_TOP);
        EXPECT_GE(sp, sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE);

        uint64_t argc{};
        emu->memory.read_memory(sp, &argc, sizeof(argc));
        EXPECT_EQ(argc, 3u);
    }

    TEST(MacosProcessContext, ArgvEnvpAndAppleVectorsAreNullTerminatedInOrder)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello", "one"}, {"PATH=/usr/bin"}, "/bin/hello");

        const auto sp = emu->emu().reg(sogen::arm64_register::sp);

        const auto read_pointer = [&](const size_t slot) {
            uint64_t value{};
            emu->memory.read_memory(sp + slot * sizeof(uint64_t), &value, sizeof(value));
            return value;
        };

        const auto read_string_at = [&](const uint64_t address) { return sogen::read_guest_string<char>(emu->memory, address); };

        EXPECT_EQ(read_string_at(read_pointer(1)), "/bin/hello");
        EXPECT_EQ(read_string_at(read_pointer(2)), "one");
        EXPECT_EQ(read_pointer(3), 0u) << "argv must be NULL terminated";
        EXPECT_EQ(read_string_at(read_pointer(4)), "PATH=/usr/bin");
        EXPECT_EQ(read_pointer(5), 0u) << "envp must be NULL terminated";
        EXPECT_EQ(read_string_at(read_pointer(6)).rfind("executable_path=", 0), 0u);
    }

    TEST(MacosProcessContext, EntryPointBecomesTheProgramCounter)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, 0x100000200ULL, {"/bin/hello"}, {}, "/bin/hello");

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), 0x100000200ULL);
    }

    TEST(MacosProcessContext, ThreadSelfLivesInTpidrroNotTpidr)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");

        ASSERT_NE(emu->process.active_thread, nullptr);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::tpidrro_el0), emu->process.active_thread->thread_self);
    }

    TEST(MacosProcessContext, SavedRegistersRoundTrip)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        emu->emu().reg(sogen::arm64_register::x5, 0xAABBCCDDEEFF0011ULL);
        emu->emu().reg(sogen::arm64_register::x28, 0x1122334455667788ULL);
        emu->process.active_thread->save(emu->emu());

        emu->emu().reg(sogen::arm64_register::x5, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x28, uint64_t{0});
        emu->process.active_thread->restore(emu->emu());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x5), 0xAABBCCDDEEFF0011ULL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x28), 0x1122334455667788ULL);
    }

    TEST(MacosProcessContext, StackIsMappedReadWriteAndNotExecutable)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");

        const auto info = emu->memory.get_region_info(sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_PAGE_SIZE);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_write);
    }

    TEST(MacosProcessContext, RefusesVectorsTooLargeForTheStackInsteadOfWritingPastIt)
    {
        const auto emu = macos_test::make_emulator();

        const std::string oversized(sogen::MACOS_MAIN_STACK_SIZE + 1, 'a');
        EXPECT_NO_THROW(emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {oversized}, {}, "/bin/hello"));

        EXPECT_EQ(emu->process.active_thread, nullptr);
        EXPECT_EQ(emu->process.stack_base, 0u);
        EXPECT_EQ(emu->process.stack_size, 0u);
        EXPECT_FALSE(emu->memory.get_region_info(sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_PAGE_SIZE).has_value());
    }

    TEST(MacosProcessContext, DefaultsToTheFirstRegularDarwinUserAndStaffGroup)
    {
        const sogen::macos_process_context process{};

        EXPECT_EQ(process.uid, 501u);
        EXPECT_EQ(process.gid, 20u);
        EXPECT_EQ(process.euid, 501u);
        EXPECT_EQ(process.egid, 20u);
        // Not 1: dyld reads the pid and, seeing launchd's, runs libignition to boot the system instead
        // of launching the executable. ppid 1 is correct and unrelated -- launchd is the parent.
        EXPECT_NE(process.pid, 1u);
        EXPECT_EQ(process.ppid, 1u);
        EXPECT_EQ(process.current_working_directory, "/");
    }

    TEST(MacosProcessContext, ThreadSelfIsANonNullMappedTokenAndTpidrIsUntouched)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");

        ASSERT_NE(emu->process.active_thread, nullptr);
        EXPECT_NE(emu->process.active_thread->thread_self, 0u);
        EXPECT_TRUE(emu->memory.get_region_info(emu->process.active_thread->thread_self).has_value());
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::tpidr_el0), 0u);
    }

    TEST(MacosProcessContext, AppleVectorCarriesExactlyExecutablePathAndADeterministicStackGuard)
    {
        constexpr std::string_view stack_guard_prefix = "stack_guard=0x";

        const auto emu = macos_test::make_emulator();
        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");

        ASSERT_EQ(emu->process.apple.size(), 2u);
        EXPECT_EQ(emu->process.apple[0], "executable_path=/bin/hello");
        EXPECT_EQ(emu->process.apple[1].rfind(stack_guard_prefix, 0), 0u);
        EXPECT_EQ(emu->process.apple[1].size(), stack_guard_prefix.size() + 16);

        const auto sp = emu->emu().reg(sogen::arm64_register::sp);

        const auto read_pointer = [&](const size_t slot) {
            uint64_t value{};
            emu->memory.read_memory(sp + slot * sizeof(uint64_t), &value, sizeof(value));
            return value;
        };

        EXPECT_EQ(sogen::read_guest_string<char>(emu->memory, read_pointer(4)), emu->process.apple[0]);
        EXPECT_EQ(sogen::read_guest_string<char>(emu->memory, read_pointer(5)), emu->process.apple[1]);
        EXPECT_EQ(read_pointer(6), 0u) << "apple must be NULL terminated";

        const auto other = macos_test::make_emulator();
        other->process.setup(other->emu(), other->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");

        EXPECT_EQ(other->process.apple[1], emu->process.apple[1]);
    }

    TEST(MacosProcessContext, SavedRegistersRoundTripCoversTheSpecialRegisters)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        emu->emu().reg(sogen::arm64_register::x29, 0x1111111111110000ULL);
        emu->emu().reg(sogen::arm64_register::x30, 0x2222222222220000ULL);
        emu->emu().reg(sogen::arm64_register::sp, 0x16FBFFF000ULL);
        emu->emu().reg(sogen::arm64_register::pc, 0x100004444ULL);
        emu->emu().reg(sogen::arm64_register::nzcv, 0xA0000000ULL);
        emu->emu().reg(sogen::arm64_register::tpidrro_el0, 0x5555555555550000ULL);
        emu->emu().reg(sogen::arm64_register::tpidr_el0, 0x6666666666660000ULL);

        const auto nzcv = emu->emu().reg(sogen::arm64_register::nzcv);
        ASSERT_NE(nzcv, 0u);

        emu->process.active_thread->save(emu->emu());

        emu->emu().reg(sogen::arm64_register::x29, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::x30, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::sp, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::pc, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::nzcv, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::tpidrro_el0, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::tpidr_el0, uint64_t{0});

        emu->process.active_thread->restore(emu->emu());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x29), 0x1111111111110000ULL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x30), 0x2222222222220000ULL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::sp), 0x16FBFFF000ULL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), 0x100004444ULL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv), nzcv);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::tpidrro_el0), 0x5555555555550000ULL);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::tpidr_el0), 0x6666666666660000ULL);
    }

    TEST(MacosProcessContext, SavedRegistersRoundTripTheVectorState)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        using vector_value = std::array<uint64_t, 2>;

        const vector_value v0{0x0102030405060708ULL, 0x090A0B0C0D0E0F00ULL};
        const vector_value v8{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
        const vector_value v31{0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL};

        emu->emu().reg<vector_value>(sogen::arm64_register::v0, v0);
        emu->emu().reg<vector_value>(sogen::arm64_register::v8, v8);
        emu->emu().reg<vector_value>(sogen::arm64_register::v31, v31);
        emu->emu().reg<uint32_t>(sogen::arm64_register::fpcr, 0x01000000u);

        emu->process.active_thread->save(emu->emu());

        emu->emu().reg<vector_value>(sogen::arm64_register::v0, vector_value{});
        emu->emu().reg<vector_value>(sogen::arm64_register::v8, vector_value{});
        emu->emu().reg<vector_value>(sogen::arm64_register::v31, vector_value{});
        emu->emu().reg<uint32_t>(sogen::arm64_register::fpcr, uint32_t{0});

        emu->process.active_thread->restore(emu->emu());

        EXPECT_EQ(emu->emu().reg<vector_value>(sogen::arm64_register::v0), v0);
        EXPECT_EQ(emu->emu().reg<vector_value>(sogen::arm64_register::v8), v8);
        EXPECT_EQ(emu->emu().reg<vector_value>(sogen::arm64_register::v31), v31);
        EXPECT_EQ(emu->emu().reg<uint32_t>(sogen::arm64_register::fpcr), 0x01000000u);
    }

    // d8..d15 are callee-saved by AAPCS64, so a value in them is live across the svc a thread parks on.
    // A switch that leaves the other thread's vector registers behind hands that thread back a number it
    // never wrote.
    TEST(MacosProcessContext, SwitchingThreadsKeepsEachThreadsVectorState)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello"}, {}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        using vector_value = std::array<uint64_t, 2>;

        const auto first = emu->process.active_thread->thread_id;
        const auto second = emu->process.create_thread(0x300010000ULL, 0x4000, 0x100005000ULL);

        const vector_value first_value{0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL};
        const vector_value second_value{0xCCCCCCCCCCCCCCCCULL, 0xDDDDDDDDDDDDDDDDULL};

        emu->emu().reg<vector_value>(sogen::arm64_register::v8, first_value);

        ASSERT_TRUE(emu->activate_thread(second));
        emu->emu().reg<vector_value>(sogen::arm64_register::v8, second_value);

        ASSERT_TRUE(emu->activate_thread(first));
        EXPECT_EQ(emu->emu().reg<vector_value>(sogen::arm64_register::v8), first_value);

        ASSERT_TRUE(emu->activate_thread(second));
        EXPECT_EQ(emu->emu().reg<vector_value>(sogen::arm64_register::v8), second_value);
    }

    TEST(MacosProcessContext, ProcessStateSurvivesTheEmulatorSnapshot)
    {
        const auto emu = macos_test::make_emulator();

        emu->process.setup(emu->emu(), emu->memory, sogen::MACOS_EXECUTABLE_BASE, {"/bin/hello", "one"}, {"PATH=/usr/bin"}, "/bin/hello");
        ASSERT_NE(emu->process.active_thread, nullptr);

        emu->process.pid = 4242;
        emu->process.ppid = 7;
        emu->process.current_working_directory = "/tmp/work";
        emu->process.exit_status = 9;

        sogen::utils::buffer_serializer serializer{};
        emu->serialize(serializer, false);

        const auto restored = macos_test::make_emulator();
        sogen::utils::buffer_deserializer deserializer{serializer.get_buffer()};
        restored->deserialize(deserializer, false);

        EXPECT_EQ(restored->process.pid, 4242u);
        EXPECT_EQ(restored->process.ppid, 7u);
        EXPECT_EQ(restored->process.uid, 501u);
        EXPECT_EQ(restored->process.gid, 20u);
        EXPECT_EQ(restored->process.current_working_directory, "/tmp/work");
        EXPECT_EQ(restored->process.executable_path, "/bin/hello");
        ASSERT_TRUE(restored->process.exit_status.has_value());
        EXPECT_EQ(*restored->process.exit_status, 9);
        EXPECT_EQ(restored->process.argv, emu->process.argv);
        EXPECT_EQ(restored->process.envp, emu->process.envp);
        EXPECT_EQ(restored->process.apple, emu->process.apple);
        EXPECT_EQ(restored->process.stack_base, sogen::MACOS_MAIN_STACK_TOP - sogen::MACOS_MAIN_STACK_SIZE);
        EXPECT_EQ(restored->process.stack_size, sogen::MACOS_MAIN_STACK_SIZE);
        EXPECT_EQ(restored->process.next_thread_id, emu->process.next_thread_id);
        EXPECT_EQ(restored->process.fds.get_fds().size(), emu->process.fds.get_fds().size());

        ASSERT_EQ(restored->process.threads.size(), 1u);
        ASSERT_NE(restored->process.active_thread, nullptr);
        EXPECT_EQ(restored->process.active_thread, &restored->process.threads.begin()->second)
            << "active_thread must point into the restored map";
        EXPECT_EQ(restored->process.active_thread->thread_id, emu->process.active_thread->thread_id);
        EXPECT_EQ(restored->process.active_thread->thread_self, emu->process.active_thread->thread_self);
        EXPECT_EQ(restored->process.active_thread->saved_regs.sp, emu->process.active_thread->saved_regs.sp);
        EXPECT_EQ(restored->process.active_thread->saved_regs.pc, emu->process.active_thread->saved_regs.pc);
    }
}
